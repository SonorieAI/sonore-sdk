// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: waveform peaks, for drawing.
//
// A sampler, a convolver or any plugin that loads a file has to SHOW it. In
// our architecture the drawing happens in the page, so what the C++ side owes
// the UI is not pixels but the summary a waveform view is made of: the minimum
// and maximum of each bucket of samples.
//
// Min/max per bucket rather than an average or a peak: an average flattens a
// waveform into a grey block, and a single peak per bucket draws a shape that
// is symmetric even when the material is not. Asymmetry is exactly what a
// producer is looking at when they check a kick drum's polarity.
//
// Offline: this walks the whole file, so it belongs in prepare() or in a
// message-thread handler, never in process().

#pragma once

#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>

#include <cstdint>
#include <cstring>

#include "audiostream.h"
#include "gui.h"

namespace sonore {

/** One bucket's vertical extent, in the sample range [-1, 1]. */
struct WaveformBucket {
  float minimum = 0.0f;
  float maximum = 0.0f;
};

/**
 * A downsampled outline of some audio, one row of buckets per channel.
 *
 * The count is what the VIEW asks for, not what the file suggests: a 3-minute
 * file drawn into 800 pixels needs 800 buckets whether it is 8 million samples
 * or 80.
 */
class WaveformPeaks {
public:
  /**
   * Where one bucket begins and ends, in frames.
   *
   * Shared by both builders, because they used to compute it separately and
   * disagreed. Spans come from the bucket INDEX -- so rounding cannot drift
   * and leave the last bucket short -- while the streaming pass originally
   * went the other way and derived the bucket from the frame. Those are not
   * inverses: index-first floors the boundaries, frame-first effectively
   * ceilings them, and they part company on every bucket count that does not
   * divide the frame count exactly. 9999 buckets of 70000 frames is where it
   * showed.
   */
  static void spanFor(size_t bucket, size_t numFrames, size_t numBuckets, size_t& begin,
                      size_t& end) {
    begin = (size_t) ((double) bucket * (double) numFrames / (double) numBuckets);
    end = (size_t) ((double) (bucket + 1) * (double) numFrames / (double) numBuckets);
    if (end <= begin) end = begin + 1; // more buckets than frames: never empty
    if (end > numFrames) end = numFrames;
    if (begin > numFrames) begin = numFrames;
  }

  void build(const float* interleaved, size_t numFrames, size_t numChannels,
             size_t numBuckets) {
    channels_ = numChannels > 0 ? numChannels : 1;
    buckets_ = numBuckets > 0 ? numBuckets : 1;
    data_.assign(channels_ * buckets_, WaveformBucket{});
    if (!interleaved || numFrames == 0) return;

    for (size_t bucket = 0; bucket < buckets_; ++bucket) {
      // Spans are computed from the bucket INDEX rather than accumulated, so
      // rounding cannot drift and leave the last bucket short or past the end.
      size_t begin = 0, end = 0;
      spanFor(bucket, numFrames, buckets_, begin, end);

      for (size_t c = 0; c < channels_; ++c) {
        float lo = interleaved[begin * channels_ + c];
        float hi = lo;
        for (size_t i = begin; i < end; ++i) {
          const float v = interleaved[i * channels_ + c];
          if (v < lo) lo = v;
          if (v > hi) hi = v;
        }
        data_[c * buckets_ + bucket] = {lo, hi};
      }
    }
  }

  /**
   * The same summary, from a file that is never loaded whole.
   *
   * build() needs the material in memory, which for a sample library is the
   * problem streaming exists to avoid: a host that has to hold two gigabytes
   * to draw a two-hundred-pixel waveform has not gained anything by
   * streaming the playback.
   *
   * Read in blocks, one pass, and the buckets are accumulated as the file
   * goes past. The bucket a frame belongs to is computed from its ABSOLUTE
   * position, exactly as build() computes its spans from the bucket index --
   * so the two produce the same numbers rather than nearly the same.
   *
   * Offline: this reads a whole file from disk. It belongs on a worker
   * thread, never in process().
   *
   * Returns false if the file cannot be read or is not one of the formats
   * that can be streamed; a caller with an MP3 should decode it and use
   * build().
   */
  bool buildFromFile(const char* path, size_t numBuckets) {
    AudioFileReader reader;
    if (!reader.open(path)) return false;
    const uint64_t frames = reader.numFrames();
    const size_t channels = reader.numChannels();
    if (frames == 0 || channels == 0) return false;

    channels_ = channels;
    buckets_ = numBuckets > 0 ? numBuckets : 1;
    data_.assign(channels_ * buckets_, WaveformBucket{});
    // Nothing has been seen yet, so "the smallest so far" cannot start at
    // zero -- a file that never goes below 0.2 would report a minimum of 0
    // and draw a waveform sitting on a floor it never touches.
    std::vector<bool> started(channels_ * buckets_, false);

    // Precomputed, so the streaming loop is a walk rather than arithmetic per
    // frame per bucket.
    std::vector<uint64_t> spanBegin(buckets_), spanEnd(buckets_);
    for (size_t b = 0; b < buckets_; ++b) {
      size_t begin = 0, end = 0;
      spanFor(b, (size_t) frames, buckets_, begin, end);
      spanBegin[b] = begin;
      spanEnd[b] = end;
    }
    size_t cursor = 0;

    constexpr size_t kBlock = 8192;
    std::vector<float> block(kBlock * channels_);
    uint64_t position = 0;
    while (position < frames) {
      const size_t want = (frames - position) < kBlock ? (size_t) (frames - position) : kBlock;
      const size_t got = reader.read(position, block.data(), want);
      if (got == 0) break;

      for (size_t i = 0; i < got; ++i) {
        const uint64_t at = position + i;
        // Walk the spans forward with the stream rather than computing a
        // bucket from the frame. The inner loop is not decoration: with more
        // buckets than frames every span is a single frame and SEVERAL
        // buckets share it, which a one-bucket-per-frame pass would leave
        // empty.
        while (cursor < buckets_ && spanEnd[cursor] <= at) ++cursor;
        for (size_t b = cursor; b < buckets_ && spanBegin[b] <= at; ++b) {
          if (at >= spanEnd[b]) continue;
          for (size_t c = 0; c < channels_; ++c) {
            const float v = block[i * channels_ + c];
            const size_t slot = c * buckets_ + b;
            if (!started[slot]) {
              data_[slot] = {v, v};
              started[slot] = true;
            } else {
              if (v < data_[slot].minimum) data_[slot].minimum = v;
              if (v > data_[slot].maximum) data_[slot].maximum = v;
            }
          }
        }
      }
      position += got;
    }
    return position > 0;
  }

  /**
   * The peaks as bytes, so they can be cached rather than recomputed.
   *
   * Building them means reading the whole file, which for a long one is
   * seconds a user waits every time a session opens. A host that keeps these
   * beside the plugin cache pays that once.
   *
   * Little-endian and versioned, like every other blob this SDK writes.
   */
  void serialise(std::vector<uint8_t>& out) const {
    out.clear();
    const char* magic = "SNWF";
    out.insert(out.end(), magic, magic + 4);
    auto put32 = [&out](uint32_t v) {
      out.push_back((uint8_t) (v & 0xff));
      out.push_back((uint8_t) ((v >> 8) & 0xff));
      out.push_back((uint8_t) ((v >> 16) & 0xff));
      out.push_back((uint8_t) ((v >> 24) & 0xff));
    };
    put32(1); // version
    put32((uint32_t) channels_);
    put32((uint32_t) buckets_);
    for (const WaveformBucket& b : data_) {
      uint32_t bits = 0;
      std::memcpy(&bits, &b.minimum, 4);
      put32(bits);
      std::memcpy(&bits, &b.maximum, 4);
      put32(bits);
    }
  }

  /** Read back what serialise wrote. Refuses anything whose declared size
   *  does not match the bytes actually present, rather than reading past the
   *  end of a truncated cache file. */
  bool deserialise(const uint8_t* data, size_t size) {
    if (!data || size < 16) return false;
    if (std::memcmp(data, "SNWF", 4) != 0) return false;
    auto get32 = [data](size_t at) {
      return (uint32_t) data[at] | ((uint32_t) data[at + 1] << 8) |
             ((uint32_t) data[at + 2] << 16) | ((uint32_t) data[at + 3] << 24);
    };
    if (get32(4) != 1) return false; // a version this build does not know
    const uint32_t channels = get32(8);
    const uint32_t buckets = get32(12);
    if (channels == 0 || buckets == 0) return false;
    // Checked BEFORE allocating: a header claiming four billion buckets is a
    // corrupt file, not a reason to ask for sixty-four gigabytes.
    const uint64_t needed = (uint64_t) channels * buckets * 8 + 16;
    if (needed != (uint64_t) size) return false;

    channels_ = channels;
    buckets_ = buckets;
    data_.assign((size_t) channels * buckets, WaveformBucket{});
    for (size_t i = 0; i < data_.size(); ++i) {
      const uint32_t lo = get32(16 + i * 8);
      const uint32_t hi = get32(16 + i * 8 + 4);
      std::memcpy(&data_[i].minimum, &lo, 4);
      std::memcpy(&data_[i].maximum, &hi, 4);
    }
    return true;
  }

  size_t numChannels() const { return channels_; }
  size_t numBuckets() const { return buckets_; }

  const WaveformBucket& bucket(size_t channel, size_t index) const {
    static const WaveformBucket empty{};
    if (channel >= channels_ || index >= buckets_) return empty;
    return data_[channel * buckets_ + index];
  }

  /** The loudest excursion anywhere, for scaling a view to fill its height. */
  float magnitude() const {
    float peak = 0.0f;
    for (const WaveformBucket& b : data_) {
      const float lo = b.minimum < 0.0f ? -b.minimum : b.minimum;
      const float hi = b.maximum < 0.0f ? -b.maximum : b.maximum;
      if (lo > peak) peak = lo;
      if (hi > peak) peak = hi;
    }
    return peak;
  }

  /**
   * The peaks as JSON, ready for the page.
   *
   * Numbers go through jsNumber, which is the whole reason this helper exists
   * rather than a snprintf loop: a host that has called setlocale to a comma
   * locale would otherwise emit "0,5" and hand the webview a syntax error:
   * the same trap the parameter bridge already learned.
   */
  std::string toJson() const {
    std::string json = "{\"channels\":";
    char number[48];
    jsNumber(number, sizeof(number), (double) channels_);
    json += number;
    jsNumber(number, sizeof(number), (double) buckets_);
    json += ",\"buckets\":";
    json += number;
    json += ",\"peaks\":[";
    for (size_t c = 0; c < channels_; ++c) {
      if (c > 0) json += ',';
      json += '[';
      for (size_t i = 0; i < buckets_; ++i) {
        if (i > 0) json += ',';
        const WaveformBucket& b = data_[c * buckets_ + i];
        jsNumber(number, sizeof(number), (double) b.minimum);
        json += number;
        json += ',';
        jsNumber(number, sizeof(number), (double) b.maximum);
        json += number;
      }
      json += ']';
    }
    json += "]}";
    return json;
  }

private:
  std::vector<WaveformBucket> data_;
  size_t channels_ = 0, buckets_ = 0;
};

} // namespace sonore
