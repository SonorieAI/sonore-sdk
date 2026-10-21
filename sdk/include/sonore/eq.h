// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: the parametric equaliser, as a machine.
//
// Biquad in dsp.h is one band. Every EQ plugin is N of them with three
// things around them that a model rewrites every time and gets wrong once:
// the band bookkeeping, the click-free response to a knob, and the curve
// the faceplate draws. This is those, once -- and the bands themselves are
// better than the cookbook's.
//
// MATCHED, NOT BILINEAR. The RBJ cookbook maps the analogue prototype with
// the bilinear transform, which pins the response at Nyquist: a bell at
// 16 kHz sampled at 44.1 kHz is squeezed against the top of the band and
// comes out narrower and lopsided -- the "cramping" every analogue-matched
// EQ advertises fixing. Vicanek's "Matched Second Order Digital Filters"
// (2016) fixes it the way the name says: the POLES are mapped by the matched
// Z-transform (e^{sT}, so the resonance sits exactly where the analogue one
// does) and the ZEROS are chosen so the digital magnitude equals the
// analogue prototype's at three frequencies -- DC, the band's own frequency
// and Nyquist. That is a three-point linear solve in Vicanek's phi = sin²(w/2)
// form, worked out below for every band type from the prototype's
// magnitude, rather than his per-type closed forms copied in. The test
// measures a 16 kHz bell at 44.1 kHz against the prototype: matched agrees
// to a third of a decibel where the cookbook is off by more than one.
//
// ZIPPER. Frequency, gain and Q are smoothed (log for frequency, since the
// ear hears ratios) and the coefficients recomputed every 16 samples from
// the smoothed values; the transposed-direct-form state carries across, so
// a swept knob is a sweep and not a staircase. The test drags a bell's gain
// 12 dB in 100 ms across a tone and asks for no step the tone did not make.
//
// LINEAR PHASE. On request the same curve is realised as a symmetric FIR
// (the composite magnitude, inverse-Fourier-summed and Kaiser-windowed),
// with the latency half the kernel and reported. Designed offline, because
// a 2047-tap design is milliseconds of work: prepare() or a message thread.
//
// Included by dsp.h.
#pragma once
#ifndef SONORE_DSP_H_INCLUDED
#error "eq.h is part of dsp.h and needs what it includes first: include <sonore/dsp.h> instead."
#endif
#include <cmath>
#include <vector>
#include "audio.h"
#include "fir.h"

namespace sonore {

namespace eqdetail {

enum class Shape { Bell, LowShelf, HighShelf, Lowpass, Highpass, Notch, Bandpass };

/** The analogue prototype's |H|² at x = w/w0, RBJ's forms with A = sqrt(G). */
inline double prototypeMag2(Shape shape, double x, double q, double gainLinear) {
  const double x2 = x * x;
  const double a = std::sqrt(gainLinear);
  switch (shape) {
    case Shape::Lowpass: return 1.0 / ((1.0 - x2) * (1.0 - x2) + (x / q) * (x / q));
    case Shape::Highpass: return x2 * x2 / ((1.0 - x2) * (1.0 - x2) + (x / q) * (x / q));
    case Shape::Bandpass: return (x / q) * (x / q) / ((1.0 - x2) * (1.0 - x2) + (x / q) * (x / q));
    case Shape::Notch: return (1.0 - x2) * (1.0 - x2) / ((1.0 - x2) * (1.0 - x2) + (x / q) * (x / q));
    case Shape::Bell: {
      const double n = (1.0 - x2) * (1.0 - x2) + (a * x / q) * (a * x / q);
      const double d = (1.0 - x2) * (1.0 - x2) + (x / (a * q)) * (x / (a * q));
      return n / d;
    }
    case Shape::LowShelf: {
      const double n = (a - x2) * (a - x2) + (std::sqrt(a) * x / q) * (std::sqrt(a) * x / q);
      const double d = (1.0 - a * x2) * (1.0 - a * x2) + (std::sqrt(a) * x / q) * (std::sqrt(a) * x / q);
      return a * a * n / d;
    }
    case Shape::HighShelf: {
      const double n = (1.0 - a * x2) * (1.0 - a * x2) + (std::sqrt(a) * x / q) * (std::sqrt(a) * x / q);
      const double d = (a - x2) * (a - x2) + (std::sqrt(a) * x / q) * (std::sqrt(a) * x / q);
      return a * a * n / d;
    }
  }
  return 1.0;
}

/** Where the prototype's poles sit relative to w0, and their Q: the shelves
 *  move their pole pair by sqrt(A), the bell changes its Q by A. */
inline void prototypePoles(Shape shape, double q, double gainLinear, double& wScale, double& qPole) {
  const double a = std::sqrt(gainLinear);
  wScale = 1.0;
  qPole = q;
  if (shape == Shape::Bell) qPole = q * a;
  else if (shape == Shape::LowShelf) wScale = 1.0 / std::sqrt(a);
  else if (shape == Shape::HighShelf) wScale = std::sqrt(a);
}

/**
 * Vicanek's matched biquad for any shape. Returns false where the match has
 * no real solution -- a band pushed hard against Nyquist, every highpass and
 * notch (a three-point fit cannot place their double zero), and some steep
 * shelves -- and designBand() below falls back to the cookbook there. Every
 * EQ here reaches it through designBand.
 */
inline bool matchedCoefficients(Shape shape, double hz, double q, double gainDb, double fs, double c[5]) {
  const double pi = 3.14159265358979323846;
  const double gain = std::pow(10.0, gainDb / 20.0);
  const double w0 = 2.0 * pi * hz / fs;
  if (w0 <= 0.0 || w0 >= 0.95 * pi) return false;
  double wScale, qPole;
  prototypePoles(shape, q, gain, wScale, qPole);
  // Poles by the matched Z-transform of the prototype's pole pair.
  const double wp = w0 * wScale;
  const double zeta = 1.0 / (2.0 * qPole);
  double A1, A2;
  if (zeta < 1.0) {
    A1 = -2.0 * std::exp(-zeta * wp) * std::cos(wp * std::sqrt(1.0 - zeta * zeta));
  } else {
    A1 = -2.0 * std::exp(-zeta * wp) * std::cosh(wp * std::sqrt(zeta * zeta - 1.0));
  }
  A2 = std::exp(-2.0 * zeta * wp);
  // The denominator's magnitude squared in phi = sin²(w/2):
  //   |1 + a1 z^-1 + a2 z^-2|² = D0 + D1 phi + D2 phi².
  const double D0 = (1.0 + A1 + A2) * (1.0 + A1 + A2);
  const double D1 = -4.0 * (A1 + A1 * A2 + 4.0 * A2);
  const double D2 = 16.0 * A2;
  // Three targets: the prototype's magnitude at DC, at w0 and at Nyquist,
  // times the denominator there, give the numerator's N0 + N1 phi + N2 phi².
  const double phi0 = std::sin(0.5 * w0) * std::sin(0.5 * w0);
  const double tDc = prototypeMag2(shape, 0.0, q, gain);
  const double tMid = prototypeMag2(shape, 1.0, q, gain);
  const double tNy = prototypeMag2(shape, pi / w0, q, gain);
  const double N0 = tDc * D0;
  const double nNy = tNy * (D0 + D1 + D2);
  const double nMid = tMid * (D0 + D1 * phi0 + D2 * phi0 * phi0);
  const double denom = phi0 * phi0 - phi0;
  if (std::fabs(denom) < 1e-9) return false;
  const double N2 = ((nMid - N0) - phi0 * (nNy - N0)) / denom;
  const double N1 = (nNy - N0) - N2;
  // Back to b0, b1, b2: b0 + b1 + b2 = sqrt(N0), b0 - b1 + b2 = sqrt(N(1)),
  // 16 b0 b2 = N2.
  if (N0 < 0.0 || N0 + N1 + N2 < 0.0) return false;
  const double sDc = std::sqrt(N0), sNy = std::sqrt(N0 + N1 + N2);
  const double W = 0.5 * (sDc + sNy);
  const double disc = W * W - N2 / 4.0;
  if (disc < 0.0) return false;
  const double B0 = 0.5 * (W + std::sqrt(disc));
  const double B2 = W - B0;
  const double B1 = 0.5 * (sDc - sNy);
  c[0] = B0; c[1] = B1; c[2] = B2; c[3] = A1; c[4] = A2;
  return true;
}

/** The same, rounded to float, for a caller holding a Biquad. */
inline bool matched(Shape shape, double hz, double q, double gainDb, double fs, float* b0, float* b1, float* b2,
                    float* a1, float* a2) {
  double c[5];
  if (!matchedCoefficients(shape, hz, q, gainDb, fs, c)) return false;
  *b0 = (float) c[0]; *b1 = (float) c[1]; *b2 = (float) c[2]; *a1 = (float) c[3]; *a2 = (float) c[4];
  return true;
}

/** The cookbook (RBJ) section for a shape, normalised, in double: Biquad's
 *  own formulas, whose float cosine of a 20 Hz corner at 192 kHz is
 *  1 - 2e-7 and leaves nothing of the corner to compute with. 1 - cos is
 *  taken as 2 sin²(w0/2), which keeps its digits at any corner. */
inline void cookbookCoefficients(Shape shape, double hz, double q, double gainDb, double fs, double c[5]) {
  const double pi = 3.14159265358979323846;
  const double w0 = 2.0 * pi * hz / fs;
  const double cw = std::cos(w0), sw = std::sin(w0), sh = std::sin(0.5 * w0);
  const double oneMinusCos = 2.0 * sh * sh;
  const double alpha = sw / (2.0 * q);
  const double A = std::pow(10.0, gainDb / 40.0);
  double b0 = 1.0, b1 = 0.0, b2 = 0.0, a0 = 1.0, a1 = 0.0, a2 = 0.0;
  switch (shape) {
    case Shape::Bell:
      b0 = 1.0 + alpha * A; b1 = -2.0 * cw; b2 = 1.0 - alpha * A;
      a0 = 1.0 + alpha / A; a1 = -2.0 * cw; a2 = 1.0 - alpha / A;
      break;
    case Shape::LowShelf:
    case Shape::HighShelf: {
      const double tsa = 2.0 * std::sqrt(A) * alpha, Ap1 = A + 1.0, Am1 = A - 1.0;
      if (shape == Shape::LowShelf) {
        a0 = Ap1 + Am1 * cw + tsa; a1 = -2.0 * (Am1 + Ap1 * cw); a2 = Ap1 + Am1 * cw - tsa;
        b0 = A * (Ap1 - Am1 * cw + tsa); b1 = 2.0 * A * (Am1 - Ap1 * cw); b2 = A * (Ap1 - Am1 * cw - tsa);
      } else {
        a0 = Ap1 - Am1 * cw + tsa; a1 = 2.0 * (Am1 - Ap1 * cw); a2 = Ap1 - Am1 * cw - tsa;
        b0 = A * (Ap1 + Am1 * cw + tsa); b1 = -2.0 * A * (Am1 + Ap1 * cw); b2 = A * (Ap1 + Am1 * cw - tsa);
      }
      break;
    }
    case Shape::Lowpass:
      b0 = 0.5 * oneMinusCos; b1 = oneMinusCos; b2 = 0.5 * oneMinusCos;
      a0 = 1.0 + alpha; a1 = -2.0 * cw; a2 = 1.0 - alpha;
      break;
    case Shape::Highpass:
      b0 = 0.5 * (1.0 + cw); b1 = -(1.0 + cw); b2 = 0.5 * (1.0 + cw);
      a0 = 1.0 + alpha; a1 = -2.0 * cw; a2 = 1.0 - alpha;
      break;
    case Shape::Notch:
      b0 = 1.0; b1 = -2.0 * cw; b2 = 1.0;
      a0 = 1.0 + alpha; a1 = -2.0 * cw; a2 = 1.0 - alpha;
      break;
    case Shape::Bandpass:
      b0 = alpha; b1 = 0.0; b2 = -alpha;
      a0 = 1.0 + alpha; a1 = -2.0 * cw; a2 = 1.0 - alpha;
      break;
  }
  c[0] = b0 / a0; c[1] = b1 / a0; c[2] = b2 / a0; c[3] = a1 / a0; c[4] = a2 / a0;
}

/** |H|² of a biquad at phi = sin²(w/2): the same polynomial matched() fits,
 *  so one sine per frequency serves every candidate design. */
inline double biquadMag2(double b0, double b1, double b2, double a1, double a2, double phi) {
  const double n = (b0 + b1 + b2) * (b0 + b1 + b2) - 4.0 * phi * (b0 * b1 + b1 * b2 + 4.0 * b0 * b2) +
                   16.0 * b0 * b2 * phi * phi;
  const double d = (1.0 + a1 + a2) * (1.0 + a1 + a2) - 4.0 * phi * (a1 + a1 * a2 + 4.0 * a2) +
                   16.0 * a2 * phi * phi;
  return d > 0.0 ? n / d : 0.0;
}

/** How far each of two designs strays from the analogue prototype: the worst
 *  power ratio, either way up, at a few frequencies around w0 and below 0.9 of
 *  Nyquist. Ratios rather than dB, so no logarithm in a function that runs
 *  every 16 samples while a band moves. */
inline void worstMisses(Shape shape, double w0, double q, double gain, const double m[5],
                        const double c[5], double& missM, double& missC) {
  static const double kRatios[] = {0.125, 0.25, 0.5, 0.71, 1.0, 1.41, 2.0, 4.0};
  const double pi = 3.14159265358979323846;
  missM = missC = 1.0;
  for (double r : kRatios) {
    const double w = r * w0;
    if (w >= 0.9 * pi) break;
    const double s = std::sin(0.5 * w);
    const double t = prototypeMag2(shape, r, q, gain);
    if (!(t > 1e-12)) continue;
    const double hm = biquadMag2(m[0], m[1], m[2], m[3], m[4], s * s);
    const double hc = biquadMag2(c[0], c[1], c[2], c[3], c[4], s * s);
    const double em = hm > 1e-300 ? (hm > t ? hm / t : t / hm) : 1e300;
    const double ec = hc > 1e-300 ? (hc > t ? hc / t : t / hc) : 1e300;
    if (em > missM) missM = em;
    if (ec > missC) missC = ec;
  }
}

/**
 * One band into one biquad: the matched design where it has a solution AND
 * is the closer of the two, the cookbook otherwise, with the frequency and Q
 * held where both stay finite.
 *
 * A solution is not the same as a good one. The three-point fit lands
 * exactly on DC, w0 and Nyquist and can wander between them: a 15 kHz +12 dB
 * high shelf at 44.1 kHz read -5.45 dB at 8 kHz where the analogue shelf is
 * +1.1 (the cookbook: +0.2), a Q 2 shelf strayed 3 dB at 5 kHz, and a -12 dB
 * Q 2 bell at 18 kHz missed by 16 dB. For the gain shapes both designs are
 * measured against the prototype and the closer one is used -- which is the
 * matched design across almost all of the band, as before. ParametricEq and DynamicEq share it, so the two cannot disagree
 * about what a band IS.
 *
 * DynamicEq used to have its own copy without the fallback, installing a
 * UNITY filter whenever matched() said no -- and matched() says no to every
 * highpass and notch (a three-point fit cannot place their double zero) and
 * to some steep shelves, so those bands silently did nothing. It had no clamp
 * either: Q = 0 made the matched design return NaN coefficients as a success.
 */
inline void designCoefficients(Shape shape, float hz, float q, float gainDb, float sr, bool useMatched,
                               double c[5]) {
  const double f = (double) clampf(hz, 10.0f, sr * 0.45f);
  const double qq = q < 0.05f ? 0.05 : (double) q;
  double m[5];
  if (useMatched && matchedCoefficients(shape, f, qq, gainDb, sr, m)) {
    if (shape == Shape::Bell || shape == Shape::LowShelf || shape == Shape::HighShelf) {
      double k[5];
      cookbookCoefficients(shape, f, qq, gainDb, sr, k);
      double missM = 1.0, missC = 1.0;
      worstMisses(shape, 2.0 * 3.14159265358979323846 * f / (double) sr, qq,
                  std::pow(10.0, (double) gainDb / 20.0), m, k, missM, missC);
      if (missC < missM) {
        for (int i = 0; i < 5; ++i) c[i] = k[i];
        return;
      }
    }
    for (int i = 0; i < 5; ++i) c[i] = m[i];
    return;
  }
  cookbookCoefficients(shape, f, qq, gainDb, sr, c);
}

/** Into a double-precision section: what every EQ here runs. A float
 *  section cannot hold a low corner at a high rate -- a 20 Hz Q 4 +12 dB
 *  bell at 192 kHz measured +8.08 dB, its poles 8e-5 inside the unit circle
 *  and float resolving that to a few percent. */
inline void designBand(IirSection& filter, Shape shape, float hz, float q, float gainDb, float sr,
                       bool useMatched = true) {
  double c[5];
  designCoefficients(shape, hz, q, gainDb, sr, useMatched, c);
  filter.setCoefficients(c[0], c[1], c[2], c[3], c[4]);
}

/** ...and into a float Biquad, for a caller that holds one. */
inline void designBand(Biquad& filter, Shape shape, float hz, float q, float gainDb, float sr,
                       bool useMatched = true) {
  double c[5];
  designCoefficients(shape, hz, q, gainDb, sr, useMatched, c);
  filter.setCoefficients((float) c[0], (float) c[1], (float) c[2], (float) c[3], (float) c[4]);
}

} // namespace eqdetail

/**
 * N bands, each a matched biquad, smoothed, with the curve for the faceplate.
 * process() is per sample; the coefficients refresh every 16 samples while a
 * band's controls are still moving.
 */
template <int Bands = 8>
class ParametricEq {
  static_assert(Bands >= 1 && Bands <= 32, "1..32 bands");

public:
  /** A band's response: Bell, LowShelf, HighShelf (these three use the
   *  band's gain), Lowpass, Highpass (12 dB per octave, Q shaping the
   *  corner), Notch and Bandpass. */
  using Shape = eqdetail::Shape;
  /** One band: its shape, centre or corner frequency in Hz, gain in dB (used
   *  by the bell and the shelves; the pass, notch and bandpass shapes ignore
   *  it) and Q. A band does nothing until `enabled`. */
  struct Band {
    Shape shape = Shape::Bell; ///< the response; changing it redesigns the band without a glide
    /** Centre (bell, notch, bandpass) or corner (shelves, passes) frequency in
     *  Hz, held to 10 Hz .. 0.45 of the rate when the band is designed. */
    float hz = 1000.0f, gainDb = 0.0f, q = 0.7071f; ///< Q, 0.05 at the least; 0.7071 is a Butterworth pass
    bool enabled = false; ///< off by default; a disabled band is skipped by process() and magnitudeDb()
    /** \var gainDb
     *  Gain in dB, for the bell and the shelves; the other shapes ignore it. */
  };

  /** The rate in Hz (1 or less is taken as 48000). Every band lands on its
   *  settings without a glide and is designed for the new rate; the filters'
   *  state is kept, reset() clears it. In linear-phase mode the kernel is
   *  designed again for the new rate, as setLinearPhase() designs it --
   *  allocating and costing milliseconds, so from prepare(), as a rate
   *  change always is. It used to keep the old rate's kernel, which plays
   *  every band at the wrong frequency: a 1 kHz bell designed at 48 kHz sat
   *  at 2 kHz at 96. */
  void setSampleRate(float sr) {
    sr_ = sr > 1.0f ? sr : 48000.0f;
    for (int i = 0; i < Bands; ++i) {
      filter_[i].setSampleRate(sr_);
      // The smoothers step once per coefficient update, not once per sample,
      // so they are set up at the UPDATE rate -- the first version set them
      // up at the sample rate and its 20 ms was 320.
      hzSmooth_[i].setup(sr_ / (float) kUpdateEvery, kSmoothMs);
      gainSmooth_[i].setup(sr_ / (float) kUpdateEvery, kSmoothMs);
      qSmooth_[i].setup(sr_ / (float) kUpdateEvery, kSmoothMs);
      snap(i);
      design(i, band_[i].hz, band_[i].gainDb, band_[i].q);
    }
    counter_ = 0;
    if (linearTaps_ > 0) setLinearPhase(linearTaps_);
  }
  /** Set a band. The change is smoothed in; with `snapNow` it lands at once
   *  and the coefficients are designed here, so magnitudeDb() is current. */
  void setBand(int i, const Band& band, bool snapNow = false) {
    if (i < 0 || i >= Bands) return;
    // The same band again is not a movement. A plugin sets every band from
    // its parameters once a block, and each call marked the band as moving:
    // an 8-band EQ that nobody touched redesigned every band every 16
    // samples, 108 ns a sample against 28 once settled.
    if (!snapNow && band.shape == band_[i].shape && band.hz == band_[i].hz &&
        band.gainDb == band_[i].gainDb && band.q == band_[i].q && band.enabled == band_[i].enabled)
      return;
    band_[i] = band;
    if (snapNow) {
      snap(i);
      design(i, band.hz, band.gainDb, band.q);
      moving_[i] = false;
      return;
    }
    moving_[i] = true;
  }
  /** Band `i` as last set (the target, not the glide's position). An index
   *  outside 0..Bands-1 reads the nearest band. */
  const Band& band(int i) const { return band_[i < 0 ? 0 : (i >= Bands ? Bands - 1 : i)]; }
  /** Vicanek's matched design (default) or the cookbook's bilinear one. */
  void setMatched(bool on) {
    matched_ = on;
    for (int i = 0; i < Bands; ++i) moving_[i] = true;
  }
  /** Clears every band's filter state and the linear-phase FIR's history.
   *  The settings, and where each glide has got to, are kept. */
  void reset() {
    for (int i = 0; i < Bands; ++i) filter_[i].reset();
    fir_.reset();
  }

  /** One sample through the enabled bands in order, or through the
   *  linear-phase FIR when that is on; bands still gliding are redesigned
   *  every 16 samples. No allocation. The FIR plays the curve as it was when
   *  setLinearPhase() ran: band changes since move magnitudeDb() but are not
   *  heard until it is called again. */
  inline float process(float x) {
    if (++counter_ >= kUpdateEvery) {
      counter_ = 0;
      for (int i = 0; i < Bands; ++i)
        if (moving_[i]) update(i);
    }
    if (linearTaps_ > 0) return fir_.process(x);
    for (int i = 0; i < Bands; ++i)
      if (band_[i].enabled) x = filter_[i].process(x);
    return x;
  }

  /** The curve as it currently is, in dB, for drawing. */
  float magnitudeDb(float hz) const {
    float sum = 0.0f;
    for (int i = 0; i < Bands; ++i)
      if (band_[i].enabled) sum += filter_[i].magnitudeDb(hz);
    return sum;
  }

  /** Realise the current curve as a linear-phase FIR of `taps` (odd, up to
   *  4095; 0 returns to the minimum-phase biquads). OFFLINE: allocates and
   *  costs milliseconds; call from prepare() or a message thread, never from
   *  process(). The latency is latencySamples(). */
  void setLinearPhase(int taps) {
    linearTaps_ = taps <= 0 ? 0 : (taps | 1);
    if (linearTaps_ > 4095) linearTaps_ = 4095;
    if (linearTaps_ == 0) return;
    for (int i = 0; i < Bands; ++i) {
      snap(i);
      design(i, band_[i].hz, band_[i].gainDb, band_[i].q);
      moving_[i] = false;
    }
    // h[n] = (1/pi) int_0^pi |H(w)| cos(w (n - c)) dw over a grid fine enough
    // for the sharpest band, then a Kaiser window, then the window's small
    // gain error trimmed out.
    const int n = linearTaps_, centre = n / 2, grid = 2048;
    const double pi = 3.14159265358979323846;
    const double dw = pi / grid;
    // The target curve ONCE per grid point. It was evaluated inside the tap
    // loop -- every band's response at 2048 points for each of up to 4095
    // taps -- which is what made this take 1.3 s for four bands rather than
    // the milliseconds its comment promised.
    // On the heap: this is the offline path, and 16 KB is a quarter of the
    // wasm stack.
    std::vector<double> target((size_t) grid);
    int reference = 0;
    for (int g = 0; g < grid; ++g) {
      const double w = ((double) g + 0.5) * dw;
      target[g] = std::pow(10.0, (double) magnitudeDb((float) (w / (2.0 * pi) * (double) sr_)) / 20.0);
      if (target[g] > target[reference]) reference = g;
    }
    float* h = kernel_;
    for (int k = 0; k < n; ++k) {
      double acc = 0.0;
      for (int g = 0; g < grid; ++g)
        acc += target[g] * std::cos(((double) g + 0.5) * dw * (double) (k - centre));
      const double r = 2.0 * (double) (k - centre) / (double) (n - 1);
      h[k] = (float) (acc * dw / pi * special::kaiser(r, 6.0));
    }
    // The trim is taken where the curve is LOUDEST, not at DC. It used to be
    // DC, and a curve with a highpass or bandpass band in it is -300 dB there:
    // the whole kernel was scaled by about 1e-15 and the EQ fell silent, while
    // magnitudeDb() went on drawing the curve it was supposed to be playing.
    const double wRef = ((double) reference + 0.5) * dw;
    double realised = 0.0;
    for (int k = 0; k < n; ++k) realised += (double) h[k] * std::cos(wRef * (double) (k - centre));
    if (std::fabs(realised) > 1e-12)
      for (int k = 0; k < n; ++k) h[k] = (float) ((double) h[k] * target[reference] / realised);
    fir_.setCoefficients(h, (size_t) n);
  }
  /** Samples of latency to report: half the linear-phase kernel, or 0 while
   *  the minimum-phase biquads run. */
  int latencySamples() const { return linearTaps_ > 0 ? linearTaps_ / 2 : 0; }

private:
  static constexpr int kUpdateEvery = 16;
  static constexpr float kSmoothMs = 20.0f;

  void snap(int i) {
    hzSmooth_[i].snap(band_[i].hz);
    gainSmooth_[i].snap(band_[i].gainDb);
    qSmooth_[i].snap(band_[i].q);
  }
  void update(int i) {
    const float hz = hzSmooth_[i].next(band_[i].hz);
    const float gainDb = gainSmooth_[i].next(band_[i].gainDb);
    const float q = qSmooth_[i].next(band_[i].q);
    const bool settled = std::fabs(hz - band_[i].hz) < 1e-3f * band_[i].hz && std::fabs(gainDb - band_[i].gainDb) < 1e-3f &&
                         std::fabs(q - band_[i].q) < 1e-4f;
    // Once settled, the target is designed and the glide's last step is not:
    // it was designed first and thrown away, two designs where one lands.
    // The smoothers land with it. Left where "settled" caught them -- up to
    // a thousandth off -- the next glide set out from a curve that was not
    // the one playing, and from a different place depending on how many
    // times the band had been set since.
    if (settled) {
      snap(i);
      design(i, band_[i].hz, band_[i].gainDb, band_[i].q);
      moving_[i] = false;
      return;
    }
    design(i, hz, gainDb, q);
  }
  void design(int i, float hz, float gainDb, float q) {
    eqdetail::designBand(filter_[i], band_[i].shape, hz, q, gainDb, sr_, matched_);
  }

  IirSection filter_[Bands];
  Band band_[Bands];
  LogSmooth hzSmooth_[Bands];
  Smooth gainSmooth_[Bands], qSmooth_[Bands];
  bool moving_[Bands] = {};
  FirFilter fir_;
  float kernel_[4095]{};
  float sr_ = 48000.0f;
  int counter_ = 0, linearTaps_ = 0;
  bool matched_ = true;
};

} // namespace sonore
