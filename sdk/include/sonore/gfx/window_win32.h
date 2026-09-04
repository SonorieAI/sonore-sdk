// SPDX-License-Identifier: Apache-2.0
//
// The Win32 window a native editor lives in: the platform peer for a
// component tree.
//
// ── All it does is window, blit and input ───────────────────────────────────
//
// Because everything is rasterised into a Bitmap, this layer never draws. It
// creates a child window inside the host's, copies a buffer into it, and turns
// Windows messages into component events. That is the whole platform surface,
// and it is why porting to GTK or Cocoa is a day rather than a month.
//
// ── The two Win32 rules that break audio plugins ────────────────────────────
//
// SetCapture. Without it a drag that leaves the window stops arriving, and a
// user who drags a knob quickly gets a control that sticks. The component
// tree has its own capture, which decides WHICH component; this is the
// operating system's, which decides whether the messages arrive at all. Both
// are needed and they are not the same thing.
//
// TrackMouseEvent. Windows sends no "the pointer left" message unless it is
// asked, once, per entry. Without it a hover highlight stays lit after the
// user has moved to another plugin -- and it must be re-armed after every
// WM_MOUSELEAVE, because the request is consumed by the message it produces.
#pragma once

#if !defined(_WIN32)
#error "window_win32.h is Windows only"
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// The drag-and-drop half of the shell API. Separate from windows.h and needed
// for HDROP, DragAcceptFiles and DragQueryFile; it links against shell32,
// which every Windows process already has loaded.
#include <shellapi.h>

#include <string>
#include <vector>

#include <functional>
#include <string>
#include <vector>

#include "backing.h"
#include "component.h"
#include "displays.h"
#include "graphics.h"
#include "uia_win32.h"

namespace sonore {
namespace gfx {

class NativeWindow {
public:
  ~NativeWindow() { close(); }

  /**
   * Open inside a host's window.
   *
   * `parent` is the HWND a plugin format hands over. Null makes a top-level
   * window, which is what the standalone and the tests use.
   */
  /**
   * Open as a POPUP: a top-level window with no frame, above everything,
   * holding the mouse until it is dismissed.
   *
   * `screenX/screenY` are desktop coordinates, because a menu has to be placed
   * against the screen's edges and the control that opened it is somewhere
   * inside a host inside a window manager. Converting once, at the call site
   * that knows where the control is, beats threading four coordinate spaces
   * through here.
   *
   * onDismissedOutside fires when the mouse goes down anywhere that is NOT this
   * window -- the whole point of a menu, and the part a component tree cannot
   * see because the click never reaches it.
   */
  bool openPopup(Component& content, int screenX, int screenY, int width, int height,
                 bool grabMouse = true) {
    isPopup_ = true;
    grabsMouse_ = grabMouse;
    const bool ok = open(nullptr, content, width, height, screenX, screenY);
    if (!ok) {
      isPopup_ = false;
      return false;
    }
    // Shown WITHOUT activating: a menu that stole focus would take it from the
    // host's keyboard focus and not give it back on dismissal.
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    // The capture is what makes a click outside reach us at all. Without it the
    // click goes to whatever is behind, the menu stays open, and the user has
    // two things responding to one gesture.
    //
    // A tooltip passes false: grabbing would stop the user reaching the very
    // control it is describing.
    if (grabsMouse_) SetCapture(hwnd_);
    return true;
  }

  std::function<void()> onDismissedOutside;

  bool open(void* parent, Component& content, int width, int height) {
    return open(parent, content, width, height, 0, 0);
  }

  /**
   * `width` and `height` are LOGICAL units, as is everything above this file.
   *
   * The conversion to device pixels happens HERE, because this is the boundary:
   * a component asking for a 620-point editor should not have to know what
   * display it is about to appear on, and the caller that did the multiplying
   * itself is the caller that gets it wrong when the answer changes.
   *
   * The scale is read from the PARENT before the window exists, since our own
   * has no DPI yet. Inside a host that parent is the host's window and is on
   * the right monitor by construction; with no parent it is the system's, which
   * is what a standalone gets. It is re-read from our own window immediately
   * after creation, and a disagreement -- possible when a top-level window
   * lands on a different monitor from the one the system prefers -- is applied
   * before the first paint rather than being visible as a jump.
   */
  bool open(void* parent, Component& content, int width, int height, int screenX, int screenY) {
    const bool popup = isPopup_;
    close();
    isPopup_ = popup; // close() resets it; this call is the one that set it
    content_ = &content;
    router_ = new MouseRouter(content);
    if (!registerClass()) return false;

    const float openScale = dpiScaleForWindow((HWND) parent);
    logicalWidth_ = (float) width;
    logicalHeight_ = (float) height;
    width = Backing::toDevice(logicalWidth_, openScale);
    height = Backing::toDevice(logicalHeight_, openScale);

    // BEFORE CreateWindowExW, and this order is not cosmetic.
    //
    // CreateWindowExW dispatches WM_NCCREATE, WM_CREATE and WM_SIZE
    // SYNCHRONOUSLY, into a procedure that already has `this`. WM_SIZE calls
    // resized(), which renders. So the backing store paints once before
    // CreateWindowExW has even returned -- and if it is still holding the
    // component from a previous open(), that component was destroyed when the
    // editor closed and this paints through a dangling pointer.
    //
    // It survived the FIRST open of a fresh window, because there the stale
    // pointer is null. It crashed on the second, inside a host, which is where
    // reopening an editor is the ordinary thing to do rather than an edge case.
    backing_.setContent(content_);
    backing_.setScale(openScale);
    backing_.setDeviceSize(width, height);

    // A top-level window is created WITHOUT WS_VISIBLE so a test can drive one
    // without anything appearing on screen; a caller that wants to see it calls
    // ShowWindow. A child window inside a host is visible from the start,
    // because the host has already decided to show the editor.
    // WS_POPUP has no frame and no caption, which is what a menu is. It is also
    // the only style that can sit above the host's own window rather than
    // inside it.
    const DWORD style = isPopup_ ? WS_POPUP
                                 : (parent ? (WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN)
                                           : WS_OVERLAPPEDWINDOW);
    // WS_EX_NOACTIVATE keeps the host's focus where it was; WS_EX_TOPMOST keeps
    // the menu above it. Neither is optional: without the first, dismissing the
    // menu leaves the keyboard somewhere the user did not put it.
    const DWORD exStyle = isPopup_ ? (WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE) : 0;
    RECT r{0, 0, width, height};
    if (!parent && !isPopup_) AdjustWindowRect(&r, style, FALSE);

    hwnd_ = CreateWindowExW(exStyle, className(), L"", style, screenX, screenY,
                            r.right - r.left, r.bottom - r.top,
                            (HWND) parent, nullptr, thisModule(), this);
    if (!hwnd_) {
      delete router_;
      router_ = nullptr;
      return false;
    }
    refreshDisplays();
    // Our own window's DPI, now that there is one. Usually the parent's, and
    // when it is not -- a top-level window that landed on another monitor --
    // it is applied here, before anything is shown, rather than as a visible
    // jump a frame later.
    const float realScale = dpiScaleForWindow(hwnd_);
    if (realScale != openScale) {
      backing_.setScale(realScale);
      width = Backing::toDevice(logicalWidth_, realScale);
      height = Backing::toDevice(logicalHeight_, realScale);
      SetWindowPos(hwnd_, nullptr, 0, 0, width, height,
                   SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
      backing_.setDeviceSize(width, height);
    }
    render();
    // The accessibility provider, which is what a screen reader talks to.
    // Created with the window because WM_GETOBJECT can arrive before anything
    // else -- a reader already running when the editor opens asks immediately.
    uiaRoot_ = new uia::RootProvider(hwnd_, content_, accessibleWindowName_);
    uiaRoot_->setScale(backing_.scale());

    // 33 ms, the same rate as the webview host. Faster is invisible on a
    // knob; slower makes automation look like it is stepping.
    SetTimer(hwnd_, kTimerId, 33, nullptr);
    // Files can be dropped on us. Costs nothing when nothing accepts them: the
    // router finds no interested component and the drop is ignored.
    DragAcceptFiles(hwnd_, TRUE);
    return true;
  }

  void close() {
    if (hwnd_) {
      // Cleared BEFORE DestroyWindow: destroying sends WM_DESTROY straight
      // back into the procedure, and a procedure that finds a live pointer
      // there will paint into an object that is halfway gone.
      SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
      KillTimer(hwnd_, kTimerId);
      DestroyWindow(hwnd_);
      hwnd_ = nullptr;
    }
    if (uiaRoot_) {
      // DETACHED before released: UIA may hold references of its own and go on
      // calling in after the window is gone, and an provider still holding the
      // editor's component tree would walk freed memory. Detaching leaves an
      // object that answers "empty" forever, which is the correct answer for a
      // window that no longer exists.
      uiaRoot_->detach();
      uiaRoot_->Release();
      uiaRoot_ = nullptr;
      // Tells UIA to drop anything it cached for this window. Without it a
      // reader keeps announcing controls from an editor that has closed.
      if (UiaClientsAreListening()) UiaReturnRawElementProvider(hwnd_, 0, 0, nullptr);
    }
    delete router_;
    router_ = nullptr;
    content_ = nullptr;
    // The backing store too. It holds the same pointer and outlives the window
    // -- a NativeWindow is a member of NativeEditor and is reused across every
    // open -- so leaving it set means the next paint reaches a component the
    // editor has already destroyed.
    backing_.setContent(nullptr);
    isPopup_ = false;
  }

  bool isPopup() const { return isPopup_; }

  /** Where the pointer last was, in this window's coordinates. Tracked because
   *  a tooltip has to be placed at the pointer and the router only knows which
   *  COMPONENT is under it. */
  Point lastMousePosition() const { return lastMouse_; }

  /**
   * Called about thirty times a second while the window is open.
   *
   * A plugin editor is not driven by its own events alone: automation, a
   * preset load and host undo all change parameters with nobody touching the
   * window, and without a clock those changes appear only the next time the
   * mouse happens to move. The webview host next door runs the same 33 ms
   * timer for the same reason.
   *
   * Set it BEFORE open() -- the timer starts there.
   */
  std::function<void()> onTick;

  void* handle() const { return hwnd_; }
  bool isOpen() const { return hwnd_ != nullptr; }
  Bitmap& bitmap() { return backing_.bitmap(); }

  /**
   * What one logical pixel is worth here.
   *
   * A host that implements the scale extension overrides whatever the monitor
   * said, because it may be compositing our view into a surface it scales
   * itself -- its answer is about the view, ours is only about the screen.
   */
  void setScale(float scale) {
    backing_.setScale(scale);
    render();
  }

  float scale() const { return backing_.scale(); }

  /** What a screen reader calls this window. Set BEFORE open(): the provider
   *  is built there, and a reader already running asks the moment it appears. */
  void setAccessibleWindowName(std::string name) { accessibleWindowName_ = std::move(name); }

  /** The client area in LOGICAL units, which is what a caller reasoning about
   *  layout wants and what setSize below is the inverse of. */
  float logicalWidth() const { return backing_.logicalWidth(); }
  float logicalHeight() const { return backing_.logicalHeight(); }
  MouseRouter* router() { return router_; }

  /** SW_SHOWNA, not SW_SHOW: an editor appearing must not steal focus from
   *  whatever the user was typing into. */
  void setVisible(bool shouldBeVisible) {
    if (!hwnd_) return;
    ShowWindow(hwnd_, shouldBeVisible ? SW_SHOWNA : SW_HIDE);
  }

  /** DEVICE pixels -- the same units Windows takes. */
  void setSize(int width, int height) {
    if (!hwnd_ || width <= 0 || height <= 0) return;
    SetWindowPos(hwnd_, nullptr, 0, 0, width, height, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    resized(width, height);
  }

  /** LOGICAL units, converted at the current scale. What an editor asking for
   *  "620 by 400" means, on any display. */
  void setLogicalSize(float width, float height) {
    logicalWidth_ = width;
    logicalHeight_ = height;
    setSize(Backing::toDevice(width, backing_.scale()),
            Backing::toDevice(height, backing_.scale()));
  }

  /**
   * Repaint if anything asked to be.
   *
   * Called from a timer. The damaged rectangle is currently the whole window
   * rather than the union the tree reports -- correct, and wasteful. Partial
   * repaint needs the tree to paint only the components that intersect the
   * damage, and doing that wrong shows as stale pixels nobody can reproduce.
   * The damage IS tracked; using it is the optimisation, and it is not free.
   */
  bool renderIfDirty() {
    if (!hwnd_ || !content_ || !content_->isDirty()) return false;
    render();
    return true;
  }

private:
  /**
   * Ask Windows for every monitor.
   *
   * Refreshed when a window opens rather than once per process: somebody
   * plugging in a monitor mid-session is a thing that happens, and a list
   * cached at startup would place menus on a screen that is no longer there.
   *
   * The WORK area, not the full bounds, is what a menu is placed inside --
   * otherwise a menu near the bottom of the screen goes under the taskbar,
   * which is where the host's own transport usually is.
   */
  static void refreshDisplays() {
    std::vector<Display> found;
    EnumDisplayMonitors(nullptr, nullptr, &monitorCallback, (LPARAM) &found);
    if (!found.empty()) Displays::set(std::move(found));
  }

  static BOOL CALLBACK monitorCallback(HMONITOR monitor, HDC, LPRECT, LPARAM userData) {
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) return TRUE; // skip it, keep enumerating

    Display d;
    d.bounds = Rect((float) info.rcMonitor.left, (float) info.rcMonitor.top,
                    (float) (info.rcMonitor.right - info.rcMonitor.left),
                    (float) (info.rcMonitor.bottom - info.rcMonitor.top));
    d.workArea = Rect((float) info.rcWork.left, (float) info.rcWork.top,
                      (float) (info.rcWork.right - info.rcWork.left),
                      (float) (info.rcWork.bottom - info.rcWork.top));
    d.isMain = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
    ((std::vector<Display>*) userData)->push_back(d);
    return TRUE;
  }

  static HMODULE thisModule() {
    HMODULE m = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR) &thisModule, &m);
    return m;
  }

  static std::wstring& classNameStorage() {
    static std::wstring name;
    return name;
  }
  static const wchar_t* className() { return classNameStorage().c_str(); }

  static ATOM registerClass() {
    static ATOM atom = 0;
    if (atom) return atom;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS; // CS_DBLCLKS or no double clicks arrive
    wc.lpfnWndProc = wndProc;
    wc.hInstance = thisModule();
    // NULL, deliberately. With a class cursor set, Windows resets the pointer to
    // it on every WM_MOUSEMOVE and a WM_SETCURSOR handler is never consulted --
    // so a knob would show an arrow no matter what the component asked for.
    wc.hCursor = nullptr;
    // No background brush: every pixel is painted from the bitmap, and letting
    // Windows erase first is a flash of flat colour on every resize.
    wc.hbrBackground = nullptr;
    // A name of our own, never a shared one. Two modules sharing a class means
    // a window running a procedure in a module that may since have been
    // unloaded -- the same reasoning as the webview host next door, and the
    // same crash if ignored.
    for (int attempt = 0; attempt < 64 && !atom; ++attempt) {
      wchar_t buffer[80];
      std::swprintf(buffer, 80, L"SonoreNativeUI_%p_%d", (void*) thisModule(), attempt);
      classNameStorage() = buffer;
      wc.lpszClassName = classNameStorage().c_str();
      atom = RegisterClassExW(&wc);
      if (!atom && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) break;
    }
    if (!atom) classNameStorage().clear();
    return atom;
  }

  /** `width` and `height` are DEVICE pixels, which is what WM_SIZE reports.
   *  The logical size the tree is laid out to is derived from the scale. */
  void resized(int width, int height) {
    if (!content_ || width <= 0 || height <= 0) return;
    backing_.setDeviceSize(width, height);
    render();
  }

  /**
   * Repaint, drawing only what changed.
   *
   * This is what the comment on renderIfDirty asked for since this file was
   * written: "the damaged rectangle is currently the whole window rather than
   * the union the tree reports -- correct, and wasteful. The damage IS tracked;
   * using it is the optimisation, and it is not free."
   *
   * It is not free, and here is the price: the ground under the damaged area
   * has to be cleared before the tree paints over it, or the previous frame
   * shows through anything transparent. Clearing the WHOLE bitmap would undo
   * the saving entirely, so it clears exactly the damaged rectangles.
   *
   * A full repaint is still what happens on the first frame and after a resize,
   * because there is no previous frame to keep.
   */
public:
  /**
   * Where an input method's candidate window should sit, in DEVICE pixels
   * relative to the client area. False when nothing focused has a caret.
   *
   * Separated from the call that uses it so it can be TESTED. Whether imm32
   * moved a window is not observable from here; whether the arithmetic put the
   * point beside the caret of the focused field is exactly observable, and it
   * is the half that can be wrong.
   */
  bool compositionPoint(int* x, int* y) const {
    if (!router_ || !x || !y) return false;
    Component* focused = router_->focused();
    if (!focused) return false;
    Rect caret;
    if (!focused->caretBounds(&caret)) return false;

    // The caret is in the FOCUSED component's coordinates and the IME wants
    // the window's. Walking up through the parents is what converts them, and
    // it is the step that silently produces a point in the top-left corner if
    // it is left out -- which looks exactly like not having implemented this
    // at all.
    const Point origin = focused->localToRoot({caret.x, caret.y + caret.h});
    *x = (int) (origin.x * backing_.scale() + 0.5f);
    *y = (int) (origin.y * backing_.scale() + 0.5f);
    return true;
  }

  /**
   * Tell the input method where to put its candidate list.
   *
   * imm32 is loaded at RUNTIME rather than linked, which is the same rule
   * every optional backend here follows -- and here it also means no plugin
   * built with this SDK has to add -limm32 to a link line that already works.
   * A machine without imm32 (there is no such Windows, but the check costs
   * nothing) simply gets the default placement.
   */
  void placeCompositionWindow() {
    int x = 0, y = 0;
    if (!compositionPoint(&x, &y)) return;

    using ImmGetContextFn = HIMC(WINAPI*)(HWND);
    using ImmSetCompositionWindowFn = BOOL(WINAPI*)(HIMC, COMPOSITIONFORM*);
    using ImmReleaseContextFn = BOOL(WINAPI*)(HWND, HIMC);
    static HMODULE imm = LoadLibraryW(L"imm32.dll");
    if (!imm) return;
    static auto getContext = (ImmGetContextFn) (void*) GetProcAddress(imm, "ImmGetContext");
    static auto setWindow =
        (ImmSetCompositionWindowFn) (void*) GetProcAddress(imm, "ImmSetCompositionWindow");
    static auto release = (ImmReleaseContextFn) (void*) GetProcAddress(imm, "ImmReleaseContext");
    if (!getContext || !setWindow || !release) return;

    HIMC context = getContext(hwnd_);
    if (!context) return;
    COMPOSITIONFORM form{};
    form.dwStyle = CFS_POINT;
    form.ptCurrentPos.x = x;
    form.ptCurrentPos.y = y;
    setWindow(context, &form);
    release(hwnd_, context);
  }

private:

  void render() {
    const std::vector<PixelRect>& changed = backing_.render();
    if (!hwnd_) return;
    // Only the changed rectangles are handed to Windows, so the blit in
    // WM_PAINT copies those and not the window. InvalidateRect unions what it
    // is given with what is already pending, which is exactly right.
    //
    // Device pixels, because that is the only thing Windows has ever heard of.
    // The conversion happens once, in the backing store, rather than at each
    // of the places that used to do it.
    for (const PixelRect& p : changed) {
      RECT win{p.x, p.y, p.right(), p.bottom()};
      InvalidateRect(hwnd_, &win, FALSE);
    }
  }

  /** The ground colour. Not from a LookAndFeel because the window is below
   *  the component tree and must have something to clear to before anything
   *  in the tree has painted. */
  static Colour LookAndFeelBackground() { return palette::background(); }

  /**
   * Copy the bitmap into the window.
   *
   * Windows wants BGRA and the rasteriser produces RGBA, so the channels are
   * swapped into a scratch buffer here. Storing BGRA natively would save the
   * pass and would put a platform's byte order into the rasteriser that three
   * platforms share -- the wrong side of the boundary to leak across.
   */
  void blit(HDC dc) {
    const Bitmap& bm = backing_.bitmap();
    const int w = bm.width(), h = bm.height();
    if (w <= 0 || h <= 0) return;
    scratch_.resize((size_t) w * (size_t) h * 4u);
    const uint8_t* src = bm.data();
    for (size_t i = 0; i < scratch_.size(); i += 4) {
      scratch_[i + 0] = src[i + 2];
      scratch_[i + 1] = src[i + 1];
      scratch_[i + 2] = src[i + 0];
      scratch_[i + 3] = src[i + 3];
    }
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = w;
    // NEGATIVE height: a DIB is bottom-up by default and this buffer is
    // top-down. Positive here draws the whole interface upside down.
    info.bmiHeader.biHeight = -h;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    SetDIBitsToDevice(dc, 0, 0, (DWORD) w, (DWORD) h, 0, 0, 0, (UINT) h, scratch_.data(), &info,
                      DIB_RGB_COLORS);
  }

  void armMouseLeave() {
    if (trackingLeave_ || !hwnd_) return;
    TRACKMOUSEEVENT t{};
    t.cbSize = sizeof(t);
    t.dwFlags = TME_LEAVE;
    t.hwndTrack = hwnd_;
    TrackMouseEvent(&t);
    trackingLeave_ = true;
  }

  /**
   * A mouse position from Windows, in the tree's own units.
   *
   * Windows reports DEVICE pixels, always. At 200% an unconverted position is
   * twice the coordinate the control was drawn at, so every click lands on
   * whatever is a screenful down and to the right -- or on nothing, which does
   * not read as a coordinate bug. It reads as "the plugin ignores my mouse".
   *
   * A member rather than a static because the divisor is per-window.
   */
  /**
   * The DPI scale of the monitor a window is on.
   *
   * GetDpiForWindow is Windows 10 1607 and later, and is resolved at runtime
   * rather than linked: a plugin is loaded into whatever host the user has, on
   * whatever Windows they have, and a missing import is a load failure with no
   * message rather than a wrong scale. The fallback is the desktop DC's
   * LOGPIXELSX, which is the system-wide answer and predates all of this.
   */
  static float dpiScaleForWindow(HWND hwnd) {
    typedef UINT(WINAPI * GetDpiForWindowFn)(HWND);
    static GetDpiForWindowFn fn = [] {
      HMODULE user32 = GetModuleHandleW(L"user32.dll");
      return user32 ? (GetDpiForWindowFn) GetProcAddress(user32, "GetDpiForWindow") : nullptr;
    }();
    if (fn && hwnd) {
      const UINT dpi = fn(hwnd);
      if (dpi > 0) return (float) dpi / 96.0f;
    }
    HDC dc = GetDC(nullptr);
    if (!dc) return 1.0f;
    const int dpi = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(nullptr, dc);
    return dpi > 0 ? (float) dpi / 96.0f : 1.0f;
  }

  Point pointOf(LPARAM lp) const {
    return backing_.toLogical((int) (short) LOWORD(lp), (int) (short) HIWORD(lp));
  }

  /**
   * The character a virtual key would type with no modifiers -- 'a' for VK_A.
   *
   * Used ONLY for modifier combinations, where WM_CHAR gives a control code
   * instead of a letter. MapVirtualKey is a layout-aware lookup, so a French
   * keyboard's Ctrl+A is still the key marked A rather than wherever A sits on
   * an American one.
   *
   * Lowercased for letters, so a shortcut table compares one character and
   * reads shiftDown separately -- otherwise every table needs both cases and
   * one of them is eventually forgotten.
   */
  static uint32_t characterForVirtualKey(int vk) {
    // The high bit of the result marks a DEAD key, which types nothing on its
    // own and must not be reported as a character.
    const UINT mapped = MapVirtualKeyW((UINT) vk, MAPVK_VK_TO_CHAR);
    if (mapped & 0x80000000u) return 0;
    const uint32_t typed = mapped & 0xFFFFu;
    if (typed < 0x20 || typed == 0x7f) return 0;
    if (typed >= 'A' && typed <= 'Z') return typed - 'A' + 'a';
    return typed;
  }

  /** A virtual key code, or None for anything that will arrive as a character.
   *
   *  Deliberately short. Every key not named here reaches the component tree
   *  through WM_CHAR with the user's layout applied, which is the only way a
   *  field can be typed into on a keyboard that is not American. */
  static int namedKey(int vk) {
    switch (vk) {
      case VK_BACK: return KeyPress::Backspace;
      case VK_TAB: return KeyPress::Tab;
      case VK_RETURN: return KeyPress::Return;
      case VK_ESCAPE: return KeyPress::Escape;
      case VK_DELETE: return KeyPress::Delete;
      case VK_LEFT: return KeyPress::Left;
      case VK_RIGHT: return KeyPress::Right;
      case VK_UP: return KeyPress::Up;
      case VK_DOWN: return KeyPress::Down;
      case VK_HOME: return KeyPress::Home;
      case VK_END: return KeyPress::End;
      case VK_PRIOR: return KeyPress::PageUp;
      case VK_NEXT: return KeyPress::PageDown;
      default: return KeyPress::None;
    }
  }

  /** Loaded from the system, never created: these are the shapes the user's
   *  own theme defines, and a plugin drawing its own would be the one thing on
   *  the desktop that looked wrong. */
  static HCURSOR cursorFor(MouseCursor cursor) {
    const wchar_t* name = MAKEINTRESOURCEW(32512); // IDC_ARROW
    switch (cursor) {
      case MouseCursor::Pointing: name = MAKEINTRESOURCEW(32649); break;    // IDC_HAND
      case MouseCursor::DragVertical: name = MAKEINTRESOURCEW(32645); break; // IDC_SIZENS
      case MouseCursor::DragHorizontal: name = MAKEINTRESOURCEW(32644); break; // IDC_SIZEWE
      case MouseCursor::ResizeCorner: name = MAKEINTRESOURCEW(32642); break;    // IDC_SIZENWSE
      case MouseCursor::ResizeLeftRight: name = MAKEINTRESOURCEW(32644); break; // IDC_SIZEWE
      case MouseCursor::ResizeUpDown: name = MAKEINTRESOURCEW(32645); break;    // IDC_SIZENS
      case MouseCursor::Text: name = MAKEINTRESOURCEW(32513); break;        // IDC_IBEAM
      case MouseCursor::Wait: name = MAKEINTRESOURCEW(32514); break;        // IDC_WAIT
      default: break;
    }
    return LoadCursorW(nullptr, name);
  }

  static bool shiftHeld() { return (GetKeyState(VK_SHIFT) & 0x8000) != 0; }
  static bool ctrlHeld() { return (GetKeyState(VK_CONTROL) & 0x8000) != 0; }
  static bool altHeld() { return (GetKeyState(VK_MENU) & 0x8000) != 0; }

  static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
      auto* create = (CREATESTRUCTW*) lp;
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR) create->lpCreateParams);
      return DefWindowProcW(hwnd, msg, wp, lp);
    }
    auto* self = (NativeWindow*) GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (!self || !self->router_) return DefWindowProcW(hwnd, msg, wp, lp);

    switch (msg) {
      case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);
        self->blit(dc);
        EndPaint(hwnd, &ps);
        return 0;
      }
      case WM_TIMER:
        if (wp == kTimerId && self->onTick && !self->inTick_) {
          self->inTick_ = true;
          self->onTick();
          self->inTick_ = false;
          // And repaint if the tick changed anything. Without this the clock
          // pulls new values into the widgets and leaves the old picture on
          // screen -- which is worse than having no clock, because the state
          // and the pixels then disagree.
          self->renderIfDirty();
        }
        return 0;
      // The pointer moved over this window and Windows is asking what it should
      // look like. Answering means returning TRUE; falling through to
      // DefWindowProc lets the parent decide, which for a plugin is the host.
      case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
          SetCursor(cursorFor(self->router_->cursorForPointer()));
          return TRUE;
        }
        break;

      // A screen reader asking what is in this window.
      //
      // OBJID_CLIENT is the one UIA uses. The other object ids -- the title
      // bar, the menu, the scroll bars -- belong to the window frame and are
      // DefWindowProc's business; answering them with our provider would tell
      // a reader the editor's controls are the window's title bar.
      case WM_GETOBJECT:
        if (self->uiaRoot_ && (DWORD) lp == (DWORD) UiaRootObjectId) {
          self->uiaRoot_->setScale(self->backing_.scale());
          return UiaReturnRawElementProvider(
              hwnd, wp, lp, static_cast<IRawElementProviderSimple*>(self->uiaRoot_));
        }
        break;

      case WM_ERASEBKGND:
        // Claimed, and nothing drawn. Letting Windows erase means a flash of
        // flat colour before every paint.
        return 1;
      case WM_SIZE:
        self->resized((int) LOWORD(lp), (int) HIWORD(lp));
        return 0;
      // Dragged onto a monitor with a different DPI. Windows sends the new DPI
      // in wParam and the size it wants the window to become in lParam, and a
      // window that ignores this keeps the old scale until something else
      // resizes it -- which for a standalone is never.
      //
      // The suggested rectangle is used rather than computed: Windows has
      // already worked out where the window should sit so it does not jump
      // across the monitor boundary it was dragged over.
      case WM_DPICHANGED: {
        self->backing_.setScale((float) LOWORD(wp) / 96.0f);
        const RECT* suggested = (const RECT*) lp;
        if (suggested)
          SetWindowPos(self->hwnd_, nullptr, suggested->left, suggested->top,
                       suggested->right - suggested->left, suggested->bottom - suggested->top,
                       SWP_NOZORDER | SWP_NOACTIVATE);
        self->render();
        return 0;
      }
      case WM_LBUTTONDOWN:
      case WM_LBUTTONDBLCLK: {
        // With capture held by a popup, a click ANYWHERE arrives here, in this
        // window's coordinates -- which for a click outside are negative or
        // past the edge. That is how the menu learns it was dismissed, and it
        // is the reason the capture is taken in the first place.
        if (self->isPopup_ && self->grabsMouse_) {
          const Point p = self->pointOf(lp);
          const Rect bounds = self->content_ ? self->content_->bounds() : Rect();
          if (!bounds.contains(p)) {
            if (self->onDismissedOutside) self->onDismissedOutside();
            return 0;
          }
        }
        SetCapture(hwnd);
        self->router_->mouseDown(self->pointOf(lp), msg == WM_LBUTTONDBLCLK ? 2 : 1, shiftHeld(),
                                 ctrlHeld(), altHeld());
        self->renderIfDirty();
        return 0;
      }
      // ── An input method is about to compose ─────────────────────────────
      //
      // The candidate window -- the list of characters offered while typing
      // Japanese, Chinese or Korean -- is placed by the OS, and by default it
      // has no idea where the caret is. It lands in a corner of the window,
      // unrelated to the field being typed in.
      //
      // WM_IME_STARTCOMPOSITION is where that is fixed, and it must be fixed
      // BEFORE the default handler runs, because the default handler is what
      // shows the window. So this positions and then falls through rather than
      // returning: DefWindowProc still has to do everything else about the
      // composition, and swallowing the message would leave the user with no
      // candidate list at all.
      case WM_IME_STARTCOMPOSITION:
      case WM_IME_COMPOSITION: {
        self->placeCompositionWindow();
        break; // NOT return: DefWindowProc does the rest
      }
      case WM_RBUTTONDOWN: {
        // No SetCapture: a right-click starts no gesture, so there is nothing
        // to follow the pointer for. Taking capture here would mean holding it
        // until a WM_RBUTTONUP that a host's own menu may swallow, and a window
        // that keeps the mouse after its menu closes is a window nothing else
        // on the desktop can be clicked past.
        self->router_->mouseDown(self->pointOf(lp), 1, shiftHeld(), ctrlHeld(), altHeld(),
                                 /*rightButton=*/true);
        self->renderIfDirty();
        return 0;
      }
      case WM_MOUSEMOVE:
        self->lastMouse_ = self->pointOf(lp);
        self->armMouseLeave();
        self->router_->mouseMove(self->pointOf(lp), shiftHeld(), ctrlHeld(), altHeld());
        self->renderIfDirty();
        return 0;
      case WM_LBUTTONUP:
        // The router FIRST, then ReleaseCapture -- and the order is the whole
        // of it.
        //
        // ReleaseCapture sends WM_CAPTURECHANGED synchronously, and the
        // handler below ends the gesture as a cancel at (-1,-1). Releasing
        // first therefore cancelled every click before its mouse-up arrived:
        // a button pressed and released cleanly did nothing at all, and only
        // a test driving a real window with real messages could see it.
        self->router_->mouseUp(self->pointOf(lp), shiftHeld(), ctrlHeld(), altHeld());
        ReleaseCapture();
        self->renderIfDirty();
        return 0;
      case WM_MOUSELEAVE:
        // Consumed by the message it produced: re-armed on the next move, or
        // the second exit never arrives.
        self->trackingLeave_ = false;
        self->router_->mouseExitWindow();
        self->renderIfDirty();
        return 0;
      case WM_MOUSEWHEEL: {
        // Wheel coordinates are in SCREEN space, unlike every other mouse
        // message. Using them unconverted sends the event to whatever is at
        // that spot on the desktop, which inside a host is another plugin.
        POINT p{(short) LOWORD(lp), (short) HIWORD(lp)};
        ScreenToClient(hwnd, &p);
        const float delta = (float) GET_WHEEL_DELTA_WPARAM(wp) / (float) WHEEL_DELTA;
        self->router_->mouseWheel(self->backing_.toLogical(p.x, p.y), delta, shiftHeld(),
                                  ctrlHeld(), altHeld());
        self->renderIfDirty();
        return 0;
      }
      // ── Keyboard ───────────────────────────────────────────────────────
      //
      // Windows reports a key twice and means different things each time.
      // WM_KEYDOWN is a KEY -- a physical one, before the layout, which is
      // where arrows and Return and Tab come from. WM_CHAR is a CHARACTER,
      // after the layout, dead keys and any input method have had their say,
      // and it is the only one that knows what an AltGr combination on a
      // German keyboard actually produced.
      //
      // Handling only the first gives a field that cannot type an accent.
      // Handling only the second gives one where the arrow keys do nothing.
      case WM_KEYDOWN:
      case WM_SYSKEYDOWN: {
        const bool ctrl = ctrlHeld(), alt = altHeld();
        const int code = self->namedKey((int) wp);

        if (code == KeyPress::None) {
          // ── A modifier combination ──
          //
          // Ctrl+A does not reach WM_CHAR as 'a'. It reaches it as 0x01, the
          // ASCII control code -- and Ctrl+S as 0x13, and Ctrl+[ as 0x1b,
          // which is indistinguishable from Escape. So every shortcut in this
          // SDK was dead: TextEditor compares key.character against 'a' for
          // select-all, and nothing ever delivered that. Select-all, copy, cut
          // and paste by keyboard did NOTHING in any generated plugin, and the
          // unit tests passed because they synthesise a KeyPress the peer
          // could not produce.
          //
          // The letter is recovered from the virtual key here, and ONLY here.
          // Without a modifier, WM_CHAR remains the only correct source --
          // it is the one that has seen the user's layout, their dead keys and
          // their input method, and taking letters from the virtual key would
          // give an American keyboard to everybody.
          if (!ctrl && !alt) break; // an ordinary letter; WM_CHAR has it
          const uint32_t typed = characterForVirtualKey((int) wp);
          if (typed == 0) break;
          KeyPress key;
          key.character = typed;
          key.shiftDown = shiftHeld();
          key.ctrlDown = ctrl;
          key.altDown = alt;
          if (self->router_->keyPressed(key)) {
            self->renderIfDirty();
            return 0;
          }
          break;
        }

        KeyPress key;
        key.keyCode = code;
        key.shiftDown = shiftHeld();
        key.ctrlDown = ctrl;
        key.altDown = alt;
        if (self->router_->keyPressed(key)) {
          self->renderIfDirty();
          return 0;
        }
        break;
      }
      case WM_CHAR: {
        // Surrogates arrive as two separate WM_CHARs. Pairing them is what
        // makes anything outside the basic plane -- an emoji in a preset name
        // -- arrive as one character instead of two broken halves.
        const wchar_t unit = (wchar_t) wp;
        if (unit >= 0xd800 && unit <= 0xdbff) {
          self->pendingHighSurrogate_ = unit;
          return 0;
        }
        uint32_t cp = unit;
        if (unit >= 0xdc00 && unit <= 0xdfff) {
          if (self->pendingHighSurrogate_ == 0) return 0; // an orphan; nothing to pair
          cp = 0x10000u + (((uint32_t) self->pendingHighSurrogate_ - 0xd800u) << 10) +
               ((uint32_t) unit - 0xdc00u);
        }
        self->pendingHighSurrogate_ = 0;
        KeyPress key;
        key.character = cp;
        key.shiftDown = shiftHeld();
        key.ctrlDown = ctrlHeld();
        key.altDown = altHeld();
        if (self->router_->keyPressed(key)) {
          self->renderIfDirty();
          return 0;
        }
        break;
      }
      // ── Files dropped from the desktop ─────────────────────────────────
      //
      // WM_DROPFILES rather than a full IDropTarget. The difference matters and
      // the trade is deliberate: IDropTarget gives live feedback WHILE the
      // pointer is over the window -- a highlight, a copy cursor -- and costs a
      // COM object, an OLE initialisation the host may or may not have done on
      // this thread, and a registration to unwind. WM_DROPFILES gives no
      // feedback until the drop but needs one call and no COM at all.
      //
      // A plugin is a guest in someone else's process. Calling OleInitialize on
      // a thread the host already initialised with different flags fails, and
      // failing to unregister on unload leaves a dangling interface pointer in
      // the shell. So: the simple one, and fileDragEnter is delivered at the
      // moment of the drop rather than before it. The component tree's
      // interface is the same either way, so a peer that CAN do better -- X11
      // must, because XDND requires answering before the release -- does.
      case WM_DROPFILES: {
        HDROP drop = (HDROP) wp;
        std::vector<std::string> files;
        const UINT count = DragQueryFileW(drop, 0xFFFFFFFFu, nullptr, 0);
        for (UINT i = 0; i < count; ++i) {
          const UINT chars = DragQueryFileW(drop, i, nullptr, 0);
          if (chars == 0) continue;
          std::wstring wide((size_t) chars + 1, L'\0');
          DragQueryFileW(drop, i, &wide[0], chars + 1);
          wide.resize(chars);
          const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), (int) wide.size(),
                                                nullptr, 0, nullptr, nullptr);
          std::string utf8((size_t) bytes, '\0');
          if (bytes > 0)
            WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), (int) wide.size(), &utf8[0], bytes,
                                nullptr, nullptr);
          files.push_back(std::move(utf8));
        }
        POINT at{};
        DragQueryPoint(drop, &at);
        // FINISHED before anything else runs. A handler that opens a file
        // dialog or blocks would otherwise hold the shell's memory for as long
        // as it takes, and the shell is waiting on us.
        DragFinish(drop);

        if (!files.empty()) {
          // DragQueryPoint answers in DEVICE pixels like every other Windows
          // coordinate, so it goes through the same conversion. Missed on the
          // first pass through this file, and it would have been invisible:
          // at 200% a dropped file lands on whatever is twice as far down and
          // across, which for a sampler means the wrong pad or none at all.
          const Point p = self->backing_.toLogical(at.x, at.y);
          // Enter and exit around the drop, so a target that highlights on
          // fileDragEnter gets its highlight turned off again -- the
          // WM_DROPFILES path never delivers a hover, and a target written
          // against the X11 one would otherwise stay lit forever.
          self->router_->fileDragMove(files, p);
          self->router_->filesDropped(files, p);
          self->renderIfDirty();
        }
        return 0;
      }

      case WM_CAPTURECHANGED:
        // Something took capture away -- a modal dialog, Alt-Tab. The gesture
        // is over whether the component liked it or not, and leaving the
        // router mid-drag would deliver the next unrelated move as a drag.
        //
        // Harmless after a normal release, because the router has already
        // ended that gesture and has nothing captured to tell.
        self->router_->mouseUp({-1.0f, -1.0f});
        self->renderIfDirty();
        return 0;
      default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
  }

  HWND hwnd_ = nullptr;
  Component* content_ = nullptr;
  MouseRouter* router_ = nullptr;
  Backing backing_;
  uia::RootProvider* uiaRoot_ = nullptr;
  /** What a reader calls this window. Set before open(); a plugin's name is
   *  the only thing distinguishing one editor from another in a host running
   *  twelve of them. */
  std::string accessibleWindowName_ = "Plugin editor";
  std::vector<uint8_t> scratch_;
  /** What the caller ASKED for, kept so a scale change can re-derive the device
   *  size. Recomputing it from the current device size and scale would drift by
   *  a rounding every time somebody dragged the window between monitors. */
  float logicalWidth_ = 0.0f, logicalHeight_ = 0.0f;
  Point lastMouse_;
  bool trackingLeave_ = false;
  bool inTick_ = false;
  bool isPopup_ = false;
  bool grabsMouse_ = true;
  wchar_t pendingHighSurrogate_ = 0;
  static constexpr UINT_PTR kTimerId = 1;
};

} // namespace gfx
} // namespace sonore
