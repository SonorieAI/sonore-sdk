// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: the analytic signal, and the frequency shifter built on it.
//
// A frequency shifter moves every partial by the same number of hertz --
// not a pitch shift, which multiplies them -- and the result is the Bode
// shifter's inharmonic shimmer, the feedback-safe "detune" that never
// builds up, single-sideband ring modulation. All of it needs the signal's
// quadrature twin: a copy with every frequency shifted by exactly ninety
// degrees, at every frequency at once. No FIR of sensible length does that,
// and no first-order allpass does; what does is a PAIR of allpass cascades
// whose phase responses differ by ninety degrees across the band.
//
// The pair is derived, not pasted. An elliptic half-band lowpass can be
// written as two parallel allpass branches, H(z) = ½[A0(z²) + z⁻¹A1(z²)]
// (Valenzuela & Constantinides 1983; Ansari 1985 for the elliptic design of
// exactly this class), and its coefficients come from the elliptic nome for
// the transition width asked for -- the same construction resample.h uses
// with Butterworth poles, here elliptic because elliptic is what makes a
// twelve-section pair accurate to a degree from 300 Hz to 20 kHz. Rotating
// the half-band's frequency axis by a quarter turn (z -> jz, which negates
// each section's coefficient) turns the two branches into the Hilbert pair:
// the passband between the transition edges becomes the band over which the
// branches are ninety degrees apart. The test measures that angle at six
// frequencies and the image rejection of the shifter, which is what the
// angle buys.
//
// Included by dsp.h.
#pragma once
#include <cmath>
#include "audio.h"

namespace sonore {

namespace hilbertdetail {

/**
 * Allpass coefficients of an elliptic half-band IIR with `sections` second-
 * order sections in total, for a transition band of half-width
 * `transition` (as a fraction of the sample rate) around fs/4. The nome q
 * of the elliptic modulus k = tan²(pi (1/4 - transition)) gives the
 * Jacobian elliptic sine at the section's argument; the coefficient is
 * (1 - v)/(1 + v) with v from the modulus and that sine.
 */
inline void ellipticHalfBand(int sections, double transition, double* coefficients) {
  const double pi = 3.14159265358979323846;
  const double t = transition < 1e-4 ? 1e-4 : (transition > 0.24 ? 0.24 : transition);
  const double k = std::tan(pi * (0.25 - t)) * std::tan(pi * (0.25 - t));
  const double kk = std::sqrt(1.0 - k * k);
  const double e = 0.5 * (1.0 - std::sqrt(kk)) / (1.0 + std::sqrt(kk));
  const double q = e + 2.0 * std::pow(e, 5.0) + 15.0 * std::pow(e, 9.0) + 150.0 * std::pow(e, 13.0);
  const int order = 2 * sections + 1;
  for (int i = 1; i <= sections; ++i) {
    double num = 0.0, den = 0.0;
    for (int m = 0; m < 8; ++m)
      num += (m & 1 ? -1.0 : 1.0) * std::pow(q, (double) (m * (m + 1))) * std::sin((2.0 * m + 1.0) * pi * i / order);
    for (int m = 1; m < 8; ++m) den += (m & 1 ? -1.0 : 1.0) * std::pow(q, (double) (m * m)) * std::cos(2.0 * m * pi * i / order);
    const double w = 2.0 * std::pow(q, 0.25) * num / (1.0 + 2.0 * den);
    const double v = std::sqrt((1.0 - w * w * k) * (1.0 - w * w / k));
    coefficients[i - 1] = (1.0 - v) / (1.0 + v);
  }
}

} // namespace hilbertdetail

/**
 * Two allpass cascades, ninety degrees apart from `transition` × fs up to
 * (½ − transition) × fs. Sections is the total; they alternate between the
 * branches. 12 sections and a transition of 0.005 (240 Hz at 48 kHz) is the
 * default: a degree of error across the band that matters.
 */
template <int Sections = 12>
class HilbertTransformer {
  static_assert(Sections >= 2 && Sections <= 32, "2..32 sections");

public:
  HilbertTransformer() { design(0.005); }

  /** Half-width of the region at each end of the band where the pair is
   *  not yet ninety degrees, as a fraction of the sample rate. */
  void design(double transition) {
    double c[Sections];
    hilbertdetail::ellipticHalfBand(Sections, transition, c);
    // z -> jz: each section (a + z^-2)/(1 + a z^-2) becomes (a - z^-2)/(1 - a z^-2),
    // an allpass with coefficient -a and a sign. The sections alternate.
    nA_ = nB_ = 0;
    for (int i = 0; i < Sections; ++i) {
      if (i % 2 == 0) a_[nA_++] = -c[i];
      else b_[nB_++] = -c[i];
    }
    reset();
  }
  void reset() {
    for (int i = 0; i < Sections; ++i) { ax1_[i] = ax2_[i] = ay1_[i] = ay2_[i] = 0.0f; bx1_[i] = bx2_[i] = by1_[i] = by2_[i] = 0.0f; }
    delay_ = 0.0f;
  }

  /** The in-phase and quadrature outputs for one sample. Both are delayed
   *  by the cascade's group delay; they are ninety degrees apart across the
   *  band, not aligned with the input. */
  inline void process(float x, float& inPhase, float& quadrature) {
    float a = x;
    for (int i = 0; i < nA_; ++i) a = allpass(a, a_[i], ax1_[i], ax2_[i], ay1_[i], ay2_[i]);
    float b = delay_;
    delay_ = x;
    for (int i = 0; i < nB_; ++i) b = allpass(b, b_[i], bx1_[i], bx2_[i], by1_[i], by2_[i]);
    inPhase = a;
    quadrature = b;
  }

private:
  /** (c + z^-2)/(1 + c z^-2): y = c (x - y2) + x2. */
  static inline float allpass(float x, float c, float& x1, float& x2, float& y1, float& y2) {
    const float y = c * (x - y2) + x2;
    x2 = x1; x1 = x;
    y2 = y1; y1 = flushDenormal(y);
    return y;
  }

  float a_[Sections]{}, b_[Sections]{};
  float ax1_[Sections]{}, ax2_[Sections]{}, ay1_[Sections]{}, ay2_[Sections]{};
  float bx1_[Sections]{}, bx2_[Sections]{}, by1_[Sections]{}, by2_[Sections]{};
  float delay_ = 0.0f;
  int nA_ = 0, nB_ = 0;
};

/**
 * The Bode frequency shifter: the analytic signal multiplied by a complex
 * exponential, real part out. Positive shifts move everything up; the
 * sideband that would fold to the other side is what the Hilbert pair
 * suppresses, and its residue is the image rejection the test measures.
 * `setMix` blends the shifted signal with the dry; feedback is the caller's,
 * and this is the effect that makes a feedback loop safe to build.
 */
class FrequencyShifter {
public:
  void setSampleRate(float sr) {
    sr_ = sr > 1.0f ? sr : 48000.0f;
    setShift(shiftHz_);
  }
  /** Hertz, either sign. */
  void setShift(float hz) {
    shiftHz_ = hz;
    inc_ = hz / sr_;
  }
  void reset() {
    hilbert_.reset();
    phase_ = 0.0f;
  }

  inline float process(float x) {
    float i, q;
    hilbert_.process(x, i, q);
    const float c = fastmath::sinTurns(phase_ + 0.25f), s = fastmath::sinTurns(phase_);
    phase_ += inc_;
    phase_ -= std::floor(phase_);
    // Re[(i + j q)(cos + j sin)] = i cos - q sin: the upper sideband alone.
    return i * c - q * s;
  }

private:
  HilbertTransformer<12> hilbert_;
  float sr_ = 48000.0f, shiftHz_ = 0.0f, inc_ = 0.0f, phase_ = 0.0f;
};

} // namespace sonore
