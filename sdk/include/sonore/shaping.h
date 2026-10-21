// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: waveshaping, lookup tables, ballistics and chain composition.
//
// The building blocks a generated distortion or dynamics plugin reaches for
// that were not already in dsp.h. Everything here is real-time safe once
// prepared: no allocation, no branching on sample values beyond what the maths
// requires.

#pragma once
#ifndef SONORE_DSP_H_INCLUDED
#error "shaping.h is part of dsp.h and needs what it includes first: include <sonore/dsp.h> instead."
#endif

#include <cmath>
#include <cstddef>
#include <type_traits>
#include <utility>
#include <vector>

// Included by dsp.h, so `#include <sonore/dsp.h>` stays the single entry point.
#include "audio.h"
// One definition of each interpolation polynomial, shared with the stream
// readers rather than written out again here.
#include "interpolation.h"

namespace sonore {

// The fast approximations moved to special.h: dsp.h includes THIS file at
// its end, so nothing declared here is visible to the oscillator, and the
// oscillator is the caller that matters for a fast sine.

// ── Lookup table ─────────────────────────────────────────────────────────────

/**
 * A sampled function with linear interpolation, for anything too expensive to
 * evaluate per sample.
 *
 * The table stores ONE EXTRA point past the end so interpolation never reads
 * out of bounds at the top of the range: the classic off-by-one in every
 * hand-rolled lookup, and it corrupts exactly the loudest samples.
 */
class LookupTable {
public:
  /** Sample `fn` across [min, max] into `points` entries. Offline: this
   *  allocates, so it belongs in prepare(). */
  template <typename Fn>
  void prepare(Fn&& fn, float min, float max, size_t points) {
    if (points < 2) points = 2;
    min_ = min;
    max_ = max > min ? max : min + 1.0f;
    scale_ = (float) (points - 1) / (max_ - min_);
    table_.resize(points + 1);
    for (size_t i = 0; i < points; ++i)
      table_[i] = (float) fn(min_ + (float) i / scale_);
    // The guard point: equal to the last real one, so a value exactly at max
    // interpolates against itself instead of past the end.
    table_[points] = table_[points - 1];
  }

  /** True once prepare() has run. */
  bool isPrepared() const { return table_.size() > 2; }

  /** [audio-thread] The function at `x`, interpolated between the two nearest
   *  points. Below min it is the value at min and above max the value at max;
   *  0 before prepare(). No allocation, no lock. */
  inline float operator()(float x) const {
    if (table_.empty()) return 0.0f;
    if (x <= min_) return table_.front();
    if (x >= max_) return table_[table_.size() - 2];
    const float pos = (x - min_) * scale_;
    const size_t index = (size_t) pos;
    const float frac = pos - (float) index;
    return table_[index] + frac * (table_[index + 1] - table_[index]);
  }

private:
  std::vector<float> table_;
  float min_ = 0.0f, max_ = 1.0f, scale_ = 1.0f;
};

// ── Waveshaper ───────────────────────────────────────────────────────────────

/**
 * A memoryless nonlinearity with optional drive, bias and output compensation.
 *
 * Bias matters more than it looks: pushing a signal off-centre before an odd
 * shaping curve is what produces EVEN harmonics, which is most of the
 * difference between "transistor" and "valve" in a listener's description.
 * DC introduced by the bias is removed on the way out, or it would offset
 * everything downstream.
 */
class WaveShaper {
public:
  /** The input gain, linear (1 = none), applied before the curve. A negative
   *  value is taken as 0. */
  void setDrive(float linearGain) { drive_ = linearGain > 0.0f ? linearGain : 0.0f; }
  /** An offset added to the driven input before the curve, in the curve's
   *  input units (0 = centred). The DC it leaves is removed on the way out. */
  void setBias(float bias) { bias_ = bias; }
  /** Compensate for the level the drive added, so turning drive up changes the
   *  TONE rather than just the volume. */
  void setAutoGain(bool on) { autoGain_ = on; }

  /** Tunes the output DC blocker (a 10 Hz highpass) to the rate, in Hz.
   *  Call it in prepare(), before the first process(). */
  void setSampleRate(float sr) { dc_.setSampleRate(sr); }
  /** Clears the DC blocker's state; drive, bias and auto gain are kept. */
  void reset() { dc_.reset(); }

  /** Any callable float->float: std::tanh, a LookupTable, a lambda. */
  template <typename Shape>
  inline float process(float x, Shape&& shape) {
    const float shaped = (float) shape(x * drive_ + bias_);
    const float out = dc_.process(shaped);
    if (!autoGain_ || drive_ <= 1.0f) return out;
    // The compensation is the shaped value of unity: whatever the curve does
    // to a full-scale sample is what the output is divided by.
    const float reference = (float) shape(drive_);
    return reference > 1e-6f ? out / reference : out;
  }

private:
  DcBlocker dc_;
  float drive_ = 1.0f, bias_ = 0.0f;
  bool autoGain_ = false;
};

// ── Ballistics ───────────────────────────────────────────────────────────────

/**
 * Attack/release smoothing with a choice of peak or RMS detection.
 *
 * EnvFollower in dsp.h is the peak-only version this generalises: RMS
 * detection is what makes a compressor respond to LOUDNESS rather than to
 * transients, and the two sound entirely different on the same source.
 */
class BallisticsFilter {
public:
  /** What the detector follows. */
  enum class Mode {
    Peak, ///< The sample's absolute value.
    Rms   ///< The square of the sample, smoothed, reported as its square root.
  };

  /** The rate in Hz; anything at or below 1 is taken as 48000. Recomputes the
   *  attack and release coefficients from the times last set. Until this or a
   *  setter runs, both coefficients are 0 and the output follows the input
   *  with no smoothing. */
  void setSampleRate(float sr) {
    sr_ = sr > 1.0f ? sr : 48000.0f;
    setAttack(attackMs_);
    setRelease(releaseMs_);
  }
  /** Peak or RMS. Takes effect on the next sample; the state is not cleared,
   *  so call reset() after a change made mid-stream. */
  void setMode(Mode mode) { mode_ = mode; }
  /** The time constant, in milliseconds, used while the level rises: the time
   *  to cover 63% of a step. Values below 0.001 ms are taken as 0.001. */
  void setAttack(float ms) {
    attackMs_ = ms;
    attack_ = coefficient(ms);
  }
  /** The time constant, in milliseconds, used while the level falls. Values
   *  below 0.001 ms are taken as 0.001. */
  void setRelease(float ms) {
    releaseMs_ = ms;
    release_ = coefficient(ms);
  }
  /** Back to silence: the level is 0. */
  void reset() { state_ = 0.0f; }

  /** [audio-thread] Takes one sample and returns the smoothed level, linear
   *  (1 = full scale). No allocation, no lock. */
  inline float process(float x) {
    const float value = mode_ == Mode::Rms ? x * x : std::fabs(x);
    // Attack when rising, release when falling: one filter, two time
    // constants, which is what "ballistics" means.
    const float a = value > state_ ? attack_ : release_;
    state_ = flushDenormal(value + a * (state_ - value));
    return mode_ == Mode::Rms ? std::sqrt(state_) : state_;
  }

  /** The level after the last process(), linear, without taking a sample. */
  float value() const { return mode_ == Mode::Rms ? std::sqrt(state_) : state_; }

private:
  float coefficient(float ms) const {
    const float safe = ms > 0.001f ? ms : 0.001f;
    return std::exp(-1.0f / (0.001f * safe * sr_));
  }

  Mode mode_ = Mode::Peak;
  float sr_ = 48000.0f, attackMs_ = 10.0f, releaseMs_ = 100.0f;
  float attack_ = 0.0f, release_ = 0.0f, state_ = 0.0f;
};

// ── Fractional-delay interpolation ───────────────────────────────────────────
//
// Reading a delay line at a fractional position is not a rounding question, it
// is a FILTER. Linear interpolation is a lowpass whose corner moves with the
// fraction: at a delay of exactly N samples it is transparent, and at N + 0.5
// it is down several dB at the top of the band. In a modulated delay (chorus,
// flanger, vibrato) that corner sweeps back and forth, and the result is the
// swishing every cheap chorus has.
//
// These take four points around the position (y0 = one before, y1/y2 either
// side, y3 = one after) and the fraction between y1 and y2.
namespace interpolate {

/** Two points, straight line. Cheapest, and audibly the worst on modulation. */
inline float linear(float y1, float y2, float frac) { return y1 + frac * (y2 - y1); }

/** Third-order Lagrange: the polynomial through all four points. Flat to far
 *  higher frequencies than linear at the same cost class.
 *
 *  Forwarded, not copied. This lived here as its own arithmetic and again in
 *  interpolation.h as a kernel, written months apart by someone who did not
 *  look -- which is the exact shape of bug this SDK has been bitten by three
 *  times over: a rule in more than one place, with only one copy tested. They
 *  were checked against each other over a grid of inputs before being folded
 *  together, because two implementations that quietly differ are worse than
 *  two that obviously do. */
inline float lagrange3(float y0, float y1, float y2, float y3, float frac) {
  // The kernel takes its points NEWEST FIRST, which is the order a stream
  // arrives in; this signature is oldest first, which is the order a buffer
  // read hands them over.
  const float points[4] = {y3, y2, y1, y0};
  return Lagrange3Kernel::at(points, frac);
}

/** Catmull-Rom spline: matches Lagrange's smoothness but passes through the
 *  points with continuous slope, which is what keeps a swept delay from
 *  clicking as it crosses sample boundaries. */
inline float catmullRom(float y0, float y1, float y2, float y3, float frac) {
  return CatmullRomKernel::at4(y0, y1, y2, y3, frac);
}

} // namespace interpolate

// ── Processor chain ──────────────────────────────────────────────────────────

/**
 * Compile-time composition of processors, so a generated plugin can express
 * "filter into saturator into limiter" as a type rather than as five members
 * and five hand-written forwarding calls.
 *
 * Every stage is expanded at compile time; there is no virtual dispatch and
 * nothing to allocate, which is the whole point: a chain must cost exactly
 * what writing the stages out by hand would have cost.
 */
template <typename... Stages>
class ProcessorChain {
public:
  /** How many stages the chain has: the length of `Stages`. */
  static constexpr size_t kNumStages = sizeof...(Stages);

  /** The stage at index I, so a caller can configure one by name:
   *  chain.get<0>().setCutoff(1000.0f). */
  template <size_t I>
  auto& get() {
    return std::get<I>(stages_);
  }
  /** The stage at index I, read-only. */
  template <size_t I>
  const auto& get() const {
    return std::get<I>(stages_);
  }

  /** Calls every stage's prepare(spec), in declaration order, bypassed or
   *  not. Each stage must have one. Allocates if a stage does. */
  void prepare(const ProcessSpec& spec) {
    forEach([&spec](auto& stage) { stage.prepare(spec); });
  }

  /** Calls every stage's reset(), in declaration order. */
  void reset() {
    forEach([](auto& stage) { stage.reset(); });
  }

  /** Each stage processes the block in place, in declaration order. */
  void process(AudioBlock<float>& block) {
    forEach([&block](auto& stage) { stage.process(block); });
  }

  /** Bypass one stage without removing it from the type. */
  void setBypassed(size_t index, bool bypassed) {
    if (index < kNumStages) bypassed_[index] = bypassed;
  }
  /** Whether stage `index` is bypassed; false for an index past the end. */
  bool isBypassed(size_t index) const { return index < kNumStages && bypassed_[index]; }

  /** Process honouring the bypass flags. */
  void processWithBypass(AudioBlock<float>& block) {
    size_t index = 0;
    forEach([&](auto& stage) {
      if (!bypassed_[index]) stage.process(block);
      ++index;
    });
  }

private:
  template <typename Fn, size_t... I>
  void expand(Fn&& fn, std::index_sequence<I...>) {
    // The comma fold applies fn to every stage in order, at compile time.
    (void) std::initializer_list<int>{(fn(std::get<I>(stages_)), 0)...};
  }
  template <typename Fn>
  void forEach(Fn&& fn) {
    expand(fn, std::index_sequence_for<Stages...>{});
  }

  std::tuple<Stages...> stages_;
  bool bypassed_[kNumStages > 0 ? kNumStages : 1] = {};
};

/**
 * Runs one MONO processor per channel, so a design written for a single
 * channel becomes a multichannel one without being rewritten.
 *
 * Each channel gets its own INSTANCE, which is the whole point: sharing one
 * filter's state across channels is the bug that makes a stereo signal
 * collapse towards mono as soon as the material differs between sides.
 */
template <typename MonoProcessor, size_t MaxChannels = 8>
class ProcessorDuplicator {
public:
  /** The processor for channel `index`, 0-based. An index at or past
   *  MaxChannels gives the last one rather than reading out of bounds. */
  MonoProcessor& channel(size_t index) {
    return processors_[index < MaxChannels ? index : MaxChannels - 1];
  }
  /** The processor for channel `index`, read-only; clamped as above. */
  const MonoProcessor& channel(size_t index) const {
    return processors_[index < MaxChannels ? index : MaxChannels - 1];
  }

  /** Configure every channel at once, which is the common case: the same
   *  cutoff on both sides, separate state behind each. */
  template <typename Fn>
  void forEachChannel(Fn&& fn) {
    for (size_t i = 0; i < MaxChannels; ++i) fn(processors_[i]);
  }

  /** Prepares all MaxChannels processors with a copy of `spec` whose
   *  numChannels is 1, and records how many of them the spec's channel count
   *  uses (at most MaxChannels). */
  void prepare(const ProcessSpec& spec) {
    active_ = spec.numChannels < MaxChannels ? spec.numChannels : MaxChannels;
    ProcessSpec mono = spec;
    mono.numChannels = 1;
    for (size_t i = 0; i < MaxChannels; ++i) processors_[i].prepare(mono);
  }

  /** Calls reset() on all MaxChannels processors. */
  void reset() {
    for (size_t i = 0; i < MaxChannels; ++i) processors_[i].reset();
  }

  /** Per-sample form, for a processor whose interface is float -> float. */
  void processSamples(AudioBlock<float>& block) {
    const size_t channels =
        block.getNumChannels() < MaxChannels ? block.getNumChannels() : MaxChannels;
    for (size_t c = 0; c < channels; ++c) {
      float* data = block.getChannelPointer(c);
      for (size_t i = 0; i < block.getNumSamples(); ++i) data[i] = processors_[c].process(data[i]);
    }
  }

  /** The channel count the last prepare() was given, capped at MaxChannels;
   *  0 before prepare(). processSamples() goes by the block's channel count,
   *  not by this. */
  size_t activeChannels() const { return active_; }

private:
  MonoProcessor processors_[MaxChannels];
  size_t active_ = 0;
};

// ── Stock chain stages ───────────────────────────────────────────────────────
//
// A chain is worth nothing without things to put in it. These are the two
// stages every signal path starts with, plus the adapter that makes the REST
// of the toolkit chainable: without it, ProcessorChain could only be used with
// processors written specially for it, which is a chain nobody would use.

/** Level, applied as a RAMP so a moved fader cannot click.
 *
 *  The ramp is a fixed TIME -- 10 ms, set up for the rate in prepare() -- and
 *  carries across blocks. It used to be one host block long, which at 512
 *  frames is 10 ms and at a split block of 16 is a third of a millisecond: a
 *  0 -> 1 move at a block of 1 was a step, and hosts that split blocks at
 *  every automation point send blocks that small. */
class Gain {
public:
  /** The gain to ramp to, linear (1 = unity). The 10 ms ramp starts at the
   *  next process(). A plain member, not atomic: set it on the thread that
   *  calls process(). */
  void setGainLinear(float g) { target_ = g; }
  /** The gain to ramp to, in decibels (0 dB = unity). As setGainLinear(). */
  void setGainDecibels(float db) { target_ = dbToGain(db); }
  /** The gain last set, linear: the ramp's target, not where the ramp is. */
  float getGainLinear() const { return target_; }

  /** Sizes the ramp to 10 ms at spec.sampleRate (at least one sample) and
   *  jumps to the target, as reset() does. */
  void prepare(const ProcessSpec& spec) {
    rampLength_ = (int) (spec.sampleRate * 0.010);
    if (rampLength_ < 1) rampLength_ = 1;
    reset();
  }
  /** Jumps to the target gain with no ramp. */
  void reset() {
    current_ = rampTarget_ = target_;
    remaining_ = 0;
  }

  /** [audio-thread] Multiplies every channel of `block` in place, ramping
   *  linearly towards a changed target; a target changed mid-ramp starts a
   *  new 10 ms ramp from where the gain is. At a settled unity gain the block
   *  is not touched. No allocation, no lock. */
  void process(AudioBlock<float>& block) {
    if (target_ != rampTarget_) {
      rampTarget_ = target_;
      remaining_ = rampLength_;
      step_ = (rampTarget_ - current_) / (float) remaining_;
    }
    const size_t n = block.getNumSamples(), nch = block.getNumChannels();
    if (remaining_ == 0) {
      // Settled: a plain gain, and nothing at all at unity.
      if (current_ != 1.0f)
        for (size_t c = 0; c < nch; ++c) {
          float* x = block.getChannelPointer(c);
          for (size_t i = 0; i < n; ++i) x[i] *= current_;
        }
      return;
    }
    const float start = current_;
    const int ramping = remaining_ < (int) n ? remaining_ : (int) n;
    for (size_t c = 0; c < nch; ++c) {
      float* x = block.getChannelPointer(c);
      float g = start;
      for (int i = 0; i < ramping; ++i) {
        g += step_;
        x[i] *= g;
      }
      for (size_t i = (size_t) ramping; i < n; ++i) x[i] *= rampTarget_;
    }
    remaining_ -= ramping;
    current_ = remaining_ > 0 ? start + step_ * (float) ramping : rampTarget_;
  }

private:
  float target_ = 1.0f, current_ = 1.0f, rampTarget_ = 1.0f, step_ = 0.0f;
  int rampLength_ = 480, remaining_ = 0;
};

/** A DC offset. On its own it does nothing audible; its use is BEFORE a
 *  waveshaper, where pushing the signal off centre is what asks an odd curve
 *  for even harmonics. */
class Bias {
public:
  /** The offset added to every sample, in sample units (1 = full scale). It
   *  takes effect at once, with no ramp. */
  void setBias(float bias) { bias_ = bias; }
  /** Nothing to prepare; here so the stage fits a ProcessorChain. */
  void prepare(const ProcessSpec&) {}
  /** Nothing to reset; here so the stage fits a ProcessorChain. */
  void reset() {}

  /** [audio-thread] Adds the bias to every sample of every channel, in place. */
  void process(AudioBlock<float>& block) {
    for (size_t c = 0; c < block.getNumChannels(); ++c) {
      float* data = block.getChannelPointer(c);
      for (size_t i = 0; i < block.getNumSamples(); ++i) data[i] += bias_;
    }
  }

private:
  float bias_ = 0.0f;
};

namespace stagedetail {

/** Whichever way a processor wants to be told the rate. The toolkit predates
 *  ProcessSpec in places (Biquad, LadderFilter and friends take a plain
 *  sample rate), and an adapter that only understood one of the two would
 *  leave half the library unchainable. */
template <typename T, typename = void>
struct HasPrepare : std::false_type {};
template <typename T>
struct HasPrepare<T, decltype((void) std::declval<T&>().prepare(std::declval<const ProcessSpec&>()))>
    : std::true_type {};

template <typename T, typename = void>
struct HasSetSampleRate : std::false_type {};
template <typename T>
struct HasSetSampleRate<T, decltype((void) std::declval<T&>().setSampleRate(48000.0f))>
    : std::true_type {};

template <typename T>
inline void prepareOne(T& processor, const ProcessSpec& spec) {
  if constexpr (HasPrepare<T>::value) {
    processor.prepare(spec);
  } else if constexpr (HasSetSampleRate<T>::value) {
    processor.setSampleRate((float) spec.sampleRate);
  } else {
    (void) processor;
    (void) spec;
  }
}

} // namespace stagedetail

/**
 * Turns any PER-SAMPLE processor into a chain stage, one instance per channel.
 *
 * This is what makes Biquad, SVF, LadderFilter, DcBlocker (anything with a
 * `float process(float)`) usable inside a ProcessorChain without being
 * rewritten. State stays per channel, for the same reason ProcessorDuplicator
 * exists: one filter shared across channels collapses a stereo image.
 */
template <typename SampleProcessor, size_t MaxChannels = 8>
class SampleStage {
public:
  /** Reach one channel's processor, or configure them all at once. */
  SampleProcessor& channel(size_t index) {
    return processors_[index < MaxChannels ? index : MaxChannels - 1];
  }
  /** Calls `fn(processor)` on all MaxChannels processors: the same settings
   *  on every channel, separate state behind each. */
  template <typename Fn>
  void configure(Fn&& fn) {
    for (size_t i = 0; i < MaxChannels; ++i) fn(processors_[i]);
  }

  /** Prepares all MaxChannels processors: prepare() where the processor
   *  has one, otherwise setSampleRate(spec.sampleRate), otherwise nothing.
   *  prepare() gets a copy of `spec` whose numChannels is 1, as
   *  ProcessorDuplicator passes it: each processor runs one channel. The
   *  spec used to go through as given, and a processor that sized itself by
   *  the channel count was told about channels it never sees. */
  void prepare(const ProcessSpec& spec) {
    ProcessSpec mono = spec;
    mono.numChannels = 1;
    for (size_t i = 0; i < MaxChannels; ++i) stagedetail::prepareOne(processors_[i], mono);
  }

  /** Calls reset() on all MaxChannels processors; the processor must have one. */
  void reset() {
    for (size_t i = 0; i < MaxChannels; ++i) processors_[i].reset();
  }

  /** [audio-thread] Runs each channel's samples through that channel's
   *  processor, in place. Channels past MaxChannels pass through untouched. */
  void process(AudioBlock<float>& block) {
    const size_t channels =
        block.getNumChannels() < MaxChannels ? block.getNumChannels() : MaxChannels;
    for (size_t c = 0; c < channels; ++c) {
      float* data = block.getChannelPointer(c);
      for (size_t i = 0; i < block.getNumSamples(); ++i) data[i] = processors_[c].process(data[i]);
    }
  }

private:
  SampleProcessor processors_[MaxChannels];
};

// ── Logarithmic smoothing ────────────────────────────────────────────────────

/**
 * Smoothing in the RATIO domain, for anything measured in octaves or decibels.
 *
 * Smoothing a cutoff linearly from 100 Hz to 10 kHz spends most of the glide
 * above 5 kHz, because equal steps in hertz are wildly unequal steps in pitch.
 * The ear hears ratios, so the smoothing has to work in them: this interpolates
 * the LOGARITHM and the sweep sounds even.
 *
 * Not to be confused with LogRamp in dsp.h, which is the other half of this
 * pair: that one runs for a stated number of samples and lands exactly on its
 * target, which is what a FADE needs. This one approaches for ever, which is
 * what a knob being turned needs.
 */
class LogSmooth {
public:
  /** The rate in Hz and the time constant in milliseconds: the time to cover
   *  63% of the way to a new target, as a ratio. Below 0.01 ms is taken as
   *  0.01. Before setup() the output jumps straight to each target. */
  void setup(float sampleRate, float ms = 20.0f) {
    a_ = std::exp(-1.0f / (0.001f * (ms > 0.01f ? ms : 0.01f) * sampleRate));
  }
  /** Jumps to `v` with no glide. The value starts at 1 until this is called,
   *  so call it with the first value before the first next(). Values at or
   *  below 1e-6 are taken as 1e-6: this smooths positive quantities only. */
  void snap(float v) { logZ_ = std::log(v > kFloor ? v : kFloor); }

  /** [audio-thread] One sample's step towards `target` (floored at 1e-6 like
   *  snap()), returning the new value. Calls log and exp once each; no
   *  allocation. */
  inline float next(float target) {
    const float t = std::log(target > kFloor ? target : kFloor);
    logZ_ = t + a_ * (logZ_ - t);
    return std::exp(logZ_);
  }

  /** The value after the last next() or snap(), without taking a step. */
  float value() const { return std::exp(logZ_); }

private:
  static constexpr float kFloor = 1e-6f; // log(0) is where this would explode
  float a_ = 0.0f, logZ_ = 0.0f;
};

} // namespace sonore
