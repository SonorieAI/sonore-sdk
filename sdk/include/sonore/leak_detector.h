// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: a count of live objects per class, reported at exit if any
// are still alive.
//
// ── What it is for, next to a sanitizer ─────────────────────────────────────
//
// AddressSanitizer finds leaks, and finds them better: every allocation, with
// the stack that made it. But it runs in a sanitizer build, and a plugin's
// leaks show up in a HOST -- a DAW that loads the plugin, opens and closes the
// editor forty times and quits -- which is not built with ASan and often will
// not load an ASan-built plugin at all. This works in an ordinary debug build
// inside any host: each class that declares SONORE_LEAK_DETECTOR(ClassName)
// keeps an atomic count of its live instances, and when the module unloads, a
// count above zero is reported by name. "3 instance(s) of FilterEditor were
// never deleted" says which class to look at; the sanitizer then says where.
//
// The report goes to stderr, to the debugger's output window on Windows (a
// host has no console), and to a handler if one is installed -- a test can
// install one and assert on it.
//
// ── Cost ────────────────────────────────────────────────────────────────────
//
// On by default in debug builds (NDEBUG not defined), nothing at all in
// release: the macro expands to no member, so the class's size and layout are
// what they would be without it. Define SONORE_CHECK_MEMORY_LEAKS to 1 or 0 to
// choose. Where it is on, construction and destruction are one atomic
// increment or decrement each -- lock-free, so an object made on the audio
// thread may carry one.
#pragma once

#include <atomic>
#include <cstdio>

#ifndef SONORE_CHECK_MEMORY_LEAKS
#if defined(NDEBUG)
#define SONORE_CHECK_MEMORY_LEAKS 0
#else
#define SONORE_CHECK_MEMORY_LEAKS 1
#endif
#endif

#if defined(_WIN32) && !defined(_WINDOWS_)
// The one Windows call this needs, declared exactly as <windows.h> declares
// it, so this header does not drag the Windows API into every class that
// counts itself -- and still agrees with windows.h if that comes later.
extern "C" __declspec(dllimport) void __stdcall OutputDebugStringA(const char* lpOutputString);
#endif

namespace sonore {

/** Receives each leak report: the class name, how many instances are alive
 *  (negative: more were deleted than created), and the formatted message. */
using LeakReportHandler = void (*)(const char* className, int liveCount, const char* message);

namespace detail {

// A plain function pointer in an atomic: trivially destructible, so it is
// still there when the counters report during static destruction -- a
// std::function might already have been destroyed by then.
inline std::atomic<LeakReportHandler>& leakReportHandlerSlot() {
  static std::atomic<LeakReportHandler> handler{nullptr};
  return handler;
}

inline void reportLeakMessage(const char* className, int liveCount) {
  char message[320];
  if (liveCount > 0)
    std::snprintf(message, sizeof(message),
                  "*** Leaked objects detected: %d instance(s) of class %s were never deleted\n",
                  liveCount, className);
  else
    std::snprintf(message, sizeof(message),
                  "*** More instances of class %s were deleted than were created -- a dangling "
                  "pointer was deleted, or one object deleted twice\n",
                  className);
  if (const LeakReportHandler handler = leakReportHandlerSlot().load()) {
    handler(className, liveCount, message);
    return;
  }
  std::fputs(message, stderr);
  std::fflush(stderr);
#if defined(_WIN32)
  OutputDebugStringA(message);
#endif
}

} // namespace detail

/** Installs the function leak reports go to instead of stderr and the
 *  debugger; nullptr goes back to those. Returns the previous handler. */
inline LeakReportHandler setLeakReportHandler(LeakReportHandler handler) {
  return detail::leakReportHandlerSlot().exchange(handler);
}

/**
 * Counts the live instances of OwnerClass. Normally added through
 * SONORE_LEAK_DETECTOR(OwnerClass) inside the class; a member of this type is
 * created and destroyed (and copied) with its owner.
 */
template <class OwnerClass>
class LeakedObjectDetector {
public:
  /** Counts one more live OwnerClass. Lock-free, from any thread. */
  LeakedObjectDetector() noexcept { counter().live.fetch_add(1, std::memory_order_relaxed); }
  /** A copy of an owner is one more live instance, counted as such. */
  LeakedObjectDetector(const LeakedObjectDetector&) noexcept {
    counter().live.fetch_add(1, std::memory_order_relaxed);
  }
  /** Assignment copies an object; it does not make a new one. */
  LeakedObjectDetector& operator=(const LeakedObjectDetector&) noexcept { return *this; }

  /** Counts one fewer. Going below zero -- more destroyed than made, a
   *  double delete -- is reported at once and the count reset to 0. */
  ~LeakedObjectDetector() {
    if (counter().live.fetch_sub(1, std::memory_order_relaxed) <= 0) {
      // More destroyed than created: a delete of something already deleted.
      // Reported now, while the stack still says who did it.
      counter().live.store(0, std::memory_order_relaxed);
      detail::reportLeakMessage(getClassName(), -1);
    }
  }

  /** Instances of OwnerClass alive right now. */
  static int getLiveCount() noexcept { return counter().live.load(std::memory_order_relaxed); }

  /** Reports now if any instances are alive, and says whether it did. Called
   *  automatically when the module unloads. */
  static bool reportIfLeaked() {
    const int live = getLiveCount();
    if (live <= 0) return false;
    detail::reportLeakMessage(getClassName(), live);
    return true;
  }

  /** The name SONORE_LEAK_DETECTOR gave the class, or a placeholder for a
   *  class that holds a detector by hand. */
  static const char* getClassName() { return nameOf<OwnerClass>(0); }

private:
  // Asked from inside this class, which SONORE_LEAK_DETECTOR makes a friend:
  // the name function is usually in the owner's private section.
  template <class T>
  static auto nameOf(int) -> decltype(T::sonoreLeakedObjectClassName()) {
    return T::sonoreLeakedObjectClassName();
  }
  template <class T>
  static const char* nameOf(...) {
    return "(a class without SONORE_LEAK_DETECTOR)";
  }

  /** The live count for one OwnerClass, created on first use. Its destructor
   *  runs during static destruction, after every object constructed after it
   *  is gone -- which is when anything still counted has genuinely leaked,
   *  and is where the report at exit comes from. */
  struct Counter {
    std::atomic<int> live{0}; ///< instances alive now
    /** Reports what is still alive, at static destruction. */
    ~Counter() {
      const int n = live.load(std::memory_order_relaxed);
      if (n > 0) detail::reportLeakMessage(getClassName(), n);
    }
  };
  static Counter& counter() noexcept {
    static Counter c;
    return c;
  }
};

} // namespace sonore

#if SONORE_CHECK_MEMORY_LEAKS
/** Inside a class: counts its live instances and reports any left at exit.
 *  Nothing at all when SONORE_CHECK_MEMORY_LEAKS is 0 (release builds). */
#define SONORE_LEAK_DETECTOR(OwnerClass)                                        \
  friend class ::sonore::LeakedObjectDetector<OwnerClass>;                      \
  static const char* sonoreLeakedObjectClassName() noexcept { return #OwnerClass; } \
  ::sonore::LeakedObjectDetector<OwnerClass> sonoreLeakDetector_;
#else
#define SONORE_LEAK_DETECTOR(OwnerClass)
#endif
