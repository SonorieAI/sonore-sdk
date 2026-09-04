// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: the true-peak limiter.
//
// effects.h's Limiter guarantees its ceiling against SAMPLE peaks, and a
// DAC does not play samples: it plays the band-limited curve through them,
// which swings past the samples between two of them. A "-1 dBFS" master
// can leave the converter at +0.5 dBTP; streaming loudness specs are
// written in dBTP for exactly this reason. This limiter's detector is the
// inter-sample peak estimator of ITU-R BS.1770-4 Annex 2: the same 4x
// polyphase interpolation metering.h's TruePeakMeter implements (computed
// windowed sinc, phases normalised to unity DC), and the gain engine is
// the same textbook look-ahead machine the Limiter already proved: a
// sliding minimum over the window (O(1), monotonic deque) feeding a boxcar,
// so the gain ramps linearly and lands exactly as the peak arrives.
//
// Two honest details:
//   * The interpolating detector describes the signal EIGHT SAMPLES AGO
//     (its FIR is centred), so the signal path is delayed by lookahead + 8
//     and that whole figure is the reported latency. A peak's attenuation
//     still gets the full look-ahead window to ramp in.
//   * The 4x estimate can under-read the continuous peak by a fraction of a
//     dB (BS.1770 quotes the bound), and the base-rate gain ramp modulates
//     the signal slightly. Both are covered by a fixed 0.1 dB detection
//     margin, and the test file checks the result the only honest way: it
//     reconstructs the OUTPUT at 16x and measures the real overshoot.
//
// Included by dsp.h (after metering.h, whose TruePeakMeter is the detector).
#pragma once
#include <cmath>
#include <cstdint>
#include "audio.h"

namespace sonore {

class TruePeakLimiter {
public:
  void prepare(const ProcessSpec& spec) {
    sampleRate_ = (float) spec.sampleRate;
    setRelease(releaseMs_);
    setLookahead(lookaheadMs_);
  }

  void setCeiling(float db) {
    ceiling_ = dbToGain(db);
    detectorCeiling_ = ceiling_ * kMargin;
  }
  void setRelease(float ms) {
    releaseMs_ = ms;
    release_ = std::exp(-1.0f / (0.001f * (ms > 0.1f ? ms : 0.1f) * sampleRate_));
  }
  /** Rounded to whole samples; changing it clears the state (a window that
   *  changes length mid-stream would span the wrong samples). */
  void setLookahead(float ms) {
    lookaheadMs_ = ms;
    int n = (int) (ms * 0.001f * sampleRate_ + 0.5f);
    if (n < 1) n = 1;
    if (n > kMaxWindow - 1) n = kMaxWindow - 1;
    lookahead_ = n;
    reset();
  }
  void reset() {
    for (int c = 0; c < 2; ++c) {
      delay_[c].reset();
      tp_[c].reset();
    }
    for (int i = 0; i < kMaxWindow; ++i) {
      targets_[i] = 1.0f;
      box_[i] = 1.0f;
    }
    head_ = tail_ = 0;
    counter_ = 0;
    boxPos_ = 0;
    boxSum_ = (double) (lookahead_ + 1);
    gain_ = 1.0f;
  }
  /** Look-ahead plus the interpolator's centre: report it. */
  int latencySamples() const { return lookahead_ + kDetectorLag; }
  float gainReduction() const { return gainToDb(gain_); }

  inline void process(float& left, float& right) {
    // The detector: the louder of the sample peak (instant) and the 4x
    // inter-sample estimate (kDetectorLag samples behind, which the longer
    // signal delay absorbs). The margin covers the estimator's bound.
    const float sp = std::fabs(left) > std::fabs(right) ? std::fabs(left) : std::fabs(right);
    const float ta = tp_[0].process(left), tb = tp_[1].process(right);
    const float tpk = ta > tb ? ta : tb;
    float target = 1.0f;
    if (sp > ceiling_) target = ceiling_ / sp;
    if (tpk > detectorCeiling_) {
      const float t2 = detectorCeiling_ / tpk;
      if (t2 < target) target = t2;
    }
    const int window = lookahead_ + 1;

    // Sliding minimum over the last `window` targets (monotonic deque).
    const uint32_t now = counter_++;
    targets_[now % (uint32_t) kMaxWindow] = target;
    while (head_ != tail_) {
      const uint32_t back = deque_[(tail_ + kMaxWindow - 1) % kMaxWindow];
      if (targets_[back % (uint32_t) kMaxWindow] < target) break;
      tail_ = (tail_ + kMaxWindow - 1) % kMaxWindow;
    }
    deque_[tail_] = now;
    tail_ = (tail_ + 1) % kMaxWindow;
    while (head_ != tail_ && deque_[head_] + (uint32_t) window <= now) head_ = (head_ + 1) % kMaxWindow;
    const float minimum = targets_[deque_[head_] % (uint32_t) kMaxWindow];

    // Boxcar of the same length: the linear attack ramp.
    boxSum_ += (double) minimum - (double) box_[boxPos_];
    box_[boxPos_] = minimum;
    if (++boxPos_ >= window) boxPos_ = 0;
    const float smooth = (float) (boxSum_ / (double) window);

    // Release: down with the ramp, up exponentially.
    gain_ = smooth < gain_ ? smooth : smooth + release_ * (gain_ - smooth);
    if (gain_ > 1.0f) gain_ = 1.0f;

    const float d = (float) (lookahead_ + kDetectorLag);
    const float dl = delay_[0].tap(d, left);
    const float dr = delay_[1].tap(d, right);
    left = dl * gain_;
    right = dr * gain_;
  }

private:
  static constexpr int kMaxWindow = 4096;
  static constexpr int kDetectorLag = 8;       // the 16-tap interpolator's centre
  static constexpr float kMargin = 0.98855f;   // -0.1 dB on the detector ceiling

  DelayLine<8192> delay_[2];
  TruePeakMeter tp_[2];
  float targets_[kMaxWindow]{}, box_[kMaxWindow]{};
  uint32_t deque_[kMaxWindow]{};
  int head_ = 0, tail_ = 0, boxPos_ = 0, lookahead_ = 96;
  uint32_t counter_ = 0;
  double boxSum_ = 0.0;
  float sampleRate_ = 48000.0f, ceiling_ = 1.0f, detectorCeiling_ = 0.98855f;
  float releaseMs_ = 80.0f, lookaheadMs_ = 2.0f, release_ = 0.999f, gain_ = 1.0f;
};

} // namespace sonore
