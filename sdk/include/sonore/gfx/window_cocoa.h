// SPDX-License-Identifier: Apache-2.0
//
// The Cocoa window, so the native UI runs on the third platform.
//
// ── Written, compiled, never run ────────────────────────────────────────────
//
// Say it plainly at the top rather than let a green build imply more than it
// earned: no machine in this project has executed a line of this file. macOS CI
// is the only one that could and it has not started a job since the account's
// spending limit was reached. What HAS happened is that a compiler has read it,
// against the shim in tests/au_shim -- so every name exists, every call has the
// right argument count, and the file is internally consistent. That is a real
// class of error caught, and it is not the same as working.
//
// The Win32 and X11 peers are checked against real windows with real events.
// This one is not, and the feature map says so.
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
//
// ── Screen readers and input methods (2026-10-14, compiled, not run) ────────
//
// The view is also the way in for VoiceOver and for input methods, and both
// are as unrun as the rest of this file:
//
//  - Accessibility: the view is an AXGroup whose children are the elements of
//    gfx/accessibility_mac.h, the NSAccessibility bridge over the same model
//    the Windows UIA bridge reads. Retaken and announced on this view's clock.
//
//  - Input methods: the view adopts NSTextInputClient. When a text field has
//    the focus, keyDown: goes through interpretKeyEvents:, so the system's
//    input method sees the key first and answers with insertText: (typed or
//    committed text), setMarkedText: (a composition, drawn by the field, see
//    gfx/text_input.h) or doCommandBySelector: (an editing key, delivered as
//    the KeyPress it always was). Every range arithmetic step lives in
//    TextInputSession and is tested on Windows; what is here is the NSString
//    and NSRange at the edge. Without a text field focused, keys take the
//    path they always took -- a knob has nothing to compose into, and an input
//    method that ate its keys would swallow the host's shortcuts.
//
// tests/mac_input_selftest.cpp is what a Mac developer runs to see what both
// expose; until somebody has, treat both as written and not working.
#pragma once

#if defined(__APPLE__) || defined(SONORE_APPLE_SYNTAX_CHECK)

#include <dlfcn.h>
#include <objc/message.h>
#include <objc/objc.h>
#include <objc/runtime.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <algorithm>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "accessibility_mac.h"
#include "backing.h"
#include "component.h"
#include "displays.h"
#include "graphics.h"
#include "mouse_input.h"
#include "text_drag.h"
#include "text_input.h"

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

/** A message whose answer is a struct LARGER than 16 bytes -- an NSRect.
 *
 *  On x86_64 such a struct comes back through a hidden pointer, and the
 *  message has to go through objc_msgSend_stret, which expects that pointer
 *  first. Sent through plain objc_msgSend, the compiler still passes it, in
 *  the register objc_msgSend reads `self` from: every frame and rectangle
 *  conversion on an Intel Mac messaged a stack address. arm64 has no _stret
 *  and returns structs through objc_msgSend itself. (An NSPoint, two doubles,
 *  comes back in registers on both and needs none of this.) */
template <typename Ret, typename... Args>
inline Ret msgRect(id target, SEL selector, Args... args) {
  using Fn = Ret (*)(id, SEL, Args...);
#if defined(__x86_64__)
  return reinterpret_cast<Fn>(objc_msgSend_stret)(target, selector, args...);
#else
  return reinterpret_cast<Fn>(objc_msgSend)(target, selector, args...);
#endif
}

/** Add a scheduled timer to the run loop's COMMON modes as well, so it keeps
 *  firing while a host runs the loop in a modal or event-tracking mode.
 *  NSRunLoopCommonModes is the string "kCFRunLoopCommonModes"; modes compare
 *  by value, so an NSString of it is the constant. */
inline void keepTickingInEveryMode(id timer) {
  if (!timer) return;
  id loop = msg<id>(cls("NSRunLoop"), sel_registerName("currentRunLoop"));
  id common = msg<id>(cls("NSString"), sel_registerName("stringWithUTF8String:"),
                      "kCFRunLoopCommonModes");
  if (loop && common) msg<void>(loop, sel_registerName("addTimer:forMode:"), timer, common);
}

struct CGPoint2 { double x, y; };
struct CGSize2 { double width, height; };
struct CGRect2 { CGPoint2 origin; CGSize2 size; };

/** NSRange: two NSUIntegers, which on every 64-bit Mac are unsigned long --
 *  sixteen bytes, passed and returned in two registers on both x86_64 and
 *  arm64, so plain objc_msgSend and plain C functions carry it. */
struct NSRange2 { unsigned long location, length; };

/** NSNotFound is NSIntegerMax: the largest SIGNED long, as an NSUInteger. */
constexpr unsigned long kNSNotFound = ~0ul >> 1;

inline TextRange16 rangeFromNS(NSRange2 r) {
  if (r.location >= kNSNotFound) return TextRange16{};
  return TextRange16{(size_t) r.location, (size_t) r.length};
}

inline NSRange2 rangeToNS(TextRange16 r) {
  if (r.isNotFound()) return NSRange2{kNSNotFound, 0};
  return NSRange2{(unsigned long) r.location, (unsigned long) r.length};
}

/** An NSString or NSAttributedString as UTF-8 -- insertText: and
 *  setMarkedText: are documented to take either. */
inline std::string textOf(id value) {
  if (!value) return {};
  if (msg<BOOL>(value, sel_registerName("isKindOfClass:"), cls("NSAttributedString")))
    value = msg<id>(value, sel_registerName("string"));
  if (!value) return {};
  const char* utf8 = msg<const char*>(value, sel_registerName("UTF8String"));
  return utf8 ? std::string(utf8) : std::string();
}

/** Milliseconds on a steady clock, for the accessibility bridge's rate
 *  limits. Never 0 in practice; the bridge reads 0 as "no clock". */
inline unsigned long long nowMs() {
  return (unsigned long long) std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
             .count() + 1ull;
}

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
  void (*CGContextSaveGState)(void*) = nullptr;
  void (*CGContextRestoreGState)(void*) = nullptr;
  void (*CGContextTranslateCTM)(void*, double, double) = nullptr;
  void (*CGContextScaleCTM)(void*, double, double) = nullptr;
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
    bind("CGContextSaveGState", &a.CGContextSaveGState);
    bind("CGContextRestoreGState", &a.CGContextRestoreGState);
    bind("CGContextTranslateCTM", &a.CGContextTranslateCTM);
    bind("CGContextScaleCTM", &a.CGContextScaleCTM);
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
    // The accessibility bridge before anything it points at: its elements are
    // told they are destroyed and cut loose while the view and the tree are
    // still whole, and VoiceOver -- which may hold them for a while yet --
    // finds them answering as nothing rather than reaching freed memory.
    if (axRoot_) {
      axRoot_->detach();
      delete axRoot_;
      axRoot_ = nullptr;
    }
    keyEvent_ = nullptr;
    inputTarget_ = nullptr;
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
    textDrop_.reset();
    delete router_;
    router_ = nullptr;
    content_ = nullptr;
    std::vector<NativeWindowCocoa*>& peers = openPeers();
    peers.erase(std::remove(peers.begin(), peers.end(), this), peers.end());
    // The backing store too. It holds the same pointer and OUTLIVES the window
    // -- a peer is a member of NativeEditor and is reused across every open --
    // so leaving it set means the next paint reaches a component the editor has
    // already destroyed. On Win32 that was not theoretical: CreateWindowExW
    // dispatches WM_SIZE synchronously, so the second open() of a reused editor
    // painted through the first one's freed viewport before it had returned.
    backing_.setContent(nullptr);
    // And its pixels. This object outlives the view, so what it holds now it
    // holds until the plugin is unloaded; the next open() allocates them
    // again. Nothing draws from them past here: the view went above.
    backing_.release();
    isPopup_ = false;
  }

  void* handle() const { return (void*) view_; }
  bool isOpen() const { return view_ != nullptr; }

  /** The NSView an open peer shows `content` in, or null. How a component
   *  that embeds a native view (gfx/nsview_component.h) finds the view its
   *  tree is drawn into: a component knows its root and nothing about
   *  windows. Main thread, like every Cocoa call. */
  static void* viewShowing(const Component* content) {
    for (NativeWindowCocoa* peer : openPeers())
      if (peer->content_ == content && peer->view_) return (void*) peer->view_;
    return nullptr;
  }
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
    // The VIEW's rectangle on the desktop, not the window's: the window frame
    // includes its title bar, and an editor embedded in a host's window sits
    // somewhere inside it, under the host's own toolbars -- so every menu was
    // placed that far up and to the left of the control that opened it.
    const cocoa::CGRect2 bounds = cocoa::msgRect<cocoa::CGRect2>(view_, sel_registerName("bounds"));
    const cocoa::CGRect2 inWindow = cocoa::msgRect<cocoa::CGRect2>(
        view_, sel_registerName("convertRect:toView:"), bounds, (id) nullptr);
    const cocoa::CGRect2 onScreen = cocoa::msgRect<cocoa::CGRect2>(
        window, sel_registerName("convertRectToScreen:"), inWindow);
    // Cocoa's desktop origin is the BOTTOM left of the PRIMARY screen and y
    // grows upward; this SDK's is top-left. Flipped about the primary's top,
    // as refreshDisplays() flips the screen list and makePopupWindow() flips
    // back -- the window's own screen was the reference here, and on a second
    // monitor the two disagreed by the difference in their heights.
    *x = (int) onScreen.origin.x;
    *y = (int) (primaryScreenTop() - (onScreen.origin.y + onScreen.size.height));
  }

  /** The top of the primary screen (screens[0]) in Cocoa's y-up desktop
   *  space: the one line every flip between Cocoa and this SDK goes through.
   *  NOT mainScreen, which is whichever screen holds the key window. */
  static double primaryScreenTop() {
    id screens = cocoa::msg<id>(cocoa::cls("NSScreen"), sel_registerName("screens"));
    id first = screens && cocoa::msg<unsigned long>(screens, sel_registerName("count")) > 0
                   ? cocoa::msg<id>(screens, sel_registerName("objectAtIndex:"), (unsigned long) 0)
                   : (id) nullptr;
    if (!first) return 0.0;
    const cocoa::CGRect2 frame = cocoa::msgRect<cocoa::CGRect2>(first, sel_registerName("frame"));
    return frame.origin.y + frame.size.height;
  }

  void screenSize(int* width, int* height) const {
    *width = 0;
    *height = 0;
    id screen = cocoa::msg<id>(cocoa::cls("NSScreen"), sel_registerName("mainScreen"));
    if (!screen) return;
    const cocoa::CGRect2 sf =
        cocoa::msgRect<cocoa::CGRect2>(screen, sel_registerName("frame"));
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
      // Quartz draws an image y-UP: its first row lands at the rectangle's
      // top in an unflipped context, and at the BOTTOM in this one, which
      // isFlipped makes y-down. Drawn as it was, the whole editor came out
      // mirrored top to bottom while clicks, converted into the flipped view,
      // landed where the controls were meant to be -- on the wrong ones.
      // Unflipped for this one draw, so the bitmap's first row is the top.
      cg.CGContextSaveGState(cgContext);
      cg.CGContextTranslateCTM(cgContext, 0.0, rect.size.height);
      cg.CGContextScaleCTM(cgContext, 1.0, -1.0);
      cg.CGContextDrawImage(cgContext, rect, image);
      cg.CGContextRestoreGState(cgContext);
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

  /** Points of trackpad travel not yet worth a notch, carried between events. */
  double preciseScroll = 0.0;

  /** The view's frame or backing scale changed under it (POINTS). */
  void handleFrameChange(int width, int height) { resized(width, height); }

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
    if (textDrop_) textDrop_->dragExit();
    renderIfDirty();
  }

  bool handleFilesDropped(const std::vector<std::string>& files, Point p) {
    if (!router_) return false;
    const bool taken = router_->filesDropped(files, p);
    renderIfDirty();
    return taken;
  }

  /** Text dragged in from another application: see text_drag.h. */
  bool handleTextDrag(const std::string& text, Point p) {
    if (!textDrop_) return false;
    const bool accept = textDrop_->dragMove(text, p);
    renderIfDirty();
    return accept;
  }

  bool handleTextDropped(const std::string& text, Point p) {
    if (!textDrop_) return false;
    const bool taken = textDrop_->drop(text, p);
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
    syncTextInput();
    if (axRoot_) axRoot_->tick(cocoa::nowMs());
  }

  // ── Accessibility ────────────────────────────────────────────────────────

  /** What a reader calls the editor as a whole: the view's AXGroup label.
   *  The same call, and the same default, as the Win32 peer's. */
  void setAccessibleWindowName(std::string name) { accessibleWindowName_ = std::move(name); }

  /** The NSAccessibility bridge, or null: not open, or no
   *  NSAccessibilityElement in this process (see its unavailableReason). */
  nsax::MacAccessibilityRoot* accessibility() { return axRoot_; }

  // ── Input methods (NSTextInputClient) ────────────────────────────────────
  //
  // Called from the view's registered methods below. TextInputSession does
  // the work; these find the focused field and keep the bookkeeping of the
  // key being interpreted.

  /** Whether the view adopted NSTextInputClient. Without the protocol,
   *  -[NSView inputContext] is nil and no input method can compose here; the
   *  reason is the protocol missing from the runtime, and this says so. */
  static const char* inputMethodStatus() {
    registeredViewClass();
    return textInputProtocolAdopted() ? "NSTextInputClient adopted"
                                      : "NSTextInputClient protocol not found in the runtime: "
                                        "input methods cannot compose into this view";
  }

  TextInputTarget* textTarget() const { return focusedTextInputTarget(router_); }

  /** A key goes to the input method when a field that can be typed into has
   *  the focus -- unless Command or Control is held and nothing is being
   *  composed, which is a shortcut (Cmd-A, Cmd-C) and takes the path it
   *  always took. */
  bool routesKeyThroughInputMethod(bool commandOrControl) const {
    TextInputTarget* target = textTarget();
    if (!target || target->isInputReadOnly() || !textInputProtocolAdopted()) return false;
    return !commandOrControl || target->composition().isActive();
  }

  void beginInputKey(id event) {
    keyEvent_ = event;
    keyHandled_ = false;
  }

  /** Whether anything the input method sent back used the key. */
  bool endInputKey() {
    keyEvent_ = nullptr;
    renderIfDirty();
    syncTextInput();
    return keyHandled_;
  }

  id currentInputKeyEvent() const { return keyEvent_; }
  bool isDiscardingInput() const { return discardingInput_; }
  void noteInputHandled(bool handled) { keyHandled_ = keyHandled_ || handled; }

  void inputInsertText(const std::string& utf8, TextRange16 replacement) {
    if (discardingInput_) return;
    if (TextInputTarget* target = textTarget()) {
      noteInputHandled(TextInputSession::insertText(*target, utf8, replacement));
    } else {
      // The focus moved off the field while the input method was busy: the
      // text goes where keys go, a character at a time.
      for (uint32_t c : detail::textInputDecode(utf8)) {
        KeyPress key;
        key.character = c;
        noteInputHandled(handleKey(key));
      }
    }
    renderIfDirty();
  }

  void inputSetMarkedText(const std::string& utf8, TextRange16 selected, TextRange16 replacement) {
    if (discardingInput_) return;
    TextInputTarget* target = textTarget();
    if (!target) return;
    noteInputHandled(TextInputSession::setMarkedText(*target, utf8, selected, replacement));
    renderIfDirty();
  }

  void inputUnmarkText() {
    if (discardingInput_) return;
    if (TextInputTarget* target = textTarget()) TextInputSession::unmarkText(*target);
    renderIfDirty();
  }

  TextRange16 inputSelectedRange() const {
    TextInputTarget* target = textTarget();
    return target ? TextInputSession::selectedRange(*target) : TextRange16{};
  }

  TextRange16 inputMarkedRange() const {
    TextInputTarget* target = textTarget();
    return target ? TextInputSession::markedRange(*target) : TextRange16{};
  }

  bool inputHasMarkedText() const {
    TextInputTarget* target = textTarget();
    return target && target->composition().isActive();
  }

  std::string inputSubstring(TextRange16 range, TextRange16* actual) const {
    TextInputTarget* target = textTarget();
    if (!target) {
      if (actual) *actual = TextRange16{};
      return {};
    }
    return TextInputSession::substring(*target, range, actual);
  }

  /** firstRectForCharacterRange:, on the SCREEN, y up: from the field's own
   *  coordinates to the root's, into the flipped view, onto the screen. */
  cocoa::CGRect2 inputFirstRect(TextRange16 range, TextRange16* actual) const {
    cocoa::CGRect2 out{{0.0, 0.0}, {0.0, 0.0}};
    if (actual) *actual = TextRange16{};
    TextInputTarget* target = textTarget();
    Component* field = router_ ? router_->focused() : nullptr;
    Rect local, frame;
    if (!target || !field || !content_ || !TextInputSession::firstRect(*target, range, &local, actual) ||
        !nsax::viewOnScreen(view_, &frame))
      return out;
    const Point topLeft = field->localToRoot({local.x, local.y});
    const Rect inView(topLeft.x + content_->bounds().x, topLeft.y + content_->bounds().y, local.w, local.h);
    const Rect onScreen = detail::flippedViewRectToScreen(frame, inView);
    out.origin = {(double) onScreen.x, (double) onScreen.y};
    out.size = {(double) onScreen.w, (double) onScreen.h};
    return out;
  }

  /** characterIndexForPoint:, from a SCREEN point. NSNotFound off the field. */
  unsigned long inputCharacterIndex(cocoa::CGPoint2 onScreen) const {
    TextInputTarget* target = textTarget();
    Component* field = router_ ? router_->focused() : nullptr;
    Rect frame;
    if (!target || !field || !content_ || !nsax::viewOnScreen(view_, &frame)) return cocoa::kNSNotFound;
    const Point inView =
        detail::screenPointToFlippedView(frame, {(float) onScreen.x, (float) onScreen.y});
    const Point local = field->rootToLocal({inView.x - content_->bounds().x, inView.y - content_->bounds().y});
    if (!field->localBounds().contains(local)) return cocoa::kNSNotFound;
    return (unsigned long) TextInputSession::characterIndexAt(*target, local);
  }

  /**
   * A click while something is being composed. The input method gets it
   * first -- a click inside the composition may move its caret or pick a
   * clause -- and when it declines, the field commits what was composed and
   * the input method is told to let go, so it does not go on composing into
   * a caret that has just moved. True when the input method took the click.
   */
  bool consumeMouseForInput(id event) {
    TextInputTarget* target = textTarget();
    if (!target || !target->composition().isActive() || !view_) return false;
    id context = cocoa::msg<id>(view_, sel_registerName("inputContext"));
    if (context && cocoa::msg<BOOL>(context, sel_registerName("handleEvent:"), event)) return true;
    target->commitComposition();
    discardInputMethod(context);
    renderIfDirty();
    return false;
  }

  /**
   * Keep the input method in step with the focus and the caret.
   *
   * When the focus moves to another field (or off fields), whatever the input
   * method was composing belongs to the old one -- which committed it on
   * losing focus -- and the input method is told to discard its state, so it
   * does not finish the composition into the new field. When the caret moves,
   * it is told to ask for the caret's rectangle again, so its candidate
   * window follows.
   */
  void syncTextInput() {
    if (!view_ || !textInputProtocolAdopted()) return;
    TextInputTarget* target = textTarget();
    id context = cocoa::msg<id>(view_, sel_registerName("inputContext"));
    if (target != inputTarget_) {
      if (inputTarget_) discardInputMethod(context);
      inputTarget_ = target; // compared, never followed: it may be gone
    }
    Rect caret;
    if (target && focusedCaretInRoot(router_, &caret) &&
        (caret.x != lastCaret_.x || caret.y != lastCaret_.y || caret.h != lastCaret_.h)) {
      lastCaret_ = caret;
      if (context) cocoa::msg<void>(context, sel_registerName("invalidateCharacterCoordinates"));
    }
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
    openPeers().push_back(this);
    router_ = new MouseRouter(content);
    textDrop_.reset(new TextDragDispatcher(content));
    backing_.setContent(content_);
    // The accessibility bridge, with the view: a reader already running asks
    // for the view's children as soon as it is in a window. Absent only where
    // NSAccessibilityElement is (before 10.10), and then said once rather
    // than left silent.
    if (nsax::MacAccessibilityRoot::isAvailable()) {
      axRoot_ = new nsax::MacAccessibilityRoot(view_, content_, router_, accessibleWindowName_);
      axRoot_->onChanged = [this] { renderIfDirty(); };
    } else {
      static bool said = false;
      if (!said) {
        said = true;
        std::fprintf(stderr, "[sonore] no screen reader bridge: %s\n",
                     nsax::MacAccessibilityRoot::unavailableReason());
      }
    }
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
    backing_.setScale((float) backingScale());
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
      // ...and told when it happens. Autoresizing moves the view's FRAME;
      // nothing rebuilt the bitmap or laid the content out again, so a host
      // that resized its window -- an AU above all, which never calls set_size
      // -- got the editor drawn at its old size in one corner.
      id centre = cocoa::msg<id>(cocoa::cls("NSNotificationCenter"),
                                 sel_registerName("defaultCenter"));
      id name = cocoa::msg<id>(cocoa::cls("NSString"), sel_registerName("stringWithUTF8String:"),
                               "NSViewFrameDidChangeNotification");
      if (centre && name)
        cocoa::msg<void>(centre, sel_registerName("addObserver:selector:name:object:"), view_,
                         sel_registerName("sonoreViewFrameChanged:"), name, view_);
      // In the host's window now, so its backing scale is finally known: the
      // one read at the top of open() came before there was a window to ask.
      resized(width, height);
    }

    refreshDisplays();
    startTimer();
    content_->repaint();
    render();
    return true;
  }

  bool makePopupWindow(int screenX, int screenY, int width, int height) {
    // A MENU is a window that can become key, which a borderless NSWindow
    // cannot: canBecomeKeyWindow answers NO for a window with no title bar,
    // so makeKeyAndOrderFront: below left it unkeyed, it never resigned key,
    // and a click elsewhere never dismissed it. A tooltip must not take key,
    // and keeps the plain class.
    id windowClass = grabsMouse_ ? (id) registeredMenuWindowClass() : cocoa::cls("NSWindow");
    if (!windowClass) return false;
    id window = cocoa::msg<id>(windowClass, sel_registerName("alloc"));
    if (!window) return false;

    // Cocoa places windows from the BOTTOM left with y growing up, and a menu
    // is asked for in top-left desktop coordinates like every other platform.
    // Converting here keeps the flip in one place.
    // About the PRIMARY screen, the same line desktopOrigin() and the screen
    // list flip about; mainScreen -- the screen with the key window -- put a
    // menu on another monitor at the wrong height.
    const double primaryTop = primaryScreenTop();
    const cocoa::CGRect2 rect{{(double) screenX, primaryTop - (double) screenY - (double) height},
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

  /** Files, and plain text (text_drag.h). */
  void registerForFiles() {
    id array = detail::cocoaDragTypes();
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
    // Scheduled timers run in the DEFAULT mode only, and a host in a modal
    // loop or tracking a mouse drag -- a menu open, a fader held, a modal
    // dialog -- runs the loop in another: the editor froze until it let go.
    // The common modes include them all.
    cocoa::keepTickingInEveryMode(timer_);
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

    // The primary is screens[0] in Cocoa, and its height defines the flip.
    id first = cocoa::msg<id>(screens, sel_registerName("objectAtIndex:"), (unsigned long) 0);
    if (!first) return;
    const cocoa::CGRect2 primary =
        cocoa::msgRect<cocoa::CGRect2>(first, sel_registerName("frame"));
    const double primaryTop = primary.origin.y + primary.size.height;

    std::vector<Display> found;
    for (unsigned long i = 0; i < count; ++i) {
      id screen = cocoa::msg<id>(screens, sel_registerName("objectAtIndex:"), i);
      if (!screen) continue;
      const cocoa::CGRect2 frame =
          cocoa::msgRect<cocoa::CGRect2>(screen, sel_registerName("frame"));
      const cocoa::CGRect2 visible =
          cocoa::msgRect<cocoa::CGRect2>(screen, sel_registerName("visibleFrame"));

      Display d;
      d.bounds = Rect((float) frame.origin.x,
                      (float) (primaryTop - frame.origin.y - frame.size.height),
                      (float) frame.size.width, (float) frame.size.height);
      d.workArea = Rect((float) visible.origin.x,
                        (float) (primaryTop - visible.origin.y - visible.size.height),
                        (float) visible.size.width, (float) visible.size.height);
      d.isMain = i == 0;
      // Each screen's backing pixels per point, read as backingScale() reads
      // the window's (plain objc_msgSend for a double). It was never set, so
      // scaleAt() said 1 on a Retina screen.
      using ScaleFn = double (*)(id, SEL);
      const double scale =
          reinterpret_cast<ScaleFn>(objc_msgSend)(screen, sel_registerName("backingScaleFactor"));
      if (scale > 0.0) d.scale = (float) scale;
      found.push_back(d);
    }
    if (!found.empty()) Displays::set(std::move(found));
  }

  /** POINTS, which is what a view's frame is in. See open() for why this
   *  platform is the other way round from the other two. */
  void resized(int width, int height) {
    if (!content_ || width <= 0 || height <= 0) return;
    // Re-read every time: a window dragged from a Retina display to an external
    // monitor changes its backing scale without changing its point size, and
    // the only notice we get is being asked to lay out again.
    backing_.setScale((float) backingScale());
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
      const cocoa::CGRect2 unit{{0.0, 0.0}, {1.0, 1.0}};
      const cocoa::CGRect2 backing = cocoa::msgRect<cocoa::CGRect2>(
          view_, sel_registerName("convertRectToBacking:"), unit);
      if (backing.size.width > 0.0) return backing.size.width;
    }
    return 1.0;
  }

  static Class registeredViewClass();
  static Class registeredMenuWindowClass();

  /** Set once, when the view class is registered. */
  static bool& textInputProtocolAdopted() {
    static bool adopted = false;
    return adopted;
  }

  /** Tell the input method to drop what it is composing, with every call it
   *  makes back into the view meanwhile ignored: discarding may make it
   *  commit, and the commit belongs to no field any more. */
  void discardInputMethod(id context) {
    if (!context) return;
    discardingInput_ = true;
    cocoa::msg<void>(context, sel_registerName("discardMarkedText"));
    discardingInput_ = false;
  }

  nsax::MacAccessibilityRoot* axRoot_ = nullptr;
  std::string accessibleWindowName_ = "Plugin editor";
  id keyEvent_ = nullptr;       // the keyDown: being interpreted, or null
  bool keyHandled_ = false;     // whether the input method's answers used it
  bool discardingInput_ = false;
  const TextInputTarget* inputTarget_ = nullptr; // the field last seen focused
  Rect lastCaret_;

  /** Every peer between open() and close(), for viewShowing(). */
  static std::vector<NativeWindowCocoa*>& openPeers() {
    static std::vector<NativeWindowCocoa*> peers;
    return peers;
  }

  id view_ = nullptr;
  id window_ = nullptr;
  id timer_ = nullptr;
  Component* content_ = nullptr;
  MouseRouter* router_ = nullptr;
  std::unique_ptr<TextDragDispatcher> textDrop_;
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
  // A tablet's pressure and tilt ride on ordinary mouse events (mouse_input.h).
  detail::ScopedPointerInput pen(peer->router(), detail::pointerInputFromNSEvent(event));
  bool shift = false, ctrl = false, alt = false;
  sonoreModifiers(event, &shift, &ctrl, &alt);
  // Mid-composition the input method sees the click first (see
  // consumeMouseForInput); when it takes it, the editor does not.
  if (peer->consumeMouseForInput(event)) return;
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
 * Compiled on macOS since 2026-09-01, and unrun like the rest of this file:
 * no macOS test opens a window, and there is no Mac in this loop to click one.
 */
inline void sonoreViewRightMouseDown(id self, SEL, id event) {
  NativeWindowCocoa* peer = peerOfView(self);
  if (!peer) return;
  // A tablet's pressure and tilt ride on ordinary mouse events (mouse_input.h).
  detail::ScopedPointerInput pen(peer->router(), detail::pointerInputFromNSEvent(event));
  bool shift = false, ctrl = false, alt = false;
  sonoreModifiers(event, &shift, &ctrl, &alt);
  peer->handleMouseDown(sonorePointOf(self, event), 1, shift, ctrl, alt, /*rightButton=*/true);
}

inline void sonoreViewMouseUp(id self, SEL, id event) {
  NativeWindowCocoa* peer = peerOfView(self);
  if (!peer) return;
  // A tablet's pressure and tilt ride on ordinary mouse events (mouse_input.h).
  detail::ScopedPointerInput pen(peer->router(), detail::pointerInputFromNSEvent(event));
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
  // A tablet's pressure and tilt ride on ordinary mouse events (mouse_input.h).
  detail::ScopedPointerInput pen(peer->router(), detail::pointerInputFromNSEvent(event));
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
  // whole range on one flick. But a notch PER EVENT was the same flight by
  // another route: a single flick, momentum included, is dozens of events.
  // A precise device (trackpad, Magic Mouse) reports points, so points are
  // accumulated and a notch goes out per kPointsPerNotch of travel; a wheel
  // mouse reports lines, one event per click, and keeps a notch per event.
  using PreciseFn = BOOL (*)(id, SEL);
  const bool precise = reinterpret_cast<PreciseFn>(objc_msgSend)(
                           event, sel_registerName("hasPreciseScrollingDeltas")) != NO;
  if (!precise) {
    peer->handleWheel(sonorePointOf(self, event), dy > 0.0 ? 1.0f : -1.0f, shift, ctrl, alt);
    return;
  }
  constexpr double kPointsPerNotch = 12.0;
  peer->preciseScroll += dy;
  while (peer->preciseScroll >= kPointsPerNotch) {
    peer->preciseScroll -= kPointsPerNotch;
    peer->handleWheel(sonorePointOf(self, event), 1.0f, shift, ctrl, alt);
  }
  while (peer->preciseScroll <= -kPointsPerNotch) {
    peer->preciseScroll += kPointsPerNotch;
    peer->handleWheel(sonorePointOf(self, event), -1.0f, shift, ctrl, alt);
  }
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
inline KeyPress sonoreKeyPressOf(id event) {
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
    default:
      // NSF1FunctionKey (0xF704) to NSF12FunctionKey (0xF70F), consecutive.
      if (raw >= 0xf704 && raw <= 0xf70f) key.keyCode = KeyPress::F1 + (int) (raw - 0xf704);
      break;
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
  return key;
}

/** A key nobody here used goes on up the responder chain -- to the host's
 *  window, where the spacebar is transport. Swallowed, every host shortcut
 *  stopped working the moment the editor was first responder. */
inline void sonorePassKeyOn(id self, id event) {
  id next = cocoa::msg<id>(self, sel_registerName("nextResponder"));
  if (next) cocoa::msg<void>(next, sel_registerName("keyDown:"), event);
}

inline void sonoreViewKeyDown(id self, SEL, id event) {
  NativeWindowCocoa* peer = peerOfView(self);
  if (!peer) return;

  // ── A text field has the focus: the input method sees the key first ──
  //
  // interpretKeyEvents: hands it to the system's input method, which answers
  // through the NSTextInputClient methods below: insertText: for text,
  // setMarkedText: while composing, doCommandBySelector: for an editing key.
  // Whatever none of them used goes on, as below.
  const unsigned long flags = cocoa::msg<unsigned long>(event, sel_registerName("modifierFlags"));
  const bool commandOrControl = (flags & ((1ul << 20) | (1ul << 18))) != 0;
  if (peer->routesKeyThroughInputMethod(commandOrControl)) {
    peer->beginInputKey(event);
    id events = cocoa::msg<id>(cocoa::cls("NSArray"), sel_registerName("arrayWithObject:"), event);
    if (events) cocoa::msg<void>(self, sel_registerName("interpretKeyEvents:"), events);
    // Found again: a key can close the editor, and the peer with it.
    NativeWindowCocoa* after = peerOfView(self);
    if (!after || !after->endInputKey()) sonorePassKeyOn(self, event);
    return;
  }

  const KeyPress key = sonoreKeyPressOf(event);
  const bool handled =
      !(key.keyCode == KeyPress::None && key.character == 0) && peer->handleKey(key);
  if (!handled) sonorePassKeyOn(self, event);
}

// ── NSTextInputClient ──────────────────────────────────────────────────────
//
// The protocol, as C functions on the view, each one converting at the edge
// and handing over to the peer, which hands over to TextInputSession. The
// signatures are Apple's: NSRange by value (two NSUIntegers), NSRect returned
// (through a hidden pointer on x86_64, which a C function returning the
// struct also uses), NSPoint by value.

inline void sonoreViewInsertText(id self, SEL, id text, cocoa::NSRange2 replacement) {
  if (NativeWindowCocoa* peer = peerOfView(self))
    peer->inputInsertText(cocoa::textOf(text), cocoa::rangeFromNS(replacement));
}

inline void sonoreViewSetMarkedText(id self, SEL, id text, cocoa::NSRange2 selected,
                                    cocoa::NSRange2 replacement) {
  if (NativeWindowCocoa* peer = peerOfView(self))
    peer->inputSetMarkedText(cocoa::textOf(text), cocoa::rangeFromNS(selected),
                             cocoa::rangeFromNS(replacement));
}

inline void sonoreViewUnmarkText(id self, SEL) {
  if (NativeWindowCocoa* peer = peerOfView(self)) peer->inputUnmarkText();
}

inline cocoa::NSRange2 sonoreViewSelectedRange(id self, SEL) {
  NativeWindowCocoa* peer = peerOfView(self);
  return cocoa::rangeToNS(peer ? peer->inputSelectedRange() : TextRange16{});
}

inline cocoa::NSRange2 sonoreViewMarkedRange(id self, SEL) {
  NativeWindowCocoa* peer = peerOfView(self);
  return cocoa::rangeToNS(peer ? peer->inputMarkedRange() : TextRange16{});
}

inline BOOL sonoreViewHasMarkedText(id self, SEL) {
  NativeWindowCocoa* peer = peerOfView(self);
  return peer && peer->inputHasMarkedText() ? YES : NO;
}

inline id sonoreViewAttributedSubstring(id self, SEL, cocoa::NSRange2 range, cocoa::NSRange2* actual) {
  NativeWindowCocoa* peer = peerOfView(self);
  if (!peer) return nullptr;
  TextRange16 got;
  const std::string text = peer->inputSubstring(cocoa::rangeFromNS(range), &got);
  if (actual) *actual = cocoa::rangeToNS(got);
  if (got.isNotFound()) return nullptr;
  id string = cocoa::msg<id>(cocoa::cls("NSString"), sel_registerName("stringWithUTF8String:"),
                             text.c_str());
  id made = cocoa::msg<id>(cocoa::cls("NSAttributedString"), sel_registerName("alloc"));
  made = made ? cocoa::msg<id>(made, sel_registerName("initWithString:"), string) : nullptr;
  return made ? cocoa::msg<id>(made, sel_registerName("autorelease")) : nullptr;
}

/** None: the composition is drawn with the field's own underline, and an
 *  input method's attributes (clause segments, colours) would be read by
 *  nobody. An empty list is the documented way to say so. */
inline id sonoreViewValidAttributes(id, SEL) {
  return cocoa::msg<id>(cocoa::cls("NSArray"), sel_registerName("array"));
}

inline cocoa::CGRect2 sonoreViewFirstRect(id self, SEL, cocoa::NSRange2 range, cocoa::NSRange2* actual) {
  NativeWindowCocoa* peer = peerOfView(self);
  TextRange16 got;
  const cocoa::CGRect2 rect =
      peer ? peer->inputFirstRect(cocoa::rangeFromNS(range), &got) : cocoa::CGRect2{{0.0, 0.0}, {0.0, 0.0}};
  if (actual) *actual = cocoa::rangeToNS(got);
  return rect;
}

inline unsigned long sonoreViewCharacterIndex(id self, SEL, cocoa::CGPoint2 point) {
  NativeWindowCocoa* peer = peerOfView(self);
  return peer ? peer->inputCharacterIndex(point) : cocoa::kNSNotFound;
}

/**
 * An editing command from the key bindings: deleteBackward:, insertNewline:,
 * moveLeft:, cancelOperation: and the rest.
 *
 * During a keyDown: the command IS that key, and the key is delivered as the
 * KeyPress it has always been -- so Return, Tab, Escape and the arrows behave
 * exactly as they did before keys went through the input method, and a key
 * the editor does not use still reaches the host. Outside one (an input
 * method or a menu sending a command on its own), the commonest commands are
 * named; the rest are ignored rather than guessed at.
 */
inline bool sonoreKeyPressForCommand(const char* name, KeyPress* key) {
  struct Entry {
    const char* name;
    int code;
    bool shift;
  };
  static const Entry table[] = {
      {"deleteBackward:", KeyPress::Backspace, false}, {"deleteForward:", KeyPress::Delete, false},
      {"insertNewline:", KeyPress::Return, false},     {"insertTab:", KeyPress::Tab, false},
      {"insertBacktab:", KeyPress::Tab, true},         {"cancelOperation:", KeyPress::Escape, false},
      {"moveLeft:", KeyPress::Left, false},            {"moveRight:", KeyPress::Right, false},
      {"moveUp:", KeyPress::Up, false},                {"moveDown:", KeyPress::Down, false},
      {"moveLeftAndModifySelection:", KeyPress::Left, true},
      {"moveRightAndModifySelection:", KeyPress::Right, true},
      {"moveToBeginningOfLine:", KeyPress::Home, false}, {"moveToEndOfLine:", KeyPress::End, false},
      {"moveToBeginningOfLineAndModifySelection:", KeyPress::Home, true},
      {"moveToEndOfLineAndModifySelection:", KeyPress::End, true},
      {"pageUp:", KeyPress::PageUp, false},            {"pageDown:", KeyPress::PageDown, false},
  };
  if (!name || !key) return false;
  for (const Entry& e : table)
    if (std::strcmp(e.name, name) == 0) {
      *key = KeyPress();
      key->keyCode = e.code;
      key->shiftDown = e.shift;
      return true;
    }
  return false;
}

inline void sonoreViewDoCommand(id self, SEL, SEL command) {
  NativeWindowCocoa* peer = peerOfView(self);
  if (!peer || peer->isDiscardingInput()) return;
  KeyPress key;
  if (id event = peer->currentInputKeyEvent()) key = sonoreKeyPressOf(event);
  else if (!sonoreKeyPressForCommand(sel_getName(command), &key)) return;
  if (key.keyCode == KeyPress::None && key.character == 0) return;
  peer->noteInputHandled(peer->handleKey(key));
}

// ── The view as an accessibility container ─────────────────────────────────
//
// An AXGroup named after the editor, whose children are the bridge's
// elements. Without these the view is a plain NSView: ignored by readers,
// with no children, and the editor inside it does not exist for VoiceOver.

inline id sonoreViewAxChildren(id self, SEL) {
  NativeWindowCocoa* peer = peerOfView(self);
  nsax::MacAccessibilityRoot* root = peer ? peer->accessibility() : nullptr;
  return root ? root->topLevelChildren(cocoa::nowMs()) : nsax::array({});
}

inline BOOL sonoreViewAxIsElement(id, SEL) { return YES; }

inline id sonoreViewAxRole(id, SEL) { return nsax::string("AXGroup"); }

inline id sonoreViewAxLabel(id self, SEL) {
  NativeWindowCocoa* peer = peerOfView(self);
  nsax::MacAccessibilityRoot* root = peer ? peer->accessibility() : nullptr;
  return root ? nsax::string(root->name()) : nullptr;
}

inline id sonoreViewAxHitTest(id self, SEL, nsax::AxPoint point) {
  NativeWindowCocoa* peer = peerOfView(self);
  nsax::MacAccessibilityRoot* root = peer ? peer->accessibility() : nullptr;
  return root ? root->hitTest(point, cocoa::nowMs()) : self;
}

inline id sonoreViewAxFocused(id self, SEL) {
  NativeWindowCocoa* peer = peerOfView(self);
  nsax::MacAccessibilityRoot* root = peer ? peer->accessibility() : nullptr;
  return root ? root->focusedElement(cocoa::nowMs()) : self;
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
  const std::vector<std::string> files = sonoreDraggedFiles(sender);
  const Point p = sonoreDragPoint(self, sender);
  // No files: perhaps text, for a TextDragAndDropTarget (text_drag.h).
  std::string text;
  if (files.empty() && detail::cocoaDraggedText(sender, &text))
    return peer->handleTextDrag(text, p) ? 1ul : 0ul;
  return peer->handleFileDrag(files, p) ? 1ul : 0ul;
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
  const std::vector<std::string> files = sonoreDraggedFiles(sender);
  const Point p = sonoreDragPoint(self, sender);
  std::string text;
  if (files.empty() && detail::cocoaDraggedText(sender, &text))
    return peer->handleTextDropped(text, p) ? YES : NO;
  return peer->handleFilesDropped(files, p) ? YES : NO;
}

/** Which end of the pen is down -- Cocoa says so only here. */
inline void sonoreViewTabletProximity(id, SEL, id event) {
  detail::noteCocoaTabletProximity(event);
}

inline void sonoreViewTick(id self, SEL, id) {
  NativeWindowCocoa* peer = peerOfView(self);
  if (peer) peer->handleTimer();
}

inline void sonoreViewResignedKey(id self, SEL, id) {
  NativeWindowCocoa* peer = peerOfView(self);
  if (peer) peer->handleResignKey();
}

/** The view's frame, in points, as its size now is. */
inline void sonoreViewFollowFrame(id self) {
  NativeWindowCocoa* peer = peerOfView(self);
  if (!peer) return;
  const cocoa::CGRect2 frame = cocoa::msgRect<cocoa::CGRect2>(self, sel_registerName("frame"));
  peer->handleFrameChange((int) (frame.size.width + 0.5), (int) (frame.size.height + 0.5));
}

/** NSViewFrameDidChangeNotification: the host resized the view (autoresizing,
 *  or setFrame: from an AU host). */
inline void sonoreViewFrameChanged(id self, SEL, id) { sonoreViewFollowFrame(self); }

/** The backing scale changed -- the window moved between a Retina display and
 *  one that is not. Nothing told the editor, which went on rendering at the
 *  old scale: blurred on the Retina side, twice the size on the other.
 *  NSView's own implementation does nothing, so there is no super to call. */
inline void sonoreViewBackingChanged(id self, SEL) { sonoreViewFollowFrame(self); }

/** canBecomeKeyWindow for the menu window class: YES, so a borderless menu
 *  can be key and resign it -- its only way to learn of a click elsewhere. */
inline BOOL sonoreMenuWindowCanBecomeKey(id, SEL) { return YES; }

inline Class NativeWindowCocoa::registeredMenuWindowClass() {
  static Class klass = [] {
    // Unique to this binary, for the reason registeredViewClass gives.
    char name[96];
    std::snprintf(name, sizeof(name), "SonoreMenuWindow_%llx",
                  (unsigned long long) (uintptr_t) (void*) &sonoreMenuWindowCanBecomeKey);
    if (Class existing = objc_getClass(name)) return existing;
    Class c = objc_allocateClassPair((Class) cocoa::cls("NSWindow"), name, 0);
    if (!c) return (Class) objc_getClass(name);
    class_addMethod(c, sel_registerName("canBecomeKeyWindow"), (IMP) sonoreMenuWindowCanBecomeKey,
                    "c@:");
    objc_registerClassPair(c);
    return c;
  }();
  return klass;
}

inline Class NativeWindowCocoa::registeredViewClass() {
  static Class klass = [] {
    // A name of our own, and reused if some other copy of this SDK in the same
    // process registered it first. Two plugins in one host share an
    // Objective-C runtime, and allocateClassPair on a name that already exists
    // returns null rather than replacing it.
    // Unique to this binary: two plugins built from this SDK share one
    // Objective-C runtime in a host, and a fixed name would hand the second
    // one the first one's class -- methods pointing into another module.
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
    class_addMethod(c, sel_registerName("tabletProximity:"), (IMP) sonoreViewTabletProximity,
                    "v@:@");
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
    class_addMethod(c, sel_registerName("sonoreViewFrameChanged:"), (IMP) sonoreViewFrameChanged,
                    "v@:@");
    class_addMethod(c, sel_registerName("viewDidChangeBackingProperties"),
                    (IMP) sonoreViewBackingChanged, "v@:");
    class_addMethod(c, sel_registerName("sonoreWindowResignedKey:"), (IMP) sonoreViewResignedKey,
                    "v@:@");

    // ── NSTextInputClient ──
    //
    // The protocol is ADOPTED, not only implemented: -[NSView inputContext]
    // returns nil for a view that does not conform to NSTextInputClient, and
    // with no input context interpretKeyEvents: never reaches an input
    // method. objc_getProtocol finds it once AppKit is loaded (AppKit's own
    // text views conform to it); if it does not, inputMethodStatus() says
    // so and keys keep their old path.
    if (Protocol* textInput = objc_getProtocol("NSTextInputClient"))
      textInputProtocolAdopted() = class_addProtocol(c, textInput) != NO;
    class_addMethod(c, sel_registerName("insertText:replacementRange:"), (IMP) sonoreViewInsertText,
                    "v@:@" SONORE_OBJC_RANGE);
    class_addMethod(c, sel_registerName("setMarkedText:selectedRange:replacementRange:"),
                    (IMP) sonoreViewSetMarkedText, "v@:@" SONORE_OBJC_RANGE SONORE_OBJC_RANGE);
    class_addMethod(c, sel_registerName("unmarkText"), (IMP) sonoreViewUnmarkText, "v@:");
    class_addMethod(c, sel_registerName("selectedRange"), (IMP) sonoreViewSelectedRange,
                    SONORE_OBJC_RANGE "@:");
    class_addMethod(c, sel_registerName("markedRange"), (IMP) sonoreViewMarkedRange,
                    SONORE_OBJC_RANGE "@:");
    class_addMethod(c, sel_registerName("hasMarkedText"), (IMP) sonoreViewHasMarkedText,
                    SONORE_OBJC_BOOL "@:");
    class_addMethod(c, sel_registerName("attributedSubstringForProposedRange:actualRange:"),
                    (IMP) sonoreViewAttributedSubstring, "@@:" SONORE_OBJC_RANGE "^" SONORE_OBJC_RANGE);
    class_addMethod(c, sel_registerName("validAttributesForMarkedText"),
                    (IMP) sonoreViewValidAttributes, "@@:");
    class_addMethod(c, sel_registerName("firstRectForCharacterRange:actualRange:"),
                    (IMP) sonoreViewFirstRect, SONORE_OBJC_RECT "@:" SONORE_OBJC_RANGE "^" SONORE_OBJC_RANGE);
    class_addMethod(c, sel_registerName("characterIndexForPoint:"), (IMP) sonoreViewCharacterIndex,
                    "Q@:" SONORE_OBJC_POINT);
    class_addMethod(c, sel_registerName("doCommandBySelector:"), (IMP) sonoreViewDoCommand, "v@::");

    // ── The view as the accessibility container ──
    class_addMethod(c, sel_registerName("accessibilityChildren"), (IMP) sonoreViewAxChildren, "@@:");
    class_addMethod(c, sel_registerName("isAccessibilityElement"), (IMP) sonoreViewAxIsElement,
                    SONORE_OBJC_BOOL "@:");
    class_addMethod(c, sel_registerName("accessibilityRole"), (IMP) sonoreViewAxRole, "@@:");
    class_addMethod(c, sel_registerName("accessibilityLabel"), (IMP) sonoreViewAxLabel, "@@:");
    class_addMethod(c, sel_registerName("accessibilityHitTest:"), (IMP) sonoreViewAxHitTest,
                    "@@:" SONORE_OBJC_POINT);
    class_addMethod(c, sel_registerName("accessibilityFocusedUIElement"), (IMP) sonoreViewAxFocused,
                    "@@:");
    objc_registerClassPair(c);
    return c;
  }();
  return klass;
}

} // namespace gfx
} // namespace sonore

#endif // __APPLE__
