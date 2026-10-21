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
// all the bands arrive with the same phase. An LR4's low plus high is
// exactly a second-order allpass at Q = 1/sqrt(2) -- (s^2 - sqrt2 s + 1) over
// (s^2 + sqrt2 s + 1), and the bilinear transform keeps the identity because
// every section shares one prewarp -- so each compensation is ONE biquad,
// where it used to be the crossover run again (four) and summed:
// (Bands-1)(Bands-2)/2 of them, three for four bands. The unit test sums the
// bands and asks for +/-0.15 dB from 20 Hz to 20 kHz, and checks the allpass
// against the crossover-and-sum it replaced, sample for sample.
//
// Included by dsp.h.
#pragma once
#ifndef SONORE_DSP_H_INCLUDED
#error "crossover.h is part of dsp.h and needs what it includes first: include <sonore/dsp.h> instead."
#endif
#include <cmath>
#include "audio.h"

namespace sonore {

/**
 * A Linkwitz-Riley crossover into `Bands` bands (2 to 8) that sum flat:
 * every band split off early passes through an allpass for each later
 * crossover, so all of them arrive in phase (see the file comment).
 *
 * One channel, per sample: two of these for stereo. Call setSampleRate() in
 * prepare(). The crossovers start log-spaced between 100 Hz and 8 kHz;
 * setCrossover(k, hz) takes index 0 as the lowest split and clamps to 20 Hz
 * up to 0.45 of the rate, but does not sort, so keep the frequencies
 * ascending. Setters and process() run on the audio thread, and a crossover
 * may move while audio runs: the sections are state-variable, as
 * LinkwitzRiley's are.
 */
template <int Bands = 3>
class MultibandSplitter {
  static_assert(Bands >= 2 && Bands <= 8, "2..8 bands");

public:
  /** The bands process() writes: Bands. */
  static constexpr int kBands = Bands;
  /** The split frequencies between them: Bands - 1. */
  static constexpr int kCrossovers = Bands - 1;

  /** The crossovers log-spaced from 100 Hz to 8 kHz. No filter is designed
   *  until setSampleRate(), which must come before process(). */
  MultibandSplitter() {
    // Log-spaced defaults between 100 Hz and 8 kHz, until setCrossover().
    for (int k = 0; k < kCrossovers; ++k) {
      const float t = kCrossovers > 1 ? (float) k / (float) (kCrossovers - 1) : 0.5f;
      hz_[k] = 100.0f * std::pow(80.0f, t);
    }
  }

  /** The rate in Hz (1 or less means 48 kHz). Every crossover is designed
   *  again at its frequency, clamped afresh to 0.45 of the new rate. The
   *  filters' state is kept; no allocation. */
  void setSampleRate(float sr) {
    sr_ = sr > 1.0f ? sr : 48000.0f;
    for (int k = 0; k < kCrossovers; ++k) {
      split_[k].setSampleRate(sr_);
      for (int j = 0; j < kCrossovers; ++j) comp_[k][j].setSampleRate(sr_);  // SvfSection
    }
    for (int k = 0; k < kCrossovers; ++k) setCrossover(k, hz_[k]);
  }
  /** Crossover k, ascending: index 0 is the lowest split. The frequency as
   *  asked is kept and the rate's ceiling applied to what is designed, so a
   *  later setSampleRate() designs it afresh. The clamped value was kept, and
   *  one visit to a low rate lowered a high crossover for good: 8 kHz seen at
   *  11.025 kHz came back to 48 kHz at 4.96 kHz. */
  void setCrossover(int k, float hz) {
    if (k < 0 || k >= kCrossovers) return;
    hz_[k] = hz;
    const float f = clampf(hz, 20.0f, sr_ * 0.45f);
    split_[k].setCrossover(f);
    for (int j = 0; j < kCrossovers; ++j) comp_[k][j].allpass(f, 0.70710678);
  }
  /** Clear every filter's state; the crossovers are kept. No allocation. */
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
      for (int j = 0; j < k; ++j) bands[j] = comp_[k][j].process(bands[j]);
    }
    bands[Bands - 1] = rest;
  }

private:
  LinkwitzRiley split_[kCrossovers];
  // [crossover][earlier band]: its allpass. Double and state-variable, like
  // the splits, so it follows a moving crossover as they do: see
  // LinkwitzRiley.
  SvfSection comp_[kCrossovers][kCrossovers];
  float hz_[kCrossovers]{};
  float sr_ = 48000.0f;
};

} // namespace sonore
