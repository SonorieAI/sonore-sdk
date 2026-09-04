// SPDX-License-Identifier: Apache-2.0
//
// The native editor as a plugin format sees it: one object with a window in
// it, opened into whatever handle the host passes down.
//
// ── What this adds over GenericEditor ───────────────────────────────────────
//
// GenericEditor is a Component. It has no window, no timer and no font, which
// is exactly why it can be tested by rendering into a Bitmap. This file is the
// part that cannot be tested that way: finding a typeface, making a real OS
// window, and keeping the two alive together for as long as the host wants an
// editor open.
//
// Keeping them apart means the untestable part is small and dull. Everything
// with a rule in it -- which curve, which gesture, which pixel -- is on the
// other side of the line.
//
// ── When there is no window backend ─────────────────────────────────────────
//
// Win32 and X11 have one; macOS does not yet. isAvailable() says so and
// unavailableReason() says why, so a wrapper can fall back to the webview and
// the fallback names its reason instead of the editor silently being a blank
// rectangle.
//
// On Linux, "available" is decided TWICE: once at compile time, because
// <X11/Xlib.h> may not have been installed when the plugin was built, and once
// at runtime, because libX11.so.6 may not be on the machine that loads it.
// Both are real -- a build container is usually minimal and a Wayland-only
// session genuinely has no Xlib -- and both name themselves differently.
#pragma once

#include <memory>

#include "../plugin.h"
#include "font.h"
#include "../commands.h"
#include "plugin_editor.h"
#include "popup.h"
#include "tooltip.h"
#include "resizer.h"
#include "viewport.h"
#include "system_font.h"

#if defined(_WIN32)
#include "window_win32.h"
#define SONORE_HAS_NATIVE_WINDOW_BACKEND 1
#endif

#if defined(__linux__)
#include "window_x11.h"
#if defined(SONORE_X11_HEADERS)
#define SONORE_HAS_NATIVE_WINDOW_BACKEND 1
#endif
#endif

#if defined(__APPLE__)
#include "window_cocoa.h"
#define SONORE_HAS_NATIVE_WINDOW_BACKEND 1
#endif

namespace sonore {
namespace gfx {

// One alias per platform, chosen once, so the class below is written once. The
// two peers deliberately expose the same surface; a difference between them
// has to be written on purpose rather than arrived at.
#if defined(_WIN32)
using PlatformWindow = NativeWindow;
#elif defined(__APPLE__)
using PlatformWindow = NativeWindowCocoa;
#elif defined(__linux__) && defined(SONORE_X11_HEADERS)
using PlatformWindow = NativeWindowX11;
#endif

/**
 * A popup menu with a window under it.
 *
 * Holds all three things a live menu is -- the item list, the component that
 * draws it, and the window it lives in -- because all three have to die
 * together and at a moment nobody is on the stack of. A menu that outlives its
 * items draws freed strings; a window that outlives its grab locks the desktop.
 *
 * The caller keeps this object alive and calls closeIfDone() from its own tick.
 * There is no modal loop: a plugin runs inside a host's UI thread, and spinning
 * one there is how a plugin freezes a DAW.
 */
class PopupWindow {
public:
  PopupWindow() = default;
  PopupWindow(const PopupWindow&) = delete;
  PopupWindow& operator=(const PopupWindow&) = delete;
  ~PopupWindow() { close(); }

  static bool isAvailable() {
#if defined(SONORE_HAS_NATIVE_WINDOW_BACKEND)
    return true;
#else
    return false;
#endif
  }

  /**
   * Show `menu` with its top-left corner placed against the anchor.
   *
   * The anchor is in DESKTOP coordinates and is the control that opened the
   * menu -- its position and height, so the menu can drop below it and flip
   * above when there is no room. `onChosen` is called exactly once, with the
   * item's id or 0 for dismissed.
   */
  bool show(PopupMenu menu, Font font, float anchorX, float anchorY, float anchorHeight,
            float screenW, float screenH, std::function<void(int)> onChosen) {
#if defined(SONORE_HAS_NATIVE_WINDOW_BACKEND)
    close();
    if (menu.isEmpty()) return false;

    menu_ = std::move(menu);
    onChosen_ = std::move(onChosen);
    const float w = menu_.preferredWidth(font);
    const float h = menu_.preferredHeight();
    // The screen the ANCHOR is on, not the main one. A menu opened near the
    // bottom of a second monitor used to be checked against the first
    // monitor's height, decide it fitted, and be drawn off the bottom of the
    // screen it was actually on.
    (void) screenW;
    (void) screenH;
    const Point at = PopupMenu::placeOnScreen(anchorX, anchorY, anchorHeight, w, h);

    content_ = std::unique_ptr<PopupContent>(new PopupContent(menu_, font));
    content_->onDismiss = [this](int id) { finish(id); };
    window_.onDismissedOutside = [this]() { finish(0); };

    if (!window_.openPopup(*content_, (int) at.x, (int) at.y, (int) w, (int) h)) {
      content_.reset();
      onChosen_ = nullptr;
      return false;
    }
    open_ = true;
    return true;
#else
    (void) menu; (void) font; (void) anchorX; (void) anchorY; (void) anchorHeight;
    (void) screenW; (void) screenH; (void) onChosen;
    return false;
#endif
  }

  bool isOpen() const { return open_; }

  /** Drain input, and tear the window down once something has been chosen.
   *
   *  Separate from the choice itself on purpose: finish() runs inside the
   *  window procedure, and destroying a window from inside its own message
   *  handler is how a peer ends up painting into a half-freed object. */
  void tick() {
#if defined(SONORE_HAS_NATIVE_WINDOW_BACKEND)
    if (!open_) return;
    pump();
    if (done_) close();
#endif
  }

  void close() {
#if defined(SONORE_HAS_NATIVE_WINDOW_BACKEND)
    window_.close();
    content_.reset();
    open_ = false;
    done_ = false;
    // The callback is dropped LAST and only here: a caller may well have
    // destroyed whatever it captured by now.
    onChosen_ = nullptr;
#endif
  }

  void* handle() {
#if defined(SONORE_HAS_NATIVE_WINDOW_BACKEND)
    return window_.handle();
#else
    return nullptr;
#endif
  }

  PopupContent* content() { return content_.get(); }

private:
#if defined(SONORE_HAS_NATIVE_WINDOW_BACKEND)
  void pump() {
#if defined(__linux__) && defined(SONORE_X11_HEADERS)
    window_.processEvents();
#else
    window_.renderIfDirty();
#endif
  }

  /** Called from inside the window's own event handling. Records the answer and
   *  gets out; the window is closed by the next tick. */
  void finish(int id) {
    if (done_) return;
    done_ = true;
    if (onChosen_) onChosen_(id);
  }

  PlatformWindow window_;
#endif
  PopupMenu menu_;
  std::unique_ptr<PopupContent> content_;
  std::function<void(int)> onChosen_;
  bool open_ = false;
  bool done_ = false;
};

/**
 * A small window that shows a component and takes no input at all.
 *
 * A tooltip is not a menu. A menu grabs the pointer because it has to hear the
 * click that dismisses it; a tooltip that grabbed would stop the user reaching
 * the very control it is describing, and on macOS taking key focus would pull
 * it off the host every time one appeared.
 *
 * So it is the same platform window with the grab turned off, and it exists as
 * a separate class rather than a flag on PopupWindow because the two have
 * nothing else in common -- no dismissal, no choice, no callback.
 */
class TooltipWindow {
public:
  TooltipWindow() = default;
  TooltipWindow(const TooltipWindow&) = delete;
  TooltipWindow& operator=(const TooltipWindow&) = delete;
  ~TooltipWindow() { close(); }

  bool show(Component& content, int screenX, int screenY, int width, int height) {
#if defined(SONORE_HAS_NATIVE_WINDOW_BACKEND)
    close();
    if (width <= 0 || height <= 0) return false;
    if (!window_.openPopup(content, screenX, screenY, width, height, /*grabMouse=*/false))
      return false;
    open_ = true;
    return true;
#else
    (void) content; (void) screenX; (void) screenY; (void) width; (void) height;
    return false;
#endif
  }

  void close() {
#if defined(SONORE_HAS_NATIVE_WINDOW_BACKEND)
    window_.close();
    open_ = false;
#endif
  }

  bool isOpen() const { return open_; }

  void* handle() {
#if defined(SONORE_HAS_NATIVE_WINDOW_BACKEND)
    return window_.handle();
#else
    return nullptr;
#endif
  }

private:
#if defined(SONORE_HAS_NATIVE_WINDOW_BACKEND)
  PlatformWindow window_;
#endif
  bool open_ = false;
};

/**
 * A GenericEditor with a window around it.
 *
 * Owns the content, the attachments and the OS window. Non-copyable by
 * construction: the window procedure holds a pointer to this object.
 */
class NativeEditor {
public:
  NativeEditor() = default;
  NativeEditor(const NativeEditor&) = delete;
  NativeEditor& operator=(const NativeEditor&) = delete;
  ~NativeEditor() { close(); }

  /**
   * Whether this build, on this machine, can put a native editor on screen.
   *
   * The runtime half matters as much as the compile-time half on Linux: a
   * plugin built against Xlib still has to find libX11.so.6 at load, and a
   * Wayland-only session does not have it. Answering from the build alone
   * would produce an editor that opens into nothing.
   */
  static bool isAvailable() {
#if defined(__linux__) && defined(SONORE_X11_HEADERS)
    return x11::xlib().ok;
#elif defined(__APPLE__)
    // Runtime, like X11 and unlike Windows: the peer needs CoreGraphics
    // symbols and an NSView class, and a process without a UI has neither.
    return NativeWindowCocoa::isAvailable();
#elif defined(SONORE_HAS_NATIVE_WINDOW_BACKEND)
    return true;
#else
    return false;
#endif
  }

  /** Never null, and never vague: a caller that degrades has to be able to say
   *  what it degraded away from. */
  static const char* unavailableReason() {
#if defined(__linux__) && defined(SONORE_X11_HEADERS)
    return x11::xlib().ok ? "" : "libX11.so.6 is not on this machine";
#elif defined(__linux__)
    return "this plugin was built without the X11 headers, so it has no native window";
#elif defined(__APPLE__)
    return NativeWindowCocoa::isAvailable() ? ""
                                            : "CoreGraphics or AppKit is not loaded in this process";
#elif defined(SONORE_HAS_NATIVE_WINDOW_BACKEND)
    return "";
#else
    return "no native window backend for this platform";
#endif
  }

  /**
   * Build the editor and show it inside `parent`.
   *
   * `params` must outlive this object -- it is the descriptor's own table,
   * which is static in every generated plugin.
   */
  bool open(void* parent, const ParamInfo* params, int numParams, const EditorHost& host,
            int width, int height) {
#if defined(SONORE_HAS_NATIVE_WINDOW_BACKEND)
    close();
    // A missing typeface is not a failure to open. Font draws nothing when it
    // has no face, so the controls are all still there and still work; only
    // the labels are gone. A blank window would be the worse answer.
    auto face = systemTypeface();
    Font font = face ? Font(face, 13.0f) : Font();
    hasFont_ = face != nullptr;

    editor_ = std::unique_ptr<GenericEditor>(new GenericEditor(params, numParams, host, font));

    // A plugin with forty parameters makes an editor taller than a screen, and
    // a host given that size clips it or refuses it -- either way the last
    // parameters cannot be reached. The viewport is always there and shows a
    // bar only when there is something to scroll, so a four-knob plugin looks
    // exactly as it did.
    viewport_ = std::unique_ptr<Viewport>(new Viewport());
    viewport_->setBounds({0.0f, 0.0f, (float) width, (float) height});
    editor_->setSize((float) width - ScrollBar::kThickness, editor_->preferredHeight());
    viewport_->setViewedComponent(editor_.get());

    // ── The root, which used to be the viewport itself ─────────────────────
    //
    // A plain container now, holding the viewport and the resize border above
    // it. The border has to be a SIBLING drawn last rather than a parent: it
    // must be on top to receive the edge, and it must not be in the way of
    // anything else, which is what hitTestPoint gives it.
    //
    // It is a Component and not a Widget on purpose -- it paints nothing, so
    // whatever the editor draws still reaches the window unchanged.
    root_ = std::unique_ptr<Component>(new Component());
    root_->setBounds({0.0f, 0.0f, (float) width, (float) height});
    root_->setAccessibilityIgnored(true);
    root_->addChild(viewport_.get());

    border_ = std::unique_ptr<ResizableBorder>(new ResizableBorder());
    border_->constrainer = constrainerFor(limits_);
    // Left and top off: a plugin editor cannot move its own frame, so those
    // two edges could only grow the window the wrong way. See resizer.h.
    border_->setDraggableEdges(false, false, true, true);
    border_->setCurrentSize((float) width, (float) height);
    border_->setBounds({0.0f, 0.0f, (float) width, (float) height});
    // A fixed-size plugin gets no border at all rather than a dead one, so
    // there is no edge that shows a resize cursor and then does nothing.
    if (!limits_.resizableHorizontally && !limits_.resizableVertically) {
      border_->setVisible(false);
    }
    root_->addChild(border_.get());

    ResizableBorder* border = border_.get();
    border_->onResize = [this, border](float, float, float w, float h) {
      // ASKED FOR, not applied. The host owns the frame: an editor that
      // resized itself would be a window of one size inside a frame of
      // another. If the host agrees it calls back through setSize, and that
      // is the only thing that actually changes anything here.
      border->setCurrentSize(w, h);
      if (onRequestResize) onRequestResize((int) w, (int) h);
    };

    // SUBSCRIBED, not merely available. The window has carried a 33 ms clock
    // since it was written and nothing was listening to it: tick() existed and
    // was only ever called from the webview's callback, which a native editor
    // by definition does not have. The editor therefore never followed
    // automation, a preset load or host undo -- it looked completely correct
    // until something moved a parameter without the mouse.
    //
    // Set BEFORE open(): the Win32 peer starts its timer inside open, and X11
    // fires this from its first processEvents.
    window_.onTick = [this]() {
      // ONE clock for the frame, read once and handed to everything in it.
      //
      // It used to be counted inside updateTooltip, which was the only thing
      // that needed a time -- so when the editor's sections started animating
      // there was no clock to give them, and a second counter would have been
      // a second answer to "what time is it" drifting against the first.
      const double now = (double) (++tickCount_) * kTickSeconds;

      if (editor_) {
        editor_->sync();
        // Sections opening and closing. Re-ask the height only when something
        // MOVED: it is the viewport's scroll range, and recomputing it every
        // frame of a still editor would be work for nothing.
        if (editor_->tickAnimations(now) && viewport_) {
          editor_->setSize(editor_->bounds().w, editor_->preferredHeight());
          viewport_->setViewedComponent(editor_.get());
        }
      }
      updateTooltip(now);
      // The menu rides the editor's clock rather than one of its own. It is a
      // separate window with separate input, and something has to pump it or a
      // drop-down opens and then ignores the mouse.
      popup_.tick();
    };

    // A stepped parameter drops down a real menu, ticked at the current
    // choice. GenericEditor cannot do this itself: a popup is a top-level
    // window and it is a Component.
    editor_->onOpenComboMenu = [this](ComboBox& box) { openMenuFor(box); };

    // Commands see a key only after the tree has declined it. See the comment
    // on MouseRouter::onUnhandledKey for why that order is the whole point.
    commandsRouterHook_ = [this](const KeyPress& key) { return commands_.keyPressed(key); };

    if (!window_.open(parent, *root_, width, height)) {
      root_.reset();
      border_.reset();
      viewport_.reset();
      editor_.reset();
      return false;
    }
    if (MouseRouter* router = window_.router()) router->onUnhandledKey = commandsRouterHook_;
    return true;
#else
    (void) parent; (void) params; (void) numParams; (void) host; (void) width; (void) height;
    return false;
#endif
  }

  /**
   * The plugin's named actions -- undo, save preset, bypass.
   *
   * Register them before open(), or after; the hook is installed either way.
   * A key reaches these only when nothing in the component tree took it.
   */
  CommandManager& commands() { return commands_; }
  const CommandManager& commands() const { return commands_; }

  void close() {
#if defined(SONORE_HAS_NATIVE_WINDOW_BACKEND)
    // The menu first of all: it is a separate top-level window holding a
    // pointer grab, and an editor that closed with one still up would leave the
    // desktop unable to click on anything.
    popup_.close();
    tooltipWindow_.close();
    tooltips_.clear();
    // Then the window: its procedure paints the content, and freeing the
    // content while a WM_PAINT can still arrive is the same bug in the other
    // order.
    window_.close();
    // Outside in, because each of these holds a pointer to the next. The root
    // holds the viewport and the border; the viewport holds the editor. Freeing
    // any of them before the thing that points at it is the same bug the
    // window_.close() above avoids, one level down.
    root_.reset();
    border_.reset();
    viewport_.reset();
    editor_.reset();
#endif
  }

  bool isOpen() const {
#if defined(SONORE_HAS_NATIVE_WINDOW_BACKEND)
    return editor_ != nullptr && viewport_ != nullptr && window_.isOpen();
#else
    return false;
#endif
  }

  /** Whether text is being drawn. False means every label is blank because no
   *  typeface was found on this machine -- worth saying out loud rather than
   *  leaving a user to wonder. */
  bool hasText() const { return hasFont_; }

  void* handle() const {
#if defined(SONORE_HAS_NATIVE_WINDOW_BACKEND)
    return window_.handle();
#else
    return nullptr;
#endif
  }

  void setVisible(bool shouldBeVisible) {
#if defined(SONORE_HAS_NATIVE_WINDOW_BACKEND)
    if (isOpen()) window_.setVisible(shouldBeVisible);
#else
    (void) shouldBeVisible;
#endif
  }

  /**
   * `width` and `height` are LOGICAL units.
   *
   * They used to be device pixels, and the caller did the multiplying: the CLAP
   * wrapper computed `logical * scale` and passed the product. Which meant the
   * component tree was laid out to a DEVICE size, so at 200% a 620-point editor
   * became a 1240-unit one drawn at 1240 pixels -- the same controls at the same
   * pixel sizes, which on that display is half their intended physical size.
   * The interface did not scale. It got denser and smaller.
   *
   * The scale is now setScale's business and nobody else's.
   */
  void setSize(int width, int height) {
#if defined(SONORE_HAS_NATIVE_WINDOW_BACKEND)
    if (!isOpen()) return;
    window_.setLogicalSize((float) width, (float) height);
    layOutContent();
#else
    (void) width; (void) height;
#endif
  }

  /**
   * What one logical unit is worth on this display.
   *
   * The host's answer, when it has one -- it may be compositing our view into a
   * surface it scales itself, which is a thing only it can know. Otherwise the
   * peer has already asked the screen and this is never called.
   *
   * The window keeps its LOGICAL size across the change, which is the point: the
   * editor stays the same physical size on the desk and gains pixels.
   */
  void setScale(float scale) {
#if defined(SONORE_HAS_NATIVE_WINDOW_BACKEND)
    if (!isOpen()) return;
    const float logicalW = window_.logicalWidth();
    const float logicalH = window_.logicalHeight();
    window_.setScale(scale);
    window_.setLogicalSize(logicalW, logicalH);
    layOutContent();
#else
    (void) scale;
#endif
  }

  float scale() const {
#if defined(SONORE_HAS_NATIVE_WINDOW_BACKEND)
    return window_.scale();
#else
    return 1.0f;
#endif
  }

  /**
   * One UI frame: pull the host's values in, then repaint if that changed
   * anything.
   *
   * Sync first. Repainting before syncing draws the frame BEFORE the
   * automation that arrived, which is a whole frame of lag for free.
   */
  /** The rows are laid out to the WIDTH, so a host making the editor wider has
   *  to reach the content and not only the viewport around it. The height is the
   *  content's own -- that is the whole point of scrolling. */
  void layOutContent() {
#if defined(SONORE_HAS_NATIVE_WINDOW_BACKEND)
    if (!editor_ || !viewport_) return;
    const float w = window_.logicalWidth(), h = window_.logicalHeight();
    if (root_) root_->setBounds({0.0f, 0.0f, w, h});
    viewport_->setBounds({0.0f, 0.0f, w, h});
    if (border_) {
      border_->setBounds({0.0f, 0.0f, w, h});
      // Told the size the window ACTUALLY became, not the size that was asked
      // for. A host may give less than was requested, and a border still
      // measuring from the request would make the next drag jump.
      border_->setCurrentSize(w, h);
    }
    editor_->setSize(w - ScrollBar::kThickness, editor_->preferredHeight());
    viewport_->contentResized();
#endif
  }

  void tick() {
#if defined(SONORE_HAS_NATIVE_WINDOW_BACKEND)
    if (!isOpen()) return;
    editor_->sync();
    window_.renderIfDirty();
#endif
  }


  /**
   * The X socket, or -1.
   *
   * Windows has WM_TIMER and a message loop the OS runs. X11 has neither: the
   * host owns the loop, and CLAP offers clap_host_posix_fd_support precisely so
   * a plugin can hand over a descriptor and be called back when something
   * arrives. A wrapper that ignores this can still pump on a timer, at up to
   * 33 ms of lag on every click.
   */
  int connectionFd() const {
#if defined(__linux__) && defined(SONORE_X11_HEADERS)
    return window_.connectionFd();
#else
    return -1;
#endif
  }

  /**
   * Drain whatever input is waiting, on platforms where the SDK has to ask.
   *
   * A no-op on Windows, where the OS delivers messages to the window procedure
   * without being asked. Calling it anyway is what lets a wrapper have one
   * code path.
   */
  void pumpEvents() {
#if defined(__linux__) && defined(SONORE_X11_HEADERS)
    if (isOpen()) window_.processEvents();
#endif
    // Nothing on Windows or macOS: both deliver input to the window without
    // being asked. Calling it anyway is what lets a wrapper have one path.
  }

  /** Open now, for a test that wants to look at one. */
  PopupWindow& popup() { return popup_; }

  /** What the tooltip logic currently thinks, for the same reason. */
  TooltipManager& tooltips() { return tooltips_; }

  /**
   * What the PLUGIN declared, from its descriptor. Set before open().
   *
   * Taken rather than read, because gfx/ does not know about any particular
   * plugin -- the wrapper holds kDesc and hands the numbers down. Both halves
   * come from the same struct, so what the drag allows and what the host was
   * told cannot drift apart.
   */
  void setResizeLimits(const EditorConstraints& limits) {
    limits_ = limits;
    if (border_) border_->constrainer = constrainerFor(limits);
  }

  /**
   * The user dragged an edge and this size is being ASKED for.
   *
   * Set by the wrapper, which is the only thing that can talk to the host:
   * clap_host_gui::request_resize, or IPlugFrame::resizeView. Left unset the
   * border still constrains and still reports, and nothing happens -- which is
   * the correct behaviour for a format with no way to ask.
   */
  std::function<void(int width, int height)> onRequestResize;

  /** For tests and for the standalone, which drive components without a host. */
  GenericEditor* content() { return editor_.get(); }

  /** The border, for the tests that drive a drag without a mouse. */
  ResizableBorder* resizeBorder() { return border_.get(); }

#if defined(SONORE_HAS_NATIVE_WINDOW_BACKEND)
  /**
   * The platform peer. Exposed for the tests that have to ask it something only
   * it knows -- where an input method should place its candidate list, which is
   * computed from the focused component and the device scale.
   *
   * The WHOLE declaration is inside the guard, not just its body. PlatformWindow
   * is an alias that only exists when there is a backend to alias, so a method
   * merely RETURNING one fails to compile on a build without one -- which is a
   * real configuration (WSL without the X11 headers) and is what the Linux leg
   * of the gate exists to catch. It caught this.
   */
  PlatformWindow* window() { return &window_; }
#endif

private:
#if defined(SONORE_HAS_NATIVE_WINDOW_BACKEND)
  /**
   * One tooltip frame.
   *
   * The clock is counted in TICKS rather than read from a system call. The peer
   * ticks at 33 ms, so this advances at real speed and TooltipManager stays a
   * pure function of the time it is handed -- which is what makes every one of
   * its rules an exact test instead of a sleep.
   *
   * The counting now happens in onTick and the time arrives as an argument,
   * because the tooltip stopped being the only thing in a frame that needs to
   * know when it is.
   */
  void updateTooltip(double now) {
    if (!isOpen() || !window_.router()) return;

    // A menu is up: it covers the editor and holds the pointer, so a tooltip
    // for whatever is underneath would describe something unreachable.
    if (popup_.isOpen()) {
      if (tooltips_.clear()) tooltipWindow_.close();
      return;
    }

    const Point mouse = window_.lastMousePosition();
    if (!tooltips_.update(window_.router()->hovered(), mouse, now)) return;

    tooltipWindow_.close();
    if (!tooltips_.isVisible()) return;

    const Font font = editor_ ? editor_->font() : Font();
    const Rect box = tooltips_.boundsFor(font);
    tooltipLabel_ = std::unique_ptr<Label>(new Label(tooltips_.text(), Justify::Centred));
    tooltipLabel_->setFont(font);
    tooltipLabel_->setInterceptsMouse(false);

    int originX = 0, originY = 0;
    desktopOriginOfWindow(&originX, &originY);
    // BELOW and to the right of the pointer, which is where every desktop puts
    // one: above would put it under the cursor's own hotspot and the pointer
    // would cover the first word.
    // Below and right of the pointer, then nudged back onto whichever screen
    // the pointer is on -- a tooltip near the right edge would otherwise hang
    // off it, and near the edge of a SECOND monitor would hang off that.
    const Point where = PopupMenu::placeOnScreen((float) originX + mouse.x + 12.0f,
                                                 (float) originY + mouse.y + 8.0f, 12.0f, box.w,
                                                 box.h);
    tooltipWindow_.show(*tooltipLabel_, (int) where.x, (int) where.y, (int) box.w, (int) box.h);
  }

  void desktopOriginOfWindow(int* x, int* y) {
#if defined(_WIN32)
    POINT origin{0, 0};
    ClientToScreen((HWND) window_.handle(), &origin);
    *x = (int) origin.x;
    *y = (int) origin.y;
#else
    window_.desktopOrigin(x, y);
#endif
  }

  /**
   * Turn a ComboBox into a drop-down anchored under it.
   *
   * The anchor has to be in DESKTOP coordinates, because a menu is placed
   * against the screen's edges: the box knows where it is inside the editor,
   * the editor's window knows where it is on the desktop, and only here are
   * both in scope.
   */
  void openMenuFor(ComboBox& box) {
    const std::vector<std::string>& items = box.items();
    if (items.empty()) return;

    PopupMenu menu;
    // Ids start at 1. Zero is "dismissed", so an index-as-id would make
    // choosing the first item indistinguishable from closing the menu -- and
    // the first item is the one people pick most.
    for (size_t i = 0; i < items.size(); ++i)
      menu.addItem((int) i + 1, items[i], true, (int) i == box.selectedIndex());

    float screenX = 0.0f, screenY = 0.0f, screenW = 0.0f, screenH = 0.0f;
    desktopPositionOf(box, &screenX, &screenY, &screenW, &screenH);

    ComboBox* target = &box;
    popup_.show(std::move(menu), editor_ ? editor_->font() : Font(), screenX, screenY,
                box.bounds().h, screenW, screenH, [target](int id) {
                  if (id > 0) target->setSelectedIndex(id - 1);
                });
  }

  /** Where a component sits on the desktop, and how big the desktop is. */
  void desktopPositionOf(Component& c, float* x, float* y, float* screenW, float* screenH) {
    // Inside the editor first: a component's bounds are relative to its parent,
    // and every row of the generic editor is a direct child, so one hop is
    // enough today -- but walking the chain costs nothing and does not break
    // the day a row becomes a group.
    // All the way to the ROOT, not to the editor. The editor now sits inside a
    // viewport at a negative offset, so stopping early would place a menu where
    // the row would be if nothing had ever been scrolled.
    float localX = 0.0f, localY = 0.0f;
    for (Component* p = &c; p != nullptr; p = p->parent()) {
      localX += p->bounds().x;
      localY += p->bounds().y;
    }
#if defined(_WIN32)
    POINT origin{(LONG) localX, (LONG) localY};
    ClientToScreen((HWND) window_.handle(), &origin);
    *x = (float) origin.x;
    *y = (float) origin.y;
    *screenW = (float) GetSystemMetrics(SM_CXVIRTUALSCREEN);
    *screenH = (float) GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (*screenW <= 0.0f) *screenW = (float) GetSystemMetrics(SM_CXSCREEN);
    if (*screenH <= 0.0f) *screenH = (float) GetSystemMetrics(SM_CYSCREEN);
#else
    int wx = 0, wy = 0, sw = 0, sh = 0;
    window_.desktopOrigin(&wx, &wy);
    window_.screenSize(&sw, &sh);
    *x = (float) wx + localX;
    *y = (float) wy + localY;
    *screenW = (float) sw;
    *screenH = (float) sh;
#endif
    // A screen of zero would place every menu at the origin. Better to trust
    // the anchor and let the menu hang off an edge than to pile them all in
    // one corner.
    if (!(*screenW > 0.0f)) *screenW = 1e6f;
    if (!(*screenH > 0.0f)) *screenH = 1e6f;
  }
#endif

  std::unique_ptr<GenericEditor> editor_;
  std::unique_ptr<Component> root_;
  std::unique_ptr<ResizableBorder> border_;
  EditorConstraints limits_;
  std::unique_ptr<Viewport> viewport_;
  PopupWindow popup_;
  TooltipManager tooltips_;
  TooltipWindow tooltipWindow_;
  std::unique_ptr<Label> tooltipLabel_;
  /** What the peers tick at. Named because two things now divide by it. */
  static constexpr double kTickSeconds = 0.033;
  uint64_t tickCount_ = 0;
  bool hasFont_ = false;
  CommandManager commands_;
  std::function<bool(const KeyPress&)> commandsRouterHook_;
#if defined(SONORE_HAS_NATIVE_WINDOW_BACKEND)
  PlatformWindow window_;
#endif
};

} // namespace gfx
} // namespace sonore
