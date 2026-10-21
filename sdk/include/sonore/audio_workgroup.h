// SPDX-License-Identifier: Apache-2.0
// Sonore SDK. Audio workgroups: putting a helper thread under the audio
// deadline.
//
// ── What a workgroup is ─────────────────────────────────────────────────────
//
// From macOS 11 every audio device's IO thread belongs to an os_workgroup, and
// the scheduler budgets the WHOLE group against that device's deadline. A
// helper thread outside it -- a voice renderer, a convolution partition --
// does not count toward the deadline at all: it competes with the IO thread
// rather than being scheduled beside it, and on Apple silicon it can sit on an
// efficiency core at the moment the buffer is due. Joining puts it in the
// group. That is the whole feature, and it only matters to a DSP that renders
// one block across several threads.
//
// ── Where a workgroup comes from ────────────────────────────────────────────
//
//   standalone   kAudioDevicePropertyIOThreadOSWorkgroup on the device the
//                standalone opened (CoreAudioOutput::ioWorkgroup).
//   AU plugin    kAudioUnitProperty_RenderContextObserver: the plugin hands
//                the OS a block, and the OS calls it with the render context
//                -- workgroup inside -- whenever the render thread's context
//                changes (au_wrapper.h).
//   CLAP, VST3   NO API. Neither format tells a plugin which workgroup its
//   and LV2      host's audio thread is in, so a DSP built as one of them is
//                never handed a workgroup and its helper threads run outside
//                the deadline. Said here rather than papered over: there is
//                no call that would find it.
//
// A DSP is told by declaring `void workgroupChanged(const sonore::AudioWorkgroup&)`
// -- detected like every other optional hook -- and typically hands it to a
// realtime ThreadPool (thread_pool.h), whose workers join it themselves.
//
// ── The token ───────────────────────────────────────────────────────────────
//
// os_workgroup_join makes the CALLING thread a member and fills in a token;
// os_workgroup_leave, on the same thread, with the same token, ends it.
// WorkgroupToken is that token, and it leaves when it is destroyed -- so it
// must be destroyed on the thread that joined, which is where a worker keeps
// it: on its own stack. On a platform with no workgroups a join fails, the
// token is inactive, and describe() says why. Nothing pretends to have
// happened.
//
// ── Interval and period hints ───────────────────────────────────────────────
//
// os_workgroup_interval_start/update/finish belong to the workgroup's OWNER:
// for a device's group that is CoreAudio's IO thread, which marks each cycle
// itself. A thread that joins does not call them -- what it owes the group is
// to be a real-time thread whose period matches the cycle. That is
// RealtimeThreadHints: Mach's time-constraint policy (period, computation,
// constraint), derived from the block length and the sample rate.
//
// ── Resolved at run time ────────────────────────────────────────────────────
//
// os_workgroup_* exist from macOS 11, and os_retain/os_release are spelled as
// Objective-C messages when the including file is Objective-C. Both problems
// go away by finding the six functions with dlsym: no availability checks, no
// SDK-version requirement, no header that changes meaning under ARC, and a
// Mac older than 11 gets an AudioWorkgroup that says so instead of a crash.
// The table is resolved the first time anything asks -- call
// AudioWorkgroup::isSupported() once off the audio thread, as the AU wrapper
// and the realtime pool both do, because dlsym takes a lock.
//
// ── Compiled, never run ─────────────────────────────────────────────────────
//
// There is no Mac in this loop and macOS CI is not running. This file is
// compiled for macOS by MSVC against tests/au_shim (tests/mac_syntax_mac_host.cpp),
// which proves every name and call is consistent and nothing about whether
// Apple agrees. tests/mac_host_selftest.cpp is the program a Mac runs to find
// out: it joins the default output device's workgroup from a real-time thread
// and prints what each call returned.
#pragma once

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>

#if defined(__APPLE__)
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#if defined(__has_include)
#if __has_include(<os/workgroup.h>)
#include <os/workgroup.h>
#define SONORE_HAS_OS_WORKGROUP_HEADER 1
#endif
#endif
#endif

namespace sonore {

namespace detail {

/** The six functions, found once. Every pointer is null on a platform -- or a
 *  macOS -- without them, and `ok` says whether all six were found. The
 *  workgroup is passed as void*: os_workgroup_t is an object pointer whose
 *  declared type changes with the language the header is read in, and the
 *  ABI is a pointer either way. */
struct OsWorkgroupApi {
  int (*join)(void* workgroup, void* tokenOut) = nullptr;
  void (*leave)(void* workgroup, void* token) = nullptr;
  int (*maxParallelThreads)(void* workgroup, void* attributes) = nullptr;
  bool (*testCancel)(void* workgroup) = nullptr;
  void* (*retain)(void* object) = nullptr;
  void (*release)(void* object) = nullptr;
  bool ok = false;
};

inline const OsWorkgroupApi& osWorkgroupApi() {
  static const OsWorkgroupApi api = [] {
    OsWorkgroupApi a;
#if defined(__APPLE__)
    // memcpy from the void* dlsym returns: a cast between object and
    // function pointers is what -Wpedantic objects to, and this is the form
    // every compiler accepts without comment.
    bool all = true;
    auto bind = [&all](const char* name, void* slot) {
      void* symbol = dlsym(RTLD_DEFAULT, name);
      if (!symbol) all = false;
      std::memcpy(slot, &symbol, sizeof(symbol));
    };
    bind("os_workgroup_join", &a.join);
    bind("os_workgroup_leave", &a.leave);
    bind("os_workgroup_max_parallel_threads", &a.maxParallelThreads);
    bind("os_workgroup_testcancel", &a.testCancel);
    bind("os_retain", &a.retain);
    bind("os_release", &a.release);
    a.ok = all;
#endif
    return a;
  }();
  return api;
}

/** Room for an os_workgroup_join_token_s: a 4-byte signature and
 *  OS_WORKGROUP_JOIN_TOKEN_SIZE (36 on 64-bit) opaque bytes. 64, aligned to
 *  8, so a later SDK that grew the token by a word would still fit -- and
 *  where the real header is available the size is checked rather than
 *  trusted. */
constexpr size_t kWorkgroupTokenBytes = 64;
#if defined(SONORE_HAS_OS_WORKGROUP_HEADER)
static_assert(sizeof(os_workgroup_join_token_s) <= kWorkgroupTokenBytes,
              "os_workgroup_join_token_s outgrew WorkgroupToken's storage");
#endif

/** The render context kAudioUnitProperty_RenderContextObserver delivers, as
 *  Apple's AudioUnitRenderContext lays it out: the workgroup, then reserved
 *  words. Restated because the real one is declared beside a block typedef
 *  that only a blocks-enabled compiler reads; tests/mac_host_constants.h
 *  checks the two agree on a Mac. */
struct AudioUnitRenderContextMirror {
  void* workgroup;
  uint32_t reserved[6];
};

/** kAudioUnitProperty_RenderContextObserver: 60, as macOS 11's
 *  AudioUnitProperties.h numbers it. Spelled here for SDKs that predate it. */
constexpr uint32_t kRenderContextObserverProperty = 60;

/** Mach's time-constraint policy in Mach absolute-time ticks. */
struct RealtimePolicyTicks {
  uint32_t period = 0;
  uint32_t computation = 0;
  uint32_t constraint = 0;
};

} // namespace detail

/**
 * How a real-time thread asks the scheduler for time.
 *
 * Mach's THREAD_TIME_CONSTRAINT_POLICY in milliseconds: every `periodMs` the
 * thread needs `computationMs` of CPU, finished within `constraintMs` of the
 * period starting. For audio the period is one block. The kernel refuses a
 * computation under 50 microseconds or over 50 ms, and a constraint shorter
 * than the computation; realtimePolicyTicks() clamps into that rather than
 * letting thread_policy_set fail on a tiny block.
 */
struct RealtimeThreadHints {
  double periodMs = 0.0;      ///< How often the thread runs, in ms: one block. 0 for unset.
  double computationMs = 0.0; ///< CPU it needs each period, in ms.
  double constraintMs = 0.0;  ///< Within how long of the period's start it must be done, in ms.

  /** One block at a sample rate. `busyFraction` is how much of the block a
   *  worker expects to compute -- half by default, the share Apple's own
   *  examples give an audio helper thread. */
  static RealtimeThreadHints forBlock(double sampleRate, uint32_t blockSize,
                                      double busyFraction = 0.5) {
    RealtimeThreadHints hints;
    if (!(sampleRate > 0.0) || blockSize == 0) return hints;
    const double period = 1000.0 * (double) blockSize / sampleRate;
    double fraction = busyFraction;
    if (!(fraction > 0.05)) fraction = 0.05;
    if (fraction > 1.0) fraction = 1.0;
    hints.periodMs = period;
    hints.computationMs = period * fraction;
    hints.constraintMs = period;
    return hints;
  }

  /** A period and a computation above zero, and a constraint no shorter
   *  than the computation: something the scheduler can be asked for. */
  bool isValid() const {
    return periodMs > 0.0 && computationMs > 0.0 && constraintMs >= computationMs;
  }
};

namespace detail {

/** Milliseconds to Mach ticks, given mach_timebase_info's numer/denom (ticks
 *  x numer / denom = nanoseconds; 1/1 on Intel, 125/3 on Apple silicon).
 *  Portable arithmetic, so it is tested on a machine without Mach. */
inline uint32_t machTicksFromMs(double ms, uint32_t numer, uint32_t denom) {
  if (!(ms > 0.0) || numer == 0 || denom == 0) return 0;
  const double ticks = ms * 1.0e6 * (double) denom / (double) numer;
  if (ticks >= 4294967295.0) return 0xFFFFFFFFu;
  return (uint32_t) (ticks + 0.5);
}

/** The hints as the kernel will accept them: computation inside
 *  [0.05 ms, 50 ms], constraint at least the computation, period at least
 *  the constraint (zero, "aperiodic", stays zero). */
inline RealtimePolicyTicks realtimePolicyTicks(const RealtimeThreadHints& hints, uint32_t numer,
                                               uint32_t denom) {
  RealtimePolicyTicks t;
  double computation = hints.computationMs;
  if (computation < 0.05) computation = 0.05;
  if (computation > 50.0) computation = 50.0;
  double constraint = hints.constraintMs > computation ? hints.constraintMs : computation;
  double period = hints.periodMs > 0.0 ? (hints.periodMs > constraint ? hints.periodMs : constraint)
                                       : 0.0;
  t.period = machTicksFromMs(period, numer, denom);
  t.computation = machTicksFromMs(computation, numer, denom);
  t.constraint = machTicksFromMs(constraint, numer, denom);
  return t;
}

} // namespace detail

/**
 * Make the calling thread a real-time thread with these hints.
 *
 * macOS: thread_policy_set(THREAD_TIME_CONSTRAINT_POLICY) on this thread's
 * Mach port, which is what an audio workgroup expects its members to be.
 * Everywhere else: false, and `why` says this is a macOS mechanism -- a
 * Windows or Linux audio thread gets its priority from the audio API (MMCSS,
 * the JACK server), not from a call a helper thread can make here.
 */
inline bool promoteCurrentThreadToRealtime(const RealtimeThreadHints& hints,
                                           std::string* why = nullptr) {
  auto fail = [why](std::string text) {
    if (why) *why = std::move(text);
    return false;
  };
  if (!hints.isValid())
    return fail("the realtime hints are empty: give a period and a computation (forBlock)");
#if defined(__APPLE__)
  mach_timebase_info_data_t timebase{};
  const kern_return_t timebaseResult = mach_timebase_info(&timebase);
  if (timebaseResult != KERN_SUCCESS || timebase.numer == 0 || timebase.denom == 0)
    return fail("mach_timebase_info failed (" + std::to_string((int) timebaseResult) + ")");
  const detail::RealtimePolicyTicks ticks =
      detail::realtimePolicyTicks(hints, timebase.numer, timebase.denom);
  thread_time_constraint_policy_data_t policy{};
  policy.period = ticks.period;
  policy.computation = ticks.computation;
  policy.constraint = ticks.constraint;
  // Preemptible: a helper that overruns its computation is cut off rather
  // than holding a core against the IO thread it exists to help.
  policy.preemptible = 1;
  const kern_return_t result = thread_policy_set(
      pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY,
      (thread_policy_t) &policy, THREAD_TIME_CONSTRAINT_POLICY_COUNT);
  if (result != KERN_SUCCESS)
    return fail("thread_policy_set(THREAD_TIME_CONSTRAINT_POLICY) returned " +
                std::to_string((int) result));
  if (why) why->clear();
  return true;
#else
  return fail("realtime thread hints are Mach's time-constraint policy, macOS only; on this "
              "platform the audio API sets its own thread's priority and a helper thread is "
              "left as it was");
#endif
}

class WorkgroupToken;

/**
 * An audio workgroup: on macOS an os_workgroup_t, held by reference.
 *
 * Copying retains and destroying releases -- both atomic counts, no
 * allocation, so a copy is safe to make on the audio thread. An empty one
 * (the default, and the only kind on other platforms) is false, joins
 * nothing, and says why.
 */
class AudioWorkgroup {
public:
  /** Empty: false, and joins nothing. */
  AudioWorkgroup() noexcept = default;
  /** Releases its reference, if it holds one. */
  ~AudioWorkgroup() { reset(); }

  /** Another reference to the same group, retained; safe on the audio
   *  thread. */
  AudioWorkgroup(const AudioWorkgroup& other) noexcept : handle_(retainHandle(other.handle_)) {}
  /** Takes `other`'s reference; `other` is left empty. */
  AudioWorkgroup(AudioWorkgroup&& other) noexcept : handle_(other.handle_) {
    other.handle_ = nullptr;
  }
  /** Retains `other`'s group, then releases this one's. */
  AudioWorkgroup& operator=(const AudioWorkgroup& other) noexcept {
    if (this != &other) {
      void* retained = retainHandle(other.handle_);
      reset();
      handle_ = retained;
    }
    return *this;
  }
  /** Releases this one's group, then takes `other`'s reference; `other` is
   *  left empty. */
  AudioWorkgroup& operator=(AudioWorkgroup&& other) noexcept {
    if (this != &other) {
      reset();
      handle_ = other.handle_;
      other.handle_ = nullptr;
    }
    return *this;
  }

  /** An os_workgroup_t this call does not own -- a render context's -- held
   *  with a reference of its own. Empty if the OS cannot retain it. */
  static AudioWorkgroup retaining(void* osWorkgroup) noexcept {
    AudioWorkgroup w;
    w.handle_ = retainHandle(osWorkgroup);
    return w;
  }

  /** An os_workgroup_t handed over with a reference the caller owned -- what
   *  kAudioDevicePropertyIOThreadOSWorkgroup returns -- which this object now
   *  releases. */
  static AudioWorkgroup adopting(void* osWorkgroup) noexcept {
    AudioWorkgroup w;
    w.handle_ = osWorkgroup;
    return w;
  }

  /** Give the reference away: the handle is returned and this object is
   *  empty. For handing a workgroup between threads through an atomic, which
   *  ThreadPool::offerAudioWorkgroup does; take it back with adopting(). */
  void* detachHandle() noexcept {
    void* handle = handle_;
    handle_ = nullptr;
    return handle;
  }

  /** Release the reference and become empty. A thread still joined keeps
   *  its own reference, through its token. */
  void reset() noexcept {
    if (handle_) {
      const detail::OsWorkgroupApi& api = detail::osWorkgroupApi();
      if (api.release) api.release(handle_);
    }
    handle_ = nullptr;
  }

  /** True when it holds a group. */
  bool isValid() const noexcept { return handle_ != nullptr; }
  explicit operator bool() const noexcept { return isValid(); } ///< isValid().
  /** The os_workgroup_t, for code that talks to the OS itself. */
  void* nativeHandle() const noexcept { return handle_; }

  /** The same OS group, or both empty. */
  bool operator==(const AudioWorkgroup& other) const noexcept { return handle_ == other.handle_; }
  bool operator!=(const AudioWorkgroup& other) const noexcept { return handle_ != other.handle_; } ///< Not ==.

  /** How many threads the OS would run in parallel in this group -- roughly
   *  its performance cores. 0 when empty or unsupported. */
  int maxParallelThreadCount() const {
    const detail::OsWorkgroupApi& api = detail::osWorkgroupApi();
    if (!handle_ || !api.maxParallelThreads) return 0;
    const int count = api.maxParallelThreads(handle_, nullptr);
    return count > 0 ? count : 0;
  }

  /** True once the group's owner has cancelled it -- its device stopped or
   *  changed -- after which a join fails with EINVAL. */
  bool isCancelled() const {
    const detail::OsWorkgroupApi& api = detail::osWorkgroupApi();
    return handle_ && api.testCancel && api.testCancel(handle_);
  }

  /** Make the CALLING thread a member, recorded in `token`, which leaves any
   *  group it held first. False on failure, with the reason in the token. */
  bool join(WorkgroupToken& token) const;

  /** Whether this process has os_workgroup at all: macOS 11 or later. */
  static bool isSupported() { return detail::osWorkgroupApi().ok; }

  /** Null when supported; otherwise the reason, for a message. */
  static const char* unavailableReason() {
    if (isSupported()) return nullptr;
#if defined(__APPLE__)
    return "os_workgroup is not in this macOS (it arrived in macOS 11)";
#else
    return "audio workgroups are an Apple API (macOS 11 and later); this platform has none, so "
           "there is nothing to join";
#endif
  }

private:
  static void* retainHandle(void* handle) noexcept {
    if (!handle) return nullptr;
    const detail::OsWorkgroupApi& api = detail::osWorkgroupApi();
    return api.retain ? api.retain(handle) : nullptr;
  }

  void* handle_ = nullptr;
};

/**
 * One thread's membership of a workgroup.
 *
 * Filled by AudioWorkgroup::join() and ended by leave() or the destructor --
 * on the thread that joined, because that is the only thread the OS lets end
 * it. A leave from any other thread is refused and reported rather than
 * attempted. Neither copyable nor movable: it is the thread's, and it stays
 * where that thread put it.
 */
class WorkgroupToken {
public:
  /** Where the membership stands, and why a join failed. */
  enum class Status {
    kNotJoined,     ///< Nothing has been asked of it.
    kJoined,        ///< A member now.
    kLeft,          ///< Was a member, left cleanly.
    kUnsupported,   ///< No os_workgroup on this platform or this macOS.
    kNoWorkgroup,   ///< Asked to join an empty AudioWorkgroup.
    kCancelled,     ///< os_workgroup_join: EINVAL, the group was cancelled.
    kAlreadyMember, ///< os_workgroup_join: EALREADY, in another group already.
    kFailed,        ///< os_workgroup_join returned something else: errorCode().
  };

  /** Not joined. */
  WorkgroupToken() = default;
  WorkgroupToken(const WorkgroupToken&) = delete;            ///< The thread's own: not copyable.
  WorkgroupToken& operator=(const WorkgroupToken&) = delete; ///< The thread's own: not copyable.
  WorkgroupToken(WorkgroupToken&&) = delete;                 ///< Nor movable.
  WorkgroupToken& operator=(WorkgroupToken&&) = delete;      ///< Nor movable.

  /** Leaves the group if joined -- which only the joining thread can do. On
   *  another thread it says so on stderr and keeps the group's reference
   *  rather than release it under a member. */
  ~WorkgroupToken() {
    if (status_ != Status::kJoined) return;
    if (!leave()) {
      // Destroyed on a thread that did not join. Leaving from here is not
      // something the OS allows, and releasing the group under a member
      // thread is worse than keeping one reference too many, so the
      // reference is kept and the mistake is said out loud.
      std::fprintf(stderr,
                   "[sonore] WorkgroupToken destroyed on a thread that did not join; the joining "
                   "thread is still a member of its audio workgroup\n");
      (void) workgroup_.detachHandle();
    }
  }

  /** True while the thread is a member. */
  bool isActive() const { return status_ == Status::kJoined; }
  /** True while a member of `workgroup` in particular. */
  bool isAttachedTo(const AudioWorkgroup& workgroup) const {
    return isActive() && workgroup_ == workgroup;
  }
  /** Where it stands: joined, left, or why the join failed. */
  Status status() const { return status_; }
  /** What os_workgroup_join returned, when it failed. */
  int errorCode() const { return error_; }

  /** End the membership. True if there was one and this thread ended it;
   *  false if there was none, or this is not the thread that joined. */
  bool leave() {
    if (status_ != Status::kJoined) return false;
    if (thread_ != std::this_thread::get_id()) return false;
    const detail::OsWorkgroupApi& api = detail::osWorkgroupApi();
    if (api.leave) api.leave(workgroup_.nativeHandle(), storage_);
    workgroup_.reset();
    status_ = Status::kLeft;
    return true;
  }

  /** The state in words, for a log line or a test. */
  const char* describe() const {
    switch (status_) {
      case Status::kNotJoined: return "not joined";
      case Status::kJoined: return "joined the audio workgroup";
      case Status::kLeft: return "left the audio workgroup";
      case Status::kUnsupported: {
        const char* reason = AudioWorkgroup::unavailableReason();
        return reason ? reason : "os_workgroup is unavailable";
      }
      case Status::kNoWorkgroup:
        return "no workgroup to join: the host or the device did not provide one";
      case Status::kCancelled:
        return "the workgroup was cancelled (os_workgroup_join returned EINVAL): its device "
               "stopped or changed";
      case Status::kAlreadyMember:
        return "this thread already belongs to another workgroup (os_workgroup_join returned "
               "EALREADY)";
      case Status::kFailed: return "os_workgroup_join failed; see errorCode()";
    }
    return "unknown";
  }

private:
  friend class AudioWorkgroup;
  alignas(8) unsigned char storage_[detail::kWorkgroupTokenBytes] = {};
  AudioWorkgroup workgroup_;
  std::thread::id thread_;
  Status status_ = Status::kNotJoined;
  int error_ = 0;
};

inline bool AudioWorkgroup::join(WorkgroupToken& token) const {
  // One membership per token. A token still joined elsewhere is ended first
  // -- on this thread, so if it was joined on another this is refused there
  // and the new join is refused here, rather than overwriting a live token.
  if (token.status_ == WorkgroupToken::Status::kJoined && !token.leave()) {
    token.error_ = EALREADY;
    return false;
  }
  token.error_ = 0;
  const detail::OsWorkgroupApi& api = detail::osWorkgroupApi();
  if (!api.ok) {
    token.status_ = WorkgroupToken::Status::kUnsupported;
    return false;
  }
  if (!handle_) {
    token.status_ = WorkgroupToken::Status::kNoWorkgroup;
    return false;
  }
  std::memset(token.storage_, 0, sizeof(token.storage_));
  const int result = api.join(handle_, token.storage_);
  if (result != 0) {
    token.error_ = result;
    token.status_ = result == EINVAL     ? WorkgroupToken::Status::kCancelled
                    : result == EALREADY ? WorkgroupToken::Status::kAlreadyMember
                                         : WorkgroupToken::Status::kFailed;
    return false;
  }
  token.workgroup_ = *this; // a retain: no allocation
  token.thread_ = std::this_thread::get_id();
  token.status_ = WorkgroupToken::Status::kJoined;
  return true;
}

namespace detail {
/** Does the DSP want to be told its workgroup? `void workgroupChanged(const
 *  sonore::AudioWorkgroup&)`, detected like latencySamples() and the rest. */
template <typename T, typename = void>
struct HasWorkgroupHook : std::false_type {};
template <typename T>
struct HasWorkgroupHook<
    T, std::void_t<decltype(std::declval<T&>().workgroupChanged(std::declval<const AudioWorkgroup&>()))>>
    : std::true_type {};
} // namespace detail

/**
 * Tell a DSP its workgroup, if it asked.
 *
 * Never beside a process() call: the standalone calls it with audio stopped,
 * the AU wrapper from the OS's observer on the render thread just before a
 * render. So a DSP's workgroupChanged() may run on the audio thread and must
 * not allocate or block -- which ThreadPool::offerAudioWorkgroup() is built
 * for.
 */
template <typename T>
inline void dspWorkgroupChanged(T& dsp, const AudioWorkgroup& workgroup) {
  if constexpr (detail::HasWorkgroupHook<T>::value) {
    dsp.workgroupChanged(workgroup);
  } else {
    (void) dsp;
    (void) workgroup;
  }
}

} // namespace sonore
