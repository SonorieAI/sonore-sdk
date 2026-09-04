// SPDX-License-Identifier: Apache-2.0
//
// LoadMeasurer: how much of the audio deadline a DSP is using.
//
// ── Why this belongs in THIS SDK in particular ──────────────────────────────
//
// A hand-written plugin is profiled by the person who wrote it. A generated
// one has no such person. The DSP in a Sonorie plugin was written by a model
// that has never heard it run, cannot know what a 128-frame block at 48 kHz
// costs, and will happily emit a 64-stage filter bank inside a per-sample
// loop.
//
// So the plugin has to be able to say so itself.
//
// ── What "load" means here ──────────────────────────────────────────────────
//
// The fraction of a block's REAL-TIME BUDGET spent computing it. A block of
// 128 frames at 48 kHz must be finished within 2.67 ms, because that is when
// the next one is due. Taking 1.33 ms is a load of 0.5; taking 2.67 ms is 1.0
// and the next block is already late.
//
// This is not the same as "percent of one CPU core" and the difference
// matters: a host running twenty plugins gives each of them the same deadline,
// so a load of 0.5 means this plugin alone would fill half the budget of a
// machine doing nothing else.
//
// ── Cost ────────────────────────────────────────────────────────────────────
//
// Two clock reads per block and a handful of arithmetic. steady_clock on
// Windows is QueryPerformanceCounter and on Linux a vDSO read of the monotonic
// clock; neither takes a lock or enters the kernel on any machine this will
// run on. At a 128-frame block that is two reads per 2.67 ms, which is not
// measurable against the audio itself -- confirmed rather than assumed, in
// the tests.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

namespace sonore {

/**
 * Measures the time spent in a process callback against that block's deadline.
 *
 * Written from the audio thread and read from any other, so the published
 * values are atomics. They are stored with relaxed ordering: a reader is
 * drawing a meter, and a meter that is one block stale is not wrong in any way
 * a person can see.
 */
class LoadMeasurer {
public:
  /** Call from prepare(). Zeroes the history: a rate change makes every
   *  previous measurement a measurement of something else. */
  void reset(double sampleRate) {
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    load_.store(0.0f, std::memory_order_relaxed);
    peak_.store(0.0f, std::memory_order_relaxed);
    xruns_.store(0, std::memory_order_relaxed);
    blocks_.store(0, std::memory_order_relaxed);
  }

  /**
   * Time one block.
   *
   * RAII, because the alternative is a matched pair of calls and a return
   * path that skips the second one. A DSP that bails out early on silence is
   * a normal thing to write, and it must not leave the meter reading whatever
   * the last complete block cost.
   */
  class ScopedTimer {
  public:
    ScopedTimer(LoadMeasurer& owner, uint32_t numFrames)
        : owner_(owner), frames_(numFrames), start_(std::chrono::steady_clock::now()) {}
    ~ScopedTimer() {
      const auto end = std::chrono::steady_clock::now();
      owner_.record(std::chrono::duration<double>(end - start_).count(), frames_);
    }
    ScopedTimer(const ScopedTimer&) = delete;
    ScopedTimer& operator=(const ScopedTimer&) = delete;

  private:
    LoadMeasurer& owner_;
    uint32_t frames_;
    std::chrono::steady_clock::time_point start_;
  };

  /** The smoothed load, 0..1 and beyond. Above 1.0 is not clamped: a caller
   *  needs to see 1.4 to know how much trouble it is in, and a meter pinned at
   *  the top says only that something is wrong. */
  float load() const { return load_.load(std::memory_order_relaxed); }

  /** The worst single block since the last reset. What actually causes a click
   *  is one late block, not a high average -- the average is what a person
   *  sees and the peak is what they hear. */
  float peakLoad() const { return peak_.load(std::memory_order_relaxed); }

  /** Blocks that took longer than their own deadline. */
  uint64_t xruns() const { return xruns_.load(std::memory_order_relaxed); }

  uint64_t blocks() const { return blocks_.load(std::memory_order_relaxed); }

  /** [audio-thread] Record one block directly, for a caller that already has
   *  the timing. */
  void record(double seconds, uint32_t numFrames) {
    if (numFrames == 0) return;
    const double budget = (double) numFrames / sampleRate_;
    if (budget <= 0.0) return;
    const float instant = (float) (seconds / budget);

    // Asymmetric on purpose. Rising is followed almost immediately and
    // falling slowly, because the question a load meter answers is "is this
    // about to break up", and a spike that decays away before anyone sees it
    // is a spike they still heard.
    const float previous = load_.load(std::memory_order_relaxed);
    const float smoothing = instant > previous ? 0.5f : 0.03f;
    load_.store(previous + smoothing * (instant - previous), std::memory_order_relaxed);

    if (instant > peak_.load(std::memory_order_relaxed))
      peak_.store(instant, std::memory_order_relaxed);
    if (instant >= 1.0f) xruns_.fetch_add(1, std::memory_order_relaxed);
    blocks_.fetch_add(1, std::memory_order_relaxed);
  }

  /** Forget the peak, keeping the running average. For a UI with a "reset
   *  peak" affordance, which every load meter worth reading has. */
  void clearPeak() { peak_.store(load_.load(std::memory_order_relaxed), std::memory_order_relaxed); }

private:
  double sampleRate_ = 48000.0;
  std::atomic<float> load_{0.0f};
  std::atomic<float> peak_{0.0f};
  std::atomic<uint64_t> xruns_{0};
  std::atomic<uint64_t> blocks_{0};
};

} // namespace sonore
