// SPDX-License-Identifier: Apache-2.0
//
// Cheap approximations of the expensive functions.
//
// ── What they are for ───────────────────────────────────────────────────────
//
// std::tanh is a saturating waveshaper's whole inner loop, and it costs on the
// order of forty cycles. At 96 kHz with eight times oversampling that is
// thirty million calls a second per voice, and the difference between a
// polyphonic synth that runs and one that does not.
//
// ── The honest way to ship an approximation ────────────────────────────────
//
// Say what the error IS, over what range, and check it. An approximation
// documented as "fast" and nothing else is one nobody can decide whether to
// use: the question is never "is it accurate" but "is it accurate enough for
// what I am doing", and that cannot be answered without a number.
//
// Every function here states its maximum absolute error over its stated range,
// and the tests measure that number across the range rather than at a handful
// of points -- an approximation checked at three values is one whose worst
// case is wherever nobody looked.
//
// ── They are approximations, not replacements ──────────────────────────────
//
// Nothing in this SDK uses these by default. A filter coefficient computed
// with fastExp would be subtly wrong forever; a saturator using fastTanh is
// indistinguishable by ear. The choice belongs to whoever writes the DSP, and
// making it for them silently is how a plugin ends up detuned.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

namespace sonore {

/**
 * tanh, to within 2.5e-4 over [-5, 5].
 *
 * A Padé-style rational approximation: the ratio of two cubics, which is two
 * multiplies and a divide against std::tanh's exponential. Beyond the range it
 * saturates to ±1, which is what tanh does anyway to five decimal places past
 * ±5, so the clamp is not a compromise -- it is the correct answer arrived at
 * cheaply.
 *
 * The shape matters as much as the error for a waveshaper: this is monotonic
 * and odd-symmetric like the real thing, so it cannot fold back on itself and
 * cannot introduce even harmonics that were not asked for.
 */
inline float fastTanh(float x) {
  if (x < -4.97f) return -1.0f;
  if (x > 4.97f) return 1.0f;
  const float x2 = x * x;
  const float numerator = x * (135135.0f + x2 * (17325.0f + x2 * (378.0f + x2)));
  const float denominator = 135135.0f + x2 * (62370.0f + x2 * (3150.0f + x2 * 28.0f));
  return numerator / denominator;
}

/**
 * exp, to within 2e-5 relative over [-8, 8].
 *
 * Splits the exponent into an integer power of two -- assembled directly into
 * the float's bits, which is free -- and a remainder in [-0.5, 0.5] handled by
 * a short polynomial. The bit assembly is what makes it fast; a polynomial
 * over the whole range would need far more terms for the same error.
 */
inline float fastExp(float x) {
  if (x < -87.0f) return 0.0f;   // below this a float underflows to zero anyway
  if (x > 88.0f) return 3.4e38f; // and above it, to infinity
  // log2(e), so the integer part is a power of TWO and can be written straight
  // into the exponent field.
  const float scaled = x * 1.44269504f;
  const float rounded = std::floor(scaled + 0.5f);
  const float remainder = (scaled - rounded) * 0.69314718f; // back to natural units

  // exp(remainder) over [-0.35, 0.35], where five terms are plenty.
  const float r = remainder;
  const float poly =
      1.0f + r * (1.0f + r * (0.5f + r * (0.16666667f + r * (0.04166667f + r * 0.00833333f))));

  // The power of two, built by hand. 127 is the float exponent bias.
  const int32_t exponent = (int32_t) rounded;
  int32_t bits = (exponent + 127) << 23;
  float power;
  std::memcpy(&power, &bits, sizeof(power));
  return poly * power;
}

/**
 * sin, to within 3e-7 over [-pi, pi] (measured 1.8e-7).
 *
 * A five-term odd polynomial -- odd because sin is, so the approximation
 * cannot drift asymmetric and put a DC offset into an oscillator that should
 * have none -- fitted to minimise the worst error over [0, pi/2], and only
 * ever evaluated there: sin(pi - x) == sin(x) folds the rest of the circle in.
 * Unfolded, the same series run out to pi was 6.9e-3 off, about -43 dB of
 * harmonic distortion on a held sine.
 *
 * Outside the range it WRAPS rather than clamping: an oscillator's phase is
 * unbounded and a sine that flattened past pi would be a square wave with
 * extra steps. The wrap costs what the float's resolution of x costs -- at
 * x = 1000 that is 6e-5 -- so keep a phase small and it stays exact.
 */
inline float fastSin(float x) {
  constexpr float pi = 3.14159265358979f;
  constexpr float twoPi = 6.28318530717959f;
  constexpr float halfPi = 1.57079632679490f;
  // Wrapped first. std::floor rather than fmod: fmod on a large phase is
  // itself a slow call, which would give back what the approximation saved.
  x -= twoPi * std::floor((x + pi) * (1.0f / twoPi));
  if (x > halfPi) x = pi - x;
  else if (x < -halfPi) x = -pi - x;

  const float x2 = x * x;
  return x * (1.0f + x2 * (-0.16666648f +
                           x2 * (0.0083329007f + x2 * (-0.00019800923f + x2 * 2.5905390e-6f))));
}

/** cos, to the same accuracy as fastSin, by the identity that defines it. */
inline float fastCos(float x) {
  constexpr float kHalfPi = 1.57079632679490f;
  return fastSin(x + kHalfPi);
}

/**
 * A natural logarithm, to within 1e-5 absolute over every positive normal
 * float, and 2e-6 between 1e-6 and 1e6 (measured 7.9e-6 and 1.0e-6). What is
 * left is the float's own resolution of the answer: ln(1e38) is 87.5, where
 * one step of a float is 7.6e-6.
 *
 * The exponent comes straight out of the float's bits -- exact, and free --
 * and the mantissa is brought into [sqrt(1/2), sqrt(2)), where
 * ln(m) = 2 atanh((m - 1) / (m + 1)) and the atanh series converges after
 * five terms. A Taylor series of ln(1 + m) taken to m = 1, which is what
 * this was, converges there hardly at all: 0.076 off, 0.66 dB in
 * fastGainToDb.
 *
 * Zero, negatives, NaN and subnormals return the log of the smallest normal
 * float rather than -infinity or a NaN: a NaN in an audio buffer propagates
 * through every subsequent sample and every mix bus it reaches, and a very
 * negative decibel value is what a caller asking for the log of silence
 * meant.
 */
inline float fastLog(float x) {
  if (!(x >= 1.17549435e-38f)) return -87.3365f; // ln of the smallest normal float
  int32_t bits;
  std::memcpy(&bits, &x, sizeof(bits));
  int32_t exponent = ((bits >> 23) & 0xFF) - 127;
  // The mantissa put back into [1, 2)...
  bits = (bits & 0x007FFFFF) | 0x3F800000;
  float m;
  std::memcpy(&m, &bits, sizeof(m));
  // ...and then into [sqrt(1/2), sqrt(2)), so s below stays under 0.172.
  if (m > 1.41421356f) {
    m *= 0.5f;
    ++exponent;
  }
  const float s = (m - 1.0f) / (m + 1.0f);
  const float s2 = s * s;
  const float poly =
      s * (2.0f + s2 * (0.66666667f + s2 * (0.4f + s2 * (0.28571429f + s2 * 0.22222222f))));
  return poly + (float) exponent * 0.69314718f;
}

/** Decibels from a gain, through fastLog: within 2e-5 dB from -100 to +12 dB.
 *  The shape a meter needs sixty times a second. */
inline float fastGainToDb(float gain) { return 8.6858896f * fastLog(gain); }

/** And back. 1/20 * ln(10) folded into the constant. */
inline float fastDbToGain(float db) { return fastExp(db * 0.11512925f); }

} // namespace sonore
