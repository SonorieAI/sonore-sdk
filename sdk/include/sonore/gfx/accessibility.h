// SPDX-License-Identifier: Apache-2.0
//
// Walking the tree a screen reader would want to see.
//
// The vocabulary -- roles, names, values, ranges -- is in accessible_info.h,
// which Component includes so it can declare accessibleInfo(). This is the
// traversal, which needs Components and so cannot live there.
//
// Everything here is testable against a component tree with no window at all.
// That is the reason for the split: the platform bridges are the part that
// cannot be tested on this machine, and they should be a mechanical mapping
// from this rather than a second source of truth about what a knob is called.
//
// ── One table, every platform ───────────────────────────────────────────────
//
// What each role is called on each platform lives in ONE table below, read by
// both bridges: uia_win32.h for UI Automation's control type, and
// accessibility_mac.h for NSAccessibility's role, subrole and how the value is
// read. Two switch statements in two files are two answers to "what is a
// combo box", and the second platform would drift from the first without
// anything noticing; one row per role is a thing a test can walk.
//
// The rest of what the macOS bridge decides -- which change to announce, which
// element survives a rebuild, where a rectangle is on a y-up screen -- is here
// too, in `detail`, because it is arithmetic and bookkeeping that runs on any
// machine. The bridge that cannot be run here is left holding only the calls
// into AppKit.
#pragma once

#include <cmath>
#include <string>
#include <vector>

#include "accessible_info.h"
#include "component.h"

namespace sonore {
namespace gfx {

/** How a platform that types its values (NSAccessibility: a number for a
 *  slider, a string for a field) should read this role's. */
enum class AccessibleValueKind {
  /** The value text, when there is one: a field's contents, a choice's name. */
  Text,
  /** A position in the range, with the text as its description -- "0.25",
   *  described as "-6.0 dB". */
  Number,
  /** On or off, as 1 or 0, with the state's own name as its description. */
  Toggle,
  /** The NAME is what is read: static text, whose value IS what it says. */
  NameAsValue,
};

/**
 * One role, on every platform.
 *
 * The UIA numbers are UIA_*ControlTypeId from UIAutomationClient.h -- stable
 * ABI, written out so this header needs no Windows header, and checked against
 * the real constants by static_assert in uia_win32.h. The AX strings are the
 * values of Apple's kAX*Role / kAX*Subrole constants (AXRoleConstants.h),
 * which is what NSAccessibilityButtonRole and the rest ARE: NSStrings that
 * compare by value.
 */
struct AccessibleRoleMapping {
  AccessibleRole role;   ///< the SDK's role this row maps
  long uiaControlType;   ///< its UIA_*ControlTypeId
  const char* axRole;    ///< its kAX*Role string
  const char* axSubrole; ///< its kAX*Subrole string, or null
  /** What a reader should CALL the role -- the pair handed to
   *  NSAccessibilityRoleDescription. Usually the role itself; a dialog drawn
   *  inside the editor is an AXGroup that should still be called "dialog". */
  const char* axDescribeAsRole;
  const char* axDescribeAsSubrole; ///< the subrole half of that pair, or null
  AccessibleValueKind axValue;      ///< how NSAccessibility is given its value
  /** Read as the element's TITLE (its visible caption, as an NSButton's is)
   *  rather than as a description of it. */
  bool axNameIsTitle;
};

/** The row for `role`. Every role has one; Unknown's is the fallback. */
inline const AccessibleRoleMapping& accessibleRoleMapping(AccessibleRole role) {
  // UIA ids: Button 50000, ComboBox 50003, Edit 50004, ListItem 50007,
  // List 50008, ProgressBar 50012, ScrollBar 50014, Slider 50015, Text 50020,
  // Group 50026, Window 50032, Pane 50033, Table 50036.
  static const AccessibleRoleMapping table[] = {
      // Pane, not Custom, on Windows: a Custom element makes a reader ask the
      // provider for a localized type it would then have to invent; Pane is
      // the honest "a region that holds things", and AXGroup is the same
      // thing on a Mac.
      {AccessibleRole::Unknown, 50033, "AXGroup", nullptr, "AXGroup", nullptr,
       AccessibleValueKind::Text, false},
      {AccessibleRole::Group, 50026, "AXGroup", nullptr, "AXGroup", nullptr,
       AccessibleValueKind::Text, false},
      // AXGroup, NOT AXWindow, though UIA says Window. A UIA Window element
      // needs no HWND of its own; an AXWindow promises a real NSWindow's
      // attributes -- AXMain, AXMinimized, a close button -- and a reader
      // asking for them inside a host's window gets nothing. Described as a
      // window, so it is still called one.
      {AccessibleRole::Window, 50032, "AXGroup", nullptr, "AXWindow", nullptr,
       AccessibleValueKind::Text, true},
      {AccessibleRole::Dialog, 50032, "AXGroup", nullptr, "AXWindow", "AXDialog",
       AccessibleValueKind::Text, true},
      {AccessibleRole::Button, 50000, "AXButton", nullptr, "AXButton", nullptr,
       AccessibleValueKind::Text, true},
      // A ToggleButton is a Button that also has a state. UIA has no separate
      // control type, and inventing one would produce an element no reader
      // has a phrase for. AppKit's own toggle button (an NSButton of the
      // push-on-push-off type) is an AXCheckBox with the AXToggle subrole,
      // and reads 0 or 1.
      {AccessibleRole::ToggleButton, 50000, "AXCheckBox", "AXToggle", "AXCheckBox", "AXToggle",
       AccessibleValueKind::Toggle, true},
      {AccessibleRole::Slider, 50015, "AXSlider", nullptr, "AXSlider", nullptr,
       AccessibleValueKind::Number, false},
      {AccessibleRole::Label, 50020, "AXStaticText", nullptr, "AXStaticText", nullptr,
       AccessibleValueKind::NameAsValue, false},
      {AccessibleRole::TextField, 50004, "AXTextField", nullptr, "AXTextField", nullptr,
       AccessibleValueKind::Text, false},
      // ComboBox on Windows; on a Mac this control is a POP-UP BUTTON. An
      // AXComboBox is an editable text field with a list, and promises a
      // value a reader can type into -- which a choice from a fixed list is
      // not. AppKit's NSPopUpButton is exactly this control.
      {AccessibleRole::ComboBox, 50003, "AXPopUpButton", nullptr, "AXPopUpButton", nullptr,
       AccessibleValueKind::Text, false},
      {AccessibleRole::List, 50008, "AXList", nullptr, "AXList", nullptr,
       AccessibleValueKind::Text, false},
      // AXGroup, which is what WebKit makes of a list item too: AppKit has no
      // list-item role, and AXRow belongs to a table that answers AXRows.
      {AccessibleRole::ListItem, 50007, "AXGroup", nullptr, "AXGroup", nullptr,
       AccessibleValueKind::Text, false},
      // Table, not DataGrid, on Windows: DataGrid promises the GridPattern --
      // a client may ask for the cell at row 4 column 2 and expect an element
      // back -- and claiming a pattern that is not implemented is worse than
      // claiming a simpler control type: the reader asks, gets nothing, and
      // says nothing. The same reasoning on a Mac makes it a LIST: an AXTable
      // promises AXRows and AXColumns, and a reader told "table, 0 rows" about
      // one with forty is worse off than one told "list".
      {AccessibleRole::Table, 50036, "AXList", nullptr, "AXList", nullptr,
       AccessibleValueKind::Text, false},
      {AccessibleRole::ScrollBar, 50014, "AXScrollBar", nullptr, "AXScrollBar", nullptr,
       AccessibleValueKind::Number, false},
      {AccessibleRole::ProgressBar, 50012, "AXProgressIndicator", nullptr, "AXProgressIndicator",
       nullptr, AccessibleValueKind::Number, false},
      // Group on both, and deliberately: no platform has a keyboard role, and
      // a group named "Keyboard" is a phrase every reader can say.
      {AccessibleRole::MusicalKeyboard, 50026, "AXGroup", nullptr, "AXGroup", nullptr,
       AccessibleValueKind::Text, false},
  };
  for (const AccessibleRoleMapping& row : table)
    if (row.role == role) return row;
  return table[0];
}

/**
 * One entry in the flattened tree a bridge walks.
 *
 * Flattened rather than nested, because every bridge needs both -- a linear
 * order for "next element" and a parent link for hierarchy -- and computing one
 * from the other twice is how the two end up disagreeing.
 */
struct AccessibleNode {
  Component* component = nullptr; ///< not owned; valid while the tree is unchanged
  int parent = -1; ///< index of the nearest announced ancestor in the list; -1 for none
  int depth = 0;   ///< how many announced ancestors it has
  AccessibleInfo info; ///< its description, `bounds` filled in, in root coordinates
};

/**
 * The accessible tree, in reading order.
 *
 * Skips components that are invisible or marked ignored, AND their subtrees for
 * the invisible ones -- a hidden panel's contents are not merely unannounced,
 * they are not there. An IGNORED component keeps its children: that is the
 * difference between "this thing is not worth describing" and "none of this
 * exists", and a layout container is the first and not the second.
 */
inline void collectAccessible(Component* c, std::vector<AccessibleNode>& out, int parent = -1,
                              int depth = 0, Point offset = {0.0f, 0.0f}) {
  if (!c || !c->isVisible()) return;
  const Point here{offset.x + c->bounds().x, offset.y + c->bounds().y};

  int index = parent;
  if (!c->isAccessibilityIgnored()) {
    AccessibleNode node;
    node.component = c;
    node.parent = parent;
    node.depth = depth;
    node.info = c->accessibleInfo();
    node.info.bounds = Rect(here.x, here.y, c->bounds().w, c->bounds().h);
    out.push_back(node);
    index = (int) out.size() - 1;
    ++depth;
  }

  for (Component* child : c->children()) collectAccessible(child, out, index, depth, here);
}

/** The whole tree as one string, one line per element, indented by depth.
 *
 *  For tests and for a developer asking "what would a screen reader say about
 *  my editor" -- which is a question that otherwise has no answer short of
 *  installing one. */
inline std::string describeAccessibleTree(Component* root) {
  std::vector<AccessibleNode> nodes;
  collectAccessible(root, nodes);
  std::string out;
  for (const AccessibleNode& n : nodes) {
    out.append((size_t) n.depth * 2, ' ');
    out += accessibleRoleName(n.info.role);
    if (!n.info.name.empty()) out += " \"" + n.info.name + "\"";
    if (!n.info.value.empty()) out += " = " + n.info.value;
    if (!n.info.enabled) out += " (disabled)";
    if (n.info.focused) out += " (focused)";
    out += "\n";
  }
  return out;
}

namespace detail {

/**
 * Which of the elements a bridge built for `before` are the same controls in
 * `after`: for each node of `after`, the index in `before` of the node with
 * the same component and role, or -1 for a new one.
 *
 * A reader holds ELEMENTS, not indices. VoiceOver keeps the object it is on,
 * compares objects to see whether the focus moved, and was told about this
 * one by identity -- so a bridge that rebuilt every element whenever a panel
 * opened above a knob would hand it a stranger with the knob's name, and the
 * reader would start the knob over. Matching by component keeps the object
 * while the control lives. By role as well, because a component that changes
 * what it IS (a button that becomes a toggle) is announced afresh.
 *
 * The component pointer is compared, never followed: `before` may hold
 * components that have since been destroyed.
 */
inline std::vector<int> matchAccessibleNodes(const std::vector<AccessibleNode>& before,
                                             const std::vector<AccessibleNode>& after) {
  std::vector<int> out(after.size(), -1);
  std::vector<char> taken(before.size(), 0);
  // The common case first and in one pass: nothing moved, so node i is node
  // i. Then a search for the rest -- a plugin editor is a few hundred nodes at
  // most, and this runs on a clock, not per paint.
  for (size_t i = 0; i < after.size() && i < before.size(); ++i)
    if (after[i].component == before[i].component && after[i].info.role == before[i].info.role) {
      out[i] = (int) i;
      taken[i] = 1;
    }
  for (size_t i = 0; i < after.size(); ++i) {
    if (out[i] >= 0) continue;
    for (size_t j = 0; j < before.size(); ++j)
      if (!taken[j] && before[j].component == after[i].component &&
          before[j].info.role == after[i].info.role) {
        out[i] = (int) j;
        taken[j] = 1;
        break;
      }
  }
  return out;
}

/** What a bridge should tell a reader that has not asked. */
struct AccessibilityEvent {
  enum class Kind {
    /** The keyboard focus moved to another control: say it. */
    FocusChanged,
    /** The focused control's value changed: say the new one. */
    ValueChanged,
    /** Controls appeared, went or moved: whatever a reader cached about the
     *  layout is stale. */
    LayoutChanged,
  };
  Kind kind;
  int index; // into the snapshot; -1 for LayoutChanged
};

/**
 * The bookkeeping behind unasked-for announcements: given each new snapshot
 * and the clock, which events to raise.
 *
 * The rules are the Windows bridge's, which a real UIA client has checked
 * (uia::RootProvider::announce), kept here so the Mac bridge follows the same
 * ones and a test can drive them:
 *
 *  - Compared against what was ANNOUNCED, not against the previous snapshot.
 *  - The same control by identity and role, never by name: a focused button
 *    whose label changes is still the button, and comparing names said the
 *    whole control again on every change.
 *  - Only the FOCUSED control's value, at most four times a second, the last
 *    value always: every value of every meter at 30 Hz is a reader that will
 *    not stop talking. A focus event already carries the value, so a value
 *    event never rides with one.
 *  - Layout at most four times a second as well -- an editor animating a
 *    panel open moves something every frame -- and the last change always.
 */
class AccessibilityAnnouncer {
public:
  static constexpr unsigned long long kMinimumGapMs = 250;

  std::vector<AccessibilityEvent> update(const std::vector<AccessibleNode>& nodes,
                                         unsigned long long nowMs) {
    std::vector<AccessibilityEvent> out;

    // ── Layout: what is there, what it is and where ──
    std::vector<Shape> shape;
    shape.reserve(nodes.size());
    for (const AccessibleNode& n : nodes)
      shape.push_back({n.component, n.parent, n.info.role, std::lround(n.info.bounds.x),
                       std::lround(n.info.bounds.y), std::lround(n.info.bounds.w),
                       std::lround(n.info.bounds.h)});
    if (!started_) {
      // The first look is a baseline, not news: whoever asked for the tree
      // has just been handed all of it.
      started_ = true;
      shape_ = std::move(shape);
      lastLayoutMs_ = nowMs;
    } else if (!(shape == shape_)) {
      shape_ = std::move(shape);
      layoutPending_ = true;
    }
    if (layoutPending_ && nowMs - lastLayoutMs_ >= kMinimumGapMs) {
      out.push_back({AccessibilityEvent::Kind::LayoutChanged, -1});
      layoutPending_ = false;
      lastLayoutMs_ = nowMs;
    }

    // ── Focus, and the focused control's value ──
    int focused = -1;
    for (size_t i = 0; i < nodes.size(); ++i)
      if (nodes[i].info.focused) {
        focused = (int) i;
        break;
      }
    if (focused < 0) {
      // Out of the editor: nothing to say, and the next control focused is
      // news even if it is the one that had it before.
      announcedComponent_ = nullptr;
      announcedRole_ = AccessibleRole::Unknown;
      return out;
    }
    const AccessibleNode& node = nodes[(size_t) focused];
    const bool sameControl = node.component == announcedComponent_ && node.info.role == announcedRole_;
    if (!sameControl) {
      out.push_back({AccessibilityEvent::Kind::FocusChanged, focused});
      announcedComponent_ = node.component;
      announcedRole_ = node.info.role;
      announcedValue_ = node.info.value;
      announcedNumber_ = node.info.currentValue;
      valuePending_ = false;
      lastValueMs_ = nowMs;
      return out;
    }
    if (node.info.value != announcedValue_ ||
        (node.info.hasRange && node.info.currentValue != announcedNumber_))
      valuePending_ = true;
    if (valuePending_ && nowMs - lastValueMs_ >= kMinimumGapMs) {
      out.push_back({AccessibilityEvent::Kind::ValueChanged, focused});
      announcedValue_ = node.info.value;
      announcedNumber_ = node.info.currentValue;
      valuePending_ = false;
      lastValueMs_ = nowMs;
    }
    return out;
  }

  /** Forget everything announced: the next update is a fresh baseline. */
  void reset() { *this = AccessibilityAnnouncer(); }

private:
  struct Shape {
    const Component* component;
    int parent;
    AccessibleRole role;
    long x, y, w, h;
    bool operator==(const Shape& o) const {
      return component == o.component && parent == o.parent && role == o.role && x == o.x &&
             y == o.y && w == o.w && h == o.h;
    }
  };

  bool started_ = false;
  std::vector<Shape> shape_;
  bool layoutPending_ = false;
  unsigned long long lastLayoutMs_ = 0;
  const Component* announcedComponent_ = nullptr;
  AccessibleRole announcedRole_ = AccessibleRole::Unknown;
  std::string announcedValue_;
  double announcedNumber_ = 0.0;
  bool valuePending_ = false;
  unsigned long long lastValueMs_ = 0;
};

/**
 * A rectangle in a FLIPPED view's own coordinates (y down from the top, as
 * every surface in this SDK is), on a Cocoa screen (y UP from the bottom of
 * the primary display) -- given where the view's bounds are on that screen.
 *
 * `viewOnScreen` is what -[NSWindow convertRectToScreen:] gives for the view:
 * its origin is the view's BOTTOM-left corner. An element at the top of the
 * view is therefore at the top of that rectangle, not at its origin, and a
 * bridge that forgot the flip puts VoiceOver's highlight on the mirror-image
 * control.
 */
inline Rect flippedViewRectToScreen(const Rect& viewOnScreen, const Rect& inView) {
  return Rect(viewOnScreen.x + inView.x, viewOnScreen.y + viewOnScreen.h - (inView.y + inView.h),
              inView.w, inView.h);
}

/** The inverse, for a point: a Cocoa screen point into the flipped view. */
inline Point screenPointToFlippedView(const Rect& viewOnScreen, Point onScreen) {
  return {onScreen.x - viewOnScreen.x, viewOnScreen.y + viewOnScreen.h - onScreen.y};
}

} // namespace detail

} // namespace gfx
} // namespace sonore
