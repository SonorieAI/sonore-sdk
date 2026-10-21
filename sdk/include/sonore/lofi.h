// SPDX-License-Identifier: Apache-2.0
// Sonore SDK. Degradation on purpose: bit depth, sample rate, wavefolding.
//
// Three tiny nonlinearities and one wrapper, and the wrapper is the point.
// The quantiser and the folder are the most violently aliasing shapes a plugin
// reaches for -- a fold turns one sine into dozens of harmonics, most of them
// above Nyquist -- and a model asked for "a bitcrusher" writes the one-liner
// and ships the aliasing with it. Oversampled<> pairs any per-sample shaper
// with the SDK's half-band cascade so the folded product lands where the
// filter removes it, and reports the latency the host has to be told about.
//
// The DECIMATOR is deliberately not oversampled, and the reason is worth
// stating: its aliasing is the effect. A sample-and-hold at 8 kHz is supposed
// to sound like an 8 kHz converter, images and all; filtering them off would
// leave a dull lowpass and no lo-fi. Wrap it anyway if that is the sound you
// want -- nothing prevents it -- but it is not the default for a reason.
//
// Included by dsp.h.
#pragma once
#ifndef SONORE_DSP_H_INCLUDED
#error "lofi.h is part of dsp.h and needs what it includes first: include <sonore/dsp.h> instead."
#endif
#include <cmath>
#include <type_traits>
#include <utility>
#include "audio.h"

namespace sonore {

namespace lofidetail {
/** Does the shaper have a reset()? A shaper with memory (the tape loop) must
 *  be cleared with the filters around it, or a reset leaves the last note's
 *  magnetisation in the tape. */
template <typename T, typename = void>
struct HasReset : std::false_type {};
template <typename T>
struct HasReset<T, decltype((void) std::declval<T&>().reset())> : std::true_type {};
} // namespace lofidetail

// ── Bit crusher ──────────────────────────────────────────────────────────────

/**
 * Word-length reduction. `bits` may be fractional -- 3.5 bits is a perfectly
 * good step size, and sweeping the control through integers only would step.
 * At 24 bits the step is below float's own resolution and this is transparent.
 */
class BitCrusher {
public:
  /** The word length, held to 1..24 (a NaN to 1): 2^bits levels across
   *  -1..+1, so the step is 2^(1 - bits). The default is 24, transparent. */
  void setBits(float bits) {
    bits_ = clampf(bits, 1.0f, 24.0f);
    step_ = std::pow(2.0f, 1.0f - bits_); // 2^bits levels across -1..+1
    inv_ = 1.0f / step_;
  }
  /** The word length setBits() kept, 1..24. */
  float bits() const { return bits_; }

  /** [audio-thread] `x` rounded to the nearest step. Nothing is clipped: a
   *  sample past +/-1 is quantised past it. Stateless and allocation-free, so
   *  one instance can serve every channel. */
  inline float process(float x) const {
    // Mid-tread quantiser: zero is a level, so silence stays silent instead
    // of buzzing at the lowest step.
    return step_ * std::floor(x * inv_ + 0.5f);
  }

private:
  float bits_ = 24.0f, step_ = 1.1920929e-7f, inv_ = 8388608.0f;
};

// ── Decimator ────────────────────────────────────────────────────────────────

/**
 * Sample-rate reduction by sample-and-hold, to any rate, including one that
 * does not divide the host's. A phase accumulator decides when to take a new
 * sample, so 11.025 kHz inside 48 kHz holds for 4.35 samples on average
 * rather than snapping to 4 or 5 and sounding like a different rate.
 */
class Decimator {
public:
  /** The host's rate in Hz; one of 1 or less, or NaN, is taken as 48000.
   *  The hold rate is kept -- 48 kHz until setRate() says otherwise, which
   *  at a 96 kHz host already holds every second sample. */
  void setSampleRate(float sr) { sr_ = sr > 1.0f ? sr : 48000.0f; setRate(rateHz_); }
  /** The rate to hold at. At or above the host's rate this is transparent. */
  void setRate(float hz) {
    rateHz_ = hz < 1.0f ? 1.0f : hz;
    inc_ = rateHz_ / sr_;
    if (inc_ > 1.0f) inc_ = 1.0f;
  }
  /** Zero held, and the next sample taken at once. */
  void reset() { phase_ = 1.0f; held_ = 0.0f; }

  /** [audio-thread] The held sample: `x` when the rate's phase comes round,
   *  the one taken last otherwise. Allocation-free. */
  inline float process(float x) {
    phase_ += inc_;
    if (phase_ >= 1.0f) {
      phase_ -= 1.0f;
      held_ = x;
    }
    return held_;
  }

private:
  float sr_ = 48000.0f, rateHz_ = 48000.0f, inc_ = 1.0f, phase_ = 1.0f, held_ = 0.0f;
};

// ── Wave folder ──────────────────────────────────────────────────────────────

/**
 * Triangle wavefolding: what goes past +/-1 is reflected back, as many times
 * as it takes. Pushing a sine into it adds harmonics that a clipper cannot --
 * a clipper flattens the top of the wave, a folder puts a new wave on it --
 * and the timbre keeps changing with drive long after a clipper has become a
 * square.
 *
 * `fold()` is the closed form of "reflect until inside": the input mapped onto
 * a triangle wave of period 4, which is one fmod and one abs per sample
 * instead of a loop that runs longer the harder it is driven.
 *
 * `bias` shifts the input before folding. A symmetric fold makes odd
 * harmonics only; a little bias breaks the symmetry and lets the even ones in.
 */
class WaveFolder {
public:
  /** `x` reflected back into -1..+1 at each end, as often as it takes:
   *  -1..+1 passes unchanged, 1.5 comes out 0.5, 3 comes out -1. A NaN stays
   *  a NaN. */
  static inline float fold(float x) {
    const float t = x * 0.25f + 0.25f;
    const float frac = t - std::floor(t);
    return 1.0f - 4.0f * std::fabs(frac - 0.5f);
  }

  /** The input gain, linear (1, the default, is unity); a negative one
   *  becomes 0. Above 1 the signal starts to fold. */
  void setDrive(float gain) { drive_ = gain < 0.0f ? 0.0f : gain; }
  /** Added to the driven input before the fold, in the same full-scale units;
   *  0, the default, is a symmetric fold. Silence comes out as fold(bias), so
   *  a bias puts DC on the output. */
  void setBias(float bias) { bias_ = bias; }
  /** [audio-thread] fold(x * drive + bias), in -1..+1. Stateless and
   *  allocation-free. Unoversampled: wrap it in Oversampled to keep the
   *  folded harmonics from aliasing. */
  inline float process(float x) const { return fold(x * drive_ + bias_); }

private:
  float drive_ = 1.0f, bias_ = 0.0f;
};

// ── Oversampled<> ────────────────────────────────────────────────────────────

/**
 * Any per-sample shaper, run at 2^Stages times the rate.
 *
 * The wrapper owns the shaper (reach it through shaper() to set its controls)
 * and the half-band cascade from resample.h. The shaper must be a plain
 * `float process(float)`; its own state, if any, advances at the OVERSAMPLED
 * rate, which is right for a memoryless shape and something to know about
 * for one with a time constant.
 *
 * latencySamples() is the cascade's delay at the base rate, an integer by
 * construction (see resample.h for why 32x is the ceiling), and it is the
 * number the plugin must report. The IIR flavour has none, at the cost of
 * phase linearity -- OversampledIir<> for when a parallel path does not need
 * to null.
 */
template <typename Shaper, int Stages = 2, typename Filter = HalfBandFilter<33>>
class Oversampled {
public:
  /** The oversampling factor, 2^Stages: how many times the shaper runs per
   *  input sample. */
  static constexpr int factor() { return 1 << Stages; }
  /** The cascade's delay in samples at the base rate: the number the plugin
   *  reports to the host. 0 for OversampledIir. */
  static constexpr int latencySamples() { return OversamplerT<Stages, Filter>::latencySamples(); }

  /** The shaper this wrapper owns, to set its controls; it lives as long as
   *  the wrapper. A shaper that takes a sample rate runs at factor() times
   *  the host's. */
  Shaper& shaper() { return shaper_; }
  /** As above, read-only. */
  const Shaper& shaper() const { return shaper_; }

  /** Clears the filter cascade, and the shaper too if it has state. */
  void reset() {
    os_.reset();
    if constexpr (lofidetail::HasReset<Shaper>::value) shaper_.reset();
  }

  /** [audio-thread] One base-rate sample in, one out, latencySamples() late:
   *  up, the shaper factor() times, down. Allocation-free. */
  inline float process(float x) {
    return os_.process(x, [this](float v) { return shaper_.process(v); });
  }

  /** process() over a block, in place: the same bits as n calls. With the
   *  default HalfBandFilter the filters run four samples at a time and the
   *  2x and 4x cascades cost a seventh to a ninth of the per-sample calls; the
   *  shaper still runs once per oversampled sample, in order. Mixes freely
   *  with process() on the same instance. */
  void processBlock(float* io, int n) {
    os_.processBlock(io, n, [this](float v) { return shaper_.process(v); });
  }

private:
  Shaper shaper_;
  OversamplerT<Stages, Filter> os_;
};

/** Oversampled with the polyphase IIR half-band: no latency, at the cost of
 *  phase linearity, so a dry/wet mix with it does not null. */
template <typename Shaper, int Stages = 2>
using OversampledIir = Oversampled<Shaper, Stages, PolyphaseIirHalfBand<9>>;

} // namespace sonore
