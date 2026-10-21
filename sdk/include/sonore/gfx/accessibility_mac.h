// SPDX-License-Identifier: Apache-2.0
//
// The macOS end of accessibility: handing our component tree to NSAccessibility.
//
// ── Written, compiled, never run ────────────────────────────────────────────
//
// Said first, as window_cocoa.h says it: as of 2026-10-14 no machine in this
// project has executed a line of this file. It has been COMPILED, on Windows,
// against the Objective-C runtime shim in tests/au_shim (tests/
// mac_syntax_mac_input.cpp), which proves every name exists and every call has
// the right shape -- and not that VoiceOver hears anything. The Windows bridge
// (uia_win32.h) is verified by being the screen reader; this one is verified by
// nobody until a Mac developer runs tests/mac_input_selftest.cpp, which asks
// AppKit -- through the same attribute API the accessibility server uses --
// what this file exposes, and prints it.
//
// ── The shape ───────────────────────────────────────────────────────────────
//
// Same as UIA's, on the same platform-neutral model (accessibility.h):
//
//   the editor's NSView   -- an AXGroup named after the editor, whose
//                            accessibilityChildren are the top-level elements
//                            (window_cocoa.h registers those view methods)
//   one element each      -- an NSAccessibilityElement SUBCLASS, created at
//                            runtime with objc_allocateClassPair like the
//                            view itself, one instance per accessible
//                            component
//
// The roles, subroles and how each reads its value come from
// accessibleRoleMapping -- the same table UIA reads -- so a combo box cannot be
// one thing to NVDA and another to VoiceOver by accident.
//
// ── Why elements are KEPT, where UIA makes them per call ───────────────────
//
// A UIA client identifies an element by its runtime id, so the Windows bridge
// can hand out a new COM object on every navigation. VoiceOver identifies an
// element by the OBJECT: it compares pointers to see whether the focus moved,
// and a notification is about the object it is posted for. So each element
// lives as long as its component does -- matched across rebuilds by
// detail::matchAccessibleNodes -- and is told it is destroyed
// (NSAccessibilityUIElementDestroyedNotification) when the component goes.
//
// ── Threads ────────────────────────────────────────────────────────────────
//
// None to worry about, unlike UIA: AppKit services accessibility requests on
// the main thread, which is the thread the editor runs on. Every read comes
// from a snapshot retaken on the editor's clock, and every ACTION retakes it
// first, so the component pointer it acts on was found in the live tree a
// moment ago rather than remembered from a frame that may have closed a panel.
#pragma once

#if defined(__APPLE__) || defined(SONORE_APPLE_SYNTAX_CHECK)

#include <dlfcn.h>
#include <objc/message.h>
#include <objc/objc.h>
#include <objc/runtime.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "accessibility.h"

// Method type encodings. BOOL is `bool` ("B") on arm64 and `signed char` ("c")
// on x86_64 -- @encode(BOOL) differs, and AppKit's bridge from the attribute
// API to these methods reads the encoding to box the answer. Macros because
// they are spliced into string literals; prefixed and left defined, because
// window_cocoa.h registers the view's methods with the same strings.
#if defined(__arm64__) || defined(__aarch64__)
#define SONORE_OBJC_BOOL "B"
#else
#define SONORE_OBJC_BOOL "c"
#endif
#define SONORE_OBJC_RECT "{CGRect={CGPoint=dd}{CGSize=dd}}"
#define SONORE_OBJC_POINT "{CGPoint=dd}"
#define SONORE_OBJC_RANGE "{_NSRange=QQ}"

namespace sonore {
namespace gfx {
namespace nsax {

/** objc_msgSend through a correctly typed pointer -- see cocoa::msg in
 *  window_cocoa.h, which this duplicates so the bridge needs nothing from the
 *  window peer that includes it. */
template <typename Ret, typename... Args>
inline Ret send(id target, SEL selector, Args... args) {
  using Fn = Ret (*)(id, SEL, Args...);
  return reinterpret_cast<Fn>(objc_msgSend)(target, selector, args...);
}

/** A message answering a struct larger than 16 bytes (an NSRect): through
 *  objc_msgSend_stret on x86_64, plain objc_msgSend on arm64. */
template <typename Ret, typename... Args>
inline Ret sendRect(id target, SEL selector, Args... args) {
  using Fn = Ret (*)(id, SEL, Args...);
#if defined(__x86_64__)
  return reinterpret_cast<Fn>(objc_msgSend_stret)(target, selector, args...);
#else
  return reinterpret_cast<Fn>(objc_msgSend)(target, selector, args...);
#endif
}

inline id cls(const char* name) { return (id) objc_getClass(name); }
inline SEL sel(const char* name) { return sel_registerName(name); }

/** NSPoint, NSSize and NSRect, laid out as CoreGraphics lays them out. */
struct AxPoint {
  double x, y;
};
struct AxSize {
  double width, height;
};
struct AxRect {
  AxPoint origin;
  AxSize size;
};

inline id string(const std::string& utf8) {
  return send<id>(cls("NSString"), sel("stringWithUTF8String:"), utf8.c_str());
}

/** An NSString's UTF-8, or empty for nil or anything that is not one. */
inline std::string utf8Of(id value) {
  if (!value) return {};
  if (!send<BOOL>(value, sel("isKindOfClass:"), cls("NSString"))) return {};
  const char* text = send<const char*>(value, sel("UTF8String"));
  return text ? std::string(text) : std::string();
}

inline id number(double value) { return send<id>(cls("NSNumber"), sel("numberWithDouble:"), value); }

inline id array(const std::vector<id>& items) {
  return send<id>(cls("NSArray"), sel("arrayWithObjects:count:"), items.empty() ? nullptr : items.data(),
                  (unsigned long) items.size());
}

/**
 * The two AppKit functions the bridge calls, which are C functions rather than
 * messages and so are resolved with dlsym -- the same rule window_cocoa.h
 * follows for CoreGraphics, so no project has to name a framework.
 */
struct AppKitAccessibilityApi {
  /** NSAccessibilityPostNotification(element, name). */
  void (*postNotification)(id, id) = nullptr;
  /** NSAccessibilityRoleDescription(role, subrole): the localized "button". */
  id (*roleDescription)(id, id) = nullptr;
};

inline const AppKitAccessibilityApi& appKit() {
  static AppKitAccessibilityApi api = [] {
    AppKitAccessibilityApi a;
    void* lib = dlopen("/System/Library/Frameworks/AppKit.framework/AppKit", RTLD_LAZY);
    if (!lib) return a;
    void* post = dlsym(lib, "NSAccessibilityPostNotification");
    void* describe = dlsym(lib, "NSAccessibilityRoleDescription");
    std::memcpy(&a.postNotification, &post, sizeof(post));
    std::memcpy(&a.roleDescription, &describe, sizeof(describe));
    return a;
  }();
  return api;
}

/** Post one of AppKit's notifications ("AXValueChanged" and the rest are the
 *  values of the NSAccessibility*Notification constants). */
inline void post(id element, const char* notification) {
  const AppKitAccessibilityApi& a = appKit();
  if (element && a.postNotification) a.postNotification(element, string(notification));
}

/**
 * Where a view's bounds are on the screen, in Cocoa's y-up desktop space.
 *
 * convertRect:toView:nil takes the flipped view into its window (which undoes
 * the flip), and convertRectToScreen: places the window. False with no window
 * -- a view not yet in one is nowhere a reader could point at.
 */
inline bool viewOnScreen(id view, Rect* out) {
  if (!view || !out) return false;
  id window = send<id>(view, sel("window"));
  if (!window) return false;
  const AxRect bounds = sendRect<AxRect>(view, sel("bounds"));
  const AxRect inWindow = sendRect<AxRect>(view, sel("convertRect:toView:"), bounds, (id) nullptr);
  const AxRect onScreen = sendRect<AxRect>(window, sel("convertRectToScreen:"), inWindow);
  *out = Rect((float) onScreen.origin.x, (float) onScreen.origin.y, (float) onScreen.size.width,
              (float) onScreen.size.height);
  return true;
}

class MacAccessibilityRoot;
inline Class elementClass();

/**
 * The bridge: the snapshot, the elements, and the announcements.
 *
 * Owned by NativeWindowCocoa, created with its view and detached before the
 * view goes. Detaching leaves every element answering as an empty element,
 * which is the right answer for one VoiceOver still holds after the editor
 * closed.
 */
class MacAccessibilityRoot {
public:
  MacAccessibilityRoot(id view, Component* content, MouseRouter* router, std::string name)
      : view_(view), content_(content), router_(router), name_(std::move(name)) {}

  ~MacAccessibilityRoot() { detach(); }

  MacAccessibilityRoot(const MacAccessibilityRoot&) = delete;
  MacAccessibilityRoot& operator=(const MacAccessibilityRoot&) = delete;

  /** NSAccessibilityElement is macOS 10.10 and later. Without it there is no
   *  bridge, and unavailableReason says so rather than the editor being
   *  silent for no stated reason. */
  static bool isAvailable() { return elementClass() != nullptr; }
  static const char* unavailableReason() {
    return isAvailable() ? "" : "NSAccessibilityElement is not in this process (macOS 10.10 or later, with AppKit loaded)";
  }

  /** Called after an action changed something, so the peer can repaint. */
  std::function<void()> onChanged;

  const std::string& name() const { return name_; }

  /** Everything after this answers as empty -- see the class comment. */
  void detach() {
    for (id element : elements_) retire(element);
    elements_.clear();
    nodes_.clear();
    content_ = nullptr;
    router_ = nullptr;
    view_ = nullptr;
  }

  // ── The view's side (window_cocoa.h registers these as its methods) ──────

  /** The top-level elements. The first call is how the bridge learns that a
   *  reader is there at all. */
  id topLevelChildren(unsigned long long nowMs) {
    noteAsked(nowMs);
    refresh();
    return childrenOf(-1);
  }

  /** The deepest element under a SCREEN point, or the view. */
  id hitTest(AxPoint onScreen, unsigned long long nowMs) {
    noteAsked(nowMs);
    if (nodes_.empty()) refresh();
    Rect frame;
    if (!view_ || !viewOnScreen(view_, &frame)) return view_;
    const Point p = detail::screenPointToFlippedView(frame, {(float) onScreen.x, (float) onScreen.y});
    // Backwards, so the TOPMOST wins -- the snapshot is in paint order.
    for (size_t i = nodes_.size(); i-- > 0;)
      if (nodes_[i].info.bounds.contains(p)) return elements_[i];
    return view_;
  }

  /** The element with the keyboard focus, or the view. */
  id focusedElement(unsigned long long nowMs) {
    noteAsked(nowMs);
    refresh();
    for (size_t i = 0; i < nodes_.size(); ++i)
      if (nodes_[i].info.focused) return elements_[i];
    return view_;
  }

  /**
   * On the editor's clock: retake the snapshot and announce what changed --
   * but only while a reader is around. VoiceOver being on is the usual sign
   * (-[NSWorkspace isVoiceOverEnabled], 10.13 and later); a client that asked
   * in the last few seconds is the other (Accessibility Inspector, Switch
   * Control, a test). Otherwise every open editor would walk its tree thirty
   * times a second for nobody -- the mistake the Windows bridge made first.
   */
  void tick(unsigned long long nowMs) {
    if (!asked_ || !view_) return;
    if (!voiceOverRunning() && nowMs - lastAskedMs_ > 5000) return;
    refresh();
    for (const detail::AccessibilityEvent& e : announcer_.update(nodes_, nowMs)) {
      switch (e.kind) {
        case detail::AccessibilityEvent::Kind::FocusChanged:
          post(elementAt(e.index), "AXFocusedUIElementChanged");
          break;
        case detail::AccessibilityEvent::Kind::ValueChanged:
          post(elementAt(e.index), "AXValueChanged");
          break;
        case detail::AccessibilityEvent::Kind::LayoutChanged:
          post(view_, "AXLayoutChanged");
          break;
      }
    }
  }

  /** The snapshot as it stands, for the self-test. */
  const std::vector<AccessibleNode>& nodes() const { return nodes_; }

  // ── The elements' side ───────────────────────────────────────────────────

  /** The node `element` stands for, or null when it has been retired. */
  const AccessibleNode* nodeOf(id element) const {
    const int index = indexOf(element);
    return index >= 0 && index < (int) nodes_.size() && elements_[(size_t) index] == element
               ? &nodes_[(size_t) index]
               : nullptr;
  }

  id parentOf(id element) const {
    const AccessibleNode* node = nodeOf(element);
    if (!node) return nullptr;
    return node->parent >= 0 ? elements_[(size_t) node->parent] : view_;
  }

  id childrenOfElement(id element) const {
    const AccessibleNode* node = nodeOf(element);
    if (!node) return array({});
    return childrenOf(indexOf(element));
  }

  id window() const { return view_ ? send<id>(view_, sel("window")) : nullptr; }

  /** On the screen, y up. Zero when the view is in no window. */
  AxRect frameOf(id element) const {
    AxRect out{{0.0, 0.0}, {0.0, 0.0}};
    const AccessibleNode* node = nodeOf(element);
    Rect frame;
    if (!node || !viewOnScreen(view_, &frame)) return out;
    const Rect r = detail::flippedViewRectToScreen(frame, node->info.bounds);
    out.origin = {(double) r.x, (double) r.y};
    out.size = {(double) r.w, (double) r.h};
    return out;
  }

  /** Perform `action` on the component behind `element`, from a FRESH
   *  snapshot. */
  bool perform(id element, AccessibleAction action) {
    Component* c = liveComponentOf(element);
    if (!c) return false;
    const bool done = c->performAccessibleAction(action);
    afterChange(element, done);
    return done;
  }

  /** setAccessibilityValue: -- a number to a ranged control, a string to a
   *  field (or a choice, by name). */
  bool setValue(id element, id value) {
    Component* c = liveComponentOf(element);
    if (!c || !value) return false;
    bool done = false;
    if (send<BOOL>(value, sel("isKindOfClass:"), cls("NSString")))
      done = c->setAccessibleText(utf8Of(value));
    else if (send<BOOL>(value, sel("isKindOfClass:"), cls("NSNumber")))
      done = c->setAccessibleValue(send<double>(value, sel("doubleValue")));
    afterChange(element, done);
    return done;
  }

  /** setAccessibilityFocused:YES -- the keyboard focus to that control, and
   *  the view made first responder so the keys follow it. */
  bool focus(id element) {
    Component* c = liveComponentOf(element);
    if (!c || !router_ || !c->wantsKeyboardFocus()) return false;
    router_->setFocus(c);
    id win = window();
    if (win) send<BOOL>(win, sel("makeFirstResponder:"), view_);
    afterChange(element, true);
    return router_ && router_->focused() == c;
  }

private:
  static int indexOf(id element) {
    if (!element) return -1;
    void* value = nullptr;
    object_getInstanceVariable(element, "sonoreAxIndex", &value);
    return (int) (intptr_t) value;
  }

  /** A reader asked. `nowMs` 0 is a call with no clock to hand (an element's
   *  own hit test), which proves a reader is there but not when. */
  void noteAsked(unsigned long long nowMs) {
    asked_ = true;
    if (nowMs > lastAskedMs_) lastAskedMs_ = nowMs;
  }

  static bool voiceOverRunning() {
    id workspace = send<id>(cls("NSWorkspace"), sel("sharedWorkspace"));
    if (!workspace) return false;
    // 10.13 and later. Before that there is no cheap way to ask, and the
    // asked-recently rule alone decides.
    if (!send<BOOL>(workspace, sel("respondsToSelector:"), sel("isVoiceOverEnabled"))) return false;
    return send<BOOL>(workspace, sel("isVoiceOverEnabled")) != NO;
  }

  id elementAt(int index) const {
    return index >= 0 && index < (int) elements_.size() ? elements_[(size_t) index] : nullptr;
  }

  id childrenOf(int parent) const {
    std::vector<id> out;
    for (size_t i = 0; i < nodes_.size(); ++i)
      if (nodes_[i].parent == parent) out.push_back(elements_[i]);
    return array(out);
  }

  /**
   * Retake the snapshot from the live tree and carry the elements over.
   *
   * The keyboard focus counts only while the user is IN this editor: its
   * window key and this view its first responder. The component tree keeps
   * its focused control when the user clicks elsewhere in the host, and a
   * reader told about focus in a background editor talks over what the user
   * is doing -- the same rule, for the same reason, as uia::RootProvider.
   */
  void refresh() {
    if (!content_ || !view_) return;
    std::vector<AccessibleNode> fresh;
    collectAccessible(content_, fresh);
    id win = window();
    const bool inHere = win && send<BOOL>(win, sel("isKeyWindow")) &&
                        send<id>(win, sel("firstResponder")) == view_;
    if (!inHere)
      for (AccessibleNode& node : fresh) node.info.focused = false;

    const std::vector<int> match = detail::matchAccessibleNodes(nodes_, fresh);
    std::vector<id> kept(fresh.size(), nullptr);
    std::vector<char> reused(elements_.size(), 0);
    for (size_t i = 0; i < fresh.size(); ++i)
      if (match[i] >= 0) {
        kept[i] = elements_[(size_t) match[i]];
        reused[(size_t) match[i]] = 1;
      }
    // Gone first, so a reader is told before anything new arrives.
    for (size_t j = 0; j < elements_.size(); ++j)
      if (!reused[j]) retire(elements_[j]);
    Class klass = elementClass();
    for (size_t i = 0; i < fresh.size(); ++i) {
      if (!kept[i] && klass) {
        id made = send<id>((id) klass, sel("alloc"));
        made = made ? send<id>(made, sel("init")) : nullptr;
        if (made) object_setInstanceVariable(made, "sonoreAxRoot", this);
        kept[i] = made;
      }
      if (kept[i]) object_setInstanceVariable(kept[i], "sonoreAxIndex", (void*) (intptr_t) i);
    }
    nodes_.swap(fresh);
    elements_.swap(kept);
  }

  /** A component found in the live tree just now, for `element`; null when
   *  the element no longer stands for anything. */
  Component* liveComponentOf(id element) {
    refresh();
    const AccessibleNode* node = nodeOf(element);
    return node ? node->component : nullptr;
  }

  /** After a reader changed something: repaint, and tell it the value moved
   *  when the element is not the focused one (whose value the clock
   *  announces anyway). */
  void afterChange(id element, bool changed) {
    if (!changed) return;
    if (onChanged) onChanged();
    refresh();
    const AccessibleNode* node = nodeOf(element);
    if (node && !node->info.focused) post(element, "AXValueChanged");
  }

  /** Told it is destroyed, cut loose, released. VoiceOver may hold it for a
   *  while yet; with no root it answers as nothing. */
  static void retire(id element) {
    if (!element) return;
    post(element, "AXUIElementDestroyed");
    object_setInstanceVariable(element, "sonoreAxRoot", nullptr);
    object_setInstanceVariable(element, "sonoreAxIndex", (void*) (intptr_t) -1);
    send<void>(element, sel("release"));
  }

  id view_;
  Component* content_;
  MouseRouter* router_;
  std::string name_;
  std::vector<AccessibleNode> nodes_;
  std::vector<id> elements_; // retained, one per node
  detail::AccessibilityAnnouncer announcer_;
  bool asked_ = false;
  unsigned long long lastAskedMs_ = 0;
};

// ── The element's methods ──────────────────────────────────────────────────
//
// NSAccessibility's informal protocol, as C functions registered on the
// element class. Each finds its root through an instance variable and answers
// from the snapshot; a retired element finds none and answers as nothing.

inline MacAccessibilityRoot* rootOf(id element) {
  void* value = nullptr;
  if (element) object_getInstanceVariable(element, "sonoreAxRoot", &value);
  return static_cast<MacAccessibilityRoot*>(value);
}

inline const AccessibleNode* nodeOfElement(id self) {
  MacAccessibilityRoot* root = rootOf(self);
  return root ? root->nodeOf(self) : nullptr;
}

inline id axRole(id self, SEL) {
  const AccessibleNode* n = nodeOfElement(self);
  return string(accessibleRoleMapping(n ? n->info.role : AccessibleRole::Unknown).axRole);
}

inline id axSubrole(id self, SEL) {
  const AccessibleNode* n = nodeOfElement(self);
  const char* subrole = n ? accessibleRoleMapping(n->info.role).axSubrole : nullptr;
  return subrole ? string(subrole) : nullptr;
}

inline id axRoleDescription(id self, SEL) {
  const AccessibleNode* n = nodeOfElement(self);
  const AccessibleRoleMapping& m = accessibleRoleMapping(n ? n->info.role : AccessibleRole::Unknown);
  const AppKitAccessibilityApi& a = appKit();
  if (!a.roleDescription) return nullptr;
  return a.roleDescription(string(m.axDescribeAsRole),
                           m.axDescribeAsSubrole ? string(m.axDescribeAsSubrole) : nullptr);
}

/** The name as a DESCRIPTION -- unless the role reads it as its title or as
 *  its value, where saying it twice is what a reader would then do. */
inline id axLabel(id self, SEL) {
  const AccessibleNode* n = nodeOfElement(self);
  if (!n || n->info.name.empty()) return nullptr;
  const AccessibleRoleMapping& m = accessibleRoleMapping(n->info.role);
  if (m.axNameIsTitle || m.axValue == AccessibleValueKind::NameAsValue) return nullptr;
  return string(n->info.name);
}

inline id axTitle(id self, SEL) {
  const AccessibleNode* n = nodeOfElement(self);
  if (!n || n->info.name.empty() || !accessibleRoleMapping(n->info.role).axNameIsTitle) return nullptr;
  return string(n->info.name);
}

/** Typed as the role reads it: a number for a slider (with the text as its
 *  description), 0 or 1 for a toggle, the text for a field or a choice. */
inline id axValue(id self, SEL) {
  const AccessibleNode* n = nodeOfElement(self);
  if (!n) return nullptr;
  const AccessibleInfo& i = n->info;
  switch (accessibleRoleMapping(i.role).axValue) {
    case AccessibleValueKind::Number:
      if (i.hasRange) return number(i.currentValue);
      return i.value.empty() ? nullptr : string(i.value);
    case AccessibleValueKind::Toggle:
      if (i.toggle == AccessibleToggle::NotToggle) return nullptr;
      return number(i.toggle == AccessibleToggle::On ? 1.0 : 0.0);
    case AccessibleValueKind::NameAsValue:
      return i.name.empty() ? nullptr : string(i.name);
    case AccessibleValueKind::Text:
      break;
  }
  return i.value.empty() ? nullptr : string(i.value);
}

inline id axValueDescription(id self, SEL) {
  const AccessibleNode* n = nodeOfElement(self);
  if (!n || n->info.value.empty()) return nullptr;
  const AccessibleValueKind kind = accessibleRoleMapping(n->info.role).axValue;
  if (kind != AccessibleValueKind::Number && kind != AccessibleValueKind::Toggle) return nullptr;
  return string(n->info.value);
}

inline id axMinValue(id self, SEL) {
  const AccessibleNode* n = nodeOfElement(self);
  return n && n->info.hasRange ? number(n->info.minValue) : nullptr;
}

inline id axMaxValue(id self, SEL) {
  const AccessibleNode* n = nodeOfElement(self);
  return n && n->info.hasRange ? number(n->info.maxValue) : nullptr;
}

inline id axHelp(id self, SEL) {
  const AccessibleNode* n = nodeOfElement(self);
  return n && !n->info.description.empty() ? string(n->info.description) : nullptr;
}

inline void axSetValue(id self, SEL, id value) {
  if (MacAccessibilityRoot* root = rootOf(self)) root->setValue(self, value);
}

inline AxRect axFrame(id self, SEL) {
  MacAccessibilityRoot* root = rootOf(self);
  return root ? root->frameOf(self) : AxRect{{0.0, 0.0}, {0.0, 0.0}};
}

inline id axParent(id self, SEL) {
  MacAccessibilityRoot* root = rootOf(self);
  return root ? root->parentOf(self) : nullptr;
}

inline id axChildren(id self, SEL) {
  MacAccessibilityRoot* root = rootOf(self);
  return root ? root->childrenOfElement(self) : array({});
}

inline id axWindow(id self, SEL) {
  MacAccessibilityRoot* root = rootOf(self);
  return root ? root->window() : nullptr;
}

inline BOOL axIsElement(id self, SEL) { return nodeOfElement(self) ? YES : NO; }

inline BOOL axIsEnabled(id self, SEL) {
  const AccessibleNode* n = nodeOfElement(self);
  return n && n->info.enabled ? YES : NO;
}

inline BOOL axIsFocused(id self, SEL) {
  const AccessibleNode* n = nodeOfElement(self);
  return n && n->info.focused ? YES : NO;
}

inline void axSetFocused(id self, SEL, BOOL focused) {
  if (!focused) return; // focus goes TO something; nothing to do to take it away
  if (MacAccessibilityRoot* root = rootOf(self)) root->focus(self);
}

inline BOOL axIsSelected(id self, SEL) {
  const AccessibleNode* n = nodeOfElement(self);
  return n && n->info.selected ? YES : NO;
}

inline id axHitTest(id self, SEL, AxPoint point) {
  MacAccessibilityRoot* root = rootOf(self);
  // From the root, which knows paint order: the element under a point may be
  // a sibling drawn over this one, and the topmost is what the user sees.
  return root ? root->hitTest(point, 0) : self;
}

inline BOOL axPerform(id self, AccessibleAction action) {
  MacAccessibilityRoot* root = rootOf(self);
  return root && root->perform(self, action) ? YES : NO;
}
inline BOOL axPress(id self, SEL) { return axPerform(self, AccessibleAction::Press); }
inline BOOL axIncrement(id self, SEL) { return axPerform(self, AccessibleAction::Increment); }
inline BOOL axDecrement(id self, SEL) { return axPerform(self, AccessibleAction::Decrement); }
inline BOOL axShowMenu(id self, SEL) { return axPerform(self, AccessibleAction::ShowMenu); }

/**
 * isAccessibilitySelectorAllowed: -- which of the methods above apply to THIS
 * element now. AppKit builds the action list and the settable attributes from
 * the implemented methods filtered through this, so a label offers no press
 * and a read-only field no settable value, from the one class all elements
 * share.
 */
inline BOOL axSelectorAllowed(id self, SEL, SEL asked) {
  const AccessibleNode* n = nodeOfElement(self);
  if (!n) return NO;
  const AccessibleInfo& i = n->info;
  if (asked == sel("setAccessibilityValue:")) return i.valueSettable ? YES : NO;
  if (asked == sel("setAccessibilityFocused:")) return i.focusable ? YES : NO;
  if (asked == sel("accessibilityPerformPress")) return i.offers(AccessibleAction::Press) ? YES : NO;
  if (asked == sel("accessibilityPerformIncrement")) return i.offers(AccessibleAction::Increment) ? YES : NO;
  if (asked == sel("accessibilityPerformDecrement")) return i.offers(AccessibleAction::Decrement) ? YES : NO;
  if (asked == sel("accessibilityPerformShowMenu")) return i.offers(AccessibleAction::ShowMenu) ? YES : NO;
  if (asked == sel("accessibilityMinValue") || asked == sel("accessibilityMaxValue"))
    return i.hasRange ? YES : NO;
  return YES;
}

/**
 * The element class, registered once per binary.
 *
 * A subclass of NSAccessibilityElement (10.10), which already implements the
 * protocol's plumbing -- the attribute API a reader's server calls, built from
 * the methods below -- and needs from us what the element IS. Named after an
 * address in this binary, for the reason window_cocoa.h names its view class
 * so: two plugins built from this SDK share one Objective-C runtime.
 */
inline Class elementClass() {
  static Class klass = []() -> Class {
    Class base = objc_getClass("NSAccessibilityElement");
    if (!base) return nullptr;
    char name[96];
    std::snprintf(name, sizeof(name), "SonoreAXElement_%llx",
                  (unsigned long long) (uintptr_t) (void*) &axRole);
    if (Class existing = objc_getClass(name)) return existing;
    Class c = objc_allocateClassPair(base, name, 0);
    if (!c) return objc_getClass(name);
    class_addIvar(c, "sonoreAxRoot", sizeof(void*), 3, "^v");
    class_addIvar(c, "sonoreAxIndex", sizeof(void*), 3, "^v");
    auto add = [&](const char* selector, IMP imp, const char* types) {
      class_addMethod(c, sel_registerName(selector), imp, types);
    };
    add("accessibilityRole", (IMP) axRole, "@@:");
    add("accessibilitySubrole", (IMP) axSubrole, "@@:");
    add("accessibilityRoleDescription", (IMP) axRoleDescription, "@@:");
    add("accessibilityLabel", (IMP) axLabel, "@@:");
    add("accessibilityTitle", (IMP) axTitle, "@@:");
    add("accessibilityValue", (IMP) axValue, "@@:");
    add("setAccessibilityValue:", (IMP) axSetValue, "v@:@");
    add("accessibilityValueDescription", (IMP) axValueDescription, "@@:");
    add("accessibilityMinValue", (IMP) axMinValue, "@@:");
    add("accessibilityMaxValue", (IMP) axMaxValue, "@@:");
    add("accessibilityHelp", (IMP) axHelp, "@@:");
    add("accessibilityFrame", (IMP) axFrame, SONORE_OBJC_RECT "@:");
    add("accessibilityParent", (IMP) axParent, "@@:");
    add("accessibilityChildren", (IMP) axChildren, "@@:");
    add("accessibilityWindow", (IMP) axWindow, "@@:");
    add("accessibilityTopLevelUIElement", (IMP) axWindow, "@@:");
    add("isAccessibilityElement", (IMP) axIsElement, SONORE_OBJC_BOOL "@:");
    add("isAccessibilityEnabled", (IMP) axIsEnabled, SONORE_OBJC_BOOL "@:");
    add("isAccessibilityFocused", (IMP) axIsFocused, SONORE_OBJC_BOOL "@:");
    add("setAccessibilityFocused:", (IMP) axSetFocused, "v@:" SONORE_OBJC_BOOL);
    add("isAccessibilitySelected", (IMP) axIsSelected, SONORE_OBJC_BOOL "@:");
    add("accessibilityHitTest:", (IMP) axHitTest, "@@:" SONORE_OBJC_POINT);
    add("accessibilityPerformPress", (IMP) axPress, SONORE_OBJC_BOOL "@:");
    add("accessibilityPerformIncrement", (IMP) axIncrement, SONORE_OBJC_BOOL "@:");
    add("accessibilityPerformDecrement", (IMP) axDecrement, SONORE_OBJC_BOOL "@:");
    add("accessibilityPerformShowMenu", (IMP) axShowMenu, SONORE_OBJC_BOOL "@:");
    add("isAccessibilitySelectorAllowed:", (IMP) axSelectorAllowed, SONORE_OBJC_BOOL "@::");
    objc_registerClassPair(c);
    return c;
  }();
  return klass;
}

} // namespace nsax
} // namespace gfx
} // namespace sonore

#endif // __APPLE__ || SONORE_APPLE_SYNTAX_CHECK
