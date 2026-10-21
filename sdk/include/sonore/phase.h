// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: a phase accumulator that does not drift.
//
// Every oscillator here keeps its phase as `phase += inc; if (phase >= 1)
// phase -= 1;` in float, and for a note that lasts a few seconds that is
// fine. It is not fine for anything that has to stay IN STEP with something
// else for a long time -- an LFO locked to the song position, two oscillators
// meant to beat at exactly 0.5 Hz, a test tone compared against a reference
// -- because the float accumulator does not merely round, it DRIFTS. The
// increment is rounded once, and that error is added every sample; after an
// hour at 48 kHz a 440 Hz float accumulator has slid a measurable fraction of
// a cycle (the test prints by how much), and nothing about it resets.
//
// So the phase is an unsigned 64-bit integer counting 2^-64 of a turn.
//
//   - The addition is EXACT. Integer adds do not round, so the only error is
//     the increment's own: hz / sr rounded once to a double (about 1e-18 of
//     a turn for a 440 Hz tone) and truncated to 2^-64. The test holds a
//     440 Hz phase against the exact rational answer for 10^8 samples -- 35
//     minutes at 48 kHz -- and it is out by under 1e-10 of a cycle.
//   - The wrap is FREE and exact: unsigned overflow is defined in C++ and is
//     precisely "subtract one whole turn". There is no compare, no branch,
//     and no way for a large increment to land outside [0, 1).
//   - A negative increment is the same add in two's complement. Running
//     backwards needs no second code path.
//
// The value comes out as turns in [0, 1) or radians in [0, 2pi), and the
// conversion is built so it cannot round up onto the far end: a phase of
// 0.99999999999 that comes back as 1.0 is a table index one past the end.
//
// Nothing here allocates; advance() is an add and a shift.
#pragma once

#include <cmath>
#include <cstdint>

namespace sonore {

/**
 * A drift-free phase accumulator, in TURNS (0..1 is one cycle).
 *
 * The reference's Phase wraps a floating-point value at 2pi and returns the
 * phase before each step; advance() here keeps that shape -- it returns the
 * phase BEFORE stepping -- with turns as the unit, because that is what
 * fastmath::sinTurns and every wavetable lookup here take.
 */
class Phase {
public:
  /** Back to zero. The increment is kept. */
  void reset() {
    acc_ = 0;
    wrapped_ = false;
  }

  /** Cycles per sample, any real number: 1.25 is the same as 0.25, and a
   *  negative increment runs backwards. */
  void setIncrement(double turnsPerSample) {
    inc_ = toFixed(turnsPerSample);
    backwards_ = turnsPerSample < 0.0;
  }
  /** The increment as `hz / sampleRate` turns per sample: a negative
   *  frequency runs backwards, and a rate of 0 or less sets an increment of
   *  0, which holds the phase still. */
  void setFrequency(double hz, double sampleRate) {
    setIncrement(sampleRate > 0.0 ? hz / sampleRate : 0.0);
  }
  /** The increment as stored, in turns per sample, in [0, 1). */
  double getIncrement() const { return toTurns(inc_); }

  /** Jump to a phase -- a hard sync, a retrigger at a chosen point. */
  void setTurns(double turns) { acc_ = toFixed(turns); }

  /** The current phase, in [0, 1). */
  double turns() const { return toTurns(acc_); }
  /** The current phase in float, also strictly below 1: a plain cast of
   *  turns() can round 0.99999999 up to 1.0f, and a table indexed by it is
   *  read one past the end. */
  float turnsFloat() const { return (float) (acc_ >> 40) * (1.0f / 16777216.0f); }
  /** The current phase, in [0, 2pi). */
  double radians() const {
    const double r = turns() * kTwoPiD;
    return r < kTwoPiD ? r : 0.0;
  }

  /** Step by the stored increment, returning the phase BEFORE the step. */
  double advance() {
    const double before = turns();
    step();
    return before;
  }
  /** Step by this increment (turns), returning the phase before it. For a
   *  frequency that moves every sample; the increment is kept for the next
   *  advance(). */
  double advance(double incrementTurns) {
    setIncrement(incrementTurns);
    return advance();
  }

  /** Did the last step cross the 0/1 boundary (either way)? The sample a
   *  hard-sync or a polyBLEP correction has to act on. */
  bool wrapped() const { return wrapped_; }

private:
  static constexpr double kTwoPiD = 6.283185307179586476925;

  void step() {
    const std::uint64_t before = acc_;
    acc_ += inc_; // modulo 2^64: the wrap, exactly
    wrapped_ = inc_ != 0 && (backwards_ ? acc_ > before : acc_ < before);
  }

  /** Turns -> 2^-64ths of a turn. Only the fractional part matters; it is
   *  taken by subtracting floor, which is exact for a double. Scaling by
   *  2^64 is exact too (a power of two), so a double increment is held to
   *  the last bit it has. */
  static std::uint64_t toFixed(double turns) {
    if (!std::isfinite(turns)) return 0;
    const double f = turns - std::floor(turns); // [0, 1], 1 only by rounding
    if (!(f < 1.0)) return 0;
    return (std::uint64_t) (f * 18446744073709551616.0); // 2^64
  }

  /** The top 53 bits, so the double is exact and at most 1 - 2^-53. Taking
   *  all 64 and multiplying by 2^-64 rounds the last 2^11 values to 1.0. */
  static double toTurns(std::uint64_t fixed) {
    return (double) (fixed >> 11) * (1.0 / 9007199254740992.0); // 2^53
  }

  std::uint64_t acc_ = 0, inc_ = 0;
  bool backwards_ = false, wrapped_ = false;
};

} // namespace sonore
