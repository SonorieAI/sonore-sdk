// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: the true-peak limiter.
//
// effects.h's Limiter guarantees its ceiling against SAMPLE peaks, and a
// DAC does not play samples: it plays the band-limited curve through them,
// which swings past the samples between two of them. A "-1 dBFS" master
// can leave the converter at +0.5 dBTP; streaming loudness specs are
// written in dBTP for exactly this reason. This limiter's detector is the
// inter-sample peak estimator of ITU-R BS.1770-4 Annex 2, the polyphase
// interpolation metering.h's TruePeakMeter implements (computed windowed
// sinc, phases normalised to unity DC), taken further than the meter's 4x:
// 8x with 64 taps a phase (see Detector); and the gain engine is
// the same textbook look-ahead machine the Limiter already proved: a
// sliding minimum over the window (O(1), monotonic deque) feeding a boxcar,
// so the gain ramps linearly and lands exactly as the peak arrives.
//
// Two honest details:
//   * The interpolating detector describes the signal 32 SAMPLES AGO (its
//     FIR is centred), so the signal path is delayed by lookahead + 32
//     and that whole figure is the reported latency. A peak's attenuation
//     still gets the full look-ahead window to ramp in.
//   * The 8x estimate can under-read the continuous peak by a fraction of a
//     dB (BS.1770 quotes the bound for 4x), and the base-rate gain ramp
//     modulates the signal slightly. Both are covered by a fixed 0.25 dB
//     detection margin, and the test file checks the result the only
//     honest way: it reconstructs the OUTPUT at 16x and measures the real
//     overshoot.
//
// Included by dsp.h, after metering.h (whose TruePeakMeter was the detector
// before the limiter got its own).
#pragma once
#ifndef SONORE_DSP_H_INCLUDED
#error "limiting.h is part of dsp.h and needs what it includes first: include <sonore/dsp.h> instead."
#endif
#include <cmath>
#include <cstdint>
#include "audio.h"

namespace sonore {

/**
 * A look-ahead limiter that holds its ceiling against TRUE peaks -- the
 * inter-sample peaks a converter plays -- rather than against sample values
 * (see the file comment for the gain engine, and Detector for the
 * estimator).
 *
 * Stereo and linked: one gain for both channels, so the image holds still.
 * Call prepare() before audio; it works out the release and the look-ahead
 * for the rate and clears the state. Then setCeiling() (dB, default 0),
 * setRelease() (ms, default 80) and setLookahead() (ms, default 2, rounded
 * to whole samples, at most 4094 of them) run on the audio thread with
 * process(): the same look-ahead again is free, a different one clears the
 * state. The signal comes out latencySamples() late, the look-ahead plus
 * the detector's 32-sample centre; report it to the host, and again when
 * the look-ahead changes. gainReduction() is in dB, negative while limiting.
 */
class TruePeakLimiter {
public:
  /** Take spec.sampleRate, work the release and the look-ahead out for it
   *  and clear the state. Nothing is allocated. latencySamples() can change
   *  here, with the rate. */
  void prepare(const ProcessSpec& spec) {
    sampleRate_ = (float) spec.sampleRate;
    setRelease(releaseMs_);
    setLookahead(lookaheadMs_);
    reset();
  }

  /** The ceiling in dBFS (true peak) the output is held under, 0 until set.
   *  The inter-sample estimate is held 0.25 dB under it, the margin the file
   *  comment explains. Not clamped: give it a finite number. A NaN is
   *  ignored and the ceiling stays where it was: taken as given, every peak
   *  compared false against it and the limiter stopped limiting. */
  void setCeiling(float db) {
    if (db != db) return;
    ceiling_ = dbToGain(db);
    detectorCeiling_ = ceiling_ * kMargin;
  }
  /** The gain's recovery after a peak, a time constant in ms (at least 0.1),
   *  at the rate prepare() set. The attack is always the look-ahead's
   *  linear ramp. */
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
    // A window of n + 1 targets in a ring of kMaxWindow slots that reads full
    // as empty: n + 1 must stay below kMaxWindow.
    if (n > kMaxWindow - 2) n = kMaxWindow - 2;
    // The same window: nothing to do. This cleared the state on EVERY call,
    // and a plugin applies its parameters every block: called per block at
    // the window it already had, the delay line was emptied each time and
    // 22% of a 512-sample block (95% of a 64-sample one) came out silent.
    if (n == lookahead_ && primed_) return;
    lookahead_ = n;
    reset();
  }
  /** Silence: the delay lines, the detectors and the window cleared, the
   *  gain back at unity. The settings stay. */
  void reset() {
    for (int c = 0; c < 2; ++c) {
      delay_[c].reset();
      tp_[c].reset();
    }
    for (int i = 0; i < kDetectorLag; ++i) samplePeaks_[i] = 0.0f;
    samplePeakPos_ = 0;
    for (int i = 0; i < kMaxWindow; ++i) {
      targets_[i] = 1.0f;
      box_[i] = 1.0f;
    }
    head_ = tail_ = 0;
    counter_ = 0;
    boxPos_ = 0;
    boxSum_ = (double) (lookahead_ + 1);
    gain_ = 1.0f;
    primed_ = true;
  }
  /** Look-ahead plus the interpolator's centre: report it. */
  int latencySamples() const { return lookahead_ + kDetectorLag; }
  /** The gain applied to the last frame, in dB: 0 at rest, negative while
   *  limiting. */
  float gainReduction() const { return gainToDb(gain_); }

  /** [audio-thread] One stereo frame, in place, latencySamples() late, both
   *  channels at the same gain. Allocation-free. */
  inline void process(float& left, float& right) {
    // The detector: the louder of the sample peak and the 8x
    // inter-sample estimate (kDetectorLag samples behind, which the longer
    // signal delay absorbs). The margin covers the estimator's bound.
    //
    // BOTH detectors refer to the same sample: the one kDetectorLag behind
    // the input, which is what the delay of lookahead + kDetectorLag emits
    // `lookahead` samples from now -- the span the window covers. The true
    // peak estimate is that late by construction; the sample peak was taken
    // from the sample arriving NOW, eight early, so its ramp released before
    // its sample came out: a 0 dBFS click through a -1 dB ceiling left at
    // -0.40 dBFS with a fast release. It is delayed to match.
    const float spNow = std::fabs(left) > std::fabs(right) ? std::fabs(left) : std::fabs(right);
    const float sp = samplePeaks_[samplePeakPos_];
    samplePeaks_[samplePeakPos_] = spNow;
    if (++samplePeakPos_ >= kDetectorLag) samplePeakPos_ = 0;
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
    // A 64-bit clock, and an age test that holds across a wrap. The clock
    // was 32 bits and the test `then + window <= now`: in the last `window`
    // samples before 2^32 -- every 24.9 hours at 48 kHz, 6.2 at 192 -- that
    // sum wrapped, the deque emptied, the minimum came from a stale slot,
    // and a +12 dBFS click went through a -1 dB ceiling at +12.
    const uint64_t now = counter_++;
    targets_[now % (uint64_t) kMaxWindow] = target;
    while (head_ != tail_) {
      const uint64_t back = deque_[(tail_ + kMaxWindow - 1) % kMaxWindow];
      if (targets_[back % (uint64_t) kMaxWindow] < target) break;
      tail_ = (tail_ + kMaxWindow - 1) % kMaxWindow;
    }
    deque_[tail_] = now;
    tail_ = (tail_ + 1) % kMaxWindow;
    while (head_ != tail_ && now - deque_[head_] >= (uint64_t) window) head_ = (head_ + 1) % kMaxWindow;
    const float minimum = targets_[deque_[head_] % (uint64_t) kMaxWindow];

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
  /** The inter-sample estimate the ceiling is held against: 8x, 64 taps a
   *  phase, Kaiser (beta 8). It was TruePeakMeter's 4x and 16 taps -- a
   *  meter's detector, and a limiter steering by one lets real peaks past:
   *  against a 32x reconstruction it read clipped program 1.0 dB low, and
   *  a -1 dBTP ceiling let through -0.09 dBTP. This reads the same program
   *  within 0.19 dB and tones to 19.5 kHz within 0.12 dB at 44.1 kHz, which
   *  the margin covers. Content at 20.5 kHz in a 44.1 kHz stream still reads
   *  0.36 dB low: the edge of what 64 taps can see that close to Nyquist. */
  struct Detector {
    static constexpr int kPhases = 8;      ///< 8x: the sample and seven points between
    static constexpr int kHalf = 32;       ///< half the filter, and the detector's lag in samples
    static constexpr int kTaps = 2 * kHalf; ///< taps per interpolating phase

    /** Builds the seven phases: windowed sinc (Kaiser, beta 8), each
     *  normalised to unity at DC. Runs once, when the limiter is made. */
    Detector() {
      auto i0 = [](double x) {
        double sum = 1.0, term = 1.0;
        for (int k = 1; k < 40; ++k) {
          term *= (x / (2.0 * k)) * (x / (2.0 * k));
          sum += term;
        }
        return sum;
      };
      const double beta = 8.0, i0beta = i0(beta), pi = 3.14159265358979323846;
      for (int ph = 1; ph < kPhases; ++ph) {
        const double frac = (double) ph / (double) kPhases;
        double sum = 0.0;
        for (int k = 0; k < kTaps; ++k) {
          // Tap k meets window[k]; the point lands kHalf - frac into the
          // window, between window[kHalf - 1] and window[kHalf], as the
          // meter's do.
          const double t = (double) (k - kHalf) + frac;
          const double sinc = std::fabs(t) < 1e-12 ? 1.0 : std::sin(pi * t) / (pi * t);
          const double r = t / (double) (kHalf + 1);
          const double w = std::fabs(r) >= 1.0 ? 0.0 : i0(beta * std::sqrt(1.0 - r * r)) / i0beta;
          taps[ph - 1][k] = (float) (sinc * w);
          sum += sinc * w;
        }
        // Unity at DC, phase by phase: a flat top must not read low.
        for (int k = 0; k < kTaps; ++k) taps[ph - 1][k] = (float) ((double) taps[ph - 1][k] / sum);
      }
    }
    /** Clear the history. */
    void reset() {
      for (float& h : history) h = 0.0f;
      write = 0;
    }
    /** Push `x`; the largest magnitude among the sample and the seven
     *  interpolated points of the stretch kHalf samples back. */
    inline float process(float x) {
      history[write] = x;
      history[write + kTaps] = x;
      const float* window = history + write + 1; // oldest first
      write = (write + 1) % kTaps;
      float peak = std::fabs(window[kHalf]);
      for (int ph = 0; ph < kPhases - 1; ++ph) {
        const float a = std::fabs(simd::dot(window, taps[ph], (size_t) kTaps));
        if (a > peak) peak = a;
      }
      return peak;
    }

    float taps[kPhases - 1][kTaps];  ///< phase 1/8 .. 7/8's filters
    float history[2 * kTaps] = {};   ///< the last kTaps inputs, written twice so a window is contiguous
    int write = 0;                   ///< the slot the next input goes to
  };

  static constexpr int kMaxWindow = 4096;
  static constexpr int kDetectorLag = Detector::kHalf; // the interpolator's centre
  static constexpr float kMargin = 0.97162795f;        // -0.25 dB on the detector ceiling

  DelayLine<8192> delay_[2];
  Detector tp_[2];
  float targets_[kMaxWindow]{}, box_[kMaxWindow]{};
  float samplePeaks_[kDetectorLag]{};
  int samplePeakPos_ = 0;
  uint64_t deque_[kMaxWindow]{};
  int head_ = 0, tail_ = 0, boxPos_ = 0, lookahead_ = 96;
  uint64_t counter_ = 0;
  double boxSum_ = 0.0;
  float sampleRate_ = 48000.0f, ceiling_ = 1.0f, detectorCeiling_ = 0.97162795f;
  float releaseMs_ = 80.0f, lookaheadMs_ = 2.0f, release_ = 0.999f, gain_ = 1.0f;
  bool primed_ = false; // reset() has run, so the state is one
};

} // namespace sonore
