// SPDX-License-Identifier: Apache-2.0
//
// Work that happens somewhere other than the two threads that matter.
//
// ── The two threads it must not be ─────────────────────────────────────────
//
// Reading a hundred-megabyte sample cannot happen on the AUDIO thread: it
// allocates, it blocks on a disk, and either of those is a dropout. It cannot
// happen on the UI thread either, because that one belongs to the host -- a
// plugin that spends four seconds in a message handler is a plugin that freezes
// the DAW, and the user blames the DAW.
//
// So: a small pool, owned by the plugin, that neither of those threads waits
// on.
//
// ── Why the default is two threads and not sixteen ─────────────────────────
//
// A thread pool conventionally defaults to the number of cores, which is
// right for an application because there is one of it. A plugin is not one of it. A session
// with forty instances -- ordinary in a mix -- would create forty pools, and on
// a sixteen-core machine that is six hundred threads, all of them idle, all of
// them costing a stack and a scheduler slot.
//
// And they would buy nothing. What a plugin does in the background is READ
// FILES. That is I/O-bound: two threads saturate a disk queue as well as
// sixteen do, and on a network share more threads make it slower. So the
// default is two, the cap is four, and a caller who genuinely has parallel
// CPU work can ask for more and say why.
//
// ── Cancellation is not optional ────────────────────────────────────────────
//
// A user who opens a folder of forty thousand samples and immediately opens a
// different one must not wait for the first scan. So every job is handed a flag
// and is expected to look at it; a job that ignores it is a job that delays
// close() for as long as it runs, and close() is called from a destructor the
// host is waiting on.
#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace sonore {

class ThreadPool {
public:
  /** A job. `shouldStop` becomes true when the pool is closing or the job has
   *  been cancelled; a long one is expected to check it. */
  using Job = std::function<void(const std::atomic<bool>& shouldStop)>;

  /**
   * `threads` of 0 asks for the default -- two, capped at four. See the header
   * for why that is not the core count.
   */
  explicit ThreadPool(int threads = 0) {
    int count = threads;
    if (count <= 0) {
      const unsigned cores = std::thread::hardware_concurrency();
      count = cores >= 4 ? 2 : 1;
    }
    if (count > kMaxThreads) count = kMaxThreads;

    workers_.reserve((size_t) count);
    for (int i = 0; i < count; ++i) workers_.emplace_back([this]() { work(); });
  }

  ~ThreadPool() { close(); }

  ThreadPool(const ThreadPool&) = delete;
  ThreadPool& operator=(const ThreadPool&) = delete;

  int numThreads() const { return (int) workers_.size(); }

  /**
   * Queue a job.
   *
   * Returns false once the pool is closing, so a caller cannot silently lose
   * work into a pool that will never run it -- which during teardown is exactly
   * when a "load this sample" would otherwise vanish.
   */
  bool addJob(Job job) {
    if (!job) return false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (closing_) return false;
      queue_.push_back(std::move(job));
      ++outstanding_;
    }
    condition_.notify_one();
    return true;
  }

  /** Queued but not yet started, plus running. What "is it still working"
   *  means to a caller showing a spinner. */
  int numJobsOutstanding() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return outstanding_;
  }

  /**
   * Drop everything not yet started and ask running jobs to stop.
   *
   * The pool stays usable -- new jobs may be added afterwards. This is what a
   * browser calls when the user opens a different folder: the previous scan is
   * pointless, and waiting for it before starting the new one is the delay the
   * user notices.
   */
  void cancelPendingJobs() {
    std::lock_guard<std::mutex> lock(mutex_);
    outstanding_ -= (int) queue_.size();
    queue_.clear();
    // A generation counter rather than a flag that has to be cleared: a job
    // started before the cancel sees a stale generation and stops, and one
    // added after it does not, with no window in between where a flag is set
    // and the new job reads it.
    ++generation_;
  }

  /**
   * Wait until nothing is outstanding, or the timeout runs out.
   *
   * Returns whether it drained. NEVER call this from the audio thread -- it
   * takes a lock and waits on a condition, and it is here for tests and for a
   * shutdown that has already decided to block.
   */
  bool waitForAll(int timeoutMs = 5000) {
    std::unique_lock<std::mutex> lock(mutex_);
    return idle_.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                          [this]() { return outstanding_ == 0; });
  }

  /**
   * Stop taking work, ask everything running to stop, and join.
   *
   * Called by the destructor, and safe to call twice -- a plugin torn down
   * twice by a host that is confused is not a crash worth having.
   */
  void close() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (closing_) return;
      closing_ = true;
      outstanding_ -= (int) queue_.size();
      queue_.clear();
      ++generation_;
    }
    condition_.notify_all();
    for (std::thread& worker : workers_)
      if (worker.joinable()) worker.join();
    workers_.clear();
  }

  bool isClosing() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return closing_;
  }

  /** Two, unless a caller asks otherwise, and never more than this. */
  static constexpr int kMaxThreads = 4;

private:
  void work() {
    // Per-worker, so one job's cancellation cannot be seen by another. The
    // reference handed to a job outlives the job, which is what makes taking it
    // by const& safe.
    std::atomic<bool> shouldStop{false};

    for (;;) {
      Job job;
      uint64_t startedAt = 0;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        condition_.wait(lock, [this]() { return closing_ || !queue_.empty(); });
        if (closing_ && queue_.empty()) return;
        if (queue_.empty()) continue;
        job = std::move(queue_.front());
        queue_.pop_front();
        startedAt = generation_;
      }

      shouldStop.store(false, std::memory_order_release);

      // The job runs OUTSIDE the lock, obviously -- and the flag it is handed
      // is updated by a small poller rather than by the job checking the mutex,
      // because a job asking "should I stop" ten thousand times a second must
      // not contend with addJob.
      Watcher watcher(*this, shouldStop, startedAt);

      // A throwing job must not take its worker with it. One bad sample loader
      // would otherwise silently reduce the pool to nothing, and the symptom is
      // that background work stops happening at all.
      try {
        job(shouldStop);
      } catch (...) {
      }

      watcher.stop();

      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (outstanding_ > 0) --outstanding_;
        if (outstanding_ == 0) idle_.notify_all();
      }
    }
  }

  /**
   * Turns "the pool was closed or cancelled" into the flag a job is watching.
   *
   * A thread per running job sounds heavy and is not: it sleeps, and there are
   * at most four of them. The alternative -- having the job take the pool's
   * mutex to ask -- puts every long loop in contention with addJob.
   */
  class Watcher {
  public:
    Watcher(ThreadPool& pool, std::atomic<bool>& flag, uint64_t generation)
        : pool_(pool), flag_(flag), generation_(generation) {
      thread_ = std::thread([this]() { run(); });
    }

    void stop() {
      done_.store(true, std::memory_order_release);
      if (thread_.joinable()) thread_.join();
    }

    ~Watcher() { stop(); }

  private:
    void run() {
      while (!done_.load(std::memory_order_acquire)) {
        {
          std::lock_guard<std::mutex> lock(pool_.mutex_);
          if (pool_.closing_ || pool_.generation_ != generation_) {
            flag_.store(true, std::memory_order_release);
            return;
          }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
    }

    ThreadPool& pool_;
    std::atomic<bool>& flag_;
    uint64_t generation_;
    std::atomic<bool> done_{false};
    std::thread thread_;
  };

  mutable std::mutex mutex_;
  std::condition_variable condition_;
  std::condition_variable idle_;
  std::deque<Job> queue_;
  std::vector<std::thread> workers_;
  int outstanding_ = 0;
  uint64_t generation_ = 0;
  bool closing_ = false;
};

} // namespace sonore
