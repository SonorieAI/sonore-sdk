// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: writing FLAC.
//
// We already decode FLAC ourselves; this is the other half. It is worth owning
// for the same reason: the format is simple enough to be provable, and a
// lossless encoder can be checked the only way that counts: encode, decode,
// compare SAMPLE FOR SAMPLE, and then have a completely independent decoder
// (ffmpeg) do the same.
//
// Fixed predictors only, no LPC. The four fixed predictors are successive
// differences, they cost nothing to compute, and on real material they get
// most of what LPC would: an encoder that spends ten times the CPU for another
// five percent is the wrong trade for a plugin exporting a render. The output
// is fully compliant either way: a decoder cannot tell which predictors an
// encoder chose to use.
//
// Offline: allocates and writes files.

#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "path_io.h"

namespace sonore {
namespace flacenc {

/** MSB-first bit writer. FLAC is bit-packed, and a writer that flushes a
 *  partial byte at the wrong moment produces a file that decodes as garbage
 *  from that point on.
 *
 *  Bits are gathered in a word and go out a byte at a time as they fill one.
 *  This wrote every bit on its own -- a shift, a count and a branch each --
 *  and a four-minute render is half a billion bits. The bytes are the same:
 *  a write still puts the low `bits` bits of its value down, highest first,
 *  after everything written before it. */
class BitWriter {
public:
  /** The low `bits` bits of `value`, 1 to 64, highest first. 0 or fewer
   *  writes nothing. */
  void writeBits(uint64_t value, int bits) {
    if (bits <= 0) return;
    if (bits > 32) {
      put((uint32_t) ((value >> 32) & ((1ull << (bits - 32)) - 1u)), bits - 32);
      put((uint32_t) value, 32);
      return;
    }
    put((uint32_t) (value & ((1ull << bits) - 1u)), bits);
  }

  /** `value` in `bits`-bit two's complement, which is what FLAC's signed
   *  fields are. */
  void writeSigned(int64_t value, int bits) { writeBits((uint64_t) value, bits); }

  /** Unary quotient then `param` remainder bits: FLAC's Rice coding. */
  void writeRice(int32_t residual, int param) {
    // Zigzag: interleave positive and negative so small magnitudes of either
    // sign stay small unsigned numbers.
    //
    // The left half is done in UNSIGNED arithmetic on purpose: shifting a
    // negative int left is undefined behaviour in C++17 (it only became
    // defined in C++20), and this line is reached with negative residuals on
    // every real signal. Every compiler we use happens to produce the
    // intended bits anyway, which is exactly why UBSan had to be the thing
    // that caught it. The arithmetic right shift of the sign is
    // implementation-defined rather than undefined and is the standard
    // zigzag idiom, so that half stays.
    const uint32_t folded = ((uint32_t) residual << 1) ^ (uint32_t) (residual >> 31);
    uint32_t quotient = folded >> param;
    const uint32_t rest = param > 0 ? folded & ((1u << param) - 1u) : 0u;
    // The quotient's zeros, its one and the remainder, in one write when
    // they fit in 32 bits -- nearly always -- and the zeros first otherwise.
    if (quotient < 32u && quotient + 1u + (uint32_t) param <= 32u) {
      put((1u << param) | rest, (int) quotient + 1 + param);
      return;
    }
    for (; quotient >= 32u; quotient -= 32u) put(0u, 32);
    put(1u, (int) quotient + 1);
    if (param > 0) put(rest, param);
  }

  /** Pad to the next byte with zeros. */
  void alignToByte() {
    if (bitCount_ != 0) put(0u, 8 - bitCount_);
  }

  /** The whole bytes written so far. A partial last byte is not in it until
   *  alignToByte(). */
  const std::vector<uint8_t>& bytes() const { return data_; }
  /** How many whole bytes are written. */
  size_t byteSize() const { return data_.size(); }
  /** Forget everything, the partial byte included, to start a new stream. */
  void clear() {
    data_.clear();
    current_ = 0;
    bitCount_ = 0;
  }

private:
  /** The low `bits` (1 to 32) of `value`, which holds nothing above them. */
  void put(uint32_t value, int bits) {
    // At most 7 bits wait in `current_`, so 39 at most are held here.
    uint64_t held = ((uint64_t) current_ << bits) | value;
    int count = bitCount_ + bits;
    while (count >= 8) {
      count -= 8;
      data_.push_back((uint8_t) (held >> count));
    }
    current_ = (uint32_t) (held & ((1u << count) - 1u));
    bitCount_ = count;
  }

  std::vector<uint8_t> data_;
  uint32_t current_ = 0; // the bits of a byte not yet complete, at the bottom
  int bitCount_ = 0;     // how many, 0..7
};

/** FLAC's CRC-8 (x^8 + x^2 + x + 1) over the frame header. A decoder that
 *  verifies it (ffmpeg does) rejects the frame outright if this is wrong,
 *  so it is not optional however tempting a zero looks. */
inline uint8_t crc8(const uint8_t* data, size_t size) {
  uint8_t crc = 0;
  for (size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (int b = 0; b < 8; ++b) crc = (uint8_t) ((crc & 0x80) ? ((crc << 1) ^ 0x07) : (crc << 1));
  }
  return crc;
}

/** FLAC's CRC-16 (x^16 + x^15 + x^2 + 1) over the whole frame. By table: a
 *  render's frames are every byte of the file, and eight conditional shifts
 *  a byte was a quarter of what writing a FLAC cost. Each entry is what the
 *  eight steps do to a byte that has reached the top of the register; the
 *  steps are linear, so a lookup a byte gives the same CRC for every input. */
inline uint16_t crc16(const uint8_t* data, size_t size) {
  struct Table {
    uint16_t entry[256];
    Table() {
      for (int i = 0; i < 256; ++i) {
        uint16_t crc = (uint16_t) (i << 8);
        for (int b = 0; b < 8; ++b)
          crc = (uint16_t) ((crc & 0x8000) ? ((crc << 1) ^ 0x8005) : (crc << 1));
        entry[i] = crc;
      }
    }
  };
  static const Table table;
  uint16_t crc = 0;
  for (size_t i = 0; i < size; ++i)
    crc = (uint16_t) ((uint16_t) (crc << 8) ^ table.entry[(crc >> 8) ^ data[i]]);
  return crc;
}

/** The UTF-8-style coded number FLAC uses for frame indices. */
inline void writeUtf8(BitWriter& writer, uint64_t value) {
  if (value < 0x80) {
    writer.writeBits(value, 8);
    return;
  }
  int bytes = 2;
  uint64_t limit = 0x800;
  while (value >= limit && bytes < 7) {
    ++bytes;
    limit <<= 5;
  }
  // The LEAD byte's payload only: `bytes` ones, a zero, then 7 - bytes bits.
  // The other 6 * (bytes - 1) bits go out in the continuation bytes below.
  // This once wrote the whole value's width here and then the continuation
  // bytes as well, so every multi-byte number was 6 * (bytes - 1) zero bits
  // too long. Frames 0..127 take one byte, so nothing noticed until frame 128
  // -- sample 524288, 10.9 s into a 48 kHz file -- where ffmpeg stopped with
  // "utf8 fscked" and our own decoder lost sync. The tests' files were six
  // frames long.
  const int leadBits = 7 - bytes;
  uint64_t lead = 0;
  for (int i = 0; i < bytes; ++i) lead = (lead << 1) | 1u;
  lead <<= 1; // the terminating zero
  writer.writeBits(lead, bytes + 1);
  writer.writeBits(value >> (6 * (bytes - 1)), leadBits);
  for (int i = bytes - 2; i >= 0; --i) {
    writer.writeBits(2, 2); // the 10 continuation prefix
    writer.writeBits((value >> (6 * i)) & 0x3f, 6);
  }
}

/** Residuals for one fixed predictor order, and the total magnitude that
 *  decides whether it is the best one. */
inline uint64_t fixedResiduals(const int32_t* samples, size_t count, int order,
                               std::vector<int64_t>* out) {
  out->assign(count > (size_t) order ? count - (size_t) order : 0, 0);
  uint64_t total = 0;
  for (size_t i = (size_t) order; i < count; ++i) {
    int64_t r = 0;
    switch (order) {
      case 0: r = samples[i]; break;
      case 1: r = (int64_t) samples[i] - samples[i - 1]; break;
      case 2: r = (int64_t) samples[i] - 2ll * samples[i - 1] + samples[i - 2]; break;
      case 3:
        r = (int64_t) samples[i] - 3ll * samples[i - 1] + 3ll * samples[i - 2] - samples[i - 3];
        break;
      default:
        r = (int64_t) samples[i] - 4ll * samples[i - 1] + 6ll * samples[i - 2] -
            4ll * samples[i - 3] + samples[i - 4];
        break;
    }
    (*out)[i - (size_t) order] = r;
    total += (uint64_t) (r < 0 ? -r : r);
  }
  return total;
}

/** fixedResiduals' totals for orders 0 to 4 at once, in one pass and without
 *  storing a residual: choosing the order needs only these, and computing
 *  and storing all five sets of residuals to keep one was most of the
 *  encoder's arithmetic. The same formulas, so the same totals; an order
 *  the block is too short for gets its residuals from none of its samples
 *  and is never chosen. */
inline void fixedTotals(const int32_t* samples, size_t count, uint64_t totals[5]) {
  auto magnitude = [](int64_t r) { return (uint64_t) (r < 0 ? -r : r); };
  for (int k = 0; k < 5; ++k) totals[k] = 0;
  const size_t head = count < 4 ? count : 4;
  for (size_t i = 0; i < head; ++i) {
    const int64_t s = samples[i];
    totals[0] += magnitude(s);
    if (i >= 1) totals[1] += magnitude(s - samples[i - 1]);
    if (i >= 2) totals[2] += magnitude(s - 2ll * samples[i - 1] + samples[i - 2]);
    if (i >= 3) totals[3] += magnitude(s - 3ll * samples[i - 1] + 3ll * samples[i - 2] - samples[i - 3]);
  }
  for (size_t i = 4; i < count; ++i) {
    const int64_t s = samples[i];
    totals[0] += magnitude(s);
    totals[1] += magnitude(s - samples[i - 1]);
    totals[2] += magnitude(s - 2ll * samples[i - 1] + samples[i - 2]);
    totals[3] += magnitude(s - 3ll * samples[i - 1] + 3ll * samples[i - 2] - samples[i - 3]);
    totals[4] += magnitude(s - 4ll * samples[i - 1] + 6ll * samples[i - 2] - 4ll * samples[i - 3] +
                           samples[i - 4]);
  }
}

/** The Rice parameter that spends the fewest bits on these residuals: up to
 *  30, which the 5-bit parameters of residual method 1 can carry.
 *
 *  It stopped at 14, the most the 4-bit method-0 field holds below its
 *  escape code. 24-bit audio routinely needs more: with every residual
 *  spilling into the unary part, ten seconds of -6 dBFS noise came out 12.8
 *  times the size of the PCM, a full-scale tone 25 times, and ffmpeg
 *  refused to decode any of it. */
inline int bestRiceParam(const std::vector<int64_t>& residuals) {
  if (residuals.empty()) return 0;
  uint64_t sum = 0;
  for (int64_t r : residuals) sum += (uint64_t) (r < 0 ? -r : r);
  const double mean = (double) sum / (double) residuals.size();
  int param = 0;
  while (param < 30 && (double) (1ull << (param + 1)) < mean * 1.4) ++param;
  return param;
}

} // namespace flacenc

/**
 * Write a FLAC file from interleaved float samples.
 *
 * `bitDepth` is 16 or 24: the float input is quantised to it, which is what
 * makes the result LOSSLESS with respect to what was written: a float sample
 * is not losslessly representable in a format that stores integers, and
 * claiming otherwise would be the lie this whole file exists to avoid.
 */
inline bool writeFlac(const char* path, const float* interleaved, size_t numFrames,
                      uint16_t numChannels, uint32_t sampleRate, int bitDepth = 24) {
  using namespace flacenc;
  if (!path || !interleaved || numFrames == 0 || numChannels == 0 || numChannels > 8)
    return false;
  if (bitDepth != 16 && bitDepth != 24) return false;
  // STREAMINFO holds the rate in twenty bits and the length in thirty-six,
  // and a rate of zero is invalid. writeBits keeps the low bits of whatever
  // it is given, so these were written and reported as success: a zero-rate
  // file no decoder opens, readFlac included, and 1,096,576 Hz quietly
  // stored as 48000 -- a file that plays at the wrong speed. Refused, as
  // writeWav and writeAiff refuse a zero rate.
  if (sampleRate == 0 || sampleRate > 0xFFFFFu) return false;
  if ((uint64_t) numFrames > 0xFFFFFFFFFull) return false;

  constexpr size_t kBlockSize = 4096; // code 0b1100, so no extra header bytes
  // 2^(N-1), the scale every reader in the SDK divides by, clipped below to
  // the one code a positive full scale lacks. With 2^(N-1) - 1 a file read
  // and written again came back with half its samples one step smaller.
  const double scale = bitDepth == 16 ? 32768.0 : 8388608.0;

  std::vector<uint8_t> out;
  out.insert(out.end(), {'f', 'L', 'a', 'C'});

  // STREAMINFO, the one mandatory metadata block.
  {
    BitWriter w;
    w.writeBits(kBlockSize, 16); // min block size
    w.writeBits(kBlockSize, 16); // max block size
    w.writeBits(0, 24);          // min frame size, 0 = unknown
    w.writeBits(0, 24);          // max frame size, 0 = unknown
    w.writeBits(sampleRate, 20);
    w.writeBits((uint64_t) (numChannels - 1), 3);
    w.writeBits((uint64_t) (bitDepth - 1), 5);
    w.writeBits((uint64_t) numFrames, 36);
    for (int i = 0; i < 16; ++i) w.writeBits(0, 8); // MD5: zero means "not computed", which is legal
    const std::vector<uint8_t>& info = w.bytes();

    out.push_back(0x80); // last-metadata-block flag | type 0
    out.push_back(0);
    out.push_back(0);
    out.push_back((uint8_t) info.size());
    out.insert(out.end(), info.begin(), info.end());
  }

  std::vector<int32_t> channelSamples[8];
  std::vector<int64_t> best; // the chosen order's residuals, refilled each subframe
  BitWriter frame;           // one frame's bits, emptied for the next: it keeps its memory

  uint64_t frameNumber = 0;
  for (size_t start = 0; start < numFrames; start += kBlockSize) {
    const size_t count = (numFrames - start) < kBlockSize ? (numFrames - start) : kBlockSize;

    for (uint16_t c = 0; c < numChannels; ++c) {
      channelSamples[c].assign(count, 0);
      for (size_t i = 0; i < count; ++i) {
        double v = std::nearbyint((double) interleaved[(start + i) * numChannels + c] * scale);
        if (!(v >= -scale)) v = -scale; // NaN too
        if (v > scale - 1.0) v = scale - 1.0;
        channelSamples[c][i] = (int32_t) v;
      }
    }

    frame.clear();
    frame.writeBits(0x3ffe, 14); // sync
    frame.writeBits(0, 1);       // reserved
    frame.writeBits(0, 1);       // fixed block size, so the number below is a FRAME index
    // A short final block would need its own size code; the sample count in
    // STREAMINFO tells the decoder where the audio really ends, and every
    // frame carries its own block size here.
    frame.writeBits(count == kBlockSize ? 0xC : 0x7, 4); // 0xC = 4096, 0x7 = 16-bit follows
    frame.writeBits(0, 4);                               // sample rate: from STREAMINFO
    frame.writeBits((uint64_t) (numChannels - 1), 4);    // independent channels
    frame.writeBits(bitDepth == 16 ? 4 : 6, 3);          // 100 = 16 bit, 110 = 24 bit
    frame.writeBits(0, 1);                               // reserved
    writeUtf8(frame, frameNumber++);
    if (count != kBlockSize) frame.writeBits(count - 1, 16);

    // The CRC-8 covers the header as BYTES, so it has to be aligned and read
    // back out of the writer rather than computed from the values.
    frame.alignToByte();
    const uint8_t headerCrc = crc8(frame.bytes().data(), frame.byteSize());
    frame.writeBits(headerCrc, 8);

    for (uint16_t c = 0; c < numChannels; ++c) {
      const std::vector<int32_t>& samples = channelSamples[c];

      // Constant subframes are worth checking for: silence and DC are common
      // in rendered material and cost four bytes instead of thousands.
      bool constant = true;
      for (size_t i = 1; i < count; ++i)
        if (samples[i] != samples[0]) {
          constant = false;
          break;
        }
      if (constant) {
        frame.writeBits(0, 1);      // mandatory zero
        frame.writeBits(0, 6);      // CONSTANT
        frame.writeBits(0, 1);      // no wasted bits
        frame.writeSigned(samples[0], bitDepth);
        continue;
      }

      // The first order with the smallest total, as when each order's
      // residuals were made in turn and the best kept; then that order's.
      uint64_t totals[5];
      fixedTotals(samples.data(), count, totals);
      int bestOrder = 0;
      uint64_t bestTotal = UINT64_MAX;
      for (int order = 0; order <= 4 && (size_t) order < count; ++order) {
        if (totals[order] < bestTotal) {
          bestTotal = totals[order];
          bestOrder = order;
        }
      }
      fixedResiduals(samples.data(), count, bestOrder, &best);

      frame.writeBits(0, 1);                            // mandatory zero
      frame.writeBits((uint64_t) (8 + bestOrder), 6);   // FIXED, this order
      frame.writeBits(0, 1);                            // no wasted bits
      for (int i = 0; i < bestOrder; ++i) frame.writeSigned(samples[(size_t) i], bitDepth);

      // Method 0 (4-bit parameters, 15 the escape) while the parameter fits
      // under 15; method 1 (5-bit, 31 the escape) above it.
      const int param = bestRiceParam(best);
      const int method = param < 15 ? 0 : 1;
      frame.writeBits((uint64_t) method, 2);
      frame.writeBits(0, 4); // partition order 0: one partition
      frame.writeBits((uint64_t) param, method == 0 ? 4 : 5);
      for (int64_t r : best) frame.writeRice((int32_t) r, param);
    }

    frame.alignToByte();
    const uint16_t frameCrc = crc16(frame.bytes().data(), frame.byteSize());
    out.insert(out.end(), frame.bytes().begin(), frame.bytes().end());
    out.push_back((uint8_t) (frameCrc >> 8));
    out.push_back((uint8_t) (frameCrc & 0xff));
  }

  std::FILE* f = ::sonore::pathio::open(path, "wb");
  if (!f) return false;
  const bool ok = std::fwrite(out.data(), 1, out.size(), f) == out.size();
  std::fclose(f);
  return ok;
}

} // namespace sonore
