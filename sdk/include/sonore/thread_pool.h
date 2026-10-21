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
//
// ── The other kind: a REALTIME pool ─────────────────────────────────────────
//
// Built with RealtimeThreadHints, the pool is for the opposite job: helpers
// that render part of a block, under the audio deadline. Each worker makes
// itself a real-time thread (Mach's time-constraint policy, from the hints)
// and joins the audio workgroup the pool is given -- itself, on its own
// thread, which is the only way os_workgroup_join works -- before it takes its
// next job. See audio_workgroup.h for where a workgroup comes from: the
// standalone's device, an AU host's render context, and from CLAP, VST3 and
// LV2 nowhere, because those formats have no way to say.
//
// Two ways in. setAudioWorkgroup() from any thread but the audio one: it takes
// the pool's lock and wakes every worker. offerAudioWorkgroup() FROM the audio
// thread, where a DSP's workgroupChanged() hook runs in an AU: wait-free, a
// retain and an atomic exchange, and the workers pick it up before their next
// job. What a realtime pool does NOT change is the queue: addJob() still
// takes a lock and may allocate for the job's closure, so a DSP that feeds it
// from the audio thread is trading that for the parallelism, knowingly.
//
// Everything about workgroups and real-time scheduling is macOS. Elsewhere a
// realtime pool runs its jobs exactly as an ordinary one does, and
// workgroupState() says that nothing was joined and why. Compiled for macOS
// and never run there: no Mac in this loop (see audio_workgroup.h).
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "audio_workgroup.h"

namespace sonore {

/**
 * A small set of worker threads, owned by a plugin or an application, for
 * work that must not run on the audio thread or the UI thread -- reading
 * files above all. Mirrors the reference's class of the same name, with a
 * job as a function handed its stop flag. See the file comment for why the
 * default is two threads (one on a machine with fewer than four cores) and
 * for the realtime kind, built with RealtimeThreadHints.
 *
 * addJob, cancelPendingJobs, waitForAll and the queries take a lock: any
 * thread but the audio one (the file comment says what a realtime pool
 * allows). Jobs start on the workers in the order queued and are expected to
 * check `shouldStop`; close(), which the destructor calls, waits for the
 * running ones to return, so a job that ignores the flag holds up whoever
 * destroys the pool. A job must not close or destroy its own pool: that
 * would join the thread it is running on.
 */
class ThreadPool {
public:
  /** A job. `shouldStop` becomes true when the pool is closing or the job has
   *  been cancelled; a long one is expected to check it. */
  using Job = std::function<void(const std::atomic<bool>& shouldStop)>;

  /**
   * `threads` of 0 asks for the default -- two, or one on a machine with
   * fewer than four cores -- and any count is capped at kMaxThreads (four).
   * See the header for why that is not the core count.
   */
  explicit ThreadPool(int threads = 0) {
    int count = threads;
    if (count <= 0) {
      const unsigned cores = std::thread::hardware_concurrency();
      count = cores >= 4 ? 2 : 1;
    }
    if (count > kMaxThreads) count = kMaxThreads;
    start(count);
  }

  /**
   * A REALTIME pool of `threads` workers (1 to kMaxRealtimeThreads), each a
   * real-time thread with these hints and each joining the audio workgroup
   * the pool is given. Give the hints the block it will render --
   * RealtimeThreadHints::forBlock(rate, block) -- and change them with
   * setRealtimeHints() when the block does.
   */
  ThreadPool(int threads, const RealtimeThreadHints& hints) : realtime_(true), hints_(hints) {
    // Resolved here, on the thread building the pool: dlsym takes a lock, and
    // the first offer may come from the audio thread.
    (void) AudioWorkgroup::isSupported();
    int count = threads > 0 ? threads : 1;
    if (count > kMaxRealtimeThreads) count = kMaxRealtimeThreads;
    start(count);
  }

  /** close()s the pool -- dropping what has not started and waiting for
   *  what has -- and releases a workgroup offer nobody took. */
  ~ThreadPool() {
    close();
    // An offer nobody took is still a reference.
    const uintptr_t offered = offered_.exchange(0, std::memory_order_acq_rel);
    if (offered > kOfferedNone) AudioWorkgroup::adopting((void*) offered).reset();
  }

  ///@{
  /** Not copyable: the workers hold a pointer to this pool. */
  ThreadPool(const ThreadPool&) = delete;
  ThreadPool& operator=(const ThreadPool&) = delete;
  ///@}

  /** How many worker threads it has; 0 after close(). */
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
    // Every job running NOW is told, under the same lock that hands jobs out:
    // one started before this sees its flag set, and one started after it is
    // registered with a fresh flag, with no window between the two.
    stopRunningLocked();
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
      stopRunningLocked();
    }
    condition_.notify_all();
    for (std::thread& worker : workers_)
      if (worker.joinable()) worker.join();
    workers_.clear();
  }


  /** True once close() has begun; addJob() then refuses. Any thread. */
  bool isClosing() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return closing_;
  }

  /** Two, unless a caller asks otherwise, and never more than this. */
  static constexpr int kMaxThreads = 4;
  /** A realtime pool renders, so it may be as wide as the machine's
   *  performance cores (AudioWorkgroup::maxParallelThreadCount) -- up to this. */
  static constexpr int kMaxRealtimeThreads = 16;

  /** True for a pool built with RealtimeThreadHints. */
  bool isRealtime() const { return realtime_; }

  // ── Realtime pools: the audio workgroup ──────────────────────────────────

  /**
   * [any thread but the audio one] Join every worker to this workgroup, or
   * an empty one to leave whatever they are in.
   *
   * Takes the lock and wakes the workers, each of which leaves its old group
   * and joins this one on its own thread. False on a pool that is not
   * realtime: Apple's audio workgroups are for real-time threads, and a
   * file-loading worker inside one would be charged against the audio
   * deadline for its disk waits.
   */
  bool setAudioWorkgroup(const AudioWorkgroup& workgroup) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!realtime_) {
        message_ = "not a realtime pool: audio workgroups are for real-time threads, so this "
                   "pool's workers were left as they are";
        return false;
      }
      // A new generation even for the same group: a worker whose join failed
      // -- the group was cancelled, the thread was elsewhere -- tries again,
      // and one already in it does nothing (settle() asks first).
      workgroup_ = workgroup;
      ++generation_;
    }
    condition_.notify_all();
    return true;
  }

  /**
   * [audio thread] The same, without waiting for anything: a retain and an
   * atomic exchange. The workers adopt it before their next job, which is
   * when it matters -- the jobs ARE the parallel render. An offer replaced
   * before any worker took it is released here, on this thread: a reference
   * count dropping, which frees nothing while the group's owner still holds
   * it.
   */
  void offerAudioWorkgroup(const AudioWorkgroup& workgroup) noexcept {
    if (!realtime_) {
      offerRefused_.store(true, std::memory_order_relaxed);
      return;
    }
    AudioWorkgroup copy = workgroup;
    const uintptr_t value = copy ? (uintptr_t) copy.detachHandle() : kOfferedNone;
    const uintptr_t previous = offered_.exchange(value, std::memory_order_acq_rel);
    if (previous > kOfferedNone) AudioWorkgroup::adopting((void*) previous).reset();
  }

  /**
   * [any thread but the audio one] New scheduling hints -- the block size or
   * the rate changed. Every worker applies them to itself before its next
   * job. False on a pool that is not realtime.
   */
  bool setRealtimeHints(const RealtimeThreadHints& hints) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!realtime_) {
        message_ = "not a realtime pool: built without RealtimeThreadHints";
        return false;
      }
      hints_ = hints;
      ++generation_;
    }
    condition_.notify_all();
    return true;
  }

  /** What the workers did with the last workgroup and hints they were given. */
  struct WorkgroupState {
    /** How many workers the pool has. */
    int workers = 0;
    /** Workers that are members of the pool's workgroup now. */
    int joined = 0;
    /** Workers running with the real-time policy now. */
    int realtime = 0;
    /** Whether the pool holds a workgroup at all. */
    bool hasWorkgroup = false;
    /** Bumped by every new workgroup or new hints. */
    uint64_t generation = 0;
    /** Workers that have acted on `generation` -- all of them, once settled. */
    int settled = 0;
    /** Empty when everything asked for happened; otherwise why not. */
    std::string message;
  };

  /** A snapshot of the workers' reports, for a diagnostic page or a test.
   *  Takes the lock: not from the audio thread. */
  WorkgroupState workgroupState() const {
    std::lock_guard<std::mutex> lock(mutex_);
    WorkgroupState state;
    state.workers = (int) reports_.size();
    state.hasWorkgroup = workgroup_.isValid();
    state.generation = generation_;
    for (const WorkerReport& report : reports_) {
      if (report.joined) ++state.joined;
      if (report.realtime) ++state.realtime;
      if (report.generation == generation_) ++state.settled;
      if (state.message.empty() && !report.message.empty()) state.message = report.message;
    }
    if (state.message.empty()) state.message = message_;
    if (state.message.empty() && offerRefused_.load(std::memory_order_relaxed))
      state.message = "a workgroup was offered to a pool that is not realtime, and refused";
    return state;
  }

private:
  /** One worker's account of itself, written by that worker under the lock. */
  struct WorkerReport {
    uint64_t generation = 0; ///< the last generation it acted on
    bool joined = false;     ///< a member of the pool's workgroup now
    bool realtime = false;   ///< running with the real-time policy now
    std::string message;     ///< why something asked for did not happen; empty if it did
  };

  /** offered_'s values: 0 is an empty mailbox, kOfferedNone an offer of NO
   *  workgroup, anything else an os_workgroup_t carrying a reference. */
  static constexpr uintptr_t kOfferedNone = 1;

  void start(int count) {
    workers_.reserve((size_t) count);
    running_.reserve((size_t) count);
    reports_.assign((size_t) count, WorkerReport());
    // A realtime pool's workers have hints to apply before anything else,
    // so they start one generation behind.
    if (realtime_) generation_ = 1;
    for (int i = 0; i < count; ++i) workers_.emplace_back([this, i]() { work(i); });
  }

  /** Move an offer from the audio thread into the pool. Called with mutex_
   *  held, by a worker. */
  void adoptOfferLocked() {
    const uintptr_t value = offered_.exchange(0, std::memory_order_acq_rel);
    if (value == 0) return;
    AudioWorkgroup offered =
        value == kOfferedNone ? AudioWorkgroup() : AudioWorkgroup::adopting((void*) value);
    if (offered == workgroup_) return;
    workgroup_ = std::move(offered);
    ++generation_;
    condition_.notify_all();
  }

  /** Apply the pool's hints and workgroup to THIS thread -- the only thread
   *  that can do either -- and report what happened. Outside the lock: a
   *  thread_policy_set is a system call. */
  void settle(int index, WorkgroupToken& token, const AudioWorkgroup& workgroup,
              const RealtimeThreadHints& hints, uint64_t generation) {
    std::string message;
    bool promoted = false;
    if (realtime_) {
      std::string why;
      promoted = promoteCurrentThreadToRealtime(hints, &why);
      if (!promoted) message = why;
    }
    if (!token.isAttachedTo(workgroup)) {
      token.leave();
      if (workgroup && !workgroup.join(token) && message.empty()) message = token.describe();
    }
    if (!workgroup && message.empty() && realtime_ && !AudioWorkgroup::isSupported())
      message = AudioWorkgroup::unavailableReason();
    std::lock_guard<std::mutex> lock(mutex_);
    WorkerReport& report = reports_[(size_t) index];
    report.generation = generation;
    report.joined = token.isActive();
    report.realtime = promoted;
    report.message = std::move(message);
  }

  void work(int index) {
    // Per-worker, so one job's cancellation cannot be seen by another. The
    // reference handed to a job outlives the job, which is what makes taking it
    // by const& safe.
    std::atomic<bool> shouldStop{false};
    // This thread's workgroup membership. On this thread's stack because only
    // this thread may end it, which it does on the way out.
    WorkgroupToken token;
    uint64_t applied = 0;

    for (;;) {
      Job job;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        adoptOfferLocked();
        condition_.wait(lock, [this, &applied]() {
          return closing_ || !queue_.empty() || generation_ != applied;
        });
        adoptOfferLocked();
        if (generation_ != applied && !closing_) {
          // Copied under the lock -- a retain, no allocation -- and applied
          // outside it.
          const AudioWorkgroup workgroup = workgroup_;
          const RealtimeThreadHints hints = hints_;
          const uint64_t generation = generation_;
          lock.unlock();
          settle(index, token, workgroup, hints, generation);
          applied = generation;
          continue;
        }
        if (closing_ && queue_.empty()) break;
        if (queue_.empty()) continue;
        job = std::move(queue_.front());
        queue_.pop_front();
        // Registered where cancel and close can reach it, under the lock
        // that handed the job out. The first version started a watcher
        // thread per job to poll for cancellation and joined it afterwards
        // -- a thread created and destroyed for every file a browser
        // scanned, and a join that could wait out the watcher's 5 ms sleep.
        shouldStop.store(false, std::memory_order_release);
        running_.push_back(&shouldStop);
      }

      // A throwing job must not take its worker with it. One bad sample loader
      // would otherwise silently reduce the pool to nothing, and the symptom is
      // that background work stops happening at all.
      try {
        job(shouldStop);
      } catch (...) {
      }
      // The closure is part of the job, and goes before the job is counted
      // as done. Destroyed at the end of the loop instead, it outlived the
      // count: waitForAll() returned, and numJobsOutstanding() said 0, while
      // what the closure captured -- a stream holding its file open, the last
      // reference to a buffer, a guard whose destructor touches the caller's
      // state -- was still being torn down here, racing the caller.
      job = nullptr;

      {
        std::lock_guard<std::mutex> lock(mutex_);
        for (size_t i = 0; i < running_.size(); ++i)
          if (running_[i] == &shouldStop) {
            running_.erase(running_.begin() + (std::ptrdiff_t) i);
            break;
          }
        if (outstanding_ > 0) --outstanding_;
        if (outstanding_ == 0) idle_.notify_all();
      }
    }
    // Out of the workgroup before the thread ends, on the thread that joined.
    token.leave();
  }

  /** Ask every running job to stop. Called with mutex_ held. */
  void stopRunningLocked() {
    for (std::atomic<bool>* flag : running_) flag->store(true, std::memory_order_release);
  }

  mutable std::mutex mutex_;
  std::condition_variable condition_;
  std::condition_variable idle_;
  std::deque<Job> queue_;
  std::vector<std::thread> workers_;
  int outstanding_ = 0;
  /** The stop flags of the jobs running right now, one per busy worker. */
  std::vector<std::atomic<bool>*> running_;
  bool closing_ = false;

  // ── Realtime pools ───────────────────────────────────────────────────────
  const bool realtime_ = false;
  RealtimeThreadHints hints_;
  AudioWorkgroup workgroup_;
  /** Bumped whenever the workgroup or the hints change; each worker settles
   *  until it has caught up. */
  uint64_t generation_ = 0;
  std::vector<WorkerReport> reports_;
  std::string message_;
  /** The audio thread's mailbox. See kOfferedNone. */
  std::atomic<uintptr_t> offered_{0};
  std::atomic<bool> offerRefused_{false};
};

} // namespace sonore
