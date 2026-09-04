// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: audio files beyond WAV.
//
// AIFF (and AIFF-C "sowt") because Mac-sourced sample libraries still ship it,
// and FLAC because impulse-response packs do. The FLAC decoder is OUR OWN
// implementation of the published format, no library, no licence to carry,
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
// which container the user dropped on them.

#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "audiofile_codecs.h"
#include "wav.h"

namespace sonore {

namespace aiffdetail {

inline uint32_t readU32BE(const uint8_t* p) {
  return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) | ((uint32_t) p[2] << 8) |
         (uint32_t) p[3];
}
inline uint16_t readU16BE(const uint8_t* p) { return (uint16_t) ((p[0] << 8) | p[1]); }

/** AIFF stores the sample rate as an 80-bit IEEE 754 extended float: the one
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

/** Read an AIFF or AIFF-C file: 16/24-bit PCM, big-endian ("NONE") or
 *  little-endian ("sowt", what modern Macs actually write). */
inline bool readAiff(const char* path, WavData* out) {
  if (!path || !out) return false;
  std::FILE* f = std::fopen(path, "rb");
  if (!f) return false;
  std::fseek(f, 0, SEEK_END);
  const long size = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if (size < 12) { std::fclose(f); return false; }
  std::vector<uint8_t> data((size_t) size);
  const bool okRead = std::fread(data.data(), 1, (size_t) size, f) == (size_t) size;
  std::fclose(f);
  if (!okRead) return false;

  using namespace aiffdetail;
  if (std::memcmp(data.data(), "FORM", 4) != 0) return false;
  const bool aifc = std::memcmp(data.data() + 8, "AIFC", 4) == 0;
  if (!aifc && std::memcmp(data.data() + 8, "AIFF", 4) != 0) return false;

  uint16_t channels = 0, bits = 0;
  uint32_t frames = 0;
  double rate = 0.0;
  bool littleEndian = false;
  const uint8_t* audio = nullptr;
  size_t audioBytes = 0;

  // Chunk walk, exactly like the WAV reader: trusting fixed offsets is the
  // classic shortcut that breaks on files written by real software.
  size_t pos = 12;
  while (pos + 8 <= data.size()) {
    const uint8_t* hdr = data.data() + pos;
    const uint32_t chunkLen = readU32BE(hdr + 4);
    const uint8_t* body = hdr + 8;
    if (pos + 8 + chunkLen > data.size()) break;
    if (std::memcmp(hdr, "COMM", 4) == 0 && chunkLen >= 18) {
      channels = readU16BE(body);
      frames = readU32BE(body + 2);
      bits = readU16BE(body + 6);
      rate = readExtended80(body + 8);
      if (aifc && chunkLen >= 22) {
        if (std::memcmp(body + 18, "sowt", 4) == 0) littleEndian = true;
        else if (std::memcmp(body + 18, "NONE", 4) != 0) return false; // compressed AIFC
      }
    } else if (std::memcmp(hdr, "SSND", 4) == 0 && chunkLen >= 8) {
      const uint32_t offset = readU32BE(body);
      if ((size_t) offset + 8 > chunkLen) break;
      audio = body + 8 + offset;
      audioBytes = chunkLen - 8 - offset;
    }
    pos += 8 + chunkLen + (chunkLen & 1); // chunks are word-aligned
  }

  if (!audio || channels == 0 || rate <= 0.0 || rate > 4.0e9 || (bits != 16 && bits != 24))
    return false;
  const size_t bytesPer = bits / 8u;
  size_t total = audioBytes / bytesPer;
  const size_t wanted = (size_t) frames * channels;
  if (wanted > 0 && wanted < total) total = wanted;

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

namespace aiffwrite {

inline void writeU32BE(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back((uint8_t) (v >> 24));
  out.push_back((uint8_t) ((v >> 16) & 0xff));
  out.push_back((uint8_t) ((v >> 8) & 0xff));
  out.push_back((uint8_t) (v & 0xff));
}

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

} // namespace aiffwrite

/** Write an AIFF file: 16- or 24-bit PCM, big-endian, which is what the
 *  format means by default and what a Mac tool expects to find. */
inline bool writeAiff(const char* path, const float* interleaved, size_t numFrames,
                      uint16_t numChannels, uint32_t sampleRate, int bitDepth = 24) {
  if (!path || !interleaved || numChannels == 0) return false;
  if (bitDepth != 16 && bitDepth != 24) return false;
  using namespace aiffwrite;

  const size_t bytesPer = (size_t) (bitDepth / 8);
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

  for (size_t i = 0; i < total; ++i) {
    float v = interleaved[i];
    if (v > 1.0f) v = 1.0f;
    if (v < -1.0f) v = -1.0f;
    if (bitDepth == 16) {
      const int32_t q = (int32_t) std::lround(v * 32767.0f);
      out.push_back((uint8_t) ((q >> 8) & 0xff));
      out.push_back((uint8_t) (q & 0xff));
    } else {
      const int32_t q = (int32_t) std::lround(v * 8388607.0f);
      out.push_back((uint8_t) ((q >> 16) & 0xff));
      out.push_back((uint8_t) ((q >> 8) & 0xff));
      out.push_back((uint8_t) (q & 0xff));
    }
  }

  std::FILE* f = std::fopen(path, "wb");
  if (!f) return false;
  const bool ok = std::fwrite(out.data(), 1, out.size(), f) == out.size();
  std::fclose(f);
  return ok;
}

// ── FLAC ─────────────────────────────────────────────────────────────────────

namespace flacdetail {

/** MSB-first bit reader over a byte buffer. Out-of-data is sticky: every read
 *  after the end returns 0 and flags failure, so decode loops need one check
 *  at the end instead of one per read. */
struct BitReader {
  const uint8_t* data;
  size_t size;
  size_t byte = 0;
  int bit = 0; // bits consumed of data[byte], 0..7
  bool failed = false;

  BitReader(const uint8_t* d, size_t s) : data(d), size(s) {}

  inline uint32_t readBit() {
    if (byte >= size) { failed = true; return 0; }
    const uint32_t v = (data[byte] >> (7 - bit)) & 1u;
    if (++bit == 8) { bit = 0; ++byte; }
    return v;
  }

  inline uint64_t readBits(int n) {
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

  /** Unary: count zeros up to the terminating 1. */
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
      for (int i = 0; i < count; ++i) out[idx++] = (int32_t) readRice(br, param);
    }
    if (br.failed) return false;
  }
  return true;
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
    for (int i = order; i < blockSize; ++i) {
      const int64_t r = out[i];
      int64_t p = 0;
      switch (order) {
        case 0: p = 0; break;
        case 1: p = out[i - 1]; break;
        case 2: p = 2ll * out[i - 1] - out[i - 2]; break;
        case 3: p = 3ll * out[i - 1] - 3ll * out[i - 2] + out[i - 3]; break;
        case 4: p = 4ll * out[i - 1] - 6ll * out[i - 2] + 4ll * out[i - 3] - out[i - 4]; break;
      }
      out[i] = (int32_t) (p + r);
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
    for (int i = order; i < blockSize; ++i) {
      int64_t acc = 0;
      for (int j = 0; j < order; ++j) acc += (int64_t) coef[j] * (int64_t) out[i - 1 - j];
      out[i] = (int32_t) ((acc >> shift) + out[i]);
    }
  } else {
    return false; // reserved type
  }

  if (wasted > 0)
    for (int i = 0; i < blockSize; ++i) out[i] = (int32_t) ((uint32_t) out[i] << wasted);
  return !br.failed;
}

} // namespace flacdetail

/** Read a FLAC file: our own decoder for the published format. 16/24-bit,
 *  mono/stereo (all four stereo decorrelation modes), any block size. */
inline bool readFlac(const char* path, WavData* out) {
  if (!path || !out) return false;
  std::FILE* f = std::fopen(path, "rb");
  if (!f) return false;
  std::fseek(f, 0, SEEK_END);
  const long size = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if (size < 42) { std::fclose(f); return false; }
  std::vector<uint8_t> data((size_t) size);
  const bool okRead = std::fread(data.data(), 1, (size_t) size, f) == (size_t) size;
  std::fclose(f);
  if (!okRead || std::memcmp(data.data(), "fLaC", 4) != 0) return false;

  using namespace flacdetail;

  // Metadata blocks: STREAMINFO is mandatory and first.
  size_t pos = 4;
  uint32_t sampleRate = 0;
  int channels = 0, bps = 0;
  uint64_t totalSamples = 0;
  bool sawStreamInfo = false;
  while (pos + 4 <= data.size()) {
    const bool last = (data[pos] & 0x80) != 0;
    const int type = data[pos] & 0x7f;
    const uint32_t len =
        ((uint32_t) data[pos + 1] << 16) | ((uint32_t) data[pos + 2] << 8) | data[pos + 3];
    pos += 4;
    if (pos + len > data.size()) return false;
    if (type == 0 && len >= 34) {
      const uint8_t* si = data.data() + pos;
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
  if (!sawStreamInfo || sampleRate == 0 || channels < 1 || channels > 2 ||
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
  std::vector<int32_t> ch0, ch1;

  BitReader br(data.data() + pos, data.size() - pos);
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

    const int frameChans = chanAsgn < 8 ? chanAsgn + 1 : 2;
    if (frameChans != channels) return false;
    ch0.assign((size_t) blockSize, 0);
    if (channels > 1) ch1.assign((size_t) blockSize, 0);

    for (int c = 0; c < frameChans; ++c) {
      int subBps = frameBps;
      // The SIDE channel carries one extra bit: forgetting this corrupts
      // every stereo-decorrelated file.
      if ((chanAsgn == 8 && c == 1) || (chanAsgn == 9 && c == 0) ||
          (chanAsgn == 10 && c == 1))
        ++subBps;
      if (!decodeSubframe(br, (c == 0 ? ch0 : ch1).data(), blockSize, subBps)) return false;
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

    for (int i = 0; i < blockSize; ++i) {
      out->samples.push_back((float) ch0[i] * scale);
      if (channels > 1) out->samples.push_back((float) ch1[i] * scale);
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

// ── Lossy formats ────────────────────────────────────────────────────────────

namespace lossydetail {

/** Slurp a whole file. Both lossy decoders want the bytes in memory, and a
 *  sample or impulse response is small enough that streaming would be
 *  complexity without a payer. */
inline bool readFile(const char* path, std::vector<uint8_t>* out) {
  std::FILE* f = std::fopen(path, "rb");
  if (!f) return false;
  std::fseek(f, 0, SEEK_END);
  const long size = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if (size <= 0) {
    std::fclose(f);
    return false;
  }
  out->resize((size_t) size);
  const bool ok = std::fread(out->data(), 1, (size_t) size, f) == (size_t) size;
  std::fclose(f);
  return ok;
}

} // namespace lossydetail

/** Read an MP3. Frames are decoded one at a time; the decoder finds its own
 *  sync, so ID3 tags and other leading junk are skipped rather than parsed. */
inline bool readMp3(const char* path, WavData* out) {
  if (!path || !out) return false;
  std::vector<uint8_t> bytes;
  if (!lossydetail::readFile(path, &bytes)) return false;

  codec::mp3dec_t decoder;
  codec::mp3dec_init(&decoder);
  out->samples.clear();
  out->sampleRate = 0;
  out->numChannels = 0;

  const uint8_t* p = bytes.data();
  int left = (int) bytes.size();
  float frame[MINIMP3_MAX_SAMPLES_PER_FRAME];
  while (left > 0) {
    codec::mp3dec_frame_info_t info{};
    const int samples = codec::mp3dec_decode_frame(&decoder, p, left, frame, &info);
    if (info.frame_bytes <= 0) break; // no more syncable data
    p += info.frame_bytes;
    left -= info.frame_bytes;
    if (samples <= 0) continue; // a skipped tag or a free-format probe
    if (out->numChannels == 0) {
      out->numChannels = (uint16_t) info.channels;
      out->sampleRate = (uint32_t) info.hz;
    } else if ((int) out->numChannels != info.channels ||
               (int) out->sampleRate != info.hz) {
      // A stream that changes shape mid-file would silently corrupt the
      // interleaving; refuse rather than produce nonsense.
      return false;
    }
    out->samples.insert(out->samples.end(), frame,
                        frame + (size_t) samples * (size_t) info.channels);
  }
  return !out->samples.empty();
}

/** Read an Ogg Vorbis file. */
inline bool readOgg(const char* path, WavData* out) {
  if (!path || !out) return false;
  std::vector<uint8_t> bytes;
  if (!lossydetail::readFile(path, &bytes)) return false;

  int error = 0;
  codec::stb_vorbis* v =
      codec::stb_vorbis_open_memory(bytes.data(), (int) bytes.size(), &error, nullptr);
  if (!v) return false;

  const codec::stb_vorbis_info info = codec::stb_vorbis_get_info(v);
  const unsigned int frames = codec::stb_vorbis_stream_length_in_samples(v);
  if (info.channels <= 0 || frames == 0) {
    codec::stb_vorbis_close(v);
    return false;
  }

  // stb_vorbis derives `frames` from the granule position on the file's LAST
  // page, which a crafted file controls -- so cap before assigning, or a bogus
  // billion-frame granulepos zero-fills gigabytes and crashes the host.
  if ((uint64_t) frames * (uint64_t) info.channels > kMaxDecodedSamples) {
    codec::stb_vorbis_close(v);
    return false;
  }
  out->sampleRate = (uint32_t) info.sample_rate;
  out->numChannels = (uint16_t) info.channels;
  out->samples.assign((size_t) frames * (size_t) info.channels, 0.0f);
  // The interleaved float path, so no planar-to-interleaved pass is needed.
  const int got = codec::stb_vorbis_get_samples_float_interleaved(
      v, info.channels, out->samples.data(), (int) out->samples.size());
  codec::stb_vorbis_close(v);
  if (got <= 0) return false;
  // A stream can end shorter than its header claimed; trust what decoded.
  out->samples.resize((size_t) got * (size_t) info.channels);
  return !out->samples.empty();
}

/** Read whichever of WAV/AIFF/FLAC/MP3/Ogg the user actually dropped on the
 *  plugin: sniff the magic, dispatch. */
inline bool readAudioFile(const char* path, WavData* out) {
  if (!path || !out) return false;
  std::FILE* f = std::fopen(path, "rb");
  if (!f) return false;
  uint8_t magic[4] = {0};
  const size_t got = std::fread(magic, 1, 4, f);
  std::fclose(f);
  if (got != 4) return false;
  if (std::memcmp(magic, "RIFF", 4) == 0) return readWav(path, out);
  if (std::memcmp(magic, "FORM", 4) == 0) return readAiff(path, out);
  if (std::memcmp(magic, "fLaC", 4) == 0) return readFlac(path, out);
  if (std::memcmp(magic, "OggS", 4) == 0) return readOgg(path, out);
  // MP3 has no single magic: it may start with an ID3 tag or straight into a
  // frame sync (0xFF Ex). Anything else is refused rather than guessed at.
  if (std::memcmp(magic, "ID3", 3) == 0) return readMp3(path, out);
  if (magic[0] == 0xFF && (magic[1] & 0xE0) == 0xE0) return readMp3(path, out);
  return false;
}

} // namespace sonore
