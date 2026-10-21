// SPDX-License-Identifier: Apache-2.0
//
// VBlankAttachment: a callback once per display refresh, on the UI thread.
//
// ── Why a plugin wants the refresh, when it has a 33 ms tick ────────────────
//
// The editor's own tick is the right clock for following automation and for
// meters that do not need to be smoother than the eye can track a number. It
// is the wrong clock for MOTION: a scope trace, a spectrum, a momentum scroll,
// anything that animates continuously. 30 Hz against a 60 Hz display shows
// every frame twice, and a tick that is not locked to the display drifts
// against it and judders -- a frame shown three times, then once. Drawing once
// per refresh, when the refresh happens, is the only way motion looks smooth.
//
// ── How each platform hears the refresh ─────────────────────────────────────
//
//   Windows  A worker thread waits on the display -- IDXGIOutput::WaitForVBlank
//            on the primary output, or DwmFlush where DXGI is unavailable, or a
//            sleep at the monitor's rate as a last resort -- and posts ONE
//            message to a message-only window on the UI thread. One: a flag
//            stays set until the UI thread has handled it, so a UI thread that
//            falls behind sees one late frame rather than a queue of them.
//            dxgi.dll and dwmapi.dll are loaded at run time, never linked.
//   macOS    CVDisplayLink, whose callback runs on its own thread, hops to the
//            main queue with dispatch_async_f under the same one-pending rule.
//            CoreVideo and libdispatch are looked up at run time. Compiled
//            against the shim, never run.
//   X11      No display link. A timer at the monitor's refresh rate (XRandR's
//            current rate, 60 Hz if it cannot be read), checked each time the
//            X11 peer pumps its events -- so the callback rate is the lower of
//            the monitor's and however often the host pumps the window. Said
//            plainly because it is a real limit: an X11 plugin has no thread of
//            its own on which to run a timer that may touch the UI.
//
// ── What an attachment is attached to ───────────────────────────────────────
//
// A component, as in the reference: the callback runs only while that
// component is in a window (routerFor() finds one), so an editor that is
// closed but not yet destroyed is not animated for nobody. A null component
// means "always".
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <thread>
#include <vector>

#include "component.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dxgi.h>
#elif defined(__APPLE__) || defined(SONORE_APPLE_SYNTAX_CHECK)
#include <dlfcn.h>
#else
#include <dlfcn.h>
#endif

namespace sonore {
namespace gfx {

class VBlankAttachment;

namespace detail {

/**
 * The one source of refreshes in the process, and the attachments it serves.
 *
 * Process-wide because the display is: two editors open in one host share a
 * monitor and need one thread waiting on it, not two.
 */
class VBlankDispatcher {
public:
  static VBlankDispatcher& instance() {
    static VBlankDispatcher d;
    return d;
  }

  void add(VBlankAttachment* a) {
    if (std::find(attached_.begin(), attached_.end(), a) != attached_.end()) return;
    attached_.push_back(a);
    if (attached_.size() == 1) start();
  }

  void remove(VBlankAttachment* a) {
    attached_.erase(std::remove(attached_.begin(), attached_.end(), a), attached_.end());
    if (attached_.empty()) stop();
  }

  /** Run every attachment's callback. UI thread only. */
  inline void deliver(double timestamp);

  /** The display's refresh rate in Hz, as best this platform can say. */
  static double refreshRate() {
#if defined(_WIN32)
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode) && mode.dmDisplayFrequency > 1)
      return (double) mode.dmDisplayFrequency;
    return 60.0;
#elif defined(__APPLE__) || defined(SONORE_APPLE_SYNTAX_CHECK)
    return macRefreshRate();
#else
    return x11RefreshRate();
#endif
  }

  /** What is producing the refreshes: "dxgi", "dwm", "timer", "cvdisplaylink",
   *  "x11-timer", or "none" while nothing is attached. */
  const char* backendName() const { return backend_.load(); }

  /** The X11 timer: deliver if a refresh period has passed since the last
   *  delivery. Called by the X11 peer on every pump; harmless elsewhere. */
  void pump() {
#if !defined(_WIN32) && !(defined(__APPLE__) || defined(SONORE_APPLE_SYNTAX_CHECK))
    if (attached_.empty()) return;
    const double now = seconds();
    if (period_ <= 0.0) period_ = 1.0 / refreshRate();
    if (now - lastPump_ < period_ * 0.98) return;
    // Phase-locked to the period rather than to whenever the pump came, so a
    // pump that arrives late does not push every later frame later too.
    lastPump_ = now - std::fmod(now - lastPump_, period_);
    if (now - lastPump_ > period_) lastPump_ = now;
    deliver(now);
#endif
  }

  static double seconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
  }

private:
  VBlankDispatcher() = default;
  ~VBlankDispatcher() { stop(); }

#if defined(_WIN32)
  static constexpr UINT kVBlankMessage = WM_APP + 0x5B1;

  static LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == kVBlankMessage) {
      VBlankDispatcher& d = instance();
      LARGE_INTEGER frequency;
      QueryPerformanceFrequency(&frequency);
      const double stamp = (double) d.stampTicks_.load() / (double) frequency.QuadPart;
      d.pending_.store(false);
      d.deliver(stamp);
      return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
  }

  void start() {
    if (!window_) {
      WNDCLASSW wc{};
      wc.lpfnWndProc = &VBlankDispatcher::windowProc;
      wc.hInstance = GetModuleHandleW(nullptr);
      // Unique to this module: two plugins built on the SDK in one host each
      // register their own class, with their own procedure.
      wchar_t name[64];
      wsprintfW(name, L"SonoreVBlank_%p", (void*) &VBlankDispatcher::windowProc);
      wc.lpszClassName = name;
      RegisterClassW(&wc);
      window_ = CreateWindowExW(0, name, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                wc.hInstance, nullptr);
    }
    if (!window_) return;
    running_.store(true);
    pending_.store(false);
    backend_ = "starting";
    HWND target = window_;
    worker_ = std::thread([this, target] { waitLoop(target); });
  }

  void stop() {
    if (worker_.joinable()) {
      running_.store(false);
      worker_.join();
    }
    // The window goes too, with the last attachment. Kept, it outlived the
    // plugin: a host unloading the DLL after closing its editor was left a
    // window whose procedure was in the unloaded code, and the next message
    // to it -- the refresh the worker had already posted, or anyone's --
    // crashed the host. A message still queued for it finds no window and
    // is dropped by DispatchMessage.
    if (HWND window = window_) {
      window_ = nullptr;
      if (GetWindowThreadProcessId(window, nullptr) == GetCurrentThreadId()) {
        DestroyWindow(window);
      } else {
        // Only its own thread can destroy it: from any other, nothing in
        // this module is left for it to call, and its thread is asked.
        SetWindowLongPtrW(window, GWLP_WNDPROC, (LONG_PTR) &DefWindowProcW);
        PostMessageW(window, WM_CLOSE, 0, 0);
      }
    }
    backend_ = "none";
  }

  /** On the worker: wait for each refresh and post one message per refresh
   *  the UI thread has not yet handled. */
  void waitLoop(HWND target) {
    using CreateFactory = HRESULT(WINAPI*)(REFIID, void**);
    using Flush = HRESULT(WINAPI*)();
    HMODULE dxgi = LoadLibraryExW(L"dxgi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    HMODULE dwm = LoadLibraryExW(L"dwmapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    auto create = dxgi ? reinterpret_cast<CreateFactory>(
                             reinterpret_cast<void*>(GetProcAddress(dxgi, "CreateDXGIFactory1")))
                       : nullptr;
    auto flush = dwm ? reinterpret_cast<Flush>(reinterpret_cast<void*>(GetProcAddress(dwm, "DwmFlush")))
                     : nullptr;

    IDXGIOutput* output = nullptr;
    if (create) {
      IDXGIFactory1* factory = nullptr;
      if (SUCCEEDED(create(__uuidof(IDXGIFactory1), (void**) &factory)) && factory) {
        // The output showing the primary monitor, or failing that the first.
        const HMONITOR primary = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
        IDXGIOutput* first = nullptr;
        IDXGIAdapter1* adapter = nullptr;
        for (UINT a = 0; !output && factory->EnumAdapters1(a, &adapter) == S_OK; ++a) {
          IDXGIOutput* candidate = nullptr;
          for (UINT o = 0; !output && adapter->EnumOutputs(o, &candidate) == S_OK; ++o) {
            DXGI_OUTPUT_DESC desc{};
            if (SUCCEEDED(candidate->GetDesc(&desc)) && desc.Monitor == primary) output = candidate;
            else if (!first) first = candidate;
            else candidate->Release();
          }
          adapter->Release();
        }
        if (!output) output = first;
        else if (first) first->Release();
        factory->Release();
      }
    }

    const double period = 1.0 / refreshRate();
    int failures = 0;
    while (running_.load()) {
      bool waited = false;
      if (output && failures < 3) {
        if (SUCCEEDED(output->WaitForVBlank())) {
          backend_ = "dxgi";
          waited = true;
          failures = 0;
        } else {
          ++failures;
        }
      }
      if (!waited && flush && failures < 6) {
        // DwmFlush returns at the compositor's next present, which is its
        // refresh -- and fails when composition is off.
        if (SUCCEEDED(flush())) {
          backend_ = "dwm";
          waited = true;
        } else {
          ++failures;
        }
      }
      if (!waited) {
        backend_ = "timer";
        Sleep((DWORD) (period * 1000.0 + 0.5));
      }
      LARGE_INTEGER now;
      QueryPerformanceCounter(&now);
      stampTicks_.store(now.QuadPart);
      if (!pending_.exchange(true)) PostMessageW(target, kVBlankMessage, 0, 0);
    }
    if (output) output->Release();
    if (dxgi) FreeLibrary(dxgi);
    if (dwm) FreeLibrary(dwm);
  }

  HWND window_ = nullptr;
  std::thread worker_;
  std::atomic<bool> running_{false};
  std::atomic<bool> pending_{false};
  std::atomic<long long> stampTicks_{0};

#elif defined(__APPLE__) || defined(SONORE_APPLE_SYNTAX_CHECK)

  // CoreVideo's types, as opaque as Apple's own headers make them.
  struct CVTimeStamp; // only ever pointed to
  using CVDisplayLinkRef = void*;
  using CVReturn = int32_t;
  using CVOptionFlags = uint64_t;
  using OutputCallback = CVReturn (*)(CVDisplayLinkRef, const CVTimeStamp*, const CVTimeStamp*,
                                      CVOptionFlags, CVOptionFlags*, void*);
  struct CVTime {
    int64_t timeValue;
    int32_t timeScale;
    int32_t flags;
  };

  struct CoreVideo {
    CVReturn (*create)(CVDisplayLinkRef*) = nullptr;
    CVReturn (*setCallback)(CVDisplayLinkRef, OutputCallback, void*) = nullptr;
    CVReturn (*startLink)(CVDisplayLinkRef) = nullptr;
    CVReturn (*stopLink)(CVDisplayLinkRef) = nullptr;
    void (*release)(CVDisplayLinkRef) = nullptr;
    CVTime (*nominalPeriod)(CVDisplayLinkRef) = nullptr;
    void (*asyncF)(void*, void*, void (*)(void*)) = nullptr;
    void* mainQueue = nullptr;
    bool ok() const {
      return create && setCallback && startLink && stopLink && release && asyncF && mainQueue;
    }
  };

  static const CoreVideo& coreVideo() {
    static CoreVideo api = [] {
      CoreVideo a;
      void* cv = dlopen("/System/Library/Frameworks/CoreVideo.framework/CoreVideo", RTLD_LAZY);
      if (cv) {
        a.create = reinterpret_cast<CVReturn (*)(CVDisplayLinkRef*)>(
            dlsym(cv, "CVDisplayLinkCreateWithActiveCGDisplays"));
        a.setCallback = reinterpret_cast<CVReturn (*)(CVDisplayLinkRef, OutputCallback, void*)>(
            dlsym(cv, "CVDisplayLinkSetOutputCallback"));
        a.startLink = reinterpret_cast<CVReturn (*)(CVDisplayLinkRef)>(dlsym(cv, "CVDisplayLinkStart"));
        a.stopLink = reinterpret_cast<CVReturn (*)(CVDisplayLinkRef)>(dlsym(cv, "CVDisplayLinkStop"));
        a.release = reinterpret_cast<void (*)(CVDisplayLinkRef)>(dlsym(cv, "CVDisplayLinkRelease"));
        a.nominalPeriod = reinterpret_cast<CVTime (*)(CVDisplayLinkRef)>(
            dlsym(cv, "CVDisplayLinkGetNominalOutputVideoRefreshPeriod"));
      }
      // dispatch_get_main_queue() is a macro for &_dispatch_main_q.
      a.asyncF = reinterpret_cast<void (*)(void*, void*, void (*)(void*))>(
          dlsym(RTLD_DEFAULT, "dispatch_async_f"));
      a.mainQueue = dlsym(RTLD_DEFAULT, "_dispatch_main_q");
      return a;
    }();
    return api;
  }

  static double macRefreshRate() {
    const CoreVideo& cv = coreVideo();
    if (!cv.create || !cv.nominalPeriod || !cv.release) return 60.0;
    CVDisplayLinkRef link = nullptr;
    if (cv.create(&link) != 0 || !link) return 60.0;
    const CVTime t = cv.nominalPeriod(link);
    cv.release(link);
    return (t.timeValue > 0 && t.timeScale > 0) ? (double) t.timeScale / (double) t.timeValue : 60.0;
  }

  /** On CoreVideo's thread. */
  static CVReturn displayLinkFired(CVDisplayLinkRef, const CVTimeStamp*, const CVTimeStamp*,
                                   CVOptionFlags, CVOptionFlags* flagsOut, void*) {
    if (flagsOut) *flagsOut = 0;
    VBlankDispatcher& d = instance();
    d.stampSeconds_.store(seconds());
    if (!d.pending_.exchange(true)) coreVideo().asyncF(coreVideo().mainQueue, &d, &onMainQueue);
    return 0;
  }

  /** On the main queue. */
  static void onMainQueue(void* context) {
    auto* d = static_cast<VBlankDispatcher*>(context);
    d->pending_.store(false);
    d->deliver(d->stampSeconds_.load());
  }

  void start() {
    const CoreVideo& cv = coreVideo();
    if (!cv.ok() || link_) return;
    if (cv.create(&link_) != 0 || !link_) {
      link_ = nullptr;
      return;
    }
    cv.setCallback(link_, &displayLinkFired, this);
    cv.startLink(link_);
    backend_ = "cvdisplaylink";
  }

  void stop() {
    const CoreVideo& cv = coreVideo();
    if (link_) {
      cv.stopLink(link_);
      cv.release(link_);
      link_ = nullptr;
    }
    backend_ = "none";
  }

  CVDisplayLinkRef link_ = nullptr;
  std::atomic<bool> pending_{false};
  std::atomic<double> stampSeconds_{0.0};

#else // X11 and anything else: a timer, pumped by the peer

  /** XRandR's current rate, from libX11 and libXrandr opened here -- by
   *  their C signatures with opaque pointers, so this needs neither header. */
  static double x11RefreshRate() {
    static double rate = [] {
      void* x11 = dlopen("libX11.so.6", RTLD_LAZY);
      void* xrr = dlopen("libXrandr.so.2", RTLD_LAZY);
      double r = 60.0;
      if (x11 && xrr) {
        auto open = reinterpret_cast<void* (*)(const char*)>(dlsym(x11, "XOpenDisplay"));
        auto close = reinterpret_cast<int (*)(void*)>(dlsym(x11, "XCloseDisplay"));
        auto rootOf = reinterpret_cast<unsigned long (*)(void*)>(dlsym(x11, "XDefaultRootWindow"));
        auto info = reinterpret_cast<void* (*)(void*, unsigned long)>(dlsym(xrr, "XRRGetScreenInfo"));
        auto current = reinterpret_cast<short (*)(void*)>(dlsym(xrr, "XRRConfigCurrentRate"));
        auto release = reinterpret_cast<void (*)(void*)>(dlsym(xrr, "XRRFreeScreenConfigInfo"));
        if (open && close && rootOf && info && current && release)
          if (void* display = open(nullptr)) {
            if (void* config = info(display, rootOf(display))) {
              const short hz = current(config);
              if (hz > 1) r = (double) hz;
              release(config);
            }
            close(display);
          }
      }
      return r;
    }();
    return rate;
  }

  void start() {
    backend_ = "x11-timer";
    period_ = 1.0 / refreshRate();
    lastPump_ = seconds();
  }
  void stop() { backend_ = "none"; }

  double period_ = 0.0;
  double lastPump_ = 0.0;
#endif

  std::vector<VBlankAttachment*> attached_;
  // Atomic: on Windows the worker thread writes it as it finds out which
  // wait works, while the UI thread may be reading it.
  std::atomic<const char*> backend_{"none"};
};

} // namespace detail

/**
 * Call `callback` once per display refresh, on the UI thread, for as long as
 * this object lives and `component` is in a window.
 *
 * The argument is the refresh's timestamp in seconds -- the same clock for
 * every attachment, so two animations driven by it agree about the time.
 */
class VBlankAttachment {
public:
  /** Attached to nothing: never fires, and isEmpty() is true. It cannot be
   *  filled in later (the class is neither copyable nor movable), so a member
   *  that starts empty is held in a std::unique_ptr or std::optional. */
  VBlankAttachment() = default;

  /**
   * Call `callback` with each refresh's timestamp, in seconds, while
   * `component` (not owned) is in a window; a null component means always.
   * A component already being destroyed gives an attachment that never
   * fires, and an empty callback attaches nothing. The first attachment in
   * the process starts the platform's wait. UI thread only.
   */
  VBlankAttachment(Component* component, std::function<void(double)> callback)
      : component_(component), callback_(std::move(callback)) {
    guard_.owner = this;
    if (component_ && !component_->addComponentListener(&guard_)) {
      component_ = nullptr; // being destroyed: never fires
      orphaned_ = true;
    }
    if (callback_) detail::VBlankDispatcher::instance().add(this);
  }

  /** For a caller with no use for the timestamp. */
  VBlankAttachment(Component* component, std::function<void()> callback)
      : VBlankAttachment(component, callback ? std::function<void(double)>(
                                                   [callback](double) { callback(); })
                                             : std::function<void(double)>()) {}

  /** No more calls; the last attachment in the process stops the wait. It
   *  may be destroyed from inside its own callback. */
  ~VBlankAttachment() { detail::VBlankDispatcher::instance().remove(this); }

  /** Not copyable: the dispatcher holds this object's address. */
  VBlankAttachment(const VBlankAttachment&) = delete;
  /** Not assignable, for the same reason. */
  VBlankAttachment& operator=(const VBlankAttachment&) = delete;

  /** Made without a callback: it never fires. */
  bool isEmpty() const { return !callback_; }

  /** The refresh rate of the display, in Hz. */
  static double displayRefreshRate() { return detail::VBlankDispatcher::refreshRate(); }

  /** Which mechanism is producing refreshes right now -- see the header
   *  comment. "none" while nothing is attached. */
  static const char* backendName() { return detail::VBlankDispatcher::instance().backendName(); }

  /** The X11 peer's pump; see detail::VBlankDispatcher::pump. */
  static void pumpTimerFallback() { detail::VBlankDispatcher::instance().pump(); }

private:
  friend class detail::VBlankDispatcher;

  /** Listens to the attached component, so one destroyed first leaves the
   *  attachment orphaned -- alive, never firing -- instead of holding a
   *  dangling pointer. Detaches itself when the attachment goes. */
  struct Guard : ComponentListener {
    VBlankAttachment* owner = nullptr;
    void componentBeingDeleted(Component& c) override {
      // A component destroyed before its attachment: the attachment stays
      // but never fires again, as the reference's does.
      if (owner->component_ == &c) {
        owner->component_ = nullptr;
        owner->orphaned_ = true;
      }
    }
  };

  void fire(double timestamp) {
    if (!callback_ || orphaned_) return;
    if (component_ && routerFor(component_) == nullptr) return; // not in a window
    // From a COPY, so the callback may destroy this attachment -- the usual
    // way an animation that has finished stops asking for frames. Nothing
    // below the call touches a member.
    const std::function<void(double)> run = callback_;
    run(timestamp);
  }

  Component* component_ = nullptr;
  std::function<void(double)> callback_;
  Guard guard_;
  bool orphaned_ = false;
};

inline void detail::VBlankDispatcher::deliver(double timestamp) {
  // Over a snapshot, each checked before its call: a callback may destroy its
  // own attachment, or another.
  const std::vector<VBlankAttachment*> told = attached_;
  for (VBlankAttachment* a : told)
    if (std::find(attached_.begin(), attached_.end(), a) != attached_.end()) a->fire(timestamp);
}

} // namespace gfx
} // namespace sonore
