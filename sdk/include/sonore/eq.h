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
#include <cmath>
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
 * no real solution (a band pushed hard against Nyquist), and the caller
 * falls back to the cookbook there.
 */
inline bool matched(Shape shape, double hz, double q, double gainDb, double fs, float* b0, float* b1, float* b2,
                    float* a1, float* a2) {
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
  *b0 = (float) B0; *b1 = (float) B1; *b2 = (float) B2; *a1 = (float) A1; *a2 = (float) A2;
  return true;
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
  using Shape = eqdetail::Shape;
  struct Band {
    Shape shape = Shape::Bell;
    float hz = 1000.0f, gainDb = 0.0f, q = 0.7071f;
    bool enabled = false;
  };

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
  }
  /** Set a band. The change is smoothed in; with `snapNow` it lands at once
   *  and the coefficients are designed here, so magnitudeDb() is current. */
  void setBand(int i, const Band& band, bool snapNow = false) {
    if (i < 0 || i >= Bands) return;
    band_[i] = band;
    if (snapNow) {
      snap(i);
      design(i, band.hz, band.gainDb, band.q);
      moving_[i] = false;
      return;
    }
    moving_[i] = true;
  }
  const Band& band(int i) const { return band_[i < 0 ? 0 : (i >= Bands ? Bands - 1 : i)]; }
  /** Vicanek's matched design (default) or the cookbook's bilinear one. */
  void setMatched(bool on) {
    matched_ = on;
    for (int i = 0; i < Bands; ++i) moving_[i] = true;
  }
  void reset() {
    for (int i = 0; i < Bands; ++i) filter_[i].reset();
    fir_.reset();
  }

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
    // for the sharpest band, then a Kaiser window, then unity at DC.
    const int n = linearTaps_, centre = n / 2, grid = 2048;
    const double pi = 3.14159265358979323846;
    const double dw = pi / grid;
    float* h = kernel_;
    double sum = 0.0;
    for (int k = 0; k < n; ++k) {
      double acc = 0.0;
      for (int g = 0; g < grid; ++g) {
        const double w = ((double) g + 0.5) * dw;
        const float hz = (float) (w / (2.0 * pi) * (double) sr_);
        acc += std::pow(10.0, (double) magnitudeDb(hz) / 20.0) * std::cos(w * (double) (k - centre));
      }
      const double r = 2.0 * (double) (k - centre) / (double) (n - 1);
      h[k] = (float) (acc * dw / pi * special::kaiser(r, 6.0));
      sum += h[k];
    }
    const double dc = std::pow(10.0, (double) magnitudeDb(0.0f) / 20.0);
    if (std::fabs(sum) > 1e-9) for (int k = 0; k < n; ++k) h[k] = (float) ((double) h[k] * dc / sum);
    fir_.setCoefficients(h, (size_t) n);
  }
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
    design(i, hz, gainDb, q);
    if (settled) {
      design(i, band_[i].hz, band_[i].gainDb, band_[i].q);
      moving_[i] = false;
    }
  }
  void design(int i, float hz, float gainDb, float q) {
    const float f = clampf(hz, 10.0f, sr_ * 0.45f);
    const float qq = q < 0.05f ? 0.05f : q;
    float b0, b1, b2, a1, a2;
    if (matched_ && eqdetail::matched(band_[i].shape, f, qq, gainDb, sr_, &b0, &b1, &b2, &a1, &a2)) {
      filter_[i].setCoefficients(b0, b1, b2, a1, a2);
      return;
    }
    switch (band_[i].shape) {
      case Shape::Bell: filter_[i].peak(f, qq, gainDb); break;
      case Shape::LowShelf: filter_[i].lowShelf(f, qq, gainDb); break;
      case Shape::HighShelf: filter_[i].highShelf(f, qq, gainDb); break;
      case Shape::Lowpass: filter_[i].lowpass(f, qq); break;
      case Shape::Highpass: filter_[i].highpass(f, qq); break;
      case Shape::Notch: filter_[i].notch(f, qq); break;
      case Shape::Bandpass: filter_[i].bandpass(f, qq); break;
    }
  }

  Biquad filter_[Bands];
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
