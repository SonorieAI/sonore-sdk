// SPDX-License-Identifier: Apache-2.0
//
// The message thread: running code on it, and holding it still.
//
// ── What it is for ──────────────────────────────────────────────────────────
//
// Components may only be touched from one thread. A sample finishes loading on
// a worker, a device list changes on a driver's thread, a licence check comes
// back from a pool -- and every one of them has something to tell the
// interface. MessageManager is the one door: callAsync() queues a function for
// the message thread, callFunctionOnMessageThread() queues it and waits,
// CallbackMessage and MessageListener are the same thing as objects, and
// MessageManagerLock lets a background thread hold the message thread still
// while it does the touching itself.
//
// The editor's 33 ms tick already carries the PARAMETER traffic and stays the
// right road for it (UiEventQueue is lock-free and the audio thread may push
// into it). This is for everything else, and none of it is for the AUDIO
// thread: posting allocates and takes a mutex, and a lock on the message
// thread from an audio callback is how a plugin deadlocks a DAW.
//
// ── One queue of our own, and the OS only as an alarm clock ────────────────
//
// Every call lands in a deque owned by this object, and the OS is only asked
// to wake the message thread up -- once, however many calls are waiting.
// Posting one OS message per call is the obvious design and it fails three
// ways: Windows drops posted messages past 10,000 per thread, so a worker in
// a tight loop loses callbacks silently; a nested modal loop (a native file
// dialog) can reorder them; and a lock request needs to know whether it was
// ever delivered, which the OS will not say. The wake-up per platform:
//
//   Windows  a message-only window (HWND_MESSAGE) created on the message
//            thread; PostMessage wakes it. The host pumps it like any other.
//   macOS    a CFRunLoopSource on the message thread's run loop, in the
//            common modes so it still fires under a modal panel or a drag.
//   Linux    an eventfd (a pipe where there is none). runDispatchLoop polls
//            it; inside a host, wakeUpFileDescriptor() is what to hand the
//            host's fd support (CLAP posix-fd, VST3 IRunLoop).
//
// The message thread is whichever thread first asks for the instance, which
// in a plugin is the host's UI thread and in an application its main thread.
//
// ── Draining in order, a bounded amount at a time ──────────────────────────
//
// Messages are taken ONE at a time from the front, so a callback that runs a
// nested loop (a modal dialog pumping messages) carries on in the right order
// rather than overtaking the rest of a batch it had already taken. And a
// wake-up drains at most a few hundred, or 20 ms, before it re-arms itself and
// gives the OS loop a turn: a worker posting faster than the message thread
// can run would otherwise freeze input and painting for as long as it posted.
// Windows hands out posted messages before input, paint and timers, so there
// a queue that has not run dry for 50 ms asks for its next batch by a timer
// whenever one of those is waiting.
//
// ── The lock ────────────────────────────────────────────────────────────────
//
// MessageManagerLock posts a message that, when the message thread reaches it,
// parks the message thread inside that callback until the lock is released.
// That is the "safe point": between two messages, never in the middle of one,
// so whatever the message thread was doing is finished and nothing it owns is
// half-updated. While parked it runs nothing else, so two lock holders are
// mutually exclusive with each other and with every other callback.
//
// The danger is the obvious one. A worker waiting for the lock while the
// message thread waits for the worker -- a destructor joining it -- never
// ends. So a lock can be given a ThreadExitSignal (woken the instant the
// signal is raised) or a ThreadPool job's stop flag (looked at every 2 ms),
// and gives up when either is set. Every worker that may be stopped from the
// message thread should pass one.
//
// What is RUN on this machine: all of it on Windows. macOS and Linux are
// written against their documented APIs and only compiled here.
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cwchar>
#if defined(_MSC_VER)
#pragma comment(lib, "user32.lib")
#endif
#elif defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <CoreFoundation/CFNotificationCenter.h>
#include <CoreFoundation/CFRunLoop.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <pthread.h>
#include <unistd.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/eventfd.h>
#endif
#endif

namespace sonore {

class MessageManager;
class MessageManagerLock;
class MessageListener;

namespace detail {
namespace msgloop {

/** How many messages one wake-up runs before it re-arms and lets the OS loop
 *  have a turn, and for how long. See the header. */
constexpr int kDispatchBudget = 256;
constexpr int kDispatchBudgetMs = 20;
/** Windows: how long the queue may be worked on without running dry before
 *  input, paint and timers that are waiting go first. See platformYield. */
constexpr int kYieldAfterBusyMs = 50;

/**
 * One request for the message thread's exclusive attention.
 *
 * Shared by three parties -- the waiting thread, the message parked on the
 * message thread, and an exit signal that may wake the waiter -- so it is
 * reference-counted: whichever of them finishes last frees it, and none of
 * them has to know which.
 */
struct LockRequest {
  enum class State {
    Waiting,   // posted, not yet reached by the message thread
    Acquired,  // the message thread is parked; the requester holds it
    Released,  // the requester let go; the message thread may continue
    Abandoned, // the requester gave up before the message was reached
    Dropped,   // the message was destroyed undelivered (manager deleted)
  };
  std::mutex mutex;
  std::condition_variable changed;
  State state = State::Waiting;

  /** Taken under the mutex on purpose: a waiter checks its abort condition
   *  holding it, so a signal raised in between cannot be missed. */
  void wake() {
    std::lock_guard<std::mutex> lock(mutex);
    changed.notify_all();
  }
};

/** What a thread blocked in callFunctionOnMessageThread waits on. */
struct SyncCallState {
  std::mutex mutex;
  std::condition_variable done;
  bool finished = false;
  bool ran = false;
  std::exception_ptr error;

  void finish(bool didRun) {
    std::lock_guard<std::mutex> lock(mutex);
    if (finished) return;
    finished = true;
    ran = didRun;
    done.notify_all();
  }
};

/**
 * The link between a MessageListener and the messages in flight to it.
 *
 * A message holds the anchor, not the listener, so a listener deleted while
 * messages are queued leaves them pointing at nothing rather than at freed
 * memory. Delivery happens under the anchor's mutex, so a listener destroyed
 * on another thread waits for a delivery already under way; recursive,
 * because a listener deleting itself from its own handleMessage is ordinary.
 */
struct ListenerAnchor {
  std::recursive_mutex mutex;
  MessageListener* target = nullptr;
};

/**
 * State that outlives any one MessageManager: the instance pointer, the
 * mutex that guards it and the queue, and which thread holds the lock.
 *
 * Function-local, so it exists before the first instance and is per binary
 * -- two plugins built with this SDK in one host each get their own, which
 * is right, because each has its own copy of every callback in the queue.
 */
struct Globals {
  std::mutex mutex;
  std::atomic<MessageManager*> instance{nullptr};
  std::atomic<std::thread::id> threadWithLock{std::thread::id()};
  /** Bumped by every deleteInstance, so a dispatch running when its manager
   *  was deleted (from inside a callback) can tell -- even if the next one
   *  was allocated at the same address. */
  uint64_t generation = 0;
  /** Told about an exception a callback threw before it is dropped; null
   *  when nobody asked (see MessageManager::setUnhandledExceptionHandler). A
   *  bare pointer, so reading it in the catch allocates nothing. */
  std::atomic<void (*)(const std::exception*)> exceptionHandler{nullptr};
};

inline Globals& globals() {
  static Globals g;
  return g;
}

#if defined(_WIN32)
/** The wake-up. Any value works on a window nobody else sends to. */
constexpr UINT kWakeMessage = WM_APP + 0x51E;
/** The wake-up that waits its turn behind input, paint and timers: see
 *  MessageManager::platformYield. */
constexpr UINT_PTR kRearmTimer = 0x51E;
/** A broadcast's WM_COPYDATA tag, so a stranger's copy-data is ignored. */
constexpr ULONG_PTR kBroadcastTag = 0x534F4E42; // "SONB"
/** Every Sonore message window has this TITLE, whatever its class. The class
 *  name is per module (see messageWindowClass); a broadcast finds the others
 *  by title, across modules and processes. */
constexpr const wchar_t* kWindowTitle = L"SonoreMessageWindow";
#endif

#if defined(__APPLE__)
/** objc_msgSend through a correctly typed pointer: variadic in the header,
 *  not in the ABI, and arm64 passes the two differently. */
template <typename Ret, typename... Args>
Ret objcSend(id target, const char* selector, Args... args) {
  using Fn = Ret (*)(id, SEL, Args...);
  return reinterpret_cast<Fn>(objc_msgSend)(target, sel_registerName(selector), args...);
}

/** NSApp, or nil where AppKit is not loaded -- a command-line tool, which
 *  then runs the plain CFRunLoop instead. */
inline id applicationOrNil() {
  Class cls = objc_getClass("NSApplication");
  if (!cls) return (id) 0;
  return objcSend<id>((id) cls, "sharedApplication");
}

/** NSPoint's layout: two CGFloats, which are doubles on every 64-bit Mac. */
struct EventPoint {
  double x;
  double y;
};

constexpr const char* kBroadcastName = "SonoreBroadcastMessage";
#endif

} // namespace msgloop
} // namespace detail

/**
 * A flag a thread is asked to stop by, which can WAKE a thread that is
 * waiting for the message thread.
 *
 * The reference's Thread carries this; this SDK's threads are std::thread and
 * ThreadPool, so it is a small object of its own. A worker owns one (or is
 * handed one), checks threadShouldExit() in its loop, and passes it to every
 * MessageManagerLock it takes -- so the owner's signalThreadShouldExit(),
 * followed by a join, cannot deadlock against a lock the worker is waiting
 * for.
 */
class ThreadExitSignal {
public:
  /** Lowered: threadShouldExit() is false. */
  ThreadExitSignal() = default;
  ///@{
  /** Not copyable: a MessageManagerLock waiting on it holds a reference to
   *  this object. */
  ThreadExitSignal(const ThreadExitSignal&) = delete;
  ThreadExitSignal& operator=(const ThreadExitSignal&) = delete;
  ///@}

  /** Raise it, and wake every lock currently waiting on it. Any thread. */
  void signalThreadShouldExit() {
    flag_.store(true, std::memory_order_release);
    std::vector<std::shared_ptr<detail::msgloop::LockRequest>> live;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      for (const auto& weak : waiters_)
        if (auto request = weak.lock()) live.push_back(std::move(request));
    }
    for (const auto& request : live) request->wake();
  }

  /** True once signalThreadShouldExit() has been called, until reset().
   *  Any thread; a lock-free load. */
  bool threadShouldExit() const noexcept { return flag_.load(std::memory_order_acquire); }

  /** Lower it again, for a worker that is restarted rather than replaced. */
  void reset() noexcept { flag_.store(false, std::memory_order_release); }

  /** The bare flag, for code that takes one -- a ThreadPool job's signature. */
  const std::atomic<bool>& flag() const noexcept { return flag_; }

private:
  friend class MessageManagerLock;

  void addWaiter(const std::shared_ptr<detail::msgloop::LockRequest>& request) {
    std::lock_guard<std::mutex> lock(mutex_);
    waiters_.erase(std::remove_if(waiters_.begin(), waiters_.end(),
                                  [](const std::weak_ptr<detail::msgloop::LockRequest>& w) {
                                    return w.expired();
                                  }),
                   waiters_.end());
    waiters_.push_back(request);
  }

  void removeWaiter(const std::shared_ptr<detail::msgloop::LockRequest>& request) {
    std::lock_guard<std::mutex> lock(mutex_);
    waiters_.erase(std::remove_if(waiters_.begin(), waiters_.end(),
                                  [&](const std::weak_ptr<detail::msgloop::LockRequest>& w) {
                                    const auto held = w.lock();
                                    return !held || held == request;
                                  }),
                   waiters_.end());
  }

  std::atomic<bool> flag_{false};
  std::mutex mutex_;
  std::vector<std::weak_ptr<detail::msgloop::LockRequest>> waiters_;
};

/**
 * The message thread's queue, and the loop that drains it.
 *
 * One per binary, created by the first getInstance() on the thread that is
 * to be the message thread. Everything static here is safe from any thread;
 * the members say which thread they belong to.
 */
class MessageManager {
public:
  /**
   * Anything that travels through the queue.
   *
   * Allocated with `new` and handed over by post(): from then on the queue
   * owns it and deletes it after messageCallback() runs -- or, if it never
   * runs because the manager was deleted first, without running it. A
   * message must never be posted from the stack, and must not be touched
   * after post() returns, whatever post() returned.
   */
  class MessageBase {
  public:
    /** Virtual, so the queue can delete any message. It runs on the message
     *  thread after the callback, on the posting thread when post() is
     *  refused, and in deleteInstance() for a message that never ran. */
    virtual ~MessageBase() = default;

    /** Runs on the message thread. */
    virtual void messageCallback() = 0;

    /** Queue this message. False when there is no MessageManager (the
     *  message has then already been deleted). Any thread. */
    bool post();

  protected:
    /** For subclasses only: CallbackMessage, Message, or one of your own. */
    MessageBase() = default;

  private:
    MessageBase(const MessageBase&) = delete;
    MessageBase& operator=(const MessageBase&) = delete;
  };

  /** Told about a broadcastMessage() from another process, on the message
   *  thread. The text is whatever the other side sent: untrusted. */
  class BroadcastListener {
  public:
    /** Deregister before destroying: the manager keeps a plain pointer. */
    virtual ~BroadcastListener() = default;
    /** One broadcast, on the message thread. `message` is the text as the
     *  sender wrote it, any bytes at all: validate before acting on it. */
    virtual void broadcastMessageReceived(const std::string& message) = 0;
  };

  /** The reference's signature for a blocking call: a function and a pointer. */
  using MessageCallbackFunction = void*(void* userData);

  // ── The instance ──────────────────────────────────────────────────────────

  /** The manager, created on first use. The thread that creates it becomes
   *  the message thread -- call this first from the UI/main thread. */
  static MessageManager* getInstance();

  /** The manager if one exists; never creates one. Any thread. */
  static MessageManager* getInstanceWithoutCreating() noexcept {
    return detail::msgloop::globals().instance.load(std::memory_order_acquire);
  }

  /**
   * Delete the manager. Call it on the message thread, at shutdown.
   *
   * Queued messages are deleted without running; a thread blocked in
   * callFunctionOnMessageThread is released (with false), and one waiting for
   * a MessageManagerLock gives up. A plugin should call this as its last
   * editor closes; a static clean-up calls it at unload in case nobody did.
   */
  static void deleteInstance();

  // ── Threads ───────────────────────────────────────────────────────────────

  /** True when called on the message thread. Any thread. */
  bool isThisTheMessageThread() const noexcept {
    return messageThread_.load(std::memory_order_acquire) == std::this_thread::get_id();
  }

  /**
   * Make the calling thread the message thread from now on.
   *
   * The wake-up moves with it: on Windows a new message window is created on
   * this thread (the old one is retired), on macOS the source moves to this
   * thread's run loop. On macOS the message thread should stay the main
   * thread -- AppKit requires it -- and this is for tests and odd hosts.
   */
  void setCurrentThreadAsMessageThread();

  /** The message thread's id. Any thread. */
  std::thread::id getCurrentMessageThread() const noexcept {
    return messageThread_.load(std::memory_order_acquire);
  }

  /** True on the message thread, and on a thread holding a
   *  MessageManagerLock: the two places a component may be touched. */
  bool currentThreadHasLockedMessageManager() const noexcept {
    const std::thread::id me = std::this_thread::get_id();
    return messageThread_.load(std::memory_order_acquire) == me ||
           detail::msgloop::globals().threadWithLock.load(std::memory_order_acquire) == me;
  }

  /** True when a manager exists and this is its message thread. Never
   *  creates one, so it is safe in code that may run before or after the
   *  manager's life. Any thread. */
  static bool existsAndIsCurrentThread() noexcept {
    MessageManager* mm = getInstanceWithoutCreating();
    return mm && mm->isThisTheMessageThread();
  }

  /** True when a manager exists and this thread may touch components: it is
   *  the message thread, or holds a MessageManagerLock. Never creates one.
   *  Any thread. */
  static bool existsAndIsLockedByCurrentThread() noexcept {
    MessageManager* mm = getInstanceWithoutCreating();
    return mm && mm->currentThreadHasLockedMessageManager();
  }

  // ── Running code on the message thread ────────────────────────────────────

  /**
   * Queue a function for the message thread. Any thread except the audio
   * thread (this allocates and takes a mutex). Calls from one thread run in
   * the order they were made. False if there is no manager.
   */
  static bool callAsync(std::function<void()> function);

  /**
   * Run a function on the message thread and wait for it.
   *
   * Called ON the message thread -- or from a thread holding a
   * MessageManagerLock, which already has the message thread parked -- it
   * simply runs, here and now. Waiting for the queue in either case would
   * be waiting for ourselves.
   *
   * Returns false if it never ran (no manager, or the manager was deleted
   * while it waited). An exception thrown by the function is rethrown here.
   * It still deadlocks if the message thread is itself waiting for the
   * caller; callAsync is the answer there.
   */
  bool callFunctionOnMessageThread(const std::function<void()>& function);

  /** The reference's shape of the same call: returns what the function
   *  returned, or nullptr if it never ran. */
  void* callFunctionOnMessageThread(MessageCallbackFunction* function, void* userData);

  // ── The loop, for an application that owns one ────────────────────────────

  /**
   * Run the OS loop on the message thread until stopDispatchLoop(). A plugin
   * never calls this -- the host owns the loop -- and a call from any other
   * thread returns at once.
   */
  void runDispatchLoop();

  /** Ask runDispatchLoop to return, after everything already queued has run.
   *  Any thread. */
  void stopDispatchLoop();

  /** True from stopDispatchLoop() until the runDispatchLoop() it stopped
   *  returns. Any thread. */
  bool hasStopMessageBeenSent() const noexcept { return quitPosted_.load(std::memory_order_acquire); }

  /**
   * Pump the OS loop for up to `milliseconds`, then return. False if a stop
   * arrived meanwhile. For a modal sequence, and for tests; message thread
   * only.
   */
  bool runDispatchLoopUntil(int milliseconds);

  /**
   * Run what is queued now, on the calling (message) thread, without waiting
   * for the OS to deliver the wake-up. For a host that calls the plugin back
   * on an fd or a timer rather than through an OS message, and for tests.
   * Returns how many ran; 0 off the message thread.
   */
  int dispatchPendingMessages();

  /** How many messages are waiting. A diagnostic: it can be stale by the
   *  time it is read. */
  int numPendingMessages() const;

  // ── Broadcasts to other processes ─────────────────────────────────────────

  /**
   * Send a line of text to every OTHER Sonore message thread on the machine
   * -- other applications built with this SDK, and other plugins in other
   * hosts -- which hand it to their BroadcastListeners.
   *
   * Windows: WM_COPYDATA to every Sonore message window (found by title),
   * waiting up to two seconds per recipient. macOS: a distributed
   * notification. Linux has no equivalent the reference offers either, and
   * this returns false there. Returns whether it was sent to anyone/anything.
   * Message thread.
   */
  static bool broadcastMessage(const std::string& message);

  /** Message thread only, like the listener lists elsewhere in this SDK. */
  void registerBroadcastListener(BroadcastListener* listener) {
    if (!listener) return;
    if (std::find(broadcastListeners_.begin(), broadcastListeners_.end(), listener) !=
        broadcastListeners_.end())
      return;
    broadcastListeners_.push_back(listener);
  }

  /** Stops telling `listener`; one that is not registered is ignored.
   *  Message thread only. */
  void deregisterBroadcastListener(BroadcastListener* listener) {
    broadcastListeners_.erase(
        std::remove(broadcastListeners_.begin(), broadcastListeners_.end(), listener),
        broadcastListeners_.end());
  }

  /**
   * Be told about an exception a queued callback threw, before it is dropped
   * (the pointer is null for one that is not a std::exception). The loop
   * still never unwinds: this is a report, called on the message thread from
   * the catch. application.h points it at Application::unhandledException;
   * null removes it. Any thread.
   */
  static void setUnhandledExceptionHandler(void (*handler)(const std::exception*)) noexcept {
    detail::msgloop::globals().exceptionHandler.store(handler, std::memory_order_release);
  }

  /** Hand an exception caught somewhere else on the message thread -- a
   *  window procedure, which must not unwind into the OS either -- to the
   *  same hook. */
  static void sendUnhandledException(const std::exception* e) noexcept { reportException(e); }

#if !defined(_WIN32) && !defined(__APPLE__)
  // ── Linux: the descriptors the loop polls ─────────────────────────────────

  /** Readable when messages are waiting. Inside a host, register it with the
   *  host's fd support and call dispatchPendingMessages() when it fires. */
  int wakeUpFileDescriptor() const noexcept { return wakeReadFd_; }

  /** Have runDispatchLoop poll another descriptor -- an X11 connection -- and
   *  call back on the message thread when it is readable. Replaces an
   *  existing registration for the same fd. Message thread. */
  bool registerFdCallback(int fd, std::function<void(int fd)> callback) {
    if (fd < 0 || !callback) return false;
    for (FdCallback& existing : fdCallbacks_)
      if (existing.fd == fd) {
        existing.callback = std::move(callback);
        return true;
      }
    fdCallbacks_.push_back(FdCallback{fd, std::move(callback)});
    return true;
  }

  void unregisterFdCallback(int fd) {
    fdCallbacks_.erase(std::remove_if(fdCallbacks_.begin(), fdCallbacks_.end(),
                                      [fd](const FdCallback& c) { return c.fd == fd; }),
                       fdCallbacks_.end());
  }
#endif

  ///@{
  /** Not copyable: there is one manager, reached through getInstance(). */
  MessageManager(const MessageManager&) = delete;
  MessageManager& operator=(const MessageManager&) = delete;
  ///@}

private:
  friend class MessageManagerLock;

  MessageManager() {
    messageThread_.store(std::this_thread::get_id(), std::memory_order_release);
    platformInitialise();
  }

  ~MessageManager() = default;

  /** Takes ownership of `message` whatever happens. */
  static bool postToQueue(MessageBase* message) {
    // Declared BEFORE the lock, so a message that is refused is deleted after
    // the lock is released: its destructor is user code, and may post.
    std::unique_ptr<MessageBase> owned(message);
    if (!owned) return false;
    detail::msgloop::Globals& g = detail::msgloop::globals();
    std::lock_guard<std::mutex> lock(g.mutex);
    MessageManager* mm = g.instance.load(std::memory_order_acquire);
    if (!mm) return false;
    mm->queue_.push_back(std::move(owned));
    // One wake-up for any number of waiting messages. Under the lock, so the
    // handle it uses cannot be closed by a deleteInstance in between.
    if (!mm->wakePending_) {
      mm->wakePending_ = true;
      if (!mm->platformWake()) mm->wakePending_ = false; // retry on the next post
    }
    return true;
  }

  /** Is this still the live manager it was when `generation` was read? */
  bool stillAlive(uint64_t generation) const {
    detail::msgloop::Globals& g = detail::msgloop::globals();
    std::lock_guard<std::mutex> lock(g.mutex);
    return g.instance.load(std::memory_order_acquire) == this && g.generation == generation;
  }

  uint64_t currentGeneration() const {
    detail::msgloop::Globals& g = detail::msgloop::globals();
    std::lock_guard<std::mutex> lock(g.mutex);
    return g.generation;
  }

  /**
   * Run queued messages, one at a time from the front, within the budget.
   *
   * `this` may be deleted by a callback (an application shutting down from
   * inside one), so after every delivery the globals are asked whether it is
   * still the live instance before anything of ours is touched again.
   */
  int dispatchQueue() {
    if (!isThisTheMessageThread()) return 0;
    detail::msgloop::Globals& g = detail::msgloop::globals();
    uint64_t generation = 0;
    {
      std::lock_guard<std::mutex> lock(g.mutex);
      if (g.instance.load(std::memory_order_acquire) != this) return 0;
      wakePending_ = false;
      generation = g.generation;
    }
    int handled = 0;
    const auto started = std::chrono::steady_clock::now();
#if defined(_WIN32)
    if (!busy_) {
      busy_ = true;
      busySince_ = started;
    }
#endif
    for (;;) {
      std::unique_ptr<MessageBase> next;
      {
        std::lock_guard<std::mutex> lock(g.mutex);
        if (g.instance.load(std::memory_order_acquire) != this || g.generation != generation)
          return handled;
        if (queue_.empty()) {
#if defined(_WIN32)
          busy_ = false; // caught up: see platformYield
#endif
          break;
        }
        next = std::move(queue_.front());
        queue_.pop_front();
      }
      deliver(*next);
      next.reset(); // its destructor, outside the lock
      ++handled;
      const bool overBudget =
          handled >= detail::msgloop::kDispatchBudget ||
          std::chrono::steady_clock::now() - started >=
              std::chrono::milliseconds(detail::msgloop::kDispatchBudgetMs);
      if (overBudget) {
        std::lock_guard<std::mutex> lock(g.mutex);
        if (g.instance.load(std::memory_order_acquire) != this || g.generation != generation)
          return handled;
        if (!queue_.empty() && !wakePending_) {
          wakePending_ = true;
          if (!platformWake()) wakePending_ = false;
        }
        break;
      }
    }
    return handled;
  }

  static void deliver(MessageBase& message) {
    // A throwing callback must not unwind into the OS loop -- through
    // DispatchMessage or a CFRunLoop callout that is undefined behaviour, and
    // in a host it is the host that crashes. It is dropped here, like a
    // throwing ThreadPool job; callFunctionOnMessageThread catches its own
    // and rethrows on the caller's side.
    try {
      message.messageCallback();
    } catch (const std::exception& e) {
      reportException(&e);
    } catch (...) {
      reportException(nullptr);
    }
  }

  /** The hook, if one is set; itself kept from throwing back into the loop. */
  static void reportException(const std::exception* e) noexcept {
    auto handler = detail::msgloop::globals().exceptionHandler.load(std::memory_order_acquire);
    if (!handler) return;
    try {
      handler(e);
    } catch (...) {
    }
  }

  void deliverBroadcast(const std::string& text) {
    const std::vector<BroadcastListener*> copy = broadcastListeners_;
    for (BroadcastListener* listener : copy) {
      if (std::find(broadcastListeners_.begin(), broadcastListeners_.end(), listener) ==
          broadcastListeners_.end())
        continue;
      listener->broadcastMessageReceived(text);
    }
  }

  void quitReceivedNow() {
    quitReceived_.store(true, std::memory_order_release);
    platformStopLoop();
  }

  // ── Platform ──────────────────────────────────────────────────────────────

#if defined(_WIN32)
  static HMODULE thisModule() {
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR) &MessageManager::windowProc, &module);
    return module;
  }

  /** A class name of this module's own, as gfx/window_win32.h does: two
   *  modules sharing one class means a window running a procedure in a module
   *  that may since have been unloaded. */
  static const wchar_t* messageWindowClass() {
    static std::wstring name;
    static ATOM atom = 0;
    if (atom) return name.c_str();
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &MessageManager::windowProc;
    wc.hInstance = thisModule();
    for (int attempt = 0; attempt < 64 && !atom; ++attempt) {
      wchar_t buffer[96];
      std::swprintf(buffer, 96, L"SonoreMessageWindow_%p_%d", (void*) thisModule(), attempt);
      name = buffer;
      wc.lpszClassName = name.c_str();
      atom = RegisterClassExW(&wc);
      if (!atom && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) break;
    }
    if (!atom) return nullptr;
    return name.c_str();
  }

  static LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == detail::msgloop::kWakeMessage) {
      if (auto* mm = reinterpret_cast<MessageManager*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA))) {
        if (mm->platformYield(hwnd)) return 0;
        mm->dispatchQueue();
      }
      return 0;
    }
    if (message == WM_TIMER && wParam == detail::msgloop::kRearmTimer) {
      KillTimer(hwnd, detail::msgloop::kRearmTimer); // one wake-up, not a repeating one
      if (auto* mm = reinterpret_cast<MessageManager*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA)))
        mm->dispatchQueue();
      return 0;
    }
    if (message == WM_COPYDATA && GetWindowLongPtrW(hwnd, GWLP_USERDATA) != 0) {
      const auto* data = reinterpret_cast<const COPYDATASTRUCT*>(lParam);
      if (data && data->dwData == detail::msgloop::kBroadcastTag) {
        // Copied and QUEUED, not delivered here: this is a SENT message, the
        // other process is blocked until we return, and a listener doing
        // anything that talks to another process from inside it can stall
        // both. A megabyte is far more than a line of text needs.
        const size_t size = (std::min)((size_t) data->cbData, (size_t) 1 << 20);
        std::string text(data->lpData ? static_cast<const char*>(data->lpData) : "",
                         data->lpData ? size : 0);
        MessageManager::callAsync([text]() {
          if (MessageManager* mm = MessageManager::getInstanceWithoutCreating())
            mm->deliverBroadcast(text);
        });
        return TRUE;
      }
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
  }

  HWND createWindowOnThisThread() {
    const wchar_t* cls = messageWindowClass();
    if (!cls) return nullptr;
    HWND hwnd = CreateWindowExW(0, cls, detail::msgloop::kWindowTitle, 0, 0, 0, 0, 0,
                                HWND_MESSAGE, nullptr, thisModule(), nullptr);
    if (hwnd) SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR) this);
    return hwnd;
  }

  /**
   * Get rid of a message window, from whichever thread we are on.
   *
   * DestroyWindow only works on the thread that made the window. From any
   * other -- a static clean-up running at DLL unload, a setCurrentThread...
   * from a new thread -- the window's procedure is swapped for
   * DefWindowProcW first, so nothing can call into this module once it is
   * gone, and the owning thread is asked to close it.
   */
  static void retireWindow(HWND hwnd) {
    if (!hwnd) return;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
    if (GetWindowThreadProcessId(hwnd, nullptr) == GetCurrentThreadId()) {
      DestroyWindow(hwnd);
      return;
    }
    SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR) &DefWindowProcW);
    PostMessageW(hwnd, WM_CLOSE, 0, 0);
  }

  void platformInitialise() { window_ = createWindowOnThisThread(); }

  void platformShutdown() {
    HWND old = nullptr;
    {
      std::lock_guard<std::mutex> lock(detail::msgloop::globals().mutex);
      old = window_;
      window_ = nullptr;
    }
    retireWindow(old);
  }

  void platformMoveToCurrentThread() {
    HWND fresh = createWindowOnThisThread();
    HWND old = nullptr;
    {
      std::lock_guard<std::mutex> lock(detail::msgloop::globals().mutex);
      old = window_;
      window_ = fresh;
      // Whatever was waiting was woken on the OLD thread, which may never
      // pump again. Wake the new one.
      wakePending_ = !queue_.empty();
      if (wakePending_ && !platformWake()) wakePending_ = false;
    }
    retireWindow(old);
  }

  /** Called with the globals mutex held. */
  bool platformWake() {
    return window_ && PostMessageW(window_, detail::msgloop::kWakeMessage, 0, 0) != 0;
  }

  /**
   * A wake-up while the queue has not been caught up with for a while: let
   * what else is waiting go first. On the message thread; true when the next
   * batch was put off.
   *
   * Windows hands a thread its POSTED messages before its input, its paint
   * and its timers. Under a worker that posts faster than this thread runs,
   * a wake-up is always posted again -- by the worker, or by the re-arm --
   * and taken at once, every time: during a one-second flood a 10 ms timer
   * (an editor's tick) did not fire once, and clicks and repaints waited
   * with it. So once the queue has been worked on for kYieldAfterBusyMs
   * without running dry, a wake-up that finds any of those waiting asks for
   * the next batch by a timer instead, which Windows hands out after all of
   * them; the wake-up stays pending meanwhile, so posting adds no other.
   * Not sooner: a burst is still drained at full speed, and an ordinary call
   * is never delayed behind a repaint.
   */
  bool platformYield(HWND hwnd) {
    if (!busy_ || std::chrono::steady_clock::now() - busySince_ <
                      std::chrono::milliseconds(detail::msgloop::kYieldAfterBusyMs))
      return false;
    const UINT kinds = QS_INPUT | QS_PAINT | QS_TIMER | QS_HOTKEY;
    if ((HIWORD(GetQueueStatus(kinds)) & kinds) == 0) return false;
    if (SetTimer(hwnd, detail::msgloop::kRearmTimer, USER_TIMER_MINIMUM, nullptr) == 0) return false;
    busy_ = false; // they get their turn now, and the next stretch is timed afresh
    return true;
  }

  void platformStopLoop() {}

  HWND window_ = nullptr;
  // Message thread only: the queue has not run dry since busySince_.
  bool busy_ = false;
  std::chrono::steady_clock::time_point busySince_{};

#elif defined(__APPLE__)
  static void performSource(void* info) {
    if (info) static_cast<MessageManager*>(info)->dispatchQueue();
  }

  static void broadcastArrived(CFNotificationCenterRef, void*, CFStringRef, const void*,
                               CFDictionaryRef userInfo) {
    if (!userInfo) return;
    const void* pidValue = CFDictionaryGetValue(userInfo, CFSTR("pid"));
    if (pidValue && CFGetTypeID((CFTypeRef) pidValue) == CFNumberGetTypeID()) {
      SInt64 sender = 0;
      CFNumberGetValue((CFNumberRef) pidValue, kCFNumberSInt64Type, &sender);
      if (sender == (SInt64) getpid()) return; // our own, as on Windows
    }
    const void* textValue = CFDictionaryGetValue(userInfo, CFSTR("message"));
    if (!textValue || CFGetTypeID((CFTypeRef) textValue) != CFStringGetTypeID()) return;
    CFStringRef cfText = (CFStringRef) textValue;
    const CFIndex capacity =
        CFStringGetMaximumSizeForEncoding(CFStringGetLength(cfText), kCFStringEncodingUTF8) + 1;
    if (capacity <= 0 || capacity > (CFIndex) (1 << 20)) return;
    std::vector<char> buffer((size_t) capacity, '\0');
    if (!CFStringGetCString(cfText, buffer.data(), capacity, kCFStringEncodingUTF8)) return;
    const std::string text(buffer.data());
    MessageManager::callAsync([text]() {
      if (MessageManager* mm = MessageManager::getInstanceWithoutCreating())
        mm->deliverBroadcast(text);
    });
  }

  CFRunLoopSourceRef createSourceOnThisThread(CFRunLoopRef* loopOut) {
    CFRunLoopRef loop = CFRunLoopGetCurrent();
    CFRunLoopSourceContext context;
    std::memset(&context, 0, sizeof(context));
    context.info = this;
    context.perform = &MessageManager::performSource;
    CFRunLoopSourceRef source = CFRunLoopSourceCreate(kCFAllocatorDefault, 0, &context);
    if (source) {
      // The COMMON modes: the default mode alone stops firing under a modal
      // panel or while a slider is dragged, which is exactly when a worker
      // finishing something wants to say so.
      CFRunLoopAddSource(loop, source, kCFRunLoopCommonModes);
      CFRetain((CFTypeRef) loop);
      *loopOut = loop;
    }
    return source;
  }

  static void releaseSource(CFRunLoopSourceRef source, CFRunLoopRef loop) {
    // Invalidate is safe from any thread and removes it from every loop, so
    // perform can never be called on a manager that is gone.
    if (source) {
      CFRunLoopSourceInvalidate(source);
      CFRelease((CFTypeRef) source);
    }
    if (loop) CFRelease((CFTypeRef) loop);
  }

  void platformInitialise() {
    source_ = createSourceOnThisThread(&runLoop_);
    if (CFNotificationCenterRef centre = CFNotificationCenterGetDistributedCenter()) {
      CFStringRef name =
          CFStringCreateWithCString(kCFAllocatorDefault, detail::msgloop::kBroadcastName,
                                    kCFStringEncodingUTF8);
      if (name) {
        CFNotificationCenterAddObserver(centre, this, &MessageManager::broadcastArrived, name,
                                        nullptr,
                                        CFNotificationSuspensionBehaviorDeliverImmediately);
        CFRelease((CFTypeRef) name);
        observing_ = true;
      }
    }
  }

  void platformShutdown() {
    if (observing_)
      if (CFNotificationCenterRef centre = CFNotificationCenterGetDistributedCenter())
        CFNotificationCenterRemoveEveryObserver(centre, this);
    observing_ = false;
    CFRunLoopSourceRef source = nullptr;
    CFRunLoopRef loop = nullptr;
    {
      std::lock_guard<std::mutex> lock(detail::msgloop::globals().mutex);
      source = source_;
      loop = runLoop_;
      source_ = nullptr;
      runLoop_ = nullptr;
    }
    releaseSource(source, loop);
  }

  void platformMoveToCurrentThread() {
    CFRunLoopRef freshLoop = nullptr;
    CFRunLoopSourceRef fresh = createSourceOnThisThread(&freshLoop);
    CFRunLoopSourceRef old = nullptr;
    CFRunLoopRef oldLoop = nullptr;
    {
      std::lock_guard<std::mutex> lock(detail::msgloop::globals().mutex);
      old = source_;
      oldLoop = runLoop_;
      source_ = fresh;
      runLoop_ = freshLoop;
      wakePending_ = !queue_.empty();
      if (wakePending_ && !platformWake()) wakePending_ = false;
    }
    releaseSource(old, oldLoop);
  }

  /** Called with the globals mutex held. */
  bool platformWake() {
    if (!source_ || !runLoop_) return false;
    CFRunLoopSourceSignal(source_);
    CFRunLoopWakeUp(runLoop_);
    return true;
  }

  void platformStopLoop() {
    using detail::msgloop::objcSend;
    if (id app = detail::msgloop::applicationOrNil()) {
      if (objcSend<BOOL>(app, "isRunning")) {
        objcSend<void>(app, "stop:", (id) 0);
        // -stop: is only noticed after the NEXT event. One that does nothing,
        // so -run returns now rather than at the user's next mouse move.
        Class eventClass = objc_getClass("NSEvent");
        if (eventClass) {
          const detail::msgloop::EventPoint origin{0.0, 0.0};
          id event = objcSend<id>(
              (id) eventClass,
              "otherEventWithType:location:modifierFlags:timestamp:windowNumber:context:"
              "subtype:data1:data2:",
              (unsigned long) 15 /* NSEventTypeApplicationDefined */, origin, (unsigned long) 0,
              0.0, (long) 0, (id) 0, (short) 0, (long) 0, (long) 0);
          if (event) objcSend<void>(app, "postEvent:atStart:", event, (BOOL) 1);
        }
      }
    }
    if (runLoop_) CFRunLoopStop(runLoop_);
  }

  CFRunLoopRef runLoop_ = nullptr;
  CFRunLoopSourceRef source_ = nullptr;
  bool observing_ = false;

#else
  struct FdCallback {
    int fd;
    std::function<void(int)> callback;
  };

  static void makeNonBlocking(int fd) {
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
  }

  void platformInitialise() {
#if defined(__linux__)
    // One descriptor that counts, rather than a pipe's two that can fill up:
    // writes never block and never fail for want of room.
    wakeReadFd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (wakeReadFd_ >= 0) {
      wakeWriteFd_ = wakeReadFd_;
      eventFd_ = true;
      return;
    }
#endif
    int ends[2] = {-1, -1};
    if (pipe(ends) == 0) {
      makeNonBlocking(ends[0]);
      makeNonBlocking(ends[1]);
      wakeReadFd_ = ends[0];
      wakeWriteFd_ = ends[1];
    }
  }

  void platformShutdown() {
    int readFd = -1, writeFd = -1;
    {
      std::lock_guard<std::mutex> lock(detail::msgloop::globals().mutex);
      readFd = wakeReadFd_;
      writeFd = wakeWriteFd_;
      wakeReadFd_ = wakeWriteFd_ = -1;
    }
    if (readFd >= 0) close(readFd);
    if (writeFd >= 0 && writeFd != readFd) close(writeFd);
  }

  /** The descriptor is not tied to a thread: whoever polls it next is the
   *  message thread. Only a wake-up is owed, in case the old one was taken. */
  void platformMoveToCurrentThread() {
    std::lock_guard<std::mutex> lock(detail::msgloop::globals().mutex);
    wakePending_ = !queue_.empty();
    if (wakePending_ && !platformWake()) wakePending_ = false;
  }

  /** Called with the globals mutex held. */
  bool platformWake() {
    if (wakeWriteFd_ < 0) return false;
    if (eventFd_) {
      const uint64_t one = 1;
      const auto wrote = write(wakeWriteFd_, &one, sizeof(one));
      return wrote == (decltype(wrote)) sizeof(one);
    }
    const unsigned char one = 1;
    // A full pipe already holds a wake-up, which is all a write was for.
    const auto wrote = write(wakeWriteFd_, &one, 1);
    return wrote == 1 || errno == EAGAIN;
  }

  void drainWakeUp() {
    if (wakeReadFd_ < 0) return;
    unsigned char buffer[64];
    for (int i = 0; i < 64; ++i)
      if (read(wakeReadFd_, buffer, sizeof(buffer)) <= 0) break;
  }

  /** One poll of the wake-up and every registered descriptor. False when
   *  this manager was deleted by something it ran. */
  bool pollOnce(int timeoutMs, uint64_t generation) {
    std::vector<pollfd> fds;
    fds.reserve(fdCallbacks_.size() + 1);
    pollfd wake{};
    wake.fd = wakeReadFd_;
    wake.events = POLLIN;
    fds.push_back(wake);
    for (const FdCallback& c : fdCallbacks_) {
      pollfd entry{};
      entry.fd = c.fd;
      entry.events = POLLIN;
      fds.push_back(entry);
    }
    const int ready = poll(fds.data(), (nfds_t) fds.size(), timeoutMs);
    if (ready <= 0) return true;
    if (fds[0].revents != 0) {
      drainWakeUp();
      dispatchQueue();
      if (!stillAlive(generation)) return false;
    }
    for (size_t i = 1; i < fds.size(); ++i) {
      if (fds[i].revents == 0) continue;
      // Looked up again: an earlier callback may have unregistered this one.
      std::function<void(int)> callback;
      for (const FdCallback& c : fdCallbacks_)
        if (c.fd == fds[i].fd) callback = c.callback;
      if (callback) callback(fds[i].fd);
      if (!stillAlive(generation)) return false;
    }
    return true;
  }

  void platformStopLoop() {}

  int wakeReadFd_ = -1;
  int wakeWriteFd_ = -1;
  bool eventFd_ = false;
  std::vector<FdCallback> fdCallbacks_;
#endif

  std::atomic<std::thread::id> messageThread_{std::thread::id()};
  std::atomic<bool> quitPosted_{false};
  std::atomic<bool> quitReceived_{false};
  // Guarded by the globals mutex.
  std::deque<std::unique_ptr<MessageBase>> queue_;
  bool wakePending_ = false;
  // Message thread only.
  std::vector<BroadcastListener*> broadcastListeners_;
};

/**
 * A message with a callback: subclass, implement messageCallback(), and
 * `(new MyMessage(...))->post()`. The queue deletes it after it has run.
 */
class CallbackMessage : public MessageManager::MessageBase {
public:
  /** Made with `new` by a subclass, then post()ed. */
  CallbackMessage() = default;
  /** Run by the queue, never by the poster; see MessageBase. */
  ~CallbackMessage() override = default;
};

/**
 * What a MessageListener receives. Subclass it to carry data; post it with
 * MessageListener::postMessage, which takes ownership.
 */
class Message : public MessageManager::MessageBase {
public:
  /** Made with `new` by a subclass, then handed to postMessage(). */
  Message() = default;
  /** Run by the queue after handleMessage(), or without it when the
   *  listener has gone. */
  ~Message() override = default;

private:
  friend class MessageListener;
  void messageCallback() final;

  std::shared_ptr<detail::msgloop::ListenerAnchor> recipient_;
};

/**
 * Something that receives Messages on the message thread.
 *
 * Messages posted to a listener that has since been deleted are dropped
 * rather than delivered to freed memory. Deleting a listener on another
 * thread waits for a delivery to it that is already running. One caveat the
 * language imposes: by the time ~MessageListener runs, a derived class's own
 * members are gone, so a listener that can be deleted on a thread other than
 * the message thread should be deleted while holding a MessageManagerLock.
 */
class MessageListener {
public:
  /** Messages still queued for this listener are dropped; one being
   *  delivered on another thread right now is waited for. */
  virtual ~MessageListener() {
    std::lock_guard<std::recursive_mutex> lock(anchor_->mutex);
    anchor_->target = nullptr;
  }

  /** Runs on the message thread. */
  virtual void handleMessage(const Message& message) = 0;

  /** Queue a message for this listener; takes ownership of it. Any thread.
   *  False (and the message deleted) when there is no MessageManager. */
  bool postMessage(Message* message) const {
    if (!message) return false;
    message->recipient_ = anchor_;
    return message->post();
  }

  ///@{
  /** Not copyable: queued messages are addressed to this object. */
  MessageListener(const MessageListener&) = delete;
  MessageListener& operator=(const MessageListener&) = delete;
  ///@}

protected:
  /** For subclasses only. */
  MessageListener() : anchor_(std::make_shared<detail::msgloop::ListenerAnchor>()) {
    anchor_->target = this;
  }

private:
  std::shared_ptr<detail::msgloop::ListenerAnchor> anchor_;
};

/**
 * Exclusive access to the message thread, from any thread, for as long as
 * this object lives.
 *
 * On the message thread -- or nested inside another lock on the same thread
 * -- it is gained at once. Anywhere else it waits for the message thread to
 * reach a safe point between two messages and park there (see the header);
 * while it is held, nothing else runs on the message thread, so this thread
 * may touch components as if it were the message thread.
 *
 * Pass the thread's ThreadExitSignal, or a ThreadPool job's stop flag, so
 * that stopping the thread from the message thread cannot deadlock against
 * this wait: the lock gives up and lockWasGained() says so. ALWAYS check it.
 *
 * Do not, while holding it, wait for anything the message thread must do:
 * it is parked. SendMessage to a window it owns is the classic way.
 */
class MessageManagerLock {
public:
  /** Waits for as long as it takes, or until there is no manager. */
  MessageManagerLock() : locked_(attempt(nullptr, nullptr)) {}

  /** Gives up the moment `exitSignal` is raised. */
  explicit MessageManagerLock(ThreadExitSignal& exitSignal) : locked_(attempt(&exitSignal, nullptr)) {}

  /** Gives up when `shouldExit` becomes true -- a ThreadPool job's flag.
   *  Looked at every 2 ms, since a bare atomic cannot wake anyone. */
  explicit MessageManagerLock(const std::atomic<bool>& shouldExit)
      : locked_(attempt(nullptr, &shouldExit)) {}

  /** Releases the lock if it was gained: the message thread goes on to its
   *  next message. */
  ~MessageManagerLock() { release(); }

  /** False when the wait was abandoned -- the exit signal or flag was
   *  raised, or there is no manager -- and then nothing may be touched. */
  bool lockWasGained() const noexcept { return locked_; }

  ///@{
  /** Not copyable: the lock is this object's lifetime. */
  MessageManagerLock(const MessageManagerLock&) = delete;
  MessageManagerLock& operator=(const MessageManagerLock&) = delete;
  ///@}

private:
  bool attempt(ThreadExitSignal* signal, const std::atomic<bool>* flag);
  void release();

  // Declared before locked_: attempt() fills it in during locked_'s
  // initialisation.
  std::shared_ptr<detail::msgloop::LockRequest> request_;
  bool locked_ = false;
};

/**
 * An update that can be asked for from any thread, coalesced into one call
 * on the message thread -- and CANCELLED with a guarantee.
 *
 * The callback runs under this object's lock, and cancelPendingUpdate() takes
 * the same lock: if the callback is running, cancel waits for it to finish;
 * once cancel returns, a pending callback will not start. That is the whole
 * difference from an updater built on a bare flag, where a callback can
 * start between "cancel" and the line after it. The lock is recursive, so the
 * callback may trigger or cancel itself.
 *
 * The price is the same lock: a callback that waits for a thread that is
 * calling trigger or cancel deadlocks, and trigger blocks while a callback
 * runs.
 */
class LockingAsyncUpdater {
public:
  /** `callback` is what each coalesced update runs, on the message thread
   *  and under this object's lock. */
  explicit LockingAsyncUpdater(std::function<void()> callback)
      : impl_(std::make_shared<Impl>()) {
    impl_->callback = std::move(callback);
  }

  /** Waits for a running callback, and guarantees none starts afterwards. */
  ~LockingAsyncUpdater() {
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
    impl_->callback = nullptr;
    impl_->canPost = true;
  }

  /** Ask for the callback. Any number of calls before it runs make one
   *  call. Any thread except the audio thread. */
  void triggerAsyncUpdate() {
    {
      std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
      if (!impl_->canPost) return;
      impl_->canPost = false;
    }
    std::shared_ptr<Impl> impl = impl_;
    if (MessageManager::callAsync([impl]() { impl->fire(); })) return;
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
    impl_->canPost = true;
  }

  /** After this returns, a pending callback will not run (until the next
   *  trigger). Waits for one that is running now. */
  void cancelPendingUpdate() noexcept {
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
    impl_->canPost = true;
  }

  /** Run a pending callback now, on this thread. */
  void handleUpdateNowIfNeeded() { impl_->fire(); }

  /** True from a trigger until its callback starts or it is cancelled. Takes
   *  the lock, so it waits while a callback runs. */
  bool isUpdatePending() const noexcept {
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
    return !impl_->canPost;
  }

  ///@{
  /** Not copyable: messages in flight belong to this updater. */
  LockingAsyncUpdater(const LockingAsyncUpdater&) = delete;
  LockingAsyncUpdater& operator=(const LockingAsyncUpdater&) = delete;
  ///@}

private:
  /** What the updater shares with its messages in flight: the callback,
   *  whether an update is pending, and the recursive lock that trigger,
   *  cancel and the callback all run under. A message that arrives after the
   *  updater is gone finds the callback cleared and nothing pending. */
  struct Impl {
    mutable std::recursive_mutex mutex;
    std::function<void()> callback;
    /** True when nothing is pending. A message that arrives and finds it
     *  true was cancelled, and does nothing. */
    bool canPost = true;

    void fire() {
      std::lock_guard<std::recursive_mutex> lock(mutex);
      if (canPost) return;
      canPost = true; // before the call, so the callback can re-trigger
      // A copy runs: a callback that deletes its own updater clears
      // `callback`, and destroying a std::function while it is executing
      // destroys the lambda it is in the middle of.
      const std::function<void()> running = callback;
      if (running) running();
    }
  };

  // Shared with the messages in flight, so one arriving after the updater
  // is gone finds an Impl with no callback rather than freed memory.
  std::shared_ptr<Impl> impl_;
};

// ── Out-of-line definitions ─────────────────────────────────────────────────

namespace detail {
namespace msgloop {

class FunctionMessage : public MessageManager::MessageBase {
public:
  explicit FunctionMessage(std::function<void()> fn) : function_(std::move(fn)) {}
  void messageCallback() override {
    if (function_) function_();
  }

private:
  std::function<void()> function_;
};

class SyncCallMessage : public MessageManager::MessageBase {
public:
  SyncCallMessage(const std::function<void()>& fn, std::shared_ptr<SyncCallState> state)
      : function_(fn), state_(std::move(state)) {}

  /** Never run: release the waiter rather than leave it blocked forever. */
  ~SyncCallMessage() override { state_->finish(false); }

  void messageCallback() override {
    try {
      function_();
    } catch (...) {
      std::lock_guard<std::mutex> lock(state_->mutex);
      state_->error = std::current_exception();
    }
    state_->finish(true);
  }

private:
  std::function<void()> function_;
  std::shared_ptr<SyncCallState> state_;
};

/** The message a MessageManagerLock posts: it parks the message thread. */
class BlockingMessage : public MessageManager::MessageBase {
public:
  explicit BlockingMessage(std::shared_ptr<LockRequest> request) : request_(std::move(request)) {}

  ~BlockingMessage() override {
    if (delivered_) return;
    std::lock_guard<std::mutex> lock(request_->mutex);
    if (request_->state == LockRequest::State::Waiting) {
      request_->state = LockRequest::State::Dropped;
      request_->changed.notify_all();
    }
  }

  void messageCallback() override {
    delivered_ = true;
    std::unique_lock<std::mutex> lock(request_->mutex);
    // Abandoned before we got here: the requester's exit signal won.
    if (request_->state != LockRequest::State::Waiting) return;
    request_->state = LockRequest::State::Acquired;
    request_->changed.notify_all();
    // The safe point. Nothing else runs on this thread until the holder lets go.
    request_->changed.wait(lock, [this]() {
      return request_->state != LockRequest::State::Acquired;
    });
  }

private:
  std::shared_ptr<LockRequest> request_;
  bool delivered_ = false;
};

/** Deletes a manager nobody deleted, when the binary unloads -- above all a
 *  plugin DLL, whose window procedure would otherwise outlive its code. */
struct Reaper {
  ~Reaper() { MessageManager::deleteInstance(); }
};

} // namespace msgloop
} // namespace detail

inline bool MessageManager::MessageBase::post() { return MessageManager::postToQueue(this); }

inline MessageManager* MessageManager::getInstance() {
  detail::msgloop::Globals& g = detail::msgloop::globals();
  if (MessageManager* existing = g.instance.load(std::memory_order_acquire)) return existing;
  MessageManager* created = nullptr;
  {
    std::lock_guard<std::mutex> lock(g.mutex);
    if (MessageManager* existing = g.instance.load(std::memory_order_acquire)) return existing;
    created = new MessageManager();
    g.instance.store(created, std::memory_order_release);
  }
  // After globals(), so it is destroyed BEFORE them.
  static detail::msgloop::Reaper reaper;
  (void) reaper;
  return created;
}

inline void MessageManager::deleteInstance() {
  detail::msgloop::Globals& g = detail::msgloop::globals();
  MessageManager* mm = nullptr;
  std::deque<std::unique_ptr<MessageBase>> leftovers;
  {
    std::lock_guard<std::mutex> lock(g.mutex);
    mm = g.instance.load(std::memory_order_acquire);
    if (!mm) return;
    g.instance.store(nullptr, std::memory_order_release);
    ++g.generation;
    leftovers.swap(mm->queue_);
  }
  mm->platformShutdown();
  // Destroyed undelivered: a blocked caller is released, a lock request is
  // told it was dropped.
  leftovers.clear();
  delete mm;
}

inline void MessageManager::setCurrentThreadAsMessageThread() {
  const std::thread::id me = std::this_thread::get_id();
  if (messageThread_.load(std::memory_order_acquire) == me) return;
  messageThread_.store(me, std::memory_order_release);
  platformMoveToCurrentThread();
}

inline bool MessageManager::callAsync(std::function<void()> function) {
  if (!function) return false;
  return postToQueue(new detail::msgloop::FunctionMessage(std::move(function)));
}

inline bool MessageManager::callFunctionOnMessageThread(const std::function<void()>& function) {
  if (!function) return false;
  // Already in possession of the message thread -- on it, or holding the lock
  // that has it parked. Queueing would mean waiting for ourselves.
  if (currentThreadHasLockedMessageManager()) {
    function();
    return true;
  }
  auto state = std::make_shared<detail::msgloop::SyncCallState>();
  if (!postToQueue(new detail::msgloop::SyncCallMessage(function, state))) return false;
  std::unique_lock<std::mutex> lock(state->mutex);
  state->done.wait(lock, [&state]() { return state->finished; });
  if (state->error) std::rethrow_exception(state->error);
  return state->ran;
}

inline void* MessageManager::callFunctionOnMessageThread(MessageCallbackFunction* function,
                                                         void* userData) {
  if (!function) return nullptr;
  void* result = nullptr;
  callFunctionOnMessageThread(std::function<void()>([&]() { result = function(userData); }));
  return result;
}

inline void MessageManager::stopDispatchLoop() {
  quitPosted_.store(true, std::memory_order_release);
  // Through the queue, so everything posted before the stop still runs.
  postToQueue(new detail::msgloop::FunctionMessage([]() {
    if (MessageManager* mm = MessageManager::getInstanceWithoutCreating()) mm->quitReceivedNow();
  }));
}

inline int MessageManager::numPendingMessages() const {
  std::lock_guard<std::mutex> lock(detail::msgloop::globals().mutex);
  return (int) queue_.size();
}

inline int MessageManager::dispatchPendingMessages() {
  if (!isThisTheMessageThread()) return 0;
#if !defined(_WIN32) && !defined(__APPLE__)
  drainWakeUp();
#endif
  return dispatchQueue();
}

inline void MessageManager::runDispatchLoop() {
  if (!isThisTheMessageThread()) return;
  const uint64_t generation = currentGeneration();
#if defined(_WIN32)
  MSG msg;
  while (!quitReceived_.load(std::memory_order_acquire)) {
    const BOOL got = GetMessageW(&msg, nullptr, 0, 0);
    if (got == 0 || got == -1) break; // WM_QUIT, or no queue at all
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
    if (!stillAlive(generation)) return;
  }
#elif defined(__APPLE__)
  using detail::msgloop::objcSend;
  // NSApp's -run is the MAIN thread's (see runDispatchLoopUntil); a message
  // thread elsewhere runs its own run loop.
  id app = pthread_main_np() ? detail::msgloop::applicationOrNil() : (id) 0;
  while (!quitReceived_.load(std::memory_order_acquire)) {
    if (app) objcSend<void>(app, "run"); // returns after -stop:
    else CFRunLoopRun();                 // returns after CFRunLoopStop
    if (!stillAlive(generation)) return;
  }
#else
  while (!quitReceived_.load(std::memory_order_acquire))
    if (!pollOnce(-1, generation)) return;
#endif
  // The stop is consumed, so the loop can be run again -- an application
  // that runs a second loop for a modal session, and every test.
  quitReceived_.store(false, std::memory_order_release);
  quitPosted_.store(false, std::memory_order_release);
}

inline bool MessageManager::runDispatchLoopUntil(int milliseconds) {
  if (!isThisTheMessageThread()) return !quitReceived_.load(std::memory_order_acquire);
  const uint64_t generation = currentGeneration();
  const auto end = std::chrono::steady_clock::now() +
                   std::chrono::milliseconds(milliseconds > 0 ? milliseconds : 0);
  for (;;) {
    if (quitReceived_.load(std::memory_order_acquire)) return false;
    const auto now = std::chrono::steady_clock::now();
    const long long remaining =
        (long long) std::chrono::duration_cast<std::chrono::milliseconds>(end - now).count();
#if defined(_WIN32)
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT) {
        // Not ours to swallow: put it back for whichever loop owns the app.
        PostQuitMessage((int) msg.wParam);
        return false;
      }
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
      if (!stillAlive(generation)) return false;
      if (quitReceived_.load(std::memory_order_acquire)) return false;
      // The time is up even if the OS never runs dry: a window that
      // invalidates itself as it paints always has a WM_PAINT ready, and
      // draining "until empty" never returned -- a modal box over it ran on
      // after it was answered.
      if (std::chrono::steady_clock::now() >= end) break;
    }
    if (remaining <= 0 || std::chrono::steady_clock::now() >= end) break;
    MsgWaitForMultipleObjectsEx(0, nullptr, (DWORD) (std::min)(remaining, 1000LL), QS_ALLINPUT,
                                MWMO_INPUTAVAILABLE);
#elif defined(__APPLE__)
    using detail::msgloop::objcSend;
    if (remaining <= 0) {
      dispatchQueue();
      break;
    }
    const double slice = (double) (std::min)(remaining, 10LL) / 1000.0;
    // AppKit's event queue belongs to the MAIN thread: nextEventMatchingMask
    // anywhere else throws, and so does creating NSApp there, which
    // applicationOrNil() does. A message thread moved to another thread
    // (setCurrentThreadAsMessageThread) aborted the process on its first
    // dispatch. Off the main thread the loop runs that thread's own run loop,
    // which is where the wake source was moved to.
    if (id app = pthread_main_np() ? detail::msgloop::applicationOrNil() : (id) 0) {
      id pool = objcSend<id>(objcSend<id>((id) objc_getClass("NSAutoreleasePool"), "alloc"), "init");
      id until = objcSend<id>((id) objc_getClass("NSDate"), "dateWithTimeIntervalSinceNow:", slice);
      id event = objcSend<id>(app, "nextEventMatchingMask:untilDate:inMode:dequeue:",
                              ~(unsigned long long) 0, until, (id) kCFRunLoopDefaultMode,
                              (BOOL) 1);
      if (event) objcSend<void>(app, "sendEvent:", event);
      if (pool) objcSend<void>(pool, "drain");
    } else {
      CFRunLoopRunInMode(kCFRunLoopDefaultMode, slice, true);
    }
    if (!stillAlive(generation)) return false;
#else
    if (!pollOnce((int) (std::max)(0LL, (std::min)(remaining, 1000LL)), generation)) return false;
    if (remaining <= 0) break;
#endif
  }
  return !quitReceived_.load(std::memory_order_acquire);
}

inline bool MessageManager::broadcastMessage(const std::string& message) {
#if defined(_WIN32)
  MessageManager* mm = getInstanceWithoutCreating();
  HWND self = nullptr;
  if (mm) {
    std::lock_guard<std::mutex> lock(detail::msgloop::globals().mutex);
    self = mm->window_;
  }
  std::vector<HWND> targets;
  HWND found = nullptr;
  while ((found = FindWindowExW(HWND_MESSAGE, found, nullptr, detail::msgloop::kWindowTitle)) !=
         nullptr)
    if (found != self) targets.push_back(found);
  COPYDATASTRUCT data{};
  data.dwData = detail::msgloop::kBroadcastTag;
  data.cbData = (DWORD) message.size();
  data.lpData = (void*) message.data();
  int delivered = 0;
  for (HWND target : targets) {
    DWORD_PTR result = 0;
    // NORMAL, not BLOCK: two processes broadcasting to each other at once
    // each keep answering the other's copy while they wait, instead of
    // standing in a two-second deadlock.
    if (SendMessageTimeoutW(target, WM_COPYDATA, (WPARAM) self, (LPARAM) &data,
                            SMTO_NORMAL | SMTO_ABORTIFHUNG, 2000, &result) != 0)
      ++delivered;
  }
  return delivered > 0;
#elif defined(__APPLE__)
  CFNotificationCenterRef centre = CFNotificationCenterGetDistributedCenter();
  if (!centre) return false;
  CFStringRef name = CFStringCreateWithCString(kCFAllocatorDefault, detail::msgloop::kBroadcastName,
                                               kCFStringEncodingUTF8);
  CFStringRef text = CFStringCreateWithCString(kCFAllocatorDefault, message.c_str(),
                                               kCFStringEncodingUTF8);
  const SInt64 pid = (SInt64) getpid();
  CFNumberRef sender = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt64Type, &pid);
  CFMutableDictionaryRef info = CFDictionaryCreateMutable(
      kCFAllocatorDefault, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
  bool sent = false;
  if (name && text && sender && info) {
    CFDictionarySetValue(info, CFSTR("message"), text);
    CFDictionarySetValue(info, CFSTR("pid"), sender);
    CFNotificationCenterPostNotification(centre, name, nullptr, (CFDictionaryRef) info, true);
    sent = true;
  }
  if (info) CFRelease((CFTypeRef) info);
  if (sender) CFRelease((CFTypeRef) sender);
  if (text) CFRelease((CFTypeRef) text);
  if (name) CFRelease((CFTypeRef) name);
  return sent;
#else
  (void) message;
  return false;
#endif
}

inline void Message::messageCallback() {
  if (!recipient_) return;
  std::lock_guard<std::recursive_mutex> lock(recipient_->mutex);
  if (MessageListener* target = recipient_->target) target->handleMessage(*this);
}

inline bool MessageManagerLock::attempt(ThreadExitSignal* signal, const std::atomic<bool>* flag) {
  using detail::msgloop::LockRequest;
  auto shouldAbort = [signal, flag]() {
    return (signal && signal->threadShouldExit()) ||
           (flag && flag->load(std::memory_order_acquire));
  };

  MessageManager* mm = MessageManager::getInstanceWithoutCreating();
  if (!mm) return false;
  // The message thread itself, or a thread already holding a lock: it has
  // the access already, and waiting for the message thread would be waiting
  // for ourselves.
  if (mm->currentThreadHasLockedMessageManager()) return true;
  if (shouldAbort()) return false;

  auto request = std::make_shared<LockRequest>();
  if (signal) signal->addWaiter(request);
  struct Unregister {
    ThreadExitSignal* signal;
    std::shared_ptr<LockRequest> request;
    ~Unregister() {
      if (signal) signal->removeWaiter(request);
    }
  } unregister{signal, request};

  if (!MessageManager::postToQueue(new detail::msgloop::BlockingMessage(request))) return false;

  std::unique_lock<std::mutex> lock(request->mutex);
  for (;;) {
    if (request->state == LockRequest::State::Acquired) {
      if (!shouldAbort()) break;
      // Granted at the moment the thread was told to stop: give it straight
      // back. A caller that is exiting must not be told it holds the lock.
      request->state = LockRequest::State::Released;
      request->changed.notify_all();
      return false;
    }
    if (request->state == LockRequest::State::Dropped) return false;
    if (shouldAbort()) {
      request->state = LockRequest::State::Abandoned;
      request->changed.notify_all();
      return false;
    }
    if (flag) request->changed.wait_for(lock, std::chrono::milliseconds(2));
    else request->changed.wait(lock);
  }
  lock.unlock();
  request_ = std::move(request);
  detail::msgloop::globals().threadWithLock.store(std::this_thread::get_id(),
                                                  std::memory_order_release);
  return true;
}

inline void MessageManagerLock::release() {
  if (!request_) return;
  // Cleared before the message thread is let go, so it never sees itself
  // running while another thread is still recorded as the holder.
  detail::msgloop::globals().threadWithLock.store(std::thread::id(), std::memory_order_release);
  {
    std::lock_guard<std::mutex> lock(request_->mutex);
    request_->state = detail::msgloop::LockRequest::State::Released;
    request_->changed.notify_all();
  }
  request_.reset();
  locked_ = false;
}

} // namespace sonore
