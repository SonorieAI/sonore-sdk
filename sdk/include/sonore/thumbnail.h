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
#include <string>
#include <vector>

namespace sonore {

/** One bucket: what the loudest and quietest samples in it were, and how much
 *  energy it carried. */
struct ThumbnailBucket {
  float low = 0.0f;
  float high = 0.0f;
  float rms = 0.0f;
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
        // The span in 64-bit arithmetic throughout. A 32-bit multiply of a
        // bucket index by a frame count overflows on any file longer than a
        // few minutes, and the failure is a bucket reading somebody else's
        // samples -- which still looks like a waveform.
        const uint64_t from = (uint64_t) b * numFrames / buckets;
        uint64_t to = (uint64_t) (b + 1) * numFrames / buckets;
        if (to <= from) to = from + 1;
        if (to > numFrames) to = numFrames;

        float low = src[from], high = src[from];
        double energy = 0.0;
        for (uint64_t i = from; i < to; ++i) {
          const float v = src[(size_t) i];
          if (v < low) low = v;
          if (v > high) high = v;
          energy += (double) v * (double) v;
        }
        ThumbnailBucket& out = data_[(size_t) c * buckets + b];
        out.low = low;
        out.high = high;
        out.rms = (float) std::sqrt(energy / (double) (to - from));
      }
    }
  }

  uint32_t numChannels() const { return channels_; }
  uint32_t numBuckets() const { return buckets_; }
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

private:
  static std::string trimmed(float v) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.4f", (double) v);
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
