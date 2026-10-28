// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: audio files beyond WAV.
//
// AIFF (and AIFF-C "sowt") because Mac-sourced sample libraries still ship it,
// and FLAC because impulse-response packs do. The FLAC decoder is OUR OWN
// implementation of the published format (no library, no licence to carry),
// and it is graded the only way a lossless codec honestly can be: decode and
// compare SAMPLE-EXACT against the same material as PCM (see sdk_tests).
//
// MP3 and Ogg Vorbis come from vendored public-domain decoders (see
// audiofile_codecs.h) because there is nothing to differentiate in a Huffman
// table; FLAC and AIFF are ours because they are simple enough to own and to
// prove sample-exact.
//
// Offline-only by design, like wav.h: these allocate and do file IO, so they
// belong in prepare()-time code and tools, never in process().
//
// Everything funnels into the same WavData a caller already handles;
// readAudioFile() sniffs the magic and dispatches, so callers need not care
// which container the user dropped on them. An application that wants the
// operating system's decoders behind that (M4A, WMA...) opts in through
// system_audio_format.h; the set here stays the default.
//
// Every reader has a twin that takes the file's BYTES instead of its path --
// readAudioFile(data, size, &out), and readWav/readAiff/readFlac/readMp3/
// readOgg likewise -- for audio compiled into the plugin itself. The file
// readers read the file and call the twin, so the two cannot disagree.

#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#if defined(_MSC_VER)
#include <intrin.h> // _BitScanReverse64 and _byteswap_uint64, for the FLAC bit reader
#endif

#include "audiofile_codecs.h"
#include "wav.h"
#include "path_io.h"

namespace sonore {

namespace aiffdetail {

inline uint32_t readU32BE(const uint8_t* p) {
  return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) | ((uint32_t) p[2] << 8) |
         (uint32_t) p[3];
}
inline uint16_t readU16BE(const uint8_t* p) { return (uint16_t) ((p[0] << 8) | p[1]); }

/** AIFF stores the sample rate as an 80-bit IEEE 754 extended float, the one
 *  place the format shows its Motorola heritage. Only the values that are real
 *  sample rates need to survive the conversion. */
inline double readExtended80(const uint8_t* p) {
  const int exponent = ((p[0] & 0x7f) << 8 | p[1]) - 16383;
  uint64_t mantissa = 0;
  for (int i = 0; i < 8; ++i) mantissa = (mantissa << 8) | p[2 + i];
  if (mantissa == 0) return 0.0;
  // A sample rate is a small number. The exponent field can say 2^16384,
  // and multiplying up to that gives +inf, which the caller then casts to
  // an integer -- undefined behaviour on a corrupt COMM chunk. Anything
  // outside 2^-30..2^30 is refused here and the file is rejected upstream.
  if (exponent > 30 || exponent < -30) return 0.0;
  double value = (double) mantissa;
  int shift = exponent - 63;
  while (shift > 0) { value *= 2.0; --shift; }
  while (shift < 0) { value *= 0.5; ++shift; }
  return (p[0] & 0x80) ? -value : value;
}

} // namespace aiffdetail

/** Decode an AIFF or AIFF-C file that is already in memory: `size` bytes at
 *  `fileData`, as they would be on disk (an IR compiled into the plugin, say).
 *  The decoder the file reader below uses, with its refusals and ceiling. */
inline bool readAiff(const void* fileData, size_t size, WavData* out,
                     size_t maxSamples = kMaxDecodedSamples) {
  if (!fileData || !out) return false;
  // The bounds the file reader holds before it reads a byte.
  if (size < 12 || size > (size_t) (1u << 30)) return false;
  const uint8_t* const data = static_cast<const uint8_t*>(fileData);

  using namespace aiffdetail;
  if (std::memcmp(data, "FORM", 4) != 0) return false;
  const bool aifc = std::memcmp(data + 8, "AIFC", 4) == 0;
  if (!aifc && std::memcmp(data + 8, "AIFF", 4) != 0) return false;

  uint16_t channels = 0, bits = 0;
  uint32_t frames = 0;
  double rate = 0.0;
  bool littleEndian = false;
  const uint8_t* audio = nullptr;
  size_t audioBytes = 0;

  // Chunk walk, exactly like the WAV reader: trusting fixed offsets is the
  // classic shortcut that breaks on files written by real software.
  size_t pos = 12;
  while (pos + 8 <= size) {
    const uint8_t* hdr = data + pos;
    const uint32_t chunkLen = readU32BE(hdr + 4);
    const uint8_t* body = hdr + 8;
    if (chunkLen > size - pos - 8) { // pos + 8 <= size: the loop guard
      // Sound data that claims more than the file holds: a take whose header
      // was never finished. What is there is read, as in a WAV.
      const size_t there = size - pos - 8;
      if (std::memcmp(hdr, "SSND", 4) == 0 && there >= 8) {
        const uint32_t offset = readU32BE(body);
        if ((size_t) offset + 8 <= there) {
          audio = body + 8 + offset;
          audioBytes = there - 8 - offset;
        }
      }
      break;
    }
    if (std::memcmp(hdr, "COMM", 4) == 0 && chunkLen >= 18) {
      channels = readU16BE(body);
      frames = readU32BE(body + 2);
      bits = readU16BE(body + 6);
      rate = readExtended80(body + 8);
      if (aifc && chunkLen >= 22) {
        // "twos" is AIFF-C's other name for big-endian PCM, which the
        // streaming reader takes; refusing it here made the two disagree.
        if (std::memcmp(body + 18, "sowt", 4) == 0) littleEndian = true;
        else if (std::memcmp(body + 18, "NONE", 4) != 0 && std::memcmp(body + 18, "twos", 4) != 0)
          return false; // compressed AIFC
      }
    } else if (std::memcmp(hdr, "SSND", 4) == 0 && chunkLen >= 8) {
      const uint32_t offset = readU32BE(body);
      if ((size_t) offset + 8 > chunkLen) break;
      audio = body + 8 + offset;
      audioBytes = chunkLen - 8 - offset;
    }
    pos += 8 + chunkLen + (chunkLen & 1); // chunks are word-aligned
  }

  // At most 32 channels, as readWav: a header claiming 65535 is a damaged
  // file, and every reader of the result sizes per-channel work from it.
  if (!audio || channels == 0 || channels > 32 || rate <= 0.0 || rate > 4.0e9 ||
      (bits != 16 && bits != 24))
    return false;
  const size_t bytesPer = bits / 8u;
  size_t total = audioBytes / bytesPer;
  total -= total % channels; // whole frames
  const size_t wanted = (size_t) frames * channels;
  if (wanted > 0 && wanted < total) total = wanted;
  if (total > maxSamples) return false; // the ceiling every reader keeps

  out->sampleRate = (uint32_t) (rate + 0.5);
  out->numChannels = channels;
  out->samples.resize(total);
  const uint8_t* p = audio;
  for (size_t i = 0; i < total; ++i, p += bytesPer) {
    int32_t v;
    if (bits == 16) {
      v = littleEndian ? (int16_t) (p[0] | (p[1] << 8)) : (int16_t) ((p[0] << 8) | p[1]);
      out->samples[i] = (float) v / 32768.0f;
    } else {
      v = littleEndian ? ((int32_t) ((p[2] << 24) | (p[1] << 16) | (p[0] << 8)) >> 8)
                       : ((int32_t) ((p[0] << 24) | (p[1] << 16) | (p[2] << 8)) >> 8);
      out->samples[i] = (float) v / 8388608.0f;
    }
  }
  return true;
}

/** Read an AIFF or AIFF-C file: 16/24-bit PCM, big-endian ("NONE") or
 *  little-endian ("sowt", what modern Macs actually write). */
inline bool readAiff(const char* path, WavData* out, size_t maxSamples = kMaxDecodedSamples) {
  if (!path || !out) return false;
  std::FILE* f = ::sonore::pathio::open(path, "rb");
  if (!f) return false;
  std::fseek(f, 0, SEEK_END);
  const long size = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  // The whole file is read into memory, as readWav does -- and capped as it
  // is. This reader had no cap at all.
  if (size < 12 || size > (long) (1u << 30)) { std::fclose(f); return false; }
  std::vector<uint8_t> data((size_t) size);
  const bool okRead = std::fread(data.data(), 1, (size_t) size, f) == (size_t) size;
  std::fclose(f);
  if (!okRead) return false;
  return readAiff(data.data(), data.size(), out, maxSamples);
}

namespace aiffwrite {

/** Append `v` as four bytes, most significant first, as AIFF stores it. */
inline void writeU32BE(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back((uint8_t) (v >> 24));
  out.push_back((uint8_t) ((v >> 16) & 0xff));
  out.push_back((uint8_t) ((v >> 8) & 0xff));
  out.push_back((uint8_t) (v & 0xff));
}

/** Append `v` as two bytes, most significant first. */
inline void writeU16BE(std::vector<uint8_t>& out, uint16_t v) {
  out.push_back((uint8_t) (v >> 8));
  out.push_back((uint8_t) (v & 0xff));
}

/** The sample rate as an 80-bit IEEE extended float, which is the one field
 *  that makes AIFF awkward to write. Only positive, finite rates need to
 *  survive, so the general case is not attempted. */
inline void writeExtended80(std::vector<uint8_t>& out, double rate) {
  if (rate <= 0.0) {
    for (int i = 0; i < 10; ++i) out.push_back(0);
    return;
  }
  int exponent = 0;
  double mantissa = std::frexp(rate, &exponent); // rate = mantissa * 2^exponent, 0.5<=m<1
  // The extended format stores an EXPLICIT leading one, unlike float and
  // double: shifting as if it were implicit halves every rate written.
  const uint64_t bits = (uint64_t) std::ldexp(mantissa, 64);
  const uint16_t biased = (uint16_t) (exponent - 1 + 16383);
  writeU16BE(out, biased);
  for (int i = 7; i >= 0; --i) out.push_back((uint8_t) ((bits >> (i * 8)) & 0xff));
}

/** `v` in steps of 1/full, rounded and clipped to [-full, full - 1]. */
inline int32_t quantise(float v, double full) {
  double q = std::nearbyint((double) v * full);
  if (!(q >= -full)) q = -full; // NaN too
  if (q > full - 1.0) q = full - 1.0;
  return (int32_t) q;
}

} // namespace aiffwrite

/** Write an AIFF file: 16- or 24-bit PCM, big-endian, which is what the
 *  format means by default and what a Mac tool expects to find. */
inline bool writeAiff(const char* path, const float* interleaved, size_t numFrames,
                      uint16_t numChannels, uint32_t sampleRate, int bitDepth = 24) {
  if (!path || !interleaved || numChannels == 0 || sampleRate == 0) return false;
  if (bitDepth != 16 && bitDepth != 24) return false;
  using namespace aiffwrite;

  const size_t bytesPer = (size_t) (bitDepth / 8);
  // Every length in an AIFF is 32 bits. Past 4 GB of sound the SSND and FORM
  // sizes wrapped, and the file said it was a few bytes long: refused
  // instead, before a byte is built.
  if (numFrames > (0xFFFFFFFFull - 64u) / ((uint64_t) numChannels * bytesPer)) return false;
  const size_t total = numFrames * numChannels;
  const uint32_t soundBytes = (uint32_t) (total * bytesPer + 8); // + offset/blocksize

  std::vector<uint8_t> out;
  out.insert(out.end(), {'F', 'O', 'R', 'M'});
  writeU32BE(out, (uint32_t) (4 + 8 + 18 + 8 + soundBytes));
  out.insert(out.end(), {'A', 'I', 'F', 'F'});

  out.insert(out.end(), {'C', 'O', 'M', 'M'});
  writeU32BE(out, 18);
  writeU16BE(out, numChannels);
  writeU32BE(out, (uint32_t) numFrames);
  writeU16BE(out, (uint16_t) bitDepth);
  writeExtended80(out, (double) sampleRate);

  out.insert(out.end(), {'S', 'S', 'N', 'D'});
  writeU32BE(out, soundBytes);
  writeU32BE(out, 0); // offset
  writeU32BE(out, 0); // block size

  // Scaled by 2^(N-1), the scale every reader in the SDK divides by, and
  // clipped to the one code a positive full scale lacks. Scaled by
  // 2^(N-1) - 1, a file read and written again came back with half its
  // samples one step smaller.
  //
  // Into a buffer sized once, not a push_back a byte: 69 million of those,
  // each checking the capacity, were most of a four-minute file's 0.75 s.
  const size_t headerBytes = out.size();
  out.resize(headerBytes + total * bytesPer);
  uint8_t* p = out.data() + headerBytes;
  if (bitDepth == 16) {
    for (size_t i = 0; i < total; ++i, p += 2) {
      const int32_t q = aiffwrite::quantise(interleaved[i], 32768.0);
      p[0] = (uint8_t) ((q >> 8) & 0xff);
      p[1] = (uint8_t) (q & 0xff);
    }
  } else {
    for (size_t i = 0; i < total; ++i, p += 3) {
      const int32_t q = aiffwrite::quantise(interleaved[i], 8388608.0);
      p[0] = (uint8_t) ((q >> 16) & 0xff);
      p[1] = (uint8_t) ((q >> 8) & 0xff);
      p[2] = (uint8_t) (q & 0xff);
    }
  }

  std::FILE* f = ::sonore::pathio::open(path, "wb");
  if (!f) return false;
  const bool ok = std::fwrite(out.data(), 1, out.size(), f) == out.size();
  std::fclose(f);
  return ok;
}

// ── FLAC ─────────────────────────────────────────────────────────────────────

namespace flacdetail {

/** Eight bytes as one big-endian number: FLAC's bit order, a word at a time. */
inline uint64_t loadBigEndian64(const uint8_t* p) {
  uint64_t v;
  std::memcpy(&v, p, sizeof(v));
#if defined(_MSC_VER) && !defined(__clang__)
  return _byteswap_uint64(v);
#elif defined(__GNUC__) || defined(__clang__)
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
  return v;
#else
  return __builtin_bswap64(v);
#endif
#else
  (void) v;
  return ((uint64_t) p[0] << 56) | ((uint64_t) p[1] << 48) | ((uint64_t) p[2] << 40) |
         ((uint64_t) p[3] << 32) | ((uint64_t) p[4] << 24) | ((uint64_t) p[5] << 16) |
         ((uint64_t) p[6] << 8) | (uint64_t) p[7];
#endif
}

inline uint32_t loadBigEndian32(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, sizeof(v));
#if defined(_MSC_VER) && !defined(__clang__)
  return _byteswap_ulong(v);
#elif defined(__GNUC__) || defined(__clang__)
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
  return v;
#else
  return __builtin_bswap32(v);
#endif
#else
  (void) v;
  return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) | ((uint32_t) p[2] << 8) | (uint32_t) p[3];
#endif
}

/** Zero bits above the highest one in `v`, which must not be zero. */
inline int leadingZeros64(uint64_t v) {
#if defined(_MSC_VER) && !defined(__clang__)
  unsigned long index = 0;
#if defined(_M_X64) || defined(_M_ARM64)
  _BitScanReverse64(&index, v);
  return 63 - (int) index;
#else
  if (_BitScanReverse(&index, (unsigned long) (v >> 32))) return 31 - (int) index;
  _BitScanReverse(&index, (unsigned long) v);
  return 63 - (int) index;
#endif
#elif defined(__GNUC__) || defined(__clang__)
  return __builtin_clzll(v);
#else
  int n = 0;
  while (!(v & (1ull << 63))) {
    v <<= 1;
    ++n;
  }
  return n;
#endif
}

/**
 * MSB-first bit reader over a byte buffer. Out-of-data is sticky: every read
 * after the end returns 0 and flags failure, so decode loops need one check
 * at the end instead of one per read.
 *
 * It read every bit on its own -- a bounds check, a shift and a branch each
 * -- and a four-minute FLAC took 0.6 to 1.1 s to decode where the same audio
 * as a WAV takes 60 ms. Now a read with eight bytes still ahead is one load,
 * a byte swap and two shifts, and the Rice codes, which are most of a file,
 * are taken off a register by readRiceRun.
 *
 * Within eight bytes of the end, a read goes the old way, a bit at a time:
 * that is where a read can run out, and what it returns then, how far it got
 * and when `failed` is raised are what a damaged or truncated file is judged
 * by. The word path never reaches the end, so it never fails, and it returns
 * the bits the old one would have, from the same position, and leaves the
 * reader at the same position after them.
 */
struct BitReader {
  const uint8_t* data;
  size_t size;
  size_t byte = 0;
  int bit = 0; // bits consumed of data[byte], 0..7
  bool failed = false;

  BitReader(const uint8_t* d, size_t s) : data(d), size(s) {}

  /** Whether eight whole bytes lie ahead: 57 bits or more past the position,
   *  enough for any read below. */
  inline bool wordAhead() const { return byte < size && size - byte >= 8; }

  inline void skip(int n) {
    const size_t p = (size_t) bit + (size_t) n;
    byte += p >> 3;
    bit = (int) (p & 7u);
  }

  inline uint32_t readBit() {
    if (byte >= size) { failed = true; return 0; }
    const uint32_t v = (data[byte] >> (7 - bit)) & 1u;
    if (++bit == 8) { bit = 0; ++byte; }
    return v;
  }

  inline uint64_t readBits(int n) {
    if (n > 0 && n <= 57 && wordAhead()) {
      const uint64_t v = (loadBigEndian64(data + byte) << bit) >> (64 - n);
      skip(n);
      return v;
    }
    return readBitsOneByOne(n);
  }

  /** The reader as it always was: what every read near the end goes through. */
  inline uint64_t readBitsOneByOne(int n) {
    uint64_t v = 0;
    // Byte-aligned fast path for the common wide reads.
    while (n >= 8 && bit == 0) {
      if (byte >= size) { failed = true; return 0; }
      v = (v << 8) | data[byte++];
      n -= 8;
    }
    while (n > 0) { v = (v << 1) | readBit(); --n; }
    return v;
  }

  inline int64_t readSigned(int n) {
    if (n <= 0) return 0;
    uint64_t v = readBits(n);
    const uint64_t sign = 1ull << (n - 1);
    return (v & sign) ? (int64_t) (v | ~(sign * 2 - 1)) : (int64_t) v;
  }

  /** Unary: count zeros up to the terminating 1. Bit by bit, as it always
   *  was: readRiceRun takes the ordinary codes off a word and leaves this the
   *  long ones, the last bytes and a stream that has failed. */
  inline uint32_t readUnary() {
    uint32_t q = 0;
    while (!failed && readBit() == 0) {
      if (++q > 1u << 24) { failed = true; break; } // corrupt stream guard
    }
    return q;
  }

  inline void alignToByte() {
    if (bit != 0) { bit = 0; ++byte; }
  }
};

/** Rice/Golomb residual: unary quotient, `param` remainder bits, zigzag. */
inline int64_t readRice(BitReader& br, int param) {
  const uint32_t q = br.readUnary();
  const uint64_t u = ((uint64_t) q << param) | br.readBits(param);
  return (int64_t) (u >> 1) ^ -(int64_t) (u & 1);
}

/**
 * `count` Rice codes into `out`: the loop the whole decode spends its time in.
 *
 * The stream's next bits are held in a register, topped up four bytes at a
 * time, and a code -- its zeros, its one and its remainder -- is taken off
 * the top with a count of leading zeros and two shifts. Reading each code
 * from memory at its bit position instead made every code wait for the load
 * the one before it decided, and a FLAC is millions of codes.
 *
 * A code longer than what is held, a run of zeros past it, the last four
 * bytes of the stream and a stream that has failed all go to readRice, from
 * the exact position the register had reached, and get the value they
 * always got.
 */
inline void readRiceRun(BitReader& br, int param, int32_t* out, int count) {
  int i = 0;
  if (!br.failed && br.byte < br.size) {
    size_t next = br.byte; // the first byte not yet in `held`
    uint64_t held = 0;     // the next `ready` bits of the stream, at the top
    int ready = 0;
    if (br.bit != 0) {
      held = (uint64_t) br.data[next++] << (56 + br.bit);
      ready = 8 - br.bit;
    }
    for (; i < count; ++i) {
      if (ready <= 32) {
        if (br.size - next < 4) break;
        held |= (uint64_t) loadBigEndian32(br.data + next) << (32 - ready);
        next += 4;
        ready += 32;
      }
      if (held == 0) break; // nothing but zeros held: a long quotient
      const int zeros = leadingZeros64(held);
      const int length = zeros + 1 + param;
      if (length > ready) break;
      held = (held << zeros) << 1;
      const uint64_t rest = param > 0 ? held >> (64 - param) : 0;
      if (param > 0) held <<= param;
      ready -= length;
      const uint64_t u = ((uint64_t) zeros << param) | rest;
      out[i] = (int32_t) ((int64_t) (u >> 1) ^ -(int64_t) (u & 1));
    }
    const size_t at = next * 8u - (size_t) ready; // in bits
    br.byte = at >> 3;
    br.bit = (int) (at & 7u);
  }
  for (; i < count; ++i) out[i] = (int32_t) readRice(br, param);
}

/** The UTF-8-style coded frame/sample number. The VALUE is irrelevant for
 *  linear decoding; the LENGTH must be walked correctly or the header CRC and
 *  everything after it shifts. */
inline void skipUtf8(BitReader& br) {
  const uint32_t first = (uint32_t) br.readBits(8);
  int follow = 0;
  for (uint32_t m = 0x80; first & m; m >>= 1) ++follow;
  if (follow > 0) --follow; // the count includes the lead byte's own bit
  while (follow-- > 0) br.readBits(8);
}

inline bool decodeResidual(BitReader& br, int32_t* out, int blockSize, int predOrder) {
  const int method = (int) br.readBits(2);
  if (method > 1) return false;
  const int paramBits = method == 0 ? 4 : 5;
  const int escape = method == 0 ? 0xF : 0x1F;
  const int partOrder = (int) br.readBits(4);
  const int partitions = 1 << partOrder;
  if (blockSize % partitions != 0) return false;
  int idx = predOrder;
  for (int p = 0; p < partitions; ++p) {
    int count = blockSize >> partOrder;
    if (p == 0) count -= predOrder;
    if (count < 0) return false;
    const int param = (int) br.readBits(paramBits);
    if (param == escape) {
      const int raw = (int) br.readBits(5);
      for (int i = 0; i < count; ++i) out[idx++] = (int32_t) br.readSigned(raw);
    } else {
      readRiceRun(br, param, out + idx, count);
      idx += count;
    }
    if (br.failed) return false;
  }
  return true;
}

/** The LPC prediction added back to each residual, for an order the compiler
 *  knows, so the loop over the coefficients is unrolled. The arithmetic is
 *  the general loop's, term for term, and exact in 64 bits: a coefficient is
 *  at most 15 bits, a sample 32, and there are at most 32 of them. */
template <int Order>
inline void restoreLpcOf(int32_t* out, int blockSize, const int32_t* coef, int shift) {
  int32_t c[Order];
  for (int j = 0; j < Order; ++j) c[j] = coef[j];
  for (int i = Order; i < blockSize; ++i) {
    int64_t acc = 0;
    for (int j = 0; j < Order; ++j) acc += (int64_t) c[j] * (int64_t) out[i - 1 - j];
    out[i] = (int32_t) ((acc >> shift) + out[i]);
  }
}

inline void restoreLpc(int32_t* out, int blockSize, const int32_t* coef, int order, int shift) {
  switch (order) {
    case 1: restoreLpcOf<1>(out, blockSize, coef, shift); return;
    case 2: restoreLpcOf<2>(out, blockSize, coef, shift); return;
    case 3: restoreLpcOf<3>(out, blockSize, coef, shift); return;
    case 4: restoreLpcOf<4>(out, blockSize, coef, shift); return;
    case 5: restoreLpcOf<5>(out, blockSize, coef, shift); return;
    case 6: restoreLpcOf<6>(out, blockSize, coef, shift); return;
    case 7: restoreLpcOf<7>(out, blockSize, coef, shift); return;
    case 8: restoreLpcOf<8>(out, blockSize, coef, shift); return;
    case 9: restoreLpcOf<9>(out, blockSize, coef, shift); return;
    case 10: restoreLpcOf<10>(out, blockSize, coef, shift); return;
    case 11: restoreLpcOf<11>(out, blockSize, coef, shift); return;
    case 12: restoreLpcOf<12>(out, blockSize, coef, shift); return;
    default: break;
  }
  // Past 12, which libFLAC's own presets never go: the general loop.
  for (int i = order; i < blockSize; ++i) {
    int64_t acc = 0;
    for (int j = 0; j < order; ++j) acc += (int64_t) coef[j] * (int64_t) out[i - 1 - j];
    out[i] = (int32_t) ((acc >> shift) + out[i]);
  }
}

inline bool decodeSubframe(BitReader& br, int32_t* out, int blockSize, int bps) {
  if (br.readBit() != 0) return false; // mandatory zero pad
  const int type = (int) br.readBits(6);
  int wasted = 0;
  if (br.readBit()) {
    wasted = 1;
    while (!br.failed && br.readBit() == 0) ++wasted;
  }
  bps -= wasted;
  if (bps <= 0 || bps > 33) return false;

  if (type == 0) { // CONSTANT
    const int64_t v = br.readSigned(bps);
    for (int i = 0; i < blockSize; ++i) out[i] = (int32_t) v;
  } else if (type == 1) { // VERBATIM
    for (int i = 0; i < blockSize; ++i) out[i] = (int32_t) br.readSigned(bps);
  } else if (type >= 8 && type <= 12) { // FIXED, order 0..4
    const int order = type - 8;
    if (order > blockSize) return false;
    for (int i = 0; i < order; ++i) out[i] = (int32_t) br.readSigned(bps);
    if (!decodeResidual(br, out, blockSize, order)) return false;
    // One loop per order rather than a switch per sample. Order 0 predicts
    // zero, which leaves each residual as its sample.
    switch (order) {
      case 1:
        for (int i = 1; i < blockSize; ++i) out[i] = (int32_t) ((int64_t) out[i - 1] + out[i]);
        break;
      case 2:
        for (int i = 2; i < blockSize; ++i)
          out[i] = (int32_t) (2ll * out[i - 1] - out[i - 2] + out[i]);
        break;
      case 3:
        for (int i = 3; i < blockSize; ++i)
          out[i] = (int32_t) (3ll * out[i - 1] - 3ll * out[i - 2] + out[i - 3] + out[i]);
        break;
      case 4:
        for (int i = 4; i < blockSize; ++i)
          out[i] = (int32_t) (4ll * out[i - 1] - 6ll * out[i - 2] + 4ll * out[i - 3] - out[i - 4] +
                              out[i]);
        break;
      default:
        break;
    }
  } else if (type >= 32) { // LPC, order 1..32
    const int order = type - 31;
    if (order > blockSize) return false;
    for (int i = 0; i < order; ++i) out[i] = (int32_t) br.readSigned(bps);
    const int precision = (int) br.readBits(4) + 1;
    if (precision > 15) return false; // 0b1111 is the spec's invalid marker
    const int shift = (int) br.readSigned(5);
    if (shift < 0) return false;
    int32_t coef[32];
    for (int i = 0; i < order; ++i) coef[i] = (int32_t) br.readSigned(precision);
    if (!decodeResidual(br, out, blockSize, order)) return false;
    restoreLpc(out, blockSize, coef, order, shift);
  } else {
    return false; // reserved type
  }

  if (wasted > 0)
    for (int i = 0; i < blockSize; ++i) out[i] = (int32_t) ((uint32_t) out[i] << wasted);
  return !br.failed;
}

/** The length of an ID3v2 tag at the start of `data`, or 0 when there is
 *  none: a ten-byte header ending in a 28-bit "syncsafe" size (seven bits a
 *  byte), the tag, and a ten-byte footer when the flags say there is one. */
inline size_t id3v2Length(const uint8_t* data, size_t size) {
  if (size < 10 || std::memcmp(data, "ID3", 3) != 0) return 0;
  if ((data[6] | data[7] | data[8] | data[9]) & 0x80) return 0; // not syncsafe: not a tag
  const size_t body = ((size_t) data[6] << 21) | ((size_t) data[7] << 14) |
                      ((size_t) data[8] << 7) | (size_t) data[9];
  return 10 + body + ((data[5] & 0x10) ? 10u : 0u);
}

} // namespace flacdetail

/** Decode a FLAC file that is already in memory: `size` bytes at `fileData`,
 *  as they would be on disk. The decoder the file reader below uses. */
inline bool readFlac(const void* fileData, size_t size, WavData* out) {
  if (!fileData || !out) return false;
  // The bounds the file reader holds before it reads a byte.
  if (size < 42 || size > (size_t) (1u << 30)) return false;
  const uint8_t* const data = static_cast<const uint8_t*>(fileData);
  // An ID3v2 tag in front of the stream, as some taggers write into FLAC
  // files too. libFLAC and ffmpeg skip one, so every player opens such a
  // file; this decoder refused it, and readAudioFile sent it to the MP3
  // decoder, which found no MP3 in it.
  const size_t start = flacdetail::id3v2Length(data, size);
  if (start > size || size - start < 42 ||
      std::memcmp(data + start, "fLaC", 4) != 0)
    return false;

  using namespace flacdetail;

  // Metadata blocks: STREAMINFO is mandatory and first.
  size_t pos = start + 4;
  uint32_t sampleRate = 0;
  int channels = 0, bps = 0;
  uint64_t totalSamples = 0;
  bool sawStreamInfo = false;
  while (pos + 4 <= size) {
    const bool last = (data[pos] & 0x80) != 0;
    const int type = data[pos] & 0x7f;
    const uint32_t len =
        ((uint32_t) data[pos + 1] << 16) | ((uint32_t) data[pos + 2] << 8) | data[pos + 3];
    pos += 4;
    if (len > size - pos) return false; // as what is LEFT: see state_bag.h
    if (type == 0 && len >= 34) {
      const uint8_t* si = data + pos;
      sampleRate = ((uint32_t) si[10] << 12) | ((uint32_t) si[11] << 4) | (si[12] >> 4);
      channels = ((si[12] >> 1) & 0x7) + 1;
      bps = (((si[12] & 1) << 4) | (si[13] >> 4)) + 1;
      totalSamples = ((uint64_t) (si[13] & 0xf) << 32) | ((uint64_t) si[14] << 24) |
                     ((uint64_t) si[15] << 16) | ((uint64_t) si[16] << 8) | si[17];
      sawStreamInfo = true;
    }
    pos += len;
    if (last) break;
  }
  if (!sawStreamInfo || sampleRate == 0 || channels < 1 || channels > 8 ||
      (bps != 16 && bps != 24))
    return false;

  out->sampleRate = sampleRate;
  out->numChannels = (uint16_t) channels;
  out->samples.clear();
  // A crafted STREAMINFO can set totalSamples to 2^36; reserving that many
  // floats is an instant bad_alloc. Refuse a declared length past the ceiling,
  // and reserve only the plausible amount as a hint -- the decode loop below
  // grows the vector for real and stops at the same ceiling.
  if (totalSamples > 0) {
    const uint64_t declared = (uint64_t) totalSamples * (uint64_t) channels;
    if (declared > kMaxDecodedSamples) return false;
    out->samples.reserve((size_t) declared);
  }

  const float scale = bps == 16 ? 1.0f / 32768.0f : 1.0f / 8388608.0f;
  std::vector<int32_t> chans[8];
  std::vector<int32_t>& ch0 = chans[0];
  std::vector<int32_t>& ch1 = chans[1];

  BitReader br(data + pos, size - pos);
  while (!br.failed && br.byte < br.size) {
    // Frame header. 14-bit sync, then the layout the spec fixes.
    const uint32_t sync = (uint32_t) br.readBits(14);
    if (br.failed) break; // clean EOF between frames
    if (sync != 0x3ffe) return false;
    br.readBit();                                     // reserved
    br.readBit();                                     // blocking strategy
    const int bsCode = (int) br.readBits(4);
    const int srCode = (int) br.readBits(4);
    const int chanAsgn = (int) br.readBits(4);
    const int ssCode = (int) br.readBits(3);
    br.readBit(); // reserved
    skipUtf8(br);

    int blockSize = 0;
    switch (bsCode) {
      case 0: return false;
      case 1: blockSize = 192; break;
      case 6: blockSize = (int) br.readBits(8) + 1; break;
      case 7: blockSize = (int) br.readBits(16) + 1; break;
      default:
        blockSize = bsCode <= 5 ? 576 << (bsCode - 2) : 256 << (bsCode - 8);
        break;
    }
    if (srCode == 12) br.readBits(8);
    else if (srCode == 13 || srCode == 14) br.readBits(16);
    int frameBps = bps;
    switch (ssCode) {
      case 0: frameBps = bps; break;
      case 1: frameBps = 8; break;
      case 2: frameBps = 12; break;
      case 4: frameBps = 16; break;
      case 5: frameBps = 20; break;
      case 6: frameBps = 24; break;
      default: return false;
    }
    br.readBits(8); // header CRC-8 (the sample-exact test IS our integrity check)

    if (chanAsgn > 10) return false; // reserved
    const int frameChans = chanAsgn < 8 ? chanAsgn + 1 : 2;
    if (frameChans != channels) return false;
    for (int c = 0; c < channels; ++c) chans[c].assign((size_t) blockSize, 0);

    for (int c = 0; c < frameChans; ++c) {
      int subBps = frameBps;
      // The SIDE channel carries one extra bit; forgetting this corrupts
      // every stereo-decorrelated file.
      if ((chanAsgn == 8 && c == 1) || (chanAsgn == 9 && c == 0) ||
          (chanAsgn == 10 && c == 1))
        ++subBps;
      if (!decodeSubframe(br, chans[c].data(), blockSize, subBps)) return false;
    }
    br.alignToByte();
    br.readBits(16); // frame CRC-16

    // Undo the stereo decorrelation.
    // In 64 bits, then truncated. A valid stream never overflows a 32-bit
    // sum, but a corrupt one puts 2^31 in a channel and the next addition is
    // signed overflow -- undefined behaviour the fuzzer found in one flip.
    // The truncation is the same wrong sample a valid decoder would produce;
    // what it is not is a licence for the optimiser.
    if (chanAsgn == 8) { // left/side
      for (int i = 0; i < blockSize; ++i) ch1[i] = (int32_t) ((int64_t) ch0[i] - ch1[i]);
    } else if (chanAsgn == 9) { // side/right
      for (int i = 0; i < blockSize; ++i) ch0[i] = (int32_t) ((int64_t) ch0[i] + ch1[i]);
    } else if (chanAsgn == 10) { // mid/side
      for (int i = 0; i < blockSize; ++i) {
        const int64_t side = ch1[i];
        // Unsigned shift: mid can be negative, and shifting a negative int
        // left is UB in C++17 -- same class as the encoder's zigzag, found by
        // the same UBSan sweep.
        const int64_t mid = (int32_t) (((uint32_t) ch0[i] << 1) | ((uint32_t) side & 1u));
        ch0[i] = (int32_t) ((mid + side) >> 1);
        ch1[i] = (int32_t) ((mid - side) >> 1);
      }
    }

    // Interleaved straight into place, the vector grown once a frame: a
    // push_back a sample checked the capacity every time and kept the
    // conversion from being done several samples at a time.
    const size_t at = out->samples.size();
    out->samples.resize(at + (size_t) blockSize * (size_t) channels);
    float* const into = out->samples.data() + at;
    if (channels == 2) {
      const int32_t* const left = ch0.data();
      const int32_t* const right = ch1.data();
      for (int i = 0; i < blockSize; ++i) {
        into[2 * i] = (float) left[i] * scale;
        into[2 * i + 1] = (float) right[i] * scale;
      }
    } else {
      for (int c = 0; c < channels; ++c) {
        const int32_t* const from = chans[c].data();
        for (int i = 0; i < blockSize; ++i) into[(size_t) i * channels + c] = (float) from[i] * scale;
      }
    }
    if (totalSamples > 0 && out->samples.size() >= (size_t) totalSamples * channels) break;
    // The output ceiling, enforced during growth for the totalSamples==0 case
    // (a legal "unknown length" STREAMINFO): a file of all-CONSTANT subframes
    // amplifies ~13 header bytes into a 65536-sample block, so the
    // input-bounded loop alone is not a bound on OUTPUT.
    if (out->samples.size() > kMaxDecodedSamples) return false;
  }

  if (totalSamples > 0) {
    const size_t want = (size_t) totalSamples * channels;
    if (out->samples.size() < want) return false; // truncated stream
    out->samples.resize(want);
  }
  return !out->samples.empty();
}

/** Read a FLAC file: our own decoder for the published format. 16/24-bit,
 *  one to eight channels (and all four stereo decorrelation modes), any
 *  block size. It stopped at two, while writeFlac writes up to eight: the
 *  SDK could not read back its own 5.1 file. The file is read whole and
 *  handed to the in-memory readFlac() above. */
inline bool readFlac(const char* path, WavData* out) {
  if (!path || !out) return false;
  std::FILE* f = ::sonore::pathio::open(path, "rb");
  if (!f) return false;
  std::fseek(f, 0, SEEK_END);
  const long size = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  // Bounded as readWav and readAiff are: 1 GB is far past any FLAC a plugin
  // loads whole, and past it -- on a platform whose long is 64 bits -- the
  // whole file went into memory before a byte of it was checked.
  if (size < 42 || size > (long) (1u << 30)) { std::fclose(f); return false; }
  std::vector<uint8_t> data((size_t) size);
  const bool okRead = std::fread(data.data(), 1, (size_t) size, f) == (size_t) size;
  std::fclose(f);
  if (!okRead) return false;
  return readFlac(data.data(), data.size(), out);
}

// ── Lossy formats ────────────────────────────────────────────────────────────

namespace lossydetail {

/** Slurp a whole file. Both lossy decoders want the bytes in memory, and a
 *  sample or impulse response is small enough that streaming would be
 *  complexity without a payer. */
inline bool readFile(const char* path, std::vector<uint8_t>* out) {
  std::FILE* f = ::sonore::pathio::open(path, "rb");
  if (!f) return false;
  std::fseek(f, 0, SEEK_END);
  const long size = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  // The same 1 GB bound as the other readers: an MP3 or Ogg is read whole
  // before it is decoded, and an unbounded read put any file a user chose
  // into memory first.
  if (size <= 0 || size > (long) (1u << 30)) {
    std::fclose(f);
    return false;
  }
  out->resize((size_t) size);
  const bool ok = std::fread(out->data(), 1, (size_t) size, f) == (size_t) size;
  std::fclose(f);
  return ok;
}

/**
 * The Xing/Info frame an encoder puts first: metadata in the shape of an
 * audio frame, which decodes as a frame of silence. Its LAME extension --
 * which ffmpeg writes too, as "Lavc" -- says how many samples of encoder
 * delay lead the audio and how many of padding follow it.
 *
 * Returns false when the frame is ordinary audio. Otherwise `*frames` is the
 * audio frame count (0 if the tag does not say), and `*delay` / `*padding`
 * the samples to drop at either end, the decoder's own 529 included (0 when
 * there is no extension to say).
 */
inline bool mp3InfoTag(const uint8_t* frame, int size, uint32_t* frames, int* delay, int* padding) {
  *frames = 0;
  *delay = 0;
  *padding = 0;
  if (size < 4) return false;
  const bool mpeg1 = (frame[1] & 0x08) != 0;
  const bool mono = (frame[3] & 0xC0) == 0xC0;
  // After the header, the CRC if there is one, and the side information.
  const int at = 4 + ((frame[1] & 1) ? 0 : 2) + (mpeg1 ? (mono ? 17 : 32) : (mono ? 9 : 17));
  if (at + 8 > size) return false;
  const uint8_t* tag = frame + at;
  if (std::memcmp(tag, "Xing", 4) != 0 && std::memcmp(tag, "Info", 4) != 0) return false;
  const uint8_t flags = tag[7];
  int pos = at + 8;
  if (flags & 1) {
    if (pos + 4 > size) return true;
    *frames = (uint32_t) frame[pos] << 24 | (uint32_t) frame[pos + 1] << 16 |
              (uint32_t) frame[pos + 2] << 8 | frame[pos + 3];
    pos += 4;
  }
  if (flags & 2) pos += 4;   // byte count
  if (flags & 4) pos += 100; // seek table
  if (flags & 8) pos += 4;   // quality
  if (pos + 24 <= size && frame[pos] != 0) {
    const uint8_t* e = frame + pos + 21;
    const int encoderDelay = (e[0] << 4) | (e[1] >> 4);
    const int encoderPadding = ((e[1] & 0x0F) << 8) | e[2];
    constexpr int kDecoderDelay = 528 + 1;
    *delay = encoderDelay + kDecoderDelay;
    *padding = encoderPadding > kDecoderDelay ? encoderPadding - kDecoderDelay : 0;
  }
  return true;
}

/**
 * Where an MP3's frames end: before the tags a tagger appends. ID3v1 is the
 * last 128 bytes, "TAG", with a 227-byte "TAG+" before it when extended;
 * APEv2 ends in a 32-byte "APETAGEX" footer that gives the size of its items
 * and footer, and says whether a 32-byte header leads them.
 *
 * The decoder takes a frame whose header matches the last one only if it
 * ends exactly at the end of the buffer or another matching header follows
 * it. A tag after the LAST frame is neither, so that frame was refused, the
 * decoder state reset, and the frame dropped: every MP3 carrying an ID3v1 tag
 * -- most of them -- came out a frame short, 26 ms at 44.1 kHz, and a
 * gapless loop played that much early. minimp3's own file layer strips these
 * tags for the same reason; this SDK calls the frame decoder directly.
 */
inline size_t mp3FramesEnd(const uint8_t* data, size_t size) {
  if (size >= 128 && std::memcmp(data + size - 128, "TAG", 3) == 0) {
    size -= 128;
    if (size >= 227 && std::memcmp(data + size - 227, "TAG+", 4) == 0) size -= 227;
  }
  if (size >= 32 && std::memcmp(data + size - 32, "APETAGEX", 8) == 0) {
    const uint8_t* footer = data + size - 32;
    const uint32_t tagSize = wavdetail::readU32(footer + 12); // items and footer
    const uint32_t flags = wavdetail::readU32(footer + 20);
    const uint64_t total = (uint64_t) tagSize + ((flags & 0x80000000u) ? 32u : 0u);
    if (tagSize >= 32 && total <= (uint64_t) size) size -= (size_t) total;
  }
  return size;
}

} // namespace lossydetail

/** Decode an MP3 that is already in memory: `size` bytes at `fileData`, as
 *  they would be on disk. The decoder the file reader below uses, with its
 *  encoder-delay trimming and its ceiling. */
inline bool readMp3(const void* fileData, size_t size, WavData* out,
                    size_t maxSamples = kMaxDecodedSamples) {
  if (!fileData || !out) return false;
  // The bound the file reader holds before it reads a byte.
  if (size == 0 || size > (size_t) (1u << 30)) return false;
  const uint8_t* const bytes = static_cast<const uint8_t*>(fileData);

  codec::mp3dec_t decoder;
  codec::mp3dec_init(&decoder);
  out->samples.clear();
  out->sampleRate = 0;
  out->numChannels = 0;

  const uint8_t* p = bytes;
  int left = (int) lossydetail::mp3FramesEnd(bytes, size);
  float frame[codec::kMp3MaxSamplesPerFrame];
  // The encoder's Info frame, when there is one: decoded, it is a frame of
  // silence, and its delay and padding were never trimmed -- every MP3 from
  // LAME or ffmpeg started 2257 samples late at 44.1 kHz and ran long.
  bool first = true;
  size_t trimStart = 0, keep = 0; // in frames; keep 0 = as decoded
  while (left > 0) {
    codec::mp3dec_frame_info_t info{};
    const int samples = codec::mp3dec_decode_frame(&decoder, p, left, frame, &info);
    if (info.frame_bytes <= 0) break; // no more syncable data
    const uint8_t* const at = p + info.frame_offset;
    const int atBytes = info.frame_bytes - info.frame_offset;
    p += info.frame_bytes;
    left -= info.frame_bytes;
    if (samples <= 0) continue; // a skipped tag or a free-format probe
    if (first) {
      first = false;
      uint32_t frames = 0;
      int delay = 0, padding = 0;
      if (info.layer == 3 && lossydetail::mp3InfoTag(at, atBytes, &frames, &delay, &padding)) {
        trimStart = (size_t) delay;
        const uint64_t total = (uint64_t) frames * (uint64_t) samples;
        if (frames > 0 && total > (uint64_t) delay + (uint64_t) padding)
          keep = (size_t) (total - (uint64_t) delay - (uint64_t) padding);
        continue;
      }
    }
    if (out->numChannels == 0) {
      out->numChannels = (uint16_t) info.channels;
      out->sampleRate = (uint32_t) info.hz;
    } else if ((int) out->numChannels != info.channels ||
               (int) out->sampleRate != info.hz) {
      // A stream that changes shape mid-file would silently corrupt the
      // interleaving; refuse rather than produce nonsense.
      return false;
    }
    const size_t count = (size_t) samples * (size_t) info.channels;
    if (out->samples.size() + count > maxSamples) {
      out->samples.clear();
      out->samples.shrink_to_fit();
      return false;
    }
    out->samples.insert(out->samples.end(), frame, frame + count);
  }
  if (out->numChannels > 0 && (trimStart > 0 || keep > 0)) {
    const size_t channels = out->numChannels;
    const size_t decoded = out->samples.size() / channels;
    const size_t from = trimStart < decoded ? trimStart : decoded;
    size_t frames = decoded - from;
    if (keep > 0 && keep < frames) frames = keep;
    out->samples.erase(out->samples.begin(), out->samples.begin() + (std::ptrdiff_t) (from * channels));
    out->samples.resize(frames * channels);
  }
  return !out->samples.empty();
}

/** Read an MP3. Frames are decoded one at a time; the decoder finds its own
 *  sync, so ID3 tags and other leading junk are skipped rather than parsed.
 *
 *  Refused past `maxSamples` decoded samples, the ceiling every other format
 *  here is held to. MP3 had none, and it expands 30 to 60 times: a long mix
 *  dropped onto a sampler asked for gigabytes, and the bad_alloc took the
 *  host with it. The parameter exists for the test; callers leave it. */
inline bool readMp3(const char* path, WavData* out, size_t maxSamples = kMaxDecodedSamples) {
  if (!path || !out) return false;
  std::vector<uint8_t> bytes;
  if (!lossydetail::readFile(path, &bytes)) return false;
  return readMp3(bytes.data(), bytes.size(), out, maxSamples);
}

/** Decode an Ogg Vorbis file that is already in memory: `size` bytes at
 *  `fileData`, as they would be on disk. */
inline bool readOgg(const void* fileData, size_t size, WavData* out) {
  if (!fileData || !out) return false;
  // The bound the file reader holds before it reads a byte.
  if (size == 0 || size > (size_t) (1u << 30)) return false;
  const uint8_t* const bytes = static_cast<const uint8_t*>(fileData);

  int error = 0;
  codec::stb_vorbis* v =
      codec::stb_vorbis_open_memory(bytes, (int) size, &error, nullptr);
  if (!v) return false;

  const codec::stb_vorbis_info info = codec::stb_vorbis_get_info(v);
  if (info.channels <= 0) {
    codec::stb_vorbis_close(v);
    return false;
  }
  const size_t channels = (size_t) info.channels;
  out->samples.clear();
  out->sampleRate = (uint32_t) info.sample_rate;
  out->numChannels = (uint16_t) info.channels;

  // Decoded to the end of the stream, not to a length read off it.
  //
  // stb_vorbis's length is the granule position on the file's LAST page,
  // which is where the stream says it ENDS, not how much of it there is. A
  // stream captured from the middle of a broadcast keeps the broadcast's
  // granules, so half a second recorded twenty minutes in claims 53 million
  // frames. That length sized the output: such a file was zero-filled to
  // hundreds of megabytes before its half second decoded, and refused
  // outright past the ceiling -- a file every player opens. A crafted file
  // claims what it likes the same way. So the length is only a hint, taken
  // when the bytes could hold it, and the ceiling is held as the audio
  // arrives, as readMp3 holds it.
  const uint64_t declared = codec::stb_vorbis_stream_length_in_samples(v);
  // Vorbis spends at least a few hundred bits a second per channel; 64
  // samples a byte is generous past any real encoding.
  if (declared * channels <= kMaxDecodedSamples && declared * channels <= (uint64_t) size * 64u)
    out->samples.reserve((size_t) (declared * channels));
  constexpr int kChunkFrames = 4096;
  std::vector<float> chunk((size_t) kChunkFrames * channels);
  for (;;) {
    // The interleaved float path, so no planar-to-interleaved pass is needed.
    const int got = codec::stb_vorbis_get_samples_float_interleaved(
        v, info.channels, chunk.data(), (int) chunk.size());
    if (got <= 0) break;
    const size_t count = (size_t) got * channels;
    if (out->samples.size() + count > kMaxDecodedSamples) {
      codec::stb_vorbis_close(v);
      out->samples.clear();
      out->samples.shrink_to_fit();
      return false;
    }
    out->samples.insert(out->samples.end(), chunk.begin(), chunk.begin() + (std::ptrdiff_t) count);
  }
  codec::stb_vorbis_close(v);
  return !out->samples.empty();
}

/** Read an Ogg Vorbis file. */
inline bool readOgg(const char* path, WavData* out) {
  if (!path || !out) return false;
  std::vector<uint8_t> bytes;
  if (!lossydetail::readFile(path, &bytes)) return false;
  return readOgg(bytes.data(), bytes.size(), out);
}

/**
 * A decoder of last resort for readAudioFile: called with whatever the
 * built-in decoders did not open -- a container none of them knows (M4A,
 * WMA), or one they know carrying a codec they refuse (MP3 or ADPCM inside a
 * RIFF). Null by default, which keeps the fixed set: a file opens or not by
 * what the PLUGIN contains, identically on every machine.
 *
 * system_audio_format.h is what installs one -- enableSystemAudioFormats()
 * hands the operating system's decoders this slot -- and it is a slot rather
 * than an `#include` because those decoders need `<windows.h>` and Media
 * Foundation, which this header must not drag into every plugin that reads a
 * WAV.
 */
using AudioFileFallback = bool (*)(const char* path, WavData* out);

namespace audiofiledetail {

/** One per module, which is what a plugin wants: its setting is its own,
 *  whatever the host or another plugin chose. Atomic because samples load on
 *  worker threads while the application may be switching it. */
inline std::atomic<AudioFileFallback>& fallbackSlot() {
  static std::atomic<AudioFileFallback> slot{nullptr};
  return slot;
}

} // namespace audiofiledetail

/** Install (or, with nullptr, remove) the decoder of last resort. */
inline void setAudioFileFallback(AudioFileFallback decoder) {
  audiofiledetail::fallbackSlot().store(decoder, std::memory_order_release);
}

/** [any thread] The decoder setAudioFileFallback() installed in this
 *  module, or null for none. */
inline AudioFileFallback audioFileFallback() {
  return audiofiledetail::fallbackSlot().load(std::memory_order_acquire);
}

/** Read whichever of WAV/AIFF/FLAC/MP3/Ogg the user actually dropped on the
 *  plugin: sniff the magic, dispatch. The built-in decoder always goes first
 *  when the magic names one; the fallback, if one is installed, only ever
 *  sees what they could not open. */
inline bool readAudioFile(const char* path, WavData* out) {
  if (!path || !out) return false;
  std::FILE* f = ::sonore::pathio::open(path, "rb");
  if (!f) return false;
  uint8_t magic[10] = {0};
  const size_t got = std::fread(magic, 1, 10, f);
  // An ID3v2 tag says nothing about what follows it: usually MP3 frames, but
  // some taggers put one in front of a FLAC stream too.
  bool flacBehindTag = false;
  const size_t tag = got == 10 ? flacdetail::id3v2Length(magic, 10) : 0;
  if (tag > 0 && tag <= 0x7FFFFFFFu && std::fseek(f, (long) tag, SEEK_SET) == 0) {
    uint8_t after[4] = {0};
    flacBehindTag = std::fread(after, 1, 4, f) == 4 && std::memcmp(after, "fLaC", 4) == 0;
  }
  std::fclose(f);
  if (got < 4) return false;
  bool ok = false;
  if (std::memcmp(magic, "RIFF", 4) == 0) ok = readWav(path, out);
  else if (std::memcmp(magic, "FORM", 4) == 0) ok = readAiff(path, out);
  else if (std::memcmp(magic, "fLaC", 4) == 0) ok = readFlac(path, out);
  else if (std::memcmp(magic, "OggS", 4) == 0) ok = readOgg(path, out);
  // MP3 has no single magic: it may start with an ID3 tag or straight into a
  // frame sync (0xFF Ex). Anything else is refused rather than guessed at.
  // ADTS AAC shares the sync and lands here too; minimp3 refuses it, and the
  // fallback below is where it can still open.
  else if (std::memcmp(magic, "ID3", 3) == 0) ok = flacBehindTag ? readFlac(path, out) : readMp3(path, out);
  else if (magic[0] == 0xFF && (magic[1] & 0xE0) == 0xE0) ok = readMp3(path, out);
  if (ok) return true;
  const AudioFileFallback fallback = audioFileFallback();
  if (!fallback) return false;
  // Whatever a refusing decoder left half-filled is not the fallback's to
  // inherit: it starts from an empty result, as the first decoder did.
  out->samples.clear();
  out->sampleRate = 0;
  out->numChannels = 0;
  return fallback(path, out);
}

/**
 * readAudioFile() for a file that is already in memory: `size` bytes at
 * `fileData`, exactly as they would be on disk -- an impulse response or a
 * sample compiled into the plugin's binary, which is how a plugin ships
 * factory content that cannot go missing. The same magic, the same five
 * decoders, the same refusals and ceilings.
 *
 * The fallback setAudioFileFallback() installs is not asked: the decoders it
 * stands for are the operating system's, and they open files, not memory.
 */
inline bool readAudioFile(const void* fileData, size_t size, WavData* out) {
  if (!fileData || !out || size < 4) return false;
  const uint8_t* const magic = static_cast<const uint8_t*>(fileData);
  // An ID3v2 tag says nothing about what follows it, as in the file reader.
  const size_t tag = size >= 10 ? flacdetail::id3v2Length(magic, 10) : 0;
  const bool flacBehindTag = tag > 0 && tag <= size - 4 && std::memcmp(magic + tag, "fLaC", 4) == 0;
  if (std::memcmp(magic, "RIFF", 4) == 0) return readWav(fileData, size, out);
  if (std::memcmp(magic, "FORM", 4) == 0) return readAiff(fileData, size, out);
  if (std::memcmp(magic, "fLaC", 4) == 0) return readFlac(fileData, size, out);
  if (std::memcmp(magic, "OggS", 4) == 0) return readOgg(fileData, size, out);
  if (std::memcmp(magic, "ID3", 3) == 0)
    return flacBehindTag ? readFlac(fileData, size, out) : readMp3(fileData, size, out);
  if (magic[0] == 0xFF && (magic[1] & 0xE0) == 0xE0) return readMp3(fileData, size, out);
  return false;
}

} // namespace sonore
