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

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace sonore {
namespace flacenc {

/** MSB-first bit writer. FLAC is bit-packed, and a writer that flushes a
 *  partial byte at the wrong moment produces a file that decodes as garbage
 *  from that point on. */
class BitWriter {
public:
  void writeBits(uint64_t value, int bits) {
    for (int i = bits - 1; i >= 0; --i) writeBit((value >> i) & 1u);
  }

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
    const uint32_t quotient = folded >> param;
    for (uint32_t i = 0; i < quotient; ++i) writeBit(0);
    writeBit(1);
    if (param > 0) writeBits(folded & ((1u << param) - 1u), param);
  }

  /** Pad to the next byte with zeros. */
  void alignToByte() {
    while (bitCount_ != 0) writeBit(0);
  }

  const std::vector<uint8_t>& bytes() const { return data_; }
  size_t byteSize() const { return data_.size(); }
  void clear() {
    data_.clear();
    current_ = 0;
    bitCount_ = 0;
  }

private:
  void writeBit(uint32_t bit) {
    current_ = (uint8_t) ((current_ << 1) | (bit & 1u));
    if (++bitCount_ == 8) {
      data_.push_back(current_);
      current_ = 0;
      bitCount_ = 0;
    }
  }

  std::vector<uint8_t> data_;
  uint8_t current_ = 0;
  int bitCount_ = 0;
};

/** FLAC's CRC-8 (x^8 + x^2 + x + 1) over the frame header. A decoder that
 *  verifies it, ffmpeg does, rejects the frame outright if this is wrong,
 *  so it is not optional however tempting a zero looks. */
inline uint8_t crc8(const uint8_t* data, size_t size) {
  uint8_t crc = 0;
  for (size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (int b = 0; b < 8; ++b) crc = (uint8_t) ((crc & 0x80) ? ((crc << 1) ^ 0x07) : (crc << 1));
  }
  return crc;
}

/** FLAC's CRC-16 (x^16 + x^15 + x^2 + 1) over the whole frame. */
inline uint16_t crc16(const uint8_t* data, size_t size) {
  uint16_t crc = 0;
  for (size_t i = 0; i < size; ++i) {
    crc ^= (uint16_t) ((uint16_t) data[i] << 8);
    for (int b = 0; b < 8; ++b)
      crc = (uint16_t) ((crc & 0x8000) ? ((crc << 1) ^ 0x8005) : (crc << 1));
  }
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
  const int dataBits = bytes * 6 - 6 + (7 - bytes);
  uint64_t lead = 0;
  for (int i = 0; i < bytes; ++i) lead = (lead << 1) | 1u;
  lead <<= 1; // the terminating zero
  writer.writeBits(lead, bytes + 1);
  writer.writeBits(value >> (6 * (bytes - 1)), dataBits);
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

/** The Rice parameter that spends the fewest bits on these residuals. */
inline int bestRiceParam(const std::vector<int64_t>& residuals) {
  if (residuals.empty()) return 0;
  uint64_t sum = 0;
  for (int64_t r : residuals) sum += (uint64_t) (r < 0 ? -r : r);
  const double mean = (double) sum / (double) residuals.size();
  int param = 0;
  while (param < 14 && (double) (1u << (param + 1)) < mean * 1.4) ++param;
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

  constexpr size_t kBlockSize = 4096; // code 0b1100, so no extra header bytes
  const double scale = bitDepth == 16 ? 32767.0 : 8388607.0;

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
  std::vector<int64_t> residuals, best;

  uint64_t frameNumber = 0;
  for (size_t start = 0; start < numFrames; start += kBlockSize) {
    const size_t count = (numFrames - start) < kBlockSize ? (numFrames - start) : kBlockSize;

    for (uint16_t c = 0; c < numChannels; ++c) {
      channelSamples[c].assign(count, 0);
      for (size_t i = 0; i < count; ++i) {
        double v = (double) interleaved[(start + i) * numChannels + c] * scale;
        if (v > scale) v = scale;
        if (v < -scale - 1.0) v = -scale - 1.0;
        channelSamples[c][i] = (int32_t) std::lround(v);
      }
    }

    BitWriter frame;
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

      int bestOrder = 0;
      uint64_t bestTotal = UINT64_MAX;
      for (int order = 0; order <= 4 && (size_t) order < count; ++order) {
        const uint64_t total = fixedResiduals(samples.data(), count, order, &residuals);
        if (total < bestTotal) {
          bestTotal = total;
          bestOrder = order;
          best = residuals;
        }
      }

      frame.writeBits(0, 1);                            // mandatory zero
      frame.writeBits((uint64_t) (8 + bestOrder), 6);   // FIXED, this order
      frame.writeBits(0, 1);                            // no wasted bits
      for (int i = 0; i < bestOrder; ++i) frame.writeSigned(samples[(size_t) i], bitDepth);

      frame.writeBits(0, 2); // residual method 0: 4-bit Rice parameters
      frame.writeBits(0, 4); // partition order 0: one partition
      const int param = bestRiceParam(best);
      frame.writeBits((uint64_t) param, 4);
      for (int64_t r : best) frame.writeRice((int32_t) r, param);
    }

    frame.alignToByte();
    const uint16_t frameCrc = crc16(frame.bytes().data(), frame.byteSize());
    out.insert(out.end(), frame.bytes().begin(), frame.bytes().end());
    out.push_back((uint8_t) (frameCrc >> 8));
    out.push_back((uint8_t) (frameCrc & 0xff));
  }

  std::FILE* f = std::fopen(path, "wb");
  if (!f) return false;
  const bool ok = std::fwrite(out.data(), 1, out.size(), f) == out.size();
  std::fclose(f);
  return ok;
}

} // namespace sonore
