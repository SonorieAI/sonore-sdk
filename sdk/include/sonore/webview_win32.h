// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: the Windows webview backend (Edge WebView2).
//
// Creates a child HWND the host can embed, hosts a WebView2 inside it, injects
// the `window.sonore` bridge before any page script runs, and pumps parameter
// and meter updates into the page on a timer.
//
// Design notes that are load-bearing:
//
//  - WebView2 comes up ASYNCHRONOUSLY (environment -> controller -> webview,
//    each via a COM completion handler). Every public method therefore has to
//    work BEFORE the webview exists: HTML, size and pending updates are stored
//    and applied on arrival. A DAW opens and closes editors far faster than
//    Edge initialises, so "not ready yet" is the normal case, not an edge one.
//
//  - The runtime may be ABSENT (older Windows without the Edge WebView2
//    Runtime). That degrades to a plain message in the child window: never a
//    failed plugin load, and never a silent black rectangle.
//
//  - The webview is created with a per-plugin user-data folder under %LOCALAPPDATA%.
//    Without an explicit folder WebView2 tries to write beside the host EXE,
//    which fails in Program Files and takes the whole editor down with it.
//
//  - The page is served with NavigateToString, so a generated UI is entirely
//    self-contained. There is no network access and no local file exposure.
#pragma once

#if defined(_WIN32)

// windows.h defines min and max as MACROS, which turn any later std::min or
// std::max into a syntax error -- and only when this header happens to be
// included FIRST, which makes it an include-order landmine rather than a bug.
// WIN32_LEAN_AND_MEAN keeps winsock, RPC, OLE and the shell out.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>
#include <shlwapi.h>

#include <atomic>
#include <functional>
#include <cwchar>
#include <memory>
#include <string>
#include <vector>

#include "gui.h"

#if defined(SONORE_HAS_WEBVIEW2)
#include <WebView2.h>
#endif

namespace sonore {
namespace win32 {

inline std::wstring widen(const std::string& s) {
  if (s.empty()) return std::wstring();
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int) s.size(), nullptr, 0);
  std::wstring out((size_t) n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int) s.size(), out.data(), n);
  return out;
}

inline std::string narrow(const wchar_t* s) {
  if (!s) return std::string();
  const int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
  if (n <= 1) return std::string();
  std::string out((size_t) (n - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, s, -1, out.data(), n, nullptr, nullptr);
  return out;
}

inline LRESULT CALLBACK hostWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

/** THIS module, not the host EXE.
 *
 *  GetModuleHandle(nullptr) answers with whatever EXE happens to be running
 *  us, which for a plugin is the DAW. Registering a window class against the
 *  DAW is wrong twice over: the class outlives this module, so unloading the
 *  plugin leaves a class whose window procedure points into freed code, and
 *  the class is shared with every other module in the process.
 *
 *  A class registered against a DLL's own handle is released by Windows when
 *  that DLL unloads, which is exactly the lifetime wanted. */
inline HINSTANCE thisModule() {
  static HINSTANCE instance = [] {
    HMODULE module = nullptr;
    // The address of a function in this header. It is inline, so every module
    // that includes it has a copy of its own and each finds itself.
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR) &hostWndProc, &module);
    return (HINSTANCE) module;
  }();
  return instance;
}

/** The child window class name, unique to THIS module.
 *
 *  A fixed name was a bug with a very specific shape: the first Sonore plugin
 *  in a session registered it and worked, and the second -- a different .clap
 *  or .vst3, in the same DAW process -- got ERROR_CLASS_ALREADY_EXISTS,
 *  failed to make its child window, and showed the user an empty editor. One
 *  plugin looked fine; two did not, which is every real session.
 *
 *  The module's base address makes it unique, is stable for as long as the
 *  module is loaded -- but it is a STARTING POINT, not a guarantee. Modules
 *  are unloaded and reloaded over a session and a later one can be mapped
 *  where an earlier one used to live, so the name is settled by registration
 *  rather than assumed, and only once RegisterClassExW has accepted it. */
inline std::wstring& windowClassNameStorage() {
  static std::wstring name;
  return name;
}

inline const wchar_t* windowClassName() { return windowClassNameStorage().c_str(); }

inline ATOM registerWindowClass() {
  static ATOM atom = 0;
  if (atom) return atom;
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.style = CS_HREDRAW | CS_VREDRAW;
  wc.lpfnWndProc = hostWndProc;
  wc.hInstance = thisModule();
  // MAKEINTRESOURCEW, not IDC_ARROW: that macro follows the project's UNICODE
  // setting and yields an LPSTR in ANSI builds, which LoadCursorW rejects. A
  // plugin is compiled inside somebody else's build, so never assume either.
  wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); // IDC_ARROW
  // A dark ground so an editor never flashes white before the page paints.
  static HBRUSH brush = CreateSolidBrush(RGB(13, 16, 20));
  wc.hbrBackground = brush;
  // TAKEN, not assumed. Treating an existing class of the same name as good
  // enough is how a window ends up running somebody else's window procedure,
  // and if that somebody has since been unloaded, running freed code -- which
  // is exactly the crash that came of it: NtUserCreateWindowEx dispatching
  // into an address in a module that no longer had a function there.
  //
  // A collision means take another name, never share one.
  for (int attempt = 0; attempt < 64 && !atom; ++attempt) {
    wchar_t buffer[80];
    if (attempt == 0)
      std::swprintf(buffer, 80, L"SonoreWebViewHost_%p", (void*) thisModule());
    else
      std::swprintf(buffer, 80, L"SonoreWebViewHost_%p_%d", (void*) thisModule(), attempt);
    windowClassNameStorage() = buffer;
    wc.lpszClassName = windowClassNameStorage().c_str();
    atom = RegisterClassExW(&wc);
    if (!atom && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) break;
  }
  if (!atom) windowClassNameStorage().clear();
  return atom;
}

#if defined(SONORE_HAS_WEBVIEW2)

/** One-method COM callback, refcounted. WebView2's creation API hands results
 *  back through these; a template keeps the three we need to one implementation. */
template <typename Interface, typename Arg1, typename Arg2>
class ComCallback : public Interface {
public:
  using Fn = std::function<HRESULT(Arg1, Arg2)>;
  explicit ComCallback(Fn fn) : fn_(std::move(fn)) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
    if (!ppv) return E_POINTER;
    if (riid == IID_IUnknown || riid == __uuidof(Interface)) {
      *ppv = static_cast<Interface*>(this);
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG n = --refs_;
    if (n == 0) delete this;
    return n;
  }
  HRESULT STDMETHODCALLTYPE Invoke(Arg1 a, Arg2 b) override { return fn_(a, b); }

private:
  Fn fn_;
  std::atomic<ULONG> refs_{1};
};

using EnvHandler = ComCallback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler,
                               HRESULT, ICoreWebView2Environment*>;
using ControllerHandler = ComCallback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler,
                                      HRESULT, ICoreWebView2Controller*>;
using SuspendHandler = ComCallback<ICoreWebView2TrySuspendCompletedHandler, HRESULT, BOOL>;
using MessageHandler = ComCallback<ICoreWebView2WebMessageReceivedEventHandler,
                                   ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs*>;
using KeyHandler = ComCallback<ICoreWebView2AcceleratorKeyPressedEventHandler,
                               ICoreWebView2Controller*,
                               ICoreWebView2AcceleratorKeyPressedEventArgs*>;

#endif // SONORE_HAS_WEBVIEW2

/**
 * A webview embedded in a child window.
 *
 * Lifetime: create() makes the HWND immediately (so the host can embed it right
 * away) and kicks off WebView2 creation; the page arrives later. destroy()
 * tears everything down in the reverse order.
 */
class WebViewHost {
public:
  /** Called on the MAIN thread when the page sends a bridge message. */
  std::function<void(const BridgeMessage&)> onMessage;

  /**
   * Whether the PAGE wants the keyboard.
   *
   * False by default, and that default is the whole point. A webview with
   * focus swallows every keystroke, so a plugin whose editor is open eats the
   * spacebar -- and the spacebar is transport in every DAW there is. The user
   * clicks the plugin, presses play, and nothing happens; nothing looks
   * broken, and the plugin gets blamed.
   *
   * So keys are handed BACK to the host unless a page says it needs them,
   * which it does while a text field has focus. A page that never asks
   * behaves like a plugin with no text entry, which is most of them.
   */
  bool captureKeys = false;

  ~WebViewHost() { destroy(); }

  bool create(HWND parent, uint32_t width, uint32_t height, std::string html,
              std::string bridge, std::string userDataName) {
    if (hwnd_) return true;
    width_ = width;
    height_ = height;
    html_ = std::move(html);
    bridge_ = std::move(bridge);
    userDataName_ = std::move(userDataName);

    // WebView2 is COM, and COM needs this thread to be in a single-threaded
    // apartment. A DAW's main thread already is, in which case this returns
    // S_FALSE and we still balance it; RPC_E_CHANGED_MODE means the thread is
    // deliberately in a different apartment and must NOT be uninitialised by us.
    const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    comInitialised_ = SUCCEEDED(co);

    if (!registerWindowClass()) return false;
    hwnd_ = CreateWindowExW(0, windowClassName(), L"", WS_CHILD | WS_CLIPCHILDREN | WS_VISIBLE,
                            0, 0, (int) width_, (int) height_, parent, nullptr, thisModule(),
                            nullptr);
    if (!hwnd_) return false;
    SetWindowLongPtrW(hwnd_, GWLP_USERDATA, (LONG_PTR) this);

#if defined(SONORE_HAS_WEBVIEW2)
    startWebView();
#else
    setStatusText("This build has no webview backend.");
#endif
    // The UI clock: parameter echoes and meters, ~30 Hz. Cheap, and it stops
    // the moment the editor closes.
    SetTimer(hwnd_, kTimerId, 33, nullptr);
    return true;
  }

  void destroy() {
    // First, before anything is released: any handler that has not run yet
    // must find out it is too late while this object is still whole.
    alive_->store(false);
    alive_ = std::make_shared<std::atomic<bool>>(true);
    if (hwnd_) KillTimer(hwnd_, kTimerId);
#if defined(SONORE_HAS_WEBVIEW2)
    if (controller_) {
      controller_->Close();
      controller_->Release();
      controller_ = nullptr;
    }
    if (webview_) {
      webview_->Release();
      webview_ = nullptr;
    }
#endif
    if (hwnd_) {
      DestroyWindow(hwnd_);
      hwnd_ = nullptr;
    }
    ready_ = false;
    if (comInitialised_) {
      CoUninitialize();
      comInitialised_ = false;
    }
  }

  /**
   * Hand the host back the keys the page does not want.
   *
   * WebView2 raises AcceleratorKeyPressed for exactly the keys this is about
   * -- space, tab, escape, function and arrow keys -- BEFORE the page sees
   * them. Marking one handled stops the browser acting on it, and reposting
   * it to the window above ours puts it where a DAW's key bindings live.
   *
   * Only when the page has not asked for the keyboard. A page editing text
   * needs its space bar, and the two cannot both be true at once.
   */
  void installKeyForwarding() {
#if defined(SONORE_HAS_WEBVIEW2)
    if (!controller_) return;
    auto* handler = new KeyHandler(
        [this](ICoreWebView2Controller*,
               ICoreWebView2AcceleratorKeyPressedEventArgs* args) -> HRESULT {
          if (!args || captureKeys) return S_OK;
          COREWEBVIEW2_KEY_EVENT_KIND kind = COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN;
          UINT key = 0;
          args->get_KeyEventKind(&kind);
          args->get_VirtualKey(&key);
          const bool down = kind == COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN ||
                            kind == COREWEBVIEW2_KEY_EVENT_KIND_SYSTEM_KEY_DOWN;
          args->put_Handled(TRUE);
          // To the window ABOVE the plugin's, which is the host's. Posting
          // rather than sending: this is inside the webview's own event
          // handler, and a DAW that opened a dialog in response would be
          // re-entering it.
          if (HWND parent = GetParent(hwnd_))
            PostMessageW(parent, down ? WM_KEYDOWN : WM_KEYUP, (WPARAM) key, 0);
          return S_OK;
        });
    EventRegistrationToken token{};
    controller_->add_AcceleratorKeyPressed(handler, &token);
    handler->Release();
#endif
  }

  HWND hwnd() const { return hwnd_; }
  /** The platform window this view lives in, as an opaque handle: an HWND on
   *  Windows, a GtkWidget* under GTK, an NSView* on macOS. Only a modal
   *  dialog needs it, and only so the dialog is owned by the editor rather
   *  than floating loose behind the DAW. */
  void* nativeWindow() const { return (void*) hwnd_; }
  /** The platform-neutral name every backend exposes, so the wrapper's GUI code
   *  is written once rather than once per OS. */
  void* handle() const { return (void*) hwnd_; }
  bool ready() const { return ready_; }
  /** Why there is no webview, when there isn't one. Empty means healthy.
   *  A host that logs this gets a real answer instead of a blank rectangle. */
  const std::string& status() const { return status_; }

  void setSize(uint32_t width, uint32_t height) {
    width_ = width;
    height_ = height;
    if (hwnd_) SetWindowPos(hwnd_, nullptr, 0, 0, (int) width, (int) height,
                            SWP_NOZORDER | SWP_NOMOVE | SWP_NOACTIVATE);
    layout();
  }

  /**
   * Hiding SUSPENDS, showing resumes.
   *
   * Hosts hide editors far more often than they close them, and before this
   * a hidden editor kept its renderer fully warm: webview_bench measured
   * hiding six editors at -0.1 MB. Telling WebView2 the view is invisible
   * lets it drop rendering resources, and TrySuspend freezes the renderer
   * the way a background browser tab is frozen -- DOM and JS state intact,
   * which is exactly the difference between this and destroying the editor.
   *
   * Two details are load-bearing:
   *
   * - put_IsVisible(FALSE) must come BEFORE TrySuspend, which refuses while
   *   the view is visible. The result callback is deliberately a no-op: a
   *   refused suspend (a navigation in flight, devtools open) just means the
   *   renderer stays warm, which was yesterday's behaviour.
   *
   * - eval() must go quiet while hidden -- see the guard there. The meter
   *   tick keeps arriving from the wrapper, and ExecuteScript on a suspended
   *   renderer RESUMES it, so one frame of an invisible meter would undo the
   *   whole saving forever.
   */
  void setVisible(bool visible) {
    if (hwnd_) ShowWindow(hwnd_, visible ? SW_SHOW : SW_HIDE);
    visible_ = visible;
#if defined(SONORE_HAS_WEBVIEW2)
    if (!ready_ || !controller_) return;
    if (visible) {
      controller_->put_IsVisible(TRUE);
      ICoreWebView2_19* v19 = nullptr;
      if (webview_ && SUCCEEDED(webview_->QueryInterface(IID_PPV_ARGS(&v19))) && v19) {
        v19->put_MemoryUsageTargetLevel(COREWEBVIEW2_MEMORY_USAGE_TARGET_LEVEL_NORMAL);
        v19->Release();
      }
      ICoreWebView2_3* v3 = nullptr;
      if (webview_ && SUCCEEDED(webview_->QueryInterface(IID_PPV_ARGS(&v3))) && v3) {
        v3->Resume();
        v3->Release();
      }
    } else {
      controller_->put_IsVisible(FALSE);
      // Freezing alone stops CPU but returns almost no memory -- measured at
      // -1.9 MB across six hidden editors. The LOW memory target is the API
      // built for a hidden view: the renderer purges caches and runs an
      // aggressive GC, and THAT is where the commit comes back. Set before
      // the suspend, so the purge happens while the renderer can still run.
      ICoreWebView2_19* v19 = nullptr;
      if (webview_ && SUCCEEDED(webview_->QueryInterface(IID_PPV_ARGS(&v19))) && v19) {
        v19->put_MemoryUsageTargetLevel(COREWEBVIEW2_MEMORY_USAGE_TARGET_LEVEL_LOW);
        v19->Release();
      }
      ICoreWebView2_3* v3 = nullptr;
      if (webview_ && SUCCEEDED(webview_->QueryInterface(IID_PPV_ARGS(&v3))) && v3) {
        // The result is deliberately ignored: a refused suspend (navigation in
        // flight, devtools open) just leaves the renderer warm, which was the
        // old behaviour. Both calls were verified succeeding -- S_OK, ok=1 --
        // while this was being measured; the numbers in webview_bench are the
        // real observability.
        auto* done = new SuspendHandler([](HRESULT, BOOL) -> HRESULT { return S_OK; });
        v3->TrySuspend(done);
        done->Release();
        v3->Release();
      }
    }
#endif
  }

  /** Run JS in the page. Silently ignored until the webview exists: callers
   *  are timers and parameter echoes, and dropping one frame of a meter while
   *  Edge starts is correct behaviour, not an error to report. */
  void eval(const std::string& js) {
#if defined(SONORE_HAS_WEBVIEW2)
    if (!ready_ || !webview_) return;
    // Silent while hidden, and not merely as an optimisation: ExecuteScript
    // on a suspended renderer WAKES it, so the wrapper's 33 ms meter tick
    // would resurrect every hidden editor within one frame of it suspending.
    // Nothing is lost -- the tick keeps running, and the first one after
    // setVisible(true) repaints the meters from current state.
    if (!visible_) return;
    webview_->ExecuteScript(widen(js).c_str(), nullptr);
#else
    (void) js;
#endif
  }

  /** Called by the window procedure on WM_TIMER. */
  std::function<void()> onTick;

private:
  static constexpr UINT_PTR kTimerId = 1;

  void layout() {
#if defined(SONORE_HAS_WEBVIEW2)
    if (controller_) {
      RECT r{0, 0, (LONG) width_, (LONG) height_};
      controller_->put_Bounds(r);
    }
#endif
  }

  /** A last-resort face when there is no webview at all: centred text, drawn by
   *  the window procedure. Better than a black rectangle the user can't read.
   *  Also reported to the debugger and stderr: "the editor is blank" is
   *  otherwise the least actionable bug report a user can send. */
  void setStatusText(std::string text) {
    status_ = std::move(text);
    const std::string line = "[sonore:webview] " + status_ + "\n";
    OutputDebugStringA(line.c_str());
    std::fputs(line.c_str(), stderr);
    if (hwnd_) InvalidateRect(hwnd_, nullptr, TRUE);
  }

#if defined(SONORE_HAS_WEBVIEW2)
  /** Is the object these callbacks were made for still there?
   *
   *  WebView2 starts asynchronously and takes a few hundred milliseconds. A
   *  user who opens an editor and closes it inside that window -- clicking
   *  the wrong plugin, or a host that probes an editor to measure it -- had
   *  the completion handler fire into freed memory, because the handlers held
   *  a bare `this`. It is a crash you cannot reproduce by hand reliably and
   *  which lands on whoever happens to close a window quickly.
   *
   *  A token both sides share settles it: destroy() marks this generation
   *  dead and starts a new one, so a late handler can tell it is late. */
  /**
   * One WebView2 environment for the whole binary, and one user-data folder
   * for ALL Sonore plugins.
   *
   * Both halves exist for the same measured reason. WebView2 shares the
   * browser and GPU processes between webviews created against the SAME
   * user-data folder and runtime version -- and the folder used to be
   * per-plugin, which GUARANTEED two different Sonore plugins in one session
   * paid for two complete browser trees. webview_bench measured it: the
   * second plugin's first editor cost +152 MB where a shared-tree editor
   * costs ~30 MB. The folder is now one per vendor, so every Sonore editor
   * in a host shares one browser and one GPU process, and only the ~30 MB
   * renderer is per-editor.
   *
   * The environment SINGLETON is the in-process half: creating an
   * environment per editor open redid async startup work per open and could
   * never share the object. One static, callbacks queued while creation is
   * in flight, all on the main STA thread so there is nothing to lock. A
   * FAILED creation is deliberately not cached -- the ordinary failure is
   * "runtime not installed", and a user who installs it mid-session should
   * get an editor on the next open, not a cached refusal.
   *
   * What survives from the old comment, because it is the part that was
   * always load-bearing: the folder lives under LOCALAPPDATA. Letting
   * WebView2 default to the host EXE's directory fails inside Program Files
   * and takes the editor down with it.
   */
  static void sharedEnvironment(std::function<void(ICoreWebView2Environment*)> onReady) {
    static ICoreWebView2Environment* env = nullptr;
    static std::vector<std::function<void(ICoreWebView2Environment*)>>* pending = nullptr;

    if (env) {
      onReady(env);
      return;
    }
    if (pending) { // creation already in flight: join it
      pending->push_back(std::move(onReady));
      return;
    }

    wchar_t appData[MAX_PATH]{};
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", appData, MAX_PATH)) {
      onReady(nullptr);
      return;
    }
    const std::wstring folder = std::wstring(appData) + L"\\Sonore\\WebView2";
    createFolders(folder);

    pending = new std::vector<std::function<void(ICoreWebView2Environment*)>>();
    pending->push_back(std::move(onReady));

    auto* handler = new EnvHandler([](HRESULT hr, ICoreWebView2Environment* created) -> HRESULT {
      if (SUCCEEDED(hr) && created) {
        env = created;
        env->AddRef(); // process-lifetime, on purpose: every future editor uses it
      }
      auto* waiters = pending;
      pending = nullptr; // cleared BEFORE the callbacks, so a retry from one is clean
      for (auto& waiter : *waiters) waiter(env);
      delete waiters;
      return S_OK;
    });
    const HRESULT hr =
        CreateCoreWebView2EnvironmentWithOptions(nullptr, folder.c_str(), nullptr, handler);
    handler->Release();
    if (FAILED(hr)) {
      auto* waiters = pending;
      pending = nullptr;
      for (auto& waiter : *waiters) waiter(nullptr);
      delete waiters;
    }
  }

  void startWebView() {
    auto alive = alive_;
    sharedEnvironment([this, alive](ICoreWebView2Environment* env) {
      if (!alive->load()) return; // the editor closed while Edge was starting
      if (!env) {
        setStatusText("Microsoft Edge WebView2 Runtime is not installed.");
        return;
      }
      auto* controllerHandler =
          new ControllerHandler([this, alive](HRESULT hr2, ICoreWebView2Controller* c) -> HRESULT {
            if (!alive->load()) {
              // Closed while the controller was being built. It arrived after
              // its window did not: close it, or a live controller sits
              // parented to a destroyed HWND for the life of the process.
              if (c) c->Close();
              return S_OK;
            }
            if (FAILED(hr2) || !c) {
              setStatusText("The webview could not be created.");
              return S_OK;
            }
            controller_ = c;
            controller_->AddRef();
            controller_->get_CoreWebView2(&webview_);
            if (!webview_) {
              setStatusText("The webview could not be created.");
              return S_OK;
            }
            configure();
            installKeyForwarding();
            ready_ = true;
            layout();
            // Inject the bridge so it exists before ANY page script, then load.
            webview_->AddScriptToExecuteOnDocumentCreated(widen(bridge_).c_str(), nullptr);
            webview_->NavigateToString(widen(html_).c_str());
            return S_OK;
          });
      env->CreateCoreWebView2Controller(hwnd_, controllerHandler);
      // The API holds its own reference now; keeping ours would leak the
      // handler once per editor open.
      controllerHandler->Release();
    });
  }

  void configure() {
    ICoreWebView2Settings* settings = nullptr;
    if (SUCCEEDED(webview_->get_Settings(&settings)) && settings) {
      // A plugin face is an appliance, not a browser: no dev tools, no context
      // menu, no status bar, and no zoom gesture that would desync the layout
      // from the size the host was told.
      settings->put_AreDefaultContextMenusEnabled(FALSE);
      settings->put_AreDevToolsEnabled(FALSE);
      settings->put_IsStatusBarEnabled(FALSE);
      settings->put_IsZoomControlEnabled(FALSE);
      settings->put_AreHostObjectsAllowed(FALSE);
      settings->put_IsWebMessageEnabled(TRUE);
      settings->Release();
    }

    EventRegistrationToken token{};
    auto* messageHandler =
        new MessageHandler([this](ICoreWebView2*,
                                  ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
          if (!args || !onMessage) return S_OK;
          LPWSTR raw = nullptr;
          if (SUCCEEDED(args->TryGetWebMessageAsString(&raw)) && raw) {
            const BridgeMessage msg = parseBridgeMessage(narrow(raw));
            if (msg.kind != BridgeMessage::Kind::None) onMessage(msg);
            CoTaskMemFree(raw);
          }
          return S_OK;
        });
    webview_->add_WebMessageReceived(messageHandler, &token);
    // The webview holds the subscription's reference for its own lifetime.
    messageHandler->Release();
  }

  static void createFolders(const std::wstring& path) {
    // CreateDirectory only makes one level, so walk the path.
    for (size_t i = 3; i <= path.size(); ++i) {
      if (i == path.size() || path[i] == L'\\') {
        const std::wstring part = path.substr(0, i);
        CreateDirectoryW(part.c_str(), nullptr);
      }
    }
  }

  ICoreWebView2Controller* controller_ = nullptr;
  ICoreWebView2* webview_ = nullptr;
#endif // SONORE_HAS_WEBVIEW2

  HWND hwnd_ = nullptr;
  uint32_t width_ = 700;
  uint32_t height_ = 420;
  bool ready_ = false;
  bool visible_ = true;
  bool comInitialised_ = false;
  /** Shared with every pending async handler; see startWebView. */
  std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
  std::string html_, bridge_, userDataName_, status_;

  friend LRESULT CALLBACK hostWndProc(HWND, UINT, WPARAM, LPARAM);
};

inline LRESULT CALLBACK hostWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  auto* host = (WebViewHost*) GetWindowLongPtrW(hwnd, GWLP_USERDATA);
  switch (msg) {
    case WM_SIZE:
      if (host) host->layout();
      return 0;
    case WM_TIMER:
      if (host && host->onTick) host->onTick();
      return 0;
    case WM_PAINT: {
      if (host && !host->status_.empty()) {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);
        RECT r{};
        GetClientRect(hwnd, &r);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(190, 205, 220));
        const std::wstring text = widen(host->status_);
        DrawTextW(dc, text.c_str(), (int) text.size(), &r,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_WORDBREAK);
        EndPaint(hwnd, &ps);
        return 0;
      }
      break;
    }
    default:
      break;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace win32
} // namespace sonore

#endif // _WIN32
