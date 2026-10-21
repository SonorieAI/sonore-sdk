// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: pitch: finding it, and moving it.
//
// PitchDetector is YIN (de Cheveigné & Kawahara, 2002): the difference
// function of a window against itself, normalised by its own running mean so
// the lag-zero trivial minimum disappears, the first dip under a threshold
// taken as the period, refined by a parabola through its neighbours. Chosen
// over plain autocorrelation because autocorrelation's octave errors are the
// classic tuner failure -- a rich waveform correlates almost as well with
// itself half a period later -- and the cumulative-mean normalisation is
// exactly the fix for that.
//
// PitchShifter is the two-head delay-line shifter: a read pointer that runs
// slower or faster than the write pointer changes the pitch, wraps
// periodically, and a second head half a window behind crossfades over the
// wrap so it is never heard. It is the design in every classic hardware
// harmoniser and it is what "shimmer" is made of. Grainy on a slow sweep and
// slightly comb-flavoured on a wide interval, like the hardware; a phase
// vocoder over stft.h is the cleaner and far costlier alternative.
//
// Included by dsp.h.
#pragma once
#ifndef SONORE_DSP_H_INCLUDED
#error "pitch.h is part of dsp.h and needs what it includes first: include <sonore/dsp.h> instead."
#endif
#include <cmath>
#include <cstddef>
#include "audio.h"

namespace sonore {

// ── Detection ────────────────────────────────────────────────────────────────

/**
 * Feed samples in; every hop a new estimate is ready.
 *
 * Window sets the lowest detectable pitch (a period must fit in half the
 * window: 2048 at 48 kHz reaches down to ~47 Hz) and the cost, which is the
 * thing to know about: each analysis is Window/2 differences for every lag
 * in range, so narrowing the range with setRange() is how a bass tuner and
 * a vocal tuner cost different amounts. At the default range this is about
 * a thousand multiply-adds per sample -- fine for a tuner or a monophonic
 * tracker, not something to run per voice.
 *
 * push() spreads that over the hop: each sample adds the terms of the
 * difference function it completes, so every call costs the same and the
 * estimate is ready on the same sample it always was. It used to do the
 * whole analysis in the one call that ended a hop -- 0.75 ms inside a single
 * audio block, every 1024 samples.
 */
template <int Window = 2048>
class PitchDetector {
  static_assert(Window >= 64 && (Window & (Window - 1)) == 0, "Window must be a power of two");

public:
  /** Half the window, in samples: the hop (a new estimate every kHalf
   *  push() calls) and the most lags the difference function holds. */
  static constexpr int kHalf = Window / 2;

  /** The default range's lags clamped into the window from the start. The
   *  members' 40 Hz floor at 48 kHz is a 1200-sample lag and a 2048 window
   *  holds 1022: used as constructed, the analysis summed past diff_. */
  PitchDetector() {
    setRange(minHz_, maxHz_);
    summedTo_ = maxLag_;
    stale_ = false;
  }

  /** The rate the pushed samples are at, in Hz; 1 or less means 48 kHz. The
   *  range's lags are worked out again from it, and the window may now see
   *  less low than the range asks (see lowestHz()). No allocation: the
   *  buffers are sized by Window. */
  void setSampleRate(float sr) {
    sr_ = sr > 1.0f ? sr : 48000.0f;
    setRange(minHz_, maxHz_);
  }
  /** Lags outside this band are never examined. */
  void setRange(float minHz, float maxHz) {
    minHz_ = minHz > 1.0f ? minHz : 1.0f;
    maxHz_ = maxHz > minHz_ ? maxHz : minHz_ * 2.0f;
    minLag_ = (int) (sr_ / maxHz_);
    maxLag_ = (int) (sr_ / minHz_);
    if (minLag_ < 2) minLag_ = 2;
    // BOTH lags are held inside the window. The first version clamped only
    // the top one, and then restored "max above min" by adding one to min --
    // so a range whose ceiling was a few hertz (min lag in the tens of
    // thousands) put max just past it and process() walked off the end of
    // diff_. Found by MSVC's analyser rather than by a test, which is why
    // there is a test for it now.
    if (minLag_ > kHalf - 3) minLag_ = kHalf - 3;
    // Whether the floor asked for is lower than the window can see: a 2048
    // window holds 1022 lags, which is 47 Hz at 48 kHz but 94 Hz at 96 kHz
    // and 188 Hz at 192 kHz.
    floorClamped_ = maxLag_ > kHalf - 2;
    if (maxLag_ > kHalf - 2) maxLag_ = kHalf - 2;
    if (maxLag_ <= minLag_) maxLag_ = minLag_ + 1;
    // The hop in progress summed its terms up to the old top lag. Called
    // again with the same range -- a host re-applying parameters every
    // block -- it changes nothing; changed, that one window is analysed
    // whole when it completes.
    if (maxLag_ != summedTo_) stale_ = true;
  }
  /** YIN's absolute threshold on the normalised difference: lower is stricter
   *  about what counts as periodic. 0.1-0.15 is the paper's range. */
  void setThreshold(float t) { threshold_ = clampf(t, 0.01f, 0.9f); }
  /** Forget the buffered input and the last estimate: frequency() and
   *  confidence() read 0 until the next hop completes. The rate, range and
   *  threshold are kept. No allocation. */
  void reset() {
    for (int i = 0; i < Window; ++i) buffer_[i] = 0.0f;
    for (int i = 0; i < kHalf; ++i) diff_[i] = 0.0f;
    write_ = 0;
    sinceHop_ = 0;
    summedTo_ = maxLag_;
    stale_ = false;
    frequency_ = 0.0f;
    confidence_ = 0.0f;
  }

  /** [audio thread] Returns true when a fresh estimate was just computed.
   *  Costs maxLag multiply-adds, every call. */
  inline bool push(float x) {
    constexpr int mask = Window - 1;
    buffer_[write_] = x;
    write_ = (write_ + 1) & mask;
    // The window this hop completes runs from `base`: this sample is its
    // index kHalf + m, and its first half is the previous hop, already here.
    // d(tau) = sum over j < kHalf of (x[j] - x[j + tau])^2, and each term
    // is added on the call that brings in the later of its two samples' --
    // spread so that call m adds the terms ending at index m (all of them in
    // the previous hop) and those ending at kHalf + m (on this sample).
    // Lags 1..m, then m+1..maxLag: maxLag terms per call, whatever m is.
    const int m = sinceHop_;
    if (m == 0) summedTo_ = maxLag_;
    const int base = (write_ - 1 - kHalf - m) & mask;
    const int top = summedTo_;
    const float early = buffer_[(base + m) & mask];
    const int earlyTop = m < top ? m : top;
    for (int tau = 1; tau <= earlyTop; ++tau) {
      const float e = buffer_[(base + m - tau) & mask] - early;
      diff_[tau] += e * e;
    }
    for (int tau = m + 1; tau <= top; ++tau) {
      const float e = buffer_[(base + kHalf + m - tau) & mask] - x;
      diff_[tau] += e * e;
    }
    if (++sinceHop_ < kHalf) return false;
    sinceHop_ = 0;
    if (stale_) {
      // The range moved mid-hop: this one window, whole, the old way.
      for (int i = 0; i < Window; ++i) frame_[i] = buffer_[(write_ + i) & mask];
      analyse(frame_, Window);
      stale_ = false;
    } else {
      decide();
    }
    for (int i = 0; i < kHalf; ++i) diff_[i] = 0.0f;
    return true;
  }

  /** Estimate from a caller's buffer of at least Window samples. Sets
   *  frequency() and confidence() the same way push() does. */
  void analyse(const float* x, size_t n) {
    if ((int) n < Window) { frequency_ = 0.0f; confidence_ = 0.0f; return; }
    // The difference function from lag 1, not from the bottom of the band.
    // The first draft started at minLag_ to save the lags below it, and the
    // missing-fundamental test caught it: the normalisation's running mean
    // then lacks the small values near lag 0, sits higher, and a strong
    // second harmonic's dip reads 0.14 instead of 0.17 -- under the
    // threshold, and the octave above is reported. The paper starts at 1
    // for a reason; the extra lags cost a few percent.
    for (int tau = 1; tau <= maxLag_; ++tau) {
      float d = 0.0f;
      for (int j = 0; j < kHalf; ++j) {
        const float e = x[j] - x[j + tau];
        d += e * e;
      }
      diff_[tau] = d;
    }
    decide();
  }

  /** Hz of the last estimate; 0 before the first, and 0 for a note below
   *  what the window can see at this rate (see lowestHz()). */
  float frequency() const { return frequency_; }
  /** The lowest pitch this window can detect at the current rate: the floor
   *  setRange() asked for, or higher when the window is too short for it. */
  float lowestHz() const { return sr_ / (float) maxLag_; }
  /** 1 - the normalised difference at the chosen lag: 1 is a pure period,
   *  under ~0.5 is noise. */
  float confidence() const { return confidence_; }

private:
  /** From a difference function in diff_[1..maxLag_] to an estimate. */
  void decide() {
    // Cumulative mean normalisation.
    float running = 0.0f;
    for (int tau = 1; tau <= maxLag_; ++tau) {
      running += diff_[tau];
      diff_[tau] = running > 1e-20f ? diff_[tau] * (float) tau / running : 1.0f;
    }
    // The FIRST dip under the threshold, followed down to its local minimum.
    // Not the global minimum: on a rich waveform the second harmonic's lag
    // can dip a hair lower than the fundamental's, and taking the deepest
    // reports the octave above. "First under the threshold" is YIN's whole
    // answer to the octave error.
    int best = -1;
    for (int tau = minLag_; tau <= maxLag_; ++tau) {
      if (diff_[tau] < threshold_) {
        best = tau;
        while (best + 1 <= maxLag_ && diff_[best + 1] < diff_[best]) ++best;
        break;
      }
    }
    if (best < 0) {
      // Nothing under the threshold: report the best there was, with the
      // confidence that says so, so a caller can decide what to do with it.
      best = minLag_;
      for (int tau = minLag_ + 1; tau <= maxLag_; ++tau)
        if (diff_[tau] < diff_[best]) best = tau;
    }
    // Parabolic refinement through the neighbours.
    float lag = (float) best;
    if (best > minLag_ && best < maxLag_) {
      const float a = diff_[best - 1], b = diff_[best], c = diff_[best + 1];
      const float denom = a - 2.0f * b + c;
      if (std::fabs(denom) > 1e-12f) lag += clampf(0.5f * (a - c) / denom, -1.0f, 1.0f);
    }
    // A best lag pinned at a CLAMPED top is no estimate: the period may well
    // be longer than the window can hold. It was reported as the floor
    // itself -- an 82 Hz guitar string read 93.93 Hz at 96 kHz and 187.87 Hz
    // at 192 kHz, and so did 110 Hz -- a confident-looking wrong number
    // where "below what I can see" was the truth.
    if (floorClamped_ && best >= maxLag_) {
      frequency_ = 0.0f;
      confidence_ = 0.0f;
      return;
    }
    frequency_ = sr_ / lag;
    confidence_ = clampf(1.0f - diff_[best], 0.0f, 1.0f);
  }

  float buffer_[Window]{};
  float frame_[Window]{};
  float diff_[kHalf]{};
  bool floorClamped_ = false;
  float sr_ = 48000.0f, minHz_ = 40.0f, maxHz_ = 2000.0f, threshold_ = 0.15f;
  int minLag_ = 24, maxLag_ = 1200, write_ = 0, sinceHop_ = 0;
  int summedTo_ = 1200; // the top lag the hop in progress is summing
  bool stale_ = false;  // the range changed since this hop began
  float frequency_ = 0.0f, confidence_ = 0.0f;
};

// ── Shifting ─────────────────────────────────────────────────────────────────

/**
 * Two read heads on one delay line, half a window apart, crossfaded with
 * equal-power sine/cosine gains so the level does not dip at the seams.
 *
 * The window is the grain size: longer is smoother in pitch and blurrier in
 * time, and 40-60 ms is where vocals and guitars sit. Latency is half of it.
 */
template <int MaxWindow = 8192>
class PitchShifter {
public:
  /** The rate in Hz (1 or less means 48 kHz). The window is worked out again
   *  from the milliseconds last asked for, so latencySamples() changes with
   *  the rate. No allocation: the delay line is MaxWindow samples. */
  void setSampleRate(float sr) {
    sr_ = sr > 1.0f ? sr : 48000.0f;
    // From what was ASKED for, not from what the last rate allowed: a visit
    // to 192 kHz clamped a 50 ms window to 42.6 and kept it, so back at 48 kHz
    // the shifter ran at the clamp's latency for good.
    setWindowMs(requestedMs_);
  }
  /** The grain length in ms, 50 by default: clamped to 5 ms up to what
   *  MaxWindow holds at this rate (8192 samples is about 170 ms at 48 kHz),
   *  and rounded to an even number of samples. The latency is half of it,
   *  so a plugin reports latencySamples() again after a change. */
  void setWindowMs(float ms) {
    requestedMs_ = ms;
    windowMs_ = clampf(ms, 5.0f, 1000.0f * (float) (MaxWindow - 8) / sr_);
    // An EVEN number of samples, so the half-window the unshifted head reads
    // at is a whole one and the latency below is exact rather than a delay
    // spread across two samples (50 ms at 44.1 kHz is 2205).
    window_ = 2.0f * std::round(windowMs_ * 0.001f * sr_ * 0.5f);
  }
  /** The shift in semitones, stored as the ratio 2^(semitones / 12) and so
   *  clamped to -24 .. +24. */
  void setSemitones(float semitones) { setRatio(std::pow(2.0f, semitones / 12.0f)); }
  /** 2 = up an octave, 0.5 = down one. */
  void setRatio(float ratio) { ratio_ = clampf(ratio, 0.25f, 4.0f); }
  /** Where an impulse comes out at no shift: half the window, plus the two
   *  samples the cubic read head sits behind the write point. Declared as
   *  half the window alone, a host aligning by it left the result two late. */
  int latencySamples() const { return (int) (window_ * 0.5f) + 2; }
  /** Clear the delay line and put the read heads back at the start. The
   *  window and the ratio are kept. */
  void reset() { line_.reset(); phase_ = 0.0f; }

  /** [audio-thread] One sample in, the shifted sample out. Two cubic reads
   *  and two fast sines per sample; no allocation, no lock. */
  inline float process(float x) {
    // The read point runs away from the write point at (1 - ratio) samples
    // per sample: up-shifting reads faster (delay shrinks), down-shifting
    // slower. Wrapped into the window, which is where the second head hides
    // the jump.
    phase_ += 1.0f - ratio_;
    while (phase_ < 0.0f) phase_ += window_;
    while (phase_ >= window_) phase_ -= window_;
    const float dA = phase_;
    float dB = phase_ + window_ * 0.5f;
    if (dB >= window_) dB -= window_;
    // Both heads sweep, so both must read cubic (DelayLine's rule).
    const float a = line_.readCubic(dA + 2.0f);
    const float b = line_.readCubic(dB + 2.0f);
    // Equal-power heads: sin over half a turn of the window. sinTurns is the
    // same curve to within 3e-7 and a fraction of std::sin's cost, which
    // this paid twice a sample.
    const float halfTurnsPerSample = 0.5f / window_;
    const float ga = fastmath::sinTurns(dA * halfTurnsPerSample);
    const float gb = fastmath::sinTurns(dB * halfTurnsPerSample);
    line_.write(x);
    return a * ga + b * gb;
  }

private:
  DelayLine<MaxWindow> line_;
  float sr_ = 48000.0f, windowMs_ = 50.0f, requestedMs_ = 50.0f, window_ = 2400.0f, ratio_ = 1.0f, phase_ = 0.0f;
};

} // namespace sonore
