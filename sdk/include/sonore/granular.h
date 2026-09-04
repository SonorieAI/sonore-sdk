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
  struct Parameters {
    float sizeMs = 80.0f;        // grain length
    float density = 20.0f;       // grains per second
    float position = 0.1f;       // 0..1: how far back from the write head (live) / into the buffer (loaded)
    float jitter = 0.05f;        // 0..1 of the buffer, random start scatter
    float pitch = 1.0f;          // playback ratio, 2 = up an octave
    float pitchJitter = 0.0f;    // semitones of random detune per grain
    float spread = 0.5f;         // 0..1 stereo width of the cloud
    float reverse = 0.0f;        // 0..1 probability a grain plays backwards
  };

  void setSampleRate(float sr) {
    sr_ = sr > 1.0f ? sr : 48000.0f;
    setParameters(params_);
  }
  void setParameters(const Parameters& p) {
    params_ = p;
    sizeSamples_ = clampf(p.sizeMs, 1.0f, 2000.0f) * 0.001f * sr_;
    if (sizeSamples_ > (float) (BufferSamples / 2)) sizeSamples_ = (float) (BufferSamples / 2);
    schedInc_ = clampf(p.density, 0.0f, 1000.0f) / sr_;
  }
  void setSeed(uint64_t seed) { random_.setSeed(seed); }
  void reset() {
    for (int i = 0; i < BufferSamples; ++i) buffer_[i] = 0.0f;
    for (auto& g : grains_) g.active = false;
    write_ = 0;
    sched_ = 0.0f;
    loaded_ = false;
    started_ = 0;
  }

  /** Feed the live buffer. Call once per sample before render(); skip it to
   *  freeze what is there. */
  inline void write(float x) {
    buffer_[write_] = x;
    if (++write_ >= BufferSamples) write_ = 0;
  }
  /** Load a sample instead of writing live; `position` then indexes into it. */
  void load(const float* samples, size_t count) {
    reset();
    const size_t n = count < (size_t) BufferSamples ? count : (size_t) BufferSamples;
    for (size_t i = 0; i < n; ++i) buffer_[i] = samples[i];
    loadedLength_ = (int) n;
    loaded_ = true;
    write_ = 0;
  }

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
  struct Grain {
    bool active = false;
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
      if (rate > 0.0f && length * rate + 8.0f > span) length = (span - 8.0f) / rate;
      const float minBehind = length * (rate > 0.0f ? rate : 0.0f) + 4.0f;
      float back = behind - jitter;
      if (back < minBehind) back = minBehind;
      if (back > span - 4.0f) back = span - 4.0f;
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
    while (pos < 0.0f) pos += span;
    while (pos >= span) pos -= span;
    const int i1 = (int) pos;
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
