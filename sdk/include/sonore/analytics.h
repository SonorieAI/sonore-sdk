// SPDX-License-Identifier: Apache-2.0
//
// Analytics: events a plugin decides to record, and the destinations it
// writes itself to send them somewhere.
//
// ── NOTHING IS SENT BY DEFAULT ──────────────────────────────────────────────
//
// Read this first, because it is the whole design. Including this header
// sends nothing, logs nothing and opens no connection. Analytics starts with
// no destinations at all, so every logEvent() is dropped on the floor until a
// plugin adds one -- and the SDK ships no destination that talks to a network.
// ThreadedAnalyticsDestination batches and retries, but its send is a pure
// virtual the plugin author writes: what leaves the user's machine, where it
// goes, and over what, is code in the plugin, where a reviewer can read it.
//
// That is deliberate rather than an omission. A generated plugin that phoned
// home on its own would be a liability for whoever ships it -- a privacy
// policy it does not know it needs, a firewall prompt in the middle of a
// session, a support thread about why a reverb talks to a server. So the
// shape here is the reference's (a singleton, an event with parameters, a
// destination interface, a threaded batching base, a button tracker), and the
// policy is the opposite of "on unless you turn it off": off unless you write
// the part that turns it on. setSuspended(true) is there for the user's own
// opt-out switch on top of that.
//
// ── Threads ─────────────────────────────────────────────────────────────────
//
// logEvent() may be called from any thread EXCEPT the audio thread: it takes
// a lock and copies strings. A plugin that wants to count something that
// happens in process() counts it in an atomic and logs the count from the
// editor's timer.
//
// Destinations are called under Analytics' lock, on the logging thread, so a
// destination's logEvent() must be quick and must not call back into
// Analytics. ThreadedAnalyticsDestination's is a push onto a queue; the slow
// part -- the send -- happens on its own thread.
//
// ── Unloading ───────────────────────────────────────────────────────────────
//
// Stop a ThreadedAnalyticsDestination (stopAnalyticsThread(), or remove it
// from Analytics) before the plugin's module unloads. Left to the singleton's
// static destructor it would be joined while the OS holds the loader lock,
// and on Windows a thread cannot finish exiting while that lock is held: the
// host hangs on FreeLibrary. That is not a bug this header can fix from the
// inside, so it is written here where the person who can avoid it will see it.
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "json.h"
#include "path_io.h"

namespace sonore {

/** Named values attached to an event or to the user. Sorted by key, so two
 *  events with the same parameters serialise identically. */
using AnalyticsParameters = std::map<std::string, std::string>;

/** One recorded event, as a destination receives it. */
struct AnalyticsEvent {
  /** What happened, as the plugin names it: "preset_saved". */
  std::string name;
  /** The plugin's own classification -- the reference's eventType. Zero unless
   *  the plugin uses it; nothing here interprets it. */
  int eventType = 0;
  /** Seconds since the Unix epoch, wall clock, when logEvent() was called. A
   *  wall clock rather than a monotonic one because events outlive the
   *  process: they are saved to disk and sent in a later session, and a
   *  monotonic reading means nothing after a reboot. */
  double timestamp = 0.0;
  /** The values logged with this event. */
  AnalyticsParameters parameters;
  /** Copied from Analytics at the moment of logging, so an event keeps the
   *  user it was logged for even if the id changes before it is sent. */
  std::string userId;
  /** Analytics' user properties at the moment of logging, copied likewise. */
  AnalyticsParameters userProperties;

  /** Every field equal, the timestamp exactly. */
  bool operator==(const AnalyticsEvent& o) const {
    return name == o.name && eventType == o.eventType && timestamp == o.timestamp &&
           parameters == o.parameters && userId == o.userId && userProperties == o.userProperties;
  }
  /** Not operator==. */
  bool operator!=(const AnalyticsEvent& o) const { return !(*this == o); }
};

/** Where events go. Implement logEvent(); see the threading note above. */
class AnalyticsDestination {
public:
  /** Virtual: Analytics owns destinations and deletes them through this. */
  virtual ~AnalyticsDestination() = default;
  /** One event, on the thread that logged it and under Analytics' lock:
   *  be quick, and do not call back into Analytics. `event` is valid only
   *  during the call -- copy what is kept. */
  virtual void logEvent(const AnalyticsEvent& event) = 0;
};

/**
 * The event log a plugin writes to, and the destinations it fans out to.
 *
 * instance() is the shared one the reference's singleton corresponds to.
 * The constructor is public as well, because a test -- or a plugin that keeps
 * two independent streams -- wants one that nothing else can write into.
 */
class Analytics {
public:
  /** An instance of its own, with no destinations and not suspended. */
  Analytics() = default;
  /** Destroys every destination (see the header on unloading). */
  ~Analytics() { removeAllDestinations(); }
  ///@{
  /** Not copyable: it owns its destinations. */
  Analytics(const Analytics&) = delete;
  Analytics& operator=(const Analytics&) = delete;
  ///@}

  /** The process-wide (more exactly: module-wide) instance. */
  static Analytics& instance() {
    static Analytics shared;
    return shared;
  }

  /** Takes ownership. Returns the destination, for a caller that wants to keep
   *  talking to it (start its thread, read its counters). Null is ignored. */
  AnalyticsDestination* addDestination(std::unique_ptr<AnalyticsDestination> destination) {
    if (!destination) return nullptr;
    AnalyticsDestination* raw = destination.get();
    std::lock_guard<std::mutex> lock(mutex_);
    destinations_.push_back(std::move(destination));
    return raw;
  }

  /** Destroys the destination. False if it was not one of ours. */
  bool removeDestination(AnalyticsDestination* destination) {
    std::unique_ptr<AnalyticsDestination> doomed;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      for (size_t i = 0; i < destinations_.size(); ++i)
        if (destinations_[i].get() == destination) {
          doomed = std::move(destinations_[i]);
          destinations_.erase(destinations_.begin() + (std::ptrdiff_t) i);
          break;
        }
    }
    // Destroyed OUTSIDE the lock: a threaded destination joins its thread
    // here, and that thread may be about to ask whether it is still wanted.
    return doomed != nullptr;
  }

  /** Destroys every destination, outside the lock, as removeDestination()
   *  does. Not from inside a destination's logEvent(). */
  void removeAllDestinations() {
    std::vector<std::unique_ptr<AnalyticsDestination>> doomed;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      doomed.swap(destinations_);
    }
  }

  /** How many destinations there are. Any thread but the audio one. */
  size_t numDestinations() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return destinations_.size();
  }

  /** The user id copied into every event logged from now on; events
   *  already logged keep theirs. */
  void setUserId(std::string id) {
    std::lock_guard<std::mutex> lock(mutex_);
    userId_ = std::move(id);
  }
  /** A copy of the current user id; "" until set. */
  std::string userId() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return userId_;
  }

  /** Replaces the user properties copied into every event from now on. */
  void setUserProperties(AnalyticsParameters properties) {
    std::lock_guard<std::mutex> lock(mutex_);
    userProperties_ = std::move(properties);
  }
  /** A copy of the current user properties. */
  AnalyticsParameters userProperties() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return userProperties_;
  }

  /** While suspended, logEvent() drops everything -- the switch a plugin
   *  wires to its own "share usage data" preference. Events already queued in
   *  a destination are not recalled; a plugin honouring an opt-out that
   *  strictly also removes its destinations. */
  void setSuspended(bool shouldBeSuspended) {
    std::lock_guard<std::mutex> lock(mutex_);
    suspended_ = shouldBeSuspended;
  }
  /** True while setSuspended(true) is in force; false at construction. */
  bool isSuspended() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return suspended_;
  }

  /** Where timestamps come from: seconds since the Unix epoch. Replaced by a
   *  test that wants to know what the timestamp will be. Null restores the
   *  system clock. */
  void setClock(std::function<double()> secondsSinceEpoch) {
    std::lock_guard<std::mutex> lock(mutex_);
    clock_ = std::move(secondsSinceEpoch);
  }

  /** Record an event. Dropped when suspended or when there is nowhere for it
   *  to go -- which, until a destination is added, is always. */
  void logEvent(const std::string& name, const AnalyticsParameters& parameters = {},
                int eventType = 0) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (suspended_ || destinations_.empty()) return;
    AnalyticsEvent event;
    event.name = name;
    event.eventType = eventType;
    event.timestamp = clock_ ? clock_() : systemSeconds();
    event.parameters = parameters;
    event.userId = userId_;
    event.userProperties = userProperties_;
    for (auto& d : destinations_) d->logEvent(event);
  }

  /** The system clock: seconds since the Unix epoch, with a fraction. What
   *  timestamps come from unless setClock() replaced it. */
  static double systemSeconds() {
    return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch())
        .count();
  }

private:
  mutable std::mutex mutex_;
  std::vector<std::unique_ptr<AnalyticsDestination>> destinations_;
  std::string userId_;
  AnalyticsParameters userProperties_;
  std::function<double()> clock_;
  bool suspended_ = false;
};

/**
 * A destination that queues events and hands them over in batches, from a
 * thread of its own, to a send the plugin writes.
 *
 * What it does so the send does not have to:
 *
 *  - BATCHES. Events collect for batchPeriodMs and go out up to maxBatchSize
 *    at a time: one request a second rather than one per click.
 *  - RETRIES. A send that returns false (or throws) puts its batch back at
 *    the front of the queue, in order, and the next attempt waits twice as
 *    long as the last, up to maxRetryDelayMs. The first success resets the
 *    wait. A machine that is offline costs one failed attempt per five
 *    minutes, not one per second.
 *  - RATE LIMITS. A bucket of maxEventsPerMinute tokens, refilled at that rate:
 *    a plugin that logs from a loop by mistake is cut off at a minute's worth
 *    rather than filling the queue, the disk and somebody's server. Dropped
 *    events are counted, not hidden.
 *  - BOUNDS THE QUEUE. Past maxQueuedEvents the OLDEST are dropped, because
 *    the newest describe what the user is doing now.
 *  - PERSISTS. stopAnalyticsThread() writes whatever is unsent to
 *    unsentEventsFile, and startAnalyticsThread() reads it back and deletes
 *    it, so a session that ends offline is sent by the next one. Override
 *    saveUnloggedEvents/restoreUnloggedEvents to keep them elsewhere.
 *
 * Time comes from a clock the caller can replace (setClock), and one pass of
 * the thread's loop is a public call (processPendingEvents), so every one of
 * the behaviours above can be tested without sleeping.
 *
 * LIFETIME, the one rule a subclass must follow: call stopAnalyticsThread()
 * in the SUBCLASS's destructor. The thread calls logBatchedEvents(), which the
 * subclass implements; by the time this base destructor runs the subclass is
 * gone, and a thread still sending would call into a destroyed object. The
 * base destructor stops the thread too, as a last resort, but it cannot make
 * that call safe.
 */
class ThreadedAnalyticsDestination : public AnalyticsDestination {
public:
  /** How the queue batches, retries, limits and persists; fixed at
   *  construction. Out-of-range values are clamped rather than refused: a
   *  period, batch size or queue bound below 1 becomes 1, a retry ceiling
   *  below the period becomes the period, and a negative or non-finite rate
   *  turns the limiter off. settings() returns the values in force. */
  struct Settings {
    /** How long events collect before a send, and the first retry delay. */
    int batchPeriodMs = 1000;
    /** The most events one send is handed. */
    int maxBatchSize = 50;
    /** The ceiling the doubling retry delay stops at. */
    int maxRetryDelayMs = 5 * 60 * 1000;
    /** The most events held; past it the oldest are dropped. */
    size_t maxQueuedEvents = 2000;
    /** Zero turns the limiter off. */
    double maxEventsPerMinute = 600.0;
    /** Where unsent events wait between sessions. Empty: they are dropped at
     *  shutdown, and nothing is written to disk at all. */
    std::string unsentEventsFile;
  };

  /** Counted since construction, for a plugin's diagnostics and for tests. */
  struct Counters {
    uint64_t delivered = 0;          ///< events in batches a send accepted
    uint64_t failedAttempts = 0;     ///< sends that returned false or threw
    uint64_t droppedQueueFull = 0;   ///< oldest events dropped past maxQueuedEvents
    uint64_t droppedRateLimited = 0; ///< events refused by the rate limit
    uint64_t restored = 0;           ///< events read back from the unsent-events file
    uint64_t saved = 0;              ///< events written to the unsent-events file
  };

  // Two constructors rather than `Settings settings = Settings()`: a default
  // argument that builds a nested struct with member initializers is
  // rejected by clang and gcc ("default member initializer needed within
  // definition of enclosing class") -- MSVC alone accepts it.
  /** With the default Settings. The thread does not start until
   *  startAnalyticsThread(). */
  ThreadedAnalyticsDestination() : ThreadedAnalyticsDestination(Settings()) {}

  /** With `settings`, clamped as Settings describes. The thread does not
   *  start until startAnalyticsThread(). */
  explicit ThreadedAnalyticsDestination(Settings settings) : settings_(sanitised(settings)) {
    clock_ = []() { return steadySeconds(); };
    tokens_ = settings_.maxEventsPerMinute;
    lastRefill_ = clock_();
    nextAttempt_ = lastRefill_ + periodSeconds();
    retryDelaySeconds_ = periodSeconds();
  }

  /** Stops the thread if the subclass did not -- the last resort the class
   *  comment warns about. */
  ~ThreadedAnalyticsDestination() override { stopThreadOnly(); }

  ///@{
  /** Not copyable: the thread holds a pointer to this object. */
  ThreadedAnalyticsDestination(const ThreadedAnalyticsDestination&) = delete;
  ThreadedAnalyticsDestination& operator=(const ThreadedAnalyticsDestination&) = delete;
  ///@}

  /** The settings in force, after clamping. */
  const Settings& settings() const { return settings_; }

  /** Queue an event. Cheap: a rate check and a push. */
  void logEvent(const AnalyticsEvent& event) final {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!takeTokenLocked()) {
      ++counters_.droppedRateLimited;
      return;
    }
    queue_.push_back(event);
    trimQueueLocked();
  }

  /**
   * Restore what an earlier session left unsent, then start the thread. The
   * first batch goes out one batch period from now. False if it was already
   * running.
   */
  bool startAnalyticsThread() {
    std::lock_guard<std::mutex> control(controlMutex_);
    if (thread_.joinable()) return false;
    std::deque<AnalyticsEvent> restored;
    if (!restoredFromDisk_) {
      restoredFromDisk_ = true;
      restoreUnloggedEvents(restored);
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      counters_.restored += restored.size();
      // Older than anything logged since, so they go in FRONT.
      queue_.insert(queue_.begin(), std::make_move_iterator(restored.begin()),
                    std::make_move_iterator(restored.end()));
      trimQueueLocked();
      stopRequested_ = false;
      nextAttempt_ = clock_() + periodSeconds();
    }
    thread_ = std::thread([this]() { run(); });
    running_.store(true);
    return true;
  }

  /**
   * Stop the thread -- after any send in progress returns -- and save what
   * is still queued. Safe to call twice, and when the thread never started
   * (the queue is saved either way, so events logged with no thread are not
   * lost).
   */
  void stopAnalyticsThread() {
    std::lock_guard<std::mutex> control(controlMutex_);
    stopThreadOnly();
    std::deque<AnalyticsEvent> unsent;
    // A destination that never started never read what an EARLIER session
    // left on disk. Saving over it would lose those events, so they are read
    // first and kept, ahead of this session's.
    if (!restoredFromDisk_) {
      restoredFromDisk_ = true;
      restoreUnloggedEvents(unsent);
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      unsent.insert(unsent.end(), std::make_move_iterator(queue_.begin()), std::make_move_iterator(queue_.end()));
      queue_.clear();
      while (unsent.size() > settings_.maxQueuedEvents) {
        unsent.pop_front();
        ++counters_.droppedQueueFull;
      }
    }
    if (unsent.empty()) return; // nothing to keep, and nothing on disk this has not already read
    if (saveUnloggedEvents(unsent)) {
      std::lock_guard<std::mutex> lock(mutex_);
      counters_.saved += unsent.size();
    } else {
      // Not saved: they stay queued, so a later start or send still has them.
      std::lock_guard<std::mutex> lock(mutex_);
      queue_.insert(queue_.begin(), std::make_move_iterator(unsent.begin()),
                    std::make_move_iterator(unsent.end()));
      trimQueueLocked();
    }
  }

  /** Lock-free, so a send that asks cannot deadlock against a stop that is
   *  waiting for that very send to return. */
  bool isThreadRunning() const { return running_.load(); }

  /**
   * One pass of the thread's loop, at the clock's current time: if a send is
   * due and events are waiting, send up to maxBatchSize of them.
   *
   * Returns how many were delivered, 0 when nothing was due or waiting, and
   * -1 when a send was attempted and failed. Callable with or without the
   * thread running; two passes never send at once.
   */
  int processPendingEvents() {
    std::vector<AnalyticsEvent> batch;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (sending_) return 0;
      const double now = clock_();
      if (now < nextAttempt_) return 0;
      if (queue_.empty()) {
        nextAttempt_ = now + periodSeconds();
        return 0;
      }
      const size_t n = (std::min)(queue_.size(), (size_t) settings_.maxBatchSize);
      batch.reserve(n);
      std::move(queue_.begin(), queue_.begin() + (std::ptrdiff_t) n, std::back_inserter(batch));
      queue_.erase(queue_.begin(), queue_.begin() + (std::ptrdiff_t) n);
      sending_ = true;
      inFlight_ = n;
    }

    bool delivered = false;
    // A send that throws is a send that failed. Letting it out would end the
    // thread, and every event after it would wait for a restart that only a
    // new session brings.
    try {
      delivered = logBatchedEvents(batch);
    } catch (...) {
      delivered = false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    sending_ = false;
    inFlight_ = 0;
    const double now = clock_();
    if (delivered) {
      counters_.delivered += batch.size();
      retryDelaySeconds_ = periodSeconds();
      nextAttempt_ = now + periodSeconds();
      return (int) batch.size();
    }
    ++counters_.failedAttempts;
    queue_.insert(queue_.begin(), std::make_move_iterator(batch.begin()),
                  std::make_move_iterator(batch.end()));
    trimQueueLocked();
    retryDelaySeconds_ = (std::min)(retryDelaySeconds_ * 2.0, (double) settings_.maxRetryDelayMs / 1000.0);
    nextAttempt_ = now + retryDelaySeconds_;
    return -1;
  }

  /** Monotonic seconds. Null restores the steady clock. Set it before the
   *  thread starts: the rate limiter and the schedule are both measured
   *  against it, and the first send is due one batch period after this. */
  void setClock(std::function<double()> monotonicSeconds) {
    std::lock_guard<std::mutex> lock(mutex_);
    clock_ = monotonicSeconds ? std::move(monotonicSeconds) : std::function<double()>([]() {
      return steadySeconds();
    });
    lastRefill_ = clock_();
    nextAttempt_ = lastRefill_ + periodSeconds();
  }

  /** Queued, including a batch currently being sent. */
  size_t numPendingEvents() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size() + inFlight_;
  }

  /** A snapshot of the counters. Any thread. */
  Counters counters() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return counters_;
  }

  /** The wait after the next failure would be twice this. */
  double currentRetryDelaySeconds() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return retryDelaySeconds_;
  }

  /** When the next send may happen, on the destination's clock. */
  double nextAttemptTime() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return nextAttempt_;
  }

  // ── The unsent-events file ──────────────────────────────────────────────
  //
  // JSON, through json.h, because the file is read back by a later version
  // of the plugin and a self-describing format is the one that version can
  // still read. It is UNTRUSTED input like any file on a user's disk -- it can
  // be truncated by a crash, edited, or replaced -- so the parser below takes
  // what it can validate and skips the rest, and fuzz_modules.cpp feeds it
  // mutations of real files.

  /** The "format" field of the unsent-events file. */
  static constexpr const char* kFileFormat = "sonore-analytics-events";
  /** The "version" field of the unsent-events file. */
  static constexpr int kFileVersion = 1;
  /** Larger than any honest file: 2000 events of a few hundred bytes. */
  static constexpr size_t kMaxFileBytes = (size_t) 16 * 1024 * 1024;
  /** The longest name, id, key or value the file reader accepts, in bytes. */
  static constexpr size_t kMaxStringBytes = 64 * 1024;
  /** The most parameters (or user properties) the file reader accepts in
   *  one event. */
  static constexpr size_t kMaxParametersPerEvent = 256;

  /** `events` as the unsent-events file's JSON text: compact, every field of
   *  each event. */
  static std::string serialiseEvents(const std::deque<AnalyticsEvent>& events) {
    JsonValue root = JsonValue::object();
    root.set("format", kFileFormat);
    root.set("version", kFileVersion);
    JsonValue list = JsonValue::array();
    for (const AnalyticsEvent& e : events) {
      JsonValue item = JsonValue::object();
      item.set("name", e.name);
      item.set("type", e.eventType);
      item.set("time", e.timestamp);
      item.set("user", e.userId);
      item.set("parameters", toJson(e.parameters));
      item.set("userProperties", toJson(e.userProperties));
      list.append(std::move(item));
    }
    root.set("events", std::move(list));
    return root.toString();
  }

  /**
   * Read a file's text back into events, appending at most `maxEvents`.
   *
   * False when the text is not this format at all -- not JSON, the wrong
   * format tag, a version from the future. Within a recognised file, an event
   * that fails validation is SKIPPED and the rest are kept: one corrupted
   * record should not cost the session's other ninety-nine. `skipped`, when
   * given, says how many.
   */
  static bool parseEvents(const std::string& text, std::deque<AnalyticsEvent>& out,
                          size_t maxEvents, size_t* skipped = nullptr) {
    if (skipped) *skipped = 0;
    if (text.empty() || text.size() > kMaxFileBytes) return false;
    std::string error;
    const JsonValue root = JsonValue::parse(text, &error);
    if (!error.empty() || !root.isObject()) return false;
    if (root["format"].asString() != kFileFormat) return false;
    const JsonValue& version = root["version"];
    if (!version.isNumber() || version.asNumber() < 1.0 || version.asNumber() > (double) kFileVersion)
      return false;
    const JsonValue& list = root["events"];
    if (!list.isArray()) return false;
    size_t rejected = 0;
    for (size_t i = 0; i < list.size() && out.size() < maxEvents; ++i) {
      AnalyticsEvent e;
      if (eventFromJson(list.at(i), e)) out.push_back(std::move(e));
      else ++rejected;
    }
    if (skipped) *skipped = rejected;
    return true;
  }

protected:
  /**
   * Deliver a batch. Runs on the analytics thread (or in processPendingEvents'
   * caller). Return true once the batch is accepted -- it will not be offered
   * again -- or false to have it retried later, in order, with backoff.
   *
   * May block for as long as a request takes; stopLoggingEvents() is the
   * moment to abandon one.
   */
  virtual bool logBatchedEvents(const std::vector<AnalyticsEvent>& batch) = 0;

  /** Called when stopping, from the stopping thread, BEFORE it waits for the
   *  analytics thread. A send blocked on a slow network should give up when
   *  this is called; the batch it was sending is saved with the rest. */
  virtual void stopLoggingEvents() {}

  /** Keep unsent events until the next session. The default writes
   *  Settings::unsentEventsFile (and removes it when there is nothing to
   *  keep). Return false if they could not be kept. */
  virtual bool saveUnloggedEvents(const std::deque<AnalyticsEvent>& events) {
    const std::string& path = settings_.unsentEventsFile;
    if (path.empty()) return events.empty();
    if (events.empty()) {
      pathio::remove(path.c_str());
      return true;
    }
    const std::string text = serialiseEvents(events);
    const std::string temp = path + ".tmp";
    std::FILE* f = pathio::open(temp.c_str(), "wb");
    if (!f) return false;
    bool wrote = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    wrote = std::fflush(f) == 0 && wrote;
    wrote = std::fclose(f) == 0 && wrote;
    if (!wrote) {
      pathio::remove(temp.c_str());
      return false;
    }
    // Remove, then rename: rename does not replace on Windows. A crash in
    // between leaves only the .tmp, which restore also looks for -- these
    // are usage events, and the cost of the window is at worst a session's
    // worth sent twice or not at all, not a corrupted file.
    pathio::remove(path.c_str());
    if (pathio::rename(temp.c_str(), path.c_str()) != 0) {
      pathio::remove(temp.c_str());
      return false;
    }
    return true;
  }

  /** The default reads Settings::unsentEventsFile (or the .tmp a crash left)
   *  and deletes it: events are restored once, then they are the queue's. */
  virtual void restoreUnloggedEvents(std::deque<AnalyticsEvent>& into) {
    const std::string& path = settings_.unsentEventsFile;
    if (path.empty()) return;
    const std::string candidates[2] = {path, path + ".tmp"};
    for (const std::string& file : candidates) {
      std::string text;
      if (!readFile(file, text)) continue;
      parseEvents(text, into, settings_.maxQueuedEvents);
      pathio::remove(file.c_str());
    }
  }

private:
  static Settings sanitised(Settings s) {
    s.batchPeriodMs = (std::max)(1, s.batchPeriodMs);
    s.maxBatchSize = (std::max)(1, s.maxBatchSize);
    s.maxRetryDelayMs = (std::max)(s.batchPeriodMs, s.maxRetryDelayMs);
    s.maxQueuedEvents = (std::max)((size_t) 1, s.maxQueuedEvents);
    if (!(s.maxEventsPerMinute > 0.0) || !std::isfinite(s.maxEventsPerMinute)) s.maxEventsPerMinute = 0.0;
    return s;
  }

  static double steadySeconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
  }

  double periodSeconds() const { return (double) settings_.batchPeriodMs / 1000.0; }

  bool takeTokenLocked() {
    const double rate = settings_.maxEventsPerMinute;
    if (rate <= 0.0) return true;
    const double now = clock_();
    // A clock that went BACKWARDS refills nothing rather than a negative
    // amount, which would empty the bucket for a mistake that was not ours.
    if (now > lastRefill_) tokens_ = (std::min)(rate, tokens_ + (now - lastRefill_) * rate / 60.0);
    lastRefill_ = now;
    if (tokens_ < 1.0) return false;
    tokens_ -= 1.0;
    return true;
  }

  void trimQueueLocked() {
    while (queue_.size() > settings_.maxQueuedEvents) {
      queue_.pop_front();
      ++counters_.droppedQueueFull;
    }
  }

  void run() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stopRequested_) {
      const double wait = nextAttempt_ - clock_();
      if (wait > 0.0) {
        // Re-read the clock at least four times a second rather than trusting
        // one long wait: a replaced clock need not advance in real time, and
        // a suspended laptop wakes to a steady clock far ahead of the wait.
        wakeup_.wait_for(lock, std::chrono::duration<double>((std::min)(wait, 0.25)));
        continue;
      }
      lock.unlock();
      processPendingEvents();
      lock.lock();
    }
  }

  void stopThreadOnly() {
    if (!thread_.joinable()) return;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopRequested_ = true;
    }
    wakeup_.notify_all();
    stopLoggingEvents();
    thread_.join();
    running_.store(false);
  }

  static JsonValue toJson(const AnalyticsParameters& p) {
    JsonValue o = JsonValue::object();
    for (const auto& kv : p) o.set(kv.first, kv.second);
    return o;
  }

  static bool parametersFromJson(const JsonValue& v, AnalyticsParameters& out) {
    if (v.isNull()) return true; // absent: no parameters
    if (!v.isObject() || v.size() > kMaxParametersPerEvent) return false;
    for (const auto& kv : v.members()) {
      if (!kv.second.isString()) return false;
      const std::string value = kv.second.asString();
      if (kv.first.size() > kMaxStringBytes || value.size() > kMaxStringBytes) return false;
      out[kv.first] = value;
    }
    return true;
  }

  static bool eventFromJson(const JsonValue& v, AnalyticsEvent& e) {
    if (!v.isObject()) return false;
    const JsonValue& name = v["name"];
    if (!name.isString()) return false;
    e.name = name.asString();
    if (e.name.empty() || e.name.size() > kMaxStringBytes) return false;
    const JsonValue& type = v["type"];
    if (!type.isNull()) {
      // In int range and whole, checked BEFORE the conversion: a double past
      // INT_MAX converted to int is undefined, not a large number.
      const double t = type.asNumber(0.0);
      if (!type.isNumber() || !(t >= -2147483648.0 && t <= 2147483647.0) || t != std::floor(t))
        return false;
      e.eventType = (int) t;
    }
    const JsonValue& time = v["time"];
    if (!time.isNumber() || !std::isfinite(time.asNumber())) return false;
    e.timestamp = time.asNumber();
    const JsonValue& user = v["user"];
    if (!user.isNull()) {
      if (!user.isString()) return false;
      e.userId = user.asString();
      if (e.userId.size() > kMaxStringBytes) return false;
    }
    return parametersFromJson(v["parameters"], e.parameters) &&
           parametersFromJson(v["userProperties"], e.userProperties);
  }

  static bool readFile(const std::string& path, std::string& out) {
    std::FILE* f = pathio::open(path.c_str(), "rb");
    if (!f) return false;
    char chunk[16384];
    size_t got = 0;
    while ((got = std::fread(chunk, 1, sizeof(chunk), f)) > 0) {
      out.append(chunk, got);
      // Past the cap it is not a file this wrote; stop reading it into memory.
      if (out.size() > kMaxFileBytes) break;
    }
    std::fclose(f);
    return !out.empty();
  }

  const Settings settings_;
  mutable std::mutex mutex_;
  /** Serialises start and stop against each other; never held by the thread. */
  mutable std::mutex controlMutex_;
  std::condition_variable wakeup_;
  std::thread thread_;
  std::atomic<bool> running_{false};
  std::function<double()> clock_;
  std::deque<AnalyticsEvent> queue_;
  Counters counters_;
  double tokens_ = 0.0;
  double lastRefill_ = 0.0;
  double nextAttempt_ = 0.0;
  double retryDelaySeconds_ = 1.0;
  size_t inFlight_ = 0;
  /** The file is read ONCE per destination, by whichever of start and stop
   *  comes first; after that its events are in the queue. */
  bool restoredFromDisk_ = false;
  bool sending_ = false;
  bool stopRequested_ = false;
};

} // namespace sonore
