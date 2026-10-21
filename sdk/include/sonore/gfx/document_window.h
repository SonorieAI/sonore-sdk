// SPDX-License-Identifier: Apache-2.0
//
// Desktop windows for an application: TopLevelWindow, ResizableWindow and
// DocumentWindow -- a component tree in a window of its own, with a title,
// close and minimise boxes, limits on its size, full screen and a menu bar.
//
// ── Built the way a host builds a plugin's window ──────────────────────────
//
// The SDK already had the hard half: the window_*.h peers turn a component
// tree into pixels and the platform's input into component events, inside a
// parent window somebody else made. Here this SDK is that somebody else. A
// TopLevelWindow makes a FRAME -- an HWND, an NSWindow, an X window -- owns
// everything a frame owns (the title, the close box, the size limits, the
// zoomed and minimised states, always-on-top), and opens the very same peer
// inside it as a plugin editor would be opened inside a DAW. Nothing that the
// peers already do is done twice, and every fix to a peer reaches both.
//
// The TopLevelWindow IS the root component, as in the reference: the peer
// paints it, and its children are the content, the drawn title bar, the menu
// bar and the resizers.
//
// ── Native or drawn title bar ──────────────────────────────────────────────
//
// Native by default: the platform's own frame, buttons and edge dragging,
// with the limits enforced where the platform asks for them
// (WM_GETMINMAXINFO, WM_SIZING and WM_WINDOWPOSCHANGING; contentMin/MaxSize;
// WM_NORMAL_HINTS). setUsingNativeTitleBar(false) gives a frameless window
// with a title bar DRAWN by the tree -- its buttons, its double-click to
// zoom and its drag to move are components -- and resizing by a border or a
// corner grip that work in DESKTOP coordinates: a left-edge drag moves the
// window under the pointer, so measuring the drag in the window's own
// coordinates would feed every move back into the next.
//
// ── Units ──────────────────────────────────────────────────────────────────
//
// Everything here is LOGICAL: the units the tree is laid out in. A window's
// bounds are its CLIENT area -- the component -- on the desktop, divided by
// the window's scale on Windows and X11 (whose desktops are in device
// pixels) and in points on macOS (which is logical already).
//
// ── Lifetime ───────────────────────────────────────────────────────────────
//
// The frame is made when it is first needed -- shown, measured, asked for its
// handle -- not in the constructor, because the style it is made with comes
// from getDesktopWindowStyleFlags(), a virtual a base constructor cannot
// reach. A change of style afterwards remakes it. closeButtonPressed() may
// delete the window: the drawn buttons report through the message queue, so
// nothing of the window is on the stack when it goes.
//
// ── What runs where ────────────────────────────────────────────────────────
//
// Windows is RUN: sizes, limits, aspect ratio, the corner and border drags,
// the drawn title bar's buttons and drag, maximise/minimise/kiosk,
// always-on-top, alpha, the close box, keys reaching the tree, and the list
// of windows. The NSWindow and X11 frames are written against their
// documented APIs and only COMPILED here (no Mac, no X server).
#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "../message_manager.h"
#include "component.h"
#include "displays.h"
#include "graphics.h"
#include "menu_bar.h"
#include "resizer.h"
#include "system_font.h"
#include "tooltip.h"
#include "widgets.h"

#if defined(_WIN32)
#include "window_win32.h"
#include "../path_io.h"
#define SONORE_TOP_LEVEL_BACKEND 1
#elif defined(__APPLE__)
#include "window_cocoa.h"
#define SONORE_TOP_LEVEL_BACKEND 1
#elif defined(__linux__)
#include "window_x11.h"
#if defined(SONORE_X11_HEADERS)
#include <sys/timerfd.h>
#define SONORE_TOP_LEVEL_BACKEND 1
#endif
#endif

namespace sonore {
namespace gfx {


class TopLevelWindow;

namespace detail {
namespace tlw {

class Frame;

#if defined(_WIN32)
using PeerWindow = NativeWindow;
#elif defined(__APPLE__)
using PeerWindow = NativeWindowCocoa;
#elif defined(SONORE_X11_HEADERS)
using PeerWindow = NativeWindowX11;
#else
/** No window backend in this build: a peer that never opens. */
class PeerWindow {
public:
  std::function<void()> onTick;
  bool open(void*, Component&, int, int) { return false; }
  void close() {}
  bool isOpen() const { return false; }
  void* handle() const { return nullptr; }
  void setSize(int, int) {}
  float scale() const { return 1.0f; }
  bool renderIfDirty() { return false; }
};
#endif

/** Every live TopLevelWindow, oldest first. */
inline std::vector<TopLevelWindow*>& liveWindows() {
  static std::vector<TopLevelWindow*> windows;
  return windows;
}

/** Things that want a word with every window's tree once a tick -- the
 *  embedded native views (native_embed.h) checking where they now are. */
using TickHook = void (*)(Component& root);
inline std::vector<TickHook>& tickHooks() {
  static std::vector<TickHook> hooks;
  return hooks;
}
inline void addTickHook(TickHook hook) {
  std::vector<TickHook>& hooks = tickHooks();
  if (std::find(hooks.begin(), hooks.end(), hook) == hooks.end()) hooks.push_back(hook);
}

/** Things that must take their native windows OUT of a peer's window before
 *  it is destroyed -- the embedded native views again. Destroying a window
 *  destroys its children, and a foreign HWND or an ActiveX site parented to
 *  the peer is not the peer's to destroy: a frame remade by setResizable
 *  took the caller's window with it. Called with the peer's own handle. */
using DetachHook = void (*)(void* peerWindow);
inline std::vector<DetachHook>& detachHooks() {
  static std::vector<DetachHook> hooks;
  return hooks;
}
inline void addDetachHook(DetachHook hook) {
  std::vector<DetachHook>& hooks = detachHooks();
  if (std::find(hooks.begin(), hooks.end(), hook) == hooks.end()) hooks.push_back(hook);
}

/** Round a logical size to device pixels the way Backing does. */
inline int toDevice(float logical, float scale) { return (int) std::lround(logical * scale); }

} // namespace tlw
} // namespace detail

/**
 * A window on the desktop holding a component tree -- the tree being this
 * object.
 *
 * The base of ResizableWindow and DocumentWindow, and on its own a plain
 * window: SplashScreen and the progress window are TopLevelWindows. Message
 * thread only.
 */
class TopLevelWindow : public Widget {
public:
  /** What the frame should have. The reference's names; combined into the
   *  value getDesktopWindowStyleFlags() returns. */
  enum StyleFlags {
    /** An entry on the taskbar; without it the window is a tool window that
     *  has none (skipped by the taskbar on X11). */
    windowAppearsOnTaskbar = 1 << 0,
    /** A transient window such as a splash: on Windows it is shown without
     *  being activated, on X11 it is typed as a utility window. */
    windowIsTemporary = 1 << 1,
    /** The platform's title bar and frame; without it the window is
     *  frameless and anything that looks like a title bar is drawn. */
    windowHasTitleBar = 1 << 2,
    /** The user may resize it by the frame's edges. */
    windowIsResizable = 1 << 3,
    /** A minimise box in the native title bar (ignored without one). */
    windowHasMinimiseButton = 1 << 4,
    /** A maximise box in the native title bar (ignored without one). */
    windowHasMaximiseButton = 1 << 5,
    /** A close box in the native title bar; with a title bar but without
     *  this, Windows greys out Close in the window menu. */
    windowHasCloseButton = 1 << 6,
    /** A shadow under the window. Only macOS applies it; Windows and X11
     *  draw what their frame draws. */
    windowHasDropShadow = 1 << 7,
  };

  /** `addToDesktop` false leaves it a plain component until addToDesktop(). */
  explicit TopLevelWindow(std::string name = std::string(), bool addToDesktop = true);
  /** Takes the window off the desktop (the peer, then the frame) and out of
   *  the list getTopLevelWindow() reads. A callback queued by the window
   *  sees lifetimeToken() expired and does nothing. */
  ~TopLevelWindow() override;

  TopLevelWindow(const TopLevelWindow&) = delete;
  TopLevelWindow& operator=(const TopLevelWindow&) = delete;

  // ── On the desktop ────────────────────────────────────────────────────────

  /** Give it a window (made now). False when the platform has no window
   *  backend in this build or refused one. */
  bool addToDesktop();
  /** Take the window away; the component tree stays. */
  void removeFromDesktop();
  /** Has a window -- made, or to be made the moment one is needed. */
  bool isOnDesktop() const { return frame_ != nullptr || wantsDesktop_; }

  /** The frame: the HWND, the NSWindow*, or the X window id. Makes the frame
   *  if it was still to be made. */
  void* getWindowHandle();
  /** The peer's window inside the frame -- what an embedded native view is
   *  parented to. */
  void* getContentHandle();
  /** The peer itself, for a test or a platform call nothing here makes. */
  detail::tlw::PeerWindow& getPeer() { return peer_; }

  // ── Title, visibility, order ─────────────────────────────────────────────

  /** The window's title: the native title bar's text, the drawn title
   *  bar's, and the accessible name. Takes effect at once if the frame
   *  exists. */
  void setName(const std::string& title);
  /** The title set by the constructor or setName(). */
  const std::string& getName() const { return title_; }

  /**
   * Show or hide the WINDOW (and the component). Shadows Component's
   * setVisible on purpose, as the reference's does: through a TopLevelWindow
   * it is the window that appears. Showing activates it.
   */
  void setVisible(bool shouldBeVisible);
  /** True when the frame exists and the platform shows it; false before
   *  the frame is made. */
  bool isWindowVisible() const;

  /** Raise the window above the others, making the frame if it is still to
   *  be made. With `takeKeyboardFocus` it also becomes the active window;
   *  without, the active window stays active. */
  void toFront(bool takeKeyboardFocus);
  /** The window has the platform's focus: the one the user is working in. */
  bool isActiveWindow() const;

  /** Keep the window above every window that is not itself on top. Kept
   *  when the frame is remade, and applied when it is first made. */
  void setAlwaysOnTop(bool shouldStayOnTop);
  /** What setAlwaysOnTop() last asked for. */
  bool isAlwaysOnTop() const { return alwaysOnTop_; }

  /** The platform's frame (true, the default) or a frameless window whose
   *  title bar is drawn (DocumentWindow). Remakes the frame. */
  void setUsingNativeTitleBar(bool useNative);
  /** True (the default) for the platform's frame, false for a drawn one. */
  bool isUsingNativeTitleBar() const { return nativeTitleBar_; }

  /** The whole window's opacity, 0..1: a fade. */
  void setWindowAlpha(float alpha);
  /** The opacity, 0..1 (values set outside it are clamped). */
  float getWindowAlpha() const { return alpha_; }

  // ── Geometry (logical, the client area on the desktop) ───────────────────

  /** The client area on the desktop, in logical units: as the platform has
   *  it once the frame exists, otherwise the bounds it will be made with. */
  Rect getWindowBounds() const;
  /** Moves and sizes the window; the size is held to the constrainer. */
  void setWindowBounds(const Rect& bounds);
  /** Keep the position, change the size (held to the constrainer). */
  void setWindowSize(float width, float height);
  /** Keep the size, move the client area's top-left corner to (x, y) on
   *  the desktop. */
  void setTopLeftPosition(float x, float y);
  /** Size it and put it in the middle of the main display's work area. */
  void centreWithSize(float width, float height);
  /** Device pixels per logical unit for this window (1 on macOS). */
  float getDesktopScale() const;

  /** The size limits a drag and a programmatic resize obey. */
  const BoundsConstrainer& getConstrainer() const { return constrainer_; }
  /** Replace the limits (copied). The frame's own minimum and maximum
   *  follow, and a window already outside them is brought inside. */
  void setConstrainer(const BoundsConstrainer& constrainer);

  // ── The list of windows ──────────────────────────────────────────────────

  /** How many TopLevelWindow objects exist, on the desktop or not. */
  static int getNumTopLevelWindows() { return (int) detail::tlw::liveWindows().size(); }
  /** The window at `index`, 0-based in the order they were constructed
   *  (oldest first); null outside 0..getNumTopLevelWindows()-1. */
  static TopLevelWindow* getTopLevelWindow(int index) {
    const auto& all = detail::tlw::liveWindows();
    return index >= 0 && index < (int) all.size() ? all[(size_t) index] : nullptr;
  }
  /** The one the user is working in, or null. */
  static TopLevelWindow* getActiveTopLevelWindow() {
    for (TopLevelWindow* w : detail::tlw::liveWindows())
      if (w->isActiveWindow()) return w;
    return nullptr;
  }
  /** The window whose tree `c` is in, or null. */
  static TopLevelWindow* findTopLevelWindowFor(const Component* c) {
    const Component* root = c;
    while (root && root->parent()) root = root->parent();
    for (TopLevelWindow* w : detail::tlw::liveWindows())
      if (w == root) return w;
    return nullptr;
  }

  // ── Overridables ─────────────────────────────────────────────────────────

  /** The style the frame is made with. Overridden by every subclass that
   *  adds something; call setUsingNativeTitleBar or recreateFrame() after a
   *  change for it to take effect. */
  virtual int getDesktopWindowStyleFlags() const {
    int flags = windowAppearsOnTaskbar | windowHasDropShadow;
    if (nativeTitleBar_) flags |= windowHasTitleBar | windowHasCloseButton | windowHasMinimiseButton;
    return flags;
  }

  /** The OS asked for the window to close: its close box, Alt+F4, the
   *  Dock's quit. Nothing closes unless this does something. */
  virtual void userTriedToCloseWindow() {}

  /** It became, or stopped being, the window the user is working in. */
  virtual void activeWindowStatusChanged() {}

  /** About thirty times a second while the window is open (the peer's
   *  clock), with the milliseconds since the last. */
  virtual void windowTick(double elapsedMs) { (void) elapsedMs; }
  /** Called on the same tick, right after windowTick(), with the same
   *  milliseconds (0 on the first tick). It may delete the window. */
  std::function<void(double elapsedMs)> onTick;

  /** Fills the window with the look and feel's background. */
  void paint(Graphics& g) override { g.fillAll(lookAndFeel().background()); }

  /** Remake the frame with the current style flags, keeping the bounds, the
   *  visibility and the states that survive it. */
  void recreateFrame();

  /** How the frame is now: zoomed, minimised, the whole screen. */
  bool frameIsMaximised() const;
  /** Minimised (or, on Windows, asked to be while still hidden); false
   *  without a frame. */
  bool frameIsMinimised() const;
  /** Covering the whole monitor with no frame (setFrameKiosk). */
  bool frameIsKiosk() const;
  /** Zoom the frame to the work area, or restore it. Makes the frame if it
   *  was still to be made; on Windows a hidden window takes the state as it
   *  is shown. */
  void setFrameMaximised(bool shouldBe);
  /** Minimise the frame, or restore it; made if still to be made, and on
   *  Windows applied as a hidden window is shown. */
  void setFrameMinimised(bool shouldBe);
  /** The whole monitor with no frame, or back to where and how the window
   *  was before. Makes the frame if it was still to be made. */
  void setFrameKiosk(bool shouldBe);

  /** The window's icon: the title bar's and the task switcher's. */
  void setIcon(const Bitmap& icon);

protected:
  /** Make the frame if it is wanted and not made yet. */
  bool ensureFrame();
  /** The frame is made (unlike isOnDesktop, which is also true while it is
   *  still to be made). */
  bool hasFrame() const { return frame_ != nullptr; }
  /** A token whose expiry tells a queued callback the window is gone. */
  std::weak_ptr<char> lifetimeToken() const { return alive_; }
  /** Called when the constrainer changed, for subclasses holding copies. */
  virtual void constrainerChanged() {}

private:
  friend class detail::tlw::Frame;

  // Called by the frame.
  void frameClientResized(int deviceW, int deviceH);
  void frameCloseRequested() { userTriedToCloseWindow(); }
  void frameActiveChanged(bool active) {
    if (active == active_) return;
    active_ = active;
    activeWindowStatusChanged();
    repaint();
  }
  /** The constrainer, in device pixels of the client area: w and h are
   *  replaced with the nearest allowed size. */
  void frameConstrain(float scale, int* w, int* h) const;
  /** The client limits in device pixels; 0 = none. */
  void frameLimits(float scale, int* minW, int* minH, int* maxW, int* maxH) const;
  void* peerHandle() const { return peer_.isOpen() ? peer_.handle() : nullptr; }
  void peerTicked();
  /** The peer closed, the detach hooks told first. */
  void closePeer();

  std::string title_;
  std::unique_ptr<detail::tlw::Frame> frame_;
  mutable detail::tlw::PeerWindow peer_;
  BoundsConstrainer constrainer_;
  Rect windowBounds_{0.0f, 0.0f, 400.0f, 300.0f};
  bool centreOnCreate_ = true;
  bool wantsDesktop_ = false;
  bool creating_ = false;
  bool visible_ = false;
  bool active_ = false;
  bool alwaysOnTop_ = false;
  bool nativeTitleBar_ = true;
  float alpha_ = 1.0f;
  Bitmap icon_;
  std::chrono::steady_clock::time_point lastTick_{};
  bool ticked_ = false;
  std::shared_ptr<char> alive_ = std::make_shared<char>('\0');
};

namespace detail {
namespace tlw {

/**
 * The drag zones of a frameless window, measured on the DESKTOP.
 *
 * ResizableBorder measures a drag in the window's coordinates, which is right
 * for a plugin editor (whose origin never moves) and wrong here: dragging the
 * left edge moves the window, the window's coordinates move with it, and the
 * drag would read its own result as the next movement.
 */
class WindowBorder : public ResizableBorder {
public:
  std::function<Rect()> currentBounds;
  std::function<void(const Rect&)> applyBounds;

  void mouseDown(const MouseEvent& e) override {
    held_ = zoneAt(e.position);
    if (held_ == ResizeZone::None || !currentBounds) return;
    start_ = currentBounds();
    pointer_ = Point(start_.x + e.rootPosition.x, start_.y + e.rootPosition.y);
  }

  void mouseDrag(const MouseEvent& e) override {
    if (held_ == ResizeZone::None || !currentBounds || !applyBounds) return;
    const Rect now = currentBounds();
    const float dx = now.x + e.rootPosition.x - pointer_.x;
    const float dy = now.y + e.rootPosition.y - pointer_.y;
    const bool left = held_ == ResizeZone::Left || held_ == ResizeZone::TopLeft ||
                      held_ == ResizeZone::BottomLeft;
    const bool right = held_ == ResizeZone::Right || held_ == ResizeZone::TopRight ||
                       held_ == ResizeZone::BottomRight;
    const bool top = held_ == ResizeZone::Top || held_ == ResizeZone::TopLeft ||
                     held_ == ResizeZone::TopRight;
    const bool bottom = held_ == ResizeZone::Bottom || held_ == ResizeZone::BottomLeft ||
                        held_ == ResizeZone::BottomRight;
    float w = start_.w + (right ? dx : 0.0f) - (left ? dx : 0.0f);
    float h = start_.h + (bottom ? dy : 0.0f) - (top ? dy : 0.0f);
    constrainer.constrain(&w, &h);
    // The edge that was NOT grabbed stays where it was.
    const float x = left ? start_.right() - w : start_.x;
    const float y = top ? start_.bottom() - h : start_.y;
    applyBounds(Rect(x, y, w, h));
  }

  void mouseUp(const MouseEvent&) override { held_ = ResizeZone::None; }

private:
  ResizeZone held_ = ResizeZone::None;
  Rect start_;
  Point pointer_;
};

/** A drawn title-bar button: a shape rather than a glyph, so it needs no
 *  font that has one. */
class TitleBarButton : public Widget {
public:
  enum class Kind { minimise, maximise, close };

  explicit TitleBarButton(Kind kind) : kind_(kind) {
    setAccessibleName(kind == Kind::close ? "Close" : (kind == Kind::minimise ? "Minimise" : "Maximise"));
  }

  std::function<void()> onClick;
  Kind kind() const { return kind_; }

  void paint(Graphics& g) override {
    LookAndFeel& lf = lookAndFeel();
    const Rect b = localBounds();
    if (pressed_ || hovered_) {
      g.setColour(kind_ == Kind::close ? Colour(232, 17, 35) : lf.outline());
      g.fillRect(b);
    }
    g.setColour(kind_ == Kind::close && (pressed_ || hovered_) ? Colour(255, 255, 255) : lf.text());
    const float s = (std::min)(b.w, b.h) * 0.18f;
    const Point c = b.centre();
    switch (kind_) {
      case Kind::minimise: g.drawLine(c.x - s, c.y, c.x + s, c.y, 1.2f); break;
      case Kind::maximise: g.drawRect(Rect(c.x - s, c.y - s, s * 2.0f, s * 2.0f), 1.2f); break;
      case Kind::close:
        g.drawLine(c.x - s, c.y - s, c.x + s, c.y + s, 1.2f);
        g.drawLine(c.x - s, c.y + s, c.x + s, c.y - s, 1.2f);
        break;
    }
  }

  AccessibleInfo accessibleInfo() const override {
    AccessibleInfo info = baseInfo(AccessibleRole::Button);
    return info;
  }

  void mouseDown(const MouseEvent&) override {
    pressed_ = true;
    repaint();
  }
  void mouseDrag(const MouseEvent& e) override {
    const bool inside = localBounds().contains(e.position);
    if (inside != pressed_) {
      pressed_ = inside;
      repaint();
    }
  }
  void mouseUp(const MouseEvent& e) override {
    const bool fire = pressed_ && localBounds().contains(e.position);
    pressed_ = false;
    repaint();
    if (fire && onClick) onClick();
  }

private:
  Kind kind_;
  bool pressed_ = false;
};

/** The drawn title bar: the title, the buttons, a drag that moves the window
 *  and a double-click that zooms it. */
class TitleBar : public Widget {
public:
  TitleBar()
      : minimise_(TitleBarButton::Kind::minimise), maximise_(TitleBarButton::Kind::maximise),
        close_(TitleBarButton::Kind::close) {
    addChild(&minimise_);
    addChild(&maximise_);
    addChild(&close_);
    setAccessibilityIgnored(true);
  }

  std::function<Rect()> currentBounds;
  std::function<void(float x, float y)> moveTo;
  std::function<void()> onDoubleClick;
  std::function<bool()> isActive;
  /** Read when painting: the window's name, whose change repaints the whole
   *  tree anyway. */
  std::function<std::string()> title;
  void setButtons(int required, bool onLeft) {
    required_ = required;
    onLeft_ = onLeft;
    resized();
    repaint();
  }
  void setTextCentred(bool centred) {
    centred_ = centred;
    repaint();
  }

  TitleBarButton& minimiseButton() { return minimise_; }
  TitleBarButton& maximiseButton() { return maximise_; }
  TitleBarButton& closeButton() { return close_; }

  void resized() override {
    const Rect b = localBounds();
    const float size = b.h;
    // Close outermost, as on every desktop that puts them on that side.
    TitleBarButton* order[3] = {&close_, &maximise_, &minimise_};
    const int bits[3] = {4, 2, 1};
    float edge = onLeft_ ? 0.0f : b.w;
    textLeft_ = 0.0f;
    textRight_ = b.w;
    for (int i = 0; i < 3; ++i) {
      const bool wanted = (required_ & bits[i]) != 0;
      order[i]->setVisible(wanted);
      if (!wanted) continue;
      if (onLeft_) {
        order[i]->setBounds(Rect(edge, 0.0f, size, size));
        edge += size;
      } else {
        edge -= size;
        order[i]->setBounds(Rect(edge, 0.0f, size, size));
      }
    }
    if (onLeft_) textLeft_ = edge;
    else textRight_ = edge;
  }

  void paint(Graphics& g) override {
    LookAndFeel& lf = lookAndFeel();
    g.fillAll(lf.panel());
    g.setColour(lf.outline());
    g.fillRect(Rect(0.0f, bounds().h - 1.0f, bounds().w, 1.0f));
    const bool active = !isActive || isActive();
    WidgetState state = stateFor(false);
    state.enabled = active; // an inactive window's title is drawn dimmed
    const float pad = 8.0f;
    const Rect text(textLeft_ + pad, 0.0f, (std::max)(0.0f, textRight_ - textLeft_ - pad * 2.0f),
                    bounds().h);
    lf.drawLabel(g, text, title ? title() : std::string(), font(),
                 centred_ ? Justify::Centred : Justify::Left, state);
  }

  void mouseDown(const MouseEvent& e) override {
    if (e.clickCount >= 2) {
      dragging_ = false;
      if (onDoubleClick) onDoubleClick();
      return;
    }
    if (!currentBounds) return;
    start_ = currentBounds();
    pointer_ = Point(start_.x + e.rootPosition.x, start_.y + e.rootPosition.y);
    dragging_ = true;
  }

  void mouseDrag(const MouseEvent& e) override {
    if (!dragging_ || !currentBounds || !moveTo) return;
    // On the desktop, for the reason WindowBorder gives: the window moves
    // under the pointer, and its own coordinates move with it.
    const Rect now = currentBounds();
    const float x = now.x + e.rootPosition.x, y = now.y + e.rootPosition.y;
    moveTo(start_.x + (x - pointer_.x), start_.y + (y - pointer_.y));
  }

  void mouseUp(const MouseEvent&) override { dragging_ = false; }

private:
  TitleBarButton minimise_, maximise_, close_;
  int required_ = 7;
  bool onLeft_ = false;
  bool centred_ = true;
  float textLeft_ = 0.0f, textRight_ = 0.0f;
  bool dragging_ = false;
  Rect start_;
  Point pointer_;
};

// ── The frames ───────────────────────────────────────────────────────────────

#if defined(_WIN32)

/** The window's DPI scale, resolved at run time (Windows 10 1607+), with the
 *  system's as the fallback -- as window_win32.h does for the peer. */
inline float frameDpiScale(HWND hwnd) {
  typedef UINT(WINAPI * GetDpiForWindowFn)(HWND);
  static const GetDpiForWindowFn forWindow = [] {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    return user32 ? (GetDpiForWindowFn) (void*) GetProcAddress(user32, "GetDpiForWindow") : nullptr;
  }();
  if (forWindow && hwnd) {
    const UINT dpi = forWindow(hwnd);
    if (dpi > 0) return (float) dpi / 96.0f;
  }
  HDC dc = GetDC(nullptr);
  if (!dc) return 1.0f;
  const int dpi = GetDeviceCaps(dc, LOGPIXELSX);
  ReleaseDC(nullptr, dc);
  return dpi > 0 ? (float) dpi / 96.0f : 1.0f;
}

/**
 * An HICON from a Bitmap, `size` pixels square: the window icon and the tray
 * icon. A 32-bit colour bitmap with STRAIGHT alpha (icons are not
 * premultiplied, the rasteriser's pixels are) and an all-zero mask, which
 * tells Windows to use the alpha. The caller destroys it.
 */
inline HICON iconFromBitmap(const Bitmap& source, int size) {
  if (source.isEmpty() || size <= 0) return nullptr;
  Bitmap scaled(size, size);
  {
    Graphics g(scaled);
    g.drawImage(source, Rect(0.0f, 0.0f, (float) size, (float) size));
  }
  BITMAPV5HEADER header{};
  header.bV5Size = sizeof(header);
  header.bV5Width = size;
  header.bV5Height = -size; // top-down
  header.bV5Planes = 1;
  header.bV5BitCount = 32;
  header.bV5Compression = BI_BITFIELDS;
  header.bV5RedMask = 0x00FF0000;
  header.bV5GreenMask = 0x0000FF00;
  header.bV5BlueMask = 0x000000FF;
  header.bV5AlphaMask = 0xFF000000;
  void* bits = nullptr;
  HDC dc = GetDC(nullptr);
  HBITMAP colour = CreateDIBSection(dc, (BITMAPINFO*) &header, DIB_RGB_COLORS, &bits, nullptr, 0);
  ReleaseDC(nullptr, dc);
  if (!colour || !bits) {
    if (colour) DeleteObject(colour);
    return nullptr;
  }
  auto* out = static_cast<uint8_t*>(bits);
  const uint8_t* in = scaled.data();
  for (int i = 0; i < size * size; ++i) {
    const uint8_t a = in[i * 4 + 3];
    auto unpremultiply = [a](uint8_t v) {
      return a == 0 ? (uint8_t) 0 : (uint8_t) (std::min)(255, ((int) v * 255 + a / 2) / a);
    };
    out[i * 4 + 0] = unpremultiply(in[i * 4 + 2]);
    out[i * 4 + 1] = unpremultiply(in[i * 4 + 1]);
    out[i * 4 + 2] = unpremultiply(in[i * 4 + 0]);
    out[i * 4 + 3] = a;
  }
  HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
  ICONINFO info{};
  info.fIcon = TRUE;
  info.hbmMask = mask;
  info.hbmColor = colour;
  HICON icon = mask ? CreateIconIndirect(&info) : nullptr;
  if (mask) DeleteObject(mask);
  DeleteObject(colour);
  return icon;
}

/**
 * The Win32 frame: a top-level HWND whose client area the peer's child window
 * fills.
 *
 * The frame answers the frame's questions -- how big it may be, whether it
 * closes -- and passes the keyboard to the child, which is where the tree's
 * focus lives. The child takes the OS focus only while text is being typed
 * (window_win32.h's syncKeyboardFocus) and hands it back to its parent
 * otherwise; here the parent is the frame, so keys arriving at the frame are
 * forwarded rather than lost.
 */
class Frame {
public:
  explicit Frame(TopLevelWindow& owner) : owner_(&owner) {}
  ~Frame() { destroy(); }
  Frame(const Frame&) = delete;
  Frame& operator=(const Frame&) = delete;

  bool create(int flags, const std::string& title, const Rect& logical, bool centre,
              bool topMost) {
    destroy();
    flags_ = flags;
    const wchar_t* cls = windowClass();
    if (!cls) return false;
    style_ = WS_CLIPCHILDREN;
    if (flags & TopLevelWindow::windowHasTitleBar) {
      style_ |= WS_CAPTION | WS_SYSMENU;
      if (flags & TopLevelWindow::windowIsResizable) style_ |= WS_THICKFRAME;
      if (flags & TopLevelWindow::windowHasMinimiseButton) style_ |= WS_MINIMIZEBOX;
      if (flags & TopLevelWindow::windowHasMaximiseButton) style_ |= WS_MAXIMIZEBOX;
    } else {
      style_ |= WS_POPUP;
      // A frameless window on the taskbar still minimises and restores from
      // it, which takes these two bits; they draw nothing without a caption.
      if (flags & TopLevelWindow::windowAppearsOnTaskbar)
        style_ |= WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;
    }
    exStyle_ = (flags & TopLevelWindow::windowAppearsOnTaskbar) ? WS_EX_APPWINDOW : WS_EX_TOOLWINDOW;
    if (topMost) exStyle_ |= WS_EX_TOPMOST;
    hwnd_ = CreateWindowExW(exStyle_, cls, pathio::widen(title.c_str()).c_str(), style_,
                            CW_USEDEFAULT, CW_USEDEFAULT, 200, 200, nullptr, nullptr, module(),
                            this);
    if (!hwnd_) return false;
    if ((flags & TopLevelWindow::windowHasTitleBar) &&
        !(flags & TopLevelWindow::windowHasCloseButton))
      if (HMENU menu = GetSystemMenu(hwnd_, FALSE))
        EnableMenuItem(menu, SC_CLOSE, MF_BYCOMMAND | MF_GRAYED);
    const float s = scale();
    const int w = toDevice(logical.w, s), h = toDevice(logical.h, s);
    if (centre) {
      const RECT work = workAreaOf(hwnd_);
      setClientDevice(work.left + ((work.right - work.left) - w) / 2,
                      work.top + ((work.bottom - work.top) - h) / 2, w, h, true);
    } else {
      setClientDevice(toDevice(logical.x, s), toDevice(logical.y, s), w, h, true);
    }
    return true;
  }

  void destroy() {
    if (kiosk_) kiosk_ = false;
    if (hwnd_) {
      HWND gone = hwnd_;
      hwnd_ = nullptr;
      // Cleared first, so the WM_DESTROY family reaches DefWindowProc and not
      // an object that is going.
      SetWindowLongPtrW(gone, GWLP_USERDATA, 0);
      DestroyWindow(gone);
    }
    if (iconBig_) DestroyIcon(iconBig_);
    if (iconSmall_) DestroyIcon(iconSmall_);
    iconBig_ = iconSmall_ = nullptr;
  }

  void* handle() const { return hwnd_; }
  void* contentParent() const { return hwnd_; }
  float scale() const { return frameDpiScale(hwnd_); }

  void peerOpened(PeerWindow&) {}

  void setTitle(const std::string& title) {
    if (hwnd_) SetWindowTextW(hwnd_, pathio::widen(title.c_str()).c_str());
  }

  void show(bool visible) {
    if (!hwnd_) return;
    if (!visible) {
      ShowWindow(hwnd_, SW_HIDE);
      return;
    }
    // A state asked for while hidden is applied as the window appears;
    // ShowWindow with SW_MAXIMIZE on a hidden window would show it anyway.
    const bool temporary = (flags_ & TopLevelWindow::windowIsTemporary) != 0;
    int command = temporary ? SW_SHOWNA : SW_SHOW;
    if (pendingMaximise_) command = SW_SHOWMAXIMIZED;
    if (pendingMinimise_) command = SW_SHOWMINNOACTIVE;
    pendingMaximise_ = pendingMinimise_ = false;
    ShowWindow(hwnd_, command);
    // The topmost band asked for while hidden, asked for again as it shows:
    // a hidden window can be refused it where a shown one is not.
    if (topMost_ && !(GetWindowLongPtrW(hwnd_, GWL_EXSTYLE) & WS_EX_TOPMOST))
      SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
  }

  bool isShowing() const { return hwnd_ && IsWindowVisible(hwnd_); }

  /** The client area on the desktop, device pixels. */
  RECT clientDevice() const {
    RECT r{0, 0, 0, 0};
    if (!hwnd_) return r;
    GetClientRect(hwnd_, &r);
    POINT origin{0, 0};
    ClientToScreen(hwnd_, &origin);
    OffsetRect(&r, origin.x, origin.y);
    return r;
  }

  void setClientDevice(int x, int y, int w, int h, bool move) {
    if (!hwnd_) return;
    RECT r{x, y, x + w, y + h};
    adjust(&r);
    SetWindowPos(hwnd_, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top,
                 SWP_NOZORDER | SWP_NOACTIVATE | (move ? 0u : (UINT) SWP_NOMOVE));
  }

  void centre(int w, int h) {
    const RECT work = workAreaOf(hwnd_);
    setClientDevice(work.left + ((work.right - work.left) - w) / 2,
                    work.top + ((work.bottom - work.top) - h) / 2, w, h, true);
  }

  /** True once the frame's WS_EX_TOPMOST says what was asked. SetWindowPos
   *  answered and the style stayed as it was, once, in a full run with a
   *  hundred processes making windows around this one: the z-order is the
   *  window manager's to grant. Asked again a few times, then reported, with
   *  lastTopMostError() for why. */
  bool setAlwaysOnTop(bool on) {
    topMost_ = on;
    if (!hwnd_) return false;
    lastTopMostError_ = 0;
    for (int attempt = 0; attempt < 4; ++attempt) {
      if (!SetWindowPos(hwnd_, on ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE))
        lastTopMostError_ = GetLastError();
      if (((GetWindowLongPtrW(hwnd_, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0) == on) return true;
      Sleep(10);
    }
    return false;
  }
  DWORD lastTopMostError() const { return lastTopMostError_; }

  void setMaximised(bool on) {
    if (!hwnd_) return;
    if (!IsWindowVisible(hwnd_)) {
      pendingMaximise_ = on;
      if (on) pendingMinimise_ = false;
      return;
    }
    ShowWindow(hwnd_, on ? SW_MAXIMIZE : SW_RESTORE);
  }
  bool isMaximised() const { return hwnd_ && (IsZoomed(hwnd_) || pendingMaximise_); }

  void setMinimised(bool on) {
    if (!hwnd_) return;
    if (!IsWindowVisible(hwnd_)) {
      pendingMinimise_ = on;
      return;
    }
    ShowWindow(hwnd_, on ? SW_MINIMIZE : SW_RESTORE);
  }
  bool isMinimised() const { return hwnd_ && (IsIconic(hwnd_) || pendingMinimise_); }

  /** The whole monitor, no frame: what the reference calls kiosk mode. The
   *  placement and style are kept to come back to. */
  void setKiosk(bool on) {
    if (!hwnd_ || on == kiosk_) return;
    if (on) {
      saved_.length = sizeof(saved_);
      GetWindowPlacement(hwnd_, &saved_);
      savedStyle_ = (DWORD) GetWindowLongPtrW(hwnd_, GWL_STYLE);
      MONITORINFO info{};
      info.cbSize = sizeof(info);
      GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &info);
      kiosk_ = true;
      SetWindowLongPtrW(hwnd_, GWL_STYLE,
                        (LONG_PTR) ((savedStyle_ & ~(DWORD) (WS_CAPTION | WS_THICKFRAME)) | WS_POPUP));
      const RECT& m = info.rcMonitor;
      SetWindowPos(hwnd_, HWND_TOP, m.left, m.top, m.right - m.left, m.bottom - m.top,
                   SWP_FRAMECHANGED | SWP_NOACTIVATE);
    } else {
      kiosk_ = false;
      SetWindowLongPtrW(hwnd_, GWL_STYLE, (LONG_PTR) savedStyle_);
      SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
                   SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
      // The placement's show command would SHOW a hidden window; keep what
      // it was.
      if (!IsWindowVisible(hwnd_)) saved_.showCmd = SW_HIDE;
      SetWindowPlacement(hwnd_, &saved_);
    }
  }
  bool isKiosk() const { return kiosk_; }

  void setAlpha(float alpha) {
    if (!hwnd_) return;
    const LONG_PTR ex = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
    if (!(ex & WS_EX_LAYERED)) SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, ex | WS_EX_LAYERED);
    const float a = alpha < 0.0f ? 0.0f : (alpha > 1.0f ? 1.0f : alpha);
    SetLayeredWindowAttributes(hwnd_, 0, (BYTE) std::lround(a * 255.0f), LWA_ALPHA);
  }

  void toFront(bool activate) {
    if (!hwnd_) return;
    SetWindowPos(hwnd_, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | (activate ? 0u : (UINT) SWP_NOACTIVATE));
    if (activate) SetForegroundWindow(hwnd_);
  }

  bool isActive() const { return hwnd_ && GetActiveWindow() == hwnd_; }

  void limitsChanged() {
    // Applied by the next WM_GETMINMAXINFO; a size already outside them is
    // brought inside now.
    if (!hwnd_ || IsZoomed(hwnd_) || IsIconic(hwnd_) || kiosk_) return;
    const RECT c = clientDevice();
    int w = c.right - c.left, h = c.bottom - c.top;
    owner_->frameConstrain(scale(), &w, &h);
    if (w != c.right - c.left || h != c.bottom - c.top) setClientDevice(c.left, c.top, w, h, false);
  }

  void setIcon(const Bitmap& icon) {
    if (!hwnd_) return;
    HICON big = iconFromBitmap(icon, GetSystemMetrics(SM_CXICON));
    HICON little = iconFromBitmap(icon, GetSystemMetrics(SM_CXSMICON));
    SendMessageW(hwnd_, WM_SETICON, ICON_BIG, (LPARAM) big);
    SendMessageW(hwnd_, WM_SETICON, ICON_SMALL, (LPARAM) little);
    if (iconBig_) DestroyIcon(iconBig_);
    if (iconSmall_) DestroyIcon(iconSmall_);
    iconBig_ = big;
    iconSmall_ = little;
  }

private:
  static HMODULE module() {
    HMODULE m = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR) &Frame::module, &m);
    return m;
  }

  /** A class of this module's own, for the reason window_win32.h gives. */
  static const wchar_t* windowClass() {
    static std::wstring name;
    static ATOM atom = 0;
    if (atom) return name.c_str();
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = &Frame::proc;
    wc.hInstance = module();
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.hbrBackground = nullptr; // the peer covers every pixel
    for (int attempt = 0; attempt < 64 && !atom; ++attempt) {
      wchar_t buffer[96];
      std::swprintf(buffer, 96, L"SonoreTopLevel_%p_%d", (void*) module(), attempt);
      name = buffer;
      wc.lpszClassName = name.c_str();
      atom = RegisterClassExW(&wc);
      if (!atom && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) break;
    }
    return atom ? name.c_str() : nullptr;
  }

  static RECT workAreaOf(HWND hwnd) {
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    HMONITOR monitor = hwnd ? MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY)
                            : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    if (!GetMonitorInfoW(monitor, &info)) {
      RECT r{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
      return r;
    }
    return info.rcWork;
  }

  /** Client rectangle -> window rectangle, at this window's DPI when the
   *  system can say (the frame's border is thicker at 200%). */
  void adjust(RECT* r) const {
    typedef BOOL(WINAPI * AdjustForDpiFn)(LPRECT, DWORD, BOOL, DWORD, UINT);
    static const AdjustForDpiFn forDpi = [] {
      HMODULE user32 = GetModuleHandleW(L"user32.dll");
      return user32 ? (AdjustForDpiFn) (void*) GetProcAddress(user32, "AdjustWindowRectExForDpi")
                    : nullptr;
    }();
    const DWORD style = hwnd_ ? (DWORD) GetWindowLongPtrW(hwnd_, GWL_STYLE) : style_;
    const DWORD ex = hwnd_ ? (DWORD) GetWindowLongPtrW(hwnd_, GWL_EXSTYLE) : exStyle_;
    if (forDpi) forDpi(r, style, FALSE, ex, (UINT) std::lround(scale() * 96.0f));
    else AdjustWindowRectEx(r, style, FALSE, ex);
  }

  /** What the frame adds around the client area. */
  SIZE nonClientExtra() const {
    RECT r{0, 0, 0, 0};
    adjust(&r);
    return SIZE{r.right - r.left, r.bottom - r.top};
  }

  static LRESULT CALLBACK proc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_NCCREATE) {
      const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lp);
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR) create->lpCreateParams);
      return DefWindowProcW(hwnd, message, wp, lp);
    }
    auto* self = reinterpret_cast<Frame*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self || self->hwnd_ != hwnd) return DefWindowProcW(hwnd, message, wp, lp);
    // Nothing may unwind into DispatchMessage. And after a handler that
    // reached application code returns, `self` may be gone -- a close box
    // that deletes its window -- so nothing of it is touched again.
    try {
      LRESULT result = 0;
      if (self->handle(message, wp, lp, &result)) return result;
    } catch (const std::exception& e) {
      MessageManager::sendUnhandledException(&e);
      return 0;
    } catch (...) {
      MessageManager::sendUnhandledException(nullptr);
      return 0;
    }
    return DefWindowProcW(hwnd, message, wp, lp);
  }

  /** True when handled (and `result` set). Every path that calls the owner
   *  returns true: the owner may have deleted this frame. */
  bool handle(UINT message, WPARAM wp, LPARAM lp, LRESULT* result) {
    switch (message) {
      case WM_SIZE:
        if (wp != SIZE_MINIMIZED) owner_->frameClientResized((int) LOWORD(lp), (int) HIWORD(lp));
        return true;

      case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lp);
        int minW = 0, minH = 0, maxW = 0, maxH = 0;
        owner_->frameLimits(scale(), &minW, &minH, &maxW, &maxH);
        const SIZE extra = nonClientExtra();
        if (minW > 0) info->ptMinTrackSize.x = (std::max)(info->ptMinTrackSize.x, (LONG) (minW + extra.cx));
        if (minH > 0) info->ptMinTrackSize.y = (std::max)(info->ptMinTrackSize.y, (LONG) (minH + extra.cy));
        if (maxW > 0) info->ptMaxTrackSize.x = maxW + extra.cx;
        if (maxH > 0) info->ptMaxTrackSize.y = maxH + extra.cy;
        // A frameless window zoomed by the system would cover the taskbar:
        // its "maximised" is the work area, as a framed window's is.
        if (!(flags_ & TopLevelWindow::windowHasTitleBar) && !kiosk_) {
          MONITORINFO mi{};
          mi.cbSize = sizeof(mi);
          if (GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &mi)) {
            info->ptMaxPosition.x = mi.rcWork.left - mi.rcMonitor.left;
            info->ptMaxPosition.y = mi.rcWork.top - mi.rcMonitor.top;
            info->ptMaxSize.x = mi.rcWork.right - mi.rcWork.left;
            info->ptMaxSize.y = mi.rcWork.bottom - mi.rcWork.top;
          }
        }
        return true;
      }

      // A size set by anybody -- SetWindowPos from code as well as the user's
      // drag -- held to the limits. DefWindowProc only does this for framed
      // windows, and only for the track sizes.
      case WM_WINDOWPOSCHANGING: {
        auto* pos = reinterpret_cast<WINDOWPOS*>(lp);
        if (!(pos->flags & SWP_NOSIZE) && !kiosk_ && !IsZoomed(hwnd_) && !IsIconic(hwnd_)) {
          const SIZE extra = nonClientExtra();
          int w = pos->cx - extra.cx, h = pos->cy - extra.cy;
          owner_->frameConstrain(scale(), &w, &h);
          pos->cx = w + extra.cx;
          pos->cy = h + extra.cy;
        }
        return false;
      }

      // The user dragging an edge: the rectangle is corrected here, so the
      // outline under the pointer is the size the window will be -- the
      // aspect ratio included, which WM_GETMINMAXINFO cannot express.
      case WM_SIZING: {
        auto* r = reinterpret_cast<RECT*>(lp);
        const SIZE extra = nonClientExtra();
        int w = (r->right - r->left) - extra.cx, h = (r->bottom - r->top) - extra.cy;
        owner_->frameConstrain(scale(), &w, &h);
        const bool left = wp == WMSZ_LEFT || wp == WMSZ_TOPLEFT || wp == WMSZ_BOTTOMLEFT;
        const bool top = wp == WMSZ_TOP || wp == WMSZ_TOPLEFT || wp == WMSZ_TOPRIGHT;
        if (left) r->left = r->right - (w + extra.cx);
        else r->right = r->left + w + extra.cx;
        if (top) r->top = r->bottom - (h + extra.cy);
        else r->bottom = r->top + h + extra.cy;
        *result = TRUE;
        return true;
      }

      case WM_CLOSE:
        owner_->frameCloseRequested();
        return true;

      case WM_ACTIVATE: {
        HWND hwnd = hwnd_;
        TopLevelWindow* owner = owner_;
        *result = DefWindowProcW(hwnd, message, wp, lp);
        owner->frameActiveChanged(LOWORD(wp) != WA_INACTIVE);
        return true;
      }

      // The tree's keyboard focus lives in the peer's child window. While no
      // text is being typed the child gives the OS focus back to us, so what
      // arrives here is passed down rather than lost.
      case WM_KEYDOWN:
      case WM_KEYUP:
      case WM_CHAR:
      case WM_SYSKEYDOWN:
      case WM_MOUSEWHEEL:
      case WM_MOUSEHWHEEL:
        if (HWND child = (HWND) owner_->peerHandle()) {
          *result = SendMessageW(child, message, wp, lp);
          return true;
        }
        return false;

      case WM_ERASEBKGND:
        *result = 1;
        return true;

      case WM_DPICHANGED: {
        const auto* suggested = reinterpret_cast<const RECT*>(lp);
        if (suggested)
          SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top,
                       suggested->right - suggested->left, suggested->bottom - suggested->top,
                       SWP_NOZORDER | SWP_NOACTIVATE);
        return true;
      }

      default:
        return false;
    }
  }

  TopLevelWindow* owner_;
  HWND hwnd_ = nullptr;
  DWORD lastTopMostError_ = 0;
  bool topMost_ = false;
  DWORD style_ = 0, exStyle_ = 0;
  int flags_ = 0;
  bool pendingMaximise_ = false, pendingMinimise_ = false;
  bool kiosk_ = false;
  WINDOWPLACEMENT saved_{};
  DWORD savedStyle_ = 0;
  HICON iconBig_ = nullptr, iconSmall_ = nullptr;
};

#elif defined(__APPLE__)

inline id nsString(const std::string& text) {
  return cocoa::msg<id>(cocoa::cls("NSString"), sel_registerName("stringWithUTF8String:"),
                        text.c_str());
}

inline Frame* frameOfDelegate(id self) {
  void* value = nullptr;
  object_getInstanceVariable(self, "sonoreFrame", &value);
  return static_cast<Frame*>(value);
}

inline BOOL frameDelegateShouldClose(id self, SEL, id);
inline void frameDelegateResized(id self, SEL, id);
inline void frameDelegateBecameKey(id self, SEL, id);
inline void frameDelegateResignedKey(id self, SEL, id);
inline BOOL frameWindowCanBecomeKey(id, SEL) { return YES; }

/**
 * The NSWindow frame. The peer's view is added to the content view and told to
 * follow its size (autoresizing), and a delegate turns the close box into a
 * question for the owner -- windowShouldClose: answers NO, and the owner
 * decides -- and reports resizes and key status.
 *
 * The window class is a subclass whose canBecomeKeyWindow says YES, because
 * a borderless NSWindow otherwise can never take the keyboard.
 */
class Frame {
public:
  explicit Frame(TopLevelWindow& owner) : owner_(&owner) {}
  ~Frame() { destroy(); }
  Frame(const Frame&) = delete;
  Frame& operator=(const Frame&) = delete;

  bool create(int flags, const std::string& title, const Rect& logical, bool centre,
              bool topMost) {
    destroy();
    flags_ = flags;
    // AppKit wants its application object before any window.
    cocoa::msg<id>(cocoa::cls("NSApplication"), sel_registerName("sharedApplication"));
    unsigned long mask = 0; // NSWindowStyleMaskBorderless
    if (flags & TopLevelWindow::windowHasTitleBar) {
      mask |= 1; // titled
      if (flags & TopLevelWindow::windowHasCloseButton) mask |= 2;
      if (flags & TopLevelWindow::windowHasMinimiseButton) mask |= 4;
    }
    if (flags & TopLevelWindow::windowIsResizable) mask |= 8;
    const double top = NativeWindowCocoa::primaryScreenTop();
    const cocoa::CGRect2 rect{{(double) logical.x, top - (double) (logical.y + logical.h)},
                              {(double) logical.w, (double) logical.h}};
    id allocated = cocoa::msg<id>((id) windowClass(), sel_registerName("alloc"));
    using InitFn = id (*)(id, SEL, cocoa::CGRect2, unsigned long, unsigned long, BOOL);
    window_ = reinterpret_cast<InitFn>(objc_msgSend)(
        allocated, sel_registerName("initWithContentRect:styleMask:backing:defer:"), rect, mask,
        2ul /* NSBackingStoreBuffered */, (BOOL) NO);
    if (!window_) return false;
    // Ours to release (close() below), as makePopupWindow does for menus.
    cocoa::msg<void>(window_, sel_registerName("setReleasedWhenClosed:"), (BOOL) NO);
    cocoa::msg<void>(window_, sel_registerName("setHasShadow:"),
                     (BOOL) ((flags & TopLevelWindow::windowHasDropShadow) ? YES : NO));
    delegate_ = cocoa::msg<id>(cocoa::msg<id>((id) delegateClass(), sel_registerName("alloc")),
                               sel_registerName("init"));
    if (delegate_) {
      object_setInstanceVariable(delegate_, "sonoreFrame", this);
      cocoa::msg<void>(window_, sel_registerName("setDelegate:"), delegate_);
    }
    // NSWindowCollectionBehaviorFullScreenPrimary, so toggleFullScreen: works.
    cocoa::msg<void>(window_, sel_registerName("setCollectionBehavior:"), (unsigned long) (1u << 7));
    setTitle(title);
    if (topMost) setAlwaysOnTop(true);
    if (centre) cocoa::msg<void>(window_, sel_registerName("center"));
    return true;
  }

  void destroy() {
    if (!window_) return;
    if (delegate_) {
      object_setInstanceVariable(delegate_, "sonoreFrame", nullptr);
      cocoa::msg<void>(window_, sel_registerName("setDelegate:"), (id) nullptr);
    }
    cocoa::msg<void>(window_, sel_registerName("orderOut:"), (id) nullptr);
    cocoa::msg<void>(window_, sel_registerName("close"));
    cocoa::msg<void>(window_, sel_registerName("release"));
    window_ = nullptr;
    if (delegate_) cocoa::msg<void>(delegate_, sel_registerName("release"));
    delegate_ = nullptr;
  }

  void* handle() const { return (void*) window_; }
  void* contentParent() const {
    return window_ ? (void*) cocoa::msg<id>(window_, sel_registerName("contentView")) : nullptr;
  }
  float scale() const { return 1.0f; } // points: AppKit applies the backing scale

  void peerOpened(PeerWindow& peer) {
    // NSViewWidthSizable | NSViewHeightSizable: the peer's view follows the
    // content view, and the peer follows its own frame.
    if (id view = (id) peer.handle())
      cocoa::msg<void>(view, sel_registerName("setAutoresizingMask:"), (unsigned long) (2 | 16));
  }

  void setTitle(const std::string& title) {
    if (window_) cocoa::msg<void>(window_, sel_registerName("setTitle:"), nsString(title));
  }

  void show(bool visible) {
    if (!window_) return;
    if (visible) {
      if (pendingMaximise_ && !isZoomedNow()) cocoa::msg<void>(window_, sel_registerName("zoom:"), (id) nullptr);
      pendingMaximise_ = false;
      cocoa::msg<void>(window_, sel_registerName("makeKeyAndOrderFront:"), (id) nullptr);
      if (pendingMinimise_) cocoa::msg<void>(window_, sel_registerName("miniaturize:"), (id) nullptr);
      pendingMinimise_ = false;
    } else {
      cocoa::msg<void>(window_, sel_registerName("orderOut:"), (id) nullptr);
    }
  }

  bool isShowing() const {
    return window_ && cocoa::msg<BOOL>(window_, sel_registerName("isVisible"));
  }

  /** The content rectangle on the desktop, top-left origin, points. */
  Rect clientRect() const {
    if (!window_) return Rect();
    const cocoa::CGRect2 frame = cocoa::msgRect<cocoa::CGRect2>(window_, sel_registerName("frame"));
    const cocoa::CGRect2 content = cocoa::msgRect<cocoa::CGRect2>(
        window_, sel_registerName("contentRectForFrameRect:"), frame);
    const double top = NativeWindowCocoa::primaryScreenTop();
    return Rect((float) content.origin.x,
                (float) (top - (content.origin.y + content.size.height)),
                (float) content.size.width, (float) content.size.height);
  }

  void setClientRect(const Rect& r) {
    if (!window_) return;
    const double top = NativeWindowCocoa::primaryScreenTop();
    const cocoa::CGRect2 content{{(double) r.x, top - (double) (r.y + r.h)},
                                 {(double) r.w, (double) r.h}};
    const cocoa::CGRect2 frame = cocoa::msgRect<cocoa::CGRect2>(
        window_, sel_registerName("frameRectForContentRect:"), content);
    using SetFrameFn = void (*)(id, SEL, cocoa::CGRect2, BOOL);
    reinterpret_cast<SetFrameFn>(objc_msgSend)(window_, sel_registerName("setFrame:display:"),
                                               frame, (BOOL) YES);
  }

  void centre() {
    if (window_) cocoa::msg<void>(window_, sel_registerName("center"));
  }

  void setAlwaysOnTop(bool on) {
    // NSFloatingWindowLevel is 3, NSNormalWindowLevel 0.
    if (window_) cocoa::msg<void>(window_, sel_registerName("setLevel:"), (long) (on ? 3 : 0));
  }

  void setMaximised(bool on) {
    if (!window_) return;
    if (!isShowing()) {
      pendingMaximise_ = on;
      return;
    }
    if (on != isZoomedNow()) cocoa::msg<void>(window_, sel_registerName("zoom:"), (id) nullptr);
  }
  bool isMaximised() const { return pendingMaximise_ || isZoomedNow(); }

  void setMinimised(bool on) {
    if (!window_) return;
    if (!isShowing() && on) {
      pendingMinimise_ = true;
      return;
    }
    cocoa::msg<void>(window_, sel_registerName(on ? "miniaturize:" : "deminiaturize:"), (id) nullptr);
  }
  bool isMinimised() const {
    return pendingMinimise_ ||
           (window_ && cocoa::msg<BOOL>(window_, sel_registerName("isMiniaturized")));
  }

  /** The system's own full-screen space. */
  void setKiosk(bool on) {
    if (!window_ || on == isKiosk()) return;
    cocoa::msg<void>(window_, sel_registerName("toggleFullScreen:"), (id) nullptr);
  }
  bool isKiosk() const {
    // NSWindowStyleMaskFullScreen is 1 << 14.
    return window_ &&
           (cocoa::msg<unsigned long>(window_, sel_registerName("styleMask")) & (1ul << 14)) != 0;
  }

  void setAlpha(float alpha) {
    if (window_) cocoa::msg<void>(window_, sel_registerName("setAlphaValue:"), (double) alpha);
  }

  void toFront(bool activate) {
    if (!window_) return;
    cocoa::msg<void>(window_,
                     sel_registerName(activate ? "makeKeyAndOrderFront:" : "orderFront:"),
                     (id) nullptr);
  }

  bool isActive() const { return window_ && cocoa::msg<BOOL>(window_, sel_registerName("isKeyWindow")); }

  void limitsChanged() {
    if (!window_) return;
    int minW = 0, minH = 0, maxW = 0, maxH = 0;
    owner_->frameLimits(1.0f, &minW, &minH, &maxW, &maxH);
    using SetSizeFn = void (*)(id, SEL, cocoa::CGSize2);
    reinterpret_cast<SetSizeFn>(objc_msgSend)(window_, sel_registerName("setContentMinSize:"),
                                              cocoa::CGSize2{(double) minW, (double) minH});
    reinterpret_cast<SetSizeFn>(objc_msgSend)(
        window_, sel_registerName("setContentMaxSize:"),
        cocoa::CGSize2{maxW > 0 ? (double) maxW : 1.0e7, maxH > 0 ? (double) maxH : 1.0e7});
    const BoundsConstrainer& c = owner_->getConstrainer();
    if (c.aspectRatio > 0.0f)
      reinterpret_cast<SetSizeFn>(objc_msgSend)(window_, sel_registerName("setContentAspectRatio:"),
                                                cocoa::CGSize2{(double) c.aspectRatio, 1.0});
  }

  void setIcon(const Bitmap&) {
    // A window has no icon of its own on macOS: the Dock shows the bundle's.
  }

  // Delegate callbacks.
  void closeRequested() { owner_->frameCloseRequested(); }
  void resized() {
    const Rect r = clientRect();
    owner_->frameClientResized((int) std::lround(r.w), (int) std::lround(r.h));
  }
  void activeChanged(bool active) { owner_->frameActiveChanged(active); }

private:
  bool isZoomedNow() const { return window_ && cocoa::msg<BOOL>(window_, sel_registerName("isZoomed")); }

  static Class windowClass() {
    static Class klass = [] {
      char name[96];
      std::snprintf(name, sizeof(name), "SonoreTopLevelWindow_%llx",
                    (unsigned long long) (uintptr_t) (void*) &frameWindowCanBecomeKey);
      if (Class existing = objc_getClass(name)) return existing;
      Class c = objc_allocateClassPair((Class) cocoa::cls("NSWindow"), name, 0);
      if (!c) return (Class) objc_getClass(name);
      class_addMethod(c, sel_registerName("canBecomeKeyWindow"), (IMP) frameWindowCanBecomeKey, "c@:");
      class_addMethod(c, sel_registerName("canBecomeMainWindow"), (IMP) frameWindowCanBecomeKey, "c@:");
      objc_registerClassPair(c);
      return c;
    }();
    return klass;
  }

  static Class delegateClass() {
    static Class klass = [] {
      // Unique to this binary, for the reason window_cocoa.h gives.
      char name[96];
      std::snprintf(name, sizeof(name), "SonoreTopLevelDelegate_%llx",
                    (unsigned long long) (uintptr_t) (void*) &frameDelegateResized);
      if (Class existing = objc_getClass(name)) return existing;
      Class c = objc_allocateClassPair((Class) cocoa::cls("NSObject"), name, 0);
      if (!c) return (Class) objc_getClass(name);
      class_addIvar(c, "sonoreFrame", sizeof(void*), 3, "^v");
      class_addMethod(c, sel_registerName("windowShouldClose:"), (IMP) frameDelegateShouldClose, "c@:@");
      class_addMethod(c, sel_registerName("windowDidResize:"), (IMP) frameDelegateResized, "v@:@");
      class_addMethod(c, sel_registerName("windowDidBecomeKey:"), (IMP) frameDelegateBecameKey, "v@:@");
      class_addMethod(c, sel_registerName("windowDidResignKey:"), (IMP) frameDelegateResignedKey, "v@:@");
      objc_registerClassPair(c);
      return c;
    }();
    return klass;
  }

  TopLevelWindow* owner_;
  id window_ = nullptr;
  id delegate_ = nullptr;
  int flags_ = 0;
  bool pendingMaximise_ = false, pendingMinimise_ = false;
};

/** NO, always: closing is the owner's decision (userTriedToCloseWindow). */
inline BOOL frameDelegateShouldClose(id self, SEL, id) {
  if (Frame* frame = frameOfDelegate(self)) frame->closeRequested();
  return NO;
}
inline void frameDelegateResized(id self, SEL, id) {
  if (Frame* frame = frameOfDelegate(self)) frame->resized();
}
inline void frameDelegateBecameKey(id self, SEL, id) {
  if (Frame* frame = frameOfDelegate(self)) frame->activeChanged(true);
}
inline void frameDelegateResignedKey(id self, SEL, id) {
  if (Frame* frame = frameOfDelegate(self)) frame->activeChanged(false);
}

#elif defined(SONORE_X11_HEADERS)

/** The Xlib calls a frame needs beyond the peer's list. */
struct X11FrameApi {
  bool ok = false;
  int (*XMoveResizeWindow)(::Display*, ::Window, int, int, unsigned, unsigned) = nullptr;
  int (*XMoveWindow)(::Display*, ::Window, int, int) = nullptr;
  int (*XIconifyWindow)(::Display*, ::Window, int) = nullptr;
  int (*XRaiseWindow)(::Display*, ::Window) = nullptr;
  int (*XSetInputFocus)(::Display*, ::Window, int, ::Time) = nullptr;
  int (*XReparentWindow)(::Display*, ::Window, ::Window, int, int) = nullptr;
};

inline const X11FrameApi& x11Frame() {
  static X11FrameApi api = [] {
    X11FrameApi a;
    void* lib = dlopen("libX11.so.6", RTLD_LAZY | RTLD_GLOBAL);
    if (!lib) lib = dlopen("libX11.so", RTLD_LAZY | RTLD_GLOBAL);
    if (!lib) return a;
    bool all = true;
    auto bind = [&](const char* name, void* slot) {
      void* sym = dlsym(lib, name);
      if (!sym) all = false;
      std::memcpy(slot, &sym, sizeof(sym));
    };
    bind("XMoveResizeWindow", &a.XMoveResizeWindow);
    bind("XMoveWindow", &a.XMoveWindow);
    bind("XIconifyWindow", &a.XIconifyWindow);
    bind("XRaiseWindow", &a.XRaiseWindow);
    bind("XSetInputFocus", &a.XSetInputFocus);
    bind("XReparentWindow", &a.XReparentWindow);
    a.ok = all;
    return a;
  }();
  return api;
}

/**
 * The X11 frame: a top-level window on a connection of its own (the peer has
 * another), managed by the window manager through the EWMH and ICCCM
 * properties -- WM_DELETE_WINDOW for the close box, WM_NORMAL_HINTS for the
 * limits, _NET_WM_STATE for above/maximised/fullscreen, _MOTIF_WM_HINTS to ask
 * for no decorations.
 *
 * X gives nobody a clock or a loop: both connections and a 33 ms timerfd are
 * handed to the MessageManager's poll (registerFdCallback), which is what
 * runDispatchLoop waits in. Without a MessageManager the application pumps.
 */
class Frame {
public:
  explicit Frame(TopLevelWindow& owner) : owner_(&owner) {}
  ~Frame() { destroy(); }
  Frame(const Frame&) = delete;
  Frame& operator=(const Frame&) = delete;

  bool create(int flags, const std::string& title, const Rect& logical, bool centre,
              bool topMost) {
    destroy();
    alive_ = std::make_shared<char>('\0');
    flags_ = flags;
    const x11::XlibApi& x = x11::xlib();
    if (!x.ok || !x11Frame().ok) return false;
    display_ = x.XOpenDisplay(nullptr);
    if (!display_) return false;
    const int screen = x.XDefaultScreen(display_);
    root_ = RootWindow(display_, screen);
    ::XSetWindowAttributes attrs{};
    attrs.event_mask = StructureNotifyMask | FocusChangeMask | PropertyChangeMask;
    const int w = (std::max)(1, (int) std::lround(logical.w));
    const int h = (std::max)(1, (int) std::lround(logical.h));
    int px = (int) std::lround(logical.x), py = (int) std::lround(logical.y);
    if (centre) {
      px = (x.XDisplayWidth(display_, screen) - w) / 2;
      py = (x.XDisplayHeight(display_, screen) - h) / 2;
    }
    window_ = x.XCreateWindow(display_, root_, px, py, (unsigned) w, (unsigned) h, 0,
                              x.XDefaultDepth(display_, screen), InputOutput,
                              x.XDefaultVisual(display_, screen), CWEventMask, &attrs);
    if (!window_) {
      x.XCloseDisplay(display_);
      display_ = nullptr;
      return false;
    }
    wmProtocols_ = atom("WM_PROTOCOLS");
    wmDelete_ = atom("WM_DELETE_WINDOW");
    x.XChangeProperty(display_, window_, wmProtocols_, 4 /*XA_ATOM*/, 32, PropModeReplace,
                      (const unsigned char*) &wmDelete_, 1);
    setTitle(title);
    if (!(flags & TopLevelWindow::windowHasTitleBar)) {
      // _MOTIF_WM_HINTS: flags = decorations, decorations = none.
      const long hints[5] = {2, 0, 0, 0, 0};
      const ::Atom motif = atom("_MOTIF_WM_HINTS");
      x.XChangeProperty(display_, window_, motif, motif, 32, PropModeReplace,
                        (const unsigned char*) hints, 5);
    }
    // Set before mapping, where a window manager reads them.
    std::vector<::Atom> states;
    if (!(flags & TopLevelWindow::windowAppearsOnTaskbar)) states.push_back(atom("_NET_WM_STATE_SKIP_TASKBAR"));
    if (topMost) states.push_back(atom("_NET_WM_STATE_ABOVE"));
    if (!states.empty())
      x.XChangeProperty(display_, window_, atom("_NET_WM_STATE"), 4, 32, PropModeReplace,
                        (const unsigned char*) states.data(), (int) states.size());
    const ::Atom type = atom((flags & TopLevelWindow::windowIsTemporary) ? "_NET_WM_WINDOW_TYPE_UTILITY"
                                                                       : "_NET_WM_WINDOW_TYPE_NORMAL");
    x.XChangeProperty(display_, window_, atom("_NET_WM_WINDOW_TYPE"), 4, 32, PropModeReplace,
                      (const unsigned char*) &type, 1);
    writeSizeHints(px, py, !centre);
    x.XFlush(display_);
    registerWithLoop();
    return true;
  }

  void destroy() {
    alive_.reset();
    unregisterFromLoop();
    if (!display_) return;
    const x11::XlibApi& x = x11::xlib();
    if (window_) x.XDestroyWindow(display_, window_);
    window_ = 0;
    x.XCloseDisplay(display_);
    display_ = nullptr;
  }

  void* handle() const { return (void*) (uintptr_t) window_; }
  void* contentParent() const { return handle(); }
  /** The peer's: it reads the desktop's (Xft.dpi). Until it opens, 1. */
  float scale() const { return peer_ && peer_->isOpen() ? peer_->scale() : 1.0f; }

  void peerOpened(PeerWindow& peer) {
    peer_ = &peer;
#if defined(__linux__)
    if (MessageManager* mm = MessageManager::getInstanceWithoutCreating()) {
      peerFd_ = peer.connectionFd();
      PeerWindow* p = &peer;
      if (peerFd_ >= 0) mm->registerFdCallback(peerFd_, [p](int) { p->processEvents(); });
      // The peer's clock: X has none to give it.
      timerFd_ = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
      if (timerFd_ >= 0) {
        itimerspec spec{};
        spec.it_interval.tv_nsec = 33 * 1000000L;
        spec.it_value.tv_nsec = 33 * 1000000L;
        timerfd_settime(timerFd_, 0, &spec, nullptr);
        const int fd = timerFd_;
        mm->registerFdCallback(timerFd_, [p, fd](int) {
          uint64_t expirations = 0;
          const auto got = read(fd, &expirations, sizeof(expirations)); // glibc's
          (void) got;                                                    // warn_unused_result
          p->processEvents();
        });
      }
    }
#endif
  }

  void setTitle(const std::string& title) {
    if (!display_) return;
    const x11::XlibApi& x = x11::xlib();
    x.XChangeProperty(display_, window_, atom("_NET_WM_NAME"), atom("UTF8_STRING"), 8,
                      PropModeReplace, (const unsigned char*) title.c_str(), (int) title.size());
    x.XChangeProperty(display_, window_, 39 /*XA_WM_NAME*/, 31 /*XA_STRING*/, 8, PropModeReplace,
                      (const unsigned char*) title.c_str(), (int) title.size());
    x.XFlush(display_);
  }

  void show(bool visible) {
    if (!display_) return;
    const x11::XlibApi& x = x11::xlib();
    if (visible) {
      x.XMapWindow(display_, window_);
      x.XFlush(display_);
      mapped_ = true;
      if (pendingMaximise_) setMaximised(true);
      if (pendingMinimise_) setMinimised(true);
      pendingMaximise_ = pendingMinimise_ = false;
    } else {
      x.XUnmapWindow(display_, window_);
      mapped_ = false;
    }
    x.XFlush(display_);
  }

  bool isShowing() const { return display_ && mapped_; }

  /** The client area on the desktop, device pixels. */
  Rect clientDevice() const {
    if (!display_) return Rect();
    const x11::XlibApi& x = x11::xlib();
    ::XWindowAttributes attrs{};
    x.XGetWindowAttributes(display_, window_, &attrs);
    int rx = 0, ry = 0;
    ::Window child = 0;
    x.XTranslateCoordinates(display_, window_, root_, 0, 0, &rx, &ry, &child);
    return Rect((float) rx, (float) ry, (float) attrs.width, (float) attrs.height);
  }

  void setClientDevice(int px, int py, int w, int h, bool move) {
    if (!display_) return;
    if (move) x11Frame().XMoveResizeWindow(display_, window_, px, py, (unsigned) w, (unsigned) h);
    else x11::xlib().XResizeWindow(display_, window_, (unsigned) w, (unsigned) h);
    writeSizeHints(px, py, move);
    x11::xlib().XFlush(display_);
  }

  void centre(int w, int h) {
    if (!display_) return;
    const x11::XlibApi& x = x11::xlib();
    const int screen = x.XDefaultScreen(display_);
    setClientDevice((x.XDisplayWidth(display_, screen) - w) / 2,
                    (x.XDisplayHeight(display_, screen) - h) / 2, w, h, true);
  }

  void setAlwaysOnTop(bool on) { changeState(on, atom("_NET_WM_STATE_ABOVE"), 0); }

  void setMaximised(bool on) {
    if (!mapped_) {
      pendingMaximise_ = on;
      return;
    }
    maximised_ = on;
    changeState(on, atom("_NET_WM_STATE_MAXIMIZED_VERT"), atom("_NET_WM_STATE_MAXIMIZED_HORZ"));
  }
  bool isMaximised() const { return pendingMaximise_ || maximised_; }

  void setMinimised(bool on) {
    if (!display_) return;
    if (!mapped_) {
      pendingMinimise_ = on;
      return;
    }
    minimised_ = on;
    if (on) x11Frame().XIconifyWindow(display_, window_, x11::xlib().XDefaultScreen(display_));
    else x11::xlib().XMapWindow(display_, window_);
    x11::xlib().XFlush(display_);
  }
  bool isMinimised() const { return pendingMinimise_ || minimised_; }

  void setKiosk(bool on) {
    kiosk_ = on;
    changeState(on, atom("_NET_WM_STATE_FULLSCREEN"), 0);
  }
  bool isKiosk() const { return kiosk_; }

  void setAlpha(float alpha) {
    if (!display_) return;
    const float a = alpha < 0.0f ? 0.0f : (alpha > 1.0f ? 1.0f : alpha);
    const unsigned long opacity = (unsigned long) ((double) a * 4294967295.0);
    x11::xlib().XChangeProperty(display_, window_, atom("_NET_WM_WINDOW_OPACITY"), 6 /*CARDINAL*/,
                                32, PropModeReplace, (const unsigned char*) &opacity, 1);
    x11::xlib().XFlush(display_);
  }

  void toFront(bool activate) {
    if (!display_) return;
    x11Frame().XRaiseWindow(display_, window_);
    if (activate) sendToRoot(atom("_NET_ACTIVE_WINDOW"), 1, 0, 0);
    x11::xlib().XFlush(display_);
  }

  bool isActive() const { return active_; }

  void limitsChanged() {
    const Rect c = clientDevice();
    writeSizeHints((int) c.x, (int) c.y, true);
    x11::xlib().XFlush(display_);
  }

  void setIcon(const Bitmap& icon) {
    if (!display_ || icon.isEmpty()) return;
    // _NET_WM_ICON: width, height, then ARGB pixels, each in a long.
    std::vector<unsigned long> data;
    data.reserve((size_t) icon.width() * (size_t) icon.height() + 2);
    data.push_back((unsigned long) icon.width());
    data.push_back((unsigned long) icon.height());
    for (int py = 0; py < icon.height(); ++py)
      for (int px = 0; px < icon.width(); ++px) {
        const PremulColour c = icon.pixelAt(px, py);
        auto un = [&c](uint8_t v) {
          return c.a == 0 ? 0u : (unsigned) (std::min)(255, ((int) v * 255 + c.a / 2) / c.a);
        };
        data.push_back(((unsigned long) c.a << 24) | ((unsigned long) un(c.r) << 16) |
                       ((unsigned long) un(c.g) << 8) | (unsigned long) un(c.b));
      }
    x11::xlib().XChangeProperty(display_, window_, atom("_NET_WM_ICON"), 6 /*CARDINAL*/, 32,
                                PropModeReplace, (const unsigned char*) data.data(),
                                (int) data.size());
    x11::xlib().XFlush(display_);
  }

  /**
   * Drain the frame's connection -- ALL of it: XPending has already moved
   * what arrived into Xlib's queue, so the descriptor will not wake the loop
   * again for events left behind. Stops the moment application code has
   * deleted the window, or remade this frame.
   */
  void pump() {
    if (!display_) return;
    const x11::XlibApi& x = x11::xlib();
    std::weak_ptr<char> ownerAlive = owner_->alive_;
    std::weak_ptr<char> frameAlive = alive_;
    auto gone = [&ownerAlive, &frameAlive]() { return ownerAlive.expired() || frameAlive.expired(); };
    while (display_ && x.XPending(display_) > 0) {
      ::XEvent event;
      x.XNextEvent(display_, &event);
      switch (event.type) {
        case ConfigureNotify:
          if (event.xconfigure.window == window_) {
            owner_->frameClientResized(event.xconfigure.width, event.xconfigure.height);
            if (gone()) return;
          }
          break;
        case ClientMessage:
          if (event.xclient.message_type == wmProtocols_ &&
              (::Atom) event.xclient.data.l[0] == wmDelete_) {
            owner_->frameCloseRequested();
            if (gone()) return;
          }
          break;
        case FocusIn:
          active_ = true;
          // The keyboard to the peer's window, where the tree's focus is.
          if (peer_ && peer_->isOpen())
            x11Frame().XSetInputFocus(display_, (::Window) (uintptr_t) peer_->handle(),
                                      1 /*RevertToPointerRoot*/, CurrentTime);
          owner_->frameActiveChanged(true);
          if (gone()) return;
          break;
        case FocusOut:
          active_ = false;
          owner_->frameActiveChanged(false);
          if (gone()) return;
          break;
        case MapNotify:
          minimised_ = false;
          break;
        case UnmapNotify:
          if (mapped_) minimised_ = true;
          break;
        default:
          break;
      }
    }
  }

private:
  ::Atom atom(const char* name) const { return x11::xlib().XInternAtom(display_, name, 0); }

  /** WM_NORMAL_HINTS as the 18 longs of XSizeHints' wire form: flags,
   *  position, size, minimum, maximum, increments, aspect, base, gravity. */
  void writeSizeHints(int px, int py, bool positioned) {
    int minW = 0, minH = 0, maxW = 0, maxH = 0;
    owner_->frameLimits(scale(), &minW, &minH, &maxW, &maxH);
    long hints[18] = {0};
    long flags = 0;
    if (positioned) flags |= 1; // USPosition
    hints[1] = px;
    hints[2] = py;
    if (minW > 0 || minH > 0) {
      flags |= 16; // PMinSize
      hints[5] = minW;
      hints[6] = minH;
    }
    if (maxW > 0 || maxH > 0) {
      flags |= 32; // PMaxSize
      hints[7] = maxW > 0 ? maxW : 32767;
      hints[8] = maxH > 0 ? maxH : 32767;
    }
    if (!(flags_ & TopLevelWindow::windowIsResizable)) {
      // Not resizable: minimum = maximum = now.
      const Rect c = clientDevice();
      flags |= 16 | 32;
      hints[5] = hints[7] = (long) c.w;
      hints[6] = hints[8] = (long) c.h;
    }
    hints[0] = flags;
    x11::xlib().XChangeProperty(display_, window_, 40 /*XA_WM_NORMAL_HINTS*/,
                                41 /*XA_WM_SIZE_HINTS*/, 32, PropModeReplace,
                                (const unsigned char*) hints, 18);
  }

  void sendToRoot(::Atom type, long a0, long a1, long a2) {
    ::XEvent event{};
    event.xclient.type = ClientMessage;
    event.xclient.window = window_;
    event.xclient.message_type = type;
    event.xclient.format = 32;
    event.xclient.data.l[0] = a0;
    event.xclient.data.l[1] = a1;
    event.xclient.data.l[2] = a2;
    event.xclient.data.l[3] = 1; // from a normal application
    x11::xlib().XSendEvent(display_, root_, 0, SubstructureRedirectMask | SubstructureNotifyMask,
                           &event);
  }

  /** _NET_WM_STATE add/remove, the EWMH way: a client message to the root. */
  void changeState(bool add, ::Atom first, ::Atom second) {
    if (!display_) return;
    sendToRoot(atom("_NET_WM_STATE"), add ? 1 : 0, (long) first, (long) second);
    x11::xlib().XFlush(display_);
  }

  void registerWithLoop() {
#if defined(__linux__)
    if (MessageManager* mm = MessageManager::getInstanceWithoutCreating()) {
      frameFd_ = x11::xlib().XConnectionNumber(display_);
      Frame* self = this;
      mm->registerFdCallback(frameFd_, [self](int) { self->pump(); });
    }
#endif
  }

  void unregisterFromLoop() {
#if defined(__linux__)
    if (MessageManager* mm = MessageManager::getInstanceWithoutCreating()) {
      if (frameFd_ >= 0) mm->unregisterFdCallback(frameFd_);
      if (peerFd_ >= 0) mm->unregisterFdCallback(peerFd_);
      if (timerFd_ >= 0) mm->unregisterFdCallback(timerFd_);
    }
    if (timerFd_ >= 0) close(timerFd_);
#endif
    frameFd_ = peerFd_ = timerFd_ = -1;
  }

  TopLevelWindow* owner_;
  PeerWindow* peer_ = nullptr;
  std::shared_ptr<char> alive_ = std::make_shared<char>('\0');
  ::Display* display_ = nullptr;
  ::Window window_ = 0, root_ = 0;
  ::Atom wmProtocols_ = 0, wmDelete_ = 0;
  int flags_ = 0;
  int frameFd_ = -1, peerFd_ = -1, timerFd_ = -1;
  bool mapped_ = false, active_ = false, maximised_ = false, minimised_ = false, kiosk_ = false;
  bool pendingMaximise_ = false, pendingMinimise_ = false;
};

#else

/** No window backend in this build: a frame that is never made. */
class Frame {
public:
  explicit Frame(TopLevelWindow&) {}
  bool create(int, const std::string&, const Rect&, bool, bool) { return false; }
  void destroy() {}
  void* handle() const { return nullptr; }
  void* contentParent() const { return nullptr; }
  float scale() const { return 1.0f; }
  void peerOpened(PeerWindow&) {}
  void setTitle(const std::string&) {}
  void show(bool) {}
  bool isShowing() const { return false; }
  void setAlwaysOnTop(bool) {}
  void setMaximised(bool) {}
  bool isMaximised() const { return false; }
  void setMinimised(bool) {}
  bool isMinimised() const { return false; }
  void setKiosk(bool) {}
  bool isKiosk() const { return false; }
  void setAlpha(float) {}
  void toFront(bool) {}
  bool isActive() const { return false; }
  void limitsChanged() {}
  void setIcon(const Bitmap&) {}
};

#endif

} // namespace tlw
} // namespace detail

// ── TopLevelWindow ──────────────────────────────────────────────────────────

inline TopLevelWindow::TopLevelWindow(std::string name, bool addToDesktopNow)
    : title_(std::move(name)), wantsDesktop_(addToDesktopNow) {
  // A window's labels are drawn in the desktop's face; a subclass or the
  // caller may set another.
  if (std::shared_ptr<Typeface> face = systemTypeface()) setFont(Font(face, 13.0f));
  setAccessibleName(title_);
  detail::tlw::liveWindows().push_back(this);
}

inline TopLevelWindow::~TopLevelWindow() {
  alive_.reset();
  removeFromDesktop();
  std::vector<TopLevelWindow*>& all = detail::tlw::liveWindows();
  all.erase(std::remove(all.begin(), all.end(), this), all.end());
}

inline bool TopLevelWindow::addToDesktop() {
  wantsDesktop_ = true;
  return ensureFrame();
}

inline void TopLevelWindow::removeFromDesktop() {
  // Bounds kept, so a window put back appears where it was.
  if (frame_) {
    windowBounds_ = getWindowBounds();
    centreOnCreate_ = false;
  }
  wantsDesktop_ = false;
  // The peer first: its window is a child of the frame's.
  closePeer();
  if (frame_) frame_->destroy();
  frame_.reset();
}

inline void TopLevelWindow::closePeer() {
  // Over a copy: a hook is free to register another.
  if (void* content = peerHandle()) {
    const std::vector<detail::tlw::DetachHook> hooks = detail::tlw::detachHooks();
    for (detail::tlw::DetachHook hook : hooks) hook(content);
  }
  peer_.close();
}

inline bool TopLevelWindow::ensureFrame() {
  if (frame_) return true;
  if (!wantsDesktop_ || creating_) return false;
  creating_ = true;
  std::unique_ptr<detail::tlw::Frame> frame(new detail::tlw::Frame(*this));
  Rect bounds = windowBounds_;
  constrainer_.constrain(&bounds.w, &bounds.h);
  if (!frame->create(getDesktopWindowStyleFlags(), title_, bounds, centreOnCreate_, alwaysOnTop_)) {
    creating_ = false;
    wantsDesktop_ = false;
    return false;
  }
  frame_ = std::move(frame);
  peer_.onTick = [this]() { peerTicked(); };
#if defined(_WIN32)
  peer_.setAccessibleWindowName(title_.empty() ? std::string("Window") : title_);
#endif
  if (!peer_.open(frame_->contentParent(), *this, (int) std::lround(bounds.w),
                  (int) std::lround(bounds.h))) {
    frame_->destroy();
    frame_.reset();
    creating_ = false;
    wantsDesktop_ = false;
    return false;
  }
  frame_->peerOpened(peer_);
#if defined(SONORE_X11_HEADERS) && !defined(_WIN32) && !defined(__APPLE__)
  // The desktop's scale is the peer's to read; the frame was made at 1.
  {
    const float s = peer_.scale();
    const int w = detail::tlw::toDevice(bounds.w, s), h = detail::tlw::toDevice(bounds.h, s);
    if (centreOnCreate_) frame_->centre(w, h);
    else frame_->setClientDevice(detail::tlw::toDevice(bounds.x, s),
                                 detail::tlw::toDevice(bounds.y, s), w, h, true);
  }
#endif
  frame_->limitsChanged();
  // The peer to the frame's client size, which is the truth after rounding
  // and after the platform applied its own minimum.
#if defined(_WIN32)
  {
    const RECT c = frame_->clientDevice();
    frameClientResized(c.right - c.left, c.bottom - c.top);
  }
#elif defined(__APPLE__)
  {
    const Rect c = frame_->clientRect();
    frameClientResized((int) std::lround(c.w), (int) std::lround(c.h));
  }
#elif defined(SONORE_X11_HEADERS)
  {
    const Rect c = frame_->clientDevice();
    frameClientResized((int) c.w, (int) c.h);
  }
#endif
  if (alpha_ < 1.0f) frame_->setAlpha(alpha_);
  if (!icon_.isEmpty()) frame_->setIcon(icon_);
  if (visible_) frame_->show(true);
  creating_ = false;
  return true;
}

inline void* TopLevelWindow::getWindowHandle() {
  ensureFrame();
  return frame_ ? frame_->handle() : nullptr;
}

inline void* TopLevelWindow::getContentHandle() {
  ensureFrame();
  return peerHandle();
}

inline void TopLevelWindow::setName(const std::string& title) {
  title_ = title;
  setAccessibleName(title);
  if (frame_) frame_->setTitle(title);
  repaint();
}

inline void TopLevelWindow::setVisible(bool shouldBeVisible) {
  Component::setVisible(shouldBeVisible);
  visible_ = shouldBeVisible;
  if (shouldBeVisible) ensureFrame();
  if (frame_) frame_->show(shouldBeVisible);
}

inline bool TopLevelWindow::isWindowVisible() const { return frame_ && frame_->isShowing(); }

inline void TopLevelWindow::toFront(bool takeKeyboardFocus) {
  if (ensureFrame()) frame_->toFront(takeKeyboardFocus);
}

inline bool TopLevelWindow::isActiveWindow() const { return frame_ && frame_->isActive(); }

inline void TopLevelWindow::setAlwaysOnTop(bool shouldStayOnTop) {
  alwaysOnTop_ = shouldStayOnTop;
  if (frame_) frame_->setAlwaysOnTop(shouldStayOnTop);
}

inline void TopLevelWindow::setUsingNativeTitleBar(bool useNative) {
  if (useNative == nativeTitleBar_) return;
  nativeTitleBar_ = useNative;
  recreateFrame();
  resized();
  repaint();
}

inline void TopLevelWindow::setWindowAlpha(float alpha) {
  alpha_ = alpha < 0.0f ? 0.0f : (alpha > 1.0f ? 1.0f : alpha);
  if (frame_) frame_->setAlpha(alpha_);
}

inline void TopLevelWindow::recreateFrame() {
  if (!frame_) return;
  const Rect bounds = getWindowBounds();
  const bool wasVisible = frame_->isShowing();
  const bool wasMaximised = frame_->isMaximised();
  closePeer();
  frame_->destroy();
  frame_.reset();
  windowBounds_ = bounds;
  centreOnCreate_ = false;
  wantsDesktop_ = true;
  visible_ = false;
  if (!ensureFrame()) return;
  if (wasMaximised) frame_->setMaximised(true);
  if (wasVisible) {
    visible_ = true;
    frame_->show(true);
  }
}

inline float TopLevelWindow::getDesktopScale() const {
#if defined(__APPLE__)
  return 1.0f;
#elif defined(_WIN32)
  return frame_ ? frame_->scale() : detail::tlw::frameDpiScale(nullptr);
#else
  return peer_.isOpen() ? peer_.scale() : 1.0f;
#endif
}

inline Rect TopLevelWindow::getWindowBounds() const {
  if (!frame_) return windowBounds_;
#if defined(_WIN32)
  const float s = frame_->scale();
  const RECT c = frame_->clientDevice();
  return Rect((float) c.left / s, (float) c.top / s, (float) (c.right - c.left) / s,
              (float) (c.bottom - c.top) / s);
#elif defined(__APPLE__)
  return frame_->clientRect();
#elif defined(SONORE_X11_HEADERS)
  const float s = getDesktopScale();
  const Rect c = frame_->clientDevice();
  return Rect(c.x / s, c.y / s, c.w / s, c.h / s);
#else
  return windowBounds_;
#endif
}

inline void TopLevelWindow::setWindowBounds(const Rect& requested) {
  Rect r = requested;
  constrainer_.constrain(&r.w, &r.h);
  windowBounds_ = r;
  centreOnCreate_ = false;
  if (!frame_) return;
#if defined(_WIN32) || defined(SONORE_X11_HEADERS)
  const float s = getDesktopScale();
  frame_->setClientDevice(detail::tlw::toDevice(r.x, s), detail::tlw::toDevice(r.y, s),
                          detail::tlw::toDevice(r.w, s), detail::tlw::toDevice(r.h, s), true);
#elif defined(__APPLE__)
  frame_->setClientRect(r);
#endif
}

inline void TopLevelWindow::setWindowSize(float width, float height) {
  const Rect now = getWindowBounds();
  setWindowBounds(Rect(now.x, now.y, width, height));
}

inline void TopLevelWindow::setTopLeftPosition(float x, float y) {
  const Rect now = getWindowBounds();
  setWindowBounds(Rect(x, y, now.w, now.h));
}

inline void TopLevelWindow::centreWithSize(float width, float height) {
  float w = width, h = height;
  constrainer_.constrain(&w, &h);
  if (!frame_) {
    windowBounds_ = Rect(0.0f, 0.0f, w, h);
    centreOnCreate_ = true;
    return;
  }
#if defined(_WIN32) || defined(SONORE_X11_HEADERS)
  const float s = getDesktopScale();
  frame_->centre(detail::tlw::toDevice(w, s), detail::tlw::toDevice(h, s));
#elif defined(__APPLE__)
  const Rect now = frame_->clientRect();
  frame_->setClientRect(Rect(now.x, now.y, w, h));
  frame_->centre();
#endif
}

inline void TopLevelWindow::setConstrainer(const BoundsConstrainer& constrainer) {
  constrainer_ = constrainer;
  constrainerChanged();
  if (frame_) frame_->limitsChanged();
}

inline bool TopLevelWindow::frameIsMaximised() const { return frame_ && frame_->isMaximised(); }
inline bool TopLevelWindow::frameIsMinimised() const { return frame_ && frame_->isMinimised(); }
inline bool TopLevelWindow::frameIsKiosk() const { return frame_ && frame_->isKiosk(); }

inline void TopLevelWindow::setFrameMaximised(bool shouldBe) {
  if (ensureFrame()) frame_->setMaximised(shouldBe);
}
inline void TopLevelWindow::setFrameMinimised(bool shouldBe) {
  if (ensureFrame()) frame_->setMinimised(shouldBe);
}
inline void TopLevelWindow::setFrameKiosk(bool shouldBe) {
  if (ensureFrame()) frame_->setKiosk(shouldBe);
}

inline void TopLevelWindow::setIcon(const Bitmap& icon) {
  icon_ = icon;
  if (frame_) frame_->setIcon(icon);
}

inline void TopLevelWindow::frameClientResized(int deviceW, int deviceH) {
  if (deviceW <= 0 || deviceH <= 0 || !peer_.isOpen()) return;
  peer_.setSize(deviceW, deviceH);
}

inline void TopLevelWindow::frameConstrain(float scale, int* w, int* h) const {
  const float s = scale > 0.0f ? scale : 1.0f;
  float lw = (float) *w / s, lh = (float) *h / s;
  constrainer_.constrain(&lw, &lh);
  *w = detail::tlw::toDevice(lw, s);
  *h = detail::tlw::toDevice(lh, s);
}

inline void TopLevelWindow::frameLimits(float scale, int* minW, int* minH, int* maxW,
                                        int* maxH) const {
  const float s = scale > 0.0f ? scale : 1.0f;
  *minW = constrainer_.minWidth > 1.0f ? detail::tlw::toDevice(constrainer_.minWidth, s) : 0;
  *minH = constrainer_.minHeight > 1.0f ? detail::tlw::toDevice(constrainer_.minHeight, s) : 0;
  *maxW = constrainer_.maxWidth > 0.0f ? detail::tlw::toDevice(constrainer_.maxWidth, s) : 0;
  *maxH = constrainer_.maxHeight > 0.0f ? detail::tlw::toDevice(constrainer_.maxHeight, s) : 0;
}

inline void TopLevelWindow::peerTicked() {
  const auto now = std::chrono::steady_clock::now();
  const double elapsed =
      ticked_ ? std::chrono::duration<double, std::milli>(now - lastTick_).count() : 0.0;
  lastTick_ = now;
  ticked_ = true;
  // Kept alive across the calls: any of them may delete the window.
  std::weak_ptr<char> token = alive_;
  for (detail::tlw::TickHook hook : detail::tlw::tickHooks()) {
    hook(*this);
    if (token.expired()) return;
  }
  windowTick(elapsed);
  if (token.expired()) return;
  if (onTick) onTick(elapsed);
}

/**
 * A TopLevelWindow with a content component, size limits, a resizer, and the
 * zoomed, minimised and full-screen states.
 *
 * The reference's name "full screen" for setFullScreen is kept, and means what
 * it means there: ZOOMED (maximised) -- the window fills the work area with
 * its frame. Covering the whole monitor with no frame is setKioskMode.
 */
class ResizableWindow : public TopLevelWindow {
public:
  /** Not resizable until setResizable(); the background is the look and
   *  feel's until setBackgroundColour(). */
  explicit ResizableWindow(std::string name = std::string(), bool addToDesktop = true)
      : TopLevelWindow(std::move(name), addToDesktop) {
    initialiseResizers();
  }
  /** As above, with the colour painted behind the content. */
  ResizableWindow(std::string name, Colour background, bool addToDesktop = true)
      : TopLevelWindow(std::move(name), addToDesktop), background_(background),
        hasBackground_(true) {
    initialiseResizers();
  }

  /** Closes the window first, then deletes the content if the window owns
   *  it (setContentOwned). */
  ~ResizableWindow() override {
    // The window before the content: the peer paints the tree.
    removeFromDesktop();
    clearContentComponent();
  }

  int getDesktopWindowStyleFlags() const override {
    int flags = TopLevelWindow::getDesktopWindowStyleFlags();
    if (resizable_) flags |= windowIsResizable;
    if (isUsingNativeTitleBar()) flags |= windowHasMaximiseButton;
    return flags;
  }

  // ── Background ────────────────────────────────────────────────────────────

  /** The colour painted behind the content (and where there is none). */
  void setBackgroundColour(Colour c) {
    background_ = c;
    hasBackground_ = true;
    repaint();
  }
  /** The colour set by the constructor or setBackgroundColour(), else the
   *  look and feel's background. */
  Colour getBackgroundColour() const {
    return hasBackground_ ? background_ : lookAndFeel().background();
  }

  /** Fills the window with getBackgroundColour(). */
  void paint(Graphics& g) override { g.fillAll(getBackgroundColour()); }

  // ── Content ──────────────────────────────────────────────────────────────

  /** The window owns it from now on. With `resizeToFit`, the window takes
   *  the content's current size; otherwise the content fills the window. */
  void setContentOwned(Component* content, bool resizeToFit) {
    setContent(content, true, resizeToFit);
  }
  /** As above, but the caller keeps it. */
  void setContentNonOwned(Component* content, bool resizeToFit) {
    setContent(content, false, resizeToFit);
  }
  /** Take the content out of the window, deleting it if the window owns
   *  it. Setting another content does this first. */
  void clearContentComponent() {
    if (content_) removeChild(content_);
    if (ownsContent_) delete content_;
    content_ = nullptr;
    ownsContent_ = false;
  }
  /** The content, owned or not; null when there is none. */
  Component* getContentComponent() const { return content_; }

  /** Size the window so the CONTENT is this big: the bars above it added.
   *  From the insets rather than from the current layout, which before the
   *  window first exists has no size to subtract from. */
  void setContentComponentSize(float width, float height) {
    setWindowSize(width, height + getContentTopInset());
  }

  /** What sits above the content -- a drawn title bar, a menu bar. */
  virtual float getContentTopInset() const { return 0.0f; }

  /** Where the content goes, in the window's own coordinates. */
  Rect getContentArea() const {
    const float top = getContentTopInset();
    return Rect(0.0f, top, bounds().w, (std::max)(0.0f, bounds().h - top));
  }

  // ── Resizing ─────────────────────────────────────────────────────────────

  /**
   * Whether the user may resize it, and how: the platform's edges with a
   * native frame; with a drawn one, a border all round -- or a corner grip in
   * either case when `useBottomRightCornerResizer`. Remakes the frame when
   * the answer for the frame changes.
   */
  void setResizable(bool shouldBeResizable, bool useBottomRightCornerResizer) {
    const bool frameChanges = shouldBeResizable != resizable_;
    resizable_ = shouldBeResizable;
    useCorner_ = useBottomRightCornerResizer;
    if (frameChanges) recreateFrame();
    resized();
  }
  /** What setResizable() last asked for (false by default). */
  bool isResizable() const { return resizable_; }

  /** Zero for a maximum means none. */
  void setResizeLimits(float minWidth, float minHeight, float maxWidth, float maxHeight) {
    BoundsConstrainer c = getConstrainer();
    c.setSizeLimits(minWidth, minHeight, maxWidth, maxHeight);
    setConstrainer(c);
    // A window already outside the new limits is brought inside them.
    const Rect now = getWindowBounds();
    float w = now.w, h = now.h;
    c.constrain(&w, &h);
    if (hasFrame() && (w != now.w || h != now.h)) setWindowBounds(Rect(now.x, now.y, w, h));
  }

  /** Width over height, held while resizing; 0 frees it. */
  void setFixedAspectRatio(float widthOverHeight) {
    BoundsConstrainer c = getConstrainer();
    c.aspectRatio = widthOverHeight > 0.0f ? widthOverHeight : 0.0f;
    setConstrainer(c);
  }

  /** The corner grip while it is shown (setResizable with the corner),
   *  else null. Owned by the window. */
  ResizableCorner* getCornerResizer() { return corner_.isVisible() ? &corner_ : nullptr; }
  /** The drag border while it is shown (resizable, drawn title bar, no
   *  corner grip), else null. Owned by the window. */
  ResizableBorder* getBorderResizer() { return border_.isVisible() ? &border_ : nullptr; }

  // ── States ───────────────────────────────────────────────────────────────

  /** Zoomed to the work area, frame and all (the reference's meaning). */
  void setFullScreen(bool shouldBeFullScreen) { setFrameMaximised(shouldBeFullScreen); }
  /** Zoomed (maximised), as setFullScreen means it. */
  bool isFullScreen() const { return frameIsMaximised(); }

  /** Minimise the window, or restore it. */
  void setMinimised(bool shouldMinimise) { setFrameMinimised(shouldMinimise); }
  /** Minimised; false while the window has no frame. */
  bool isMinimised() const { return frameIsMinimised(); }

  /** The whole monitor, no frame, no taskbar. */
  void setKioskMode(bool shouldBeKiosk) { setFrameKiosk(shouldBeKiosk); }
  /** In kiosk mode (setKioskMode). */
  bool isKioskMode() const { return frameIsKiosk(); }

  /** Lays the content out in getContentArea() and places the border and
   *  the corner grip over it. A subclass that overrides it calls it. */
  void resized() override {
    const Rect area = getContentArea();
    if (content_) content_->setBounds(area);
    const Rect all = localBounds();
    const bool drawnFrame = !isUsingNativeTitleBar();
    border_.setVisible(resizable_ && drawnFrame && !useCorner_);
    corner_.setVisible(resizable_ && useCorner_);
    border_.setBounds(all);
    border_.setCurrentSize(all.w, all.h);
    corner_.setBounds(Rect(all.w - ResizableCorner::kSize, all.h - ResizableCorner::kSize,
                           ResizableCorner::kSize, ResizableCorner::kSize));
    corner_.setCurrentSize(all.w, all.h);
    // Last in the child list: drawn over the content and asked first.
    border_.toFront();
    corner_.toFront();
  }

protected:
  void constrainerChanged() override {
    border_.constrainer = getConstrainer();
    corner_.constrainer = getConstrainer();
  }

private:
  void initialiseResizers() {
    border_.setVisible(false);
    corner_.setVisible(false);
    addChild(&border_);
    addChild(&corner_);
    border_.currentBounds = [this]() { return getWindowBounds(); };
    border_.applyBounds = [this](const Rect& r) { setWindowBounds(r); };
    // The corner measures in the window's coordinates, whose origin does
    // not move when the bottom-right corner is dragged.
    corner_.onResize = [this](float w, float h) { setWindowSize(w, h); };
    constrainerChanged();
  }

  void setContent(Component* content, bool owned, bool resizeToFit) {
    if (content == content_) {
      ownsContent_ = owned;
      return;
    }
    clearContentComponent();
    content_ = content;
    ownsContent_ = owned;
    if (!content_) return;
    addChild(content_);
    content_->toFront();
    border_.toFront();
    corner_.toFront();
    if (resizeToFit) setContentComponentSize(content_->bounds().w, content_->bounds().h);
    else content_->setBounds(getContentArea());
  }

  Component* content_ = nullptr;
  bool ownsContent_ = false;
  bool resizable_ = false;
  bool useCorner_ = false;
  Colour background_;
  bool hasBackground_ = false;
  detail::tlw::WindowBorder border_;
  ResizableCorner corner_;
};

/**
 * A ResizableWindow with a title bar, its buttons, and a menu bar.
 *
 * With the native title bar (the default) the buttons are the platform's;
 * with setUsingNativeTitleBar(false) they are drawn (minimise, maximise,
 * close, on the right or the left) and the bar drags the window. Either way a
 * close request -- the box, Alt+F4, the drawn button -- arrives at
 * closeButtonPressed(), and nothing closes unless that does something. The
 * drawn buttons report through the message queue, so closeButtonPressed may
 * delete the window.
 *
 * The menu bar: setMenuBar(model) asks createMenuBarComponent(model) for the
 * component that shows it -- gfx/menu_bar.h's MenuBarComponent unless it is
 * overridden (or setMenuBarComponent hands one over directly).
 */
class DocumentWindow : public ResizableWindow {
public:
  /** The title bar's buttons, OR-ed into the `requiredButtons` the
   *  constructor and setTitleBarButtonsRequired() take. */
  enum TitleBarButtons {
    minimiseButton = 1, ///< Calls minimiseButtonPressed().
    maximiseButton = 2, ///< Calls maximiseButtonPressed(): zoom or restore.
    closeButton = 4,    ///< Calls closeButtonPressed().
    allButtons = 7,     ///< All three.
  };

  /** A window titled `title`, painted `background` behind the content,
   *  with the title bar buttons in `requiredButtons` (TitleBarButtons
   *  OR-ed). The title bar is native until setUsingNativeTitleBar(false). */
  DocumentWindow(std::string title, Colour background, int requiredButtons,
                 bool addToDesktop = true)
      : ResizableWindow(std::move(title), background, addToDesktop), buttons_(requiredButtons) {
    addChild(&titleBar_);
    titleBar_.title = [this]() { return getName(); };
    titleBar_.setButtons(buttons_, buttonsOnLeft_);
    titleBar_.currentBounds = [this]() { return getWindowBounds(); };
    titleBar_.moveTo = [this](float x, float y) {
      if (!isFullScreen()) setTopLeftPosition(x, y);
    };
    titleBar_.isActive = [this]() { return isActiveWindow(); };
    titleBar_.onDoubleClick = [this]() { queue([](DocumentWindow& w) { w.maximiseButtonPressed(); }); };
    titleBar_.minimiseButton().onClick = [this]() {
      queue([](DocumentWindow& w) { w.minimiseButtonPressed(); });
    };
    titleBar_.maximiseButton().onClick = [this]() {
      queue([](DocumentWindow& w) { w.maximiseButtonPressed(); });
    };
    titleBar_.closeButton().onClick = [this]() {
      queue([](DocumentWindow& w) { w.closeButtonPressed(); });
    };
  }

  /** Closes the window, then deletes an owned content and the menu bar
   *  component. The MenuBarModel is the caller's and is not deleted. */
  ~DocumentWindow() override {
    removeFromDesktop();
    clearContentComponent();
  }

  int getDesktopWindowStyleFlags() const override {
    int flags = windowAppearsOnTaskbar | windowHasDropShadow;
    if (isResizable()) flags |= windowIsResizable;
    if (isUsingNativeTitleBar()) {
      flags |= windowHasTitleBar;
      if (buttons_ & minimiseButton) flags |= windowHasMinimiseButton;
      if (buttons_ & maximiseButton) flags |= windowHasMaximiseButton;
      if (buttons_ & closeButton) flags |= windowHasCloseButton;
    }
    return flags;
  }

  // ── The title bar ────────────────────────────────────────────────────────

  /** Which buttons, and on which side (drawn bar). Remakes a native frame. */
  void setTitleBarButtonsRequired(int requiredButtons, bool positionOnLeft) {
    const bool frameChanges = requiredButtons != buttons_;
    buttons_ = requiredButtons;
    buttonsOnLeft_ = positionOnLeft;
    titleBar_.setButtons(buttons_, buttonsOnLeft_);
    if (frameChanges && isUsingNativeTitleBar()) recreateFrame();
  }
  /** The TitleBarButtons bits asked for (allButtons by default). */
  int getTitleBarButtonsRequired() const { return buttons_; }

  /** The drawn title bar's height in logical units, at least 8 (the
   *  default is kDefaultTitleBarHeight). No effect on a native bar. */
  void setTitleBarHeight(float height) {
    titleBarHeight_ = height > 8.0f ? height : 8.0f;
    resized();
  }
  /** The drawn title bar's height; 0 with a native title bar, whose height
   *  is outside the client area. */
  float getTitleBarHeight() const { return isUsingNativeTitleBar() ? 0.0f : titleBarHeight_; }

  /** Centre the drawn bar's title in the space the buttons leave (true, the
   *  default) or left-align it there. */
  void setTitleBarTextCentred(bool centred) { titleBar_.setTextCentred(centred); }

  /** The drawn bar's area in the window's coordinates; empty with a native
   *  title bar. */
  Rect getTitleBarArea() const {
    return isUsingNativeTitleBar() ? Rect() : Rect(0.0f, 0.0f, bounds().w, titleBarHeight_);
  }

  /** The drawn buttons, for a test or a theme; null with a native bar. */
  Widget* getCloseButton() { return drawnButton(titleBar_.closeButton(), closeButton); }
  /** The drawn minimise button; null with a native bar or without the
   *  minimiseButton bit. */
  Widget* getMinimiseButton() { return drawnButton(titleBar_.minimiseButton(), minimiseButton); }
  /** The drawn maximise button; null with a native bar or without the
   *  maximiseButton bit. */
  Widget* getMaximiseButton() { return drawnButton(titleBar_.maximiseButton(), maximiseButton); }

  // ── The menu bar ─────────────────────────────────────────────────────────

  /**
   * Show a menu bar for `model` under the title bar (null removes it). The
   * component comes from createMenuBarComponent(); `height` 0 means the
   * default, 24.
   */
  void setMenuBar(MenuBarModel* model, float height = 0.0f) {
    menuBarModel_ = model;
    menuBarHeight_ = height > 0.0f ? height : kDefaultMenuBarHeight;
    std::unique_ptr<Component> bar = model ? createMenuBarComponent(*model) : nullptr;
    setMenuBarComponentOwned(std::move(bar));
  }
  /** The model setMenuBar() was given, or null. Not owned. A model
   *  destroyed first empties the bar safely, but this pointer is not
   *  cleared: call setMenuBar(nullptr) before deleting the model. */
  MenuBarModel* getMenuBarModel() const { return menuBarModel_; }

  /** Any component as the menu bar, owned by the window from now on. */
  void setMenuBarComponent(Component* newMenuBar, float height = 0.0f) {
    if (height > 0.0f) menuBarHeight_ = height;
    setMenuBarComponentOwned(std::unique_ptr<Component>(newMenuBar));
  }
  /** The component showing the menu bar (owned by the window), or null. */
  Component* getMenuBarComponent() const { return menuBar_.get(); }
  /** The menu bar's height in logical units; 0 without a menu bar. */
  float getMenuBarHeight() const { return menuBar_ ? menuBarHeight_ : 0.0f; }

  /**
   * THE HOOK: the component that shows `model` -- a MenuBarComponent, the
   * bar that also takes F10 and Alt+letter. Overridden for a bar of its own.
   */
  virtual std::unique_ptr<Component> createMenuBarComponent(MenuBarModel& model) {
    return std::unique_ptr<Component>(new MenuBarComponent(&model));
  }

  // ── What the buttons do ──────────────────────────────────────────────────

  /** The close box, Alt+F4, the drawn button. Calls onCloseButtonPressed;
   *  override (or set that) to close or quit. May delete the window. */
  virtual void closeButtonPressed() {
    if (onCloseButtonPressed) onCloseButtonPressed();
  }
  /** The drawn minimise button: minimises the window. Called from the
   *  message queue, not from inside the click. */
  virtual void minimiseButtonPressed() { setMinimised(true); }
  /** The drawn maximise button or a double click on the drawn bar: zooms
   *  the window, or restores it if zoomed. Called from the message queue. */
  virtual void maximiseButtonPressed() { setFullScreen(!isFullScreen()); }

  /** What the default closeButtonPressed() calls; unset, a close request
   *  does nothing. It may delete the window. */
  std::function<void()> onCloseButtonPressed;

  /** Passes the platform's close request to closeButtonPressed(). */
  void userTriedToCloseWindow() override { closeButtonPressed(); }

  /** The drawn title bar's height (0 when native) plus the menu bar's. */
  float getContentTopInset() const override { return getTitleBarHeight() + getMenuBarHeight(); }

  /** Lays out the drawn title bar and the menu bar above the content, then
   *  the content and resizers (ResizableWindow::resized). A subclass that
   *  overrides it calls it. */
  void resized() override {
    const Rect all = localBounds();
    titleBar_.setVisible(!isUsingNativeTitleBar());
    titleBar_.setBounds(getTitleBarArea());
    if (menuBar_) menuBar_->setBounds(Rect(0.0f, getTitleBarHeight(), all.w, menuBarHeight_));
    ResizableWindow::resized();
    // Above the border: the bar's top and right edges are its own (the drag,
    // the close button), so a drawn window is resized from the sides below
    // it, the bottom and the bottom corners.
    titleBar_.toFront();
  }

  /** The drawn title bar's height until setTitleBarHeight, logical units. */
  static constexpr float kDefaultTitleBarHeight = 26.0f;
  /** The menu bar's height when setMenuBar is given 0, logical units. */
  static constexpr float kDefaultMenuBarHeight = 24.0f;

private:
  /** Run `action` on this window from the message queue -- never from inside
   *  the click that asked, since it may delete the window the click is in.
   *  Without a MessageManager it runs at once. */
  void queue(void (*action)(DocumentWindow&)) {
    std::weak_ptr<char> token = lifetimeToken();
    DocumentWindow* self = this;
    if (MessageManager::getInstanceWithoutCreating() &&
        MessageManager::callAsync([token, self, action]() {
          if (!token.expired()) action(*self);
        }))
      return;
    action(*this);
  }

  Widget* drawnButton(detail::tlw::TitleBarButton& b, int bit) {
    return (!isUsingNativeTitleBar() && (buttons_ & bit)) ? &b : nullptr;
  }

  void setMenuBarComponentOwned(std::unique_ptr<Component> bar) {
    if (menuBar_) removeChild(menuBar_.get());
    menuBar_ = std::move(bar);
    if (menuBar_) addChild(menuBar_.get());
    titleBar_.toFront();
    resized();
    repaint();
  }

  detail::tlw::TitleBar titleBar_;
  std::unique_ptr<Component> menuBar_;
  MenuBarModel* menuBarModel_ = nullptr;
  float menuBarHeight_ = kDefaultMenuBarHeight;
  float titleBarHeight_ = kDefaultTitleBarHeight;
  int buttons_ = allButtons;
  bool buttonsOnLeft_ = false;
};

} // namespace gfx
} // namespace sonore
