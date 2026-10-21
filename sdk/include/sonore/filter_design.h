// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: filters steeper than one biquad.
//
// dsp.h has the RBJ cookbook, which is every shape a SINGLE biquad can be. A
// 24 dB/octave lowpass is not one of those shapes, and neither is a 48 dB
// crossover or an anti-alias filter worth the name. Getting there means more
// than one section, and the only interesting question is what Q each section
// takes: cascading identical biquads does NOT give a Butterworth, it gives a
// droopy passband and a -6 dB point that has wandered off the cutoff.
//
// What decides it: the poles of an Nth-order Butterworth sit at equal angles
// on a semicircle, and each conjugate pair becomes one biquad whose Q is set
// by where that pair sits. Order 4 is the familiar 0.54 and 1.31: two very
// different sections that are flat together and wrong separately.
//
// Odd orders need a real pole as well, which is a FIRST-order section: not a
// biquad with something zeroed by accident, but a deliberate one-pole stage,
// because a 5th-order filter that quietly rounds to 4th or 6th is a filter
// whose slope is not what the user asked for.
#pragma once

#include <cmath>
#include <cstddef>

#include "dsp.h"

namespace sonore {

/** How many biquad sections an order needs, not counting the odd first-order
 *  one. Order 4 -> 2, order 5 -> 2 (plus a one-pole), order 1 -> 0. */
constexpr int butterworthBiquads(int order) { return order / 2; }

/**
 * The Q of one section of an Nth-order Butterworth.
 *
 * The poles lie at angles (2k+1)*pi/(2N) measured from the imaginary axis, and
 * a conjugate pair at angle theta has Q = 1/(2 cos theta). Section 0 of an
 * order-4 gives 0.5412 and section 1 gives 1.3066: the classic pair, and the
 * reason cascading two 0.7071 sections is not the same filter.
 */
inline double butterworthQ(int order, int section) {
  if (order < 2 || section < 0 || section >= butterworthBiquads(order)) return 0.70710678;
  const double pi = 3.14159265358979323846;
  // The angles differ between even and odd orders, and using the even
  // spacing for both is a filter that LOOKS like a lowpass and is 4.8 dB
  // down at its own cutoff. An odd order has a pole ON the real axis, so the
  // remaining pairs are spaced pi/N from it rather than pi/2N from the axis.
  //
  //   even N: theta = pi (2k+1) / 2N   -> order 4 gives 0.5412, 1.3066
  //   odd  N: theta = pi (k+1) / N     -> order 5 gives 0.6180, 1.6180
  //
  // The order-5 pair being the golden ratio is the giveaway that the odd
  // case is its own formula and not a special case of the even one.
  const double theta = (order % 2 == 0) ? pi * (double) (2 * section + 1) / (double) (2 * order)
                                        : pi * (double) (section + 1) / (double) order;
  return 1.0 / (2.0 * std::cos(theta));
}

/**
 * One section of a Chebyshev Type I design: where its pole pair sits.
 *
 * Butterworth puts its poles on a circle; Chebyshev puts them on an ELLIPSE,
 * squashed towards the imaginary axis by an amount the ripple decides. That
 * squashing is the whole trade: the poles crowd nearer the passband edge, the
 * transition gets steeper, and the passband stops being flat and starts to
 * ripple by exactly the amount that was asked for.
 *
 * `frequencyScale` is the part Butterworth does not have. Each section sits at
 * a DIFFERENT frequency -- the pole's distance from the origin -- rather than
 * all of them at the cutoff, and ignoring it gives a filter that looks
 * plausible and has the wrong corner.
 */
struct ChebyshevSection {
  /** The section's Q, from where its pole pair sits on the ellipse. */
  double q = 0.70710678;
  /** The pair's distance from the origin, as a multiple of the passband
   *  edge: the section's natural frequency is the edge times this. */
  double frequencyScale = 1.0;
};

/** The ripple as an amplitude ratio: 1 dB of ripple is eps = 0.5088. */
inline double chebyshevEpsilon(double rippleDb) {
  if (rippleDb <= 0.0) rippleDb = 0.01;
  return std::sqrt(std::pow(10.0, rippleDb / 10.0) - 1.0);
}

/** Where pole pair `section` (0-based, 0 .. order / 2 - 1) of an
 *  order-`order` Type I design with `rippleDb` of passband ripple sits. An
 *  order below 2 gives the defaults (Q 0.7071, scale 1), and a ripple of 0
 *  or less is taken as 0.01 dB. An odd order's real pole is not a pair and
 *  is not returned here: CascadedIir builds it apart. */
inline ChebyshevSection chebyshevSectionFor(int order, int section, double rippleDb) {
  ChebyshevSection out;
  if (order < 2) return out;
  const double pi = 3.14159265358979323846;
  const double eps = chebyshevEpsilon(rippleDb);
  // asinh, spelled out: MSVC has std::asinh but the identity is one line and
  // makes the geometry visible -- v0 is how far the ellipse is squashed.
  const double v0 = std::log(1.0 / eps + std::sqrt(1.0 / (eps * eps) + 1.0)) / (double) order;
  const double sinhV = std::sinh(v0), coshV = std::cosh(v0);

  // ONE spacing for every order, (2k+1) pi / 2N, because theta here is
  // measured from the IMAGINARY axis (it is the sine that makes the real part).
  // An odd order's real pole is simply the k with theta = pi/2, which is not a
  // pair and is built separately -- the pairs below it keep the same spacing.
  //
  // This once borrowed butterworthQ's odd-order rule, pi (k+1) / N. That rule
  // is right THERE because it measures from the real axis, into Q = 1/(2 cos);
  // put into the sine and cosine below it is a different pole altogether.
  // Order 3 at 1 dB ripple was 13.2 dB down at its passband edge instead of
  // 1 dB; order 5, 21.3. The tests only ever designed orders 4 and 6.
  const double theta = pi * (double) (2 * section + 1) / (double) (2 * order);
  const double re = -sinhV * std::sin(theta);
  const double im = coshV * std::cos(theta);
  const double magnitude = std::sqrt(re * re + im * im);
  out.frequencyScale = magnitude;
  out.q = magnitude / (2.0 * std::fabs(re));
  return out;
}

// IirSection, the double-precision section, lives in dsp.h beside Biquad:
// the equalisers need it too, and they do not include this file.

/** The bilinear substitution's normalised coefficients, shared by both kinds
 *  of section below. `ok` is false for a degenerate denominator. */
struct BilinearCoefficients {
  ///@{
  /** The digital section with a0 normalised to 1: numerator b0 + b1 z^-1 +
   *  b2 z^-2, denominator 1 + a1 z^-1 + a2 z^-2, in the order
   *  setCoefficients() takes them. The defaults pass the input through. */
  double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
  ///@}
  /** False when the denominator came out degenerate (the digital a0, before
   *  normalising, near zero); the coefficients are then the pass-through
   *  defaults, and
   *  bilinearSection() leaves its section alone. */
  bool ok = false;
};

/**
 * One second-order analog section, turned into a digital biquad.
 *
 * The cookbook shapes in dsp.h cover a section whose zero sits where its pole
 * does, or at DC, or at infinity. Chebyshev Type II needs one whose zero is
 * somewhere ELSE entirely -- a notch part-way up the stopband, with the pole
 * below it -- and no named shape expresses that. So this does the substitution
 * itself.
 *
 *   H(S) = (b2 S^2 + b1 S + b0) / (a2 S^2 + a1 S + a0)
 *
 * with S the PROTOTYPE frequency, normalised so the design's corner is at 1,
 * and S = c(1 - z^-1)/(1 + z^-1) where c = 1/tan(pi f0 / fs). That tan is the
 * prewarping: without it the corner lands somewhere near where it was asked
 * for and drifts further out the closer it gets to Nyquist.
 *
 * The analog coefficients come highest power first, for a corner of
 * `cutoffHz` at `sampleRate`, both in Hz; the corner must lie between 0 and
 * Nyquist for the tangent to mean anything. A design-time call: a tangent
 * and a few divisions, in double.
 */
inline BilinearCoefficients bilinearCoefficients(double b2, double b1, double b0, double a2,
                                                 double a1, double a0, double cutoffHz,
                                                 double sampleRate) {
  const double pi = 3.14159265358979323846;
  const double c = 1.0 / std::tan(pi * cutoffHz / sampleRate);
  const double cc = c * c;

  const double nb0 = b2 * cc + b1 * c + b0;
  const double nb1 = 2.0 * (b0 - b2 * cc);
  const double nb2 = b2 * cc - b1 * c + b0;
  const double na0 = a2 * cc + a1 * c + a0;
  const double na1 = 2.0 * (a0 - a2 * cc);
  const double na2 = a2 * cc - a1 * c + a0;

  BilinearCoefficients out;
  if (std::fabs(na0) < 1e-30) return out;
  out.b0 = nb0 / na0;
  out.b1 = nb1 / na0;
  out.b2 = nb2 / na0;
  out.a1 = na1 / na0;
  out.a2 = na2 / na0;
  out.ok = true;
  return out;
}

/** bilinearCoefficients() written into a float Biquad. A degenerate result
 *  leaves `out` as it was. Its state is not cleared. */
inline void bilinearSection(double b2, double b1, double b0, double a2, double a1, double a0,
                            double cutoffHz, double sampleRate, Biquad& out) {
  const BilinearCoefficients k = bilinearCoefficients(b2, b1, b0, a2, a1, a0, cutoffHz, sampleRate);
  if (!k.ok) return;
  out.setCoefficients((float) k.b0, (float) k.b1, (float) k.b2, (float) k.a1, (float) k.a2);
}

/** As above, into a double-precision IirSection: the form to use for a
 *  corner far below the sample rate, where float coefficients lose it. */
inline void bilinearSection(double b2, double b1, double b0, double a2, double a1, double a0,
                            double cutoffHz, double sampleRate, IirSection& out) {
  const BilinearCoefficients k = bilinearCoefficients(b2, b1, b0, a2, a1, a0, cutoffHz, sampleRate);
  if (!k.ok) return;
  out.setCoefficients(k.b0, k.b1, k.b2, k.a1, k.a2);
}

/**
 * The same substitution for a FIRST-order section, H(S) = (b1 S + b0)/(a1 S + a0).
 *
 * Not bilinearSection with the squared terms zeroed. That works on paper and
 * not in a float: a first-order section pushed through the second-order
 * formula comes out with a POLE ON z = -1 (or +1 for a highpass) that is
 * meant to cancel a zero at the same place. In exact arithmetic it does; in
 * float coefficients the two land a rounding apart, and the leftover pole sits
 * on the unit circle -- an order-3 Type II highpass at 2 kHz built that way
 * integrated its noise to 125 times its input level.
 */
inline void bilinearFirstOrder(double b1, double b0, double a1, double a0, double cutoffHz,
                               double sampleRate, Biquad& out) {
  const double pi = 3.14159265358979323846;
  const double c = 1.0 / std::tan(pi * cutoffHz / sampleRate);
  const double na0 = a1 * c + a0;
  if (std::fabs(na0) < 1e-30) return;
  out.setCoefficients((float) ((b1 * c + b0) / na0), (float) ((b0 - b1 * c) / na0), 0.0f,
                      (float) ((a0 - a1 * c) / na0), 0.0f);
}

/** As above, into a double-precision IirSection. A degenerate denominator
 *  leaves `out` as it was. */
inline void bilinearFirstOrder(double b1, double b0, double a1, double a0, double cutoffHz,
                               double sampleRate, IirSection& out) {
  const double pi = 3.14159265358979323846;
  const double c = 1.0 / std::tan(pi * cutoffHz / sampleRate);
  const double na0 = a1 * c + a0;
  if (std::fabs(na0) < 1e-30) return;
  out.setCoefficients((b1 * c + b0) / na0, (b0 - b1 * c) / na0, 0.0, (a0 - a1 * c) / na0, 0.0);
}

/**
 * One section of a Chebyshev Type II design.
 *
 * Type I ripples in the PASSBAND and is flat in the stopband; Type II is the
 * other way round, and that is usually the trade you want. An anti-alias
 * filter's stopband is thrown away, so ripple there costs nothing, while
 * ripple in the passband is ripple in the signal you are keeping.
 *
 * It is not an all-pole design. Type I gets its steepness from where its poles
 * sit; Type II gets it from ZEROS -- notches placed up the stopband, which is
 * why its rejection is equiripple rather than falling away for ever, and why
 * each section needs a numerator of its own.
 *
 * The poles are the reciprocals of Type I's, which is the whole construction:
 * inverting the frequency axis turns a rippling passband into a rippling
 * stopband.
 */
struct Chebyshev2Section {
  /** Pole, as its real part and its distance from the origin. */
  double poleReal = -0.70710678;
  /** The pole's distance from the origin, to go with poleReal. */
  double poleMagnitude = 1.0;
  /** Zero, on the imaginary axis, at this distance from the origin. */
  double zero = 0.0;
};

/** Where pole pair `section` (0-based, 0 .. order / 2 - 1) of an
 *  order-`order` Type II design with `stopbandDb` of rejection sits, on the
 *  prototype normalised to the stopband edge. An order below 2 gives the
 *  defaults; a rejection under 1 dB is taken as 1. */
inline Chebyshev2Section chebyshev2SectionFor(int order, int section, double stopbandDb) {
  Chebyshev2Section out;
  if (order < 2) return out;
  const double pi = 3.14159265358979323846;
  if (stopbandDb < 1.0) stopbandDb = 1.0;
  // Type II's epsilon is the RECIPROCAL of Type I's: Type I is told how much
  // ripple to allow in what it keeps, Type II how far down to push what it
  // throws away.
  const double eps = 1.0 / std::sqrt(std::pow(10.0, stopbandDb / 10.0) - 1.0);
  const double v0 = std::log(1.0 / eps + std::sqrt(1.0 / (eps * eps) + 1.0)) / (double) order;
  const double sinhV = std::sinh(v0), coshV = std::cosh(v0);

  // The same spacing as chebyshevSectionFor, for the same reason: see there.
  const double theta = pi * (double) (2 * section + 1) / (double) (2 * order);

  // Type I's pole, then inverted. 1/(x+iy) = (x-iy)/(x^2+y^2).
  const double re = -sinhV * std::sin(theta);
  const double im = coshV * std::cos(theta);
  const double denominator = re * re + im * im;
  out.poleReal = re / denominator;
  out.poleMagnitude = 1.0 / std::sqrt(denominator);
  // The zero is where cos(theta) puts it, and a theta of pi/2 -- the real
  // pole of an odd order -- would put it at infinity, which is the same thing
  // as having no zero at all.
  const double cosTheta = std::cos(theta);
  out.zero = std::fabs(cosTheta) > 1e-12 ? 1.0 / cosTheta : 0.0;
  return out;
}

/**
 * A cascade of biquads, plus at most one first-order section.
 *
 * MaxOrder bounds the storage, so a plugin pays for the steepest filter it
 * might ask for and nothing more. Designing past it is REFUSED rather than
 * clamped: a caller that asked for 12th order and silently got 8th has a
 * crossover that leaks, and would never find out from here.
 */
template <int MaxOrder = 8>
class CascadedIir {
public:
  /** Biquad storage: (MaxOrder + 1) / 2 sections, enough for every pole
   *  pair of the highest order. An odd order's real pole has its own. */
  static constexpr int kMaxSections = (MaxOrder + 1) / 2;

  /** The rate in Hz; 0 or less means 48 kHz. Set it before designing: a
   *  design is made for the rate set at the time, and a later change does
   *  not redesign, so design again after it. */
  void setSampleRate(float sr) {
    sr_ = sr > 0.0f ? sr : 48000.0f;
    for (int i = 0; i < kMaxSections; ++i) sections_[i].setSampleRate(sr_);
    onePole_.setSampleRate(sr_);
  }

  /** Clear every section's state; the design is kept. No allocation. */
  void reset() {
    for (int i = 0; i < kMaxSections; ++i) sections_[i].reset();
    onePole_.reset();
  }

  /** The order of the last successful design; 0 before one. */
  int order() const { return order_; }

  /** Maximally flat lowpass. Returns false, and leaves the filter as it was,
   *  if the order does not fit or is not a positive number. */
  bool designButterworthLowpass(float freq, int order) { return design(freq, order, false); }
  /** Maximally flat highpass, `freq` its -3 dB point in Hz. Refused, with
   *  false and the filter left as it was, for an order outside 1 ..
   *  MaxOrder or a frequency not between 0 and Nyquist. Like every design
   *  here it keeps the filter's state when the order and kind stay the
   *  same, so it may be called every block with a moving cutoff. */
  bool designButterworthHighpass(float freq, int order) { return design(freq, order, true); }

  /**
   * Chebyshev Type I: steeper than Butterworth of the same order, at the cost
   * of a passband that ripples by exactly `rippleDb`.
   *
   * The cutoff means something different here and it is worth being explicit
   * about: for Butterworth it is the -3 dB point, for Chebyshev it is the
   * PASSBAND EDGE, where the ripple stops and the response is -rippleDb. A
   * caller swapping one for the other and expecting -3 dB at the same
   * frequency will find the corner has moved.
   */
  bool designChebyshevLowpass(float freq, int order, float rippleDb) {
    return designChebyshev(freq, order, rippleDb, false);
  }
  /** Type I highpass: `freq` is the passband edge, where the response is
   *  -rippleDb, and the ripple must be above 0 dB. Refused as the lowpass
   *  is. */
  bool designChebyshevHighpass(float freq, int order, float rippleDb) {
    return designChebyshev(freq, order, rippleDb, true);
  }

  /**
   * Chebyshev Type II: FLAT passband, equiripple stopband.
   *
   * `freq` is the STOPBAND EDGE -- the frequency at which rejection reaches
   * `stopbandDb` and stays there. Not the -3 dB point and not the passband
   * edge; a caller who hands it the same number they would give a Butterworth
   * will get a filter that starts rejecting well above where they meant.
   *
   * The trade against Type I is worth stating plainly, because "Chebyshev" on
   * its own does not say which: Type I ripples in what it KEEPS, Type II in
   * what it THROWS AWAY. For an anti-alias filter that makes Type II close to
   * free -- the stopband is discarded, so ripple there costs nothing.
   */
  bool designChebyshev2Lowpass(float freq, int order, float stopbandDb) {
    return designChebyshev2(freq, order, stopbandDb, false);
  }
  /** Type II highpass: `freq` is the stopband edge, below which the
   *  rejection is at least `stopbandDb` (above 0). Refused, with false and
   *  the filter unchanged, for an order outside 1 .. MaxOrder or a
   *  frequency not between 0 and Nyquist. */
  bool designChebyshev2Highpass(float freq, int order, float stopbandDb) {
    return designChebyshev2(freq, order, stopbandDb, true);
  }

  /** [audio-thread] One sample through the cascade. Before the first
   *  design it passes the input through. No allocation. */
  inline float process(float x) {
    if (hasOnePole_) x = onePole_.process(x);
    for (int i = 0; i < biquads_; ++i) x = sections_[i].process(x);
    return x * gain_;
  }

  /** The whole cascade's gain at a frequency, in dB: the sections' responses
   *  add, because they are in series and dB is a logarithm. */
  float magnitudeDb(float freq) const {
    float sum = gain_ > 0.0f ? 20.0f * std::log10(gain_) : -300.0f;
    if (hasOnePole_) sum += onePole_.magnitudeDb(freq);
    for (int i = 0; i < biquads_; ++i) sum += sections_[i].magnitudeDb(freq);
    return sum;
  }

private:
  bool designChebyshev2(float freq, int order, float stopbandDb, bool highpass) {
    if (order < 1 || order > MaxOrder || !(stopbandDb > 0.0f)) return false;
    const float nyquist = sr_ * 0.5f;
    if (!(freq > 0.0f) || freq >= nyquist) return false;

    shape(order, highpass ? 5 : 4);
    gain_ = 1.0f;

    for (int i = 0; i < biquads_; ++i) {
      const Chebyshev2Section section = chebyshev2SectionFor(order, i, (double) stopbandDb);
      const double wz = section.zero;
      const double pm2 = section.poleMagnitude * section.poleMagnitude;

      // Numerator (S^2 + wz^2), denominator (S^2 - 2*Re(p)*S + |p|^2), scaled
      // for unity gain where the passband is. A zero at infinity -- which is
      // what a section with no zero means -- degenerates to the all-pole form.
      double b2 = 1.0, b1 = 0.0, b0 = wz * wz;
      double a2 = 1.0, a1 = -2.0 * section.poleReal, a0 = pm2;
      if (std::fabs(wz) < 1e-12) {
        b2 = 0.0;
        b0 = 1.0;
      }

      if (highpass) {
        // S -> 1/S turns a lowpass prototype into a highpass, and on a ratio
        // of polynomials that is nothing more than reversing the coefficients.
        std::swap(b2, b0);
        std::swap(a2, a0);
      }

      // Unity where the passband is: at DC for a lowpass, at Nyquist for a
      // highpass. Normalising the analog section rather than trimming the
      // cascade afterwards keeps each section's own gain sane, which matters
      // for a float cascade where an intermediate stage can otherwise sit
      // forty decibels hot.
      const double referenceNum = highpass ? b2 : b0;
      const double referenceDen = highpass ? a2 : a0;
      if (std::fabs(referenceNum) > 1e-30) {
        const double scale = referenceDen / referenceNum;
        b2 *= scale;
        b1 *= scale;
        b0 *= scale;
      }
      bilinearSection(b2, b1, b0, a2, a1, a0, (double) freq, (double) sr_, sections_[i]);
    }

    if (hasOnePole_) {
      // The real pole, which for Type II is the reciprocal of Type I's and
      // carries no zero: the odd order's extra root is at infinity.
      const double eps =
          1.0 / std::sqrt(std::pow(10.0, (double) stopbandDb / 10.0) - 1.0);
      const double v0 =
          std::log(1.0 / eps + std::sqrt(1.0 / (eps * eps) + 1.0)) / (double) order;
      // LOWPASS pr / (S + pr); HIGHPASS, by S -> 1/S, S / (S + 1/pr). The
      // highpass used to put its pole at pr rather than 1/pr, and went through
      // the second-order substitution besides -- see bilinearFirstOrder.
      const double pr = 1.0 / std::sinh(v0);
      if (highpass)
        bilinearFirstOrder(1.0, 0.0, 1.0, 1.0 / pr, (double) freq, (double) sr_, onePole_);
      else
        bilinearFirstOrder(0.0, pr, 1.0, pr, (double) freq, (double) sr_, onePole_);
    }
    return true;
  }

  bool designChebyshev(float freq, int order, float rippleDb, bool highpass) {
    if (order < 1 || order > MaxOrder || !(rippleDb > 0.0f)) return false;
    const float nyquist = sr_ * 0.5f;
    if (!(freq > 0.0f) || freq >= nyquist) return false;

    const double eps = chebyshevEpsilon((double) rippleDb);
    const double v0 =
        std::log(1.0 / eps + std::sqrt(1.0 / (eps * eps) + 1.0)) / (double) order;

    shape(order, highpass ? 3 : 2);

    // The analog prototype, normalised so the passband edge is at 1, and ONE
    // bilinear transform prewarped at that edge -- so the edge lands exactly on
    // `freq` and every pole of a stable prototype lands inside the unit circle,
    // whatever the cutoff.
    //
    // It was built from cookbook biquads at freq * frequencyScale instead, each
    // prewarped at its OWN frequency. That is not the transform of this
    // prototype, it drifts from it towards Nyquist, and a highpass divides by
    // the scale: its sections and its real pole landed past Nyquist, where the
    // tangent turns negative. A pre-check refused the pairs but not the real
    // pole, so an order-3 highpass at 12 kHz designed "fine" and rang to
    // infinity. With one prewarp there is nothing left to refuse.
    for (int i = 0; i < biquads_; ++i) {
      const ChebyshevSection section = chebyshevSectionFor(order, i, (double) rippleDb);
      const double m = section.frequencyScale, m2 = m * m, damping = m / section.q;
      // Lowpass m^2 / (S^2 + (m/q) S + m^2); the highpass is S -> 1/S.
      if (highpass)
        bilinearSection(m2, 0.0, 0.0, m2, damping, 1.0, (double) freq, (double) sr_, sections_[i]);
      else
        bilinearSection(0.0, 0.0, m2, 1.0, damping, m2, (double) freq, (double) sr_, sections_[i]);
    }

    if (hasOnePole_) {
      // The real pole, at sinh(v0) on the prototype: sinh(v0) / (S + sinh(v0)),
      // and S / (S + 1/sinh(v0)) for the highpass.
      const double sigma = std::sinh(v0);
      if (highpass)
        bilinearFirstOrder(1.0, 0.0, 1.0, 1.0 / sigma, (double) freq, (double) sr_, onePole_);
      else
        bilinearFirstOrder(0.0, sigma, 1.0, sigma, (double) freq, (double) sr_, onePole_);
    }

    // EVEN orders need a gain trim and odd orders do not, which is the part
    // that looks like a mistake until you draw it. Each section is normalised
    // to unity at DC; cascading them puts DC at 0 dB, and a Chebyshev of even
    // order is supposed to START at -rippleDb and ripple UP to 0. Without
    // this the whole passband sits `rippleDb` too high and the filter is
    // quietly louder than it was asked to be.
    gain_ = (order % 2 == 0) ? (float) (1.0 / std::sqrt(1.0 + eps * eps)) : 1.0f;
    return true;
  }

  bool design(float freq, int order, bool highpass) {
    if (order < 1 || order > MaxOrder) return false;
    const float nyquist = sr_ * 0.5f;
    if (!(freq > 0.0f) || freq >= nyquist) return false;

    shape(order, highpass ? 1 : 0);

    // By the bilinear transform directly, as the Chebyshev designs are, and
    // NOT through Biquad::lowpass/highpass: those clamp the corner to 10 Hz,
    // so a 5 Hz order-4 highpass -- a DC blocker, a rumble filter -- read
    // -23.6 dB at its own corner instead of -3, and an odd order mixed a 5 Hz
    // real pole (unclamped) with 10 Hz pairs into no Butterworth at all.
    for (int i = 0; i < biquads_; ++i) {
      const double damping = 1.0 / butterworthQ(order, i);
      if (highpass)
        bilinearSection(1.0, 0.0, 0.0, 1.0, damping, 1.0, (double) freq, (double) sr_, sections_[i]);
      else
        bilinearSection(0.0, 0.0, 1.0, 1.0, damping, 1.0, (double) freq, (double) sr_, sections_[i]);
    }

    gain_ = 1.0f;
    if (hasOnePole_) {
      // A real pole, by the bilinear transform. Expressed through the biquad
      // with its second-order terms at zero, because that is genuinely what a
      // one-pole IS in this form -- not a biquad being misused.
      const double pi = 3.14159265358979323846;
      const double k = std::tan(pi * (double) freq / (double) sr_);
      const double norm = 1.0 / (k + 1.0);
      if (highpass)
        onePole_.setCoefficients(norm, -norm, 0.0, (k - 1.0) * norm, 0.0);
      else
        onePole_.setCoefficients(k * norm, k * norm, 0.0, (k - 1.0) * norm, 0.0);
    }
    return true;
  }

  /** The cascade's shape, for a design about to be written. Its state is
   *  KEPT when the shape is the one it already had -- a cutoff re-designed
   *  every block, as a plugin applies its parameters, at the same value or a
   *  moving one -- and cleared only when the sections themselves change.
   *  Every design cleared it: a filter whose cutoff was applied per block
   *  crackled (THD+N -4 dBc at 64-sample blocks, 48 kHz) and lost 3 dB of
   *  the fundamental it was passing. */
  void shape(int order, int kind) {
    const bool same = order == order_ && kind == kind_;
    order_ = order;
    kind_ = kind;
    biquads_ = butterworthBiquads(order);
    hasOnePole_ = (order & 1) != 0;
    if (!same) reset();
  }

  IirSection sections_[kMaxSections];
  IirSection onePole_;
  float sr_ = 48000.0f;
  /** An overall trim, needed only by even-order Chebyshev. One for everything
   *  else, which costs a multiply nobody will measure. */
  float gain_ = 1.0f;
  int order_ = 0, biquads_ = 0;
  int kind_ = -1; // Butterworth / Chebyshev I / II, low or high: see shape()
  bool hasOnePole_ = false;
};

/**
 * A Linkwitz-Riley crossover of any even order.
 *
 * Two Butterworths of half the order in cascade, which is the whole
 * definition: doubling the poles is what puts both bands 6 dB down at the
 * crossover instead of 3, and what makes them sum FLAT. A pair of plain
 * Butterworths leaves a 3 dB bump there, which is audible on anything with
 * energy near the corner and is the reason multiband processors use this and
 * not the obvious thing.
 *
 * effects.h has a fixed 4th-order version, which stays: it is the common case
 * and it is smaller. This is for a caller that wants to CHOOSE: 12, 24 or 48
 * dB per octave is a control a multiband plugin should be able to offer.
 *
 * ODD orders are refused. An odd-order Linkwitz-Riley does not exist: the
 * construction is two identical halves, and half of an odd number is not a
 * filter order.
 */
template <int MaxOrder = 8>
class LinkwitzRileyN {
public:
  /** The highest order of each Butterworth half: MaxOrder / 2. */
  static constexpr int kHalfMax = MaxOrder / 2;

  /** The rate in Hz for all four cascades (0 or less means 48 kHz). Set it
   *  before setCrossover(), which designs for the rate set at the time; a
   *  later rate change needs setCrossover() again. */
  void setSampleRate(float sr) {
    lowA_.setSampleRate(sr);
    lowB_.setSampleRate(sr);
    highA_.setSampleRate(sr);
    highB_.setSampleRate(sr);
  }

  /** `order` is the LINKWITZ-RILEY order: 4 means 24 dB/octave. */
  bool setCrossover(float freq, int order) {
    if (order < 2 || (order & 1) != 0 || order > MaxOrder) return false;
    const int half = order / 2;
    // TWICE, which is the definition and not an optimisation to skip. A
    // Butterworth of order N applied once is a Butterworth; applied twice its
    // poles are doubled, and that is what puts both bands 6 dB down at the
    // crossover instead of 3 and makes them sum flat.
    //
    // Designing it once was a filter half the requested order wearing the
    // requested order's name, and its bands did not sum -- they NULLED, by
    // 114 dB, which is how it was caught.
    if (!lowA_.designButterworthLowpass(freq, half)) return false;
    if (!lowB_.designButterworthLowpass(freq, half)) return false;
    if (!highA_.designButterworthHighpass(freq, half)) return false;
    if (!highB_.designButterworthHighpass(freq, half)) return false;
    // With an ODD half the two bands come out in antiphase and the sum nulls
    // instead of adding. LR2 and LR6 need the flip; LR4 and LR8 do not.
    invertHigh_ = (half & 1) != 0;
    // No reset: each cascade keeps its state while its shape stays the same
    // and clears it when the order changes (see CascadedIir::shape). Applied
    // per block, the reset here cost the band sum 31 dB of its fundamental
    // at 192 kHz.
    order_ = order;
    return true;
  }

  /** Clear all four cascades' state; the crossover is kept. */
  void reset() {
    lowA_.reset();
    lowB_.reset();
    highA_.reset();
    highB_.reset();
  }

  /** The Linkwitz-Riley order of the last accepted setCrossover(); 0 before
   *  one. */
  int order() const { return order_; }

  /** [audio-thread] One sample into the low and the high band, whose sum is
   *  flat in level at every frequency. Before a setCrossover() both are the
   *  input unchanged. No allocation. */
  void process(float x, float& lowOut, float& highOut) {
    lowOut = lowB_.process(lowA_.process(x));
    highOut = highB_.process(highA_.process(x));
    if (invertHigh_) highOut = -highOut;
  }

private:
  CascadedIir<kHalfMax> lowA_, lowB_, highA_, highB_;
  int order_ = 0;
  bool invertHigh_ = false;
};

} // namespace sonore
