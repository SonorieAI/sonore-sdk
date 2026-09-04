// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: a Pultec-style passive program equaliser.
//
// WHAT THIS IS, stated honestly: the CONTROL TOPOLOGY and curve family of
// the classic passive program EQ (EQP-1A pattern) -- independent boost and
// cut at one selected low frequency, a broad bell up top with a bandwidth
// control, a separate high shelf cut -- with the curves built from this
// SDK's matched biquads. It is NOT a component-level simulation of any
// specific unit: that would need the unit's schematic values through
// circuit.h. What it keeps is the behaviour that makes the topology worth
// having, each part documented in published measurements of the originals:
//
//   * BOOST AND CUT AT ONCE (the "Pultec trick"): the cut network's corner
//     sits about two octaves above the boost's and is shallower when the
//     boost loads it, so full boost + full cut leaves a net low shelf, a
//     dip in the low mids just above it, and a flat top. Neither knob alone
//     can make that shape.
//   * The boost is a second-order shelf into its corner; the cut is first
//     order. Different orders are WHY their sum is not zero.
//   * The high boost is a bell whose gain grows as its bandwidth narrows
//     (sharp reaches +18 dB, broad +10), which is how the original's
//     coupled bandwidth/boost network behaved.
//   * Frequencies snap to the front-panel positions (20/30/60/100 low;
//     3/4/5/8/10/12/16 k bell; 5/10/20 k atten), because "60 Hz" on this
//     kind of EQ is a switch, not a pot.
//
// Coefficients are recomputed on the setter, not per sample: a program
// EQ's switches are set, not swept. Smooth outside if you automate it.
//
// Included by dsp.h.
#pragma once
#include <cmath>
#include "audio.h"

namespace sonore {

class PassiveEq {
public:
  void setSampleRate(float sr) {
    sr_ = sr > 1.0f ? sr : 48000.0f;
    for (Biquad* f : {&lowBoost_, &lowCut_, &highBoost_, &highCut_}) f->setSampleRate(sr_);
    update();
  }
  /** Low section frequency: snaps to 20 / 30 / 60 / 100 Hz. */
  void setLowFrequency(float hz) { lowHz_ = snap(hz, kLowHz, 4); update(); }
  /** Low boost, 0..1 (full is +13.5 dB). */
  void setLowBoost(float amount) { lowBoost01_ = clampf(amount, 0.0f, 1.0f); update(); }
  /** Low cut, 0..1 (full is -17.5 dB alone; less when the boost loads it). */
  void setLowCut(float amount) { lowCut01_ = clampf(amount, 0.0f, 1.0f); update(); }
  /** Bell frequency: snaps to 3 / 4 / 5 / 8 / 10 / 12 / 16 kHz. */
  void setHighFrequency(float hz) { highHz_ = snap(hz, kHighHz, 7); update(); }
  /** High boost, 0..1. */
  void setHighBoost(float amount) { highBoost01_ = clampf(amount, 0.0f, 1.0f); update(); }
  /** 0 broad .. 1 sharp. Sharp boosts further, as the original's did. */
  void setBandwidth(float sharp) { sharp_ = clampf(sharp, 0.0f, 1.0f); update(); }
  /** Atten shelf frequency: snaps to 5 / 10 / 20 kHz. */
  void setHighCutFrequency(float hz) { attenHz_ = snap(hz, kAttenHz, 3); update(); }
  /** High cut, 0..1 (full is -16 dB). */
  void setHighCut(float amount) { highCut01_ = clampf(amount, 0.0f, 1.0f); update(); }

  void reset() {
    for (Biquad* f : {&lowBoost_, &lowCut_, &highBoost_, &highCut_}) f->reset();
  }

  inline float process(float x) {
    x = lowBoost_.process(x);
    x = lowCut_.process(x);
    x = highBoost_.process(x);
    return highCut_.process(x);
  }

private:
  static constexpr float kLowHz[4] = {20.0f, 30.0f, 60.0f, 100.0f};
  static constexpr float kHighHz[7] = {3000.0f, 4000.0f, 5000.0f, 8000.0f, 10000.0f, 12000.0f, 16000.0f};
  static constexpr float kAttenHz[3] = {5000.0f, 10000.0f, 20000.0f};

  static float snap(float hz, const float* table, int n) {
    float best = table[0];
    for (int i = 1; i < n; ++i)
      if (std::fabs(std::log(hz / table[i])) < std::fabs(std::log(hz / best))) best = table[i];
    return best;
  }

  void update() {
    // Low boost: a second-order shelf, corner a third of an octave above
    // the dial (the originals read low), gently resonant.
    const float boostDb = 13.5f * lowBoost01_;
    designShelf(lowBoost_, eqdetail::Shape::LowShelf, lowHz_ * 1.3f, 0.6f, boostDb);
    // Low cut: FIRST order, two octaves up, and shallower as the boost
    // loads the network -- the interaction that makes the trick.
    const float cutDb = -17.5f * lowCut01_ * (1.0f - 0.55f * lowBoost01_);
    firstOrderLowShelf(lowCut_, lowHz_ * 4.0f, cutDb);
    // High bell: sharp narrows AND boosts further.
    const float q = 0.35f * std::pow(2.2f / 0.35f, sharp_);
    const float bellDb = (10.0f + 8.0f * sharp_) * highBoost01_;
    designShelf(highBoost_, eqdetail::Shape::Bell, highHz_, q, bellDb);
    // High atten: a first-order shelf hinging below its dial frequency.
    firstOrderHighShelf(highCut_, attenHz_ * 0.7f, -16.0f * highCut01_);
  }

  void designShelf(Biquad& f, eqdetail::Shape shape, float hz, float q, float gainDb) {
    float b0, b1, b2, a1, a2;
    if (std::fabs(gainDb) < 0.01f || hz >= sr_ * 0.47f) { f.setCoefficients(1.0f, 0.0f, 0.0f, 0.0f, 0.0f); return; }
    if (eqdetail::matched(shape, hz, q, gainDb, sr_, &b0, &b1, &b2, &a1, &a2))
      f.setCoefficients(b0, b1, b2, a1, a2);
    else
      f.setCoefficients(1.0f, 0.0f, 0.0f, 0.0f, 0.0f);
  }
  /** H(s) = (s + w*sqrt(G)) / (s + w/sqrt(G)): LF gain G, HF unity. */
  void firstOrderLowShelf(Biquad& f, float hz, float gainDb) {
    if (std::fabs(gainDb) < 0.01f) { f.setCoefficients(1.0f, 0.0f, 0.0f, 0.0f, 0.0f); return; }
    const float g = std::sqrt(dbToGain(gainDb));
    const float w = 2.0f * kPi * clampf(hz, 1.0f, sr_ * 0.45f);
    const float c = 2.0f * sr_;
    const float b0 = c + w * g, b1 = -c + w * g;
    const float a0 = c + w / g, a1 = -c + w / g;
    f.setCoefficients(b0 / a0, b1 / a0, 0.0f, a1 / a0, 0.0f);
  }
  /** H(s) = (G s + w) / (s + w): HF gain G, LF unity. */
  void firstOrderHighShelf(Biquad& f, float hz, float gainDb) {
    if (std::fabs(gainDb) < 0.01f) { f.setCoefficients(1.0f, 0.0f, 0.0f, 0.0f, 0.0f); return; }
    const float g = dbToGain(gainDb);
    const float w = 2.0f * kPi * clampf(hz, 1.0f, sr_ * 0.45f);
    const float c = 2.0f * sr_;
    const float b0 = g * c + w, b1 = -g * c + w;
    const float a0 = c + w, a1 = -c + w;
    f.setCoefficients(b0 / a0, b1 / a0, 0.0f, a1 / a0, 0.0f);
  }

  Biquad lowBoost_, lowCut_, highBoost_, highCut_;
  float sr_ = 48000.0f;
  float lowHz_ = 60.0f, lowBoost01_ = 0.0f, lowCut01_ = 0.0f;
  float highHz_ = 8000.0f, highBoost01_ = 0.0f, sharp_ = 0.5f;
  float attenHz_ = 10000.0f, highCut01_ = 0.0f;
};

} // namespace sonore
