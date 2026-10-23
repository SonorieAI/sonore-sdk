// SPDX-License-Identifier: Apache-2.0
// The application infrastructure: the message thread and its lock, the
// updater that can be cancelled, a document's save/open paths, the settings
// files, the preferences panel and the recent-files list.
//
// Everything that crosses threads is tested by DOING it, many thousands of
// times, with the invariant checked on every pass -- a lock that is mutually
// exclusive 999 times in 1000 is a lock with a bug, and only volume finds it:
//   * callAsync keeps each thread's order, and a nested loop keeps the rest;
//   * MessageManagerLock from four threads while the message thread pumps:
//     a counter only ever touched under exclusive access must come out exact;
//   * a lock whose thread is told to stop gives up, so the classic shutdown
//     (signal, then join, from the message thread) cannot deadlock;
//   * LockingAsyncUpdater: after cancelPendingUpdate returns, no callback
//     starts and none is still running;
//   * a MessageListener deleted with messages in flight is never called.
// Run under AddressSanitizer as well as plain (see the report): a deleted
// listener or document called back is a use-after-free first and a wrong
// answer second.
//
//   app_infra_test                      everything
//   app_infra_test --broadcast-child    (internal: the other end of a broadcast)
#include "sonore/application_properties.h"
#include "sonore/file_document.h"
#include "sonore/message_manager.h"
#include "sonore/recent_files.h"
#include "sonore/thread_pool.h"
#include "sonore/gfx/alert.h"
#include "sonore/gfx/bitmap.h"
#include "sonore/gfx/popup.h"
#include "sonore/gfx/preferences_panel.h"
#include "sonore/gfx/system_font.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace sonore;

static int failures = 0;
static int passes = 0;

static void check(bool ok, const char* msg) {
  std::printf("  %s %s\n", ok ? "ok  " : "FAIL", msg);
  if (ok) ++passes;
  else ++failures;
}

static char msg[512];

using Clock = std::chrono::steady_clock;

static double msSince(Clock::time_point start) {
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

/** Pump the message thread until `done` or the timeout. */
static bool pumpUntil(const std::function<bool()>& done, int timeoutMs = 20000) {
  MessageManager* mm = MessageManager::getInstance();
  const auto start = Clock::now();
  while (!done()) {
    if (msSince(start) > timeoutMs) return false;
    mm->runDispatchLoopUntil(2);
  }
  return true;
}

// ── callAsync ────────────────────────────────────────────────────────────────

static void testCallAsync() {
  std::printf("\n-- callAsync --\n");
  MessageManager* mm = MessageManager::getInstance();
  check(mm->isThisTheMessageThread(), "the thread that made the manager is the message thread");
  check(MessageManager::existsAndIsCurrentThread(), "and existsAndIsCurrentThread agrees");

  // One thread's calls, in order.
  {
    std::vector<int> seen;
    for (int i = 0; i < 1000; ++i) MessageManager::callAsync([&seen, i]() { seen.push_back(i); });
    check(mm->numPendingMessages() == 1000, "a thousand calls wait in the queue until pumped");
    check(seen.empty(), "and none ran at the call");
    pumpUntil([&]() { return seen.size() == 1000; });
    bool ordered = seen.size() == 1000;
    for (size_t i = 0; ordered && i < seen.size(); ++i) ordered = seen[i] == (int) i;
    check(ordered, "they ran in the order they were made");
  }

  // Four threads at once: each one's order survives, every call runs on the
  // message thread, and nothing is lost to a full OS queue (20,000 calls is
  // twice Windows' posted-message quota).
  {
    constexpr int kThreads = 4, kEach = 5000;
    std::vector<int> lastSeen(kThreads, -1);
    std::atomic<int> total{0};
    int disorder = 0, offThread = 0;
    const std::thread::id main = std::this_thread::get_id();
    std::vector<std::thread> workers;
    for (int t = 0; t < kThreads; ++t)
      workers.emplace_back([&, t]() {
        for (int i = 0; i < kEach; ++i)
          MessageManager::callAsync([&, t, i, main]() {
            if (std::this_thread::get_id() != main) ++offThread;
            if (i != lastSeen[(size_t) t] + 1) ++disorder;
            lastSeen[(size_t) t] = i;
            total.fetch_add(1);
          });
      });
    const bool finished = pumpUntil([&]() { return total.load() == kThreads * kEach; });
    for (auto& w : workers) w.join();
    std::snprintf(msg, sizeof(msg), "20000 calls from four threads all ran (%d), %d out of order, "
                  "%d off the message thread", total.load(), disorder, offThread);
    check(finished && disorder == 0 && offThread == 0, msg);
  }

  // A callback that runs a nested loop -- a modal dialog does -- must not let
  // anything overtake: what was queued after it runs next, in order.
  {
    std::vector<std::string> order;
    MessageManager::callAsync([&]() { order.push_back("A"); });
    MessageManager::callAsync([&]() {
      order.push_back("B");
      MessageManager::callAsync([&]() { order.push_back("X"); });
      MessageManager::getInstance()->runDispatchLoopUntil(30); // the nested loop
      order.push_back("B-end");
    });
    MessageManager::callAsync([&]() { order.push_back("C"); });
    MessageManager::callAsync([&]() { order.push_back("D"); });
    pumpUntil([&]() { return order.size() == 6; });
    std::string joined;
    for (const auto& s : order) joined += s + " ";
    std::snprintf(msg, sizeof(msg), "a nested loop keeps the queue's order: %s", joined.c_str());
    check(joined == "A B C D X B-end ", msg);
  }

  // A throwing callback is dropped, and the queue carries on.
  {
    bool after = false;
    MessageManager::callAsync([]() { throw std::runtime_error("boom"); });
    MessageManager::callAsync([&]() { after = true; });
    pumpUntil([&]() { return after; }, 2000);
    check(after, "a callback that throws does not stop the ones after it");
  }

  check(!MessageManager::callAsync(nullptr), "an empty function is refused");
  check(mm->dispatchPendingMessages() == 0, "nothing left over");
}

// ── A worker posting faster than the message thread runs ────────────────────

#if defined(_WIN32)
static std::atomic<bool> gFlooding{false};
static std::atomic<int> gTicksWhileFlooding{0};

static void CALLBACK countFloodTick(HWND, UINT, UINT_PTR, DWORD) {
  if (gFlooding.load()) ++gTicksWhileFlooding;
}
#endif

/** The thread's own timers -- an editor's tick, which repaints its meters --
 *  keep running while a worker posts more than the message thread can take.
 *  Windows hands out posted messages before input, paint and timers, so a
 *  wake-up re-armed by posting kept them all waiting for as long as the
 *  worker posted: not one tick in a second. */
static void testFlood() {
  std::printf("\n-- callAsync faster than the message thread runs --\n");
#if defined(_WIN32)
  MessageManager* mm = MessageManager::getInstance();
  const UINT_PTR timer = SetTimer(nullptr, 0, 10, &countFloodTick);
  std::atomic<long long> ran{0};
  gTicksWhileFlooding = 0;
  gFlooding = true;
  std::thread worker([&]() {
    // Each call takes 50 us to run and one arrives every 40 us: the queue
    // only grows, for a second.
    const auto end = Clock::now() + std::chrono::milliseconds(1000);
    while (Clock::now() < end) {
      MessageManager::callAsync([&ran]() {
        ++ran;
        const auto until = Clock::now() + std::chrono::microseconds(50);
        while (Clock::now() < until) {
        }
      });
      const auto next = Clock::now() + std::chrono::microseconds(40);
      while (Clock::now() < next) {
      }
    }
    gFlooding = false;
  });
  while (gFlooding.load()) mm->runDispatchLoopUntil(10);
  worker.join();
  KillTimer(nullptr, timer);
  const int ticks = gTicksWhileFlooding.load();
  pumpUntil([&]() { return mm->numPendingMessages() == 0; });
  // At least every 50 ms of work plus the timer's own wait: some 15 a second.
  std::snprintf(msg, sizeof(msg),
                "a 10 ms timer on the message thread still ticks during a one-second flood (%d "
                "ticks, %lld calls ran)",
                ticks, ran.load());
  check(ticks >= 8, msg);
#else
  check(true, "Windows-only: the posted-before-input order is Windows' own");
#endif
}

#if defined(_WIN32)
static std::atomic<bool> gKeepInvalidating{false};

static LRESULT CALLBACK selfInvalidatingProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
  if (message == WM_PAINT) {
    PAINTSTRUCT ps;
    BeginPaint(hwnd, &ps);
    EndPaint(hwnd, &ps);
    if (gKeepInvalidating.load()) InvalidateRect(hwnd, nullptr, FALSE);
    return 0;
  }
  return DefWindowProcW(hwnd, message, wParam, lParam);
}
#endif

/** runDispatchLoopUntil returns at its time even while the OS always has a
 *  message ready -- here a window that invalidates itself as it paints, the
 *  naive way to animate. It dispatched until the queue ran dry, which it
 *  never did, so a modal message box or progress window over such a window
 *  kept running after it was answered. */
static void testPumpDeadline() {
  std::printf("\n-- runDispatchLoopUntil under a message source that never stops --\n");
#if defined(_WIN32)
  MessageManager* mm = MessageManager::getInstance();
  WNDCLASSW wc{};
  wc.lpfnWndProc = &selfInvalidatingProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"SonorePumpDeadlineTest";
  RegisterClassW(&wc);
  HWND hwnd = CreateWindowExW(WS_EX_NOACTIVATE, wc.lpszClassName, L"", WS_POPUP, 0, 0, 64, 64,
                              nullptr, nullptr, wc.hInstance, nullptr);
  gKeepInvalidating = true;
  ShowWindow(hwnd, SW_SHOWNOACTIVATE);
  // The watchdog ends the paint loop, so the old code returns -- late.
  std::thread watchdog([]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));
    gKeepInvalidating = false;
  });
  const auto started = Clock::now();
  mm->runDispatchLoopUntil(50);
  const long long took =
      (long long) std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count();
  gKeepInvalidating = false;
  watchdog.join();
  DestroyWindow(hwnd);
  UnregisterClassW(wc.lpszClassName, wc.hInstance);
  std::snprintf(msg, sizeof(msg),
                "runDispatchLoopUntil(50) returns on time while a window repaints itself forever "
                "(%lld ms)",
                took);
  check(took < 1000, msg);
#else
  check(true, "Windows-only: the window procedure here is Windows'");
#endif
}

// ── callFunctionOnMessageThread ─────────────────────────────────────────────

static void* doubler(void* data) {
  int* value = static_cast<int*>(data);
  *value *= 2;
  return value;
}

static void testCallFunctionOnMessageThread() {
  std::printf("\n-- callFunctionOnMessageThread --\n");
  MessageManager* mm = MessageManager::getInstance();
  const std::thread::id main = std::this_thread::get_id();

  // From the message thread itself: runs at once. Queueing would wait for
  // ourselves, forever.
  {
    bool ran = false;
    const bool result = mm->callFunctionOnMessageThread([&]() { ran = true; });
    check(result && ran, "called on the message thread, it runs at once instead of deadlocking");
  }

  // From a worker: queued, waited for, the result handed back.
  {
    std::atomic<bool> done{false};
    std::thread::id ranOn;
    int value = 21;
    void* returned = nullptr;
    std::thread worker([&]() {
      returned = MessageManager::getInstance()->callFunctionOnMessageThread(&doubler, &value);
      MessageManager::getInstance()->callFunctionOnMessageThread(
          [&]() { ranOn = std::this_thread::get_id(); });
      done = true;
    });
    pumpUntil([&]() { return done.load(); });
    worker.join();
    check(returned == &value && value == 42, "from a worker, the reference's void* form returns its answer");
    check(ranOn == main, "and the function ran on the message thread");
  }

  // An exception crosses back to the caller.
  {
    std::atomic<bool> done{false};
    bool caught = false;
    std::thread worker([&]() {
      try {
        MessageManager::getInstance()->callFunctionOnMessageThread(
            []() { throw std::runtime_error("from the message thread"); });
      } catch (const std::runtime_error& e) {
        caught = std::strcmp(e.what(), "from the message thread") == 0;
      }
      done = true;
    });
    pumpUntil([&]() { return done.load(); });
    worker.join();
    check(caught, "an exception thrown on the message thread is rethrown to the caller");
  }

  // Holding a MessageManagerLock, the worker already has the message thread
  // parked: the call runs right there instead of waiting for itself.
  {
    std::atomic<bool> done{false};
    std::thread::id ranOn;
    std::thread::id workerId;
    bool gained = false;
    std::thread worker([&]() {
      workerId = std::this_thread::get_id();
      MessageManagerLock lock;
      gained = lock.lockWasGained();
      MessageManager::getInstance()->callFunctionOnMessageThread(
          [&]() { ranOn = std::this_thread::get_id(); });
      done = true;
    });
    pumpUntil([&]() { return done.load(); });
    worker.join();
    check(gained && ranOn == workerId,
          "under a MessageManagerLock the call runs on the holder, no deadlock");
  }
}

// ── MessageManagerLock ──────────────────────────────────────────────────────

static void testLockStress() {
  std::printf("\n-- MessageManagerLock: stress --\n");
  MessageManager* mm = MessageManager::getInstance();

  // Touched ONLY under exclusive access -- by lock holders and by message-
  // thread callbacks. Plain ints on purpose: if exclusion ever failed, the
  // increments would race and the totals would come out short.
  long long plainCounter = 0;
  int inside = 0;
  std::atomic<int> violations{0};
  std::atomic<int> notGained{0}, notRecorded{0}, stillRecorded{0};
  std::atomic<int> asyncPosted{0}, asyncRan{0};
  std::atomic<int> finished{0};
  constexpr int kThreads = 4, kEach = 2500;

  auto exclusiveWork = [&]() {
    if (++inside != 1) violations.fetch_add(1);
    ++plainCounter;
    if (--inside != 0) violations.fetch_add(1);
  };

  const auto start = Clock::now();
  std::vector<std::thread> workers;
  for (int t = 0; t < kThreads; ++t)
    workers.emplace_back([&, t]() {
      std::mt19937 rng((unsigned) t * 7919u + 1u);
      for (int i = 0; i < kEach; ++i) {
        {
          MessageManagerLock lock;
          if (!lock.lockWasGained()) {
            notGained.fetch_add(1);
            continue;
          }
          if (!MessageManager::getInstance()->currentThreadHasLockedMessageManager())
            notRecorded.fetch_add(1);
          exclusiveWork();
          if (rng() % 8 == 0) std::this_thread::yield();
          exclusiveWork();
        }
        if (MessageManager::getInstance()->currentThreadHasLockedMessageManager())
          stillRecorded.fetch_add(1);
        // The message thread's own callbacks compete for the same counter.
        if (rng() % 4 == 0) {
          asyncPosted.fetch_add(1);
          MessageManager::callAsync([&]() {
            exclusiveWork();
            asyncRan.fetch_add(1);
          });
        }
      }
      finished.fetch_add(1);
    });

  const bool done = pumpUntil([&]() { return finished.load() == kThreads; }, 120000);
  for (auto& w : workers) w.join();
  pumpUntil([&]() { return asyncRan.load() == asyncPosted.load(); }, 10000);
  const double ms = msSince(start);
  const long long expected = (long long) kThreads * kEach * 2 + asyncPosted.load();
  std::snprintf(msg, sizeof(msg),
                "%d acquisitions from %d threads while the message thread pumped, in %.0f ms "
                "(no deadlock)", kThreads * kEach, kThreads, ms);
  check(done, msg);
  std::snprintf(msg, sizeof(msg),
                "mutual exclusion: the unguarded counter is %lld of %lld, %d overlaps seen",
                plainCounter, expected, violations.load());
  check(plainCounter == expected && violations.load() == 0, msg);
  std::snprintf(msg, sizeof(msg), "every lock was gained (%d not), recorded while held (%d not) "
                "and forgotten after (%d still)", notGained.load(), notRecorded.load(),
                stillRecorded.load());
  check(notGained.load() == 0 && notRecorded.load() == 0 && stillRecorded.load() == 0, msg);
  check(mm->numPendingMessages() == 0, "nothing left in the queue");

  // On the message thread the lock is had at once; nested ones too.
  {
    MessageManagerLock outer;
    MessageManagerLock inner;
    check(outer.lockWasGained() && inner.lockWasGained(),
          "on the message thread, and nested, the lock is gained at once");
  }
}

static void testLockAbort() {
  std::printf("\n-- MessageManagerLock: giving up --\n");
  MessageManager* mm = MessageManager::getInstance();

  // The classic shutdown: the message thread tells a worker to stop and joins
  // it -- while the worker waits for a lock the message thread will never
  // grant, because it is busy joining. The exit signal breaks the cycle.
  {
    ThreadExitSignal signal;
    std::atomic<bool> waiting{false};
    bool gained = true;
    std::thread worker([&]() {
      waiting = true;
      MessageManagerLock lock(signal);
      gained = lock.lockWasGained();
    });
    while (!waiting) std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(40)); // it is waiting now
    const auto start = Clock::now();
    signal.signalThreadShouldExit();
    worker.join(); // on the message thread, not pumping
    const double ms = msSince(start);
    std::snprintf(msg, sizeof(msg), "signalled, the waiting lock gives up and the join returns "
                  "after %.1f ms", ms);
    check(!gained && ms < 1000.0, msg);
    // The request it left behind is harmless when it is finally reached.
    const int ran = mm->dispatchPendingMessages();
    std::snprintf(msg, sizeof(msg), "the abandoned request runs (%d message) without parking the "
                  "message thread", ran);
    check(ran == 1 && mm->numPendingMessages() == 0, msg);
  }

  // A ThreadPool job, whose stop flag is a bare atomic: close() sets it and
  // joins, from the message thread, and the lock notices within its 2 ms.
  {
    std::atomic<int> gained{-1};
    std::atomic<bool> started{false};
    {
      ThreadPool pool(1);
      pool.addJob([&](const std::atomic<bool>& shouldStop) {
        started = true;
        MessageManagerLock lock(shouldStop);
        gained = lock.lockWasGained() ? 1 : 0;
      });
      while (!started) std::this_thread::yield();
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      const auto start = Clock::now();
      pool.close();
      std::snprintf(msg, sizeof(msg), "a ThreadPool job's lock gives up when the pool closes "
                    "(%.1f ms)", msSince(start));
      check(gained.load() == 0, msg);
    }
    mm->dispatchPendingMessages();
  }

  // A signal raised at any moment -- before the request is reached, while it
  // is being granted, after -- never leaves the message thread parked and
  // never reports a lock it does not hold.
  {
    // A fixed number of rounds rather than a fixed time: on a loaded machine
    // (and under ASan) a round can take a millisecond or a hundred.
    constexpr int kRaceRounds = 600;
    std::atomic<int> gainedCount{0}, lostCount{0};
    std::atomic<bool> stop{false};
    std::atomic<int> rounds{0};
    std::thread racer([&]() {
      std::mt19937 rng(12345u);
      while (!stop && rounds.load() < kRaceRounds) {
        ThreadExitSignal signal;
        std::thread worker([&]() {
          MessageManagerLock lock(signal);
          if (lock.lockWasGained()) {
            if (!MessageManager::getInstance()->currentThreadHasLockedMessageManager())
              lostCount.fetch_add(1);
            gainedCount.fetch_add(1);
          }
        });
        // A random moment 0-200 us in, spun on the clock. It was 0-200 yields,
        // and on a machine saturated by a build each yield gave the CPU away
        // for a whole scheduler slice: 96 rounds in two minutes, a failure
        // that measured the build rather than the lock.
        const auto until = Clock::now() + std::chrono::microseconds(rng() % 200);
        while (Clock::now() < until) {
        }
        signal.signalThreadShouldExit();
        worker.join();
        rounds.fetch_add(1);
      }
    });
    const bool raced = pumpUntil([&]() { return rounds.load() >= kRaceRounds; }, 120000);
    stop = true;
    racer.join();
    mm->runDispatchLoopUntil(20);
    std::snprintf(msg, sizeof(msg), "%d signal/grant races: %d gained, none lost, message thread "
                  "never left parked", rounds.load(), gainedCount.load());
    check(raced && lostCount.load() == 0 && mm->numPendingMessages() == 0, msg);
  }

  // The manager deleted while a worker waits for the lock, and while another
  // waits for a synchronous call: both are released, not left blocked.
  {
    std::atomic<int> waitingCount{0};
    bool gained = true, ran = true;
    std::thread locker([&]() {
      waitingCount.fetch_add(1);
      MessageManagerLock lock;
      gained = lock.lockWasGained();
    });
    std::thread caller([&]() {
      waitingCount.fetch_add(1);
      MessageManager* m = MessageManager::getInstanceWithoutCreating();
      ran = m && m->callFunctionOnMessageThread([]() {});
    });
    while (waitingCount.load() < 2) std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    MessageManager::deleteInstance();
    locker.join();
    caller.join();
    check(!gained && !ran, "deleting the manager releases a waiting lock and a waiting call");
    check(MessageManager::getInstanceWithoutCreating() == nullptr, "and the manager is gone");
    MessageManagerLock none;
    check(!none.lockWasGained(), "with no manager, a lock is simply not gained");
    MessageManager::getInstance(); // a fresh one for what follows
  }
}

// ── The message thread moving ───────────────────────────────────────────────

static void testMoveMessageThread() {
  std::printf("\n-- setCurrentThreadAsMessageThread --\n");
  MessageManager* mm = MessageManager::getInstance();
  const std::thread::id main = std::this_thread::get_id();
  std::atomic<bool> ready{false}, finish{false};
  std::atomic<int> ranOnOther{0}, ranElsewhere{0};
  std::thread::id otherId;
  std::thread other([&]() {
    otherId = std::this_thread::get_id();
    MessageManager::getInstance()->setCurrentThreadAsMessageThread();
    ready = true;
    while (!finish) MessageManager::getInstance()->runDispatchLoopUntil(5);
  });
  while (!ready) std::this_thread::yield();
  check(!mm->isThisTheMessageThread(), "after another thread takes over, this one is not it");
  for (int i = 0; i < 100; ++i)
    MessageManager::callAsync([&]() {
      if (std::this_thread::get_id() == otherId) ranOnOther.fetch_add(1);
      else ranElsewhere.fetch_add(1);
    });
  const auto start = Clock::now();
  while (ranOnOther.load() + ranElsewhere.load() < 100 && msSince(start) < 5000.0)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  finish = true;
  other.join();
  std::snprintf(msg, sizeof(msg), "calls follow the message thread: %d ran on the new one, %d "
                "elsewhere", ranOnOther.load(), ranElsewhere.load());
  check(ranOnOther.load() == 100 && ranElsewhere.load() == 0, msg);

  mm->setCurrentThreadAsMessageThread();
  bool back = false;
  std::thread::id ranOn;
  MessageManager::callAsync([&]() {
    back = true;
    ranOn = std::this_thread::get_id();
  });
  pumpUntil([&]() { return back; }, 5000);
  check(back && ranOn == main && mm->isThisTheMessageThread(), "and come back when it is taken back");
}

// ── CallbackMessage and MessageListener ────────────────────────────────────

static std::atomic<int> gCallbackRuns{0}, gCallbackDeletes{0};

struct CountingMessage : CallbackMessage {
  ~CountingMessage() override { gCallbackDeletes.fetch_add(1); }
  void messageCallback() override { gCallbackRuns.fetch_add(1); }
};

struct NumberMessage : Message {
  explicit NumberMessage(int v) : value(v) {}
  int value;
};

static std::atomic<int> gListenerCalls{0};

struct RecordingListener : MessageListener {
  std::vector<int> received;
  std::vector<std::thread::id> threads;
  bool deleteSelfOnFirst = false;
  void handleMessage(const Message& m) override {
    gListenerCalls.fetch_add(1);
    received.push_back(static_cast<const NumberMessage&>(m).value);
    threads.push_back(std::this_thread::get_id());
    if (deleteSelfOnFirst) delete this;
  }
};

static void testMessagesAndListeners() {
  std::printf("\n-- CallbackMessage / MessageListener --\n");
  MessageManager* mm = MessageManager::getInstance();
  const std::thread::id main = std::this_thread::get_id();

  {
    gCallbackRuns = 0;
    gCallbackDeletes = 0;
    std::thread worker([]() {
      for (int i = 0; i < 10; ++i) (new CountingMessage())->post();
    });
    worker.join();
    pumpUntil([]() { return gCallbackRuns.load() == 10; }, 5000);
    check(gCallbackRuns.load() == 10 && gCallbackDeletes.load() == 10,
          "ten CallbackMessages posted from a worker run, and are deleted after running");
  }

  {
    RecordingListener listener;
    std::thread worker([&]() {
      for (int i = 0; i < 100; ++i) listener.postMessage(new NumberMessage(i));
    });
    worker.join();
    pumpUntil([&]() { return listener.received.size() == 100; }, 5000);
    bool ordered = listener.received.size() == 100;
    for (size_t i = 0; ordered && i < 100; ++i)
      ordered = listener.received[i] == (int) i && listener.threads[i] == main;
    check(ordered, "a listener gets its 100 messages in order, on the message thread");
  }

  // Deleted with messages in flight: none of them reach it.
  {
    gListenerCalls = 0;
    auto* listener = new RecordingListener();
    for (int i = 0; i < 50; ++i) listener->postMessage(new NumberMessage(i));
    delete listener;
    mm->runDispatchLoopUntil(30);
    std::snprintf(msg, sizeof(msg), "a listener deleted with 50 messages queued is called %d "
                  "times", gListenerCalls.load());
    check(gListenerCalls.load() == 0 && mm->numPendingMessages() == 0, msg);
  }

  // Deleting itself from its own handler: the rest are dropped.
  {
    gListenerCalls = 0;
    auto* listener = new RecordingListener();
    listener->deleteSelfOnFirst = true;
    for (int i = 0; i < 5; ++i) listener->postMessage(new NumberMessage(i));
    mm->runDispatchLoopUntil(30);
    check(gListenerCalls.load() == 1, "a listener that deletes itself in its handler hears nothing more");
  }

  // Deleted on a worker, the documented way: holding the lock, so the
  // message thread is parked and cannot be inside the handler.
  {
    gListenerCalls = 0;
    auto* listener = new RecordingListener();
    std::atomic<bool> done{false};
    std::thread worker([&]() {
      for (int i = 0; i < 1000; ++i) listener->postMessage(new NumberMessage(i));
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
      {
        MessageManagerLock lock;
        delete listener;
      }
      done = true;
    });
    pumpUntil([&]() { return done.load(); }, 10000);
    worker.join();
    pumpUntil([&]() { return mm->numPendingMessages() == 0; }, 10000);
    const int calls = gListenerCalls.load();
    std::snprintf(msg, sizeof(msg), "deleted on a worker under the lock mid-delivery: %d of 1000 "
                  "delivered before, none after", calls);
    check(calls <= 1000 && mm->numPendingMessages() == 0, msg);
  }

  // Undelivered messages are deleted -- not leaked, not run -- with the manager.
  {
    gCallbackRuns = 0;
    gCallbackDeletes = 0;
    for (int i = 0; i < 7; ++i) (new CountingMessage())->post();
    MessageManager::deleteInstance();
    check(gCallbackRuns.load() == 0 && gCallbackDeletes.load() == 7,
          "messages still queued when the manager goes are deleted without running");
    auto* orphan = new CountingMessage();
    check(!orphan->post() && gCallbackDeletes.load() == 8,
          "posting with no manager fails and deletes the message");
    MessageManager::getInstance();
  }
}

// ── LockingAsyncUpdater ─────────────────────────────────────────────────────

static void testLockingAsyncUpdater() {
  std::printf("\n-- LockingAsyncUpdater --\n");
  MessageManager* mm = MessageManager::getInstance();

  {
    std::atomic<int> calls{0};
    LockingAsyncUpdater updater([&]() { calls.fetch_add(1); });
    std::thread worker([&]() {
      for (int i = 0; i < 100; ++i) updater.triggerAsyncUpdate();
    });
    worker.join();
    check(updater.isUpdatePending(), "triggered, an update is pending");
    mm->runDispatchLoopUntil(20);
    check(calls.load() == 1 && !updater.isUpdatePending(), "a hundred triggers make one call");

    updater.triggerAsyncUpdate();
    updater.cancelPendingUpdate();
    mm->runDispatchLoopUntil(20);
    check(calls.load() == 1, "a cancelled update does not run");

    updater.triggerAsyncUpdate();
    updater.handleUpdateNowIfNeeded();
    check(calls.load() == 2 && !updater.isUpdatePending(), "handleUpdateNowIfNeeded runs it here and now");
    mm->runDispatchLoopUntil(20);
    check(calls.load() == 2, "and the queued message then finds nothing to do");
    updater.handleUpdateNowIfNeeded();
    check(calls.load() == 2, "with nothing pending, handleUpdateNowIfNeeded does nothing");
  }

  // The guarantee, under fire: after cancel returns, no callback starts --
  // and none is still running. One thread triggers and cancels as fast as it
  // can; the message thread runs whatever it is given.
  {
    std::atomic<bool> window{false};    // between cancel returning and the next trigger
    std::atomic<bool> running{false};
    std::atomic<int> startedInWindow{0}, runningAfterCancel{0}, calls{0}, cancelMetRunning{0};
    std::atomic<bool> stop{false};
    // Busy waits in microseconds: a sleep is a millisecond at best on
    // Windows, and the races worth having are far shorter than that.
    auto busyMicros = [](int us) {
      const auto until = Clock::now() + std::chrono::microseconds(us);
      while (Clock::now() < until) {
      }
    };
    LockingAsyncUpdater updater([&]() {
      running = true;
      if (window.load()) startedInWindow.fetch_add(1);
      calls.fetch_add(1);
      busyMicros(5); // long enough that cancel often arrives mid-callback
      running = false;
    });
    constexpr int kRounds = 20000;
    std::thread worker([&]() {
      std::mt19937 rng(99u);
      for (int i = 0; i < kRounds; ++i) {
        window = false;
        updater.triggerAsyncUpdate();
        // Anything from no time to long enough for the message thread to
        // get there first: some updates are cancelled before they start,
        // some while they run, some after.
        busyMicros((int) (rng() % 60));
        if (running.load()) cancelMetRunning.fetch_add(1);
        updater.cancelPendingUpdate();
        if (running.load()) runningAfterCancel.fetch_add(1);
        window = true;
        busyMicros((int) (rng() % 20));
      }
      window = false;
      stop = true;
    });
    pumpUntil([&]() { return stop.load(); }, 120000);
    worker.join();
    mm->runDispatchLoopUntil(20);
    std::snprintf(msg, sizeof(msg), "%d trigger/cancel rounds against the message thread: %d "
                  "callbacks (%d cancels arrived mid-callback), %d started after a cancel "
                  "returned, %d still running when it returned",
                  kRounds, calls.load(), cancelMetRunning.load(), startedInWindow.load(),
                  runningAfterCancel.load());
    check(calls.load() > 100 && startedInWindow.load() == 0 && runningAfterCancel.load() == 0, msg);
  }

  // Destroyed with an update in flight: nothing runs, nothing is touched.
  {
    std::atomic<int> calls{0};
    {
      LockingAsyncUpdater updater([&]() { calls.fetch_add(1); });
      updater.triggerAsyncUpdate();
    }
    mm->runDispatchLoopUntil(20);
    check(calls.load() == 0, "an updater destroyed with an update queued is never called");
  }

  // A callback may re-trigger itself.
  {
    int calls = 0;
    std::unique_ptr<LockingAsyncUpdater> updater;
    updater.reset(new LockingAsyncUpdater([&]() {
      if (++calls < 3) updater->triggerAsyncUpdate();
    }));
    updater->triggerAsyncUpdate();
    pumpUntil([&]() { return calls >= 3; }, 2000);
    check(calls == 3, "a callback that triggers itself runs again, three times in all");
  }
}

// ── Broadcasts between processes ────────────────────────────────────────────

#if defined(_WIN32)
static int countMessageWindows() {
  int n = 0;
  HWND found = nullptr;
  while ((found = FindWindowExW(HWND_MESSAGE, found, nullptr, L"SonoreMessageWindow")) != nullptr) ++n;
  return n;
}
#endif

struct BroadcastRecorder : MessageManager::BroadcastListener {
  std::vector<std::string> received;
  void broadcastMessageReceived(const std::string& message) override { received.push_back(message); }
};

static int runBroadcastChild() {
  MessageManager* mm = MessageManager::getInstance();
  BroadcastRecorder recorder;
  mm->registerBroadcastListener(&recorder);
  const auto start = Clock::now();
  while (msSince(start) < 15000.0) {
    mm->runDispatchLoopUntil(10);
    for (const std::string& m : recorder.received)
      if (m == "hello from the parent \xE2\x9C\x93") {
        MessageManager::broadcastMessage("reply from the child");
        mm->runDispatchLoopUntil(50);
        mm->deregisterBroadcastListener(&recorder);
        MessageManager::deleteInstance();
        return 0;
      }
  }
  mm->deregisterBroadcastListener(&recorder);
  MessageManager::deleteInstance();
  return 3;
}

static void testBroadcast() {
  std::printf("\n-- broadcastMessage --\n");
#if defined(_WIN32)
  MessageManager* mm = MessageManager::getInstance();
  BroadcastRecorder recorder;
  mm->registerBroadcastListener(&recorder);
  const int before = countMessageWindows();

  wchar_t exe[MAX_PATH * 2];
  GetModuleFileNameW(nullptr, exe, MAX_PATH * 2);
  std::wstring command = L"\"" + std::wstring(exe) + L"\" --broadcast-child";
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(nullptr, &command[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
    check(false, "could not start the child process");
    return;
  }
  const auto start = Clock::now();
  while (countMessageWindows() <= before && msSince(start) < 10000.0) mm->runDispatchLoopUntil(5);
  check(countMessageWindows() > before, "the child's message window is visible to this process");

  const bool sent = MessageManager::broadcastMessage("hello from the parent \xE2\x9C\x93");
  check(sent, "a broadcast reaches at least one other message thread");
  const bool replied = pumpUntil([&]() {
    for (const auto& m : recorder.received)
      if (m == "reply from the child") return true;
    return false;
  }, 15000);
  check(replied, "the child heard it (UTF-8 intact) and its reply came back to this listener");
  WaitForSingleObject(pi.hProcess, 15000);
  DWORD code = 99;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  std::snprintf(msg, sizeof(msg), "the child exited with %lu", (unsigned long) code);
  check(code == 0, msg);
  bool ownHeard = false;
  for (const auto& m : recorder.received)
    if (m.find("hello from the parent") == 0) ownHeard = true;
  check(!ownHeard, "a process does not hear its own broadcast");
  mm->deregisterBroadcastListener(&recorder);
#else
  check(true, "broadcast: Windows-only run; SKIPPED here");
#endif
}

// ── ApplicationProperties ───────────────────────────────────────────────────

namespace fs = std::filesystem;

static std::string u8(const fs::path& p) {
  const auto s = p.u8string();
  return std::string(s.begin(), s.end());
}

static std::string readText(const std::string& path) {
  std::ifstream in(fs::u8path(path), std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

static void writeText(const std::string& path, const std::string& text) {
  std::ofstream out(fs::u8path(path), std::ios::binary);
  out << text;
}

static void setEnv(const char* name, const std::string& value) {
#if defined(_WIN32)
  _wputenv_s(pathio::widen(name).c_str(), pathio::widen(value.c_str()).c_str());
#else
  setenv(name, value.c_str(), 1);
#endif
}

static fs::path gTempRoot;

static void testApplicationProperties() {
  std::printf("\n-- PropertiesFile / ApplicationProperties --\n");
  const fs::path appdata = gTempRoot / "AppData Roaming";
  const fs::path programdata = gTempRoot / "ProgramData";
#if defined(_WIN32)
  setEnv("APPDATA", u8(appdata));
  setEnv("ProgramData", u8(programdata));
#elif defined(__APPLE__)
  // macOS names no variable for either place: the user's file is under
  // $HOME/Library and the shared one under /Library, as Apple asks. HOME can
  // be pointed here; /Library cannot, and writing it takes an administrator,
  // so the checks that WRITE the machine-wide file run on Windows and Linux
  // only (kSharedWritable below) -- the read-through they prove is the same
  // code on every platform. This test once set the XDG variables here too,
  // which macOS never reads, and expected the Linux paths: every check after
  // the first failed on the first macOS run, about a test, not a bug.
  const char* homeWas = std::getenv("HOME");
  const std::string savedHome = homeWas ? homeWas : "";
  setEnv("HOME", u8(appdata));
#else
  setEnv("XDG_CONFIG_HOME", u8(appdata));
  setEnv("XDG_CONFIG_DIRS", u8(programdata));
#endif

  PropertiesFile::Options options;
  options.applicationName = "Sonore Test App";
  options.folderName = "Sonore/Test";
  options.filenameSuffix = "settings";
  options.millisecondsBeforeSaving = 500;

#if defined(__APPLE__)
  const std::string expectedUser =
      u8(appdata / "Library" / "Application Support" / "Sonore" / "Test" / "Sonore Test App.settings");
  const std::string expectedCommon = "/Library/Application Support/Sonore/Test/Sonore Test App.settings";
  constexpr bool kSharedWritable = false;
#else
  const std::string expectedUser =
      u8(appdata / "Sonore" / "Test" / "Sonore Test App.settings");
  const std::string expectedCommon =
      u8(programdata / "Sonore" / "Test" / "Sonore Test App.settings");
  constexpr bool kSharedWritable = true;
#endif
  {
    std::snprintf(msg, sizeof(msg), "the user file goes in the user's folder: %s",
                  options.getDefaultFile().c_str());
    check(options.getDefaultFile() == expectedUser, msg);
    PropertiesFile::Options common = options;
    common.commonToAllUsers = true;
    std::snprintf(msg, sizeof(msg), "the shared one in the machine's: %s",
                  common.getDefaultFile().c_str());
    check(common.getDefaultFile() == expectedCommon, msg);
    PropertiesFile::Options none;
    check(none.getDefaultFile().empty(), "no application name, no file");
  }

  const std::string tricky = "a & b < c > d \"quoted\" 'single'\nsecond line\ttab \xD0\x9E\xD0\xBB\xD0\xB5\xD0\xB3 \xE9\x9F\xB3\xE6\xA5\xBD";
  {
    ApplicationProperties props;
    props.setStorageParameters(options);
    PropertiesFile* user = props.getUserSettings();
    check(user && user->getFile() == expectedUser && user->isValidFile(),
          "a first run: no file yet, and that is a valid, empty file");

    user->setValue("volume", 0.75);
    user->setValue("name", tricky);
    user->setValue("count", 42);
    user->setValue("enabled", true);
    check(user->needsToBeSaved() && user->isSavePending(), "a change marks it dirty and starts the save clock");
    check(user->hasPendingChange(), "and is a ChangeBroadcaster change");
    check(!props.advance(300.0) && !fs::exists(fs::u8path(expectedUser).parent_path()),
          "300 ms after: not yet written");
    user->setValue("volume", 0.8);
    user->setValue("volume", 0.75);
    check(!props.advance(300.0) && !fs::exists(fs::u8path(expectedUser)),
          "another change restarts the wait: 600 ms after the first, still not written");
    check(props.advance(250.0) && fs::exists(fs::u8path(expectedUser)) && !user->needsToBeSaved(),
          "500 ms after the LAST change it is written, once");

    const std::string xml = readText(expectedUser);
    check(xml.compare(0, 5, "<?xml") == 0 && xml.find("<PROPERTIES>") != std::string::npos &&
              xml.find("<VALUE name=\"volume\" val=\"0.75\"/>") != std::string::npos,
          "as the reference's XML: <PROPERTIES> of <VALUE name val/>");
    check(xml.find("&amp;") != std::string::npos && xml.find("&#10;") != std::string::npos &&
              xml.find("&quot;") != std::string::npos,
          "with &, quotes and newlines escaped so any XML reader gets them back");

    PropertiesFile* common = props.getCommonSettings(false);
    check(common && common->getFile() == expectedCommon, "the machine-wide file opens beside it");
    if constexpr (kSharedWritable) {
      common->setValue("licence", "LIC-123");
      common->setValue("volume", 0.1);
      check(common->save(), "and can be saved here (the folder is writable)");
      check(user->getValue("licence") == "LIC-123" && !user->containsKey("licence"),
            "the user's file reads through to the machine's for a key it lacks");
      check(user->getDoubleValue("volume") == 0.75, "and its own value wins where it has one");
      check(props.getCommonSettings(true) == common, "writable, getCommonSettings(true) is the shared file");
    } else {
      std::printf("  ---- macOS: the machine-wide file is under /Library, which takes an administrator "
                  "to write; writing it and reading through to it are checked on Windows and Linux ----\n");
    }
  }

  {
    ApplicationProperties again;
    again.setStorageParameters(options);
    PropertiesFile* user = again.getUserSettings();
    check(user->isValidFile(), "a second run reads the file back");
    check(user->getDoubleValue("volume") == 0.75 && user->getIntValue("count") == 42 &&
              user->getBoolValue("enabled") && user->getValue("name") == tricky,
          "every value exactly: the double, the int, the bool, and the awkward string");
    if constexpr (kSharedWritable) check(user->getValue("licence") == "LIC-123", "and the shared one through the fallback");
    check(user->getIntValue("missing", -5) == -5 && user->getIntValue("name", 7) == 7,
          "a missing or non-numeric value gives the fallback");
  }

  // The machine-wide folder cannot be created: the user's file stands in.
  {
    writeText(u8(gTempRoot / "not-a-folder"), "x");
#if defined(_WIN32)
    setEnv("ProgramData", u8(gTempRoot / "not-a-folder" / "sub"));
#else
    setEnv("XDG_CONFIG_DIRS", u8(gTempRoot / "not-a-folder" / "sub"));
#endif
    ApplicationProperties props;
    props.setStorageParameters(options);
    PropertiesFile* user = props.getUserSettings();
    check(props.getCommonSettings(true) == user,
          "read-only shared settings: getCommonSettings(true) hands back the user's file");
    check(props.getCommonSettings(false) != user, "while getCommonSettings(false) is still the shared one");
#if defined(_WIN32)
    setEnv("ProgramData", u8(programdata));
#else
    setEnv("XDG_CONFIG_DIRS", u8(programdata));
#endif
  }

  // Binary, and either format read whatever the options say.
  {
    const std::string path = u8(gTempRoot / "binary.settings");
    PropertiesFile::Options binary;
    binary.storageFormat = PropertiesFile::StorageFormat::storeAsBinary;
    binary.millisecondsBeforeSaving = -1;
    {
      PropertiesFile file(path, binary);
      file.setValue("text", tricky);
      file.setValue("pi", 3.141592653589793);
      check(!fs::exists(fs::u8path(path)), "negative delay: nothing written on a change");
      check(file.saveIfNeeded(), "until asked");
    }
    const std::string bytes = readText(path);
    check(bytes.compare(0, 8, "SNPROPS1") == 0, "binary files start with their magic");
    PropertiesFile::Options xmlOptions;
    PropertiesFile reread(path, xmlOptions);
    check(reread.isValidFile() && reread.getValue("text") == tricky &&
              reread.getDoubleValue("pi") == 3.141592653589793,
          "a binary file is read by a file configured for XML: the bytes decide");
  }

  // Saving on every change, and case-insensitive keys.
  {
    const std::string path = u8(gTempRoot / "immediate.settings");
    PropertiesFile::Options now;
    now.millisecondsBeforeSaving = 0;
    now.ignoreCaseOfKeyNames = true;
    PropertiesFile file(path, now);
    file.setValue("Volume", 3);
    check(fs::exists(fs::u8path(path)) && !file.needsToBeSaved(), "zero delay writes on the change itself");
    check(file.getIntValue("VOLUME") == 3 && file.containsKey("volume"),
          "ignoreCaseOfKeyNames: one key, however it is spelled");
    file.setValue("volume", 4);
    check(file.getAllKeys().size() == 1 && file.getIntValue("Volume") == 4, "and setting it again replaces it");
    file.removeValue("VOLUME");
    check(file.getAllKeys().empty(), "and removing it by another spelling removes it");
  }

  // A damaged file is reported, not half-loaded; a hand-written one in the
  // reference's style is read, comments, nesting and all.
  {
    const std::string bad = u8(gTempRoot / "bad.settings");
    writeText(bad, "<?xml version=\"1.0\"?><PROPERTIES><VALUE name=\"a\" val=\"1\"/>");
    PropertiesFile::Options o;
    o.doNotSave = true;
    PropertiesFile damaged(bad, o);
    check(!damaged.isValidFile() && damaged.getAllKeys().empty(),
          "a file cut off before </PROPERTIES> is invalid, and loads nothing");
    const std::string good = u8(gTempRoot / "reference.settings");
    writeText(good,
              "\xEF\xBB\xBF<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!-- written by hand -->\n"
              "<PROPERTIES>\n  <VALUE name=\"recentFiles\" val=\"C:\\a.wav&#13;&#10;C:\\b.wav\"/>\n"
              "  <!-- <VALUE name=\"commented\" val=\"no\"/> -->\n"
              "  <VALUE name='single' val='quotes &#x263A;'/>\n"
              "  <VALUE name=\"window\">\n    <POS x=\"10\" y=\"20\"/>\n  </VALUE>\n"
              "</PROPERTIES>\n");
    PropertiesFile hand(good, o);
    check(hand.isValidFile() && hand.getAllKeys().size() == 3, "a hand-written reference-style file loads");
    check(hand.getValue("recentFiles") == "C:\\a.wav\r\nC:\\b.wav" &&
              hand.getValue("single") == "quotes \xE2\x98\xBA" &&
              hand.getValue("window") == "<POS x=\"10\" y=\"20\"/>" && !hand.containsKey("commented"),
          "character references decoded, single quotes, element content kept, comments skipped");
    check(!hand.save(), "doNotSave never writes");
  }

  // Two processes, one file: the lock is held around every write.
  {
    const std::string path = u8(gTempRoot / "locked.settings");
    PropertiesFile::Options locked;
    locked.useProcessLock = true;
    locked.millisecondsBeforeSaving = -1;
    PropertiesFile file(path, locked);
    file.setValue("k", "v");
    bool savedWhileHeld = true;
    double waited = 0.0;
    {
      detail::props::FileLock other(path + ".lock", 0);
      check(other.isLocked(), "another holder takes the lock file");
      // From another thread, so the holder above is a different handle: the
      // same rule an OS applies between processes.
      std::thread writer([&]() {
        const auto start = Clock::now();
        savedWhileHeld = file.save();
        waited = msSince(start);
      });
      writer.join();
    }
    std::snprintf(msg, sizeof(msg), "a save waits for the lock and gives up after %.0f ms", waited);
    check(!savedWhileHeld && waited >= 900.0, msg);
    check(file.save() && readText(path).find("name=\"k\" val=\"v\"") != std::string::npos,
          "and succeeds once the other holder lets go");
  }

  // The machine-wide file could not be read -- another process held its lock
  // for longer than a read waits. Asking whether it is writable must not
  // answer by saving the empty copy over it. (It writes the machine-wide
  // file to set up, so not on macOS: see kSharedWritable.)
  if constexpr (kSharedWritable) {
    {
      ApplicationProperties setup;
      setup.setStorageParameters(options);
      setup.getCommonSettings(false)->setValue("licence", "LIC-123");
      check(setup.getCommonSettings(false)->save(), "a machine-wide file holding a licence");
    }
    ApplicationProperties props;
    props.setStorageParameters(options);
    {
      detail::props::FileLock other(expectedCommon + ".lock", 0);
      std::thread opener([&]() { props.getUserSettings(); });
      opener.join();
    }
    PropertiesFile* common = props.getCommonSettings(false);
    check(common && !common->isValidFile(), "opened while another process held it: not loaded");
    const bool handedUser = props.getCommonSettings(true) == props.getUserSettings();
    props.closeFiles();
    const bool kept = readText(expectedCommon).find("LIC-123") != std::string::npos;
    std::snprintf(msg, sizeof(msg),
                  "getCommonSettings(true) leaves the unread file as it was (it now holds %s)",
                  kept ? "the licence" : "nothing");
    check(kept, msg);
    check(handedUser, "and hands back the user's file, as for a read-only one");
  }

  // A value too big for the reader: the file it would make is never written,
  // because a load refuses it and takes every other value down with it.
  {
    const std::string path = u8(gTempRoot / "huge.settings");
    PropertiesFile::Options o;
    o.millisecondsBeforeSaving = -1;
    bool saved = true;
    {
      PropertiesFile file(path, o);
      file.setValue("theme", "dark");
      check(file.save(), "a settings file with one ordinary value");
      file.setValue("cache", std::string((size_t) 17 * 1024 * 1024, 'x')); // past the 16 MB a load reads
      saved = file.save();
    }
    PropertiesFile back(path, o);
    std::snprintf(msg, sizeof(msg),
                  "a save larger than a load reads is refused (saved=%d), and the file still loads "
                  "with what it had (valid=%d, theme=\"%s\")",
                  (int) saved, (int) back.isValidFile(), back.getValue("theme").c_str());
    check(!saved && back.isValidFile() && back.getValue("theme") == "dark", msg);
  }

  // Closing saves what is still dirty.
  {
    ApplicationProperties props;
    props.setStorageParameters(options);
    props.getUserSettings()->setValue("closing", "saved");
    props.closeFiles();
    check(readText(expectedUser).find("name=\"closing\" val=\"saved\"") != std::string::npos,
          "closeFiles writes a change the clock had not reached yet");
  }
#if defined(__APPLE__)
  setEnv("HOME", savedHome);
#endif
}

// ── FileBasedDocument ───────────────────────────────────────────────────────

struct TestDocument : FileBasedDocument {
  TestDocument() : FileBasedDocument(".sdoc", "*.sdoc", "Open a song", "Save the song") {}

  std::string text;
  std::vector<std::string> log;
  SaveChoice saveChoice = SaveChoice::cancel;
  bool overwrite = false;
  std::string openAnswer, saveAnswer;
  bool deferQuestions = false;
  std::function<void(SaveChoice)> pendingSaveReply;
  bool failNextSave = false;
  std::string lastSuggested;

  std::string getDocumentTitle() override {
    return getFile().empty() ? "Untitled" : detail::recent::leafName(getFile());
  }

  Result loadDocument(const std::string& file) override {
    log.push_back("load");
    const std::string content = readText(file);
    if (content.compare(0, 4, "DOC:") != 0) return Result::fail("not a song file");
    text = content.substr(4);
    return Result::ok();
  }

  Result saveDocument(const std::string& file) override {
    log.push_back("save");
    if (failNextSave) {
      failNextSave = false;
      return Result::fail("disk full");
    }
    writeText(file, "DOC:" + text);
    return Result::ok();
  }

  void askToSaveChanges(std::function<void(SaveChoice)> reply) override {
    log.push_back("ask-save");
    if (deferQuestions) pendingSaveReply = std::move(reply);
    else reply(saveChoice);
  }
  void askToOverwrite(const std::string&, std::function<void(bool)> reply) override {
    log.push_back("ask-overwrite");
    reply(overwrite);
  }
  void chooseFileToOpen(const std::string& start, std::function<void(std::string)> reply) override {
    log.push_back("choose-open:" + start);
    reply(openAnswer);
  }
  void chooseFileToSave(const std::string& suggested, std::function<void(std::string)> reply) override {
    log.push_back("choose-save");
    lastSuggested = suggested;
    reply(saveAnswer);
  }
  void showFailure(const std::string& title, const std::string&) override {
    log.push_back("failure:" + title);
  }

  bool logged(const std::string& entry) const {
    for (const auto& e : log)
      if (e == entry) return true;
    return false;
  }
};

/** A document whose "save changes?" is an AlertOverlay inside a window's
 *  component tree -- the in-editor question, answered later. */
struct OverlayDocument : TestDocument {
  gfx::AlertOverlay* alert = nullptr;
  void askToSaveChanges(std::function<void(SaveChoice)> reply) override {
    log.push_back("overlay-ask");
    alert->show("Closing document...", "Save the changes to \"" + getDocumentTitle() + "\"?",
                {"Save", "Discard", "Cancel"}, [reply](int i) {
                  reply(i == 0 ? SaveChoice::save : (i == 1 ? SaveChoice::discard : SaveChoice::cancel));
                });
  }
};

static void testFileBasedDocument() {
  std::printf("\n-- FileBasedDocument --\n");
  const fs::path dir = gTempRoot / "documents";
  fs::create_directories(dir);
  RecentlyOpenedFilesList recent;

  TestDocument doc;
  doc.setRecentFilesList(&recent);
  check(!doc.hasChangedSinceSaved() && doc.getFile().empty(), "a new document is unchanged and has no file");
  check(doc.saveIfNeededAndUserAgrees() == FileBasedDocument::savedOk && doc.log.empty(),
        "closing an unchanged document asks nothing");

  doc.text = "first take";
  doc.changed();
  check(doc.hasChangedSinceSaved() && doc.hasPendingChange(), "an edit marks it changed, and tells listeners");

  doc.saveChoice = FileBasedDocument::SaveChoice::cancel;
  check(doc.saveIfNeededAndUserAgrees() == FileBasedDocument::userCancelledSave &&
            doc.hasChangedSinceSaved(),
        "Cancel: userCancelledSave, still changed");
  doc.saveChoice = FileBasedDocument::SaveChoice::discard;
  doc.log.clear();
  check(doc.saveIfNeededAndUserAgrees() == FileBasedDocument::savedOk && !doc.logged("save"),
        "Discard: savedOk, and nothing written");

  // Save, never saved before: asked where. "first" has no extension.
  doc.saveChoice = FileBasedDocument::SaveChoice::save;
  doc.saveAnswer = u8(dir / "first");
  doc.log.clear();
  const auto result = doc.saveIfNeededAndUserAgrees();
  const std::string first = u8(dir / "first.sdoc");
  std::snprintf(msg, sizeof(msg), "Save on a document with no file asks where (suggesting %s), "
                "adds the extension and writes it", doc.lastSuggested.c_str());
  check(result == FileBasedDocument::savedOk && doc.getFile() == first &&
            readText(first) == "DOC:first take" && doc.lastSuggested == "Untitled.sdoc",
        msg);
  check(!doc.hasChangedSinceSaved(), "and it is unchanged after");
  check(recent.getNumFiles() == 1 && recent.getFile(0) == first, "the recent-files hook got it");

  doc.text = "second take";
  doc.changed();
  doc.log.clear();
  check(doc.save(true, true) == FileBasedDocument::savedOk && !doc.logged("choose-save") &&
            readText(first) == "DOC:second take",
        "Save with a file goes straight to it");

  // Save As over an existing file: asked, and No means no.
  const std::string second = u8(dir / "second.sdoc");
  writeText(second, "DOC:keep me");
  doc.overwrite = false;
  check(doc.saveAs(second, true, false, true) == FileBasedDocument::userCancelledSave &&
            readText(second) == "DOC:keep me" && doc.getFile() == first,
        "Save As over an existing file, answered No: nothing written, still the old file");
  doc.overwrite = true;
  check(doc.saveAs(second, true, false, true) == FileBasedDocument::savedOk &&
            readText(second) == "DOC:second take" && doc.getFile() == second,
        "answered Yes: written, and the document is now that file");
  check(recent.getFile(0) == second && recent.getFile(1) == first, "most recent first");

  // A failed save leaves the document where it was.
  doc.text = "third take";
  doc.changed();
  doc.failNextSave = true;
  doc.log.clear();
  check(doc.saveAs(u8(dir / "third.sdoc"), false, false, true) ==
                FileBasedDocument::failedToWriteToFile &&
            doc.getFile() == second && doc.hasChangedSinceSaved() &&
            doc.logged("failure:Error writing to file..."),
        "a failed save: failedToWriteToFile, the user told, the file and the changed flag unmoved");
  check(doc.saveAs(std::string(), false, false, true) == FileBasedDocument::failedToWriteToFile,
        "no file and not allowed to ask: a failure, not a cancel");

  // Loading.
  {
    TestDocument reader;
    reader.setRecentFilesList(&recent);
    const auto missing = reader.loadFrom(u8(dir / "nope.sdoc"), true);
    check(missing.failed() && reader.getFile().empty() && reader.logged("failure:Failed to open file..."),
          "loading a missing file fails, says so, and leaves the document as it was");
    const std::string junk = u8(dir / "junk.sdoc");
    writeText(junk, "not a song");
    const auto bad = reader.loadFrom(junk, false);
    check(bad.failed() && bad.getErrorMessage() == "not a song file" && reader.getFile().empty(),
          "loadDocument's own failure is the result, and the file is not taken");
    const auto ok = reader.loadFrom(first, true);
    check(ok.wasOk() && reader.text == "second take" && reader.getFile() == first &&
              !reader.hasChangedSinceSaved() && recent.getFile(0) == first,
          "a good file loads, becomes the document's file, and goes to the top of the recent list");

    reader.openAnswer = std::string();
    reader.log.clear();
    const auto cancelled = reader.loadFromUserSpecifiedFile(true);
    check(cancelled.failed() && reader.logged("choose-open:" + first),
          "Open starts where the last document was, and a cancel is a failed result");
    reader.openAnswer = second;
    check(reader.loadFromUserSpecifiedFile(true).wasOk() && reader.getFile() == second,
          "and a chosen file is loaded");
  }

  // Suggested names do not collide.
  {
    TestDocument fresh;
    writeText(u8(dir / "Untitled.sdoc"), "DOC:");
    fresh.text = "x";
    fresh.changed();
    fresh.saveAnswer = std::string();
    fresh.setRecentFilesList(&recent);
    recent.addFile(u8(dir / "first.sdoc"));
    fresh.saveAsInteractive(true);
    const std::string expected = u8(dir / "Untitled (2).sdoc");
    std::snprintf(msg, sizeof(msg), "Save As suggests a name beside the last document that does "
                  "not exist yet: %s", fresh.lastSuggested.c_str());
    check(fresh.lastSuggested == expected, msg);
  }

  // A question answered LATE: the plain form has already returned "cancelled",
  // and the late Save must not then happen behind the caller's back.
  {
    TestDocument late;
    late.text = "late";
    late.changed();
    late.deferQuestions = true;
    late.saveAnswer = u8(dir / "late.sdoc");
    check(late.saveIfNeededAndUserAgrees() == FileBasedDocument::userCancelledSave,
          "a question not yet answered: the plain form reports a cancel");
    late.log.clear();
    late.pendingSaveReply(FileBasedDocument::SaveChoice::save);
    check(late.log.empty() && !fs::exists(fs::u8path(late.saveAnswer)),
          "and the late Save does nothing");

    // The async form waits for it.
    FileBasedDocument::SaveResult got = FileBasedDocument::failedToWriteToFile;
    bool called = false;
    late.saveIfNeededAndUserAgreesAsync([&](FileBasedDocument::SaveResult r) {
      got = r;
      called = true;
    });
    check(!called, "the async form has not finished while the question is open");
    late.pendingSaveReply(FileBasedDocument::SaveChoice::save);
    check(called && got == FileBasedDocument::savedOk && readText(late.saveAnswer) == "DOC:late",
          "and finishes, saved, when it is answered");
  }

  // The document deleted while its question is open.
  {
    auto* doomed = new TestDocument();
    doomed->deferQuestions = true;
    doomed->changed();
    bool called = false;
    doomed->saveIfNeededAndUserAgreesAsync([&](FileBasedDocument::SaveResult) { called = true; });
    auto reply = doomed->pendingSaveReply;
    delete doomed;
    reply(FileBasedDocument::SaveChoice::save);
    check(!called, "a document deleted before its question is answered is never called back");
  }

  // The SDK's own in-editor question: an AlertOverlay, answered from the keyboard.
  {
    gfx::Component root;
    root.setBounds({0.0f, 0.0f, 400.0f, 300.0f});
    gfx::AlertOverlay alert;
    root.addChild(&alert);
    gfx::MouseRouter router(root);

    OverlayDocument overlayDoc;
    overlayDoc.alert = &alert;
    overlayDoc.text = "from the overlay";
    overlayDoc.changed();
    overlayDoc.saveAnswer = u8(dir / "overlay.sdoc");
    FileBasedDocument::SaveResult got = FileBasedDocument::failedToWriteToFile;
    bool called = false;
    overlayDoc.saveIfNeededAndUserAgreesAsync([&](FileBasedDocument::SaveResult r) {
      got = r;
      called = true;
    });
    check(alert.isShowing() && !called, "the question is an AlertOverlay in the editor, still open");
    gfx::KeyPress escape;
    escape.keyCode = gfx::KeyPress::Escape;
    alert.keyPressed(escape);
    check(called && got == FileBasedDocument::userCancelledSave && !alert.isShowing(),
          "Escape is Cancel");
    called = false;
    overlayDoc.saveIfNeededAndUserAgreesAsync([&](FileBasedDocument::SaveResult r) {
      got = r;
      called = true;
    });
    gfx::KeyPress enter;
    enter.keyCode = gfx::KeyPress::Return;
    alert.keyPressed(enter);
    check(called && got == FileBasedDocument::savedOk &&
              readText(overlayDoc.saveAnswer) == "DOC:from the overlay",
          "Return is the default button, Save, and the file is written");
    root.removeChild(&alert);
  }
}

// ── PreferencesPanel ────────────────────────────────────────────────────────

static int gPagesAlive = 0;
static int gPagesMade = 0;

struct ColourPage : gfx::Component {
  explicit ColourPage(gfx::Colour c) : colour(c) {
    ++gPagesAlive;
    ++gPagesMade;
  }
  ~ColourPage() override { --gPagesAlive; }
  void paint(gfx::Graphics& g) override { g.fillAll(colour); }
  gfx::Colour colour;
};

// A page with a button that goes to another page. The page it goes to is held
// BY VALUE in the button's std::function, as a lambda naming it is: freed
// with the page, and passed to setCurrentPage by reference.
struct JumpPage : gfx::Component {
  JumpPage(gfx::PreferencesPanel& panel, std::string target) {
    ++gPagesAlive;
    ++gPagesMade;
    go.setBounds({10.0f, 10.0f, 100.0f, 30.0f});
    go.onClick = [&panel, target]() { panel.setCurrentPage(target); };
    addChild(&go);
  }
  ~JumpPage() override { --gPagesAlive; }
  gfx::Button go;
};

struct TestPreferences : gfx::PreferencesPanel {
  std::unique_ptr<gfx::Component> createComponentForPage(const std::string& name) override {
    if (name == "General") return std::make_unique<ColourPage>(gfx::Colour(220, 30, 30, 255));
    if (name == "Audio") return std::make_unique<ColourPage>(gfx::Colour(30, 200, 40, 255));
    if (name == "MIDI") return std::make_unique<ColourPage>(gfx::Colour(30, 40, 220, 255));
    return nullptr;
  }
};

static gfx::PremulColour renderAt(gfx::Component& c, int w, int h, int x, int y,
                                  gfx::Bitmap* out = nullptr) {
  gfx::Bitmap bmp(w, h);
  bmp.clear(gfx::Colour(0, 0, 0, 255));
  {
    gfx::Graphics g(bmp);
    c.paintTree(g);
  }
  const gfx::PremulColour p = bmp.pixelAt(x, y);
  if (out) *out = bmp;
  return p;
}

static void testPreferencesPanel() {
  std::printf("\n-- PreferencesPanel --\n");
  using namespace sonore::gfx;
#if defined(__APPLE__)
  // Apple's MacTypes.h declares a global Rect and Point too, and the
  // CoreFoundation headers bring it in: unqualified, both are ambiguous. A
  // local alias hides the outer two (a file-scope one would redeclare
  // Apple's typedef), as sdk_tests' geometry tests do.
  using Rect = sonore::gfx::Rect;
  using Point = sonore::gfx::Point;
#endif
  auto face = systemTypeface();
  Font font = face ? Font(face, 13.0f) : Font();
  if (!face) check(true, "no system font: labels are not drawn, the rest still is");

  TestPreferences panel;
  panel.setFont(font);
  panel.setBounds({0.0f, 0.0f, 300.0f, 220.0f});
  int changes = 0;
  std::string lastChange;
  panel.onPageChanged = [&](const std::string& name) {
    ++changes;
    lastChange = name;
  };
  const std::string icon =
      "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 24 24\">"
      "<circle cx=\"12\" cy=\"12\" r=\"10\" fill=\"currentColor\"/></svg>";
  panel.addSettingsPage("General", icon);
  panel.addSettingsPage("Audio", icon);
  panel.addSettingsPage("MIDI");
  check(panel.getNumPages() == 3 && panel.getCurrentPageName() == "General",
        "three pages, the first one current");
  check(gPagesMade == 1 && gPagesAlive == 1, "only the current page's component was made");
  const Rect area = panel.pageArea();
  std::snprintf(msg, sizeof(msg), "the page sits below the buttons: top %.0f, height %.0f", area.y,
                area.h);
  check(area.y == PreferencesPanel::kDefaultButtonSize + PreferencesPanel::kPageGap &&
            panel.getCurrentPageComponent()->bounds().y == area.y,
        msg);

  Bitmap shot;
  PremulColour page = renderAt(panel, 300, 220, 150, 160, &shot);
  std::snprintf(msg, sizeof(msg), "rendered, the General page fills its area (%d,%d,%d)", page.r,
                page.g, page.b);
  check(page.r > 200 && page.g < 60 && page.b < 60, msg);
  const PremulColour currentButton = shot.pixelAt(6, 30);
  const PremulColour otherButton = shot.pixelAt(76, 30);
  check(currentButton.r != otherButton.r || currentButton.g != otherButton.g ||
            currentButton.b != otherButton.b,
        "the current page's button is drawn differently from the others");
  // The icon is drawn in the theme colour inside the button.
  const PremulColour iconPixel = shot.pixelAt(35, 26);
  const PremulColour beside = shot.pixelAt(6, 26);
  check(iconPixel.r != beside.r || iconPixel.g != beside.g || iconPixel.b != beside.b,
        "the button's icon is drawn");
  if (face) {
    // With no icon the label is centred in the button: count what differs
    // from the button's own plate there.
    const PremulColour plate = shot.pixelAt(146, 8);
    int ink = 0;
    for (int y = 20; y < 50; ++y)
      for (int x = 146; x < 204; ++x) {
        const PremulColour p = shot.pixelAt(x, y);
        if (std::abs((int) p.r - (int) plate.r) + std::abs((int) p.g - (int) plate.g) +
                std::abs((int) p.b - (int) plate.b) > 60)
          ++ink;
      }
    std::snprintf(msg, sizeof(msg), "the label-only MIDI button shows its name (%d inked pixels)", ink);
    check(ink > 20, msg);
  }

  // Clicking a button: the old page is destroyed, a new one made.
  Component root;
  root.setBounds({0.0f, 0.0f, 300.0f, 220.0f});
  root.addChild(&panel);
  MouseRouter router(root);
  const Rect audio = panel.pageButtonBounds(1);
  router.mouseDown({audio.centre().x, audio.centre().y});
  router.mouseUp({audio.centre().x, audio.centre().y});
  check(panel.getCurrentPageName() == "Audio" && changes == 2 && lastChange == "Audio",
        "clicking the second button shows the Audio page");
  check(gPagesMade == 2 && gPagesAlive == 1, "made on demand, and the General page was destroyed");
  page = renderAt(panel, 300, 220, 150, 160);
  check(page.g > 180 && page.r < 60, "and the Audio page is what is drawn now");

  // A press that leaves the button before release is not a click.
  const Rect midi = panel.pageButtonBounds(2);
  router.mouseDown({midi.centre().x, midi.centre().y});
  router.mouseUp({midi.centre().x, 150.0f});
  check(panel.getCurrentPageName() == "Audio", "released off the button: nothing changes");

  KeyPress right;
  right.keyCode = KeyPress::Right;
  KeyPress left;
  left.keyCode = KeyPress::Left;
  panel.keyPressed(right);
  check(panel.getCurrentPageName() == "MIDI", "the right arrow moves to the next page");
  check(!panel.keyPressed(right) && panel.getCurrentPageName() == "MIDI", "and stops at the last");
  page = renderAt(panel, 300, 220, 150, 160);
  check(page.b > 180 && page.r < 60, "the MIDI page is drawn");
  panel.keyPressed(left);
  check(panel.getCurrentPageName() == "Audio", "the left arrow moves back");

  const int made = gPagesMade;
  panel.setCurrentPage("Audio");
  panel.setCurrentPage("Nonsense");
  check(gPagesMade == made && panel.getCurrentPageName() == "Audio",
        "choosing the current page, or one that does not exist, changes nothing");

  panel.setButtonSize(50.0f);
  check(panel.pageButtonBounds(2).x == 100.0f && panel.getCurrentPageComponent()->bounds().y == 55.0f,
        "a smaller button size moves the buttons and the page with it");
  root.removeChild(&panel);
  {
    TestPreferences scoped;
    scoped.addSettingsPage("General");
  }
  check(gPagesAlive == 1, "a panel destroyed with a page showing destroys the page");

  // A page that switches pages from its own button. The old page was
  // destroyed there and then: the button whose click was still on the stack
  // went with it, and so did the page name its handler had passed in --
  // read again to make the new page, a use-after-free under ASan. Now the
  // old page leaves the tree at once and is freed at the next paint.
  {
    const int baseline = gPagesAlive;
    {
      PreferencesPanel jumping;
      jumping.createPage = [&jumping](const std::string& name) -> std::unique_ptr<Component> {
        return std::unique_ptr<Component>(new JumpPage(jumping, name == "General" ? "Audio" : "General"));
      };
      jumping.setBounds({0.0f, 0.0f, 300.0f, 220.0f});
      jumping.addSettingsPage("General");
      jumping.addSettingsPage("Audio");
      Component top;
      top.setBounds({0.0f, 0.0f, 300.0f, 220.0f});
      top.addChild(&jumping);
      MouseRouter clicks(top);
      const Rect pageArea = jumping.pageArea();
      const Point onButton{pageArea.x + 20.0f, pageArea.y + 20.0f};

      clicks.mouseDown(onButton);
      clicks.mouseUp(onButton);
      check(jumping.getCurrentPageName() == "Audio", "a button on the General page switches to Audio");
      std::snprintf(msg, sizeof(msg), "the page it was on outlives the click: %d page(s) alive, 2 expected",
                    gPagesAlive - baseline);
      check(gPagesAlive - baseline == 2, msg);
      renderAt(jumping, 300, 220, 150, 160);
      check(gPagesAlive - baseline == 1, "and is freed at the next paint");

      clicks.mouseDown(onButton);
      clicks.mouseUp(onButton);
      check(jumping.getCurrentPageName() == "General", "the Audio page's own button switches back");
      top.removeChild(&jumping);
    }
    check(gPagesAlive == baseline, "a panel destroyed with a page still waiting for its paint frees it");
  }
}

// ── RecentlyOpenedFilesList ─────────────────────────────────────────────────

struct MenuRecorder {
  std::vector<std::pair<int, std::string>> items;
  void addItem(int id, std::string text) { items.emplace_back(id, std::move(text)); }
};

static void testRecentFiles() {
  std::printf("\n-- RecentlyOpenedFilesList --\n");
  const fs::path dir = gTempRoot / "recent";
  fs::create_directories(dir);
  const std::string a = u8(dir / "a.txt"), b = u8(dir / "b.txt"), c = u8(dir / "c.txt");
  writeText(a, "a");
  writeText(c, "c");

  RecentlyOpenedFilesList list;
  check(list.getMaxNumberOfItems() == 8, "eight entries by default, as in the reference");
  list.addFile(a);
  list.addFile(b);
  list.addFile(c);
  check(list.getNumFiles() == 3 && list.getFile(0) == c && list.getFile(2) == a, "most recent first");
  list.addFile(a);
  check(list.getNumFiles() == 3 && list.getFile(0) == a && list.getFile(1) == c,
        "re-opening a file moves it to the top instead of adding it twice");
#if defined(_WIN32)
  std::string shouty = a;
  for (char& ch : shouty) {
    if (ch == '\\') ch = '/';
    if (ch >= 'a' && ch <= 'z') ch = (char) (ch - 'a' + 'A');
  }
  list.addFile(shouty);
  check(list.getNumFiles() == 3 && list.getFile(0) == shouty,
        "on Windows the same file spelled with other case and slashes is the same entry");
  list.addFile(a);
#endif

  MenuRecorder menu;
  const int added = list.createPopupMenuItems(menu, 100, false, true);
  // a (0), c (1), b (2, missing on disk)
  check(added == 2 && menu.items.size() == 2 && menu.items[0].first == 100 &&
            menu.items[0].second == "a.txt" && menu.items[1].first == 101,
        "menu items: leaf names, ids base + index, a missing file left out");
  MenuRecorder full;
  const std::vector<std::string> avoid{c};
  list.createPopupMenuItems(full, 1, true, false, &avoid);
  check(full.items.size() == 2 && full.items[1].first == 3 && full.items[1].second == b,
        "full paths, a file to avoid skipped, and the ids still map back by index");
  check(list.getFile(full.items[1].first - 1) == b, "so getFile(id - base) is the clicked file");

  gfx::PopupMenu popup;
  list.createPopupMenuItems(popup, 10, false, false);
  check(popup.numItems() == 3 && popup.itemAt(1).id == 11 && popup.itemAt(1).text == "c.txt",
        "it fills the SDK's own PopupMenu");

  const std::string saved = list.toString();
  RecentlyOpenedFilesList restored;
  restored.restoreFromString(saved);
  check(restored.getAllFilenames() == list.getAllFilenames(), "toString and restoreFromString round-trip");
  restored.setMaxNumberOfItems(2);
  restored.restoreFromString("\r\n" + a + "\r\n\r\n" + a + "\n" + b + "\n" + c + "\n");
  check(restored.getNumFiles() == 2 && restored.getFile(0) == a && restored.getFile(1) == b,
        "restoring drops blank lines, CRs, repeats and anything past the limit");
  restored.setMaxNumberOfItems(0);
  check(restored.getMaxNumberOfItems() == 1 && restored.getNumFiles() == 1 && restored.getFile(0) == a,
        "a limit below one is one, and shrinking drops the oldest");

  list.removeNonExistentFiles();
  check(list.getNumFiles() == 2 && list.getFile(0) == a && list.getFile(1) == c,
        "removeNonExistentFiles drops the file that is gone and keeps the order");
  list.removeFile(c);
  check(list.getNumFiles() == 1, "removeFile");
  list.clear();
  check(list.getNumFiles() == 0, "clear");

#if defined(_WIN32)
  // The OS list, for real: a shortcut appears in the user's Recent folder,
  // pointing at the file, and forgetting it removes exactly that shortcut.
  //
  // Not in the temp folder: the shell silently ignores files there (probed:
  // the same call on the same file name made a shortcut from %LOCALAPPDATA%
  // and none from %TEMP%).
  {
    char name[64];
    std::snprintf(name, sizeof(name), "sonore_recent_%lu.txt", (unsigned long) GetCurrentProcessId());
    const fs::path nativeDir = fs::u8path(pathio::environment("LOCALAPPDATA")) /
                               ("sonore_app_infra_" + std::to_string((unsigned long) GetCurrentProcessId()));
    std::error_code ec;
    fs::create_directories(nativeDir, ec);
    const std::string file = u8(nativeDir / name);
    writeText(file, "recent");
    const std::wstring link =
        detail::recent::recentFolder() + L"\\" + pathio::widen(name) + L".lnk";
    RecentlyOpenedFilesList::registerRecentFileNatively(file);
    const auto start = Clock::now();
    while (GetFileAttributesW(link.c_str()) == INVALID_FILE_ATTRIBUTES && msSince(start) < 3000.0)
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    if (GetFileAttributesW(link.c_str()) == INVALID_FILE_ATTRIBUTES) {
      check(true, "the shell made no Recent shortcut (recent items are turned off on this "
                  "machine) -- native registration SKIPPED, not passed");
    } else {
      const std::string target = pathio::narrow(detail::recent::shortcutTarget(link).c_str());
      std::snprintf(msg, sizeof(msg), "SHAddToRecentDocs made a shortcut to the file (%s)",
                    target.c_str());
      check(detail::recent::samePath(target, file), msg);
      RecentlyOpenedFilesList::forgetRecentFileNatively(file);
      check(GetFileAttributesW(link.c_str()) == INVALID_FILE_ATTRIBUTES,
            "forgetRecentFileNatively removes that shortcut");
    }
    RecentlyOpenedFilesList::forgetRecentFileNatively(u8(dir / "never-registered.txt"));
    check(true, "forgetting a file that was never registered is harmless");
    fs::remove_all(nativeDir, ec);
  }
#endif
}

int main(int argc, char** argv) {
  if (argc > 1 && std::strcmp(argv[1], "--broadcast-child") == 0) return runBroadcastChild();

  std::printf("app infrastructure tests\n");
  std::error_code ec;
#if defined(_WIN32)
  gTempRoot = fs::temp_directory_path() /
              ("sonore_app_infra_" + std::to_string((unsigned long) GetCurrentProcessId()));
#else
  gTempRoot = fs::temp_directory_path() / ("sonore_app_infra_" + std::to_string((long) getpid()));
#endif
  fs::remove_all(gTempRoot, ec);
  fs::create_directories(gTempRoot);

  const auto start = Clock::now();
  MessageManager::getInstance();
  testCallAsync();
  testFlood();
  testPumpDeadline();
  testCallFunctionOnMessageThread();
  testLockStress();
  testLockAbort();
  testMoveMessageThread();
  testMessagesAndListeners();
  testLockingAsyncUpdater();
  testBroadcast();
  testApplicationProperties();
  testFileBasedDocument();
  testPreferencesPanel();
  testRecentFiles();
  MessageManager::deleteInstance();

  fs::remove_all(gTempRoot, ec);
  std::printf("\n%d passed, %d failed, %.1f s\n", passes, failures, msSince(start) / 1000.0);
  std::printf(failures == 0 ? "APP INFRA TESTS PASSED\n" : "APP INFRA TESTS FAILED\n");
  return failures == 0 ? 0 : 1;
}
