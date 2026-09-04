// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: FIR filters, designed and run.
//
// The biquads in dsp.h cover everything that wants to be cheap and minimum
// phase. An FIR is what you reach for when the requirement is LINEAR PHASE:
// a mastering EQ, a crossover whose bands must sum flat, a filter that must
// not smear transients, and the price is latency of exactly half the kernel.
//
// Design and processing are separate on purpose: design() allocates and
// belongs in prepare(), process() allocates nothing and is real-time safe.

#pragma once

#include <cmath>
#include <cstddef>
#include <vector>

// Included by dsp.h, so `#include <sonore/dsp.h>` stays the single entry point.
#include "special.h"
#include "audio.h"
#include "simd.h"

namespace sonore {

/** Window shapes for kernel design. The trade is always the same: a narrower
 *  transition band costs worse stopband rejection, and vice versa. */
enum class FirWindow {
  Rectangular, // sharpest transition, -21 dB stopband: almost never right
  Hann,        // -44 dB
  Hamming,     // -53 dB
  Blackman,    // -74 dB, the safe default
  BlackmanHarris, // -92 dB, for when nothing may leak through
  /** The only one with a DIAL.
   *
   *  Every window above is a fixed trade: a stopband figure and a transition
   *  width that come as a pair, take it or leave it. Kaiser has a parameter,
   *  so a caller can ask for the attenuation the job needs and pay for
   *  exactly that in taps -- which is what makes designing to a SPECIFICATION
   *  possible rather than picking the closest name off a list. */
  Kaiser,
};

/** How sharp a Kaiser window is. Only meaningful with FirWindow::Kaiser; the
 *  fixed windows ignore it.
 *
 *  8.6 is roughly -90 dB, which is where a float kernel stops being the limit
 *  and the format is. */
struct FirWindowParams {
  double kaiserBeta = 8.6;
};

namespace firdetail {

inline double windowValue(FirWindow kind, size_t i, size_t n, double kaiserBeta = 8.6) {
  if (n <= 1) return 1.0;
  const double t = (double) i / (double) (n - 1);
  const double pi = 3.14159265358979323846;
  switch (kind) {
    case FirWindow::Rectangular:
      return 1.0;
    case FirWindow::Hann:
      return 0.5 - 0.5 * std::cos(2.0 * pi * t);
    case FirWindow::Hamming:
      return 0.54 - 0.46 * std::cos(2.0 * pi * t);
    case FirWindow::Blackman:
      return 0.42 - 0.5 * std::cos(2.0 * pi * t) + 0.08 * std::cos(4.0 * pi * t);
    case FirWindow::BlackmanHarris:
      return 0.35875 - 0.48829 * std::cos(2.0 * pi * t) + 0.14128 * std::cos(4.0 * pi * t) -
             0.01168 * std::cos(6.0 * pi * t);
    case FirWindow::Kaiser:
      // r runs -1..1 across the kernel. The shape itself lives in special.h,
      // because a resampling kernel needed the same one and grew its own copy.
      return special::kaiser(2.0 * t - 1.0, kaiserBeta);
  }
  return 1.0;
}

/** The ideal lowpass impulse response: a sinc centred on the kernel, sampled.
 *  `normalised` is the cutoff as a fraction of the sample rate (0..0.5). */
inline double sincAt(size_t i, size_t taps, double normalised) {
  const double pi = 3.14159265358979323846;
  const double centre = (double) (taps - 1) * 0.5;
  const double x = (double) i - centre;
  const double w = 2.0 * pi * normalised;
  if (std::fabs(x) < 1e-9) return 2.0 * normalised;
  return std::sin(w * x) / (pi * x);
}

} // namespace firdetail

/**
 * A linear-phase FIR filter.
 *
 * Kernels are designed by the windowed-sinc method, which is the one that
 * gives a predictable, symmetric kernel, and symmetry is exactly what makes
 * the phase linear. An ODD tap count is forced for highpass and bandstop
 * designs because an even-length kernel has a null at Nyquist, which silently
 * turns a highpass into something that is not one.
 */
class FirFilter {
public:
  /**
   * A lowpass that MEETS A SPECIFICATION, rather than one built from a tap
   * count somebody guessed.
   *
   * The usual way round is backwards: a caller picks 63 taps and a Blackman
   * window and then measures what they got. What a caller actually knows is
   * the job -- "nothing above 18 kHz may get through by more than -80 dB, and
   * I can afford a 2 kHz transition" -- and the Kaiser method turns exactly
   * that into a beta and a tap count.
   *
   * `transitionHz` is the width of the ramp from passband to stopband, and it
   * is the expensive parameter: halving it doubles the taps. `stopbandDb` is
   * how far down the stopband must be, as a POSITIVE number of decibels.
   *
   * Returns the tap count it settled on, or 0 if the request cannot be met
   * within the tap limit -- refused rather than quietly delivered as a
   * shallower filter, because a caller that asked for -100 dB and got -40
   * would ship the -40.
   */
  size_t designLowpassToSpec(float cutoffHz, float sampleRate, float transitionHz,
                             float stopbandDb) {
    if (!(transitionHz > 0.0f) || !(sampleRate > 0.0f) || !(stopbandDb > 0.0f)) return 0;
    // Design for a little MORE than was asked for.
    //
    // Kaiser's formulas are empirical -- fitted to measured designs, not
    // derived -- and land within a decibel or two either way. Asking for -100
    // and delivering -98.9 is inside that error and still a filter that does
    // not do what it was told. Two decibels of guard band costs a handful of
    // taps and turns "about right" into "at least this much", which is what a
    // specification is for.
    const double a = (double) stopbandDb + 2.0;

    // Kaiser's own empirical formulas, and they are empirical -- fitted to
    // measured designs rather than derived. That is why the test checks the
    // filter that comes out instead of trusting the arithmetic.
    double beta = 0.0;
    if (a > 50.0)
      beta = 0.1102 * (a - 8.7);
    else if (a >= 21.0)
      beta = 0.5842 * std::pow(a - 21.0, 0.4) + 0.07886 * (a - 21.0);

    const double pi = 3.14159265358979323846;
    const double deltaOmega = 2.0 * pi * (double) transitionHz / (double) sampleRate;
    const double exact = (a - 8.0) / (2.285 * deltaOmega);
    size_t taps = (size_t) std::ceil(exact) + 1;
    taps |= 1; // odd, so there is a centre tap and the phase stays linear
    if (taps < 3) taps = 3;
    if (taps > kMaxTaps) return 0;

    const double normalised = clampCutoff(cutoffHz, sampleRate);
    build(taps, FirWindow::Kaiser,
          [normalised](size_t i, size_t n) { return firdetail::sincAt(i, n, normalised); }, beta);
    return taps;
  }

  /** Lowpass at `cutoffHz`. More taps = steeper, at one sample of latency per
   *  two taps. */
  void designLowpass(float cutoffHz, float sampleRate, size_t taps,
                     FirWindow window = FirWindow::Blackman) {
    const double normalised = clampCutoff(cutoffHz, sampleRate);
    build(taps, window, [normalised](size_t i, size_t n) {
      return firdetail::sincAt(i, n, normalised);
    });
  }

  /** Highpass, by spectral inversion of the matching lowpass: negate every
   *  coefficient and add 1 at the centre. That identity is why the tap count
   *  must be odd: there has to BE a centre.
   *
   *  The lowpass is NORMALISED to unity DC before it is inverted. Inverting
   *  the raw windowed sinc -- the first version -- leaves the highpass with a
   *  DC gain of 1 minus whatever the window made the sum, which for a
   *  201-tap Blackman is -40 dB of DC coming through a filter whose one job
   *  is to stop it. Normalised, the coefficients sum to zero to float
   *  precision and the null is real. */
  void designHighpass(float cutoffHz, float sampleRate, size_t taps,
                      FirWindow window = FirWindow::Blackman) {
    designLowpass(cutoffHz, sampleRate, taps | 1, window);
    for (float& c : coefficients_) c = -c;
    coefficients_[(coefficients_.size() - 1) / 2] += 1.0f;
  }

  /** Bandpass: the difference of two lowpass kernels, each normalised to
   *  unity DC first so their difference is zero there exactly. */
  void designBandpass(float lowHz, float highHz, float sampleRate, size_t taps,
                      FirWindow window = FirWindow::Blackman) {
    const float a = lowHz < highHz ? lowHz : highHz;
    const float b = lowHz < highHz ? highHz : lowHz;
    designLowpass(b, sampleRate, taps | 1, window);
    std::vector<float> upper = coefficients_; // design time: allocation is allowed here
    designLowpass(a, sampleRate, taps | 1, window);
    for (size_t i = 0; i < coefficients_.size() && i < upper.size(); ++i)
      coefficients_[i] = upper[i] - coefficients_[i];
  }

  /** Install a kernel designed elsewhere (a measured impulse response, say). */
  void setCoefficients(const float* taps, size_t count) {
    coefficients_.assign(taps, taps + count);
    history_.assign(count, 0.0f);
    position_ = 0;
  }

  void reset() {
    std::fill(history_.begin(), history_.end(), 0.0f);
    position_ = 0;
  }

  size_t numTaps() const { return coefficients_.size(); }

  /** Latency in samples: half the kernel, which is what linear phase costs.
   *  A plugin using this MUST report it or the host cannot time-align it. */
  int latencySamples() const {
    return coefficients_.empty() ? 0 : (int) ((coefficients_.size() - 1) / 2);
  }

  inline float process(float x) {
    if (coefficients_.empty()) return x;
    const size_t n = coefficients_.size();
    history_[position_] = x;

    // Two straight runs rather than one modulo per tap: the wrap is computed
    // once instead of n times, which is most of the cost at short kernels.
    float acc = 0.0f;
    const size_t first = n - position_;
    acc += simd::dot(coefficients_.data(), history_.data() + position_, first);
    if (position_ > 0) acc += simd::dot(coefficients_.data() + first, history_.data(), position_);

    position_ = position_ == 0 ? n - 1 : position_ - 1;
    return acc;
  }

  const std::vector<float>& coefficients() const { return coefficients_; }

private:
  static double clampCutoff(float hz, float sampleRate) {
    const double sr = sampleRate > 1.0f ? (double) sampleRate : 48000.0;
    double normalised = (double) hz / sr;
    if (normalised < 1e-5) normalised = 1e-5;
    if (normalised > 0.4999) normalised = 0.4999;
    return normalised;
  }

  /** Past this a kernel costs more than any audible gain, and a caller that
   *  asked for more has asked for something that will not fit. */
  static constexpr size_t kMaxTaps = 4096;

  template <typename Ideal>
  void build(size_t taps, FirWindow window, Ideal&& ideal, double kaiserBeta = 8.6) {
    if (taps < 3) taps = 3;
    if (taps > kMaxTaps) taps = kMaxTaps;
    coefficients_.assign(taps, 0.0f);
    double sum = 0.0;
    for (size_t i = 0; i < taps; ++i) {
      const double v = ideal(i, taps) * firdetail::windowValue(window, i, taps, kaiserBeta);
      coefficients_[i] = (float) v;
      sum += v;
    }
    // Normalise a lowpass to unity DC gain. Highpass and bandpass kernels sum
    // to ~0 by construction, so normalising them would divide by nothing:
    // the guard is what stops a "steeper filter" turning into silence.
    if (std::fabs(sum) > 1e-3)
      for (float& c : coefficients_) c = (float) ((double) c / sum);

    // The delay line is stored REVERSED relative to the kernel, so the dot
    // product above needs no index arithmetic per tap.
    history_.assign(taps, 0.0f);
    position_ = 0;
  }

  std::vector<float> coefficients_;
  std::vector<float> history_;
  size_t position_ = 0;
};

} // namespace sonore
