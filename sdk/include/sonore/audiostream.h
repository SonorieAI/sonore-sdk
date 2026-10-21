// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: reading an audio file without holding all of it.
//
// audiofile.h reads a whole file into a vector, which is the right answer for
// an impulse response, a one-shot, or anything a plugin ships with. It is the
// wrong answer for a sample library: a two-gigabyte WAV becomes two gigabytes
// of RAM per instance, and eight instances of a sampler is a session that
// will not open.
//
// What is missing is a reader that keeps the FILE open and fetches the frames
// asked for. That is easy and exact for uncompressed formats, where frame N
// is at a computable byte offset, and it is a different problem entirely for
// FLAC, MP3 and Ogg, where finding frame N means decoding to it.
//
// So this covers WAV and AIFF, and REFUSES the compressed formats by name
// rather than pretending. A caller that hands it an MP3 is told to use
// readAudioFile, which handles that properly, instead of being handed a
// reader that silently decodes the whole thing on the first read and defeats
// the purpose of asking.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <sys/types.h> // off_t, for fseeko / ftello
#endif

#include "wav.h"
#include "path_io.h"

namespace sonore {

/**
 * An open audio file, read a range at a time.
 *
 * Not thread safe, and deliberately not: a std::FILE has one position, and
 * hiding that behind a lock would put a lock in whatever thread a sampler
 * chose to read from. One reader per thread, or one reader and a queue.
 *
 * The read itself is NOT real-time safe -- it is a seek and a fread, which
 * can block for as long as the disk feels like. That is the nature of reading
 * from a disk and no wrapper changes it; a sampler streams on a worker thread
 * into a ring, exactly as the audio input backend does.
 */
class AudioFileReader {
public:
  /** Closes the file. */
  ~AudioFileReader() { close(); }

  /** Not open: every read returns 0. */
  AudioFileReader() = default;
  AudioFileReader(const AudioFileReader&) = delete;            ///< Not copyable: one file, one position.
  AudioFileReader& operator=(const AudioFileReader&) = delete; ///< Not copyable: one file, one position.

  /** Open the file at `path`, UTF-8, closing any file open before, and read
   *  its header: a PCM or float WAV, or an AIFF or uncompressed AIFF-C. The
   *  samples stay on the disk. False, with error() saying why and the reader
   *  closed, for a file that cannot be opened, is compressed or is
   *  malformed. Not for the audio thread: it opens a file. */
  bool open(const char* path) {
    close();
    // error() is about the latest open(): one that worked went on reporting
    // the failure before it, "could not open" a file it was reading.
    error_.clear();
    if (!path) {
      error_ = "no path";
      return false;
    }
    file_ = ::sonore::pathio::open(path, "rb");
    if (!file_) {
      error_ = std::string("could not open ") + path;
      return false;
    }

    uint8_t magic[4] = {0};
    if (std::fread(magic, 1, 4, file_) != 4) {
      error_ = "the file is too short to identify";
      close();
      return false;
    }
    seekTo(0);

    bool ok = false;
    if (std::memcmp(magic, "RIFF", 4) == 0) {
      ok = parseWav();
    } else if (std::memcmp(magic, "FORM", 4) == 0) {
      ok = parseAiff();
    } else {
      // Named, not guessed at. "Unsupported file" would send someone looking
      // for a corrupt header when the answer is that this reader is for
      // uncompressed audio and the other one is not.
      error_ =
          "this reader streams WAV and AIFF only; a compressed file has to be decoded, so use "
          "readAudioFile for it";
      close();
      return false;
    }
    if (!ok) close();
    return ok;
  }

  /** Close the file; harmless when none is open. sampleRate(), numChannels()
   *  and numFrames() answer 0 afterwards. */
  void close() {
    if (file_) std::fclose(file_);
    file_ = nullptr;
    frames_ = 0;
    channels_ = 0;
    sampleRate_ = 0;
  }

  /** True after a successful open(), until close(). */
  bool isOpen() const { return file_ != nullptr; }
  /** The file's rate in Hz; 0 when not open. */
  uint32_t sampleRate() const { return sampleRate_; }
  /** Channels per frame, interleaved in what read() writes; 0 when not open. */
  uint16_t numChannels() const { return channels_; }
  /** The file's length in frames (one sample per channel); 0 when not open. */
  uint64_t numFrames() const { return frames_; }
  /** Bits per sample as stored, which a caller may want to report and must
   *  not need in order to read: everything comes back as float either way. */
  uint16_t bitsPerSample() const { return bits_; }
  /** Why the latest open() failed; empty after one that succeeded. */
  const std::string& error() const { return error_; }

  /**
   * Read `frames` frames starting at `startFrame` into interleaved floats.
   *
   * Returns how many frames were actually read, which is fewer at the end of
   * the file and zero past it. The REST OF THE BUFFER IS NOT TOUCHED: a
   * caller that wants silence there can clear it, and a caller that is
   * mixing into it would not thank us for zeroing what it had.
   */
  size_t read(uint64_t startFrame, float* interleaved, size_t frames) {
    if (!file_ || !interleaved || frames == 0 || channels_ == 0) return 0;
    if (startFrame >= frames_) return 0;
    const uint64_t available = frames_ - startFrame;
    if ((uint64_t) frames > available) frames = (size_t) available;

    const size_t bytesPerFrame = (size_t) channels_ * (size_t) (bits_ / 8);
    if (!seekTo((int64_t) (dataOffset_ + startFrame * bytesPerFrame))) return 0;

    scratch_.resize(frames * bytesPerFrame);
    const size_t got = std::fread(scratch_.data(), 1, scratch_.size(), file_);
    const size_t gotFrames = bytesPerFrame ? got / bytesPerFrame : 0;
    convert(scratch_.data(), interleaved, gotFrames * (size_t) channels_);
    return gotFrames;
  }

private:
  enum class Sample { PcmU8, Pcm16, Pcm24, Pcm32, Float32 };

  void convert(const uint8_t* in, float* out, size_t count) const {
    switch (kind_) {
      case Sample::PcmU8: // WAV's 8-bit is unsigned, centred on 128
        for (size_t i = 0; i < count; ++i) out[i] = ((float) in[i] - 128.0f) / 128.0f;
        break;
      case Sample::Pcm32: // little-endian: only WAV reaches here
        for (size_t i = 0; i < count; ++i)
          out[i] = (float) ((double) (int32_t) wavdetail::readU32(in + i * 4) / 2147483648.0);
        break;
      case Sample::Pcm16:
        for (size_t i = 0; i < count; ++i) {
          const uint16_t raw = bigEndian_ ? (uint16_t) ((in[i * 2] << 8) | in[i * 2 + 1])
                                          : (uint16_t) (in[i * 2] | (in[i * 2 + 1] << 8));
          out[i] = (float) (int16_t) raw / 32768.0f;
        }
        break;
      case Sample::Pcm24:
        for (size_t i = 0; i < count; ++i) {
          const uint8_t* p = in + i * 3;
          // Built in the top three bytes and shifted down, which sign-extends
          // for free. Assembling into the bottom and masking does not.
          int32_t v = bigEndian_ ? (int32_t) ((uint32_t) p[2] << 8 | (uint32_t) p[1] << 16 |
                                              (uint32_t) p[0] << 24)
                                 : (int32_t) ((uint32_t) p[0] << 8 | (uint32_t) p[1] << 16 |
                                              (uint32_t) p[2] << 24);
          v >>= 8;
          out[i] = (float) v / 8388608.0f;
        }
        break;
      case Sample::Float32:
      default:
        std::memcpy(out, in, count * sizeof(float));
        break;
    }
  }

  /** Every offset is 64 bits. fseek and ftell take a `long`, which is 32
   *  bits on Windows: a file of 2 GiB or more did not open ("too short", from
   *  ftell's -1), and nothing past 2 GiB could be read -- in the reader whose
   *  whole purpose is files too big to load. */
  bool seekTo(int64_t offset) {
    if (offset < 0) return false;
#if defined(_WIN32)
    return _fseeki64(file_, offset, SEEK_SET) == 0;
#else
    return fseeko(file_, (off_t) offset, SEEK_SET) == 0;
#endif
  }
  int64_t fileSize() {
#if defined(_WIN32)
    if (_fseeki64(file_, 0, SEEK_END) != 0) return -1;
    return _ftelli64(file_);
#else
    if (fseeko(file_, 0, SEEK_END) != 0) return -1;
    return (int64_t) ftello(file_);
#endif
  }

  /** Read a chunk header and body without loading the file: headers are tiny
   *  and scattered, so this seeks to each one rather than mapping the lot. */
  bool readAt(int64_t offset, void* into, size_t bytes) {
    if (!seekTo(offset)) return false;
    return std::fread(into, 1, bytes, file_) == bytes;
  }

  bool parseWav() {
    const int64_t size = fileSize();
    if (size < 12) {
      error_ = "the RIFF file is too short";
      return false;
    }
    uint8_t header[12];
    if (!readAt(0, header, 12) || std::memcmp(header + 8, "WAVE", 4) != 0) {
      error_ = "not a WAVE file";
      return false;
    }

    uint16_t format = 0;
    int64_t pos = 12;
    uint32_t dataSize = 0;
    bool haveFmt = false, haveData = false;
    while (pos + 8 <= size) {
      uint8_t chunk[8];
      if (!readAt(pos, chunk, 8)) break;
      const uint32_t chunkSize = wavdetail::readU32(chunk + 4);
      const int64_t body = pos + 8;
      if (std::memcmp(chunk, "fmt ", 4) == 0 && chunkSize >= 16) {
        uint8_t fmt[40] = {0};
        const size_t want = chunkSize < sizeof(fmt) ? chunkSize : sizeof(fmt);
        if (!readAt(body, fmt, want)) break;
        format = wavdetail::readU16(fmt);
        channels_ = wavdetail::readU16(fmt + 2);
        sampleRate_ = wavdetail::readU32(fmt + 4);
        bits_ = wavdetail::readU16(fmt + 14);
        // WAVE_FORMAT_EXTENSIBLE wraps the real tag in a GUID whose first two
        // bytes carry it, which is the same unwrapping the whole-file reader
        // does and for the same reason.
        if (format == 0xFFFE && chunkSize >= 40) format = wavdetail::readU16(fmt + 24);
        haveFmt = true;
      } else if (std::memcmp(chunk, "data", 4) == 0) {
        dataOffset_ = (uint64_t) body;
        dataSize = chunkSize;
        haveData = true;
      }
      pos = nextChunk(body, chunkSize, size);
      if (pos < 0) break;
    }
    if (!haveFmt || !haveData || channels_ == 0) {
      error_ = "the WAVE file has no fmt or data chunk";
      return false;
    }
    if (!channelsPlausible()) return false;
    // A rate of zero is no rate: every position and length a caller works
    // out from it divides by it. The AIFF path refused one; this did not.
    if (sampleRate_ == 0) {
      error_ = "the WAVE sample rate is zero";
      return false;
    }
    // A data chunk that claims more than the file holds is truncated to what
    // is there: refusing outright would reject every file a recorder was
    // still writing when it lost power, and those are worth recovering.
    const uint64_t onDisk = (uint64_t) size - dataOffset_;
    if ((uint64_t) dataSize > onDisk) dataSize = (uint32_t) onDisk;
    // PCM (1) or IEEE float (3), and nothing else. A compressed tag -- WMA
    // (0x0161), AC-3 (0x0092), ADPCM -- was read as PCM of whatever depth
    // its header gave, and played as full-scale noise. The whole-file reader
    // has always refused them.
    if (format != 1 && format != 3) {
      char buffer[96];
      std::snprintf(buffer, sizeof(buffer), "WAVE format 0x%04X is compressed, not PCM or float",
                    (unsigned) format);
      error_ = buffer;
      return false;
    }
    bigEndian_ = false;
    if (!classify(format == 3, true)) return false;
    frames_ = (uint64_t) dataSize / ((uint64_t) channels_ * (bits_ / 8));
    return true;
  }

  bool parseAiff() {
    const int64_t size = fileSize();
    if (size < 12) {
      error_ = "the FORM file is too short";
      return false;
    }
    uint8_t header[12];
    if (!readAt(0, header, 12)) return false;
    const bool aifc = std::memcmp(header + 8, "AIFC", 4) == 0;
    if (!aifc && std::memcmp(header + 8, "AIFF", 4) != 0) {
      error_ = "not an AIFF file";
      return false;
    }

    int64_t pos = 12;
    uint64_t declaredFrames = 0;
    bool haveComm = false, haveSsnd = false, littleEndian = false;
    char compression[5] = {0};
    while (pos + 8 <= size) {
      uint8_t chunk[8];
      if (!readAt(pos, chunk, 8)) break;
      // AIFF is big-endian throughout, including its chunk sizes.
      const uint32_t chunkSize = (uint32_t) ((chunk[4] << 24) | (chunk[5] << 16) |
                                             (chunk[6] << 8) | chunk[7]);
      const int64_t body = pos + 8;
      if (std::memcmp(chunk, "COMM", 4) == 0 && chunkSize >= 18) {
        uint8_t comm[32] = {0};
        const size_t want = chunkSize < sizeof(comm) ? chunkSize : sizeof(comm);
        if (!readAt(body, comm, want)) break;
        channels_ = (uint16_t) ((comm[0] << 8) | comm[1]);
        declaredFrames = (uint64_t) ((uint32_t) comm[2] << 24 | (uint32_t) comm[3] << 16 |
                                     (uint32_t) comm[4] << 8 | comm[5]);
        bits_ = (uint16_t) ((comm[6] << 8) | comm[7]);
        sampleRate_ = (uint32_t) (extendedToDouble(comm + 8) + 0.5);
        // AIFF-C names its encoding; "sowt" is PCM with the bytes the other
        // way round, which is what Mac tools write and what a big-endian-only
        // reader turns into noise. "NONE" and "twos" are PCM as plain AIFF
        // has it. Anything else -- u-law, IMA ADPCM, float -- was read as PCM
        // and played as noise too; it is refused below.
        if (aifc && chunkSize >= 22) {
          littleEndian = std::memcmp(comm + 18, "sowt", 4) == 0;
          const bool pcm = littleEndian || std::memcmp(comm + 18, "NONE", 4) == 0 ||
                           std::memcmp(comm + 18, "twos", 4) == 0;
          if (!pcm) std::memcpy(compression, comm + 18, 4);
        }
        haveComm = true;
      } else if (std::memcmp(chunk, "SSND", 4) == 0 && chunkSize >= 8) {
        uint8_t ssnd[8];
        if (!readAt(body, ssnd, 8)) break;
        const uint32_t offset = (uint32_t) ((ssnd[0] << 24) | (ssnd[1] << 16) | (ssnd[2] << 8) |
                                            ssnd[3]);
        dataOffset_ = (uint64_t) body + 8 + offset;
        haveSsnd = true;
      }
      pos = nextChunk(body, chunkSize, size);
      if (pos < 0) break;
    }
    if (!haveComm || !haveSsnd || channels_ == 0) {
      error_ = "the AIFF file has no COMM or SSND chunk";
      return false;
    }
    if (compression[0]) {
      for (char& c : compression)
        if (c && (c < 0x20 || c > 0x7e)) c = '?';
      error_ = std::string("AIFF-C compression '") + compression + "' is not PCM";
      return false;
    }
    if (!channelsPlausible()) return false;
    // SSND's own offset field is added to its start, and nothing stopped it
    // pointing past the end of the file: `size - dataOffset_` then wrapped
    // to a number of frames in the billions, with no audio behind any of them.
    if (dataOffset_ > (uint64_t) size) {
      error_ = "the AIFF sound data starts past the end of the file";
      return false;
    }
    if (sampleRate_ == 0) {
      error_ = "the AIFF sample rate is not a usable number";
      return false;
    }
    bigEndian_ = !littleEndian;
    if (!classify(false, false)) return false;
    const uint64_t onDisk = (uint64_t) size - dataOffset_;
    const uint64_t fits = onDisk / ((uint64_t) channels_ * (bits_ / 8));
    frames_ = declaredFrames < fits ? declaredFrames : fits;
    return true;
  }

  /**
   * Where the next chunk starts, or -1 when the walk must stop.
   *
   * The arithmetic this replaces was `pos = body + (long) chunkSize + (pad)`,
   * written out for WAV and again for AIFF. A chunk size is a full 32-bit
   * UNSIGNED value read out of a file somebody else wrote, and `long` is
   * 32-bit and SIGNED on Windows -- so a size above two gigabytes became a
   * negative number and the walk went BACKWARDS through the file, reading
   * chunk headers out of the middle of the audio.
   *
   * Never past the end, which is a termination condition rather than an
   * error: a file can be truncated because a recorder lost power, and the
   * chunks that ARE there are still worth reading.
   *
   * A chunk of size zero is NOT one. It is legal RIFF and ordinary AIFF -- an
   * empty NAME or ANNO, a JUNK with nothing in it -- and its eight-byte
   * header alone moves the walk on, so the walk always progresses. It was
   * taken for "no progress" and ended the walk: every chunk after it went
   * unread, the data or SSND chunk among them, and a file the whole-file
   * readers play was refused here as having no audio.
   */
  static int64_t nextChunk(int64_t body, uint32_t chunkSize, int64_t size) {
    // 64-bit throughout: this is the sum that used to overflow.
    const uint64_t padded = (uint64_t) chunkSize + (uint64_t) (chunkSize & 1u);
    const uint64_t next = (uint64_t) body + padded;
    if (next > (uint64_t) size) return -1; // claims more than the file holds
    return (int64_t) next;
  }

  /** The whole-file readers stop at 32 channels and so does this one now.
   *  The field is sixteen bits, and every buffer sized from it -- a sample
   *  stream's 64K-frame window, a waveform cache, this reader's own scratch --
   *  multiplies by it: a 60-byte header claiming 65535 channels asked for
   *  17 GB, and the bad_alloc took the host down with it. */
  static constexpr uint16_t kMaxChannels = 32;
  bool channelsPlausible() {
    if (channels_ <= kMaxChannels) return true;
    char buffer[96];
    std::snprintf(buffer, sizeof(buffer), "%u channels is more than this reader opens (%u)",
                  (unsigned) channels_, (unsigned) kMaxChannels);
    error_ = buffer;
    return false;
  }

  /** `wav` admits the two depths readWav reads and readAiff does not:
   *  unsigned 8-bit and 32-bit integer PCM, which turn up in sample libraries
   *  from old drum machines and Pro Tools bounces. This reader refused them,
   *  so a WAV that readAudioFile plays would not stream, play through
   *  AudioFormatReaderSource or draw from its file. */
  bool classify(bool isFloat, bool wav) {
    if (isFloat && bits_ == 32) {
      kind_ = Sample::Float32;
      return true;
    }
    if (!isFloat && wav && bits_ == 8) {
      kind_ = Sample::PcmU8;
      return true;
    }
    if (!isFloat && wav && bits_ == 32) {
      kind_ = Sample::Pcm32;
      return true;
    }
    if (!isFloat && bits_ == 16) {
      kind_ = Sample::Pcm16;
      return true;
    }
    if (!isFloat && bits_ == 24) {
      kind_ = Sample::Pcm24;
      return true;
    }
    char buffer[96];
    std::snprintf(buffer, sizeof(buffer), "%u-bit %s is not a format this reader converts",
                  (unsigned) bits_, isFloat ? "float" : "PCM");
    error_ = buffer;
    return false;
  }

  /** AIFF stores its rate as an 80-bit IEEE extended float, which no compiler
   *  has a type for. Only positive finite rates need to survive the trip. */
  static double extendedToDouble(const uint8_t* p) {
    const int exponent = (int) (((p[0] & 0x7f) << 8) | p[1]) - 16383;
    uint64_t mantissa = 0;
    for (int i = 0; i < 8; ++i) mantissa = (mantissa << 8) | p[2 + i];
    if (mantissa == 0) return 0.0;
    // A rate is a small positive number, and the exponent field can say
    // 2^16384: the result was +inf, cast straight to uint32_t by the caller,
    // which is undefined behaviour on a corrupt COMM chunk. audiofile.h's
    // copy of this conversion learned that long ago; this one had not.
    if ((p[0] & 0x80) || exponent > 30 || exponent < -30) return 0.0;
    return std::ldexp((double) mantissa, exponent - 63);
  }

  std::FILE* file_ = nullptr;
  std::vector<uint8_t> scratch_;
  uint64_t dataOffset_ = 0, frames_ = 0;
  uint32_t sampleRate_ = 0;
  uint16_t channels_ = 0, bits_ = 0;
  Sample kind_ = Sample::Float32;
  bool bigEndian_ = false;
  std::string error_;
};

} // namespace sonore
