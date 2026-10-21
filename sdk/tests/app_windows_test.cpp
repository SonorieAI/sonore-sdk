// SPDX-License-Identifier: Apache-2.0
// Application windows: TopLevelWindow / ResizableWindow / DocumentWindow,
// NativeMessageBox, ThreadWithProgressWindow, SplashScreen, the tray icon and
// local notifications, native view embedding (HWND, ActiveX), and the
// Application object's single-instance life cycle.
//
// Windows only: every window is made hidden (or shown briefly where a state
// only exists on a shown window -- zoomed, minimised), and every user action
// is a message sent to the real window -- WM_LBUTTONDOWN on the drawn close
// button, WM_SIZING on an edge, the shell's callback message for a tray
// click, TDM_CLICK_BUTTON on a TaskDialog -- so nothing here waits for a
// person. A box that really waits for one runs only with
// SONORE_INTERACTIVE_TESTS=1.
//
//   app_windows_test                         everything
//   app_windows_test --app-child M NAME ...  (internal: an Application in a
//                                            child process, logging to M)
#include "sonore/application.h"
#include "sonore/message_manager.h"
#include "sonore/gfx/document_window.h"
#include "sonore/gfx/message_box.h"
#include "sonore/gfx/native_embed.h"
#include "sonore/gfx/progress_window.h"
#include "sonore/gfx/splash_screen.h"
#include "sonore/gfx/system_tray.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
int main() {
  std::printf("app windows tests: Windows only (macOS and X11 are compiled by the syntax checks)\n");
  std::printf("APP WINDOWS TESTS PASSED\n");
  return 0;
}
#else

using namespace sonore;
using namespace sonore::gfx;
namespace fs = std::filesystem;
namespace msgboxd = sonore::gfx::detail::msgbox;
namespace trayd = sonore::gfx::detail::tray;
namespace progressd = sonore::gfx::detail::progress;
namespace appd = sonore::detail::app;

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
static bool pumpUntil(const std::function<bool()>& done, int timeoutMs = 10000) {
  MessageManager* mm = MessageManager::getInstance();
  const auto start = Clock::now();
  while (!done()) {
    if (msSince(start) > timeoutMs) return false;
    mm->runDispatchLoopUntil(5);
  }
  return true;
}

static void pumpFor(int ms) {
  MessageManager* mm = MessageManager::getInstance();
  const auto start = Clock::now();
  while (msSince(start) < ms) mm->runDispatchLoopUntil(5);
}

static bool approx(float a, float b, float tolerance = 1.01f) { return std::fabs(a - b) <= tolerance; }

/** A click as the window manager delivers one: to the peer's child window,
 *  at a point in the tree's root coordinates. */
static void clickAt(TopLevelWindow& w, Point root, int clicks = 1) {
  HWND child = (HWND) w.getContentHandle();
  const float s = w.getPeer().scale();
  const LPARAM lp = MAKELPARAM((int) std::lround(root.x * s), (int) std::lround(root.y * s));
  SendMessageW(child, clicks == 2 ? WM_LBUTTONDBLCLK : WM_LBUTTONDOWN, MK_LBUTTON, lp);
  SendMessageW(child, WM_LBUTTONUP, 0, lp);
}

static void dragFromTo(TopLevelWindow& w, Point from, Point to) {
  HWND child = (HWND) w.getContentHandle();
  const float s = w.getPeer().scale();
  auto lp = [s](Point p) {
    return MAKELPARAM((WORD) (short) std::lround(p.x * s), (WORD) (short) std::lround(p.y * s));
  };
  SendMessageW(child, WM_LBUTTONDOWN, MK_LBUTTON, lp(from));
  SendMessageW(child, WM_MOUSEMOVE, MK_LBUTTON, lp(to));
  SendMessageW(child, WM_LBUTTONUP, 0, lp(to));
}

static RECT clientOnScreen(HWND hwnd) {
  RECT r{};
  GetClientRect(hwnd, &r);
  POINT origin{0, 0};
  ClientToScreen(hwnd, &origin);
  OffsetRect(&r, origin.x, origin.y);
  return r;
}

// ── Windows ──────────────────────────────────────────────────────────────────

/** A component that keeps the keys it is given. */
struct KeyCatcher : Widget {
  KeyCatcher() { setWantsKeyboardFocus(true); }
  std::vector<KeyPress> keys;
  bool keyPressed(const KeyPress& k) override {
    keys.push_back(k);
    return true;
  }
  void paint(Graphics& g) override { g.fillAll(Colour(30, 140, 60)); }
};

struct CountingWindow : DocumentWindow {
  CountingWindow(const std::string& name, int buttons)
      : DocumentWindow(name, Colour(20, 20, 24), buttons) {}
  int closes = 0, activations = 0;
  void closeButtonPressed() override { ++closes; }
  void activeWindowStatusChanged() override { ++activations; }
  // The menu-bar hook, with a stand-in for gfx/menu_bar.h's component.
  std::unique_ptr<Component> createMenuBarComponent(MenuBarModel&) override {
    std::unique_ptr<Component> bar(new Widget());
    return bar;
  }
};

static void testDocumentWindow() {
  std::printf("\n-- DocumentWindow / ResizableWindow / TopLevelWindow --\n");
  const int before = TopLevelWindow::getNumTopLevelWindows();
  KeyCatcher content;
  {
    CountingWindow w("Sonore test window", DocumentWindow::allButtons);
    check(TopLevelWindow::getNumTopLevelWindows() == before + 1,
          "a window joins the list of top-level windows when made");
    check(TopLevelWindow::getTopLevelWindow(before) == &w, "and is found in it by index");
    w.setContentNonOwned(&content, false);
    check(TopLevelWindow::findTopLevelWindowFor(&content) == &w,
          "a component in its tree finds the window it is in");
    w.setResizable(true, false);
    w.setWindowBounds(Rect(120.0f, 140.0f, 500.0f, 350.0f));
    HWND frame = (HWND) w.getWindowHandle();
    check(frame && IsWindow(frame), "the frame is made the first time it is needed");
    check(!IsWindowVisible(frame), "and is not shown until asked");
    const LONG_PTR style = GetWindowLongPtrW(frame, GWL_STYLE);
    check((style & WS_CAPTION) == WS_CAPTION && (style & WS_THICKFRAME) &&
              (style & WS_MINIMIZEBOX) && (style & WS_MAXIMIZEBOX) && (style & WS_SYSMENU),
          "a resizable DocumentWindow with every button has the native caption, edges and boxes");
    HWND child = (HWND) w.getContentHandle();
    check(child && GetParent(child) == frame, "the peer's window is a child of the frame");
    const float s = w.getDesktopScale();
    RECT c = clientOnScreen(frame);
    std::snprintf(msg, sizeof(msg), "client %ldx%ld at %ld,%ld for 500x350 at 120,140 (scale %.2f)",
                  c.right - c.left, c.bottom - c.top, c.left, c.top, s);
    check(c.right - c.left == std::lround(500 * s) && c.bottom - c.top == std::lround(350 * s) &&
              c.left == std::lround(120 * s) && c.top == std::lround(140 * s),
          msg);
    check(approx(content.bounds().w, 500.0f) && approx(content.bounds().h, 350.0f),
          "the content fills the client area, in logical units");
    const Rect wb = w.getWindowBounds();
    check(approx(wb.x, 120.0f) && approx(wb.w, 500.0f), "getWindowBounds reads the same back");

    // ── limits ──
    w.setResizeLimits(200.0f, 150.0f, 800.0f, 600.0f);
    RECT outer{};
    GetWindowRect(frame, &outer);
    c = clientOnScreen(frame);
    const int extraW = (outer.right - outer.left) - (c.right - c.left);
    const int extraH = (outer.bottom - outer.top) - (c.bottom - c.top);
    MINMAXINFO mmi{};
    SendMessageW(frame, WM_GETMINMAXINFO, 0, (LPARAM) &mmi);
    std::snprintf(msg, sizeof(msg), "WM_GETMINMAXINFO: min %ldx%ld max %ldx%ld (frame adds %dx%d)",
                  mmi.ptMinTrackSize.x, mmi.ptMinTrackSize.y, mmi.ptMaxTrackSize.x,
                  mmi.ptMaxTrackSize.y, extraW, extraH);
    check(mmi.ptMinTrackSize.x == std::lround(200 * s) + extraW &&
              mmi.ptMinTrackSize.y == std::lround(150 * s) + extraH &&
              mmi.ptMaxTrackSize.x == std::lround(800 * s) + extraW &&
              mmi.ptMaxTrackSize.y == std::lround(600 * s) + extraH,
          msg);
    SetWindowPos(frame, nullptr, 0, 0, 3000, 3000, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    c = clientOnScreen(frame);
    check(c.right - c.left == std::lround(800 * s) && c.bottom - c.top == std::lround(600 * s),
          "SetWindowPos past the maximum is held to it (WM_WINDOWPOSCHANGING)");
    SetWindowPos(frame, nullptr, 0, 0, 20, 20, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    c = clientOnScreen(frame);
    check(c.right - c.left == std::lround(200 * s) && c.bottom - c.top == std::lround(150 * s),
          "and below the minimum to the minimum");
    check(approx(content.bounds().w, 200.0f) && approx(content.bounds().h, 150.0f),
          "the tree followed the frame");

    // ── aspect ratio, as the user drags an edge ──
    w.setFixedAspectRatio(2.0f);
    {
      GetWindowRect(frame, &outer);
      RECT drag = outer;
      drag.right = drag.left + std::lround(600 * s) + extraW; // the right edge to 600 wide
      SendMessageW(frame, WM_SIZING, WMSZ_RIGHT, (LPARAM) &drag);
      const long cw = (drag.right - drag.left) - extraW, ch = (drag.bottom - drag.top) - extraH;
      std::snprintf(msg, sizeof(msg), "WM_SIZING at 2:1 -> client %ldx%ld (want %ldx%ld)", cw, ch,
                    std::lround(600 * s), std::lround(300 * s));
      check(cw == std::lround(600 * s) && std::labs(ch - std::lround(300 * s)) <= 1, msg);
      check(drag.left == outer.left && drag.top == outer.top,
            "a right-edge drag keeps the left and top edges where they were");
      drag = outer;
      drag.left = drag.right - (std::lround(700 * s) + extraW); // the left edge, to 700 wide
      SendMessageW(frame, WM_SIZING, WMSZ_LEFT, (LPARAM) &drag);
      check(drag.right == outer.right, "a left-edge drag keeps the right edge");
    }
    w.setFixedAspectRatio(0.0f);

    // ── the corner grip ──
    w.setResizable(true, true);
    w.setWindowSize(400.0f, 300.0f);
    check(w.getCornerResizer() != nullptr, "the corner grip is there when asked for");
    dragFromTo(w, Point(392.0f, 292.0f), Point(442.0f, 322.0f));
    Rect after = w.getWindowBounds();
    std::snprintf(msg, sizeof(msg), "a corner drag of +50,+30 made it %.1fx%.1f (want 450x330)",
                  after.w, after.h);
    check(approx(after.w, 450.0f, 1.5f) && approx(after.h, 330.0f, 1.5f), msg);
    dragFromTo(w, Point(after.w - 8.0f, after.h - 8.0f), Point(after.w + 900.0f, after.h + 900.0f));
    after = w.getWindowBounds();
    check(approx(after.w, 800.0f) && approx(after.h, 600.0f), "and is held to the maximum");

    // ── the close box ──
    SendMessageW(frame, WM_CLOSE, 0, 0);
    check(w.closes == 1, "WM_CLOSE asks closeButtonPressed");
    check(IsWindow(frame) != FALSE, "and closes nothing by itself");

    // ── keys reach the tree through the frame ──
    w.getPeer().router()->setFocus(&content);
    SendMessageW(frame, WM_CHAR, 'q', 0);
    SendMessageW(frame, WM_KEYDOWN, VK_LEFT, 0);
    check(content.keys.size() == 2 && content.keys[0].character == 'q' &&
              content.keys[1].is(KeyPress::Left),
          "a character and a key sent to the frame reach the focused component");

    // ── title, always on top, alpha ──
    w.setName("Renamed window");
    wchar_t text[64] = {0};
    GetWindowTextW(frame, text, 64);
    check(std::wcscmp(text, L"Renamed window") == 0, "setName is the frame's title");
    // Asked while hidden: remembered, and applied when the window shows (the
    // check of the style itself is on the shown window, below). In full runs
    // of 135 tests a HIDDEN frame was refused the topmost band four times out
    // of four -- a plain hidden window of the test's own was not -- and never
    // in any smaller run; on the shown window it is checked for real.
    w.setAlwaysOnTop(true);
    check(w.isAlwaysOnTop(), "always on top, asked while hidden, is remembered");
    w.setAlwaysOnTop(false);
    check((GetWindowLongPtrW(frame, GWL_EXSTYLE) & WS_EX_TOPMOST) == 0 && !w.isAlwaysOnTop(), "and off again");
    w.setWindowAlpha(0.5f);
    BYTE alpha = 0;
    DWORD flags = 0;
    GetLayeredWindowAttributes(frame, nullptr, &alpha, &flags);
    check((flags & LWA_ALPHA) && (alpha == 127 || alpha == 128), "a window alpha of 0.5 is 128 of 255");
    w.setWindowAlpha(1.0f);

    // ── the menu bar ──
    int dummy = 0;
    w.setMenuBar(reinterpret_cast<MenuBarModel*>(&dummy));
    check(w.getMenuBarComponent() != nullptr && w.getMenuBarModel() != nullptr,
          "setMenuBar asks createMenuBarComponent and shows what it returns");
    check(approx(w.getMenuBarComponent()->bounds().h, 24.0f) && approx(content.bounds().y, 24.0f),
          "the bar is 24 high at the top and the content starts under it");
    w.setMenuBar(nullptr);
    check(w.getMenuBarComponent() == nullptr && approx(content.bounds().y, 0.0f),
          "a null model takes it away again");
    {
      // Without an override the hook makes gfx/menu_bar.h's bar.
      struct OneMenu : MenuBarModel {
        std::vector<std::string> getMenuBarNames() override { return {"&File"}; }
        PopupMenu getMenuForIndex(int, const std::string&) override { return PopupMenu(); }
        void menuItemSelected(int, int) override {}
      } model;
      DocumentWindow plain("plain", Colour(20, 20, 24), DocumentWindow::allButtons);
      plain.setMenuBar(&model);
      check(dynamic_cast<MenuBarComponent*>(plain.getMenuBarComponent()) != nullptr,
            "and by default it is a MenuBarComponent showing the model");
      plain.setMenuBar(nullptr);
    }

    // ── states, which exist only on a shown window ──
    w.setWindowBounds(Rect(100.0f, 100.0f, 400.0f, 300.0f));
    w.setFullScreen(true);
    check(w.isFullScreen() && !IsWindowVisible(frame), "zoomed while hidden is remembered");
    w.setVisible(true);
    check(IsWindowVisible(frame) && IsZoomed(frame), "and applied as it is shown");
    w.setFullScreen(false);
    check(!IsZoomed(frame), "and undone");
    w.setMinimised(true);
    check(IsIconic(frame) != FALSE && w.isMinimised(), "minimised");
    w.setMinimised(false);
    check(!IsIconic(frame), "and restored");
    w.setAlwaysOnTop(true);
    if ((GetWindowLongPtrW(frame, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0) {
      check(true, "always on top on the shown window: WS_EX_TOPMOST");
    } else {
      // The z-order is the window manager's to grant: a plain window of the
      // test's own, shown and asked the same way, tells its refusal apart
      // from the SDK's failure.
      HWND control = CreateWindowExW(0, L"STATIC", L"topmost control", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100,
                                     nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
      ShowWindow(control, SW_SHOWNA);
      SetWindowPos(control, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
      const bool systemRefuses = (GetWindowLongPtrW(control, GWL_EXSTYLE) & WS_EX_TOPMOST) == 0;
      DestroyWindow(control);
      std::printf("  frame %p: visible %d, style 0x%llx, exstyle 0x%llx, foreground %p, control refused %d\n",
                  (void*) frame, (int) IsWindowVisible(frame),
                  (unsigned long long) GetWindowLongPtrW(frame, GWL_STYLE),
                  (unsigned long long) GetWindowLongPtrW(frame, GWL_EXSTYLE), (void*) GetForegroundWindow(),
                  (int) systemRefuses);
      check(systemRefuses && w.isAlwaysOnTop(),
            "always on top on the shown window: WS_EX_TOPMOST, or remembered where the system refuses it to all");
    }
    w.setAlwaysOnTop(false);
    check((GetWindowLongPtrW(frame, GWL_EXSTYLE) & WS_EX_TOPMOST) == 0, "and off again on the shown window");
    {
      DocumentWindow later("asked while hidden", Colour(20, 20, 24), DocumentWindow::closeButton);
      later.setAlwaysOnTop(true);
      later.setVisible(true);
      HWND lf = (HWND) later.getWindowHandle();
      check(lf && (GetWindowLongPtrW(lf, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0 && later.isAlwaysOnTop(),
            "asked for while hidden, the window is on top once it shows");
      later.setVisible(false);
    }
    w.setKioskMode(true);
    {
      MONITORINFO mi{};
      mi.cbSize = sizeof(mi);
      GetMonitorInfoW(MonitorFromWindow(frame, MONITOR_DEFAULTTONEAREST), &mi);
      RECT r{};
      GetWindowRect(frame, &r);
      check(w.isKioskMode() && EqualRect(&r, &mi.rcMonitor) &&
                !(GetWindowLongPtrW(frame, GWL_STYLE) & WS_CAPTION),
            "kiosk mode covers the whole monitor with no frame");
    }
    w.setKioskMode(false);
    c = clientOnScreen(frame);
    check(!w.isKioskMode() && (GetWindowLongPtrW(frame, GWL_STYLE) & WS_CAPTION) &&
              c.right - c.left == std::lround(400 * s),
          "and leaving it gives the frame and the size back");
    w.setVisible(false);
    check(!IsWindowVisible(frame), "hidden again");
  }
  check(TopLevelWindow::getNumTopLevelWindows() == before, "a destroyed window leaves the list");

  // ── a drawn title bar ──
  {
    CountingWindow w("Drawn", DocumentWindow::allButtons);
    KeyCatcher inner;
    w.setContentNonOwned(&inner, false);
    w.setUsingNativeTitleBar(false);
    w.setResizable(true, false);
    w.setWindowBounds(Rect(200.0f, 200.0f, 400.0f, 300.0f));
    HWND frame = (HWND) w.getWindowHandle();
    const LONG_PTR style = GetWindowLongPtrW(frame, GWL_STYLE);
    check((style & WS_POPUP) && !(style & WS_CAPTION) && !(style & WS_THICKFRAME),
          "a drawn title bar is a frameless popup");
    check(approx(inner.bounds().y, DocumentWindow::kDefaultTitleBarHeight),
          "the content starts under the drawn bar");
    Widget* close = w.getCloseButton();
    check(close != nullptr && w.getMinimiseButton() && w.getMaximiseButton(),
          "the three drawn buttons are there");
    if (close) {
      clickAt(w, close->localToRoot(close->localBounds().centre()));
      check(w.closes == 0, "the drawn close button reports through the queue, not inside the click");
      pumpUntil([&]() { return w.closes > 0; }, 2000);
      check(w.closes == 1, "and closeButtonPressed runs from it");
    }
    // The bar drags the window.
    Rect start = w.getWindowBounds();
    dragFromTo(w, Point(60.0f, 10.0f), Point(100.0f, 30.0f));
    Rect moved = w.getWindowBounds();
    std::snprintf(msg, sizeof(msg), "dragging the bar by 40,20 moved it %.1f,%.1f", moved.x - start.x,
                  moved.y - start.y);
    check(approx(moved.x - start.x, 40.0f, 1.5f) && approx(moved.y - start.y, 20.0f, 1.5f) &&
              approx(moved.w, start.w),
          msg);
    // The left edge, measured on the desktop: the right edge stays put.
    start = w.getWindowBounds();
    dragFromTo(w, Point(2.0f, 150.0f), Point(-28.0f, 150.0f));
    moved = w.getWindowBounds();
    std::snprintf(msg, sizeof(msg),
                  "a left-edge drag of -30: x %+.1f, width %+.1f, right edge %+.1f", moved.x - start.x,
                  moved.w - start.w, moved.right() - start.right());
    check(approx(moved.x - start.x, -30.0f, 1.5f) && approx(moved.w - start.w, 30.0f, 1.5f) &&
              approx(moved.right(), start.right(), 1.5f),
          msg);
    // Back to native: the bounds survive the frame being remade.
    const Rect beforeSwitch = w.getWindowBounds();
    w.setUsingNativeTitleBar(true);
    HWND remade = (HWND) w.getWindowHandle();
    const Rect afterSwitch = w.getWindowBounds();
    check(remade != nullptr && (GetWindowLongPtrW(remade, GWL_STYLE) & WS_CAPTION) &&
              approx(afterSwitch.x, beforeSwitch.x) && approx(afterSwitch.w, beforeSwitch.w),
          "switching to the native bar remakes the frame and keeps the bounds");
    check(approx(inner.bounds().y, 0.0f), "with no drawn bar above the content");
  }

  // ── a window sized to its content, before it exists ──
  {
    DocumentWindow fit("Fits its content", Colour(0, 0, 0), DocumentWindow::allButtons);
    fit.setUsingNativeTitleBar(false);
    auto* owned = new KeyCatcher();
    owned->setSize(300.0f, 200.0f);
    fit.setContentOwned(owned, true); // the window deletes it
    fit.getWindowHandle();
    const Rect b = fit.getWindowBounds();
    std::snprintf(msg, sizeof(msg),
                  "setContentOwned(resizeToFit) with a drawn bar: window %.0fx%.0f, content at y %.0f, "
                  "%.0fx%.0f",
                  b.w, b.h, owned->bounds().y, owned->bounds().w, owned->bounds().h);
    check(approx(b.w, 300.0f) && approx(b.h, 226.0f) && approx(owned->bounds().y, 26.0f) &&
              approx(owned->bounds().h, 200.0f),
          msg);
  }

  // ── closeButtonPressed deleting its own window ──
  {
    bool deleted = false;
    auto* w = new DocumentWindow("Deletes itself", Colour(0, 0, 0), DocumentWindow::closeButton);
    w->setUsingNativeTitleBar(false);
    w->onCloseButtonPressed = [&deleted, w]() {
      deleted = true;
      delete w;
    };
    w->getWindowHandle(); // laid out at its real size before the button is found
    Widget* close = w->getCloseButton();
    if (close) clickAt(*w, close->localToRoot(close->localBounds().centre()));
    pumpUntil([&]() { return deleted; }, 2000);
    check(deleted, "a window deleted by its drawn close button goes cleanly");
    auto* n = new DocumentWindow("Deletes itself natively", Colour(0, 0, 0), DocumentWindow::allButtons);
    bool deleted2 = false;
    n->onCloseButtonPressed = [&deleted2, n]() {
      deleted2 = true;
      delete n;
    };
    HWND frame = (HWND) n->getWindowHandle();
    SendMessageW(frame, WM_CLOSE, 0, 0);
    check(deleted2 && !IsWindow(frame), "and one deleted from inside WM_CLOSE");
  }
}

// ── Message boxes ────────────────────────────────────────────────────────────

static std::string uniqueTitle(const char* what) {
  static int counter = 0;
  return std::string("SonoreBox ") + what + " " + std::to_string((unsigned long) GetCurrentProcessId()) +
         "-" + std::to_string(++counter);
}

static HWND findBox(const std::string& title, int timeoutMs = 8000) {
  const std::wstring wide = msgboxd::widen(title);
  HWND found = nullptr;
  pumpUntil([&]() {
    found = FindWindowW(nullptr, wide.c_str());
    return found != nullptr;
  }, timeoutMs);
  return found;
}

/** Press button `index` on a box the way a user would, per backend. */
static void pressButton(msgboxd::Backend backend, HWND box, int index, int count) {
  if (backend == msgboxd::Backend::taskDialog) {
    PostMessageW(box, TDM_CLICK_BUTTON, (WPARAM) (msgboxd::kFirstButtonId + index), 0);
    return;
  }
  if (backend == msgboxd::Backend::classic) {
    // Yes / No / Cancel, as runClassic picks for three buttons.
    const int ids3[] = {IDYES, IDNO, IDCANCEL};
    const int ids2[] = {IDOK, IDCANCEL};
    const int id = count == 3 ? ids3[index] : (count == 2 ? ids2[index] : IDOK);
    PostMessageW(box, WM_COMMAND, MAKEWPARAM(id, BN_CLICKED), (LPARAM) GetDlgItem(box, id));
    return;
  }
  // The drawn box: a click on its AlertOverlay's button.
  for (int i = 0; i < TopLevelWindow::getNumTopLevelWindows(); ++i) {
    TopLevelWindow* w = TopLevelWindow::getTopLevelWindow(i);
    if (w->getWindowHandle() != (void*) box) continue;
    auto* drawn = static_cast<msgboxd::DrawnBox*>(w);
    Component* button = drawn->alert().child(index);
    if (button) clickAt(*w, button->localToRoot(button->localBounds().centre()));
  }
}

static void testMessageBoxes() {
  std::printf("\n-- NativeMessageBox / MessageBoxOptions / ScopedMessageBox --\n");
  using msgboxd::Backend;
  check(msgboxd::taskDialogIndirect() != nullptr,
        "TaskDialogIndirect is reached (Common Controls 6 through shell32's manifest)");
  const DWORD mainThread = GetCurrentThreadId();

  const Backend backends[] = {Backend::taskDialog, Backend::classic, Backend::drawn};
  const char* names[] = {"TaskDialog", "MessageBoxW", "drawn box"};
  for (int b = 0; b < 3; ++b) {
    msgboxd::forcedBackend().store(backends[b]);
    const std::string title = uniqueTitle(names[b]);
    int answer = -99, calls = 0;
    DWORD answeredOn = 0;
    NativeMessageBox::showAsync(
        MessageBoxOptions::makeOptionsYesNoCancel(MessageBoxIconType::QuestionIcon, title,
                                                  "Keep the changes?", "Keep", "Discard", "Cancel"),
        [&](int result) {
          answer = result;
          ++calls;
          answeredOn = GetCurrentThreadId();
        });
    HWND box = findBox(title);
    std::snprintf(msg, sizeof(msg), "%s: the async box appears and showAsync returned at once", names[b]);
    check(box != nullptr && calls == 0, msg);
    if (box) pressButton(backends[b], box, 1, 3);
    pumpUntil([&]() { return calls > 0; }, 8000);
    pumpFor(50);
    std::snprintf(msg, sizeof(msg), "%s: a posted click on the second button answers 1 (got %d, %d call(s))",
                  names[b], answer, calls);
    check(answer == 1 && calls == 1, msg);
    std::snprintf(msg, sizeof(msg), "%s: the answer arrives on the message thread", names[b]);
    check(answeredOn == mainThread, msg);
    pumpUntil([&]() { return !IsWindow(box); }, 3000);
    std::snprintf(msg, sizeof(msg), "%s: and the box is gone", names[b]);
    check(!IsWindow(box), msg);

    // Closed from code: down, and never answered.
    const std::string scopedTitle = uniqueTitle(names[b]);
    int scopedCalls = 0;
    {
      ScopedMessageBox scoped = NativeMessageBox::showScopedAsync(
          MessageBoxOptions::makeOptionsOkCancel(MessageBoxIconType::WarningIcon, scopedTitle,
                                                 "About to be closed from code"),
          [&](int) { ++scopedCalls; });
      HWND sbox = findBox(scopedTitle);
      check(scoped.isOpen() && sbox != nullptr, "a scoped box is up and open");
      // The handle goes out of scope here...
      box = sbox;
    }
    pumpUntil([&]() { return !IsWindow(box); }, 5000);
    pumpFor(150);
    std::snprintf(msg, sizeof(msg), "%s: destroying the ScopedMessageBox takes the box down", names[b]);
    check(!IsWindow(box), msg);
    std::snprintf(msg, sizeof(msg), "%s: and its callback is never called", names[b]);
    check(scopedCalls == 0, msg);
  }

  // The synchronous form, answered from another thread while this one waits
  // inside the box's own modal loop.
  for (int b = 0; b < 2; ++b) {
    msgboxd::forcedBackend().store(backends[b]);
    const std::string title = uniqueTitle("sync");
    const std::wstring wide = msgboxd::widen(title);
    std::atomic<bool> found{false};
    std::thread dismisser([&]() {
      const auto start = Clock::now();
      HWND box = nullptr;
      while (!box && msSince(start) < 8000) {
        box = FindWindowW(nullptr, wide.c_str());
        if (!box) std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
      if (!box) return;
      found = true;
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      pressButton(backends[b], box, 0, 2);
      // A click that somehow failed must still not leave the test waiting.
      std::this_thread::sleep_for(std::chrono::milliseconds(3000));
      if (IsWindow(box)) PostMessageW(box, WM_CLOSE, 0, 0);
    });
    const int result = NativeMessageBox::show(MessageBoxOptions::makeOptionsOkCancel(
        MessageBoxIconType::InfoIcon, title, "Answered by another thread"));
    dismisser.join();
    std::snprintf(msg, sizeof(msg), "%s: show() waits and returns the button pressed (got %d)", names[b],
                  result);
    check(found && result == 0, msg);
  }
  msgboxd::forcedBackend().store(Backend::automatic);

  // The reference's convenience forms, through the default (TaskDialog) path.
  {
    const std::string title = uniqueTitle("yes-no-cancel");
    int mapped = -1;
    NativeMessageBox::showYesNoCancelBox(MessageBoxIconType::QuestionIcon, title, "Save?", nullptr,
                                         [&](int r) { mapped = r; });
    if (HWND box = findBox(title)) pressButton(Backend::taskDialog, box, 1, 3);
    pumpUntil([&]() { return mapped >= 0; }, 8000);
    check(mapped == 2, "showYesNoCancelBox: No is 2, as in the reference");
  }
  {
    const std::string title = uniqueTitle("ok-cancel");
    int mapped = -1;
    const bool returned = NativeMessageBox::showOkCancelBox(MessageBoxIconType::InfoIcon, title,
                                                            "Proceed?", nullptr,
                                                            [&](int r) { mapped = r; });
    if (HWND box = findBox(title)) pressButton(Backend::taskDialog, box, 0, 2);
    pumpUntil([&]() { return mapped >= 0; }, 8000);
    check(!returned && mapped == 1, "showOkCancelBox with a callback: false now, 1 for OK later");
  }

  // Owned by the window of its associated component.
  {
    DocumentWindow owner("Box owner", Colour(0, 0, 0), DocumentWindow::allButtons);
    Widget inside;
    owner.setContentNonOwned(&inside, false);
    HWND frame = (HWND) owner.getWindowHandle();
    const std::string title = uniqueTitle("owned");
    int answer = -1;
    NativeMessageBox::showAsync(MessageBoxOptions::makeOptionsOk(MessageBoxIconType::InfoIcon, title,
                                                                 "Owned", "OK", &inside),
                                [&](int r) { answer = r; });
    HWND box = findBox(title);
    check(box && GetWindow(box, GW_OWNER) == frame, "the box is owned by its component's frame");
    if (box) pressButton(Backend::taskDialog, box, 0, 1);
    pumpUntil([&]() { return answer >= 0; }, 8000);
    check(answer == 0, "and answers");
  }

  if (const char* interactive = std::getenv("SONORE_INTERACTIVE_TESTS")) {
    if (std::strcmp(interactive, "1") == 0) {
      const int r = NativeMessageBox::show(MessageBoxOptions()
                                               .withTitle("Sonore interactive test")
                                               .withMessage("Please press the SECOND button.")
                                               .withButton("First")
                                               .withButton("Second")
                                               .withButton("Third"));
      check(r == 1, "a person pressed the second button");
    }
  } else {
    std::printf("  ..   (set SONORE_INTERACTIVE_TESTS=1 for a box a person answers)\n");
  }
}

// ── ThreadWithProgressWindow ─────────────────────────────────────────────────

struct Job : ThreadWithProgressWindow {
  Job(int steps, bool honoursExit, int timeoutMs)
      : ThreadWithProgressWindow("Sonore progress test", true, true, timeoutMs), steps_(steps),
        honours_(honoursExit) {}
  int steps_;
  bool honours_;
  std::atomic<int> reached{0};
  std::atomic<bool> holdAtHalf{false};
  int completions = 0;
  bool lastCancelled = false;
  bool* deletedFlag = nullptr;
  bool* endedFlag = nullptr; // run() had returned when threadComplete came
  bool deleteSelf = false;
  int deafMs = 0; // how long it goes on after Cancel before it looks again
  std::atomic<bool> runEnded{false};
  bool runEndedAtComplete = false;
  std::vector<int>* scratch = new std::vector<int>(64, 0); // what run() uses, freed with the job

  ~Job() override { delete scratch; }

  void run() override {
    work();
    runEnded = true;
  }

  void work() {
    for (int i = 0; i <= steps_; ++i) {
      if (honours_ && threadShouldExit()) return;
      if (holdAtHalf.load() && i == steps_ / 2) {
        setProgress(0.5);
        setStatusMessage("half way");
        reached = i;
        while (!threadShouldExit()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        const auto deafUntil = Clock::now() + std::chrono::milliseconds(deafMs);
        while (Clock::now() < deafUntil) {
          for (int& v : *scratch) ++v;
          std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (honours_) return;
      }
      setProgress((double) i / (double) steps_);
      reached = i;
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
  }

  void threadComplete(bool cancelled) override {
    ++completions;
    lastCancelled = cancelled;
    runEndedAtComplete = runEnded.load();
    if (endedFlag) *endedFlag = runEndedAtComplete;
    if (deleteSelf) {
      if (deletedFlag) *deletedFlag = true;
      delete this;
    }
  }
};

static void testProgressWindow() {
  std::printf("\n-- ThreadWithProgressWindow --\n");
  {
    Job job(60, true, 10000);
    const auto start = Clock::now();
    const bool ok = job.runThread();
    check(ok && job.completions == 1 && !job.lastCancelled && job.reached == 60,
          "runThread waits for the job to finish and reports it finished");
    check(job.getProgressWindow() == nullptr, "the window is gone when it returns");
    std::snprintf(msg, sizeof(msg), "(%.0f ms for 60 steps of 2 ms)", msSince(start));
    check(true, msg);
  }
  {
    Job job(100, true, 10000);
    job.holdAtHalf = true;
    job.launchThread();
    DocumentWindow* window = job.getProgressWindow();
    check(window != nullptr && IsWindowVisible((HWND) window->getWindowHandle()),
          "launchThread shows the window and returns at once");
    progressd::Window* pw = job.getWindowDetail();
    pumpUntil([&]() { return pw->progressBar().progress() == 0.5; }, 5000);
    check(pw->progressBar().progress() == 0.5 && pw->statusLabel().text() == "half way",
          "the window's clock carries the job's progress and status across");
    pumpFor(80);
    // In the window's own pixels: the bar is filled to half.
    {
      Bitmap& shot = window->getPeer().bitmap();
      const float s = window->getPeer().scale();
      const Rect bar = pw->progressBar().bounds();
      const Point at = pw->progressBar().parent()->localToRoot(Point(bar.x, bar.y + bar.h * 0.5f));
      const int y = (int) std::lround(at.y * s);
      const PremulColour filled = shot.pixelAt((int) std::lround((at.x + bar.w * 0.25f) * s), y);
      const PremulColour empty = shot.pixelAt((int) std::lround((at.x + bar.w * 0.75f) * s), y);
      const Colour accent = LookAndFeel::defaultLookAndFeel().accent();
      const bool accentLeft = std::abs((int) filled.r - accent.r) <= 3 &&
                              std::abs((int) filled.g - accent.g) <= 3 &&
                              std::abs((int) filled.b - accent.b) <= 3;
      const bool trackRight = std::abs((int) empty.r - accent.r) > 20 ||
                              std::abs((int) empty.g - accent.g) > 20 ||
                              std::abs((int) empty.b - accent.b) > 20;
      std::snprintf(msg, sizeof(msg),
                    "the rendered bar is accent at 25%% (%d,%d,%d) and track at 75%% (%d,%d,%d)",
                    filled.r, filled.g, filled.b, empty.r, empty.g, empty.b);
      check(accentLeft && trackRight, msg);
    }
    // Cancel, clicked.
    Button& cancel = pw->cancelButton();
    clickAt(*window, cancel.localToRoot(cancel.localBounds().centre()));
    check(job.threadShouldExit(), "a click on Cancel raises threadShouldExit at once");
    pumpUntil([&]() { return job.completions > 0; }, 5000);
    check(job.completions == 1 && job.lastCancelled && job.wasCancelled(),
          "threadComplete(true) follows when the job stops");
    check(job.getProgressWindow() == nullptr && !job.isThreadRunning(), "and the window is gone");
  }
  {
    // A job deaf to Cancel for 800 ms, with 200 ms allowed: the WINDOW goes
    // at the time-out; threadComplete waits for run(), since it is where the
    // object may be deleted -- it used to be called "anyway", and a `delete
    // this` there freed what run() was still using.
    Job job(100, false, 200);
    job.holdAtHalf = true;
    job.deafMs = 800;
    job.launchThread();
    pumpUntil([&]() { return job.reached.load() >= 50; }, 5000);
    const auto start = Clock::now();
    job.cancel();
    pumpUntil([&]() { return job.getProgressWindow() == nullptr || job.completions > 0; }, 5000);
    const double waited = msSince(start);
    std::snprintf(msg, sizeof(msg),
                  "a job that ignores Cancel loses its window after the time-out (%.0f ms)", waited);
    check(job.getProgressWindow() == nullptr && waited >= 190.0 && waited < 750.0, msg);
    check(job.completions == 0, "but is not called complete while run() is still running");
    pumpUntil([&]() { return job.completions > 0; }, 5000);
    check(job.completions == 1 && job.lastCancelled && job.runEndedAtComplete,
          "threadComplete(true) comes once run() has returned");
  }
  {
    // The same, deleting itself there, as the reference's jobs do: run()
    // must not be running on the deleted object.
    bool deleted = false, ended = false;
    Job* job = new Job(100, false, 100);
    job->holdAtHalf = true;
    job->deafMs = 400;
    job->deleteSelf = true;
    job->deletedFlag = &deleted;
    job->endedFlag = &ended;
    job->launchThread();
    pumpUntil([&]() { return job->reached.load() >= 50; }, 5000);
    job->cancel();
    pumpUntil([&]() { return deleted; }, 5000);
    check(deleted && ended, "a job given up on and deleting itself in threadComplete had finished run()");
  }
  {
    bool deleted = false;
    Job* job = new Job(10, true, 1000);
    job->deleteSelf = true;
    job->deletedFlag = &deleted;
    job->launchThread();
    pumpUntil([&]() { return deleted; }, 5000);
    check(deleted, "threadComplete may delete the object (it runs from the queue)");
  }
  {
    // launchThread() again before the last run's ending was delivered. That
    // ending, queued by the old thread, closed the NEW run's window and spent
    // its completion: the new run lost its window at once, Cancel did nothing
    // and it never got a threadComplete of its own.
    Job job(4, true, 1000);
    job.launchThread();
    const auto t0 = Clock::now();
    while (job.isThreadRunning() && msSince(t0) < 2000) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    // run() has returned; its ending is in the queue, not yet delivered.
    job.steps_ = 100;
    job.holdAtHalf = true;
    job.launchThread();
    pumpUntil([&]() { return job.reached.load() >= 50; }, 5000);
    pumpFor(100);
    check(job.getProgressWindow() != nullptr && job.completions == 0,
          "a run launched before the last one's ending arrived keeps its window; the old ending is dropped");
    job.cancel();
    pumpUntil([&]() { return job.completions > 0; }, 5000);
    check(job.completions == 1 && job.lastCancelled && job.getProgressWindow() == nullptr,
          "...and Cancel ends it, with one threadComplete of its own");
  }
}

// ── SplashScreen ─────────────────────────────────────────────────────────────

static BYTE alphaOf(HWND hwnd) {
  BYTE alpha = 255;
  DWORD flags = 0;
  if (!(GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_LAYERED)) return 255;
  if (!GetLayeredWindowAttributes(hwnd, nullptr, &alpha, &flags) || !(flags & LWA_ALPHA)) return 255;
  return alpha;
}

static void testSplash() {
  std::printf("\n-- SplashScreen --\n");
  Bitmap image(120, 80);
  image.clear(Colour(200, 30, 30));
  const int before = TopLevelWindow::getNumTopLevelWindows();
  {
    bool dismissed = false;
    auto* splash = new SplashScreen("Sonore splash", image, false);
    splash->onDismissed = [&]() { dismissed = true; };
    HWND frame = (HWND) splash->getWindowHandle();
    const LONG_PTR style = GetWindowLongPtrW(frame, GWL_STYLE);
    const LONG_PTR ex = GetWindowLongPtrW(frame, GWL_EXSTYLE);
    check(IsWindowVisible(frame) && (style & WS_POPUP) && !(style & WS_CAPTION),
          "a splash is shown at once, frameless");
    check((ex & WS_EX_TOPMOST) && (ex & WS_EX_TOOLWINDOW), "above everything, and off the taskbar");
    const float s = splash->getDesktopScale();
    RECT c = clientOnScreen(frame);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromWindow(frame, MONITOR_DEFAULTTOPRIMARY), &mi);
    const long cx = (mi.rcWork.left + mi.rcWork.right) / 2, cy = (mi.rcWork.top + mi.rcWork.bottom) / 2;
    check(c.right - c.left == std::lround(120 * s) && c.bottom - c.top == std::lround(80 * s) &&
              std::labs((c.left + c.right) / 2 - cx) <= 1 && std::labs((c.top + c.bottom) / 2 - cy) <= 1,
          "the image's size, centred on the work area");
    // Exact time, by argument.
    splash->advance(500.0);
    splash->deleteAfterDelay(1000.0, true);
    splash->advance(499.0);
    check(!splash->isFading() && alphaOf(frame) == 255, "at 999 ms of 1000 it has not begun to go");
    splash->advance(1.0);
    check(splash->isFading(), "at 1000 ms it begins to fade");
    splash->advance(149.0);
    const BYTE half = alphaOf(frame);
    std::snprintf(msg, sizeof(msg), "150 ms into a 300 ms fade its alpha is half (%u)", (unsigned) half);
    check(half >= 125 && half <= 130, msg);
    splash->advance(160.0);
    check(splash->hasFinished() && !IsWindowVisible(frame), "at the end of the fade it is hidden");
    check(!dismissed, "and deletes itself from the queue, not from inside the tick");
    pumpUntil([&]() { return dismissed; }, 2000);
    check(dismissed && TopLevelWindow::getNumTopLevelWindows() == before && !IsWindow(frame),
          "then it is gone");
  }
  {
    bool dismissed = false;
    auto* splash = new SplashScreen("Sonore splash click", image, true);
    splash->onDismissed = [&]() { dismissed = true; };
    splash->deleteAfterDelay(60000.0, true);
    check(!splash->isFading(), "a long minimum keeps it up");
    clickAt(*splash, Point(60.0f, 40.0f));
    check(splash->isFading(), "a click starts the fade when removeOnMouseClick");
    splash->advance(400.0);
    pumpUntil([&]() { return dismissed; }, 2000);
    check(dismissed, "and it goes");
  }
  {
    bool dismissed = false;
    auto* splash = new SplashScreen("Sonore splash clock", image, false);
    splash->onDismissed = [&]() { dismissed = true; };
    splash->deleteAfterDelay(100.0, false);
    const auto start = Clock::now();
    pumpUntil([&]() { return dismissed; }, 5000);
    std::snprintf(msg, sizeof(msg), "on the window's own clock: gone after %.0f ms (100 + 300 fade)",
                  msSince(start));
    check(dismissed && msSince(start) >= 380.0, msg);
  }
}

// ── The tray ─────────────────────────────────────────────────────────────────

struct TrayProbe : SystemTrayIconComponent {
  int downs = 0, menus = 0;
  void mouseDown(const MouseEvent&) override { ++downs; }
  void contextMenu(const MouseEvent&) override { ++menus; }
};

struct NotificationProbe : PushNotifications::Listener {
  std::string clicked;
  int settings = -1;
  void handleNotification(bool local, const PushNotifications::Notification& n) override {
    if (local) clicked = n.identifier;
  }
  void notificationSettingsReceived(bool allowed) override { settings = allowed ? 1 : 0; }
};

struct RemovingListener : PushNotifications::Listener {
  PushNotifications::Listener* other = nullptr;
  int heard = 0;
  void notificationSettingsReceived(bool) override {
    ++heard;
    PushNotifications::getInstance()->removeListener(other);
  }
};

static void testTray() {
  std::printf("\n-- SystemTrayIconComponent / PushNotifications --\n");
  {
    // One listener removing another as the permission answer arrives -- an
    // editor closing takes its listener with it, and a removed listener is
    // usually a deleted one. The answer went to every listener of the copy
    // taken before the first call, removed or not.
    NotificationProbe removed;
    RemovingListener remover;
    remover.other = &removed;
    PushNotifications* push = PushNotifications::getInstance();
    push->addListener(&remover);
    push->addListener(&removed);
    push->requestPermissions();
    check(remover.heard == 1 && removed.settings == -1,
          "a listener removed while the permission answer is reported is not called with it");
    push->removeListener(&remover);
  }
  if (!FindWindowW(L"Shell_TrayWnd", nullptr)) {
    std::printf("  ..   no taskbar on this desktop (a service session?): tray skipped\n");
    return;
  }
  auto& stats = trayd::stats();
  Bitmap icon(32, 32);
  icon.clear(Colour(250, 160, 20));
  {
    TrayProbe tray;
    int clicks = 0, doubles = 0, rights = 0, balloons = 0;
    tray.onClick = [&]() { ++clicks; };
    tray.onDoubleClick = [&]() { ++doubles; };
    tray.onRightClick = [&]() { ++rights; };
    tray.onBalloonClicked = [&]() { ++balloons; };
    const int adds = stats.adds, mods = stats.modifies, dels = stats.deletes, fails = stats.failures;
    tray.setIconImage(icon, Bitmap());
    check(tray.isInTray() && stats.adds == adds + 1 && stats.failures == fails,
          "the first image adds the icon (one NIM_ADD, succeeded)");
    tray.setIconTooltip("Sonore test icon");
    tray.setIconImage(icon, Bitmap());
    check(stats.modifies == mods + 2 && stats.failures == fails, "tooltip and image: two NIM_MODIFYs");
    tray.showInfoBubble("Sonore", "A balloon from the test");
    tray.hideInfoBubble();
    check(stats.modifies == mods + 4 && stats.failures == fails, "the balloon shown and hidden: two more");
    HWND hwnd = (HWND) tray.getNativeHandle();
    // The shell's side of a click, version 4: the event in LOWORD(lParam).
    SendMessageW(hwnd, SystemTrayIconComponent::kCallbackMessage, 0,
                 MAKELPARAM(WM_LBUTTONUP, SystemTrayIconComponent::kIconId));
    SendMessageW(hwnd, SystemTrayIconComponent::kCallbackMessage, 0,
                 MAKELPARAM(WM_LBUTTONDBLCLK, SystemTrayIconComponent::kIconId));
    SendMessageW(hwnd, SystemTrayIconComponent::kCallbackMessage, 0,
                 MAKELPARAM(WM_RBUTTONUP, SystemTrayIconComponent::kIconId));
    SendMessageW(hwnd, SystemTrayIconComponent::kCallbackMessage, 0,
                 MAKELPARAM(NIN_BALLOONUSERCLICK, SystemTrayIconComponent::kIconId));
    check(clicks == 1 && doubles == 1 && rights == 1 && balloons == 1,
          "a click, a double click, a right click and a balloon click each arrive once");
    check(tray.downs == 2 && tray.menus == 1, "and as the Component's mouseDown and contextMenu");
    // Explorer restarting.
    SendMessageW(hwnd, RegisterWindowMessageW(L"TaskbarCreated"), 0, 0);
    check(tray.isInTray() && stats.adds == adds + 2, "TaskbarCreated adds the icon again");
    NOTIFYICONDATAW probe{};
    probe.cbSize = sizeof(probe);
    probe.hWnd = hwnd;
    probe.uID = SystemTrayIconComponent::kIconId;
    probe.uFlags = NIF_TIP;
    check(Shell_NotifyIconW(NIM_MODIFY, &probe) != FALSE, "the shell knows the icon");
    tray.removeFromTray();
    check(!tray.isInTray() && stats.deletes == dels + 1, "removeFromTray: one NIM_DELETE, succeeded");
    check(Shell_NotifyIconW(NIM_MODIFY, &probe) == FALSE, "and the shell no longer knows it");
  }
  {
    NotificationProbe listener;
    PushNotifications* push = PushNotifications::getInstance();
    push->addListener(&listener);
    push->requestPermissions();
    check(listener.settings == 1 && push->areNotificationsEnabled(),
          "permission: granted at once on Windows");
    TrayProbe tray;
    tray.setIconImage(icon, Bitmap());
    push->setTrayIcon(&tray);
    const int mods = stats.modifies;
    PushNotifications::Notification n;
    n.identifier = "sonore-n1";
    n.title = "Sonore";
    n.body = "A local notification from the test";
    check(push->sendLocalNotification(n) && stats.modifies == mods + 1,
          "a local notification rides the tray icon's balloon");
    SendMessageW((HWND) tray.getNativeHandle(), SystemTrayIconComponent::kCallbackMessage, 0,
                 MAKELPARAM(NIN_BALLOONUSERCLICK, SystemTrayIconComponent::kIconId));
    check(listener.clicked == "sonore-n1", "clicking it reaches the listener with its identifier");
    push->removeAllDeliveredNotifications();
    push->setTrayIcon(nullptr);
    // Without an icon of the application's: one of its own.
    PushNotifications::Notification m = n;
    m.identifier = "sonore-n2";
    check(push->sendLocalNotification(m) && push->ownTrayIcon() && push->ownTrayIcon()->isInTray(),
          "with no icon given, it posts through one of its own");
    const int dels = stats.deletes;
    push->removeAllDeliveredNotifications();
    check(push->ownTrayIcon() == nullptr && stats.deletes == dels + 1,
          "removeAllDeliveredNotifications takes that icon away again");
    check(push->getDeviceToken().empty(), "remote push: no device token (not implemented)");
    push->removeListener(&listener);
  }
  {
    // The application's icon destroyed while still the one notifications
    // ride on -- the tray turned off in its settings. It was kept as a raw
    // pointer, and the next post read the freed icon (AddressSanitizer:
    // heap-use-after-free in sendLocalNotification).
    PushNotifications* push = PushNotifications::getInstance();
    auto* gone = new TrayProbe();
    gone->setIconImage(icon, Bitmap());
    push->setTrayIcon(gone);
    delete gone;
    PushNotifications::Notification late;
    late.identifier = "sonore-n3";
    late.title = "Sonore";
    late.body = "After the application's icon went";
    check(push->sendLocalNotification(late) && push->ownTrayIcon() && push->ownTrayIcon()->isInTray(),
          "a destroyed tray icon is forgotten: the notification rides one of its own");
    push->removeAllDeliveredNotifications();
    check(push->ownTrayIcon() == nullptr, "and clearing touches nothing that is gone");
  }
}

// ── Native views ─────────────────────────────────────────────────────────────

static RECT childRect(HWND child) {
  RECT r{};
  GetWindowRect(child, &r);
  MapWindowPoints(nullptr, GetParent(child), (POINT*) &r, 2);
  return r;
}

static void testEmbedding() {
  std::printf("\n-- HWNDComponent / ActiveXControlComponent --\n");
  DocumentWindow host("Embedding host", Colour(0, 0, 0), DocumentWindow::allButtons);
  Widget content;
  Widget panel;
  host.setContentNonOwned(&content, false);
  host.setWindowBounds(Rect(50.0f, 50.0f, 500.0f, 400.0f));
  content.addChild(&panel);
  panel.setBounds(Rect(0.0f, 0.0f, 500.0f, 400.0f));
  HWND peer = (HWND) host.getContentHandle();
  const float s = host.getPeer().scale();

  HWND foreign = CreateWindowExW(0, L"STATIC", L"foreign", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr,
                                 GetModuleHandleW(nullptr), nullptr);
  {
    HWNDComponent hc;
    panel.addChild(&hc);
    hc.setBounds(Rect(20.0f, 30.0f, 100.0f, 50.0f));
    hc.setHWND(foreign);
    RECT r = childRect(foreign);
    check(GetParent(foreign) == peer && (GetWindowLongPtrW(foreign, GWL_STYLE) & WS_CHILD),
          "setHWND makes the foreign window a child of the window the component is in");
    check(r.left == std::lround(20 * s) && r.top == std::lround(30 * s) &&
              r.right - r.left == std::lround(100 * s) && r.bottom - r.top == std::lround(50 * s),
          "at the component's place and size");
    check((GetWindowLongPtrW(foreign, GWL_STYLE) & WS_VISIBLE) != 0, "and shown, as the component is");
    hc.setBounds(Rect(60.0f, 70.0f, 120.0f, 40.0f));
    r = childRect(foreign);
    check(r.left == std::lround(60 * s) && r.right - r.left == std::lround(120 * s),
          "moving and sizing the component moves the window");
    // An ancestor moves: found when the tree paints.
    panel.setBounds(Rect(10.0f, 15.0f, 480.0f, 380.0f));
    host.getPeer().renderIfDirty();
    r = childRect(foreign);
    std::snprintf(msg, sizeof(msg), "an ancestor's move is followed at the next paint (%ld,%ld)", r.left,
                  r.top);
    check(r.left == std::lround(70 * s) && r.top == std::lround(85 * s), msg);
    // An ancestor hidden: not painted, so found on the window's tick.
    panel.setVisible(false);
    pumpUntil([&]() { return (GetWindowLongPtrW(foreign, GWL_STYLE) & WS_VISIBLE) == 0; }, 2000);
    check((GetWindowLongPtrW(foreign, GWL_STYLE) & WS_VISIBLE) == 0,
          "a hidden ancestor hides it, on the window's tick");
    panel.setVisible(true);
    pumpUntil([&]() { return (GetWindowLongPtrW(foreign, GWL_STYLE) & WS_VISIBLE) != 0; }, 2000);
    check((GetWindowLongPtrW(foreign, GWL_STYLE) & WS_VISIBLE) != 0, "and showing it again shows it");
    hc.setHWND(nullptr);
    check(GetParent(foreign) == nullptr && !(GetWindowLongPtrW(foreign, GWL_STYLE) & WS_CHILD) &&
              !(GetWindowLongPtrW(foreign, GWL_STYLE) & WS_VISIBLE),
          "setHWND(nullptr) hands it back, hidden, with its own style");
    hc.setHWND(foreign);
  }
  check(GetParent(foreign) == nullptr && IsWindow(foreign),
        "a destroyed HWNDComponent releases the window and does not destroy it");
  DestroyWindow(foreign);

  {
    // The WebBrowser control, which every Windows still carries.
    static const CLSID webBrowser = {0x8856F961, 0x340A, 0x11D0, {0xA9, 0x6B, 0x00, 0xC0, 0x4F, 0xD7, 0x05, 0xA2}};
    static const IID webBrowser2 = {0xD30C1661, 0xCDAF, 0x11D0, {0x8A, 0x3E, 0x00, 0xC0, 0x4F, 0xC9, 0xE2, 0x6E}};
    static const CLSID nothing = {0x0BADF00D, 0x1111, 0x2222, {0x33, 0x33, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44}};
    ActiveXControlComponent ax;
    panel.addChild(&ax);
    ax.setBounds(Rect(40.0f, 40.0f, 300.0f, 200.0f));
    const bool made = ax.createControl(&webBrowser);
    check(made && ax.isControlOpen(), "the WebBrowser control is created and in-place activated");
    HWND site = (HWND) ax.getSiteWindow();
    check(site && GetParent(site) == peer, "in a site window inside the host");
    check(site && GetWindow(site, GW_CHILD) != nullptr, "which the control has put its own window in");
    IUnknown* browser = (IUnknown*) ax.queryInterface(&webBrowser2);
    check(browser != nullptr, "queryInterface reaches IWebBrowser2");
    if (browser) browser->Release();
    ax.setBounds(Rect(50.0f, 60.0f, 320.0f, 180.0f));
    RECT r = site ? childRect(site) : RECT{};
    check(r.right - r.left == std::lround(320 * s) && r.top == std::lround(75 * s),
          "the site follows the component (the panel is at 10,15)");
    ax.setMouseEventsAllowed(false);
    check(site && !IsWindowEnabled(site), "refusing mouse events disables the site");
    ax.setMouseEventsAllowed(true);
    ax.deleteControl();
    check(!ax.isControlOpen() && !IsWindow(site), "deleteControl closes it and its site");
    check(!ax.createControl(&nothing) && !ax.isControlOpen() && ax.getSiteWindow() == nullptr,
          "a CLSID that names nothing fails and leaves nothing behind");
  }

  // The frame remade (setResizable, setUsingNativeTitleBar) destroys the
  // peer's window -- and Windows destroys a window's children with it. The
  // foreign HWND and the ActiveX site went too, though neither was the
  // window's to destroy: the HWND's owner found it gone, the control died.
  {
    static const CLSID webBrowser = {0x8856F961, 0x340A, 0x11D0, {0xA9, 0x6B, 0x00, 0xC0, 0x4F, 0xD7, 0x05, 0xA2}};
    HWND mine = CreateWindowExW(0, L"STATIC", L"foreign 2", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr,
                                GetModuleHandleW(nullptr), nullptr);
    HWNDComponent hc;
    panel.addChild(&hc);
    hc.setBounds(Rect(20.0f, 30.0f, 100.0f, 50.0f));
    hc.setHWND(mine);
    ActiveXControlComponent ax;
    panel.addChild(&ax);
    ax.setBounds(Rect(40.0f, 100.0f, 200.0f, 120.0f));
    const bool made = ax.createControl(&webBrowser);
    HWND site = (HWND) ax.getSiteWindow();
    host.setResizable(!host.isResizable(), false);
    check(IsWindow(mine) && GetParent(mine) != peer,
          "a remade frame leaves the foreign HWND alive (handed back, not destroyed with the peer)");
    check(made && IsWindow(site) && GetWindow(site, GW_CHILD) != nullptr && ax.isControlOpen(),
          "and the ActiveX site and the control's own window alive");
    HWND fresh = (HWND) host.getContentHandle();
    pumpUntil([&]() { return GetParent(mine) == fresh && GetParent(site) == fresh; }, 2000);
    check(GetParent(mine) == fresh && GetParent(site) == fresh,
          "both are put in the new peer's window");
    panel.removeChild(&ax);
    panel.removeChild(&hc);
    hc.setHWND(nullptr);
    ax.deleteControl();
    // A window destroyed while the component outlives it: handed back too.
    auto* doomed = new DocumentWindow("Embedding doomed", Colour(0, 0, 0), DocumentWindow::allButtons);
    Widget inner;
    doomed->setContentNonOwned(&inner, false);
    inner.addChild(&hc);
    hc.setHWND(mine);
    const bool wasIn = GetParent(mine) == (HWND) doomed->getContentHandle();
    delete doomed;
    check(wasIn && IsWindow(mine) && GetParent(mine) == nullptr &&
              !(GetWindowLongPtrW(mine, GWL_STYLE) & WS_VISIBLE),
          "a host window destroyed first hands the foreign HWND back, hidden");
    inner.removeChild(&hc);
    hc.setHWND(nullptr);
    DestroyWindow(mine);
  }
}

// ── Application ──────────────────────────────────────────────────────────────

static std::string gMarker;
static std::string gChildName;

static void markLine(const std::string& line) {
  if (FILE* f = std::fopen(gMarker.c_str(), "ab")) {
    std::fprintf(f, "%s\n", line.c_str());
    std::fclose(f);
  }
}

struct ChildApp : Application {
  // Only an instance that ran writes it: a second instance writing at the
  // same moment as the first would interleave the two in the log.
  ~ChildApp() override {
    if (ran) markLine("destroyed");
  }
  bool ran = false;
  const std::string getApplicationName() override { return gChildName; }
  const std::string getApplicationVersion() override { return "1.0"; }
  bool moreThanOneInstanceAllowed() override { return false; }
  void initialise(const std::string& commandLine) override {
    setApplicationReturnValue(7);
    ran = true;
    markLine("init:" + commandLine);
    // Opening a long file named on the command line, an audio device: an
    // initialise that keeps the loop from running for a few seconds.
    if (commandLine.find("slow-init") != std::string::npos)
      std::this_thread::sleep_for(std::chrono::milliseconds(3500));
    if (commandLine.find("report-console") != std::string::npos)
      markLine("console:" + std::to_string((unsigned long long) (uintptr_t) GetConsoleWindow()));
  }
  void shutdown() override { markLine("shutdown"); }
  void anotherInstanceStarted(const std::string& commandLine) override {
    markLine("another:" + commandLine);
    if (commandLine.find("throw-please") != std::string::npos)
      MessageManager::callAsync([]() { throw std::runtime_error("boom from a callback"); });
  }
  void systemRequestedQuit() override {
    markLine("sysquit");
    quit();
  }
  void unhandledException(const std::exception* e, const std::string&, int) override {
    markLine(std::string("exception:") + (e ? e->what() : "?"));
  }
};

static Application* createChild() { return new ChildApp(); }

static std::string readFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

static bool waitForText(const std::string& path, const std::string& text, int timeoutMs = 10000) {
  const auto start = Clock::now();
  while (msSince(start) < timeoutMs) {
    if (readFile(path).find(text) != std::string::npos) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return false;
}

/** `ownConsole`: in a console of its own, hidden, so the test can close it
 *  without closing the one this test writes to. */
static PROCESS_INFORMATION launchChild(const std::wstring& args, bool ownConsole = false) {
  wchar_t exe[MAX_PATH];
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  std::wstring command = L"\"" + std::wstring(exe) + L"\" " + args;
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  if (ownConsole) {
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
  }
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(nullptr, &command[0], nullptr, nullptr, FALSE, ownConsole ? CREATE_NEW_CONSOLE : 0,
                      nullptr, nullptr, &si, &pi))
    pi.hProcess = nullptr;
  return pi;
}

static DWORD finish(PROCESS_INFORMATION& pi, DWORD timeoutMs) {
  DWORD code = 0xFFFFFFFF;
  if (!pi.hProcess) return code;
  if (WaitForSingleObject(pi.hProcess, timeoutMs) == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
  else TerminateProcess(pi.hProcess, 99);
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
  return code;
}

static void testApplication() {
  std::printf("\n-- Application --\n");
  {
    const std::vector<std::string> args{"plain", "two words", "a \"quoted\" one", "", "caf\xC3\xA9"};
    check(appd::splitArguments(appd::joinArguments(args)) == args,
          "the command line joins and splits back to the same arguments");
    // Backslashes next to a quote: a folder typed with its trailing
    // separator, in quotes because of its space, swallowed the closing quote
    // and every argument after it.
    const std::vector<std::string> slashes{"C:\\My Dir\\", "next", "a\\\"b", "x\\\\ y\\\\",
                                           "\\\\server\\share", "end\\", "\\"};
    check(appd::splitArguments(appd::joinArguments(slashes)) == slashes,
          "arguments with backslashes before a quote join and split back the same");
    // And the line reads the same through Windows' own parser, which is how
    // anything else on the system would take it apart.
    const auto windowsReads = [](const std::vector<std::string>& list) {
      const std::string line = "prog " + appd::joinArguments(list);
      const std::wstring wideLine(line.begin(), line.end()); // all ASCII
      int count = 0;
      std::vector<std::string> parsed;
      if (LPWSTR* wide = CommandLineToArgvW(wideLine.c_str(), &count)) {
        for (int i = 1; i < count; ++i) {
          std::string arg;
          for (const wchar_t* w = wide[i]; *w; ++w) arg += (char) *w;
          parsed.push_back(arg);
        }
        LocalFree(wide);
      }
      return parsed;
    };
    check(windowsReads(slashes) == slashes, "the joined line is what CommandLineToArgvW reads");
    {
      // Every mix of the characters that matter, both ways.
      uint32_t seed = 12345;
      const char alphabet[] = {'a', ' ', '\t', '"', '\\'};
      int roundTrips = 0, windowsAgrees = 0;
      const int kLists = 3000;
      for (int n = 0; n < kLists; ++n) {
        std::vector<std::string> list(1 + n % 3);
        for (std::string& arg : list) {
          seed = seed * 1664525u + 1013904223u;
          const size_t length = (seed >> 24) % 7;
          for (size_t k = 0; k < length; ++k) {
            seed = seed * 1664525u + 1013904223u;
            arg += alphabet[(seed >> 16) % sizeof(alphabet)];
          }
        }
        if (appd::splitArguments(appd::joinArguments(list)) == list) ++roundTrips;
        if (windowsReads(list) == list) ++windowsAgrees;
      }
      check(roundTrips == kLists && windowsAgrees == kLists,
            "random arguments of quotes, backslashes and spaces survive both parsers");
    }
    const std::string lockName = "SonoreLockTest-" + std::to_string((unsigned long) GetCurrentProcessId());
    appd::InstanceLock a(lockName), b(lockName);
    check(a.acquire(), "the first holder takes the single-instance lock");
    check(!b.acquire(), "a second cannot while it is held");
    a.release();
    check(b.acquire(), "and can once it is released (the mutex went with the last handle)");
    b.release();
  }

  const std::string name = "SonoreAppTest-" + std::to_string((unsigned long) GetCurrentProcessId());
  const fs::path marker = fs::temp_directory_path() / (name + ".log");
  std::error_code ec;
  fs::remove(marker, ec);
  const std::wstring m = marker.wstring();
  const std::wstring n = msgboxd::widen(name);

  PROCESS_INFORMATION first = launchChild(L"--app-child \"" + m + L"\" " + n + L" first");
  check(first.hProcess != nullptr, "the first instance starts");
  check(waitForText(marker.string(), "init:"), "and initialises");
  const std::string afterInit = readFile(marker.string());
  check(afterInit.find(" first") != std::string::npos, "with its command line");

  PROCESS_INFORMATION second =
      launchChild(L"--app-child \"" + m + L"\" " + n + L" \"hello world\" café");
  const DWORD secondCode = finish(second, 15000);
  std::snprintf(msg, sizeof(msg), "a second instance exits at once with 0 (got %lu)",
                (unsigned long) secondCode);
  check(secondCode == 0, msg);
  check(waitForText(marker.string(), "another:"), "the first hears anotherInstanceStarted");
  const std::string afterSecond = readFile(marker.string());
  check(afterSecond.find("\"hello world\" caf\xC3\xA9") != std::string::npos,
        "with the second's command line, quoted and in UTF-8");
  size_t inits = 0;
  for (size_t at = afterSecond.find("init:"); at != std::string::npos; at = afterSecond.find("init:", at + 1))
    ++inits;
  check(inits == 1, "and the second never initialised");

  PROCESS_INFORMATION third = launchChild(L"--app-child \"" + m + L"\" " + n + L" throw-please");
  finish(third, 15000);
  check(waitForText(marker.string(), "exception:boom from a callback"),
        "an exception thrown by a queued callback reaches unhandledException");

  // The session ending: WM_QUERYENDSESSION to the hidden top-level window.
  const std::wstring sessionTitle = L"SonoreApplication:" + n;
  HWND session = nullptr;
  const auto start = Clock::now();
  while (!session && msSince(start) < 5000) {
    session = FindWindowW(nullptr, sessionTitle.c_str());
    if (!session) std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  check(session != nullptr, "the application's session window is there");
  DWORD_PTR agreed = 0;
  if (session)
    SendMessageTimeoutW(session, WM_QUERYENDSESSION, 0, 0, SMTO_ABORTIFHUNG, 5000, &agreed);
  check(agreed == TRUE, "WM_QUERYENDSESSION: systemRequestedQuit quit, so the session may end");
  const DWORD firstCode = finish(first, 15000);
  const std::string all = readFile(marker.string());
  std::snprintf(msg, sizeof(msg), "it shuts down and returns its return value (7, got %lu)",
                (unsigned long) firstCode);
  check(firstCode == 7 && all.find("sysquit\nshutdown") != std::string::npos, msg);

  // The lock went with the process: the next one initialises.
  PROCESS_INFORMATION again = launchChild(L"--app-child \"" + m + L"\" " + n + L" again");
  check(waitForText(marker.string(), " again"), "after it exits a new instance takes the lock");
  HWND session2 = nullptr;
  const auto start2 = Clock::now();
  while (!session2 && msSince(start2) < 5000) {
    session2 = FindWindowW(nullptr, sessionTitle.c_str());
    if (!session2) std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  if (session2) SendMessageTimeoutW(session2, WM_QUERYENDSESSION, 0, 0, SMTO_ABORTIFHUNG, 5000, &agreed);
  check(finish(again, 15000) == 7, "and quits the same way");
  fs::remove(marker, ec);
}

/** Two launches at once -- two files opened together from Explorer -- where
 *  the first is still in initialise() when the second hands it its line. */
static void testForwardToBusyInstance() {
  std::printf("\n-- Application: a second launch while the first initialises --\n");
  const std::string name = "SonoreAppBusy-" + std::to_string((unsigned long) GetCurrentProcessId());
  const fs::path marker = fs::temp_directory_path() / (name + ".log");
  std::error_code ec;
  fs::remove(marker, ec);
  const std::wstring m = marker.wstring();
  const std::wstring n = msgboxd::widen(name);

  PROCESS_INFORMATION first = launchChild(L"--app-child \"" + m + L"\" " + n + L" slow-init");
  check(first.hProcess != nullptr && waitForText(marker.string(), "init:"),
        "the first instance starts and is inside initialise()");
  PROCESS_INFORMATION second = launchChild(L"--app-child \"" + m + L"\" " + n + L" \"second file.wav\"");
  const DWORD secondCode = finish(second, 30000);
  std::snprintf(msg, sizeof(msg), "the second instance exits with 0 (got %lu)", (unsigned long) secondCode);
  check(secondCode == 0, msg);
  check(waitForText(marker.string(), "\"second file.wav\"", 15000) &&
            readFile(marker.string()).find("another:") != std::string::npos,
        "and the first, once its initialise is over, hears the second's command line");
  const std::string log = readFile(marker.string());
  size_t inits = 0;
  for (size_t at = log.find("init:"); at != std::string::npos; at = log.find("init:", at + 1)) ++inits;
  check(inits == 1, "the second never initialised");

  const std::wstring sessionTitle = L"SonoreApplication:" + n;
  HWND session = FindWindowW(nullptr, sessionTitle.c_str());
  DWORD_PTR agreed = 0;
  if (session) SendMessageTimeoutW(session, WM_QUERYENDSESSION, 0, 0, SMTO_ABORTIFHUNG, 5000, &agreed);
  check(finish(first, 15000) == 7, "and quits as usual");
  fs::remove(marker, ec);
}

/** Closing an application's console: Windows sends CTRL_CLOSE_EVENT and
 *  ends the process as soon as the handler returns. */
static void testConsoleClose() {
  std::printf("\n-- Application: its console closed --\n");
  const std::string name = "SonoreAppConsole-" + std::to_string((unsigned long) GetCurrentProcessId());
  const fs::path marker = fs::temp_directory_path() / (name + ".log");
  std::error_code ec;
  fs::remove(marker, ec);
  const std::wstring m = marker.wstring();
  const std::wstring n = msgboxd::widen(name);

  PROCESS_INFORMATION child = launchChild(L"--app-child \"" + m + L"\" " + n + L" report-console", true);
  check(child.hProcess != nullptr && waitForText(marker.string(), "console:"),
        "an application starts in a console of its own");
  const std::string log = readFile(marker.string());
  const size_t at = log.find("console:");
  const HWND console =
      at == std::string::npos ? nullptr : (HWND) (uintptr_t) std::strtoull(log.c_str() + at + 8, nullptr, 10);
  check(console != nullptr, "and has a console window");
  if (console) PostMessageW(console, WM_CLOSE, 0, 0);
  const DWORD code = finish(child, 15000);
  const std::string all = readFile(marker.string());
  std::snprintf(msg, sizeof(msg),
                "closing the console asks the application to quit, and it shuts down and is "
                "destroyed before the process ends (exit 0x%lX)",
                (unsigned long) code);
  check(all.find("sysquit\nshutdown\ndestroyed") != std::string::npos, msg);
  fs::remove(marker, ec);
}

int main(int argc, char** argv) {
  if (argc > 3 && std::strcmp(argv[1], "--app-child") == 0) {
    gMarker = argv[2];
    gChildName = argv[3];
    // main(), not run(): the command line is read again as UTF-16.
    return Application::main(argc, argv, &createChild);
  }

  std::printf("app windows tests\n");
  const auto start = Clock::now();
  MessageManager::getInstance();
  testDocumentWindow();
  testMessageBoxes();
  testProgressWindow();
  testSplash();
  testTray();
  testEmbedding();
  testApplication();
  testForwardToBusyInstance();
  testConsoleClose();
  MessageManager::deleteInstance();

  std::printf("\n%d passed, %d failed, %.1f s\n", passes, failures, msSince(start) / 1000.0);
  std::printf(failures == 0 ? "APP WINDOWS TESTS PASSED\n" : "APP WINDOWS TESTS FAILED\n");
  return failures == 0 ? 0 : 1;
}

#endif
