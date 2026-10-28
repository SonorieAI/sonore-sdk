// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: the handful of functions the standard library does not carry.
//
// A home, not a grab bag. Two different parts of this SDK needed a modified
// Bessel function within a few weeks of each other -- a Kaiser window for FIR
// design, and a Kaiser window inside a resampling kernel -- and each grew its
// own copy because neither author looked. That is the third time a rule has
// been written twice here, so this exists to give the next one somewhere
// obvious to live.
#pragma once

#include <cmath>
#include <complex>
#include <cstdlib>
#include <limits>

// fastTanh lives there; tanhApprox below is the same rational function under
// the name the toolkit already used, and forwards rather than repeating it.
#include "fast_math.h"

namespace sonore {
namespace special {

/**
 * Modified Bessel function of the first kind, order zero.
 *
 * The Kaiser window is defined in terms of it and almost nothing else here
 * is, which is why it is easy to write twice.
 *
 * A series rather than a table: it converges in a couple of dozen terms for
 * the arguments a window uses -- beta is rarely above ten -- and a table
 * would be more code than it removes, with a resolution to argue about.
 * Computed once per window, never per sample.
 */
inline double besselI0(double x) {
  double sum = 1.0, term = 1.0;
  const double half = x * 0.5;
  // The terms grow until k passes x/2 and only then fall, so the number
  // needed grows with x. 64 was enough for any window and silently wrong
  // past x = 70 (I0(80) 5e-7 low, I0(100) 0.4% low); 400 covers everything up to
  // where I0 overflows a double, near 713. The break below stops far
  // sooner for the arguments a window uses.
  for (int k = 1; k < 400; ++k) {
    // Each term is the previous one times (x/2k)^2, which keeps the
    // factorials out of it -- writing the series literally overflows long
    // before it converges.
    term *= (half / (double) k) * (half / (double) k);
    sum += term;
    if (term < sum * 1e-17) break;
  }
  return sum;
}

/**
 * The Kaiser window's shape at a position, `r` running -1 to 1 across it.
 *
 * The window BOTH callers actually wanted, rather than the Bessel function
 * they each derived it from separately.
 */
inline double kaiser(double r, double beta) {
  const double arg = 1.0 - r * r;
  // Zero only OUTSIDE the window. At its two ends, r = +-1, the Kaiser
  // window is I0(0) / I0(beta) = 1 / I0(beta) -- MATLAB's and scipy's value,
  // and what makes beta 0 the rectangle. This was `arg <= 0`, which set the
  // end samples to zero: every Kaiser-windowed FIR kernel lost its first and
  // last taps, and "beta 0" was a rectangle with its ends cut off.
  if (arg < 0.0) return 0.0;
  return besselI0(beta * std::sqrt(arg)) / besselI0(beta);
}

// ── Elliptic functions ───────────────────────────────────────────────────────
//
// An elliptic (Cauer) filter is the steepest of its order -- equiripple in
// both bands -- and its poles and zeros are values of the Jacobi elliptic
// functions. These are the four that design needs, in the conventions of
// Orfanidis's published lecture notes on elliptic filter design, which the
// reference follows too: the modulus k (not the parameter m = k^2), and the
// argument of cd, sn and their inverse in units of the quarter period K, so
// cde(1, k) is cd(K, k) = 0 for every k. Normalising by K is what makes the
// design equations modulus-free, which is why the notes chose it.
//
// Everything rests on the descending Landen transformation: from k it builds
// a sequence of moduli k1, k2, ... falling to zero quadratically (five or six
// steps from any k a filter uses). At modulus zero, sn and cd ARE sin and cos;
// each step back up is the Gauss transformation
//     w(n-1) = (1 + k(n)) w(n) / (1 + k(n) w(n)^2),
// which holds for sn and cd alike, and for complex arguments, which the
// design needs because the poles are off the real axis.
//
// The modulus must be in [0, 1). k = 1 is the limit where K is infinite and
// the functions degenerate to hyperbolic ones; ellipticK says so (infinity)
// and cde, sne and asne return NaN there, rather than a number from a
// Landen sequence that never converged.

namespace detail {

constexpr double kHalfPi = 1.57079632679489661923;

/** The descending Landen moduli of k. No allocation: sixteen slots is more
 *  than k = 1 - 1e-15 needs. */
struct LandenSequence {
  double k[16];
  int count = 0;

  explicit LandenSequence(double modulus) {
    double kk = modulus;
    // Carry the complement too, k' = sqrt(1 - k^2), each step by its own
    // recurrence: forming 1 - k^2 from a k near 1 cancels every digit, and
    // forming k(n) = (1 - k')/(1 + k') from a k' near 1 does the same at the
    // other end. These two are well conditioned throughout.
    double kp = std::sqrt((1.0 - modulus) * (1.0 + modulus));
    while (count < 16) {
      const double r = kk / (1.0 + kp);
      kk = r * r;
      kp = 2.0 * std::sqrt(kp) / (1.0 + kp);
      k[count++] = kk;
      // At the bottom sn is taken to be sin, which is wrong by O(k^2): below
      // 1e-9 that is under a double's resolution.
      if (kk < 1e-9) break;
    }
  }
};

inline bool validModulus(double k) { return k >= 0.0 && k < 1.0; }

} // namespace detail

/** K(k), the complete elliptic integral of the first kind -- the quarter
 *  period -- by the arithmetic-geometric mean: K = pi / (2 AGM(1, k')).
 *  The AGM converges quadratically, so this is exact to the last digit in a
 *  handful of iterations. Infinite at k = 1. */
inline double ellipticK(double k) {
  if (!(k >= 0.0 && k <= 1.0)) return std::numeric_limits<double>::quiet_NaN();
  double a = 1.0, g = std::sqrt((1.0 - k) * (1.0 + k));
  if (g == 0.0) return std::numeric_limits<double>::infinity();
  for (int i = 0; i < 32 && std::fabs(a - g) > 1e-16 * a; ++i) {
    const double next = 0.5 * (a + g);
    g = std::sqrt(a * g);
    a = next;
  }
  return detail::kHalfPi / a;
}

/** K(k) and K'(k) = K(k'), together: the ratio K'/K is the imaginary
 *  period that the elliptic design's degree equation is written in. */
inline void ellipticIntegralK(double k, double& K, double& Kprime) {
  K = ellipticK(k);
  Kprime = ellipticK(std::sqrt((1.0 - k) * (1.0 + k)));
}

/** cd(u K, k): the Jacobi cd function, argument in quarter periods. */
inline std::complex<double> cde(std::complex<double> u, double k) {
  if (!detail::validModulus(k)) return {std::numeric_limits<double>::quiet_NaN(), 0.0};
  const detail::LandenSequence landen(k);
  std::complex<double> w = std::cos(u * detail::kHalfPi);
  for (int n = landen.count; n-- > 0;) w = (1.0 + landen.k[n]) * w / (1.0 + landen.k[n] * w * w);
  return w;
}

/** sn(u K, k): the Jacobi sn function, argument in quarter periods. */
inline std::complex<double> sne(std::complex<double> u, double k) {
  if (!detail::validModulus(k)) return {std::numeric_limits<double>::quiet_NaN(), 0.0};
  const detail::LandenSequence landen(k);
  std::complex<double> w = std::sin(u * detail::kHalfPi);
  for (int n = landen.count; n-- > 0;) w = (1.0 + landen.k[n]) * w / (1.0 + landen.k[n] * w * w);
  return w;
}

/**
 * The inverse of sne: a u with sn(u K, k) = w.
 *
 * Each Gauss step is inverted on the way DOWN the sequence -- the quadratic
 * k(n) w^2 s^2 - (1 + k(n)) s + w = 0 solved for s, in the rationalised form
 * that does not divide by w -- and at the bottom u = (2/pi) asin. sn is
 * doubly periodic, so there are infinitely many answers; this is the one the
 * principal square roots and arcsine lead to, which for real w in [-1, 1] is
 * the real u in [-1, 1], and for real w in (1, 1/k] -- the stopband the
 * design maps -- is 1 + i v with v in [0, K'/K].
 */
inline std::complex<double> asne(std::complex<double> w, double k) {
  if (!detail::validModulus(k)) return {std::numeric_limits<double>::quiet_NaN(), 0.0};
  const detail::LandenSequence landen(k);
  double previous = k;
  for (int n = 0; n < landen.count; ++n) {
    w = 2.0 * w / ((1.0 + landen.k[n]) * (1.0 + std::sqrt(1.0 - previous * previous * w * w)));
    previous = landen.k[n];
  }
  return std::asin(w) / detail::kHalfPi;
}

} // namespace special

/**
 * The special functions under the reference's names, so a design written
 * against those reads the same here. Static forwarders: the free functions
 * in sonore::special are the implementation, and the only copy.
 */
struct SpecialFunctions {
  /** Modified Bessel function of the first kind, order zero. */
  static double besselI0(double x) { return special::besselI0(x); }
  /** K(k) and K'(k), modulus k. */
  static void ellipticIntegralK(double k, double& K, double& Kprime) {
    special::ellipticIntegralK(k, K, Kprime);
  }
  /** cd(u K, k), u in quarter periods. */
  static std::complex<double> cde(std::complex<double> u, double k) { return special::cde(u, k); }
  /** sn(u K, k), u in quarter periods. */
  static std::complex<double> sne(std::complex<double> u, double k) { return special::sne(u, k); }
  /** u such that sn(u K, k) = w. */
  static std::complex<double> asne(std::complex<double> w, double k) { return special::asne(w, k); }
};

// ── Fast approximations ──────────────────────────────────────────────────────
//
// A saturator running 8x oversampled calls tanh eight times per output sample
// per channel. The library version is correctly rounded to the last bit, which
// nobody can hear and everybody pays for. These are accurate to well under the
// noise floor of 24-bit audio across their stated ranges, and each says what
// that range is: an approximation used outside its domain is a bug, not a
// speed-up.
namespace fastmath {

/** tanh, as a [9/8] Padé approximant.
 *
 *  The rational form keeps the S-curve exactly ODD, which a polynomial fit
 *  does not, and odd symmetry is what stops a waveshaper generating even
 *  harmonics it was never asked for. Beyond ±5 it clamps, where real tanh is
 *  already within 1e-4 of ±1.
 *
 *  The famous short version, x(27+x²)/(27+9x²), is only good to 2e-2: fine
 *  for a soft clipper, nowhere near transparent. The measured error of THIS
 *  one is asserted in the test suite rather than claimed here. */
inline float tanhApprox(float x) {
  // The identical [9/8] Padé sat here AND in fast_math.h as fastTanh, each
  // with its own clamp and its own test -- the third rule in this SDK found
  // written twice. One implementation now; both names keep their callers.
  return fastTanh(x);
}

/** 2^x for x in [-1, 1], the range a per-sample pitch or gain modulation
 *  actually spans. Outside it, falls back to the exact function rather than
 *  returning nonsense.
 *
 *  Degree 7, because 2^x = e^(x ln2) and the series coefficients are
 *  (ln2)^k/k!: truncating at degree 4 (a first version here) left 1e-3 of
 *  error at the ends, which is audible as a stepped gain. */
inline float exp2Approx(float x) {
  if (x < -1.0f || x > 1.0f) return std::pow(2.0f, x);
  const float c1 = 0.69314718f, c2 = 0.24022651f, c3 = 0.05550411f, c4 = 0.00961813f;
  const float c5 = 0.00133336f, c6 = 0.00015404f, c7 = 0.00001525f;
  return 1.0f +
         x * (c1 + x * (c2 + x * (c3 + x * (c4 + x * (c5 + x * (c6 + x * c7))))));
}

/**
 * sin(2*pi*x) with x in TURNS, for a phase that already runs 0..1.
 *
 * An oscillator holds its phase as a fraction of a cycle, so the library call
 * costs a multiply by 2*pi before it even starts. Taking turns removes that
 * and lets the polynomial be written directly in the variable the caller has.
 *
 * Why it is worth having at all: a polyphonic synth runs this once per voice
 * per sample. Eight voices at 48 kHz is 384,000 calls a second for one note
 * held down, and std::sin is correctly rounded to the last bit -- an accuracy
 * nobody can hear and everybody pays for.
 *
 * Degree 13, which is the Taylor series of sin(2*pi*x) truncated where its
 * error falls below the noise floor of 24-bit audio. The measured error is
 * asserted in the test suite rather than claimed here, because a stated
 * accuracy nobody checks is a stated accuracy that drifts.
 *
 * Accurate over the WHOLE turn, not a quadrant: the wrap below folds any
 * phase into [-0.5, 0.5) first, so a caller does not have to know the domain.
 * An approximation used outside its range is a bug, not a speed-up.
 */
inline float sinTurns(float x) {
  // Into [-0.5, 0.5) first. std::floor rather than a cast: a cast truncates
  // toward zero, so it folds negative phases to the wrong side and the sine
  // comes out inverted below zero.
  x -= std::floor(x + 0.5f);
  // ...then into [-0.25, 0.25] by sin(pi - t) == sin(t), which is where this
  // stops being a rounding detail. At half a turn the series is at the far
  // end of its useful range and leaves 2.1e-5 of error -- about -93 dB, which
  // on a held sine is audible as harmonic distortion. Folded to a quarter the
  // same polynomial is exact to well under a 24-bit step, for one compare and
  // one subtraction.
  if (x > 0.25f) x = 0.5f - x;
  else if (x < -0.25f) x = -0.5f - x;
  const float x2 = x * x;
  // (2pi)^(2k+1) / (2k+1)!, alternating.
  const float a1 = 6.2831853f, a3 = -41.341702f, a5 = 81.605249f, a7 = -76.705860f;
  const float a9 = 42.058694f, a11 = -15.094643f, a13 = 3.8195166f;
  return x * (a1 + x2 * (a3 + x2 * (a5 + x2 * (a7 + x2 * (a9 + x2 * (a11 + x2 * a13))))));
}

/** Decibels to linear gain, for control-rate use. */
inline float dbToGainApprox(float db) { return exp2Approx(db * (1.0f / 6.020599913f)); }

} // namespace fastmath

} // namespace sonore
