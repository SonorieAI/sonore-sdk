// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: a crossover into any number of bands that still sums flat.
//
// effects.h's LinkwitzRiley splits two ways and its two halves sum to an
// allpass, which is the whole reason to use it. Stack two of them to get three
// bands and the property is GONE: the low band went through one crossover's
// phase shift, the top band through two, and where they meet they no longer
// add up -- a multiband compressor built that way has a dip at every
// crossover with the compression turned off.
//
// The fix is the standard one: every band that was split off EARLY passes
// through the allpass equivalent of every crossover that came AFTER it, so
// all the bands arrive with the same phase. An LR4's allpass is its low and
// high outputs added together, so the compensation is the same filter run
// again and summed -- (Bands-1)(Bands-2)/2 extra crossovers, three for four
// bands. The unit test sums the bands and asks for +/-0.15 dB from 20 Hz to
// 20 kHz.
//
// Included by dsp.h.
#pragma once
#include <cmath>
#include "audio.h"

namespace sonore {

template <int Bands = 3>
class MultibandSplitter {
  static_assert(Bands >= 2 && Bands <= 8, "2..8 bands");

public:
  static constexpr int kBands = Bands;
  static constexpr int kCrossovers = Bands - 1;

  MultibandSplitter() {
    // Log-spaced defaults between 100 Hz and 8 kHz, until setCrossover().
    for (int k = 0; k < kCrossovers; ++k) {
      const float t = kCrossovers > 1 ? (float) k / (float) (kCrossovers - 1) : 0.5f;
      hz_[k] = 100.0f * std::pow(80.0f, t);
    }
  }

  void setSampleRate(float sr) {
    sr_ = sr > 1.0f ? sr : 48000.0f;
    for (int k = 0; k < kCrossovers; ++k) {
      split_[k].setSampleRate(sr_);
      for (int j = 0; j < kCrossovers; ++j) comp_[k][j].setSampleRate(sr_);
    }
    for (int k = 0; k < kCrossovers; ++k) setCrossover(k, hz_[k]);
  }
  /** Crossover k, ascending: index 0 is the lowest split. */
  void setCrossover(int k, float hz) {
    if (k < 0 || k >= kCrossovers) return;
    hz_[k] = clampf(hz, 20.0f, sr_ * 0.45f);
    split_[k].setCrossover(hz_[k]);
    for (int j = 0; j < kCrossovers; ++j) comp_[k][j].setCrossover(hz_[k]);
  }
  void reset() {
    for (int k = 0; k < kCrossovers; ++k) {
      split_[k].reset();
      for (int j = 0; j < kCrossovers; ++j) comp_[k][j].reset();
    }
  }

  /** One sample into Bands outputs, lowest first. */
  inline void process(float x, float* bands) {
    float rest = x;
    for (int k = 0; k < kCrossovers; ++k) {
      float low, high;
      split_[k].process(rest, low, high);
      bands[k] = low;
      rest = high;
      // Every band already split off gets this crossover's phase shift too.
      for (int j = 0; j < k; ++j) {
        float l, h;
        comp_[k][j].process(bands[j], l, h);
        bands[j] = l + h;
      }
    }
    bands[Bands - 1] = rest;
  }

private:
  LinkwitzRiley split_[kCrossovers];
  LinkwitzRiley comp_[kCrossovers][kCrossovers]; // [crossover][earlier band]
  float hz_[kCrossovers]{};
  float sr_ = 48000.0f;
};

} // namespace sonore
