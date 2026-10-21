// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: a granular engine.
//
// Grains: short windowed slices of a buffer, started on a schedule, each
// played at its own rate and position, overlapping into a cloud. The same
// machine is a texture generator, a pitch shifter, a freeze and a smear
// depending on four numbers -- size, density, position, pitch -- and it is
// the one "granular" every request means. The buffer is written live (it is
// a delay you can scatter; stop writing and it is a freeze) or loaded once (a
// sample you can stretch).
//
// Decisions:
//   The scheduler is a phase accumulator at `density` grains per second, so
//   density is what it says and fractional densities work. `jitter` scatters
//   each grain's start around the position so the cloud does not buzz at
//   the scheduling rate -- a regular grain train IS a pulse train, and it
//   has that pitch.
//   Grains read with the cubic tap, because a grain at a pitch ratio is a
//   sweeping read.
//   Hann windows. Equal-power pan per grain across `spread`.
//   A grain that runs off the live write head is a grain reading the future;
//   the position is clamped a window behind the head, and a grain whose
//   pitch would carry it past the head before it ends is SHORTENED to fit.
//   The level is the physical sum of the grains -- no hidden normalisation,
//   because the overlap (density × size) is the user's texture control and a
//   gain that moved with it would fight the knob.
//
// Included by dsp.h. Uses Random from random.h.
#pragma once
#ifndef SONORE_DSP_H_INCLUDED
#error "granular.h is part of dsp.h and needs what it includes first: include <sonore/dsp.h> instead."
#endif
#include <cmath>
#include <cstddef>
#include <cstdint>
#include "audio.h"
#include "random.h"

namespace sonore {

/**
 * SIZE: BufferSamples floats -- 768 KB at the default four seconds at 48 kHz.
 * A member of the plugin, never a local.
 */
template <int MaxGrains = 32, int BufferSamples = 192000>
class GrainEngine {
  static_assert(MaxGrains >= 1 && MaxGrains <= 256, "1..256 grains");
  static_assert(BufferSamples >= 4096, "the buffer needs room for a grain");

public:
  /** The cloud's settings, set together with setParameters(), which puts
   *  the default in place of a field that is not finite and clamps the rest:
   *  size 1..2000 ms (and at most half the buffer), density 0..1000 grains a
   *  second, position, jitter, spread and reverse 0..1, pitch a playback
   *  ratio within -16..16 (2 is an octave up), pitch jitter 0..48 semitones. */
  struct Parameters {
    float sizeMs = 80.0f;        ///< grain length in ms, 1..2000 and at most half the buffer
    float density = 20.0f;       ///< grains started per second, 0..1000; fractions work, 0 starts none
    float position = 0.1f;       ///< 0..1: how far back from the write head (live) / into the buffer (loaded)
    float jitter = 0.05f;        ///< 0..1 of the buffer, random start scatter either side of `position`
    float pitch = 1.0f;          ///< playback ratio, -16..16: 2 = up an octave, negative reads backwards
    float pitchJitter = 0.0f;    ///< 0..48 semitones of random detune per grain, either way
    float spread = 0.5f;         ///< 0..1 stereo width of the cloud: 0 puts every grain in the centre
    float reverse = 0.0f;        ///< 0..1 probability a grain plays backwards
  };

  /** The rate in Hz; one of 1 or less, or NaN, is taken as 48000. Works the
   *  grain length and the schedule out again from the current Parameters;
   *  the buffer, a loaded sample and the sounding grains are kept. Call it
   *  before the first render() and on every rate change. */
  void setSampleRate(float sr) {
    sr_ = sr > 1.0f ? sr : 48000.0f;
    setParameters(params_);
  }
  /** New cloud settings, cleaned up as Parameters describes. They apply to
   *  the grains started from now on: a grain already sounding keeps its
   *  start, length, rate and pan. Allocation-free, so it can follow a knob
   *  every block. */
  void setParameters(const Parameters& p) {
    // Every field finite and in range before anything reads it. A NaN
    // position, pitch or jitter became a NaN read position, (int) NaN an
    // index, and readCubic read outside the buffer -- the host crashed. A
    // huge pitch was the other way to the same place: its read position
    // was too large for `pos -= span` to change, and the wrap never ended.
    const Parameters defaults{};
    auto finite = [](float v, float fallback) { return std::isfinite(v) ? v : fallback; };
    params_.sizeMs = finite(p.sizeMs, defaults.sizeMs);
    params_.density = finite(p.density, defaults.density);
    params_.position = clampf(finite(p.position, defaults.position), 0.0f, 1.0f);
    params_.jitter = clampf(finite(p.jitter, defaults.jitter), 0.0f, 1.0f);
    params_.pitch = clampf(finite(p.pitch, defaults.pitch), -16.0f, 16.0f);
    params_.pitchJitter = clampf(finite(p.pitchJitter, defaults.pitchJitter), 0.0f, 48.0f);
    params_.spread = clampf(finite(p.spread, defaults.spread), 0.0f, 1.0f);
    params_.reverse = clampf(finite(p.reverse, defaults.reverse), 0.0f, 1.0f);
    sizeSamples_ = clampf(params_.sizeMs, 1.0f, 2000.0f) * 0.001f * sr_;
    if (sizeSamples_ > (float) (BufferSamples / 2)) sizeSamples_ = (float) (BufferSamples / 2);
    schedInc_ = clampf(params_.density, 0.0f, 1000.0f) / sr_;
  }
  /** The seed of the random scatter: start jitter, detune, direction and
   *  pan. The next grain draws from the new seed's sequence, and reset()
   *  returns to its start, so the same seed and the same input give the same
   *  cloud. */
  void setSeed(uint64_t seed) { random_.setSeed(seed); }
  /** Silence the cloud: no grain sounding, the scheduler at its start, and
   *  -- live -- an empty buffer. A LOADED sample stays loaded. reset() is
   *  what a host calls on every stop and every prepare, and it used to drop
   *  the sample too: the plugin went silent on the first transport stop
   *  until the user loaded the file again. unload() is the way back to the
   *  live buffer. */
  void reset() {
    for (auto& g : grains_) g.active = false;
    sched_ = 0.0f;
    started_ = 0;
    random_.restart(); // the same cloud again, for the same input
    if (!loaded_) {
      for (int i = 0; i < BufferSamples; ++i) buffer_[i] = 0.0f;
      write_ = 0;
    }
  }
  /** Back to the live buffer, empty. */
  void unload() {
    loaded_ = false;
    loadedLength_ = 0;
    reset();
  }

  /** Feed the live buffer. Call once per sample before render(); skip it to
   *  freeze what is there. */
  inline void write(float x) {
    buffer_[write_] = x;
    if (++write_ >= BufferSamples) write_ = 0;
  }
  /** Load a sample instead of writing live; `position` then indexes into it.
   *
   *  Fewer than four samples -- or none, which is what a failed file read
   *  hands over -- is NO sample, and the engine returns to its live buffer.
   *  A loaded length of zero used to reach readCubic's `while (pos < 0) pos
   *  += span`, adding zero for ever on the audio thread; three made the start
   *  clamp's upper bound negative. */
  void load(const float* samples, size_t count) {
    unload();
    if (!samples || count < 4) return;
    const size_t n = count < (size_t) BufferSamples ? count : (size_t) BufferSamples;
    for (size_t i = 0; i < n; ++i) buffer_[i] = samples[i];
    loadedLength_ = (int) n;
    loaded_ = true;
    write_ = 0;
  }

  /** Grains sounding now, 0..MaxGrains. With all MaxGrains sounding, the
   *  next scheduled grain is skipped rather than one cut off mid-window. */
  int activeGrains() const {
    int n = 0;
    for (const auto& g : grains_) n += g.active ? 1 : 0;
    return n;
  }
  /** Grains started since reset(): the scheduler's claim, countable. */
  uint32_t grainsStarted() const { return started_; }

  /** One stereo frame of cloud. */
  inline void render(float& left, float& right) {
    // Schedule.
    sched_ += schedInc_;
    while (sched_ >= 1.0f) {
      sched_ -= 1.0f;
      spawn();
    }
    float l = 0.0f, r = 0.0f;
    for (auto& g : grains_) {
      if (!g.active) continue;
      const float t = g.age / g.length; // 0..1 through the grain
      // Hann: 0.5 - 0.5 cos(2 pi t), with the cosine as a quarter-turn sine.
      const float w = 0.5f - 0.5f * fastmath::sinTurns(t + 0.25f);
      const float pos = g.start + g.age * g.rate;
      const float s = readCubic(pos) * w;
      l += s * g.gainL;
      r += s * g.gainR;
      g.age += 1.0f;
      if (g.age >= g.length) g.active = false;
    }
    left = l;
    right = r;
  }

private:
  /** One sounding grain: its start in the buffer, how many samples it has
   *  played, its length, its read rate (negative plays it backwards) and its
   *  two pan gains. */
  struct Grain {
    /** False for a free slot. */
    bool active = false;
    /** In samples: the read position at age 0, the samples played so far
     *  and the length; then the read rate (negative backwards) and the two
     *  pan gains. */
    float start = 0.0f, age = 0.0f, length = 1.0f, rate = 1.0f, gainL = 0.7f, gainR = 0.7f;
  };

  void spawn() {
    Grain* slot = nullptr;
    for (auto& g : grains_)
      if (!g.active) { slot = &g; break; }
    if (!slot) return; // the cloud is full: a grain is skipped, never stolen mid-window
    ++started_;
    const float span = loaded_ ? (float) loadedLength_ : (float) BufferSamples;
    // Where to start. Live: `position` is a delay behind the head, and the
    // grain must finish before it catches the head. Loaded: an index.
    const float jitter = (random_.nextFloat() * 2.0f - 1.0f) * params_.jitter * span;
    float start;
    const float rate = params_.pitch *
                       std::pow(2.0f, (random_.nextFloat() * 2.0f - 1.0f) * params_.pitchJitter / 12.0f);
    const bool reverse = random_.nextFloat() < params_.reverse;
    float length = sizeSamples_;
    if (loaded_) {
      start = clampf(params_.position * span + jitter, 0.0f, span - 4.0f);
    } else {
      const float behind = clampf(params_.position, 0.0f, 1.0f) * span;
      // Furthest forward the grain will read: start + length·rate must stay
      // behind the head by a few samples. A grain that cannot fit at this
      // pitch is shortened rather than allowed to read the future.
      //
      // And furthest BACK: the head keeps writing while the grain plays,
      // over the oldest samples, one a sample. A reversed grain ends at its
      // oldest sample, length samples later, so it must start at least a
      // grain's length inside the buffer; a forward one slower than the head
      // falls behind it by length·(1 - rate). Started at the very end, near
      // position 1, a reversed grain read samples the head had just written
      // -- the newest audio in the middle of the oldest, a step of 1.40 in a
      // tone whose own steps were 0.009.
      //
      // A NEGATIVE pitch reads backwards, towards the oldest samples, while
      // the head writes over them from the other side: they are used up at
      // one plus the rate's size a sample. Counted as one, a pitch of -1 from
      // the far end read the head's newest audio halfway through the grain,
      // a step of 0.21 in a tone whose own steps were 0.004.
      const float rateUp = rate > 0.0f ? rate : 0.0f;
      const float rateDown = rate < 0.0f ? -rate : 0.0f;
      const float overrun = (reverse ? 1.0f : (rate < 1.0f ? 1.0f - rateUp : 0.0f)) + rateDown;
      if (length * (rateUp + overrun) + 12.0f > span) length = (span - 12.0f) / (rateUp + overrun);
      const float minBehind = length * rateUp + 4.0f;
      const float maxBack = span - 4.0f - length * overrun;
      float back = behind - jitter;
      if (back < minBehind) back = minBehind;
      if (back > maxBack) back = maxBack;
      start = (float) write_ - back;
      while (start < 0.0f) start += span;
    }
    slot->active = true;
    slot->start = reverse ? start + length * rate : start;
    slot->rate = reverse ? -rate : rate;
    slot->age = 0.0f;
    slot->length = length > 1.0f ? length : 1.0f;
    const float pan = (random_.nextFloat() * 2.0f - 1.0f) * clampf(params_.spread, 0.0f, 1.0f);
    const float angle = (pan + 1.0f) * 0.25f * kPi;
    slot->gainL = std::cos(angle);
    slot->gainR = std::sin(angle);
  }

  inline float readCubic(float pos) const {
    const float span = loaded_ ? (float) loadedLength_ : (float) BufferSamples;
    if (!std::isfinite(pos)) return 0.0f;
    // One wrap however far out, instead of a loop per span.
    pos -= span * std::floor(pos / span);
    int i1 = (int) pos;
    if (i1 < 0) i1 = 0;
    if (i1 >= (int) span) i1 = (int) span - 1;
    const float frac = pos - (float) i1;
    const int n = (int) span;
    const int i0 = (i1 + n - 1) % n, i2 = (i1 + 1) % n, i3 = (i1 + 2) % n;
    return CatmullRomKernel::at4(buffer_[i0], buffer_[i1], buffer_[i2], buffer_[i3], frac);
  }

  float buffer_[BufferSamples]{};
  Grain grains_[MaxGrains];
  Random random_;
  Parameters params_{};
  float sr_ = 48000.0f, sizeSamples_ = 3840.0f, schedInc_ = 0.0f, sched_ = 0.0f;
  int write_ = 0, loadedLength_ = 0;
  uint32_t started_ = 0;
  bool loaded_ = false;
};

} // namespace sonore
