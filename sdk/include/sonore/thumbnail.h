// SPDX-License-Identifier: Apache-2.0
//
// AudioThumbnail: the peak envelope a waveform display is drawn from.
//
// ── Why a waveform is not just "the samples, smaller" ───────────────────────
//
// A display is a few hundred pixels wide and a sample is a few million frames
// long, so every pixel stands for thousands of frames. Taking one of them --
// nearest-neighbour, which is what naive downsampling does -- shows whichever
// frame happened to land on the tick, and a snare hit one frame wide either
// appears at full height or vanishes entirely depending on where it fell.
//
// So each bucket keeps the MINIMUM and MAXIMUM of everything inside it. That
// is what makes a drawn waveform look like the sound: a transient is a tall
// thin spike because something in that bucket really was that loud, and
// silence is a flat line because nothing in it was.
//
// RMS is kept alongside, because peak alone makes quiet dense material and
// quiet sparse material look identical, and a page that wants to shade the
// body of the waveform differently from its outline needs both.
//
// ── Where it runs ───────────────────────────────────────────────────────────
//
// Never on the audio thread. This is a UI structure, built when a file loads
// and handed to the page as numbers; the audio thread never reads it.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace sonore {

/** One bucket: what the loudest and quietest samples in it were, and how much
 *  energy it carried. */
struct ThumbnailBucket {
  float low = 0.0f;  ///< The lowest sample in the bucket, in sample units (full scale is -1..1).
  float high = 0.0f; ///< The highest sample in the bucket.
  float rms = 0.0f;  ///< The root mean square of its samples: 0 for silence, 0.707 for a full-scale sine.
};

/**
 * A peak envelope over one or more channels.
 *
 * Built once per file. Rebuilding at a different width is cheap enough to do
 * on a window resize, but a page that redraws from the same buckets at a
 * different scale will look right too -- the envelope is the sound, not the
 * pixels.
 */
class AudioThumbnail {
public:
  /**
   * Reduce `numFrames` of interleaved-by-channel-pointer audio to `buckets`
   * buckets per channel.
   *
   * `buckets` is clamped to at least one and to no more than the number of
   * frames: asking for more buckets than there are samples would produce empty
   * ones, and an empty bucket drawn as a flat line in the middle of a waveform
   * is a gap that is not in the audio.
   */
  void build(const float* const* channels, uint32_t numChannels, uint64_t numFrames,
             uint32_t buckets) {
    channels_ = 0;
    buckets_ = 0;
    data_.clear();
    if (!channels || numChannels == 0 || numFrames == 0 || buckets == 0) return;

    if ((uint64_t) buckets > numFrames) buckets = (uint32_t) numFrames;
    channels_ = numChannels;
    buckets_ = buckets;
    data_.assign((size_t) numChannels * buckets, ThumbnailBucket{});

    for (uint32_t c = 0; c < numChannels; ++c) {
      const float* src = channels[c];
      if (!src) continue;
      for (uint32_t b = 0; b < buckets; ++b) {
        uint64_t from = 0, length = 0;
        spanOf(b, numFrames, buckets, from, length);
        float low = finiteSample(src[from]), high = low;
        double energy = 0.0;
        for (uint64_t i = from; i < from + length; ++i) {
          const float v = finiteSample(src[(size_t) i]);
          if (v < low) low = v;
          if (v > high) high = v;
          energy += (double) v * (double) v;
        }
        store(c, b, low, high, energy, length);
      }
    }
  }

  /**
   * The same buckets from INTERLEAVED audio -- a WavData's samples, which is
   * what every decoder here returns -- with no planar copy made first.
   *
   * build() wants a pointer per channel, so a caller holding a decoded file
   * pulled it apart into one vector per channel just to be told its shape: a
   * four-minute stereo song is 92 MB copied, 52 ms of the 74 it took to draw
   * the file's picture. The result is build()'s on those channels exactly:
   * every bucket takes the same samples in the same order, with the same
   * arithmetic. A stereo frame's two channels are taken together, in one
   * pass over the bucket.
   */
  void buildInterleaved(const float* interleaved, uint32_t numChannels, uint64_t numFrames,
                        uint32_t buckets) {
    channels_ = 0;
    buckets_ = 0;
    data_.clear();
    if (!interleaved || numChannels == 0 || numFrames == 0 || buckets == 0) return;
    if (numChannels == 1) { // one channel interleaved is one channel planar
      build(&interleaved, 1, numFrames, buckets);
      return;
    }

    if ((uint64_t) buckets > numFrames) buckets = (uint32_t) numFrames;
    channels_ = numChannels;
    buckets_ = buckets;
    data_.assign((size_t) numChannels * buckets, ThumbnailBucket{});

    const size_t stride = numChannels;
    for (uint32_t b = 0; b < buckets; ++b) {
      uint64_t from = 0, length = 0;
      spanOf(b, numFrames, buckets, from, length);
      const float* const first = interleaved + (size_t) from * stride;
      if (numChannels == 2) {
        float lowL = finiteSample(first[0]), highL = lowL;
        float lowR = finiteSample(first[1]), highR = lowR;
        double energyL = 0.0, energyR = 0.0;
        for (uint64_t i = 0; i < length; ++i) {
          const float l = finiteSample(first[2 * i]);
          const float r = finiteSample(first[2 * i + 1]);
          if (l < lowL) lowL = l;
          if (l > highL) highL = l;
          energyL += (double) l * (double) l;
          if (r < lowR) lowR = r;
          if (r > highR) highR = r;
          energyR += (double) r * (double) r;
        }
        store(0, b, lowL, highL, energyL, length);
        store(1, b, lowR, highR, energyR, length);
        continue;
      }
      for (uint32_t c = 0; c < numChannels; ++c) {
        float low = finiteSample(first[c]), high = low;
        double energy = 0.0;
        for (uint64_t i = 0; i < length; ++i) {
          const float v = finiteSample(first[(size_t) i * stride + c]);
          if (v < low) low = v;
          if (v > high) high = v;
          energy += (double) v * (double) v;
        }
        store(c, b, low, high, energy, length);
      }
    }
  }

  /** Channels it holds; 0 before a build. */
  uint32_t numChannels() const { return channels_; }
  /** Buckets per channel; 0 before a build. */
  uint32_t numBuckets() const { return buckets_; }
  /** True before a build, or after one of nothing. */
  bool empty() const { return data_.empty(); }

  /** One bucket. Out of range returns a silent one rather than reading past
   *  the end: a page asking for pixel 900 of an 800-bucket thumbnail is a
   *  rounding error, not a reason to crash. */
  ThumbnailBucket at(uint32_t channel, uint32_t bucket) const {
    if (channel >= channels_ || bucket >= buckets_) return ThumbnailBucket{};
    return data_[(size_t) channel * buckets_ + bucket];
  }

  /** The loudest sample anywhere in it, which is what a display normalises to
   *  when it wants the waveform to fill its box. */
  float peak() const {
    float p = 0.0f;
    for (const ThumbnailBucket& b : data_) {
      const float lo = b.low < 0.0f ? -b.low : b.low;
      const float hi = b.high < 0.0f ? -b.high : b.high;
      if (lo > p) p = lo;
      if (hi > p) p = hi;
    }
    return p;
  }

  /**
   * The envelope as JSON, for the page that draws it.
   *
   * Three flat arrays per channel rather than an array of objects: a few
   * hundred buckets becomes a few thousand key names otherwise, and the page
   * indexes them by position anyway.
   *
   * Values are rounded to four decimals. A waveform is drawn at screen
   * resolution and the extra digits are bytes crossing the bridge to be thrown
   * away by a rasteriser.
   */
  std::string toJson() const {
    std::string json = "{\"channels\":[";
    for (uint32_t c = 0; c < channels_; ++c) {
      if (c) json += ",";
      json += "{\"low\":[";
      appendRow(json, c, &ThumbnailBucket::low);
      json += "],\"high\":[";
      appendRow(json, c, &ThumbnailBucket::high);
      json += "],\"rms\":[";
      appendRow(json, c, &ThumbnailBucket::rms);
      json += "]}";
    }
    json += "],\"buckets\":" + std::to_string(buckets_);
    json += ",\"peak\":" + trimmed(peak()) + "}";
    return json;
  }

  /** What the buckets occupy, which is what a cache (thumbnail_cache.h)
   *  budgets by. */
  size_t memoryBytes() const { return data_.size() * sizeof(ThumbnailBucket); }

  /**
   * The envelope as bytes, for a cache on disk: "SNTB", a version, the
   * channel and bucket counts, then every bucket's low, high and rms as
   * little-endian float32. Exact -- a thumbnail loaded back draws the same
   * pixels -- and four times smaller than the JSON.
   */
  void saveTo(std::vector<uint8_t>& out) const {
    out.clear();
    out.reserve(16 + data_.size() * 12);
    const uint8_t magic[4] = {'S', 'N', 'T', 'B'};
    out.insert(out.end(), magic, magic + 4);
    putU32(out, 1);
    putU32(out, channels_);
    putU32(out, buckets_);
    for (const ThumbnailBucket& b : data_) {
      putFloat(out, b.low);
      putFloat(out, b.high);
      putFloat(out, b.rms);
    }
  }

  /**
   * Load what saveTo wrote. False, leaving this thumbnail as it was, for
   * anything else: a cache file is input from disk, and a truncated or
   * damaged one must cost a rebuild, not a waveform drawn from garbage. The
   * size must match the counts EXACTLY and every value must be finite.
   */
  bool loadFrom(const uint8_t* data, size_t size) {
    if (!data || size < 16 || std::memcmp(data, "SNTB", 4) != 0) return false;
    if (getU32(data + 4) != 1) return false;
    const uint32_t channels = getU32(data + 8), buckets = getU32(data + 12);
    // Bounded before multiplying, so a crafted count cannot wrap the size
    // check into agreeing with a small file.
    if (channels == 0 || buckets == 0 || channels > 1024 || buckets > (1u << 24)) return false;
    const uint64_t count = (uint64_t) channels * buckets;
    if (16 + count * 12 != (uint64_t) size) return false;
    std::vector<ThumbnailBucket> loaded((size_t) count);
    const uint8_t* p = data + 16;
    for (ThumbnailBucket& b : loaded) {
      b.low = getFloat(p);
      b.high = getFloat(p + 4);
      b.rms = getFloat(p + 8);
      p += 12;
      if (!std::isfinite(b.low) || !std::isfinite(b.high) || !std::isfinite(b.rms)) return false;
    }
    data_.swap(loaded);
    channels_ = channels;
    buckets_ = buckets;
    return true;
  }

  /** Same shape and every bucket equal, compared exactly: what a round trip
   *  through the cache is checked with. */
  bool operator==(const AudioThumbnail& other) const {
    if (channels_ != other.channels_ || buckets_ != other.buckets_) return false;
    for (size_t i = 0; i < data_.size(); ++i) {
      const ThumbnailBucket &a = data_[i], &b = other.data_[i];
      if (a.low != b.low || a.high != b.high || a.rms != b.rms) return false;
    }
    return true;
  }
  bool operator!=(const AudioThumbnail& other) const { return !(*this == other); } ///< Not ==.

private:
  /** Where bucket `b` starts and how many frames it holds. The span in
   *  64-bit arithmetic throughout: a 32-bit multiply of a bucket index by a
   *  frame count overflows on any file longer than a few minutes, and the
   *  failure is a bucket reading somebody else's samples -- which still
   *  looks like a waveform. */
  static void spanOf(uint32_t b, uint64_t numFrames, uint32_t buckets, uint64_t& from,
                     uint64_t& length) {
    from = (uint64_t) b * numFrames / buckets;
    uint64_t to = (uint64_t) (b + 1) * numFrames / buckets;
    if (to <= from) to = from + 1;
    if (to > numFrames) to = numFrames;
    length = to - from;
  }

  /** A bucket summed: its lowest and highest sample, and its rms from the
   *  energy of its `count` samples. */
  void store(uint32_t channel, uint32_t bucket, float low, float high, double energy, uint64_t count) {
    ThumbnailBucket& out = data_[(size_t) channel * buckets_ + bucket];
    out.low = low;
    out.high = high;
    out.rms = (float) std::sqrt(energy / (double) count);
  }

  /** One sample as a bucket may hold it: NaN as silence, an infinity as the
   *  largest float of its sign -- jsNumber's (gui.h) rule for the same
   *  values. A float file can hold either, and one of them kept as it was
   *  made its bucket's rms NaN or infinite: JSON no page parses, and bytes
   *  loadFrom refuses, so a cache saved with that one thumbnail in it would
   *  not load at all. Every bucket stays finite, whatever the file. */
  static float finiteSample(float v) {
    if (v != v) return 0.0f;
    const float largest = (std::numeric_limits<float>::max)();
    return v > largest ? largest : (v < -largest ? -largest : v);
  }

  static void putU32(std::vector<uint8_t>& out, uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back((uint8_t) ((v >> (8 * i)) & 0xffu));
  }
  static void putFloat(std::vector<uint8_t>& out, float v) {
    uint32_t bits;
    std::memcpy(&bits, &v, sizeof(bits));
    putU32(out, bits);
  }
  static uint32_t getU32(const uint8_t* p) {
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) |
           ((uint32_t) p[3] << 24);
  }
  static float getFloat(const uint8_t* p) {
    const uint32_t bits = getU32(p);
    float v;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
  }

  static std::string trimmed(float v) {
    // Room for the largest float in full, which %.4f writes out digit by
    // digit: 45 characters with its sign.
    char text[64];
    std::snprintf(text, sizeof(text), "%.4f", (double) v);
    // %f follows the C locale, and hosts do call setlocale(): under a German
    // one every value came out as "0,5000", which in a JSON array is two
    // numbers -- each row twice as long as the bucket count, every value
    // after the first misplaced, and a "peak" that made the whole object
    // unparseable. Only the decimal separator varies in %f's output, so it
    // is put back to the one JSON knows, as jsNumber (gui.h) does; that
    // header is far too heavy for this one to include.
    for (char* p = text; *p; ++p)
      if (*p == ',') *p = '.';
    // Trailing zeroes removed, because "0.5000" and "0.5" draw the same
    // waveform and one of them is half the bytes.
    std::string s(text);
    const size_t dot = s.find('.');
    if (dot != std::string::npos) {
      size_t end = s.size();
      while (end > dot + 2 && s[end - 1] == '0') --end;
      s.resize(end);
    }
    return s;
  }

  void appendRow(std::string& json, uint32_t channel, float ThumbnailBucket::*field) const {
    for (uint32_t b = 0; b < buckets_; ++b) {
      if (b) json += ",";
      json += trimmed(data_[(size_t) channel * buckets_ + b].*field);
    }
  }

  std::vector<ThumbnailBucket> data_;
  uint32_t channels_ = 0, buckets_ = 0;
};

} // namespace sonore
