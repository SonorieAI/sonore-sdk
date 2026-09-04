// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: pitch correction: detect, decide, shift.
//
// "Autotune" is three things this SDK already has, glued: YIN finds the
// pitch, a scale says where it should be, the phase vocoder moves it there.
// The glue is where it goes wrong -- a corrector that snaps on every frame
// warbles, one that trusts a low-confidence estimate on a breath jumps an
// octave, one that decides in hertz instead of cents snaps unevenly. So:
// decisions are made in cents against a twelve-bit scale mask, only when
// the detector is confident, and the correcting ratio glides to its target
// over a retune time -- zero is the robot, a hundred milliseconds is a
// singer.
//
// Latency is the vocoder's frame plus nothing: the detector's hop is what
// the correction lags the pitch by, which is the trade every real-time
// corrector makes.
//
// Included by dsp.h.
#pragma once
#include <cmath>
#include <cstdint>
#include "audio.h"

namespace sonore {

/**
 * SIZE: the vocoder and the detector together, ~140 KB. A member.
 */
class PitchCorrector {
public:
  static constexpr uint16_t kChromatic = 0x0FFF;
  /** Pitch-class masks from C = bit 0: a major scale rooted at `root`
   *  (0 = C). */
  static uint16_t majorScale(int root) { return rotate(0x0AB5, root); }
  static uint16_t minorScale(int root) { return rotate(0x05AD, root); }

  void setSampleRate(float sr) {
    sr_ = sr > 1.0f ? sr : 48000.0f;
    detector_.setSampleRate(sr_);
    detector_.setRange(minHz_, maxHz_);
    setRetune(retuneMs_);
  }
  void setScale(uint16_t mask) { mask_ = (mask & 0x0FFF) ? (mask & 0x0FFF) : kChromatic; }
  void setReference(float a4Hz) { a4_ = a4Hz > 100.0f ? a4Hz : 440.0f; }
  /** How fast the correction reaches its target: 0 snaps. */
  void setRetune(float ms) {
    retuneMs_ = ms < 0.0f ? 0.0f : ms;
    glide_ = retuneMs_ > 0.0f ? std::exp(-1.0f / (0.001f * retuneMs_ * sr_)) : 0.0f;
  }
  /** Pitches outside this are left alone. */
  void setRange(float minHz, float maxHz) {
    minHz_ = minHz;
    maxHz_ = maxHz;
    detector_.setRange(minHz, maxHz);
  }
  /** How much of the distance to the target is corrected: 1 is all of it. */
  void setAmount(float amount) { amount_ = clampf(amount, 0.0f, 1.0f); }
  void reset() {
    detector_.reset();
    vocoder_.reset();
    ratio_ = 1.0f;
    target_ = 1.0f;
    detected_ = 0.0f;
    targetHz_ = 0.0f;
  }
  static constexpr int latencySamples() { return PhaseVocoder<2048, 4>::latencySamples(); }

  inline float process(float x) {
    if (detector_.push(x)) decide();
    ratio_ = target_ + glide_ * (ratio_ - target_);
    vocoder_.setRatio(ratio_);
    return vocoder_.process(x);
  }

  float detectedHz() const { return detected_; }
  float targetHz() const { return targetHz_; }
  float ratio() const { return ratio_; }

private:
  static uint16_t rotate(uint16_t mask, int by) {
    by = ((by % 12) + 12) % 12;
    return (uint16_t) (((mask << by) | (mask >> (12 - by))) & 0x0FFF);
  }

  void decide() {
    const float hz = detector_.frequency();
    if (detector_.confidence() < 0.6f || hz < minHz_ || hz > maxHz_) {
      // Nothing to correct against: glide home.
      target_ = 1.0f;
      return;
    }
    detected_ = hz;
    // In cents from A4, to the nearest allowed pitch class.
    const float note = 69.0f + 12.0f * std::log2(hz / a4_);
    const int nearest = (int) std::floor(note + 0.5f);
    int best = nearest;
    float bestDist = 1e9f;
    for (int n = nearest - 6; n <= nearest + 6; ++n) {
      const int pc = ((n % 12) + 12) % 12;
      if (!(mask_ & (1u << pc))) continue;
      const float dist = std::fabs((float) n - note);
      if (dist < bestDist) { bestDist = dist; best = n; }
    }
    targetHz_ = a4_ * std::pow(2.0f, ((float) best - 69.0f) / 12.0f);
    const float full = targetHz_ / hz;
    target_ = std::pow(full, amount_);
  }

  PitchDetector<2048> detector_;
  PhaseVocoder<2048, 4> vocoder_;
  uint16_t mask_ = kChromatic;
  float sr_ = 48000.0f, a4_ = 440.0f, minHz_ = 60.0f, maxHz_ = 1500.0f, retuneMs_ = 40.0f, glide_ = 0.0f;
  float amount_ = 1.0f, ratio_ = 1.0f, target_ = 1.0f, detected_ = 0.0f, targetHz_ = 0.0f;
};

} // namespace sonore
