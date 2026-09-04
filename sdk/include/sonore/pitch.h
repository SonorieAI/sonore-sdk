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
 * a vocal tuner cost different amounts. At the default range this is a few
 * hundred multiplies per sample amortised -- fine for a tuner or a monophonic
 * tracker, not something to run per voice.
 */
template <int Window = 2048>
class PitchDetector {
  static_assert(Window >= 64 && (Window & (Window - 1)) == 0, "Window must be a power of two");

public:
  static constexpr int kHalf = Window / 2;

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
    if (maxLag_ > kHalf - 2) maxLag_ = kHalf - 2;
    if (maxLag_ <= minLag_) maxLag_ = minLag_ + 1;
  }
  /** YIN's absolute threshold on the normalised difference: lower is stricter
   *  about what counts as periodic. 0.1-0.15 is the paper's range. */
  void setThreshold(float t) { threshold_ = clampf(t, 0.01f, 0.9f); }
  void reset() {
    for (int i = 0; i < Window; ++i) buffer_[i] = 0.0f;
    write_ = 0;
    sinceHop_ = 0;
    frequency_ = 0.0f;
    confidence_ = 0.0f;
  }

  /** [audio thread] Returns true when a fresh estimate was just computed. */
  inline bool push(float x) {
    buffer_[write_] = x;
    write_ = (write_ + 1) % Window;
    if (++sinceHop_ < kHalf) return false;
    sinceHop_ = 0;
    // The last Window samples, oldest first, into a contiguous frame.
    for (int i = 0; i < Window; ++i) frame_[i] = buffer_[(write_ + i) % Window];
    analyse(frame_, Window);
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
    frequency_ = sr_ / lag;
    confidence_ = clampf(1.0f - diff_[best], 0.0f, 1.0f);
  }

  /** Hz of the last estimate; 0 before the first. */
  float frequency() const { return frequency_; }
  /** 1 - the normalised difference at the chosen lag: 1 is a pure period,
   *  under ~0.5 is noise. */
  float confidence() const { return confidence_; }

private:
  float buffer_[Window]{};
  float frame_[Window]{};
  float diff_[kHalf]{};
  float sr_ = 48000.0f, minHz_ = 40.0f, maxHz_ = 2000.0f, threshold_ = 0.15f;
  int minLag_ = 24, maxLag_ = 1200, write_ = 0, sinceHop_ = 0;
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
  void setSampleRate(float sr) {
    sr_ = sr > 1.0f ? sr : 48000.0f;
    setWindowMs(windowMs_);
  }
  void setWindowMs(float ms) {
    windowMs_ = clampf(ms, 5.0f, 1000.0f * (float) (MaxWindow - 8) / sr_);
    window_ = windowMs_ * 0.001f * sr_;
  }
  void setSemitones(float semitones) { setRatio(std::pow(2.0f, semitones / 12.0f)); }
  /** 2 = up an octave, 0.5 = down one. */
  void setRatio(float ratio) { ratio_ = clampf(ratio, 0.25f, 4.0f); }
  int latencySamples() const { return (int) (window_ * 0.5f); }
  void reset() { line_.reset(); phase_ = 0.0f; }

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
    const float ga = std::sin((float) kPi * dA / window_);
    const float gb = std::sin((float) kPi * dB / window_);
    line_.write(x);
    return a * ga + b * gb;
  }

private:
  DelayLine<MaxWindow> line_;
  float sr_ = 48000.0f, windowMs_ = 50.0f, window_ = 2400.0f, ratio_ = 1.0f, phase_ = 0.0f;
};

} // namespace sonore
