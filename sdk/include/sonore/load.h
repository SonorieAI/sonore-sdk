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
#include <cmath>
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
    cachedFrames_ = 0; // the coefficients were for the old rate
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
    /** [audio-thread] Start timing a block of `numFrames` frames; the time is
     *  recorded into `owner` when this goes out of scope, so `owner` must
     *  outlive it. One clock read; no allocation, no lock. */
    ScopedTimer(LoadMeasurer& owner, uint32_t numFrames)
        : owner_(owner), frames_(numFrames), start_(std::chrono::steady_clock::now()) {}
    /** Read the clock again and record() the block. */
    ~ScopedTimer() {
      const auto end = std::chrono::steady_clock::now();
      owner_.record(std::chrono::duration<double>(end - start_).count(), frames_);
    }
    /** Not copyable: a copy would record the block twice. */
    ScopedTimer(const ScopedTimer&) = delete;
    /** Not assignable, for the same reason. */
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

  /** [any thread] Blocks recorded since the last reset(). A block of zero
   *  frames is not one. */
  uint64_t blocks() const { return blocks_.load(std::memory_order_relaxed); }

  /** [audio-thread] Record one block directly, for a caller that already has
   *  the timing. */
  void record(double seconds, uint32_t numFrames) {
    if (numFrames == 0) return;
    // The budget and both smoothing coefficients depend only on the block
    // length and the rate, and a host keeps one block length for as long as
    // it plays. They were worked out on every block -- a division and an
    // exp() -- and are now
    // worked out when the length changes, by the same expressions, so every
    // reading is the same float it was.
    if (numFrames != cachedFrames_) {
      cachedBudget_ = (double) numFrames / sampleRate_;
      if (cachedBudget_ <= 0.0) return;
      riseSmoothing_ = (float) (1.0 - std::exp(-cachedBudget_ / 0.004));
      fallSmoothing_ = (float) (1.0 - std::exp(-cachedBudget_ / 0.35));
      cachedFrames_ = numFrames;
    }
    const double budget = cachedBudget_;
    const float instant = (float) (seconds / budget);

    // Asymmetric on purpose. Rising is followed almost immediately and
    // falling slowly, because the question a load meter answers is "is this
    // about to break up", and a spike that decays away before anyone sees it
    // is a spike they still heard.
    //
    // In SECONDS, not per block: a fixed 0.5 / 0.03 per call meant the
    // meter's time constants scaled with the block size -- a host at 32
    // frames saw it fall sixteen times faster than one at 512, and the same
    // load read differently in every DAW. About 4 ms up -- what one block
    // of 128 frames did -- and 350 ms down, what 512 frames did.
    const float previous = load_.load(std::memory_order_relaxed);
    const float smoothing = instant > previous ? riseSmoothing_ : fallSmoothing_;
    load_.store(previous + smoothing * (instant - previous), std::memory_order_relaxed);

    if (instant > peak_.load(std::memory_order_relaxed))
      peak_.store(instant, std::memory_order_relaxed);
    // A load and a store, not fetch_add: this thread is the only writer (the
    // averages above are read-modify-writes of the same kind), and a locked
    // add is a full barrier on x86, on every block, for a counter only a
    // display reads.
    if (instant >= 1.0f)
      xruns_.store(xruns_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    blocks_.store(blocks_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
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
  // The writer's own: what record() last worked out for a block length.
  uint32_t cachedFrames_ = 0;
  double cachedBudget_ = 0.0;
  float riseSmoothing_ = 0.0f, fallSmoothing_ = 0.0f;
};

} // namespace sonore
