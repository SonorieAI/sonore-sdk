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
  double q = 0.70710678;
  double frequencyScale = 1.0;
};

/** The ripple as an amplitude ratio: 1 dB of ripple is eps = 0.5088. */
inline double chebyshevEpsilon(double rippleDb) {
  if (rippleDb <= 0.0) rippleDb = 0.01;
  return std::sqrt(std::pow(10.0, rippleDb / 10.0) - 1.0);
}

inline ChebyshevSection chebyshevSectionFor(int order, int section, double rippleDb) {
  ChebyshevSection out;
  if (order < 2) return out;
  const double pi = 3.14159265358979323846;
  const double eps = chebyshevEpsilon(rippleDb);
  // asinh, spelled out: MSVC has std::asinh but the identity is one line and
  // makes the geometry visible -- v0 is how far the ellipse is squashed.
  const double v0 = std::log(1.0 / eps + std::sqrt(1.0 / (eps * eps) + 1.0)) / (double) order;
  const double sinhV = std::sinh(v0), coshV = std::cosh(v0);

  // Same angles as Butterworth for even orders, and the odd case again needs
  // its own spacing because one pole is real.
  const double theta = (order % 2 == 0)
                           ? pi * (double) (2 * section + 1) / (double) (2 * order)
                           : pi * (double) (section + 1) / (double) order;
  const double re = -sinhV * std::sin(theta);
  const double im = coshV * std::cos(theta);
  const double magnitude = std::sqrt(re * re + im * im);
  out.frequencyScale = magnitude;
  out.q = magnitude / (2.0 * std::fabs(re));
  return out;
}

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
 */
inline void bilinearSection(double b2, double b1, double b0, double a2, double a1, double a0,
                            double cutoffHz, double sampleRate, Biquad& out) {
  const double pi = 3.14159265358979323846;
  const double c = 1.0 / std::tan(pi * cutoffHz / sampleRate);
  const double cc = c * c;

  const double nb0 = b2 * cc + b1 * c + b0;
  const double nb1 = 2.0 * (b0 - b2 * cc);
  const double nb2 = b2 * cc - b1 * c + b0;
  const double na0 = a2 * cc + a1 * c + a0;
  const double na1 = 2.0 * (a0 - a2 * cc);
  const double na2 = a2 * cc - a1 * c + a0;

  if (std::fabs(na0) < 1e-30) return;
  out.setCoefficients((float) (nb0 / na0), (float) (nb1 / na0), (float) (nb2 / na0),
                      (float) (na1 / na0), (float) (na2 / na0));
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
  double poleMagnitude = 1.0;
  /** Zero, on the imaginary axis, at this distance from the origin. */
  double zero = 0.0;
};

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

  const double theta = (order % 2 == 0)
                           ? pi * (double) (2 * section + 1) / (double) (2 * order)
                           : pi * (double) (section + 1) / (double) order;

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
  static constexpr int kMaxSections = (MaxOrder + 1) / 2;

  void setSampleRate(float sr) {
    sr_ = sr > 0.0f ? sr : 48000.0f;
    for (int i = 0; i < kMaxSections; ++i) sections_[i].setSampleRate(sr_);
    onePole_.setSampleRate(sr_);
  }

  void reset() {
    for (int i = 0; i < kMaxSections; ++i) sections_[i].reset();
    onePole_.reset();
  }

  int order() const { return order_; }

  /** Maximally flat lowpass. Returns false, and leaves the filter as it was,
   *  if the order does not fit or is not a positive number. */
  bool designButterworthLowpass(float freq, int order) { return design(freq, order, false); }
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
  bool designChebyshev2Highpass(float freq, int order, float stopbandDb) {
    return designChebyshev2(freq, order, stopbandDb, true);
  }

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

    order_ = order;
    biquads_ = butterworthBiquads(order);
    hasOnePole_ = (order & 1) != 0;
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
      const double pr = 1.0 / std::sinh(v0);
      if (highpass)
        bilinearSection(1.0, 0.0, 0.0, 1.0, pr, 0.0, (double) freq, (double) sr_, onePole_);
      else
        bilinearSection(0.0, 0.0, pr, 0.0, 1.0, pr, (double) freq, (double) sr_, onePole_);
    }
    reset();
    return true;
  }

  bool designChebyshev(float freq, int order, float rippleDb, bool highpass) {
    if (order < 1 || order > MaxOrder || !(rippleDb > 0.0f)) return false;
    const float nyquist = sr_ * 0.5f;
    if (!(freq > 0.0f) || freq >= nyquist) return false;

    const double eps = chebyshevEpsilon((double) rippleDb);
    const double v0 =
        std::log(1.0 / eps + std::sqrt(1.0 / (eps * eps) + 1.0)) / (double) order;

    // Every section has to fit under Nyquist BEFORE anything is changed, or a
    // refusal leaves the filter half rebuilt. The outermost pole pair sits
    // furthest from the cutoff and is the one that fails first.
    const int biquads = butterworthBiquads(order);
    for (int i = 0; i < biquads; ++i) {
      const ChebyshevSection section = chebyshevSectionFor(order, i, (double) rippleDb);
      const double sectionFreq = highpass ? (double) freq / section.frequencyScale
                                          : (double) freq * section.frequencyScale;
      if (!(sectionFreq > 0.0) || sectionFreq >= (double) nyquist) return false;
    }

    order_ = order;
    biquads_ = biquads;
    hasOnePole_ = (order & 1) != 0;

    for (int i = 0; i < biquads_; ++i) {
      const ChebyshevSection section = chebyshevSectionFor(order, i, (double) rippleDb);
      const float sectionFreq = (float) (highpass ? (double) freq / section.frequencyScale
                                                  : (double) freq * section.frequencyScale);
      if (highpass)
        sections_[i].highpass(sectionFreq, (float) section.q);
      else
        sections_[i].lowpass(sectionFreq, (float) section.q);
    }

    if (hasOnePole_) {
      // The real pole, at sinh(v0) times the cutoff.
      const double pi = 3.14159265358979323846;
      const double scale = std::sinh(v0);
      const double poleFreq = highpass ? (double) freq / scale : (double) freq * scale;
      const double k = std::tan(pi * poleFreq / (double) sr_);
      const double norm = 1.0 / (k + 1.0);
      if (highpass)
        onePole_.setCoefficients((float) norm, (float) -norm, 0.0f, (float) ((k - 1.0) * norm),
                                 0.0f);
      else
        onePole_.setCoefficients((float) (k * norm), (float) (k * norm), 0.0f,
                                 (float) ((k - 1.0) * norm), 0.0f);
    }

    // EVEN orders need a gain trim and odd orders do not, which is the part
    // that looks like a mistake until you draw it. Each section is normalised
    // to unity at DC; cascading them puts DC at 0 dB, and a Chebyshev of even
    // order is supposed to START at -rippleDb and ripple UP to 0. Without
    // this the whole passband sits `rippleDb` too high and the filter is
    // quietly louder than it was asked to be.
    gain_ = (order % 2 == 0) ? (float) (1.0 / std::sqrt(1.0 + eps * eps)) : 1.0f;
    reset();
    return true;
  }

  bool design(float freq, int order, bool highpass) {
    if (order < 1 || order > MaxOrder) return false;
    const float nyquist = sr_ * 0.5f;
    if (!(freq > 0.0f) || freq >= nyquist) return false;

    order_ = order;
    biquads_ = butterworthBiquads(order);
    hasOnePole_ = (order & 1) != 0;

    for (int i = 0; i < biquads_; ++i) {
      const float q = (float) butterworthQ(order, i);
      if (highpass)
        sections_[i].highpass(freq, q);
      else
        sections_[i].lowpass(freq, q);
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
        onePole_.setCoefficients((float) norm, (float) -norm, 0.0f, (float) ((k - 1.0) * norm),
                                 0.0f);
      else
        onePole_.setCoefficients((float) (k * norm), (float) (k * norm), 0.0f,
                                 (float) ((k - 1.0) * norm), 0.0f);
    }
    reset();
    return true;
  }

  Biquad sections_[kMaxSections];
  Biquad onePole_;
  float sr_ = 48000.0f;
  /** An overall trim, needed only by even-order Chebyshev. One for everything
   *  else, which costs a multiply nobody will measure. */
  float gain_ = 1.0f;
  int order_ = 0, biquads_ = 0;
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
  static constexpr int kHalfMax = MaxOrder / 2;

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
    order_ = order;
    reset();
    return true;
  }

  void reset() {
    lowA_.reset();
    lowB_.reset();
    highA_.reset();
    highB_.reset();
  }

  int order() const { return order_; }

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
