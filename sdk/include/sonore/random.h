// SPDX-License-Identifier: Apache-2.0
//
// Randomness, and the noise a DSP makes out of it.
//
// ── Why not <random> ────────────────────────────────────────────────────────
//
// std::mt19937 is 2.5 kilobytes of state and its constructor loops 624 times.
// std::uniform_real_distribution is not specified to give the same numbers on
// two implementations, so a plugin's dither would differ between a Windows
// build and a Linux one and the two would not null against each other. And
// nothing in <random> promises to be free of allocation or locks, which is the
// one property this needs.
//
// So: xoshiro128+, which is sixteen bytes of state, a dozen instructions, and
// the same sequence everywhere.
//
// ── The real-time rule ──────────────────────────────────────────────────────
//
// Every call here runs on the audio thread. No allocation, no locks, no
// division, no branches on the value. A noise generator that allocated would be
// a dropout every time somebody turned it up.
#pragma once

#include <cstdint>
#include <cmath>

namespace sonore {

/**
 * The fixed default seed, in one place instead of five copies of the literal.
 *
 * Every generator below takes a seed and defaults to THIS one, and each default
 * ctor is deliberately NOT `explicit` while the seeded ctor is -- see Random.
 */
inline constexpr uint64_t kDefaultSeed = 0x2545F4914F6CDD1Dull;

/**
 * xoshiro128+, seeded by splitmix64.
 *
 * Deterministic from its seed, which matters more than it sounds: a plugin
 * whose noise differed between runs cannot be tested by rendering twice and
 * comparing, and cannot be nulled against a reference render at all.
 */
class Random {
public:
  /** The default seed is fixed, not the clock. A plugin that seeded from time
   *  would produce a different render every time it was bounced, and "bounce
   *  twice, compare" is how people check a DAW did what they asked. A caller
   *  that genuinely wants unpredictability passes its own seed.
   *
   *  The constructor is SPLIT IN TWO ON PURPOSE, and it is not style.
   *
   *  This was one `explicit Random(uint64_t seed = kDefaultSeed)`: an explicit
   *  constructor that is also the DEFAULT constructor. A DSP holding one as a
   *  member is then an aggregate whose member cannot be copy-initialised from
   *  `{}`, so `SonoreDsp dsp{}` -- which is exactly what clap_wrapper.h writes
   *  -- is ill-formed. GCC accepts it, clang does not, so a plugin using noise
   *  or dither BUILT ON LINUX AND FAILED ON MACOS with
   *  "chosen constructor is explicit in copy-initialization", pointing at the
   *  SDK header rather than at the member responsible. One did: it shipped to
   *  the marketplace Linux-only before anyone noticed.
   *
   *  A non-explicit default ctor fixes that. `explicit` stays on the SEEDED one,
   *  which is where it was ever earning anything -- it stops a bare integer
   *  silently becoming a Random. */
  Random() { setSeed(kDefaultSeed); }
  /** The sequence `seed` gives: the same numbers on every platform and every
   *  run. Any value works, 0 included. */
  explicit Random(uint64_t seed) { setSeed(seed); }

  /** Back to the start of the sequence the last seed gave: what a reset()
   *  holding one of these means. Without it a reset drew on from wherever
   *  the noise had got to, and two renders of the same audio did not null. */
  void restart() { setSeed(seed_); }

  /** Start the sequence `seed` gives, the same as a fresh Random(seed), and
   *  make it the one restart() returns to. Any value works, 0 included. */
  void setSeed(uint64_t seed) {
    seed_ = seed;
    // splitmix64, whose whole job is turning a poor seed into good state. Seeded
    // directly with a small number, xoshiro's first dozen outputs are visibly
    // correlated -- and a plugin seeded with 1 is exactly what happens.
    for (int i = 0; i < 4; ++i) {
      seed += 0x9E3779B97F4A7C15ull;
      uint64_t z = seed;
      z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
      z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
      state_[i] = (uint32_t) ((z ^ (z >> 31)) >> 32);
    }
    // All-zero state is a fixed point: it would return zero forever.
    if ((state_[0] | state_[1] | state_[2] | state_[3]) == 0) state_[0] = 1;
  }

  /** [audio thread] A uniform 32-bit value. */
  uint32_t nextUint32() {
    const uint32_t result = state_[0] + state_[3];
    const uint32_t t = state_[1] << 9;
    state_[2] ^= state_[0];
    state_[3] ^= state_[1];
    state_[1] ^= state_[2];
    state_[0] ^= state_[3];
    state_[2] ^= t;
    state_[3] = (state_[3] << 11) | (state_[3] >> 21);
    return result;
  }

  /** [audio thread] 0 up to but not including 1. Built from a multiply rather
   *  than a division, and from the TOP 24 bits: the low bits of an xorshift
   *  generator are the weakest, and float has 24 bits of mantissa anyway. */
  float nextFloat() {
    return (float) (nextUint32() >> 8) * (1.0f / 16777216.0f);
  }

  /** [audio thread] -1 to 1. What a noise source actually wants. */
  float nextBipolar() { return nextFloat() * 2.0f - 1.0f; }

  /** [audio thread] 0 up to but not including `limit`. */
  int nextInt(int limit) {
    if (limit <= 0) return 0;
    // Lemire's multiply-shift: one multiply, no modulo, and no division. The
    // bias is under one part in 2^32 for any limit a plugin uses, which is
    // beneath anything audible or visible.
    return (int) (((uint64_t) nextUint32() * (uint64_t) limit) >> 32);
  }

  /** [audio thread] True or false with even odds, from the strongest (top) bit. */
  bool nextBool() { return (nextUint32() >> 31) != 0; }

  /**
   * [audio thread] Gaussian, mean 0 and standard deviation 1.
   *
   * The sum of twelve uniforms minus six -- not Box-Muller. Box-Muller needs a
   * log and a sqrt and a cos per pair, which is a lot of transcendental
   * arithmetic per sample; this is twelve adds. Its tails stop at +-6 sigma
   * rather than being unbounded, which for audio is a feature: a true Gaussian
   * occasionally emits a sample large enough to click.
   */
  float nextGaussian() {
    float sum = 0.0f;
    for (int i = 0; i < 12; ++i) sum += nextFloat();
    return sum - 6.0f;
  }

private:
  uint32_t state_[4] = {0, 0, 0, 0};
  uint64_t seed_ = kDefaultSeed; // what restart() goes back to
};

/**
 * White noise: equal energy per hertz, which sounds bright.
 *
 * A class rather than a call on Random because a plugin wants one per voice
 * with its own seed, so two voices do not produce identical noise and cancel
 * when summed to mono.
 */
class WhiteNoise {
public:
  /** Default ctor non-explicit so this type can be a DSP member -- see Random. */
  WhiteNoise() : random_(kDefaultSeed) {}
  /** Noise from `seed`; give each voice its own so voices do not cancel. */
  explicit WhiteNoise(uint64_t seed) : random_(seed) {}

  /** Start the noise `seed` gives, the same as a fresh WhiteNoise(seed);
   *  reset() returns to it. */
  void setSeed(uint64_t seed) { random_.setSeed(seed); }
  /** The noise from the start of its seed again. */
  void reset() { random_.restart(); }

  /** [audio thread] */
  float next() { return random_.nextBipolar(); }

private:
  Random random_;
};

/**
 * Pink noise: equal energy per OCTAVE, which is what people mean when they say
 * noise sounds natural, and what every acoustic measurement is done with.
 *
 * Voss-McCartney with sixteen rows: every octave from 20 Hz to 20 kHz within
 * 0.6 dB of the others at 48 kHz (measured; the test allows 1 dB), and pink
 * down to fs/2^17 -- 1.5 Hz even at 192 kHz -- so the level in a low band does
 * not depend on the session's rate. Seven rows, which is what this had, are
 * pink only above fs/128: at 48 kHz everything under 375 Hz was white, the
 * 20-40 Hz octave 8 dB down. A true -3 dB per octave filter is an
 * infinite-order problem; this is still one new random value per sample.
 *
 * Bounded to +-1 (seventeen sources, each +-1, over seventeen); about 0.14
 * RMS, -17 dBFS.
 */
class PinkNoise {
public:
  /** Default ctor non-explicit so this type can be a DSP member -- see Random.
   *  Delegating rather than duplicating the row priming below. */
  PinkNoise() : PinkNoise(kDefaultSeed) {}
  /** Noise from `seed`, every row already drawn, so it is at its full level
   *  and colour from the first sample. */
  explicit PinkNoise(uint64_t seed) { setSeed(seed); }

  /** Start again from `seed`: the SAME sequence a fresh PinkNoise(seed) gives.
   *  It reseeded only the generator and kept the rows, their running sum and
   *  the counter, so a used noise source reseeded to 42 differed from a new
   *  one in every sample -- and "bounce twice and compare" (tape.h's hiss
   *  among others) compared two different noises. */
  void setSeed(uint64_t seed) {
    random_.setSeed(seed);
    counter_ = 0;
    for (int i = 0; i < kOctaves; ++i) rows_[i] = random_.nextBipolar();
    running_ = 0.0f;
    for (int i = 0; i < kOctaves; ++i) running_ += rows_[i];
  }

  /**
   * [audio thread] Allocates nothing and takes at most one new random value per
   * sample regardless of how many rows there are -- which is the whole point
   * of Voss-McCartney over summing sixteen independent generators.
   */
  float next() {
    ++counter_;
    // Which row changes is decided by the lowest SET bit of the counter, so row
    // 0 updates every sample, row 1 every other, row 2 every fourth. That is
    // the 1/f distribution, built out of a bit trick rather than seven filters.
    if (counter_ != 0) {
      int row = 0;
      uint32_t bits = counter_;
      while ((bits & 1u) == 0u && row < kOctaves - 1) {
        bits >>= 1;
        ++row;
      }
      const float replacement = random_.nextBipolar();
      running_ += replacement - rows_[row];
      rows_[row] = replacement;
    }
    // The white component on top, and the scale that brings the sum of
    // seventeen sources back within +-1.
    return (running_ + random_.nextBipolar()) * (1.0f / (float) (kOctaves + 1));
  }

private:
  static constexpr int kOctaves = 16;
  Random random_;
  float rows_[kOctaves] = {0};
  float running_ = 0.0f;
  uint32_t counter_ = 0;
};

/**
 * TPDF dither: what you add before reducing bit depth.
 *
 * Triangular rather than rectangular, and that is the entire point. Rectangular
 * dither removes the correlation between the signal and the quantisation error
 * but leaves the NOISE FLOOR modulated by the signal -- audible as a hiss that
 * breathes with the music. Triangular removes both, at the cost of 3 dB more
 * noise, and is what every mastering engineer means by "dither".
 */
class Dither {
public:
  /** Default ctor non-explicit so this type can be a DSP member -- see Random. */
  Dither() : random_(kDefaultSeed) {}
  /** Dither from `seed`; give each channel its own so the channels' dither is
   *  uncorrelated. */
  explicit Dither(uint64_t seed) : random_(seed) {}

  /** Start the dither `seed` gives, the same as a fresh Dither(seed). */
  void setSeed(uint64_t seed) { random_.setSeed(seed); }

  /** [audio thread] Triangular noise 2 LSB peak-to-peak, for a target of
   *  `bits` -- the sum of two 1-LSB rectangular dithers, which is the mastering
   *  standard. The difference of two uniforms IS a triangular distribution --
   *  no table and no transcendental.
   *
   *  The LSB for a signal in [-1, 1) at `bits` bits is 2 / 2^bits. This used to
   *  divide by 2^(bits-1), i.e. one LSB too few, so the dither came out at
   *  4 LSB peak-to-peak -- 6 dB hotter than TPDF, an audibly raised noise floor
   *  on anything that dithered. */
  float next(int bits) {
    if (bits <= 0 || bits >= 32) return 0.0f;
    const float lsb = 2.0f / (float) (1u << bits);
    return (random_.nextFloat() - random_.nextFloat()) * lsb;
  }

private:
  Random random_;
};

/**
 * Velvet noise: sparse random impulses, one per grid cell, each +1 or -1.
 *
 * It sounds like white noise and costs almost nothing to convolve with,
 * because it is nearly all zeros -- which is why it is the diffusion element
 * of choice in modern reverbs (Välimäki et al.): a 2000-pulse-per-second
 * velvet convolution is a few thousand adds per second of tail, where a
 * dense noise convolution is millions of multiplies. The grid keeps the
 * pulses evenly spread; the random offset within each cell is what stops
 * the grid's own period from being heard.
 */
class VelvetNoise {
public:
  /** Default ctor non-explicit so this type can be a DSP member -- see Random. */
  VelvetNoise() : random_(kDefaultSeed) {}
  /** Pulses placed and signed from `seed`; 2000 per second at 48 kHz until
   *  setSampleRate() and setDensity() say otherwise. */
  explicit VelvetNoise(uint64_t seed) : random_(seed) {}

  /** Reseed the generator, which reset() then returns to. Only the generator:
   *  the cell in progress carries on, so call reset() too for the pattern a
   *  fresh VelvetNoise(seed) with the same settings gives. */
  void setSeed(uint64_t seed) { random_.setSeed(seed); }
  /** The rate in Hz (1 or less is taken as 48000). A new rate works the grid
   *  out again for the current density and starts a new cell. */
  void setSampleRate(float sr) { sr_ = sr > 1.0f ? sr : 48000.0f; setDensity(density_); }
  /** Pulses per second. 1000-4000 is the reverb range. */
  void setDensity(float pulsesPerSecond) {
    const float density = pulsesPerSecond > 1.0f ? pulsesPerSecond : 1.0f;
    // The same density again is no change. It restarted the cell, so set
    // every block the pulse pattern depended on the block size.
    if (density == density_ && sr_ == gridSr_) return;
    density_ = density;
    gridSr_ = sr_;
    grid_ = sr_ / density_;
    cellInc_ = 1.0f / grid_;
    cell_ = 1.0f; // start a cell on the first sample
  }
  /** Back to the start of the seed's pattern: a new cell on the next sample,
   *  the same pulses as before. Rate and density are kept. */
  void reset() {
    cell_ = 1.0f;
    countdown_ = -1;
    random_.restart();
  }

  /** [audio thread] Exactly one non-zero sample per grid cell. */
  float next() {
    cell_ += cellInc_;
    if (cell_ >= 1.0f) {
      cell_ -= 1.0f;
      // Where in this cell the pulse lands, and which way it points.
      countdown_ = random_.nextInt((int) grid_ > 1 ? (int) grid_ : 1);
      sign_ = (random_.nextUint32() & 1u) ? 1.0f : -1.0f;
    }
    if (countdown_ == 0) { --countdown_; return sign_; }
    if (countdown_ > 0) --countdown_;
    return 0.0f;
  }

private:
  Random random_;
  float sr_ = 48000.0f, density_ = 2000.0f, grid_ = 24.0f, cellInc_ = 1.0f / 24.0f;
  float gridSr_ = 48000.0f; // the rate grid_ was worked out at
  float cell_ = 1.0f, sign_ = 1.0f;
  int countdown_ = -1;
};

} // namespace sonore
