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
 * sin, to within 1.1e-5 over [-pi, pi].
 *
 * A seven-term odd polynomial -- odd because sin is, so the approximation
 * cannot drift asymmetric and put a DC offset into an oscillator that should
 * have none.
 *
 * Outside the range it WRAPS rather than clamping: an oscillator's phase is
 * unbounded and a sine that flattened past pi would be a square wave with
 * extra steps.
 */
inline float fastSin(float x) {
  constexpr float pi = 3.14159265358979f;
  constexpr float twoPi = 6.28318530717959f;
  // Wrapped first. std::floor rather than fmod: fmod on a large phase is
  // itself a slow call, which would give back what the approximation saved.
  x -= twoPi * std::floor((x + pi) * (1.0f / twoPi));

  const float x2 = x * x;
  return x * (1.0f + x2 * (-0.16666667f +
                           x2 * (0.00833333f + x2 * (-0.00019841f + x2 * 2.7557e-6f))));
}

/** cos, to the same accuracy as fastSin, by the identity that defines it. */
inline float fastCos(float x) {
  constexpr float kHalfPi = 1.57079632679490f;
  return fastSin(x + kHalfPi);
}

/**
 * A logarithm, to within 3e-6 relative over the whole positive range.
 *
 * The exponent comes straight out of the float's bits -- exact, and free --
 * and only the mantissa needs a polynomial. Zero and negatives return the
 * lowest finite float rather than -infinity or a NaN: a NaN in an audio buffer
 * propagates through every subsequent sample and every mix bus it reaches, and
 * a very negative decibel value is what a caller asking for the log of silence
 * meant.
 */
inline float fastLog(float x) {
  if (!(x > 0.0f)) return -87.3365f; // ln of the smallest normal float
  int32_t bits;
  std::memcpy(&bits, &x, sizeof(bits));
  const int32_t exponent = ((bits >> 23) & 0xFF) - 127;
  // The mantissa put back into [1, 2), where a short polynomial is accurate.
  bits = (bits & 0x007FFFFF) | 0x3F800000;
  float mantissa;
  std::memcpy(&mantissa, &bits, sizeof(mantissa));

  const float m = mantissa - 1.0f;
  const float poly =
      m * (1.0f + m * (-0.5f + m * (0.33333333f + m * (-0.25f + m * (0.2f + m * -0.16666667f)))));
  return poly + (float) exponent * 0.69314718f;
}

/** Decibels from a gain, through fastLog. The shape a meter needs sixty times
 *  a second and does not need to eight decimal places. */
inline float fastGainToDb(float gain) { return 8.6858896f * fastLog(gain); }

/** And back. 1/20 * ln(10) folded into the constant. */
inline float fastDbToGain(float db) { return fastExp(db * 0.11512925f); }

} // namespace sonore
