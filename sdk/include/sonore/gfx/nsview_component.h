// SPDX-License-Identifier: Apache-2.0
//
// NSViewComponentWithParent: a native NSView inside an editor's component
// tree, on macOS.
//
// ── What it is for ──────────────────────────────────────────────────────────
//
// Everything else in gfx is drawn by the software rasteriser into the one
// NSView the Cocoa peer owns. Some things cannot be: a hosted plugin's own
// editor (host_au.h hands one back as an NSView), a system control, a video
// layer. This component gives such a view a place in the tree -- it moves
// with the component, hides with it, and is clipped to nothing but its own
// frame, because it is a real subview drawn by AppKit on top of the peer.
//
// ── "With parent" ───────────────────────────────────────────────────────────
//
// The view handed over is not put straight into the peer's view. The
// component makes a PARENT NSView of its own, puts that in the peer's view
// where the component sits, and puts the child inside it, filling it. Every
// placement decision -- frame, hidden, which window -- is made on the
// parent, so the child is never repositioned behind its owner's back: a
// plugin view that lays itself out against its superview sees one stable
// superview for its whole life, whatever the editor does with the component.
// And when the CHILD resizes itself -- plugin editors do, when a user picks a
// larger size in them -- the component follows it (setFollowsViewSize), which
// is what the reference's NSViewComponentWithParent exists to do for a hosted
// plugin's view.
//
// ── How it finds the window ─────────────────────────────────────────────────
//
// A component knows its root and nothing about windows. NativeWindowCocoa
// keeps a list of the peers that are open and answers viewShowing(root); the
// parent view is moved into that view whenever the answer changes, and taken
// out when there is none. That is checked on every resize and by a 30 Hz
// timer, which also catches moves and visibility changes -- the component
// tree has no listener for either, and polling a frame is cheap next to the
// peer's own 30 Hz tick.
//
// Coordinates need no flip: the peer's view is flipped (y down), as the
// component tree is, so a component at (x, y) in root coordinates is a frame
// at (x, y) in that view, in points.
//
// ── Other platforms, and what was run ───────────────────────────────────────
//
// An NSView exists only on macOS. Elsewhere the class still exists, so code
// that names it compiles everywhere, but setView() refuses and lastError()
// says why. The placement arithmetic (detail::nativeEmbedPlacement) is
// portable and tested on Windows by tests/mac_host_test.cpp; the Cocoa half
// is COMPILED for macOS (tests/mac_syntax_mac_host.cpp) and has never run --
// there is no Mac in this loop. tests/mac_host_selftest.cpp makes one with a
// plain NSView and prints where it went.
#pragma once

#include <cstdio>
#include <string>

#include "component.h"

#if defined(__APPLE__)
#include <objc/message.h>
#include <objc/objc.h>
#include <objc/runtime.h>

#include "window_cocoa.h"
#endif

namespace sonore {
namespace gfx {

namespace detail {

/** Where an embedded native view belongs: the component's rectangle in its
 *  ROOT's coordinates, and whether it should be seen at all -- not when the
 *  component or any ancestor is hidden, and not when it has no area. */
struct NativeEmbedPlacement {
  Rect frame;
  bool visible = false;
};

inline NativeEmbedPlacement nativeEmbedPlacement(const Component& component) {
  NativeEmbedPlacement placement;
  const Point origin = component.localToRoot(Point{0.0f, 0.0f});
  const Rect& bounds = component.bounds();
  placement.frame = Rect(origin.x, origin.y, bounds.w, bounds.h);
  placement.visible = component.isShowing() && bounds.w > 0.0f && bounds.h > 0.0f;
  return placement;
}

} // namespace detail

class NSViewComponentWithParent;

#if defined(__APPLE__)
inline Class nsViewParentClass();
#endif

/**
 * A child NSView, held in a parent NSView this component owns and places.
 *
 * setView() takes any NSView* and retains it; null removes it. The component
 * paints nothing of its own -- AppKit draws the view above the editor.
 * Main thread only, like everything in a component tree.
 */
class NSViewComponentWithParent : public Component {
public:
  /** No view yet; the parent NSView is made by the first setView(). */
  NSViewComponentWithParent() = default;
  /** Takes both views out of the editor's view and releases them; stops the
   *  timer. */
  ~NSViewComponentWithParent() override { detach(); }

  /** Host `nsView` (an NSView*), or none with null. False, with lastError(),
   *  if it could not be: off macOS always. */
  bool setView(void* nsView);
  /** The child, as given. */
  void* getView() const { return child_; }
  /** The parent NSView this component made and places -- the "parent" in
   *  the name. Null before the first setView(). */
  void* parentView() const { return parent_; }

  /** Whether the component takes the child's size when the child changes it
   *  (default true), and when it is given a child while it has no size. */
  void setFollowsViewSize(bool follows) { followsChild_ = follows; }
  /** True unless setFollowsViewSize(false). */
  bool followsViewSize() const { return followsChild_; }

  /** Size the component to the child's frame now. False with no child. */
  bool resizeToFitView();

  /** Put the parent view where the component is: in the view its tree is
   *  shown in, at its bounds, hidden when it is. Done on every resize and
   *  thirty times a second; call it to have it done now. */
  void syncNativeView();

  /** Whether the parent view is in an open editor's view right now. */
  bool isAttached() const { return attached_; }

  /** Why the last thing that failed did. */
  const std::string& lastError() const { return error_; }

  /** Called by the parent view when the child's frame changed. */
  void childFrameChanged();

  /** Moves the parent view to the new bounds, as syncNativeView(). */
  void resized() override { syncNativeView(); }

private:
  bool makeParent();
  void detach();

  void* parent_ = nullptr;
  void* child_ = nullptr;
  void* timer_ = nullptr;
  Rect lastFrame_{-1.0f, -1.0f, -1.0f, -1.0f};
  bool lastVisible_ = true;
  bool attached_ = false;
  bool followsChild_ = true;
  bool syncing_ = false;
  std::string error_;
};

#if defined(__APPLE__)

/** The owner, from the parent view's instance variable. */
inline NSViewComponentWithParent* nsViewParentOwner(id view) {
  void* owner = nullptr;
  object_getInstanceVariable(view, "sonoreOwner", &owner);
  return static_cast<NSViewComponentWithParent*>(owner);
}

inline BOOL nsViewParentIsFlipped(id, SEL) { return YES; }

inline void nsViewParentSync(id self, SEL, id) {
  if (NSViewComponentWithParent* owner = nsViewParentOwner(self)) owner->syncNativeView();
}

inline void nsViewParentChildChanged(id self, SEL, id) {
  if (NSViewComponentWithParent* owner = nsViewParentOwner(self)) owner->childFrameChanged();
}

/** The parent view's class: an NSView, flipped like the peer's, carrying a
 *  pointer to its component and the two methods its timer and its
 *  notification call. Unique to this binary, for the reason the peer's view
 *  class gives -- two plugins in one host share one runtime. */
inline Class nsViewParentClass() {
  static Class klass = [] {
    char name[96];
    std::snprintf(name, sizeof(name), "SonoreNSViewParent_%llx",
                  (unsigned long long) (uintptr_t) (void*) &nsViewParentIsFlipped);
    if (Class existing = objc_getClass(name)) return existing;
    Class c = objc_allocateClassPair((Class) cocoa::cls("NSView"), name, 0);
    if (!c) return (Class) objc_getClass(name);
    class_addIvar(c, "sonoreOwner", sizeof(void*), 3, "^v");
    class_addMethod(c, sel_registerName("isFlipped"), (IMP) nsViewParentIsFlipped, "c@:");
    class_addMethod(c, sel_registerName("sonoreSync:"), (IMP) nsViewParentSync, "v@:@");
    class_addMethod(c, sel_registerName("sonoreChildFrameChanged:"), (IMP) nsViewParentChildChanged,
                    "v@:@");
    objc_registerClassPair(c);
    return c;
  }();
  return klass;
}

inline bool NSViewComponentWithParent::makeParent() {
  if (parent_) return true;
  Class viewClass = nsViewParentClass();
  if (!viewClass) {
    error_ = "the parent view's class could not be registered with the Objective-C runtime";
    return false;
  }
  id view = cocoa::msg<id>((id) viewClass, sel_registerName("alloc"));
  const cocoa::CGRect2 frame{{0.0, 0.0}, {(double) bounds().w, (double) bounds().h}};
  using InitFrame = id (*)(id, SEL, cocoa::CGRect2);
  view = view ? reinterpret_cast<InitFrame>(objc_msgSend)(view, sel_registerName("initWithFrame:"),
                                                          frame)
              : (id) nullptr;
  if (!view) {
    error_ = "NSView would not initialise";
    return false;
  }
  object_setInstanceVariable(view, "sonoreOwner", this);
  parent_ = view;
  // The timer that keeps the parent where the component is. Retains its
  // target, so detach() invalidates it before the view goes.
  using Sched = id (*)(id, SEL, double, id, SEL, id, BOOL);
  id timer = reinterpret_cast<Sched>(objc_msgSend)(
      cocoa::cls("NSTimer"),
      sel_registerName("scheduledTimerWithTimeInterval:target:selector:userInfo:repeats:"), 0.033,
      view, sel_registerName("sonoreSync:"), (id) nullptr, (BOOL) YES);
  if (!timer) {
    error_ = "NSTimer would not schedule: the view follows the component on resize only";
  } else {
    cocoa::keepTickingInEveryMode(timer);
    timer_ = timer;
  }
  return true;
}

inline bool NSViewComponentWithParent::setView(void* nsView) {
  if (!nsView && !parent_) return true; // nothing held, nothing asked for
  if (!makeParent()) return false;
  id parent = (id) parent_;
  id centre = cocoa::msg<id>(cocoa::cls("NSNotificationCenter"), sel_registerName("defaultCenter"));
  id frameChanged = cocoa::msg<id>(cocoa::cls("NSString"), sel_registerName("stringWithUTF8String:"),
                                   "NSViewFrameDidChangeNotification");
  if (child_) {
    id old = (id) child_;
    if (centre && frameChanged)
      cocoa::msg<void>(centre, sel_registerName("removeObserver:name:object:"), parent, frameChanged,
                       old);
    cocoa::msg<void>(old, sel_registerName("removeFromSuperview"));
    cocoa::msg<void>(old, sel_registerName("release"));
    child_ = nullptr;
  }
  if (!nsView) {
    syncNativeView();
    return true;
  }
  id child = (id) nsView;
  cocoa::msg<id>(child, sel_registerName("retain"));
  child_ = child;
  // A component with no size takes the child's: the size a plugin editor
  // opens at is the size it wants.
  if (followsChild_ && (bounds().w <= 0.0f || bounds().h <= 0.0f)) resizeToFitView();
  cocoa::msg<void>(parent, sel_registerName("addSubview:"), child);
  const cocoa::CGRect2 fill{{0.0, 0.0}, {(double) bounds().w, (double) bounds().h}};
  using SetFrame = void (*)(id, SEL, cocoa::CGRect2);
  reinterpret_cast<SetFrame>(objc_msgSend)(child, sel_registerName("setFrame:"), fill);
  // 18 == NSViewWidthSizable | NSViewHeightSizable: the child fills the
  // parent whatever size the parent is given.
  cocoa::msg<void>(child, sel_registerName("setAutoresizingMask:"), (unsigned long) 18);
  cocoa::msg<void>(child, sel_registerName("setPostsFrameChangedNotifications:"), (BOOL) YES);
  if (centre && frameChanged)
    cocoa::msg<void>(centre, sel_registerName("addObserver:selector:name:object:"), parent,
                     sel_registerName("sonoreChildFrameChanged:"), frameChanged, child);
  else
    error_ = "NSNotificationCenter was not available: the component will not follow the view's size";
  lastFrame_ = Rect(-1.0f, -1.0f, -1.0f, -1.0f); // placed afresh
  syncNativeView();
  return true;
}

inline bool NSViewComponentWithParent::resizeToFitView() {
  if (!child_) return false;
  const cocoa::CGRect2 frame =
      cocoa::msgRect<cocoa::CGRect2>((id) child_, sel_registerName("frame"));
  if (!(frame.size.width > 0.0) || !(frame.size.height > 0.0)) return false;
  setSize((float) frame.size.width, (float) frame.size.height);
  return true;
}

inline void NSViewComponentWithParent::childFrameChanged() {
  // A change this component made (syncing the parent, which autoresizes the
  // child) is not the child asking for a size.
  if (!followsChild_ || syncing_ || !child_) return;
  const cocoa::CGRect2 frame =
      cocoa::msgRect<cocoa::CGRect2>((id) child_, sel_registerName("frame"));
  const float w = (float) frame.size.width, h = (float) frame.size.height;
  if (w > 0.0f && h > 0.0f && (w != bounds().w || h != bounds().h)) setSize(w, h);
}

inline void NSViewComponentWithParent::syncNativeView() {
  if (!parent_ || syncing_) return;
  syncing_ = true;
  id parent = (id) parent_;
  id host = (id) NativeWindowCocoa::viewShowing(root());
  id superview = cocoa::msg<id>(parent, sel_registerName("superview"));
  if (superview != host) {
    if (superview) cocoa::msg<void>(parent, sel_registerName("removeFromSuperview"));
    if (host) cocoa::msg<void>(host, sel_registerName("addSubview:"), parent);
    lastFrame_ = Rect(-1.0f, -1.0f, -1.0f, -1.0f);
  }
  attached_ = host != nullptr;
  const detail::NativeEmbedPlacement placement = detail::nativeEmbedPlacement(*this);
  const Rect& f = placement.frame;
  if (f.x != lastFrame_.x || f.y != lastFrame_.y || f.w != lastFrame_.w || f.h != lastFrame_.h) {
    const cocoa::CGRect2 frame{{(double) f.x, (double) f.y}, {(double) f.w, (double) f.h}};
    using SetFrame = void (*)(id, SEL, cocoa::CGRect2);
    reinterpret_cast<SetFrame>(objc_msgSend)(parent, sel_registerName("setFrame:"), frame);
    lastFrame_ = f;
  }
  const bool visible = placement.visible && attached_;
  if (visible != lastVisible_) {
    cocoa::msg<void>(parent, sel_registerName("setHidden:"), (BOOL) (visible ? NO : YES));
    lastVisible_ = visible;
  }
  syncing_ = false;
}

inline void NSViewComponentWithParent::detach() {
  if (timer_) {
    cocoa::msg<void>((id) timer_, sel_registerName("invalidate"));
    timer_ = nullptr;
  }
  if (parent_) {
    id parent = (id) parent_;
    // The back-pointer first, then the observer: NSNotificationCenter does
    // not retain observers, and one left behind is a dangling pointer the
    // next time the child resizes.
    object_setInstanceVariable(parent, "sonoreOwner", nullptr);
    id centre = cocoa::msg<id>(cocoa::cls("NSNotificationCenter"), sel_registerName("defaultCenter"));
    if (centre) cocoa::msg<void>(centre, sel_registerName("removeObserver:"), parent);
  }
  if (child_) {
    cocoa::msg<void>((id) child_, sel_registerName("removeFromSuperview"));
    cocoa::msg<void>((id) child_, sel_registerName("release"));
    child_ = nullptr;
  }
  if (parent_) {
    cocoa::msg<void>((id) parent_, sel_registerName("removeFromSuperview"));
    cocoa::msg<void>((id) parent_, sel_registerName("release"));
    parent_ = nullptr;
  }
  attached_ = false;
}

#else // not macOS

inline bool NSViewComponentWithParent::setView(void* nsView) {
  if (!nsView) return true; // nothing asked for, nothing refused
  error_ = "NSViewComponentWithParent embeds an NSView, and NSViews exist only on macOS: this "
           "build is for another platform, so nothing was embedded";
  return false;
}

inline bool NSViewComponentWithParent::resizeToFitView() { return false; }
inline void NSViewComponentWithParent::syncNativeView() {}
inline void NSViewComponentWithParent::childFrameChanged() {}
inline bool NSViewComponentWithParent::makeParent() { return false; }
inline void NSViewComponentWithParent::detach() {
  // Named, so a compiler that reports unused private fields has nothing to
  // say about the ones only the Cocoa half uses.
  (void) parent_;
  (void) timer_;
  (void) lastFrame_;
  (void) lastVisible_;
  (void) syncing_;
}

#endif // __APPLE__

} // namespace gfx
} // namespace sonore
