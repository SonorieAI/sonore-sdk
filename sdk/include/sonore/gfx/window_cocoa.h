// SPDX-License-Identifier: Apache-2.0
//
// The Cocoa window, so the native UI runs on the third platform.
//
// ── Written against the runtime, not in Objective-C ─────────────────────────
//
// Same reason as webview_cocoa.h: the SDK stays plain C++, and a generated
// project needs no .mm file, no mixed-language build and no Xcode-specific
// CMake. Every Cocoa call here is objc_msgSend on a class looked up by name,
// which is exactly what the Objective-C compiler emits anyway.
//
// The cost is that an NSView subclass has to be REGISTERED at runtime, with C
// functions as its method implementations. There is no way around that: drawing
// and input on macOS both arrive as overridden methods, and a view that
// overrides nothing draws nothing and receives nothing.
//
// ── What macOS does differently ─────────────────────────────────────────────
//
//  - Its y axis points UP, and a view's coordinates start at the bottom left.
//    Every other surface in this SDK -- the rasteriser, the component tree,
//    every peer -- is y-down. isFlipped is overridden to return YES, which
//    makes one view y-down and leaves the conversion in one place instead of
//    scattering `height - y` through the input handlers.
//
//  - There is no pointer grab. X11 has XGrabPointer and Windows has
//    SetCapture; macOS has neither, and a popup dismisses because its window
//    stops being key. That is a genuinely different mechanism, not a
//    translation of the same one.
//
//  - Coordinates for a click arrive in WINDOW space and have to be converted
//    into the view. Using them unconverted is the classic Cocoa bug and it
//    looks like an editor whose controls respond a title-bar's height away
//    from where they are.
#pragma once

#if defined(__APPLE__) || defined(SONORE_APPLE_SYNTAX_CHECK)

#include <dlfcn.h>
#include <objc/message.h>
#include <objc/objc.h>
#include <objc/runtime.h>

#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "backing.h"
#include "component.h"
#include "displays.h"
#include "graphics.h"

namespace sonore {
namespace gfx {
namespace cocoa {

/** objc_msgSend must be called through a correctly-typed pointer: it is a
 *  trampoline, and on arm64 variadic and regular calls use different registers,
 *  so a wrongly typed call corrupts arguments. Same helper the webview host
 *  uses, duplicated rather than shared so this file has no dependency on the
 *  webview backend being compiled in. */
template <typename Ret, typename... Args>
inline Ret msg(id target, SEL selector, Args... args) {
  using Fn = Ret (*)(id, SEL, Args...);
  return reinterpret_cast<Fn>(objc_msgSend)(target, selector, args...);
}

inline id cls(const char* name) { return (id) objc_getClass(name); }

struct CGPoint2 { double x, y; };
struct CGSize2 { double width, height; };
struct CGRect2 { CGPoint2 origin; CGSize2 size; };

/** The bit of CoreGraphics needed to put a rectangle of pixels on screen.
 *  Resolved with dlsym rather than linked, so nothing here adds a framework a
 *  generated project would have to name. */
struct CoreGraphicsApi {
  bool ok = false;
  void* (*CGColorSpaceCreateDeviceRGB)() = nullptr;
  void (*CGColorSpaceRelease)(void*) = nullptr;
  void* (*CGDataProviderCreateWithData)(void*, const void*, size_t, void*) = nullptr;
  void (*CGDataProviderRelease)(void*) = nullptr;
  void* (*CGImageCreate)(size_t, size_t, size_t, size_t, size_t, void*, uint32_t, void*,
                         const double*, bool, int) = nullptr;
  void (*CGImageRelease)(void*) = nullptr;
  void (*CGContextDrawImage)(void*, CGRect2, void*) = nullptr;
};

inline const CoreGraphicsApi& coreGraphics() {
  static CoreGraphicsApi api = [] {
    CoreGraphicsApi a;
    // Already loaded in any process with a UI; this only takes a handle to it.
    void* lib = dlopen("/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics", RTLD_LAZY);
    if (!lib)
      lib = dlopen("/System/Library/Frameworks/ApplicationServices.framework/ApplicationServices",
                   RTLD_LAZY);
    if (!lib) return a;
    bool all = true;
    auto bind = [&](const char* name, void* slot) {
      void* sym = dlsym(lib, name);
      if (!sym) all = false;
      std::memcpy(slot, &sym, sizeof(sym));
    };
    bind("CGColorSpaceCreateDeviceRGB", &a.CGColorSpaceCreateDeviceRGB);
    bind("CGColorSpaceRelease", &a.CGColorSpaceRelease);
    bind("CGDataProviderCreateWithData", &a.CGDataProviderCreateWithData);
    bind("CGDataProviderRelease", &a.CGDataProviderRelease);
    bind("CGImageCreate", &a.CGImageCreate);
    bind("CGImageRelease", &a.CGImageRelease);
    bind("CGContextDrawImage", &a.CGContextDrawImage);
    a.ok = all;
    return a;
  }();
  return api;
}

// kCGImageAlphaPremultipliedLast | kCGBitmapByteOrderDefault. The rasteriser
// produces premultiplied RGBA, which is exactly this layout -- so unlike the
// Win32 and X11 peers, macOS needs NO channel swap at the blit. Stating the
// constant numerically rather than including CGImage.h keeps this file to the
// three runtime headers.
constexpr uint32_t kAlphaPremultipliedLast = 1u;

} // namespace cocoa

class NativeWindowCocoa;

/** Every registered method needs a C function, and every one of them needs to
 *  find the peer that owns the view. It is stored in an instance variable, set
 *  once when the view is made. */
inline NativeWindowCocoa* peerOfView(id view) {
  void* value = nullptr;
  object_getInstanceVariable(view, "sonorePeer", &value);
  return static_cast<NativeWindowCocoa*>(value);
}

/**
 * A component tree in an NSView, with the same surface as the other two peers.
 *
 * Identical API on purpose: NativeEditor holds whichever one the platform has
 * and knows nothing about which, so a difference between platforms has to be
 * written deliberately rather than arrived at.
 */
class NativeWindowCocoa {
public:
  NativeWindowCocoa() = default;
  NativeWindowCocoa(const NativeWindowCocoa&) = delete;
  NativeWindowCocoa& operator=(const NativeWindowCocoa&) = delete;
  ~NativeWindowCocoa() { close(); }

  static bool isAvailable() { return cocoa::coreGraphics().ok && cocoa::cls("NSView") != nullptr; }

  std::function<void()> onTick;
  std::function<void()> onDismissedOutside;

  /**
   * Open inside a host's NSView.
   *
   * `parent` is the NSView* a plugin format hands over -- a real pointer here,
   * unlike X11 where the same argument carries an integer through one. Null
   * makes a bare view with no superview, which is what a test would use if
   * there were a machine to run one on.
   */
  bool open(void* parent, Component& content, int width, int height) {
    return openInternal(parent, content, width, height, 0, 0, /*popup=*/false);
  }

  /**
   * Open as a POPUP: a borderless window above everything, at desktop
   * coordinates.
   *
   * There is no pointer grab on macOS. A popup dismisses because its window
   * stops being key -- which is a different mechanism from the other two peers,
   * not a translation of theirs, and the reason this is the one platform where
   * a click outside is a NOTIFICATION rather than an event with negative
   * coordinates.
   */
  bool openPopup(Component& content, int screenX, int screenY, int width, int height,
                 bool grabMouse = true) {
    grabsMouse_ = grabMouse;
    return openInternal(nullptr, content, width, height, screenX, screenY, /*popup=*/true);
  }

  bool isPopup() const { return isPopup_; }

  /** Where the pointer last was, in this window's coordinates. */
  Point lastMousePosition() const { return lastMouse_; }

  void close() {
    if (timer_) {
      cocoa::msg<void>(timer_, sel_registerName("invalidate"));
      timer_ = nullptr;
    }
    if (window_) {
      cocoa::msg<void>(window_, sel_registerName("orderOut:"), (id) nullptr);
      cocoa::msg<void>(window_, sel_registerName("close"));
      // Ours to release, because makePopupWindow turned OFF
      // releasedWhenClosed. Left on -- which is an NSWindow's default -- the
      // close above would free the window, the window would release its
      // content view, and the release of view_ below would be one too many on
      // an object already gone. That is a use-after-free that only shows up
      // once a user has opened and closed a menu enough times, which is to say
      // in somebody's session and not here.
      cocoa::msg<void>(window_, sel_registerName("release"));
      window_ = nullptr;
    }
    if (view_) {
      // The back-pointer FIRST. Closing sends messages the view may still
      // handle, and one that found a live pointer here would paint into an
      // object halfway gone -- the same ordering the Win32 peer needs around
      // DestroyWindow.
      object_setInstanceVariable(view_, "sonorePeer", nullptr);
      // And the notification observer, before the view goes. NSNotificationCenter
      // holds observers WITHOUT retaining them, so one left registered is a
      // dangling pointer the next time any window resigns key anywhere in the
      // host -- which is constantly. Unconditional rather than only for popups:
      // removeObserver: on an object that never registered is defined to do
      // nothing, and a conditional here would be one more thing to get wrong.
      id centre = cocoa::msg<id>(cocoa::cls("NSNotificationCenter"),
                                 sel_registerName("defaultCenter"));
      if (centre) cocoa::msg<void>(centre, sel_registerName("removeObserver:"), view_);
      cocoa::msg<void>(view_, sel_registerName("removeFromSuperview"));
      cocoa::msg<void>(view_, sel_registerName("release"));
      view_ = nullptr;
    }
    delete router_;
    router_ = nullptr;
    content_ = nullptr;
    // The backing store too. It holds the same pointer and OUTLIVES the window
    // -- a peer is a member of NativeEditor and is reused across every open --
    // so leaving it set means the next paint reaches a component the editor has
    // already destroyed. On Win32 that was not theoretical: CreateWindowExW
    // dispatches WM_SIZE synchronously, so the second open() of a reused editor
    // painted through the first one's freed viewport before it had returned.
    backing_.setContent(nullptr);
    isPopup_ = false;
  }

  void* handle() const { return (void*) view_; }
  bool isOpen() const { return view_ != nullptr; }
  Bitmap& bitmap() { return backing_.bitmap(); }

  /**
   * The scale, for a host that insists.
   *
   * CLAP says outright that set_scale is not used on macOS, and it is right:
   * Cocoa applies the backing factor itself and a plugin that also applied one
   * would square it. This exists so the wrapper has something to call rather
   * than an #ifdef, and so a host that sets it to exactly what the window
   * already reports changes nothing.
   */
  void setScale(float scale) {
    backing_.setScale(scale);
    render();
  }

  float scale() const { return backing_.scale(); }
  float logicalWidth() const { return backing_.logicalWidth(); }
  float logicalHeight() const { return backing_.logicalHeight(); }

  /** POINTS, which on this platform is what setSize already takes. Here so all
   *  three peers answer the same call. */
  void setLogicalSize(float width, float height) {
    setSize((int) (width + 0.5f), (int) (height + 0.5f));
  }
  MouseRouter* router() { return router_; }

  /** No socket to poll: Cocoa's run loop is the host's, and the NSTimer set up
   *  in open() rides it. Present so a wrapper has one code path. */
  int connectionFd() const { return -1; }
  void processEvents() {}

  void setVisible(bool shouldBeVisible) {
    if (!isOpen()) return;
    cocoa::msg<void>(view_, sel_registerName("setHidden:"), (BOOL) (shouldBeVisible ? NO : YES));
    if (window_ && shouldBeVisible)
      cocoa::msg<void>(window_, sel_registerName("orderFront:"), (id) nullptr);
  }

  void setSize(int width, int height) {
    if (!isOpen() || width <= 0 || height <= 0) return;
    const cocoa::CGRect2 frame{{0.0, 0.0}, {(double) width, (double) height}};
    using SetFrame = void (*)(id, SEL, cocoa::CGRect2);
    reinterpret_cast<SetFrame>(objc_msgSend)(view_, sel_registerName("setFrame:"), frame);
    resized(width, height);
  }

  /** Where this view's top-left corner is on the desktop, for placing a menu. */
  void desktopOrigin(int* x, int* y) const {
    *x = 0;
    *y = 0;
    if (!isOpen()) return;
    id window = cocoa::msg<id>(view_, sel_registerName("window"));
    if (!window) return;
    // Cocoa's screen origin is the BOTTOM left and its y grows upward, so the
    // top of a view is (screenHeight - windowTop). Returning the raw value
    // would put every menu the wrong distance up the screen -- and on a
    // single-monitor desk it would look almost right, which is worse.
    using FrameFn = cocoa::CGRect2 (*)(id, SEL);
    const cocoa::CGRect2 wf =
        reinterpret_cast<FrameFn>(objc_msgSend)(window, sel_registerName("frame"));
    id screen = cocoa::msg<id>(window, sel_registerName("screen"));
    if (!screen) screen = cocoa::msg<id>(cocoa::cls("NSScreen"), sel_registerName("mainScreen"));
    double screenH = 0.0;
    if (screen) {
      const cocoa::CGRect2 sf =
          reinterpret_cast<FrameFn>(objc_msgSend)(screen, sel_registerName("frame"));
      screenH = sf.origin.y + sf.size.height;
    }
    *x = (int) wf.origin.x;
    *y = (int) (screenH - (wf.origin.y + wf.size.height));
  }

  void screenSize(int* width, int* height) const {
    *width = 0;
    *height = 0;
    id screen = cocoa::msg<id>(cocoa::cls("NSScreen"), sel_registerName("mainScreen"));
    if (!screen) return;
    using FrameFn = cocoa::CGRect2 (*)(id, SEL);
    const cocoa::CGRect2 sf =
        reinterpret_cast<FrameFn>(objc_msgSend)(screen, sel_registerName("frame"));
    *width = (int) sf.size.width;
    *height = (int) sf.size.height;
  }

  bool renderIfDirty() {
    if (!isOpen() || !content_ || !content_->isDirty()) return false;
    render();
    return true;
  }

  // ── Called from the registered method implementations ────────────────────

  void drawInto(void* cgContext) {
    const cocoa::CoreGraphicsApi& cg = cocoa::coreGraphics();
    Bitmap& bm = backing_.bitmap();
    if (!cg.ok || !cgContext || bm.isEmpty()) return;

    const size_t w = (size_t) bm.width(), h = (size_t) bm.height();
    // No channel swap. The rasteriser produces PREMULTIPLIED RGBA and that is
    // exactly kCGImageAlphaPremultipliedLast, so macOS is the one platform
    // where the bitmap goes straight out. Win32 wants BGRA and X11 wants BGRA;
    // both pay for it at their own edge.
    void* provider = cg.CGDataProviderCreateWithData(nullptr, bm.data(), w * h * 4u, nullptr);
    if (!provider) return;
    void* space = cg.CGColorSpaceCreateDeviceRGB();
    void* image = cg.CGImageCreate(w, h, 8, 32, w * 4u, space, cocoa::kAlphaPremultipliedLast,
                                   provider, nullptr, /*shouldInterpolate=*/false,
                                   /*intent=*/0);
    if (image) {
      // The image is w x h DEVICE pixels; the context is in POINTS. Drawing it
      // into a w x h point rectangle would draw a Retina bitmap at twice the
      // size of the view and show its top-left quarter.
      const double scale = (double) backing_.scale();
      const cocoa::CGRect2 rect{{0.0, 0.0}, {(double) w / scale, (double) h / scale}};
      cg.CGContextDrawImage(cgContext, rect, image);
      cg.CGImageRelease(image);
    }
    if (space) cg.CGColorSpaceRelease(space);
    cg.CGDataProviderRelease(provider);
  }

  void handleMouseDown(Point p, int clickCount, bool shift, bool ctrl, bool alt,
                       bool rightButton = false) {
    if (!router_) return;
    if (isPopup_ && grabsMouse_) {
      const Rect bounds = content_ ? content_->bounds() : Rect();
      if (!bounds.contains(p)) {
        if (onDismissedOutside) onDismissedOutside();
        return;
      }
    }
    router_->mouseDown(p, clickCount, shift, ctrl, alt, rightButton);
    renderIfDirty();
  }

  void handleMouseUp(Point p, bool shift, bool ctrl, bool alt) {
    if (!router_) return;
    router_->mouseUp(p, shift, ctrl, alt);
    renderIfDirty();
  }

  void handleMouseMove(Point p, bool shift, bool ctrl, bool alt) {
    lastMouse_ = p;
    if (!router_) return;
    router_->mouseMove(p, shift, ctrl, alt);
    applyCursor();
    renderIfDirty();
  }

  /**
   * Set the pointer's shape.
   *
   * Only when it changes: `set` on an NSCursor is cheap but not free, and
   * Cocoa resets the cursor itself when the pointer crosses a tracking area
   * boundary, so calling on every move would fight that rather than cooperate.
   */
  void applyCursor() {
    if (!router_) return;
    const MouseCursor wanted = router_->cursorForPointer();
    if (wanted == currentCursor_) return;
    currentCursor_ = wanted;

    // Class methods on NSCursor, which return the user's own themed cursors.
    const char* selector = "arrowCursor";
    switch (wanted) {
      case MouseCursor::Pointing: selector = "pointingHandCursor"; break;
      case MouseCursor::DragVertical: selector = "resizeUpDownCursor"; break;
      case MouseCursor::DragHorizontal: selector = "resizeLeftRightCursor"; break;
      // macOS has no public diagonal resize cursor. Rather than reach for a
      // private one -- which would be an App Store rejection and could vanish
      // in any release -- the up-down one is used, which at least says
      // "resize".
      // Cocoa has no diagonal resize cursor in its public set, so the corner
      // borrows the vertical one -- the same compromise every Mac application
      // that is not a window frame makes. Compiled on macOS, unrun like the
      // rest of this file: no macOS test opens a window yet.
      case MouseCursor::ResizeCorner: selector = "resizeUpDownCursor"; break;
      case MouseCursor::ResizeLeftRight: selector = "resizeLeftRightCursor"; break;
      case MouseCursor::ResizeUpDown: selector = "resizeUpDownCursor"; break;
      case MouseCursor::Text: selector = "IBeamCursor"; break;
      case MouseCursor::Wait: selector = "arrowCursor"; break;
      default: break;
    }
    id cursor = cocoa::msg<id>(cocoa::cls("NSCursor"), sel_registerName(selector));
    if (cursor) cocoa::msg<void>(cursor, sel_registerName("set"));
  }

  void handleMouseExit() {
    if (!router_) return;
    router_->mouseExitWindow();
    renderIfDirty();
  }

  void handleWheel(Point p, float delta, bool shift, bool ctrl, bool alt) {
    if (!router_) return;
    router_->mouseWheel(p, delta, shift, ctrl, alt);
    renderIfDirty();
  }

  bool handleFileDrag(const std::vector<std::string>& files, Point p) {
    if (!router_) return false;
    const bool accept = router_->fileDragMove(files, p);
    renderIfDirty();
    return accept;
  }

  void handleFileDragExit() {
    if (!router_) return;
    router_->fileDragExit();
    renderIfDirty();
  }

  bool handleFilesDropped(const std::vector<std::string>& files, Point p) {
    if (!router_) return false;
    const bool taken = router_->filesDropped(files, p);
    renderIfDirty();
    return taken;
  }

  bool handleKey(const KeyPress& key) {
    if (!router_) return false;
    const bool handled = router_->keyPressed(key);
    renderIfDirty();
    return handled;
  }

  void handleTimer() {
    if (onTick) onTick();
    renderIfDirty();
  }

  void handleResignKey() {
    // The macOS answer to "the user clicked somewhere else". There is no
    // pointer grab to lose, so this notification IS the dismissal.
    if (isPopup_ && onDismissedOutside) onDismissedOutside();
  }

private:
  bool openInternal(void* parent, Component& content, int width, int height, int screenX,
                    int screenY, bool popup) {
    close();
    if (!isAvailable()) return false;
    isPopup_ = popup;

    Class viewClass = registeredViewClass();
    if (!viewClass) return false;

    view_ = cocoa::msg<id>((id) viewClass, sel_registerName("alloc"));
    if (!view_) return false;
    const cocoa::CGRect2 frame{{0.0, 0.0}, {(double) width, (double) height}};
    using InitFrame = id (*)(id, SEL, cocoa::CGRect2);
    view_ = reinterpret_cast<InitFrame>(objc_msgSend)(view_, sel_registerName("initWithFrame:"),
                                                      frame);
    if (!view_) return false;
    object_setInstanceVariable(view_, "sonorePeer", this);

    content_ = &content;
    router_ = new MouseRouter(content);
    backing_.setContent(content_);
    // macOS is the odd one out, and getting it backwards is the easy mistake.
    //
    // On Windows and X11 every number the platform gives is a DEVICE pixel and
    // the logical size is derived. On macOS every number is a POINT -- a view's
    // frame, a mouse location, a dirty rectangle -- and the DEVICE size is
    // derived, by the window's backingScaleFactor. So `width` and `height` here
    // are already logical, and it is the bitmap that has to be multiplied up.
    //
    // The consequence worth stating: nothing on this platform divides a mouse
    // coordinate. A peer that copied the Win32 conversion would halve every
    // click on a Retina display.
    backing_.setScale(backingScale());
    backing_.setDeviceSize(Backing::toDevice((float) width, backing_.scale()),
                           Backing::toDevice((float) height, backing_.scale()));

    // Mouse-moved and mouse-exited only arrive through a tracking area. Without
    // one a knob never highlights on hover and the editor feels dead before it
    // is even touched.
    addTrackingArea(width, height);

    // Files can be dropped on us. Without this registration the drag methods
    // above are never called at all, however correctly they are written -- the
    // one line that makes the other four matter.
    registerForFiles();

    if (popup) {
      if (!makePopupWindow(screenX, screenY, width, height)) {
        close();
        return false;
      }
    } else if (parent) {
      cocoa::msg<void>((id) parent, sel_registerName("addSubview:"), view_);
      // Pinned, so a host resizing its window resizes the editor without a
      // round trip through set_size. 18 == NSViewWidthSizable | NSViewHeightSizable.
      cocoa::msg<void>(view_, sel_registerName("setAutoresizingMask:"), (unsigned long) 18);
    }

    refreshDisplays();
    startTimer();
    content_->repaint();
    render();
    return true;
  }

  bool makePopupWindow(int screenX, int screenY, int width, int height) {
    id windowClass = cocoa::cls("NSWindow");
    if (!windowClass) return false;
    id window = cocoa::msg<id>(windowClass, sel_registerName("alloc"));
    if (!window) return false;

    // Cocoa places windows from the BOTTOM left with y growing up, and a menu
    // is asked for in top-left desktop coordinates like every other platform.
    // Converting here keeps the flip in one place.
    int screenW = 0, screenH = 0;
    screenSizeStatic(&screenW, &screenH);
    const cocoa::CGRect2 rect{{(double) screenX, (double) (screenH - screenY - height)},
                              {(double) width, (double) height}};
    using InitWindow = id (*)(id, SEL, cocoa::CGRect2, unsigned long, unsigned long, BOOL);
    // styleMask 0 is NSWindowStyleMaskBorderless: no frame, no title bar, which
    // is what a menu is. backing 2 is NSBackingStoreBuffered.
    window = reinterpret_cast<InitWindow>(objc_msgSend)(
        window, sel_registerName("initWithContentRect:styleMask:backing:defer:"), rect, 0u, 2u,
        NO);
    if (!window) return false;

    cocoa::msg<void>(window, sel_registerName("setOpaque:"), (BOOL) NO);
    // OFF. An NSWindow made this way releases itself when closed by default,
    // and this object holds a pointer to it and to its content view; the two
    // ownership rules cannot both be right. Explicit is the one that can be
    // read. See close().
    cocoa::msg<void>(window, sel_registerName("setReleasedWhenClosed:"), (BOOL) NO);
    cocoa::msg<void>(window, sel_registerName("setContentView:"), view_);
    // 101 is NSPopUpMenuWindowLevel: above ordinary windows including the
    // host's, which is the difference between a menu and a rectangle drawn
    // inside the editor.
    cocoa::msg<void>(window, sel_registerName("setLevel:"), (long) 101);
    // A tooltip is ordered front WITHOUT becoming key. Becoming key is what
    // makes a menu dismiss when the user clicks elsewhere, and it is exactly
    // what a tooltip must not do -- it would take focus from the host every
    // time one appeared.
    if (grabsMouse_)
      cocoa::msg<void>(window, sel_registerName("makeKeyAndOrderFront:"), (id) nullptr);
    else
      cocoa::msg<void>(window, sel_registerName("orderFront:"), (id) nullptr);

    // There is no pointer grab on macOS, so THIS is the dismissal mechanism:
    // the window stops being key when the user clicks anywhere else.
    id centre = grabsMouse_ ? cocoa::msg<id>(cocoa::cls("NSNotificationCenter"),
                                             sel_registerName("defaultCenter"))
                            : (id) nullptr;
    if (centre) {
      id name = cocoa::msg<id>(cocoa::cls("NSString"), sel_registerName("stringWithUTF8String:"),
                               "NSWindowDidResignKeyNotification");
      cocoa::msg<void>(centre, sel_registerName("addObserver:selector:name:object:"), view_,
                       sel_registerName("sonoreWindowResignedKey:"), name, window);
    }
    window_ = window;
    return true;
  }

  void registerForFiles() {
    id type = cocoa::msg<id>(cocoa::cls("NSString"), sel_registerName("stringWithUTF8String:"),
                             "NSFilenamesPboardType");
    if (!type) return;
    id array = cocoa::msg<id>(cocoa::cls("NSArray"), sel_registerName("arrayWithObject:"), type);
    if (!array) return;
    cocoa::msg<void>(view_, sel_registerName("registerForDraggedTypes:"), array);
  }

  void addTrackingArea(int width, int height) {
    id areaClass = cocoa::cls("NSTrackingArea");
    if (!areaClass) return;
    id area = cocoa::msg<id>(areaClass, sel_registerName("alloc"));
    if (!area) return;
    const cocoa::CGRect2 rect{{0.0, 0.0}, {(double) width, (double) height}};
    // NSTrackingMouseEnteredAndExited(1) | NSTrackingMouseMoved(2)
    //   | NSTrackingActiveAlways(0x80) | NSTrackingInVisibleRect(0x200)
    // InVisibleRect is what keeps the area correct after a resize without
    // rebuilding it, which is the bug where hover stops working once a host
    // makes the editor bigger.
    using InitArea = id (*)(id, SEL, cocoa::CGRect2, unsigned long, id, id);
    area = reinterpret_cast<InitArea>(objc_msgSend)(
        area, sel_registerName("initWithRect:options:owner:userInfo:"), rect,
        (unsigned long) (1u | 2u | 0x80u | 0x200u), view_, (id) nullptr);
    if (!area) return;
    cocoa::msg<void>(view_, sel_registerName("addTrackingArea:"), area);
    cocoa::msg<void>(area, sel_registerName("release"));
  }

  void startTimer() {
    id timerClass = cocoa::cls("NSTimer");
    if (!timerClass) return;
    using Sched = id (*)(id, SEL, double, id, SEL, id, BOOL);
    timer_ = reinterpret_cast<Sched>(objc_msgSend)(
        timerClass, sel_registerName("scheduledTimerWithTimeInterval:target:selector:userInfo:"
                                     "repeats:"),
        0.033, view_, sel_registerName("sonoreTick:"), (id) nullptr, (BOOL) YES);
  }

  /**
   * Ask Cocoa for every screen.
   *
   * The one genuinely awkward part is the Y AXIS. Cocoa's desktop origin is the
   * BOTTOM left of the primary screen and y grows upward; everything in this
   * SDK is top-left and y-down. So each screen's rectangle is flipped about the
   * primary's height -- and a screen ABOVE the primary, which in Cocoa has a
   * large positive y, becomes a NEGATIVE y here. Getting that backwards puts
   * every menu on a second monitor at the wrong end of the desk.
   */
  static void refreshDisplays() {
    id screens = cocoa::msg<id>(cocoa::cls("NSScreen"), sel_registerName("screens"));
    if (!screens) return;
    const unsigned long count = cocoa::msg<unsigned long>(screens, sel_registerName("count"));
    if (count == 0) return;

    using FrameFn = cocoa::CGRect2 (*)(id, SEL);
    // The primary is screens[0] in Cocoa, and its height defines the flip.
    id first = cocoa::msg<id>(screens, sel_registerName("objectAtIndex:"), (unsigned long) 0);
    if (!first) return;
    const cocoa::CGRect2 primary =
        reinterpret_cast<FrameFn>(objc_msgSend)(first, sel_registerName("frame"));
    const double primaryTop = primary.origin.y + primary.size.height;

    std::vector<Display> found;
    for (unsigned long i = 0; i < count; ++i) {
      id screen = cocoa::msg<id>(screens, sel_registerName("objectAtIndex:"), i);
      if (!screen) continue;
      const cocoa::CGRect2 frame =
          reinterpret_cast<FrameFn>(objc_msgSend)(screen, sel_registerName("frame"));
      const cocoa::CGRect2 visible =
          reinterpret_cast<FrameFn>(objc_msgSend)(screen, sel_registerName("visibleFrame"));

      Display d;
      d.bounds = Rect((float) frame.origin.x,
                      (float) (primaryTop - frame.origin.y - frame.size.height),
                      (float) frame.size.width, (float) frame.size.height);
      d.workArea = Rect((float) visible.origin.x,
                        (float) (primaryTop - visible.origin.y - visible.size.height),
                        (float) visible.size.width, (float) visible.size.height);
      d.isMain = i == 0;
      found.push_back(d);
    }
    if (!found.empty()) Displays::set(std::move(found));
  }

  static void screenSizeStatic(int* width, int* height) {
    *width = 0;
    *height = 0;
    id screen = cocoa::msg<id>(cocoa::cls("NSScreen"), sel_registerName("mainScreen"));
    if (!screen) return;
    using FrameFn = cocoa::CGRect2 (*)(id, SEL);
    const cocoa::CGRect2 sf =
        reinterpret_cast<FrameFn>(objc_msgSend)(screen, sel_registerName("frame"));
    *width = (int) sf.size.width;
    *height = (int) sf.size.height;
  }

  /** POINTS, which is what a view's frame is in. See open() for why this
   *  platform is the other way round from the other two. */
  void resized(int width, int height) {
    if (!content_ || width <= 0 || height <= 0) return;
    // Re-read every time: a window dragged from a Retina display to an external
    // monitor changes its backing scale without changing its point size, and
    // the only notice we get is being asked to lay out again.
    backing_.setScale(backingScale());
    const int deviceW = Backing::toDevice((float) width, backing_.scale());
    const int deviceH = Backing::toDevice((float) height, backing_.scale());
    if (backing_.deviceWidth() == deviceW && backing_.deviceHeight() == deviceH) return;
    backing_.setDeviceSize(deviceW, deviceH);
    render();
  }

  /** See the note on the Win32 peer's render(). */
  void render() {
    const std::vector<PixelRect>& changed = backing_.render();
    if (!view_ || changed.empty()) return;
    // setNeedsDisplayInRect: rather than the whole view, so Cocoa's own
    // dirty-rect union decides what drawRect: is asked for.
    //
    // Back into POINTS on the way out, because that is what the rectangle is
    // measured in -- and outward again after the division, so a device
    // rectangle that starts halfway through a point still invalidates the whole
    // one. Truncating here leaves a one-pixel line of the previous frame along
    // the top and left of everything that moves at a fractional scale.
    const double scale = (double) backing_.scale();
    for (const PixelRect& r : changed) {
      const double x0 = std::floor((double) r.x / scale);
      const double y0 = std::floor((double) r.y / scale);
      const double x1 = std::ceil((double) r.right() / scale);
      const double y1 = std::ceil((double) r.bottom() / scale);
      const cocoa::CGRect2 rect{{x0, y0}, {x1 - x0, y1 - y0}};
      using SetNeeds = void (*)(id, SEL, cocoa::CGRect2);
      reinterpret_cast<SetNeeds>(objc_msgSend)(view_, sel_registerName("setNeedsDisplayInRect:"),
                                               rect);
    }
  }

  /**
   * The window's backing scale, or 1 before there is a window.
   *
   * -[NSWindow backingScaleFactor] rather than the screen's: a window can be on
   * a Retina display and be told to render at 1x -- a host capturing the view
   * into an image does exactly that -- and the window is the only thing that
   * knows.
   */
  double backingScale() const {
    if (window_) {
      using ScaleFn = double (*)(id, SEL);
      // Plain objc_msgSend, not the _fpret variant: on x86_64 a double comes
      // back in xmm0 and _fpret is only for long double, and on arm64 there is
      // no _fpret at all. Reaching for it here would be a link error on Apple
      // silicon and a wrong register on Intel.
      const double s = reinterpret_cast<ScaleFn>(objc_msgSend)(
          window_, sel_registerName("backingScaleFactor"));
      if (s > 0.0) return s;
    }
    if (view_) {
      // A view hosted by the plugin's host has no NSWindow of ours; ask it to
      // convert a unit rectangle to backing coordinates, which is the same
      // answer without needing to reach the window.
      using ConvertFn = cocoa::CGRect2 (*)(id, SEL, cocoa::CGRect2);
      const cocoa::CGRect2 unit{{0.0, 0.0}, {1.0, 1.0}};
      const cocoa::CGRect2 backing = reinterpret_cast<ConvertFn>(objc_msgSend)(
          view_, sel_registerName("convertRectToBacking:"), unit);
      if (backing.size.width > 0.0) return backing.size.width;
    }
    return 1.0;
  }

  static Class registeredViewClass();

  id view_ = nullptr;
  id window_ = nullptr;
  id timer_ = nullptr;
  Component* content_ = nullptr;
  MouseRouter* router_ = nullptr;
  Backing backing_;
  Point lastMouse_;
  MouseCursor currentCursor_ = MouseCursor::Default;
  bool isPopup_ = false;
  bool grabsMouse_ = true;
};

// ── The view's methods ─────────────────────────────────────────────────────
//
// Drawing and input on macOS both arrive as overridden methods, so a view that
// overrides nothing draws nothing and receives nothing. These are the C
// functions registered as those overrides.

/** Y-DOWN. Every other surface in this SDK is, and overriding this puts the
 *  conversion in one place instead of scattering `height - y` through five
 *  input handlers -- where forgetting one gives an editor whose clicks land
 *  correctly and whose drags go the wrong way. */
inline BOOL sonoreViewIsFlipped(id, SEL) { return YES; }

/** So the first click into an unfocused plugin window does something, rather
 *  than being swallowed to focus the window. In a DAW the plugin window is
 *  usually not the focused one, so without this every first click is lost. */
inline BOOL sonoreViewAcceptsFirstMouse(id, SEL, id) { return YES; }

inline void sonoreViewDrawRect(id self, SEL, cocoa::CGRect2) {
  NativeWindowCocoa* peer = peerOfView(self);
  if (!peer) return;
  id context = cocoa::msg<id>(cocoa::cls("NSGraphicsContext"), sel_registerName("currentContext"));
  if (!context) return;
  void* cg = cocoa::msg<void*>(context, sel_registerName("CGContext"));
  peer->drawInto(cg);
}

/** An event's location is in WINDOW coordinates and has to be converted into
 *  the view. Using it unconverted is the classic Cocoa mistake, and it shows up
 *  as an editor whose controls respond a title-bar's height from where they
 *  are. */
inline Point sonorePointOf(id self, id event) {
  using LocFn = cocoa::CGPoint2 (*)(id, SEL);
  const cocoa::CGPoint2 inWindow =
      reinterpret_cast<LocFn>(objc_msgSend)(event, sel_registerName("locationInWindow"));
  using ConvertFn = cocoa::CGPoint2 (*)(id, SEL, cocoa::CGPoint2, id);
  const cocoa::CGPoint2 inView = reinterpret_cast<ConvertFn>(objc_msgSend)(
      self, sel_registerName("convertPoint:fromView:"), inWindow, (id) nullptr);
  return {(float) inView.x, (float) inView.y};
}

/** Where a DRAG is, in the view's coordinates.
 *
 *  draggingLocation is in window space like an event's, and needs the same
 *  conversion -- using it unconverted drops a file a title-bar's height from
 *  where it was let go. */
inline Point sonoreDragPoint(id self, id sender) {
  if (!sender) return {0.0f, 0.0f};
  using LocFn = cocoa::CGPoint2 (*)(id, SEL);
  const cocoa::CGPoint2 inWindow =
      reinterpret_cast<LocFn>(objc_msgSend)(sender, sel_registerName("draggingLocation"));
  using ConvertFn = cocoa::CGPoint2 (*)(id, SEL, cocoa::CGPoint2, id);
  const cocoa::CGPoint2 inView = reinterpret_cast<ConvertFn>(objc_msgSend)(
      self, sel_registerName("convertPoint:fromView:"), inWindow, (id) nullptr);
  return {(float) inView.x, (float) inView.y};
}

/** NSEventModifierFlags: shift 1<<17, control 1<<18, option 1<<19. */
inline void sonoreModifiers(id event, bool* shift, bool* ctrl, bool* alt) {
  const unsigned long flags =
      cocoa::msg<unsigned long>(event, sel_registerName("modifierFlags"));
  *shift = (flags & (1ul << 17)) != 0;
  *ctrl = (flags & (1ul << 18)) != 0;
  *alt = (flags & (1ul << 19)) != 0;
}

inline void sonoreViewMouseDown(id self, SEL, id event) {
  NativeWindowCocoa* peer = peerOfView(self);
  if (!peer) return;
  bool shift = false, ctrl = false, alt = false;
  sonoreModifiers(event, &shift, &ctrl, &alt);
  // Cocoa counts clicks for us, which is the one input job it does that the
  // other two platforms leave to the toolkit.
  const long clicks = cocoa::msg<long>(event, sel_registerName("clickCount"));
  peer->handleMouseDown(sonorePointOf(self, event), clicks >= 2 ? 2 : 1, shift, ctrl, alt);
}

/**
 * The right button, and on the Mac also Control-click.
 *
 * Cocoa routes a Control-click to rightMouseDown: itself, so honouring one
 * selector honours both -- which matters because Control-click IS the right
 * click on a Mac with a one-button mouse, and a plugin where it did nothing
 * would be a plugin whose parameters cannot be automated by the usual gesture.
 *
 * Compiled on macOS since 2026-09-01.
 */
inline void sonoreViewRightMouseDown(id self, SEL, id event) {
  NativeWindowCocoa* peer = peerOfView(self);
  if (!peer) return;
  bool shift = false, ctrl = false, alt = false;
  sonoreModifiers(event, &shift, &ctrl, &alt);
  peer->handleMouseDown(sonorePointOf(self, event), 1, shift, ctrl, alt, /*rightButton=*/true);
}

inline void sonoreViewMouseUp(id self, SEL, id event) {
  NativeWindowCocoa* peer = peerOfView(self);
  if (!peer) return;
  bool shift = false, ctrl = false, alt = false;
  sonoreModifiers(event, &shift, &ctrl, &alt);
  peer->handleMouseUp(sonorePointOf(self, event), shift, ctrl, alt);
}

/** Dragged and moved are separate methods on macOS and both are needed. A view
 *  that handles only mouseMoved: loses every drag; one that handles only
 *  mouseDragged: never highlights on hover. */
inline void sonoreViewMouseMoved(id self, SEL, id event) {
  NativeWindowCocoa* peer = peerOfView(self);
  if (!peer) return;
  bool shift = false, ctrl = false, alt = false;
  sonoreModifiers(event, &shift, &ctrl, &alt);
  peer->handleMouseMove(sonorePointOf(self, event), shift, ctrl, alt);
}

inline void sonoreViewMouseExited(id self, SEL, id) {
  NativeWindowCocoa* peer = peerOfView(self);
  if (peer) peer->handleMouseExit();
}

inline void sonoreViewScrollWheel(id self, SEL, id event) {
  NativeWindowCocoa* peer = peerOfView(self);
  if (!peer) return;
  bool shift = false, ctrl = false, alt = false;
  sonoreModifiers(event, &shift, &ctrl, &alt);
  using DeltaFn = double (*)(id, SEL);
  const double dy =
      reinterpret_cast<DeltaFn>(objc_msgSend)(event, sel_registerName("scrollingDeltaY"));
  if (dy == 0.0) return;
  // Normalised to notches, because a trackpad reports continuous deltas many
  // times a second and a knob driven by the raw value would fly across its
  // whole range on one flick.
  peer->handleWheel(sonorePointOf(self, event), dy > 0.0 ? 1.0f : -1.0f, shift, ctrl, alt);
}

/** So the view can be first responder at all. A view that answers NO to this
 *  never receives keyDown:, which is the quiet way a text field ends up unable
 *  to be typed into with nothing in the code looking wrong. */
inline BOOL sonoreViewAcceptsFirstResponder(id, SEL) { return YES; }

/**
 * A key.
 *
 * Cocoa puts both halves on one event, in two properties.
 * charactersIgnoringModifiers is the KEY -- what the physical key means before
 * anything is held -- and characters is what the layout actually produced. A
 * named key is read from the first; text from the second.
 *
 * The named keys arrive as private-use code points (NSUpArrowFunctionKey and
 * friends, 0xF700 upward) rather than as separate constants, which is the one
 * genuinely odd thing about Cocoa's keyboard and the reason this switch is on
 * a character value rather than a key code.
 */
inline void sonoreViewKeyDown(id self, SEL, id event) {
  NativeWindowCocoa* peer = peerOfView(self);
  if (!peer) return;

  bool shift = false, ctrl = false, alt = false;
  sonoreModifiers(event, &shift, &ctrl, &alt);
  // Command counts as ctrl for this SDK, so one branch in a widget covers
  // Cmd-A on a Mac and Ctrl-A everywhere else. NSEventModifierFlagCommand is
  // 1<<20.
  const unsigned long flags = cocoa::msg<unsigned long>(event, sel_registerName("modifierFlags"));
  if ((flags & (1ul << 20)) != 0) ctrl = true;

  id ignoring = cocoa::msg<id>(event, sel_registerName("charactersIgnoringModifiers"));
  uint32_t raw = 0;
  if (ignoring && cocoa::msg<unsigned long>(ignoring, sel_registerName("length")) > 0)
    raw = (uint32_t) cocoa::msg<unsigned short>(ignoring, sel_registerName("characterAtIndex:"),
                                                (unsigned long) 0);

  KeyPress key;
  switch (raw) {
    case 0x7f: key.keyCode = KeyPress::Backspace; break; // Delete key, top right
    case 0x09: key.keyCode = KeyPress::Tab; break;
    case 0x0d:
    case 0x03: key.keyCode = KeyPress::Return; break; // Return and Enter
    case 0x1b: key.keyCode = KeyPress::Escape; break;
    case 0xf728: key.keyCode = KeyPress::Delete; break; // NSDeleteFunctionKey
    case 0xf702: key.keyCode = KeyPress::Left; break;
    case 0xf703: key.keyCode = KeyPress::Right; break;
    case 0xf700: key.keyCode = KeyPress::Up; break;
    case 0xf701: key.keyCode = KeyPress::Down; break;
    case 0xf729: key.keyCode = KeyPress::Home; break;
    case 0xf72b: key.keyCode = KeyPress::End; break;
    case 0xf72c: key.keyCode = KeyPress::PageUp; break;
    case 0xf72d: key.keyCode = KeyPress::PageDown; break;
    default: break;
  }

  if (key.keyCode == KeyPress::None && (ctrl || alt)) {
    // ── A modifier combination ──
    //
    // -[NSEvent characters] APPLIES Control: Ctrl+A comes back as 0x01, the
    // ASCII control code, not 'a'. Same as WM_CHAR on Windows and
    // XLookupString on X11, and the same consequence on all three -- every
    // shortcut dead, because a widget compares against 'a' and nothing ever
    // delivers it.
    //
    // charactersIgnoringModifiers is already in hand above, for the named-key
    // switch, and is exactly the right answer here: the key the user pressed,
    // with their layout applied and the modifier not folded in.
    //
    // Lowercased for letters, so one comparison serves and shiftDown carries
    // the rest -- the rule the other two peers follow, so a shortcut table
    // written once behaves the same on all three.
    if (raw >= 0x20 && raw <= 0x7e)
      key.character = (raw >= 'A' && raw <= 'Z') ? (raw - 'A' + 'a') : raw;
  } else if (key.keyCode == KeyPress::None) {
    // No modifier: the LAYOUT's answer, not the raw key. This is what makes an
    // accented character on a French keyboard arrive as itself, and taking the
    // unmodified key here instead would give an American keyboard to everyone.
    id characters = cocoa::msg<id>(event, sel_registerName("characters"));
    if (characters && cocoa::msg<unsigned long>(characters, sel_registerName("length")) > 0) {
      const uint32_t unit = (uint32_t) cocoa::msg<unsigned short>(
          characters, sel_registerName("characterAtIndex:"), (unsigned long) 0);
      // NSString is UTF-16, so anything past the basic plane arrives as a
      // surrogate pair. Combining them keeps an emoji in a preset name one
      // character rather than two broken halves.
      if (unit >= 0xd800 && unit <= 0xdbff &&
          cocoa::msg<unsigned long>(characters, sel_registerName("length")) >= 2) {
        const uint32_t low = (uint32_t) cocoa::msg<unsigned short>(
            characters, sel_registerName("characterAtIndex:"), (unsigned long) 1);
        key.character = 0x10000u + ((unit - 0xd800u) << 10) + (low - 0xdc00u);
      } else {
        key.character = unit;
      }
    }
  }

  key.shiftDown = shift;
  key.ctrlDown = ctrl;
  key.altDown = alt;
  if (key.keyCode == KeyPress::None && key.character == 0) return;
  peer->handleKey(key);
}

/**
 * The paths on a dragging pasteboard.
 *
 * NSFilenamesPboardType is the old type and NSPasteboardTypeFileURL is the
 * current one; both are still sent depending on what dragged the files. Reading
 * the old one first is deliberate -- it gives an array of plain paths, where the
 * new one gives URLs that each need converting.
 */
inline std::vector<std::string> sonoreDraggedFiles(id sender) {
  std::vector<std::string> out;
  if (!sender) return out;
  id board = cocoa::msg<id>(sender, sel_registerName("draggingPasteboard"));
  if (!board) return out;

  id type = cocoa::msg<id>(cocoa::cls("NSString"), sel_registerName("stringWithUTF8String:"),
                           "NSFilenamesPboardType");
  id list = cocoa::msg<id>(board, sel_registerName("propertyListForType:"), type);
  if (!list) return out;

  const unsigned long count = cocoa::msg<unsigned long>(list, sel_registerName("count"));
  for (unsigned long i = 0; i < count; ++i) {
    id path = cocoa::msg<id>(list, sel_registerName("objectAtIndex:"), i);
    if (!path) continue;
    const char* utf8 = cocoa::msg<const char*>(path, sel_registerName("UTF8String"));
    if (utf8) out.push_back(std::string(utf8));
  }
  return out;
}

/** NSDragOperationCopy(1) if anything will take them, NSDragOperationNone(0)
 *  otherwise. Cocoa asks on entry and on every move, like XDND and unlike
 *  WM_DROPFILES -- so this peer can light a target on hover. */
inline unsigned long sonoreViewDraggingEntered(id self, SEL, id sender) {
  NativeWindowCocoa* peer = peerOfView(self);
  if (!peer) return 0;
  return peer->handleFileDrag(sonoreDraggedFiles(sender), sonoreDragPoint(self, sender)) ? 1ul
                                                                                        : 0ul;
}

inline unsigned long sonoreViewDraggingUpdated(id self, SEL, id sender) {
  return sonoreViewDraggingEntered(self, nullptr, sender);
}

inline void sonoreViewDraggingExited(id self, SEL, id) {
  if (NativeWindowCocoa* peer = peerOfView(self)) peer->handleFileDragExit();
}

inline BOOL sonoreViewPerformDrag(id self, SEL, id sender) {
  NativeWindowCocoa* peer = peerOfView(self);
  if (!peer) return NO;
  return peer->handleFilesDropped(sonoreDraggedFiles(sender), sonoreDragPoint(self, sender)) ? YES
                                                                                             : NO;
}

inline void sonoreViewTick(id self, SEL, id) {
  NativeWindowCocoa* peer = peerOfView(self);
  if (peer) peer->handleTimer();
}

inline void sonoreViewResignedKey(id self, SEL, id) {
  NativeWindowCocoa* peer = peerOfView(self);
  if (peer) peer->handleResignKey();
}

inline Class NativeWindowCocoa::registeredViewClass() {
  static Class klass = [] {
    // A name of our own, and reused if some other copy of this SDK in the same
    // process registered it first. Two plugins in one host share an
    // Objective-C runtime, and allocateClassPair on a name that already exists
    // returns null rather than replacing it.
    // Unique to this binary -- see cocoa::uniqueClassName for why a fixed
    // name is a crash waiting for a second plugin.
    char name[96];
    std::snprintf(name, sizeof(name), "SonoreNativeView_%llx",
                  (unsigned long long) (uintptr_t) (void*) &sonoreViewIsFlipped);
    if (Class existing = objc_getClass(name)) return existing;
    Class c = objc_allocateClassPair((Class) cocoa::cls("NSView"), name, 0);
    if (!c) return (Class) objc_getClass(name);

    class_addIvar(c, "sonorePeer", sizeof(void*), 3, "^v");
    class_addMethod(c, sel_registerName("isFlipped"), (IMP) sonoreViewIsFlipped, "c@:");
    class_addMethod(c, sel_registerName("acceptsFirstMouse:"), (IMP) sonoreViewAcceptsFirstMouse,
                    "c@:@");
    class_addMethod(c, sel_registerName("drawRect:"), (IMP) sonoreViewDrawRect, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    class_addMethod(c, sel_registerName("mouseDown:"), (IMP) sonoreViewMouseDown, "v@:@");
    class_addMethod(c, sel_registerName("mouseUp:"), (IMP) sonoreViewMouseUp, "v@:@");
    class_addMethod(c, sel_registerName("rightMouseDown:"), (IMP) sonoreViewRightMouseDown,
                    "v@:@");
    // The same implementation for both: a drag and a move are one thing to a
    // MouseRouter, which already knows whether a button is down.
    class_addMethod(c, sel_registerName("mouseDragged:"), (IMP) sonoreViewMouseMoved, "v@:@");
    class_addMethod(c, sel_registerName("mouseMoved:"), (IMP) sonoreViewMouseMoved, "v@:@");
    class_addMethod(c, sel_registerName("mouseExited:"), (IMP) sonoreViewMouseExited, "v@:@");
    class_addMethod(c, sel_registerName("scrollWheel:"), (IMP) sonoreViewScrollWheel, "v@:@");
    class_addMethod(c, sel_registerName("acceptsFirstResponder"),
                    (IMP) sonoreViewAcceptsFirstResponder, "c@:");
    class_addMethod(c, sel_registerName("keyDown:"), (IMP) sonoreViewKeyDown, "v@:@");
    class_addMethod(c, sel_registerName("draggingEntered:"), (IMP) sonoreViewDraggingEntered,
                    "L@:@");
    class_addMethod(c, sel_registerName("draggingUpdated:"), (IMP) sonoreViewDraggingUpdated,
                    "L@:@");
    class_addMethod(c, sel_registerName("draggingExited:"), (IMP) sonoreViewDraggingExited,
                    "v@:@");
    class_addMethod(c, sel_registerName("performDragOperation:"), (IMP) sonoreViewPerformDrag,
                    "c@:@");
    class_addMethod(c, sel_registerName("sonoreTick:"), (IMP) sonoreViewTick, "v@:@");
    class_addMethod(c, sel_registerName("sonoreWindowResignedKey:"), (IMP) sonoreViewResignedKey,
                    "v@:@");
    objc_registerClassPair(c);
    return c;
  }();
  return klass;
}

} // namespace gfx
} // namespace sonore

#endif // __APPLE__
