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
#include <cstdlib>

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
  for (int k = 1; k < 64; ++k) {
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
  if (arg <= 0.0) return 0.0;
  return besselI0(beta * std::sqrt(arg)) / besselI0(beta);
}

} // namespace special

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
