// SPDX-License-Identifier: Apache-2.0
//
// The UIKit window, so the native UI runs on an iPhone and an iPad -- in a
// plugin's standalone app, and in an AUv3's editor inside GarageBand, AUM or
// Logic Pro for iPad.
//
// ── A UIView, registered at runtime ─────────────────────────────────────────
//
// As on the Mac (window_cocoa.h): no Objective-C source anywhere, so the view
// is a UIView subclass registered with the runtime, its methods C functions.
// It draws by being given an IMAGE: each frame that changed, the rasteriser's
// bitmap -- R G B A premultiplied, which is CoreGraphics' kCGImageAlpha-
// PremultipliedLast, so nothing is converted -- becomes a CGImage and the
// view's layer's contents. Core Animation scales nothing: the image is made
// at the screen's own scale (2 or 3 device pixels to a point).
//
// ── Touch ───────────────────────────────────────────────────────────────────
//
// touchesBegan/Moved/Ended/Cancelled arrive with a set of UITouch objects. The
// first finger of a gesture is the PRIMARY pointer and goes through the router
// as the mouse does; the others are touches of their own (MouseInputSources),
// each keeping its slot for as long as it is down. The Apple Pencil is a pen
// with pressure; a trackpad's or mouse's pointer on an iPad is a mouse.
//
// ── The clock and the popups ────────────────────────────────────────────────
//
// A CADisplayLink calls the view once a frame -- the screen's own refresh, 60
// or 120 Hz -- which is the editor's tick. A menu or tooltip is a second
// view over the window, inside a transparent SHIELD the size of the window:
// a touch on the shield and not on the menu dismisses a menu that grabs the
// pointer, as a click outside one does on a desktop.
//
// ── What it does not do yet ─────────────────────────────────────────────────
//
// The software keyboard: a text field takes a hardware keyboard's keys
// through the router, but nothing here asks iOS to show its own. VoiceOver:
// the Mac's accessibility bridge is AppKit's, and UIKit's is not written.
#pragma once

#include "../apple_platform.h"

#if defined(SONORE_IOS)

#include <dlfcn.h>
#include <objc/message.h>
#include <objc/objc.h>
#include <objc/runtime.h>
#include <CoreFoundation/CoreFoundation.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "backing.h"
#include "clipboard.h"
#include "component.h"
#include "mouse_input.h"

namespace sonore {
namespace gfx {
namespace uikit {

template <typename Ret, typename... Args>
inline Ret send(id target, const char* selector, Args... args) {
  using Fn = Ret (*)(id, SEL, Args...);
  return reinterpret_cast<Fn>(objc_msgSend)(target, sel_registerName(selector), args...);
}

/** A message answering a struct over 16 bytes (a CGRect): through the hidden
 *  pointer on x86_64 (an Intel Mac's simulator), plainly on arm64. */
template <typename Ret, typename... Args>
inline Ret sendRect(id target, const char* selector, Args... args) {
  using Fn = Ret (*)(id, SEL, Args...);
#if defined(__x86_64__)
  return reinterpret_cast<Fn>(objc_msgSend_stret)(target, sel_registerName(selector), args...);
#else
  return reinterpret_cast<Fn>(objc_msgSend)(target, sel_registerName(selector), args...);
#endif
}

inline id cls(const char* name) { return (id) objc_getClass(name); }

// CGPoint, CGSize and CGRect, restated: CoreGraphics' headers are not
// included, as on the Mac (window_cocoa.h).
struct CGPoint2 { double x, y; };
struct CGSize2 { double width, height; };
struct CGRect2 { CGPoint2 origin; CGSize2 size; };

/** The few CoreGraphics calls that turn pixels into an image. Linked by UIKit
 *  into every app; found by name, as the Mac's are. */
struct CoreGraphicsApi {
  bool ok = false;
  void* (*CGColorSpaceCreateDeviceRGB)() = nullptr;
  void (*CGColorSpaceRelease)(void*) = nullptr;
  void* (*CGDataProviderCreateWithCFData)(CFDataRef) = nullptr;
  void (*CGDataProviderRelease)(void*) = nullptr;
  void* (*CGImageCreate)(size_t, size_t, size_t, size_t, size_t, void*, uint32_t, void*, const double*, bool,
                         int) = nullptr;
  void (*CGImageRelease)(void*) = nullptr;
};

inline const CoreGraphicsApi& coreGraphics() {
  static CoreGraphicsApi api = [] {
    CoreGraphicsApi a;
    bool all = true;
    auto bind = [&](const char* name, void* slot) {
      void* sym = dlsym(RTLD_DEFAULT, name);
      if (!sym) all = false;
      std::memcpy(slot, &sym, sizeof(sym));
    };
    bind("CGColorSpaceCreateDeviceRGB", &a.CGColorSpaceCreateDeviceRGB);
    bind("CGColorSpaceRelease", &a.CGColorSpaceRelease);
    bind("CGDataProviderCreateWithCFData", &a.CGDataProviderCreateWithCFData);
    bind("CGDataProviderRelease", &a.CGDataProviderRelease);
    bind("CGImageCreate", &a.CGImageCreate);
    bind("CGImageRelease", &a.CGImageRelease);
    a.ok = all;
    return a;
  }();
  return api;
}

/** kCGImageAlphaPremultipliedLast with the default byte order: the
 *  rasteriser's own layout. */
constexpr uint32_t kAlphaPremultipliedLast = 1u;

inline double screenScale() {
  id screen = send<id>(cls("UIScreen"), "mainScreen");
  const double s = screen ? send<double>(screen, "scale") : 1.0;
  return s > 0.0 ? s : 1.0;
}

/** A runtime class name unique to this binary, as every Apple file here makes
 *  them: two plugins in one process must not share one. */
inline void uniqueName(const char* base, void* inThisImage, char* out, size_t cap) {
  std::snprintf(out, cap, "%s_%llx", base, (unsigned long long) (uintptr_t) inThisImage);
}

} // namespace uikit

class NativeWindowUIKit;

namespace uikitdetail {
inline NativeWindowUIKit* ownerOf(id view);
inline void touchesBegan(id self, SEL, id touches, id event);
inline void touchesMoved(id self, SEL, id touches, id event);
inline void touchesEnded(id self, SEL, id touches, id event);
inline void touchesCancelled(id self, SEL, id touches, id event);
inline void shieldTouched(id self, SEL, id touches, id event);
inline void displayTick(id self, SEL, id link);
inline Class viewClass();
inline Class shieldClass();
} // namespace uikitdetail

/**
 * The native window peer on iOS: a UIView the editor draws into. The same
 * surface as the other peers (native_editor.h).
 */
class NativeWindowUIKit {
public:
  NativeWindowUIKit() = default;
  ~NativeWindowUIKit() { close(); }
  NativeWindowUIKit(const NativeWindowUIKit&) = delete;
  NativeWindowUIKit& operator=(const NativeWindowUIKit&) = delete;

  /** Called once a frame, by the screen's display link. */
  std::function<void()> onTick;
  /** A touch landed outside this popup while it grabs the pointer. */
  std::function<void()> onDismissedOutside;

  /** Whether UIKit is in this process: true in any app and any extension. */
  static bool isAvailable() { return objc_getClass("UIView") != nullptr && uikit::coreGraphics().ok; }

  /** The editor's view, inside `parent` (a UIView) at its top left, `width` x
   *  `height` points -- this SDK's logical pixels are UIKit's points. */
  bool open(void* parent, Component& content, int width, int height) {
    return open(parent, content, width, height, 0, 0);
  }

  bool open(void* parent, Component& content, int width, int height, int screenX, int screenY) {
    const bool popup = isPopup_;
    const bool grab = grabsMouse_;
    const float popupScale = popupScale_;
    close();
    isPopup_ = popup;
    grabsMouse_ = grab;
    if (!isAvailable()) return false;
    const float scale = isPopup_ && popupScale > 0.0f ? popupScale : (float) uikit::screenScale();
    content_ = &content;
    router_ = new MouseRouter(content);
    backing_.setContent(content_);
    backing_.setScale(scale);
    logicalWidth_ = (float) width;
    logicalHeight_ = (float) height;
    backing_.setDeviceSize(Backing::toDevice(logicalWidth_, scale), Backing::toDevice(logicalHeight_, scale));

    view_ = makeView(uikit::CGRect2{{0.0, 0.0}, {(double) width, (double) height}});
    if (!view_) {
      close();
      return false;
    }
    id layer = uikit::send<id>(view_, "layer");
    if (layer) uikit::send<void>(layer, "setContentsScale:", (double) scale);
    // The layer shows the image at its own size: no stretching, ever.
    if (layer) uikit::send<void>(layer, "setContentsGravity:", stringConstant("kCAGravityTopLeft", "topLeft"));

    if (isPopup_) {
      // Over everything in the main editor's window, inside a shield the
      // window's size that dismisses on a touch elsewhere.
      id host = hostWindowView();
      if (!host) {
        close();
        return false;
      }
      const uikit::CGRect2 hostBounds = uikit::sendRect<uikit::CGRect2>(host, "bounds");
      shield_ = makeShield(hostBounds);
      // In points, as every Apple desktop coordinate here is (native_editor.h
      // places menus with a scale of one on Apple).
      setFrame(view_, uikit::CGRect2{{(double) screenX, (double) screenY}, {(double) width, (double) height}});
      uikit::send<void>(shield_, "addSubview:", view_);
      uikit::send<void>(host, "addSubview:", shield_);
    } else {
      id parentView = (id) parent;
      if (!parentView) {
        close();
        return false;
      }
      uikit::send<void>(parentView, "addSubview:", view_);
      mainWindow() = this;
      installClipboard();
    }
    startClock();
    open_ = true;
    render();
    return true;
  }

  bool openPopup(Component& content, int screenX, int screenY, int width, int height, bool grabMouse = true,
                 float scale = 0.0f) {
    isPopup_ = true;
    grabsMouse_ = grabMouse;
    popupScale_ = scale;
    const bool ok = open(nullptr, content, width, height, screenX, screenY);
    if (!ok) isPopup_ = false;
    return ok;
  }

  void close() {
    stopClock();
    if (view_) {
      object_setInstanceVariable(view_, "owner", nullptr);
      uikit::send<void>(view_, "removeFromSuperview");
      uikit::send<void>(view_, "release");
      view_ = nil;
    }
    if (shield_) {
      object_setInstanceVariable(shield_, "owner", nullptr);
      uikit::send<void>(shield_, "removeFromSuperview");
      uikit::send<void>(shield_, "release");
      shield_ = nil;
    }
    if (mainWindow() == this) mainWindow() = nullptr;
    open_ = false;
    delete router_;
    router_ = nullptr;
    content_ = nullptr;
    backing_.setContent(nullptr);
    backing_.release();
    isPopup_ = false;
    grabsMouse_ = false;
    popupScale_ = 0.0f;
    for (void*& t : slotTouches_) t = nullptr;
    primaryTouch_ = nullptr;
  }

  /** The UIView, as the platform handle. */
  void* handle() const { return (void*) view_; }
  bool isOpen() const { return open_; }
  bool isPopup() const { return isPopup_; }
  bool grabsPointer() const { return isPopup_ && grabsMouse_; }

  Bitmap& bitmap() { return backing_.bitmap(); }
  float scale() const { return backing_.scale(); }
  float logicalWidth() const { return logicalWidth_; }
  float logicalHeight() const { return logicalHeight_; }
  MouseRouter* router() { return router_; }
  Point lastMousePosition() const { return lastPointer_; }

  void setScale(float scale) {
    backing_.setScale(scale);
    if (view_) {
      id layer = uikit::send<id>(view_, "layer");
      if (layer) uikit::send<void>(layer, "setContentsScale:", (double) scale);
    }
    setSize(Backing::toDevice(logicalWidth_, scale), Backing::toDevice(logicalHeight_, scale));
  }

  void setLogicalSize(float width, float height) {
    logicalWidth_ = width;
    logicalHeight_ = height;
    setSize(Backing::toDevice(width, backing_.scale()), Backing::toDevice(height, backing_.scale()));
  }

  /** In device pixels: the view is sized in points to match. */
  void setSize(int width, int height) {
    if (!open_) return;
    backing_.setDeviceSize(width, height);
    const double s = backing_.scale() > 0.0f ? backing_.scale() : 1.0;
    uikit::CGRect2 frame = uikit::sendRect<uikit::CGRect2>(view_, "frame");
    frame.size = uikit::CGSize2{width / s, height / s};
    setFrame(view_, frame);
    render();
  }

  void setVisible(bool shouldBeVisible) {
    if (view_) uikit::send<void>(view_, "setHidden:", (BOOL) (shouldBeVisible ? NO : YES));
  }

  /** Where the view is in its window, in points: Apple's desktop units, as
   *  NativeWindowCocoa reports them. */
  void desktopOrigin(int* x, int* y) const {
    int ox = 0, oy = 0;
    if (view_) {
      using Convert = uikit::CGPoint2 (*)(id, SEL, uikit::CGPoint2, id);
      const uikit::CGPoint2 at = reinterpret_cast<Convert>(objc_msgSend)(
          view_, sel_registerName("convertPoint:toView:"), uikit::CGPoint2{0.0, 0.0}, (id) nil);
      ox = (int) std::lround(at.x);
      oy = (int) std::lround(at.y);
    }
    if (x) *x = ox;
    if (y) *y = oy;
  }

  /** The screen, in points. */
  void screenSize(int* width, int* height) const {
    id screen = uikit::send<id>(uikit::cls("UIScreen"), "mainScreen");
    const uikit::CGRect2 b = screen ? uikit::sendRect<uikit::CGRect2>(screen, "bounds") : uikit::CGRect2{};
    if (width) *width = (int) std::lround(b.size.width);
    if (height) *height = (int) std::lround(b.size.height);
  }

  int connectionFd() const { return -1; }
  /** UIKit delivers touches to the view: nothing to pump. */
  void processEvents() {}
  void setAccessibleWindowName(const std::string& name) { accessibleName_ = name; }

  /** UIPasteboard's general board: what every app copies to. */
  void setClipboardText(const std::string& utf8) { pasteboardSet(utf8); }
  bool getClipboardText(std::string* out) { return pasteboardGet(out); }

  bool renderIfDirty() {
    if (!open_ || !content_ || !content_->isDirty()) return false;
    render();
    return true;
  }

private:
  friend void uikitdetail::touchesBegan(id, SEL, id, id);
  friend void uikitdetail::touchesMoved(id, SEL, id, id);
  friend void uikitdetail::touchesEnded(id, SEL, id, id);
  friend void uikitdetail::touchesCancelled(id, SEL, id, id);
  friend void uikitdetail::shieldTouched(id, SEL, id, id);
  friend void uikitdetail::displayTick(id, SEL, id);

  /** The main editor's window, which a popup opens over. */
  static NativeWindowUIKit*& mainWindow() {
    static NativeWindowUIKit* window = nullptr;
    return window;
  }

  static id stringConstant(const char* symbol, const char* fallback) {
    id* p = (id*) dlsym(RTLD_DEFAULT, symbol);
    if (p && *p) return *p;
    return (id) CFAutorelease(CFStringCreateWithCString(nullptr, fallback, kCFStringEncodingUTF8));
  }

  static void setFrame(id view, uikit::CGRect2 frame) {
    using SetFrame = void (*)(id, SEL, uikit::CGRect2);
    reinterpret_cast<SetFrame>(objc_msgSend)(view, sel_registerName("setFrame:"), frame);
  }

  id makeView(uikit::CGRect2 frame) {
    Class c = uikitdetail::viewClass();
    if (!c) return nil;
    using InitFrame = id (*)(id, SEL, uikit::CGRect2);
    id view = reinterpret_cast<InitFrame>(objc_msgSend)(uikit::send<id>((id) c, "alloc"),
                                                         sel_registerName("initWithFrame:"), frame);
    if (!view) return nil;
    object_setInstanceVariable(view, "owner", this);
    uikit::send<void>(view, "setMultipleTouchEnabled:", (BOOL) YES);
    uikit::send<void>(view, "setOpaque:", (BOOL) YES);
    return view; // +1
  }

  id makeShield(uikit::CGRect2 frame) {
    Class c = uikitdetail::shieldClass();
    if (!c) return nil;
    using InitFrame = id (*)(id, SEL, uikit::CGRect2);
    id shield = reinterpret_cast<InitFrame>(objc_msgSend)(uikit::send<id>((id) c, "alloc"),
                                                           sel_registerName("initWithFrame:"), frame);
    if (!shield) return nil;
    object_setInstanceVariable(shield, "owner", this);
    uikit::send<void>(shield, "setOpaque:", (BOOL) NO);
    uikit::send<void>(shield, "setBackgroundColor:", uikit::send<id>(uikit::cls("UIColor"), "clearColor"));
    // A tooltip lets every touch through to what it describes.
    uikit::send<void>(shield, "setUserInteractionEnabled:", (BOOL) (grabsMouse_ ? YES : NO));
    return shield; // +1
  }

  /** The window the main editor is in, or its view when it is not in one. */
  static id hostWindowView() {
    NativeWindowUIKit* main = mainWindow();
    if (!main || !main->view_) return nil;
    id window = uikit::send<id>(main->view_, "window");
    return window ? window : main->view_;
  }

  void startClock() {
    id linkClass = uikit::cls("CADisplayLink");
    if (!linkClass || !view_) return;
    link_ = uikit::send<id>(linkClass, "displayLinkWithTarget:selector:", view_, sel_registerName("sonoreTick:"));
    if (!link_) return;
    uikit::send<id>(link_, "retain");
    id loop = uikit::send<id>(uikit::cls("NSRunLoop"), "mainRunLoop");
    uikit::send<void>(link_, "addToRunLoop:forMode:", loop, stringConstant("NSRunLoopCommonModes", "kCFRunLoopCommonModes"));
  }

  void stopClock() {
    if (!link_) return;
    uikit::send<void>(link_, "invalidate"); // and lets go of the view it targets
    uikit::send<void>(link_, "release");
    link_ = nil;
  }

  void tick() {
    if (onTick) onTick();
    renderIfDirty();
  }

  /** Paint what changed, and put the whole frame on the layer. */
  void render() {
    if (!open_ || !view_) return;
    const std::vector<PixelRect>& changed = backing_.render();
    if (changed.empty()) return;
    const uikit::CoreGraphicsApi& cg = uikit::coreGraphics();
    const Bitmap& b = backing_.bitmap();
    if (!cg.ok || b.isEmpty()) return;
    CFDataRef data = CFDataCreate(nullptr, b.data(), (CFIndex) b.sizeInBytes());
    if (!data) return;
    void* provider = cg.CGDataProviderCreateWithCFData(data);
    CFRelease(data);
    void* space = cg.CGColorSpaceCreateDeviceRGB();
    void* image = provider && space ? cg.CGImageCreate((size_t) b.width(), (size_t) b.height(), 8, 32,
                                                       (size_t) b.width() * 4, space, uikit::kAlphaPremultipliedLast,
                                                       provider, nullptr, false, 0)
                                    : nullptr;
    if (image) {
      id layer = uikit::send<id>(view_, "layer");
      if (layer) uikit::send<void>(layer, "setContents:", (id) image);
      cg.CGImageRelease(image);
    }
    if (space) cg.CGColorSpaceRelease(space);
    if (provider) cg.CGDataProviderRelease(provider);
  }

  // ── Touches ──

  int slotOf(void* touch, bool claim) {
    for (int s = 0; s < 10; ++s)
      if (slotTouches_[s] == touch) return s;
    if (!claim) return -1;
    for (int s = 0; s < 10; ++s)
      if (!slotTouches_[s]) {
        slotTouches_[s] = touch;
        return s;
      }
    return -1;
  }

  void touches(id set, PointerSample::Phase phase) {
    if (!router_ || !set) return;
    id all = uikit::send<id>(set, "allObjects");
    const unsigned long n = all ? uikit::send<unsigned long>(all, "count") : 0;
    const double now =
        std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    for (unsigned long i = 0; i < n; ++i) {
      id touch = uikit::send<id>(all, "objectAtIndex:", i);
      if (!touch) continue;
      using Location = uikit::CGPoint2 (*)(id, SEL, id);
      const uikit::CGPoint2 at =
          reinterpret_cast<Location>(objc_msgSend)(touch, sel_registerName("locationInView:"), view_);
      PointerSample s;
      s.phase = phase;
      // Points are this SDK's logical pixels: no conversion.
      s.rootPosition = Point{(float) at.x, (float) at.y};
      s.time = now;
      const long type = uikit::send<long>(touch, "type"); // direct, indirect, pencil, pointer
      if (type == 3) {
        s.input.type = MouseInputSourceType::Mouse;
      } else if (type == 2) {
        s.input.type = MouseInputSourceType::Pen;
      } else {
        const int slot = slotOf((void*) touch, phase == PointerSample::Phase::Down);
        if (slot < 0) continue;
        s.input.type = MouseInputSourceType::Touch;
        s.input.index = slot;
      }
      const double force = uikit::send<double>(touch, "force");
      const double most = uikit::send<double>(touch, "maximumPossibleForce");
      if (most > 0.0) s.input.pressure = (float) (force / most);
      if (phase == PointerSample::Phase::Down && !primaryTouch_) primaryTouch_ = (void*) touch;
      s.primary = primaryTouch_ == (void*) touch;
      if (s.primary) lastPointer_ = s.rootPosition;
      MouseInputSources::instance().handle(*router_, s);
      if (phase == PointerSample::Phase::Up || phase == PointerSample::Phase::Cancel) {
        if (s.input.type == MouseInputSourceType::Touch) slotTouches_[s.input.index] = nullptr;
        if (primaryTouch_ == (void*) touch) primaryTouch_ = nullptr;
      }
    }
    renderIfDirty();
  }

  // ── The pasteboard ──

  static void pasteboardSet(const std::string& utf8) {
    id board = uikit::send<id>(uikit::cls("UIPasteboard"), "generalPasteboard");
    CFStringRef text = CFStringCreateWithCString(nullptr, utf8.c_str(), kCFStringEncodingUTF8);
    if (board && text) uikit::send<void>(board, "setString:", (id) text);
    if (text) CFRelease(text);
  }

  static bool pasteboardGet(std::string* out) {
    if (!out) return false;
    id board = uikit::send<id>(uikit::cls("UIPasteboard"), "generalPasteboard");
    id text = board ? uikit::send<id>(board, "string") : nil;
    if (!text) return false;
    const char* utf8 = uikit::send<const char*>(text, "UTF8String");
    *out = utf8 ? utf8 : "";
    return !out->empty();
  }

  static void installClipboard() {
    *Clipboard::setter() = [](const std::string& text) { pasteboardSet(text); };
    *Clipboard::getter() = [](std::string* out) { return pasteboardGet(out); };
  }

  Component* content_ = nullptr;
  MouseRouter* router_ = nullptr;
  Backing backing_;
  float logicalWidth_ = 0.0f, logicalHeight_ = 0.0f;
  id view_ = nil;
  id shield_ = nil;
  id link_ = nil;
  bool open_ = false;
  bool isPopup_ = false;
  bool grabsMouse_ = false;
  float popupScale_ = 0.0f;
  void* slotTouches_[10] = {};
  void* primaryTouch_ = nullptr;
  Point lastPointer_;
  std::string accessibleName_;
};

// ── The runtime classes ──────────────────────────────────────────────────────

namespace uikitdetail {

inline NativeWindowUIKit* ownerOf(id view) {
  void* owner = nullptr;
  if (view) object_getInstanceVariable(view, "owner", &owner);
  return static_cast<NativeWindowUIKit*>(owner);
}

inline void touchesBegan(id self, SEL, id touches, id) {
  if (NativeWindowUIKit* w = ownerOf(self)) w->touches(touches, PointerSample::Phase::Down);
}
inline void touchesMoved(id self, SEL, id touches, id) {
  if (NativeWindowUIKit* w = ownerOf(self)) w->touches(touches, PointerSample::Phase::Move);
}
inline void touchesEnded(id self, SEL, id touches, id) {
  if (NativeWindowUIKit* w = ownerOf(self)) w->touches(touches, PointerSample::Phase::Up);
}
inline void touchesCancelled(id self, SEL, id touches, id) {
  if (NativeWindowUIKit* w = ownerOf(self)) w->touches(touches, PointerSample::Phase::Cancel);
}

/** A touch on the shield is a touch outside the popup it holds. */
inline void shieldTouched(id self, SEL, id, id) {
  NativeWindowUIKit* w = ownerOf(self);
  if (w && w->grabsPointer() && w->onDismissedOutside) w->onDismissedOutside();
}

inline void displayTick(id self, SEL, id) {
  if (NativeWindowUIKit* w = ownerOf(self)) w->tick();
}

inline Class viewClass() {
  static Class klass = [] {
    char name[96];
    uikit::uniqueName("SonoreUIKitView", (void*) &touchesBegan, name, sizeof(name));
    if (Class existing = objc_getClass(name)) return existing;
    Class base = objc_getClass("UIView");
    if (!base) return (Class) nullptr;
    Class c = objc_allocateClassPair(base, name, 0);
    if (!c) return (Class) nullptr;
    class_addIvar(c, "owner", sizeof(void*), (uint8_t) (sizeof(void*) == 8 ? 3 : 2), "^v");
    class_addMethod(c, sel_registerName("touchesBegan:withEvent:"), (IMP) touchesBegan, "v@:@@");
    class_addMethod(c, sel_registerName("touchesMoved:withEvent:"), (IMP) touchesMoved, "v@:@@");
    class_addMethod(c, sel_registerName("touchesEnded:withEvent:"), (IMP) touchesEnded, "v@:@@");
    class_addMethod(c, sel_registerName("touchesCancelled:withEvent:"), (IMP) touchesCancelled, "v@:@@");
    class_addMethod(c, sel_registerName("sonoreTick:"), (IMP) displayTick, "v@:@");
    objc_registerClassPair(c);
    return c;
  }();
  return klass;
}

inline Class shieldClass() {
  static Class klass = [] {
    char name[96];
    uikit::uniqueName("SonoreUIKitShield", (void*) &shieldTouched, name, sizeof(name));
    if (Class existing = objc_getClass(name)) return existing;
    Class base = objc_getClass("UIView");
    if (!base) return (Class) nullptr;
    Class c = objc_allocateClassPair(base, name, 0);
    if (!c) return (Class) nullptr;
    class_addIvar(c, "owner", sizeof(void*), (uint8_t) (sizeof(void*) == 8 ? 3 : 2), "^v");
    class_addMethod(c, sel_registerName("touchesBegan:withEvent:"), (IMP) shieldTouched, "v@:@@");
    objc_registerClassPair(c);
    return c;
  }();
  return klass;
}

} // namespace uikitdetail

} // namespace gfx
} // namespace sonore

#endif // SONORE_IOS
