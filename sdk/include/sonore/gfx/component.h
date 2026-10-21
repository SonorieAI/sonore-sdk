// SPDX-License-Identifier: Apache-2.0
//
// Component: the tree, and the mouse.
//
// ── What turns a drawing into an interface ──────────────────────────────────
//
// The graphics stack can draw a knob. A component tree is what makes that
// knob a THING: something with bounds, that knows when it was clicked, that
// repaints only itself, and that can be put inside a panel which can be put
// inside a window.
//
// ── The three rules that are easy to get wrong ──────────────────────────────
//
// MOUSE CAPTURE. After a button goes down on a component, every drag and the
// release belong to THAT component until the button comes up -- even when the
// pointer has left its bounds entirely. Without it, dragging a slider fast
// hands the second half of the gesture to whatever is next door, and the
// slider sticks. It is the single most important rule here and the one that
// looks unnecessary until a user drags quickly.
//
// Z-ORDER. Children paint in order, so the last is on top; hit testing walks
// them in REVERSE, so the topmost is asked first. Getting those the same way
// round means clicks land on whatever is underneath.
//
// DAMAGE IS IN ROOT COORDINATES. A component asking to be repainted knows its
// own coordinates and nothing about where it sits on screen. Converting at
// the point of request means the root can union damage from anywhere in the
// tree without walking it.
//
// ── Ownership ───────────────────────────────────────────────────────────────
//
// A component does NOT own its children, which is the usual arrangement and
// the right one. A plugin editor
// holds its widgets as members and adds them; the tree is a view of who is
// inside whom, not a lifetime. Owning them would mean every editor allocated
// its controls, which is the wrong default for something built once and
// painted for hours.
#pragma once

#include <functional>

#include "accessible_info.h"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "component_listeners.h"
#include "graphics.h"
#include "region.h"

namespace sonore {
namespace gfx {

class Component;

/** Which kind of pointer an event came from. A mouse unless a peer that can
 *  tell (Win32 WM_POINTER, Cocoa's tablet events) says otherwise. */
enum class MouseInputSourceType { Mouse, /**< or not reported */ Touch, /**< a finger */ Pen /**< a stylus */ };

/**
 * What a stylus reports beyond where it is.
 *
 * Normalised so a control never sees a platform's units: Windows reports tilt
 * in degrees and rotation in whole degrees, Cocoa reports tilt as -1..1 and
 * rotation in degrees counter-clockwise. A brush that scaled its width by
 * tiltX would otherwise need three formulas.
 */
struct PenDetails {
  /** Barrel rotation in radians, 0..2pi clockwise. 0 from a pen that cannot
   *  tell, which is most of them. */
  float rotation = 0.0f;
  /** -1..1: -1 leaning fully left, 1 fully right; 0 upright or unreported. */
  float tiltX = 0.0f;
  /** -1..1: -1 leaning fully away from the user, 1 fully towards. */
  float tiltY = 0.0f;
  /** The pen's other end, or a pen switched to its eraser. */
  bool eraser = false;
  /** The button on the barrel is held. */
  bool barrelButton = false;
};

/**
 * Where the events a peer is about to deliver came from.
 *
 * Set on the router by a peer that knows -- the Win32 pointer handler, the
 * Cocoa tablet path -- immediately before it calls mouseDown/Move/Up, and put
 * back to a plain mouse after. Every peer that knows nothing leaves it alone,
 * so every event that has ever been delivered is still a mouse event with no
 * pressure, exactly as before.
 */
struct PointerInput {
  /** Mouse, finger or pen. Mouse unless the peer can tell otherwise. */
  MouseInputSourceType type = MouseInputSourceType::Mouse;
  /** Which finger or pen: 0 for the mouse, the touch's slot for a finger. */
  int index = 0;
  /** 0..1, or negative when the source reports none (a mouse). */
  float pressure = -1.0f;
  /** A touch contact's ellipse orientation, radians; 0 unreported. */
  float orientation = 0.0f;
  /** Tilt, rotation and buttons when `type` is Pen; all zero otherwise. */
  PenDetails pen;
};

/**
 * One pointer event, as the component receiving it sees it.
 *
 * Built by the MouseRouter for each delivery, so every position is already in
 * the receiver's own logical coordinates (the Backing divided out the display
 * scale on the way in), alongside the root position and where the button went
 * down. A plain value: a handler that wants it later keeps a copy, and
 * `eventComponent` in that copy is a raw pointer nothing keeps alive. Mirrors
 * the reference's MouseEvent, with the pen and touch fields flattened in.
 */
struct MouseEvent {
  /** Where the pointer is, in the RECEIVING component's coordinates. */
  Point position;
  /** Where it is in the root's, for a component that needs to know how far a
   *  drag has travelled across the whole window. */
  Point rootPosition;
  /** Where the button went down, in the receiving component's coordinates.
   *  A slider computes its value from the distance between this and
   *  `position`; recomputing from the last move instead accumulates rounding
   *  and drifts over a long drag. */
  Point downPosition;
  /** 2 for the second press of a double click, else 1: no peer counts higher.
   *  Meaningful in mouseDown and contextMenu; a move reports 0, a drag or a
   *  release 1. */
  int clickCount = 1;
  /** Shift was held when the event happened. */
  bool shiftDown = false;
  /** Control was held. On macOS this is the Control key itself, not Command
   *  (a KeyPress, unlike a mouse event, folds Command into ctrlDown). */
  bool ctrlDown = false;
  /** Alt was held (Option on macOS). */
  bool altDown = false;
  /** The RIGHT button. Delivered to contextMenu() and never to mouseDown, so
   *  no control has to remember to check it before starting a drag. */
  bool rightButton = false;

  // ── Which pointer, and what it reported ──────────────────────────────────
  //
  // Added after the fields above and defaulted to "a mouse with no pressure",
  // so code that has never heard of a pen reads every event exactly as it
  // did. A drawing control asks isPressureValid() before trusting pressure.

  /** Mouse, finger or pen, copied from the router's PointerInput. */
  MouseInputSourceType sourceType = MouseInputSourceType::Mouse;
  /** The finger's or pen's slot; 0 for the mouse. */
  int sourceIndex = 0;
  /** 0..1 from a pen or a pressure-sensing touch; negative from a mouse. */
  float pressure = -1.0f;
  /** A touch contact's ellipse orientation, radians. */
  float orientation = 0.0f;
  /** Tilt, rotation and buttons when sourceType is Pen; all zero otherwise. */
  PenDetails pen;
  /** The component this event was delivered to. What a MouseListener
   *  attached to an ANCESTOR needs to know: which of its descendants it was. */
  Component* eventComponent = nullptr;

  /** Whether `pressure` holds a reading: false for a mouse, which reports none. */
  bool isPressureValid() const { return pressure >= 0.0f; }
  /** The event came from a finger on a touch screen. */
  bool isFromTouch() const { return sourceType == MouseInputSourceType::Touch; }
  /** The event came from a stylus, its tip or its eraser. */
  bool isFromPen() const { return sourceType == MouseInputSourceType::Pen; }

  /** How far the pointer is from where the button went down, in the receiver's
   *  logical units: positive to the right. Only meaningful during a press. */
  float dragDistanceX() const { return position.x - downPosition.x; }
  /** The same vertically: positive downwards, as y grows down the screen. */
  float dragDistanceY() const { return position.y - downPosition.y; }
};

/**
 * A key, as a component sees it.
 *
 * One struct for both halves of what every platform reports separately: a
 * KEY (an arrow, Return, Tab -- things with no character) and a CHARACTER
 * (what the user actually typed, after the keyboard layout, dead keys and any
 * input method have had their say).
 *
 * Both are here because a text field needs both and cannot get them from one
 * another: `character` is 0 for an arrow key, and `keyCode` cannot tell an
 * apostrophe from whatever the layout puts on that physical key. Windows sends
 * WM_KEYDOWN and WM_CHAR, X11 has XLookupString, Cocoa has charactersIgnoring-
 * Modifiers -- three shapes, one struct.
 */
struct KeyPress {
  /** Named keys, chosen to be outside Unicode so they can never collide with a
   *  real character. */
  enum Code : int {
    None = 0,  ///< not a named key: the key typed a character, which is in `character`
    Backspace = 0x10000,  ///< deletes backwards; the Mac's Delete key (top right) arrives as this
    Tab,  ///< Tab; a Tab no component takes moves the keyboard focus
    Return,  ///< Return and the keypad's Enter
    Escape,  ///< Escape
    Delete,  ///< forward delete (fn+Delete on a Mac laptop)
    Left,  ///< the left arrow
    Right,  ///< the right arrow
    Up,  ///< the up arrow
    Down,  ///< the down arrow
    Home,  ///< Home
    End,  ///< End
    PageUp,  ///< Page Up
    PageDown,  ///< Page Down
    // The function keys, consecutive so a peer maps a whole platform range in
    // one subtraction. They arrived with the menu bar, whose F10 is the one
    // keyboard route into a menu every desktop shares, and with the key
    // mapping editor, which could not otherwise be asked for F5.
    F1,  ///< F1; F2 to F12 follow it in order
    F2,
    F3,
    F4,
    F5,
    F6,
    F7,
    F8,
    F9,
    F10,
    F11,
    F12,
  };

  /** A KeyPress::Code for a named key, or None when the key typed a character
   *  instead. */
  int keyCode = None;
  /** The typed character as a Unicode code point, or 0 for a key that types
   *  nothing. NOT a byte: a plugin used outside an English keyboard layout
   *  gets accented characters here, and truncating to char would mangle them. */
  uint32_t character = 0;
  /** Shift was held. For a typed character the layout has already applied it:
   *  Shift+a arrives as 'A' with shiftDown set. */
  bool shiftDown = false;
  /** Control, or Command on macOS, was held: one test covers Ctrl+A and Cmd+A.
   *  A character typed with Ctrl or Alt arrives as the key's own letter in
   *  lower case ('a', not the control code 0x01). On Windows a printable
   *  character typed with AltGr has neither ctrlDown nor altDown set. */
  bool ctrlDown = false;
  /** Alt, or Option on macOS, was held. */
  bool altDown = false;

  /** Whether this key typed a character rather than being a named key. */
  bool isCharacter() const { return character != 0; }
  /** Whether this is the named key `code`, a KeyPress::Code. */
  bool is(int code) const { return keyCode == code; }
};

class Component;
class MouseRouter;
class Widget;
class TextInputTarget;

/**
 * Every router alive in this process.
 *
 * A router holds raw pointers to the components it is focused on, hovering
 * over, capturing and dragging. Nothing told it when one of those was
 * destroyed, so a plugin removing a focused control left the router writing
 * into freed memory on the next key press -- which AddressSanitizer caught as a
 * stack-use-after-scope, and which on a real heap is silent until it is not.
 *
 * forget() existed and had to be called by hand, which is a contract nobody
 * remembers. This makes it automatic: a Component's destructor tells every
 * router, and there are one or two of them per process -- one per open window.
 * The walk is over a list that short.
 */
inline std::vector<MouseRouter*>& liveRouters() {
  static std::vector<MouseRouter*> routers;
  return routers;
}

/**
 * What the pointer should look like over a component.
 *
 * A small closed set rather than an arbitrary image, because every one of these
 * exists on all three platforms under a different name and a custom cursor
 * would need a bitmap, a hotspot and three ways to install it -- for a plugin
 * whose controls are knobs and sliders.
 *
 * The resize grip written before this shipped with an ordinary arrow over it,
 * which is the clearest possible way to tell somebody a control does nothing.
 */
enum class MouseCursor {
  Default,         ///< the arrow; on a component, "inherit the parent's"
  Pointing,        ///< a hand: this is clickable
  DragVertical,    ///< a knob or a vertical fader
  DragHorizontal,  ///< a horizontal slider
  ResizeCorner,    ///< the diagonal, for a corner grip
  ResizeLeftRight, ///< a vertical edge
  ResizeUpDown,    ///< a horizontal edge
  Text,            ///< an I-beam, over a text field
  Wait,            ///< busy; macOS shows the arrow, having no public busy cursor
};

/**
 * What is being dragged.
 *
 * A string description and the component it came from, which between them are
 * enough for every drag a plugin actually does: "modulator 3" onto a knob, a
 * preset onto a slot, a sample onto a pad. A typed payload would need the
 * container to know every type a plugin might invent.
 */
struct DragSource {
  /** What is being dragged, in words the source and its targets agree on
   *  ("modulator 3"); when `isText` is set, the dragged text itself. */
  std::string description;
  /** The component the drag started from. Not owned; never offered its own
   *  drag as a target, and destroying it ends the drag. */
  Component* source = nullptr;
  /** Where the drag started, in root coordinates. A target that wants to know
   *  which half of itself was aimed at gets the CURRENT position in the event;
   *  this is for a source that needs to undo a move. */
  Point origin;
  /** The description IS text the user is dragging -- a selection out of a
   *  field -- rather than a name a plugin made up. A TextDragAndDropTarget
   *  is offered only these. */
  bool isText = false;
  /** Where the pointer is now, in root coordinates, kept up to date by the
   *  router. What lets a text target be told where the drag ENTERED it,
   *  which itemDragEnter has no argument for. */
  Point position;
};

/**
 * A node of the interface tree: something with bounds that paints itself,
 * holds children, and is told about the mouse, the keyboard and drags.
 *
 * Mirrors the reference's Component. A control subclasses it and overrides
 * the hooks it needs -- paint() and resized() first, then the mouse, key,
 * focus and drag callbacks -- and calls repaint() when what it shows changes.
 * Bounds are in the parent's coordinates and in LOGICAL units; nothing in the
 * tree knows the display's scale. The rules for capture, z-order and damage,
 * and why children are not owned, are in the file comment.
 *
 * Lifetime: a parent holds raw pointers to children it does not own. The
 * link is undone by whichever side is destroyed first, and a dying component
 * is forgotten by every MouseRouter, so deleting a focused or captured control
 * is safe; what is not safe is using a child after its owner destroyed it. A
 * component is a node, not a value: it cannot be copied or moved.
 *
 * Message thread only. Nothing here is synchronised; a value that changes on
 * the audio thread reaches a component through a timer or an async callback,
 * never by calling into the tree.
 */
class Component {
public:
  /** This component as a Widget, or null. What dynamic_cast<Widget*> asked,
   *  without needing RTTI: a project built with it off (/GR-, -fno-rtti)
   *  compiled the cast with a warning and then threw from it at run time --
   *  the native editor's first look-and-feel lookup was an access violation. */
  virtual Widget* asWidget() { return nullptr; }

  /**
   * Detaches this component from everything that points at it, in this order:
   * its ComponentListeners hear componentBeingDeleted, its listeners are
   * dropped, every MouseRouter forgets it (so destroying a focused, hovered or
   * captured component is safe), and it leaves its parent. Its children are not
   * destroyed -- they are not owned -- and are left with no parent.
   */
  virtual ~Component() {
    // Nothing may attach to a component that is going: the rest of this
    // destructor detaches it from its parent, and that notifies listeners
    // elsewhere which may try to (re)watch it -- a positioner whose component
    // is a child of this one does exactly that. Attached now, it would never
    // be detached, and would later unhook itself from freed memory.
    dying_ = true;
    // Listeners first, while the tree around this component is still intact
    // -- a watcher asking "where was it" gets a real answer. Then every watch
    // on this component is marked, so a caller iterating listeners or
    // delivering an event stops touching it.
    if (!componentListeners_.empty()) {
      const std::vector<ComponentListener*> told = componentListeners_;
      for (ComponentListener* l : told)
        if (std::find(componentListeners_.begin(), componentListeners_.end(), l) !=
            componentListeners_.end())
          l->componentBeingDeleted(*this);
      for (ComponentListener* l : componentListeners_) eraseFrom(l->watching_, this);
      componentListeners_.clear();
    }
    for (const auto& entry : mouseListeners_) {
      eraseFrom(entry.first->watching_, this);
      --detail::mouseListenerAttachments();
    }
    mouseListeners_.clear();
    detail::DeletionWatch::componentDying(this);
    // EVERY router is told, before the tree is touched. A router holds raw
    // pointers to what it has focused, hovered, captured and is dragging, and
    // until this existed nothing cleared them -- so a plugin removing a focused
    // control left the router writing into freed memory on the next key press.
    //
    // AddressSanitizer caught it as a stack-use-after-scope on a test's own
    // local; on a real heap it is silent until it is not. forget() had existed
    // all along and had to be called by hand, which is a contract nobody
    // remembers to keep.
    forgetEverywhere(this);
    if (parent_) parent_->removeChild(this);
    for (Component* c : children_) c->parent_ = nullptr;
  }

  /** Empty bounds at (0,0), visible, hit by clicks, not focusable, no parent. */
  Component() = default;
  /**
   * Not copyable or movable. A component is a NODE: its parent lists its
   * address and every router may hold it. Copying or moving one would
   * duplicate that linkage into an object the tree has never heard of, and
   * assigning over a live one overwrote parent_ with null while the parent
   * still listed it -- so the destructor skipped its own removal and the
   * parent later wrote into a dead object. UBSan found that in the
   * drag-and-drop test (`left = DropZone{}`); deleting the operations turns a
   * silent dangling pointer into a compile error.
   */
  Component(const Component&) = delete;
  Component& operator=(const Component&) = delete;  ///< Deleted: see the copy constructor.
  Component(Component&&) = delete;                  ///< Deleted: see the copy constructor.
  Component& operator=(Component&&) = delete;       ///< Deleted: see the copy constructor.

  // ── Geometry ─────────────────────────────────────────────────────────────

  /** In the parent's coordinates. */
  const Rect& bounds() const { return bounds_; }

  /** Origin at zero: what paint() and mouse positions are in. */
  Rect localBounds() const { return {0.0f, 0.0f, bounds_.w, bounds_.h}; }

  /**
   * Move and resize, in the parent's coordinates and logical units.
   *
   * Does nothing when the rectangle is unchanged. Otherwise the old and the new
   * area are both repainted, resized() runs if the SIZE changed (not for a move
   * alone), and ComponentListeners hear componentMovedOrResized. A NaN becomes
   * 0 and anything beyond 2^24 either way is clamped, so a layout that divided
   * by zero cannot produce bounds that are undefined to convert to int.
   */
  void setBounds(const Rect& given) {
    // What a layout computed when it went wrong -- NaN from a ratio of two
    // empty sizes, 1e30 from a division by a zero count -- made sane here,
    // once, rather than at each of the places a widget turns its size into
    // a row count or a pixel index: converting either to an int is undefined
    // behaviour. A NaN becomes 0; anything else is held within 2^24, past
    // any screen and still a whole number in a float.
    auto sane = [](float v) {
      const float limit = 16777216.0f;
      return v > -limit ? (v < limit ? v : limit) : (v == v ? -limit : 0.0f);
    };
    const Rect r(sane(given.x), sane(given.y), sane(given.w), sane(given.h));
    const bool sizeChanged = r.w != bounds_.w || r.h != bounds_.h;
    const bool moved = r.x != bounds_.x || r.y != bounds_.y;
    if (!moved && !sizeChanged) return;
    // BOTH rectangles are damaged. Repainting only the new one leaves the
    // old position on screen, which is the classic smear a moved control
    // leaves behind.
    //
    // As damage, not as repaint(): a move does not change what this
    // component LOOKS like, so its own cached image stays valid and only the
    // caches above it -- which have it at the old place -- are invalidated.
    // A resize changes the cache's size, which invalidates it by itself.
    moveDamage();
    bounds_ = r;
    moveDamage();
    // Watched across resized(), which is layout code and may delete this --
    // the listeners are told only about a component that still exists.
    detail::DeletionWatch alive(this);
    if (sizeChanged) resized();
    if (!alive.deleted && !componentListeners_.empty())
      notifyComponentListeners(alive, [&](ComponentListener& l) {
        l.componentMovedOrResized(*this, moved, sizeChanged);
      });
  }

  /** setBounds with the size changed and the position kept. */
  void setSize(float w, float h) { setBounds({bounds_.x, bounds_.y, w, h}); }

  // ── The tree ─────────────────────────────────────────────────────────────

  /** The component this one was added to, or null for a root or a component
   *  not in a tree. Not owned. */
  Component* parent() const { return parent_; }
  /** How many direct children this component has. */
  int numChildren() const { return (int) children_.size(); }
  /** For a walk over the tree -- focus traversal is the first caller.
   *  Const so nothing outside can reorder what paint and hit-test depend on. */
  const std::vector<Component*>& children() const { return children_; }
  /** The direct child at `index` in paint order (0 is drawn first, at the
   *  back), or null when `index` is out of range. */
  Component* child(int index) const {
    return (index >= 0 && index < (int) children_.size()) ? children_[(size_t) index] : nullptr;
  }

  /** Added on TOP of the existing children. Not owned. */
  void addChild(Component* c) {
    if (!c || c == this || c->parent_ == this) return;
    if (c->parent_) c->parent_->removeChild(c);
    c->parent_ = this;
    children_.push_back(c);
    c->repaint();
    hierarchyChanged(c);
  }

  /**
   * Send a child to the back of the z-order.
   *
   * Painting walks the list forward and hit-testing walks it backward, so
   * "first" means drawn first AND hit last -- which is exactly what a
   * viewport's content needs to be relative to its scroll bars.
   */
  void moveChildToBack(Component* c) {
    auto it = std::find(children_.begin(), children_.end(), c);
    if (it == children_.end() || it == children_.begin()) return;
    children_.erase(it);
    children_.insert(children_.begin(), c);
  }

  /** Declared here and defined after MouseRouter, which needs to be complete
   *  to be told. */
  static void forgetEverywhere(Component* c);

  /** Every router whose keyboard focus is `c` or inside it lets it go --
   *  through setFocus, so focusLost runs. Defined after MouseRouter too. */
  static void releaseFocusFrom(Component* c);

  /**
   * Take `c` out of this component's children, without destroying it. Does
   * nothing if `c` is not a direct child.
   *
   * Any capture, hover, keyboard focus or drag target inside `c` is dropped
   * first, so a gesture in progress on it simply ends -- it gets no mouseUp and
   * no focusLost. Its area is repainted and ComponentListeners on both sides
   * hear the hierarchy change.
   */
  void removeChild(Component* c) {
    auto it = std::find(children_.begin(), children_.end(), c);
    if (it == children_.end()) return;
    c->repaint();
    // Drop any router state -- capture, hover, focus, drag target -- pointing
    // ANYWHERE inside the detached subtree, not just at c. The destructor does
    // this via forgetEverywhere; a live removal must too, or a component taken
    // out of the tree mid-drag keeps receiving the gesture with parent_ null,
    // so rootToLocal returns identity coordinates and the control emits a wild
    // value to the host as automation. addChild's steal path routes through
    // here, so re-parenting mid-gesture cleanly ENDS it rather than corrupting
    // it. F8 from the router audit.
    forgetSubtree(c);
    (*it)->parent_ = nullptr;
    children_.erase(it);
    hierarchyChanged(c);
  }

  /** forgetEverywhere for a whole subtree -- forget(x) matches an exact
   *  pointer, so a captured or focused DESCENDANT of a removed component would
   *  be missed by forgetting only the component itself. */
  static void forgetSubtree(Component* c) {
    if (!c) return;
    forgetEverywhere(c);
    for (Component* child : c->children_) forgetSubtree(child);
  }

  /** To the top of its parent's order. */
  void toFront() {
    if (!parent_) return;
    auto& siblings = parent_->children_;
    auto it = std::find(siblings.begin(), siblings.end(), this);
    if (it == siblings.end() || it + 1 == siblings.end()) return;
    siblings.erase(it);
    siblings.push_back(this);
    repaint();
  }

  /** The topmost ancestor, or this component when it has no parent: where
   *  repaint() collects damage and what a MouseRouter dispatches into. */
  Component* root() {
    Component* c = this;
    while (c->parent_) c = c->parent_;
    return c;
  }

  /** A name for finding this component from data -- what a relative
   *  position's "otherComp.right" and a ComponentBuilder's "id" refer to.
   *  Unrelated to the accessible name, which is for people. */
  void setComponentID(std::string id) { componentID_ = std::move(id); }
  /** The ID setComponentID set; empty by default. */
  const std::string& componentID() const { return componentID_; }

  /** The first direct child with this ID, or null. */
  Component* findChildWithID(const std::string& id) const {
    for (Component* c : children_)
      if (c->componentID_ == id) return c;
    return nullptr;
  }

  // ── Visibility ───────────────────────────────────────────────────────────

  /** This component's own flag, true by default. Says nothing about its
   *  ancestors: isShowing() asks whether it is actually on screen. */
  bool isVisible() const { return visible_; }

  /**
   * Show or hide this component and everything inside it. A hidden component
   * is not painted and not hit by the mouse.
   *
   * Hiding repaints the area it covered and takes the keyboard focus from
   * anything inside it, through focusLost -- where a text field commits what
   * was typed. ComponentListeners hear componentVisibilityChanged either way.
   */
  void setVisible(bool shouldBeVisible) {
    if (visible_ == shouldBeVisible) return;
    // Hiding damages the area it USED to cover, so the damage is marked while
    // the component is still showing: repaint() does nothing for one that is
    // not. It used to drop the flag first, so hiding marked no damage at all
    // and the hidden control stayed drawn until something else repainted
    // over it -- a scroll bar left on screen after its section collapsed.
    if (!shouldBeVisible) repaint();
    visible_ = shouldBeVisible;
    if (shouldBeVisible) repaint();
    detail::DeletionWatch alive(this);
    // The keyboard leaves what stops showing, as it leaves what is removed.
    // A tab page switched by a shortcut, a section closed, a toolbar item
    // folded into the overflow: the field that had the keyboard kept it, so
    // the next keys were typed into a field nobody could see and Return
    // committed it -- and on Windows the editor went on holding the OS focus
    // for it, so the host's own keys (its space bar) stopped working. Its
    // focusLost runs, which is where a field commits what was typed.
    if (!shouldBeVisible) releaseFocusFrom(this);
    if (!alive.deleted && !componentListeners_.empty())
      notifyComponentListeners(alive,
                               [&](ComponentListener& l) { l.componentVisibilityChanged(*this); });
  }

  /** Visible AND every ancestor visible. A component inside a hidden panel is
   *  not on screen however visible it believes itself to be, and hit testing
   *  that used the flag alone would deliver clicks to it. */
  bool isShowing() const {
    for (const Component* c = this; c; c = c->parent_)
      if (!c->visible_) return false;
    return true;
  }

  /** Whether the mouse can land on this at all. A label or a decorative
   *  panel sets this false so clicks fall through to whatever is behind. */
  void setInterceptsMouse(bool shouldIntercept) { interceptsMouse_ = shouldIntercept; }
  /** True by default. False lets clicks through to what is behind, while the
   *  children of this component are still hit. */
  bool interceptsMouse() const { return interceptsMouse_; }

  // ── Coordinates ──────────────────────────────────────────────────────────

  /** A point in this component's coordinates, in its parent's. */
  Point localToParent(Point p) const { return {p.x + bounds_.x, p.y + bounds_.y}; }

  /** A point in this component's coordinates, in the root's: the coordinates
   *  of the damage list and of what a peer hands the MouseRouter. */
  Point localToRoot(Point p) const {
    for (const Component* c = this; c->parent_; c = c->parent_) p = c->localToParent(p);
    return p;
  }

  /** A point in the root's coordinates, in this component's. Allocates a
   *  short vector for the walk, so it is not for a per-pixel loop. */
  Point rootToLocal(Point p) const {
    // Down the chain of ancestors, subtracting each. Collected first because
    // the walk is naturally upward and the subtraction has to happen from the
    // top down.
    std::vector<const Component*> chain;
    for (const Component* c = this; c->parent_; c = c->parent_) chain.push_back(c);
    for (auto it = chain.rbegin(); it != chain.rend(); ++it)
      p = {p.x - (*it)->bounds_.x, p.y - (*it)->bounds_.y};
    return p;
  }

  // ── Hit testing ──────────────────────────────────────────────────────────

  /**
   * The deepest component at a point given in THIS component's coordinates,
   * or null.
   *
   * Children in reverse order, because the last one painted is on top and
   * therefore the one a user believes they clicked.
   */
  Component* hitTest(Point p) {
    if (!visible_ || !localBounds().contains(p)) return nullptr;
    for (auto it = children_.rbegin(); it != children_.rend(); ++it) {
      Component* c = *it;
      Component* hit = c->hitTest({p.x - c->bounds_.x, p.y - c->bounds_.y});
      if (hit) return hit;
    }
    return (interceptsMouse_ && hitTestPoint(p)) ? this : nullptr;
  }

  /**
   * Is this point REALLY on this component?
   *
   * The default is yes everywhere inside the bounds, which is what a rectangle
   * means. Overriding it is how a component takes a shape: a resize border that
   * is transparent in the middle so the controls underneath still work, a round
   * knob that does not swallow the clicks in the square corners around it.
   *
   * Children are unaffected -- a child inside an ignored area is still hit,
   * because this answers for THIS component's own surface and not for the
   * region. Without that, an override would silently disable a subtree.
   */
  virtual bool hitTestPoint(Point) const { return true; }

  // ── Painting ─────────────────────────────────────────────────────────────

  /**
   * Draw this component -- itself only; its children paint after it, on top.
   *
   * `g` arrives in this component's own coordinates and logical units: (0,0) is
   * the top-left of localBounds() and the display scale is already applied.
   * The clip is at most the component's bounds and often only the damaged part
   * of them, so paint() may be called for a fragment, more than once a frame:
   * draw everything from the current state each time rather than relying on
   * what is on screen. Changes to `g`'s state are undone after the call. The
   * default draws nothing.
   */
  virtual void paint(Graphics&) {}
  /** Called by setBounds after the size changed, with bounds() already new:
   *  where a component lays out its children. Not called for a move alone; the
   *  first setBounds that gives a size calls it. The default does nothing. */
  virtual void resized() {}

  /**
   * Paint this component and everything inside it -- all of it.
   *
   * Each child is translated into its own coordinates and CLIPPED to its
   * bounds, so a component that draws outside itself is trimmed rather than
   * scribbling on its siblings. That clip is why a component can be written
   * without knowing what is next to it.
   *
   * The two-argument form below paints only what is damaged; this one paints
   * the lot, which is what a fresh window and a test both want.
   */
  void paintTree(Graphics& g) {
    if (!visible_ || bounds_.isEmpty()) return;

    // Clipped to THIS component before anything is drawn, children included.
    //
    // It used to clip only for its own paint() and then clip each child to the
    // CHILD's bounds -- which constrains nothing when the child is bigger than
    // the parent or sits outside it. Every child in this SDK fitted inside its
    // parent, so the tree looked correct for as long as nothing needed
    // otherwise; the first Viewport painted four hundred pixels of content
    // through a fifty-pixel window and seven thousand pixels landed outside it.
    //
    // A component tree whose containment is a convention rather than a rule is
    // one where clipping works until somebody needs it.
    Graphics::ScopedState scope(g);
    g.clipTo(localBounds());
    if (g.isClippedOut()) return;

    // A cached image stands in for this component AND everything inside it.
    if (cachedImage_) cachedImage_->paint(g);
    else paintContents(g, nullptr);
    if (!parent_)
      if (ComponentPaintObserver* o = detail::paintObserver()) o->didPaintTree(*this, g);
  }

  /**
   * The whole subtree, painted, whether or not this component has a cached
   * image -- what a CachedComponentImage calls to fill itself. `damage`, in
   * this component's coordinates, skips children it misses; null paints all.
   * The caller has already clipped and translated.
   */
  void paintTreeUncached(Graphics& g, const RectangleList* damage = nullptr) {
    if (!visible_ || bounds_.isEmpty()) return;
    Graphics::ScopedState scope(g);
    g.clipTo(localBounds());
    if (g.isClippedOut()) return;
    paintContents(g, damage);
  }

  /**
   * Paint only what intersects `damage`, which is in THIS component's
   * coordinates.
   *
   * A component the damage does not touch is skipped entirely -- it and its
   * whole subtree. That is the saving: an editor where one knob moved visits
   * one knob rather than forty controls, and rasterises the pixels under it
   * rather than the window.
   *
   * The clip is still what actually prevents drawing outside; this only avoids
   * the work of drawing things that would be clipped away. Both are needed:
   * without the clip it would be wrong, and without this it would be slow.
   */
  void paintTree(Graphics& g, const RectangleList& damage) {
    if (!visible_ || bounds_.isEmpty()) return;
    if (damage.isEmpty()) return;
    if (!damage.intersects(localBounds())) return;

    Graphics::ScopedState scope(g);
    g.clipTo(localBounds());
    if (g.isClippedOut()) return;

    if (cachedImage_) cachedImage_->paint(g);
    else paintContents(g, &damage);
    if (!parent_)
      if (ComponentPaintObserver* o = detail::paintObserver()) o->didPaintTree(*this, g);
  }

  // ── Damage ───────────────────────────────────────────────────────────────

  /** Mark all of this component to be painted again. Nothing is drawn now: the
   *  damage is collected on the root and the peer paints it on its next frame.
   *  For a component that is not showing, only cached images above it are
   *  invalidated. */
  void repaint() { repaint(localBounds()); }

  /**
   * Marked on the ROOT, in root coordinates, so damage from anywhere in the
   * tree collects in one place without a walk.
   *
   * A LIST rather than one united rectangle. Two knobs at opposite corners of
   * an editor moving at once used to produce a damaged area covering everything
   * between them, which for a wide editor is the whole thing -- and the peers
   * then redrew all of it, thirty times a second, in a process that is also
   * running an audio callback.
   */
  void repaint(const Rect& area) {
    // Cached images are invalidated BEFORE the showing test, not after: a
    // label changing inside a hidden, buffered panel marks no damage -- there
    // is nothing on screen to mend -- but the panel's cache now holds the old
    // text, and showing the panel later would blit it. Walking up costs the
    // depth of the tree and a pointer test per level.
    invalidateCachesFrom(this, area);
    if (!isShowing()) return;
    const Point topLeft = localToRoot({area.x, area.y});
    root()->damage_.add({topLeft.x, topLeft.y, area.w, area.h});
  }

  // ── Cached rendering ─────────────────────────────────────────────────────

  /**
   * Keep this component's rendering -- itself and everything inside it -- in
   * a bitmap, and paint from that until something inside it repaints.
   *
   * Worth it for a subtree that is expensive to draw and rarely changes, when
   * something NEAR it changes often: a detailed panel under a moving meter,
   * a large static background. Here a paint is a software rasteriser, so the
   * saving is real -- a blit is a few memory copies where the paint may be
   * hundreds of antialiased paths.
   *
   * Not free: the cache is the component's size in device pixels, four
   * bytes each, and is re-rendered whenever the transform's scale or
   * sub-pixel offset changes. Defined in cached_image.h, which this header
   * includes at its end.
   */
  void setBufferedToImage(bool shouldBeBuffered);
  /** Whether a cache is installed, by setBufferedToImage or
   *  setCachedComponentImage. */
  bool isBufferedToImage() const { return cachedImage_ != nullptr; }

  /** Install any cache implementation, or null to remove it. Owned. */
  void setCachedComponentImage(std::unique_ptr<CachedComponentImage> image) {
    cachedImage_ = std::move(image);
    repaint();
  }
  /** The installed cache, owned by this component, or null. */
  CachedComponentImage* cachedComponentImage() const { return cachedImage_.get(); }

  // ── Listeners ────────────────────────────────────────────────────────────

  /** Not owned. Adding one twice is a no-op. Returns whether `l` is now
   *  attached -- false only for a component already being destroyed, which
   *  takes no new listeners (see ~Component). */
  bool addComponentListener(ComponentListener* l) {
    if (!l || dying_) return false;
    if (std::find(componentListeners_.begin(), componentListeners_.end(), l) !=
        componentListeners_.end())
      return true;
    componentListeners_.push_back(l);
    l->watching_.push_back(this);
    return true;
  }

  /** Detach `l`. A no-op for null or a listener not attached. Either side may
   *  also just be destroyed: both destructors detach. */
  void removeComponentListener(ComponentListener* l) {
    if (!l) return;
    const size_t before = componentListeners_.size();
    eraseFrom(componentListeners_, l);
    if (componentListeners_.size() != before) eraseFrom(l->watching_, this);
  }

  /**
   * `nestedChildren` true hears every event delivered to anything inside
   * this component as well as to it -- the reference's
   * wantsEventsForAllNestedChildComponents, and the only way to watch an
   * editor whose surface is entirely controls.
   */
  void addMouseListener(MouseListener* l, bool nestedChildren) {
    if (!l || dying_) return;
    for (auto& entry : mouseListeners_)
      if (entry.first == l) {
        entry.second = nestedChildren;
        return;
      }
    mouseListeners_.push_back({l, nestedChildren});
    l->watching_.push_back(this);
    ++detail::mouseListenerAttachments();
  }

  /** Detach `l`. A no-op for a listener not attached. Either side may also just
   *  be destroyed: both destructors detach. */
  void removeMouseListener(MouseListener* l) {
    for (size_t i = 0; i < mouseListeners_.size(); ++i)
      if (mouseListeners_[i].first == l) {
        mouseListeners_.erase(mouseListeners_.begin() + (std::ptrdiff_t) i);
        eraseFrom(l->watching_, this);
        --detail::mouseListenerAttachments();
        return;
      }
  }

  /** What the router reads: the listeners attached here, and whether each
   *  wants its descendants' events too. */
  const std::vector<std::pair<MouseListener*, bool>>& mouseListeners() const {
    return mouseListeners_;
  }

  /** Tell this component's listeners, and every descendant's, that the chain
   *  of parents above them changed. Called by addChild and removeChild; a
   *  peer calls it on the root when the window around a tree appears. */
  void sendParentHierarchyChanged() {
    detail::DeletionWatch alive(this);
    if (!componentListeners_.empty())
      notifyComponentListeners(
          alive, [&](ComponentListener& l) { l.componentParentHierarchyChanged(*this); });
    if (alive.deleted) return;
    // Over a copy: a listener may restructure the tree it is being told about.
    const std::vector<Component*> kids = children_;
    for (Component* c : kids) {
      if (alive.deleted) return;
      if (std::find(children_.begin(), children_.end(), c) != children_.end())
        c->sendParentHierarchyChanged();
    }
  }

  /** Whether anything is waiting to be repainted. Only a root collects damage,
   *  so this is asked of the root. */
  bool isDirty() const { return !damage_.isEmpty(); }
  /** The areas waiting to be repainted, in root coordinates. Filled on the root
   *  only; a peer paints them and then calls clearDamage(). */
  const RectangleList& damage() const { return damage_; }
  /** Everything damaged, as one rectangle -- for a peer that can only blit a
   *  single region. */
  Rect damageBounds() const { return damage_.bounds(); }
  /** Empty the damage list, once what it held has been painted. */
  void clearDamage() { damage_.clear(); }

  // ── Mouse delivery ───────────────────────────────────────────────────────

  /**
   * The left button went down on this component. The right button goes to
   * contextMenu() instead.
   *
   * From here until the release this component has the mouse: every move
   * arrives as mouseDrag and the release as mouseUp, wherever the pointer goes.
   * By the time this runs the click has moved the keyboard focus -- here if
   * wantsKeyboardFocus(), otherwise away from whatever held it -- and any
   * MouseListeners have heard the press. `clickCount` is 2 for a double
   * click's second press.
   */
  virtual void mouseDown(const MouseEvent&) {}

  /**
   * The right button went down on this component.
   *
   * A SEPARATE entry point rather than a flag inside mouseDown, and the reason
   * is the failure it prevents. Every control here starts a gesture in
   * mouseDown -- a slider records where the drag began and follows the pointer
   * from it. Delivering a right-click there means each control has to remember
   * to check the flag first, and the one that forgets moves the parameter the
   * user was trying to open a menu on. With a separate call there is nothing
   * to forget.
   *
   * No capture is taken and no drag begins, because a right-click is not a
   * gesture -- it is a request for a menu somebody else will show.
   */
  virtual void contextMenu(const MouseEvent&) {}

  /**
   * Where this component's text caret is, in its OWN coordinates, and how tall.
   *
   * The equivalent is conventionally a text input target. It exists for one
   * job that
   * cannot be done any other way: an input method's candidate window -- the
   * list of characters shown while composing Japanese, Chinese or Korean --
   * has to appear NEXT TO the caret, and only the component knows where that
   * is. Without it the OS places the list wherever it likes, which in practice
   * is a corner of the window with no relation to the field being typed in.
   *
   * Returns false by default: most components have no caret, and a component
   * that reported a false one would move the candidate list to a place nothing
   * is being typed.
   */
  virtual bool caretBounds(Rect*) const { return false; }

  /**
   * This component as somewhere an input method can compose text, or null.
   *
   * What asWidget() is to Widget, for gfx/text_input.h: a peer whose platform
   * hands it marked text (NSTextInputClient on macOS, XIM's preedit callbacks
   * on X11) asks the FOCUSED component for this, and only a text field says
   * anything but null. No RTTI, for the reason asWidget gives.
   */
  virtual TextInputTarget* textInputTarget() { return nullptr; }
  /** The pointer moved with the button held, after a mouseDown on this
   *  component -- wherever it is, so `position` may be outside localBounds()
   *  or negative. `downPosition` is where the press began. */
  virtual void mouseDrag(const MouseEvent&) {}
  /**
   * The button came up after a mouseDown on this component, wherever the
   * pointer is: a button tests `localBounds().contains(e.position)` before it
   * counts a click. When the window closes mid-press the release arrives at a
   * point just outside the component, so a button does not fire and a gesture
   * still ends. A component removed from the tree mid-press gets none.
   */
  virtual void mouseUp(const MouseEvent&) {}
  /** The pointer moved over this component with no button held. A move with a
   *  button held is a mouseDrag, to the component that was pressed. */
  virtual void mouseMove(const MouseEvent&) {}
  /** The pointer came over this component. Hover is the single deepest
   *  component under the pointer, so moving onto a child is an exit from its
   *  parent. Hover does not change during a press; it is worked out again when
   *  the button comes up. */
  virtual void mouseEnter(const MouseEvent&) {}
  /** The pointer left this component -- for another one, or out of the window,
   *  in which case the event's positions are not meaningful. */
  virtual void mouseExit(const MouseEvent&) {}
  /**
   * Returning false sends the wheel UP to the parent.
   *
   * A viewport scrolls on behalf of whatever is inside it, and what is inside
   * it is usually a plain component with no opinion about wheels -- so without
   * the walk, a wheel over the content does nothing and only a wheel exactly
   * over the scroll bar works. That is a wheel nobody uses.
   *
   * Same shape as keyPressed, for the same reason.
   */
  virtual bool mouseWheel(const MouseEvent&, float delta) {
    (void) delta;
    return false;
  }

  /**
   * Could a wheel event move THIS component right now?
   *
   * Only a scrolling container answers yes, and only while it actually has
   * somewhere to go -- a Viewport already refuses the wheel when its content
   * fits, on the grounds that a bar with nothing to scroll should not swallow
   * an event something around it could use.
   *
   * This is the same rule asked from the other direction, so a control can
   * find out whether the wheel is wanted somewhere above it.
   */
  virtual bool wouldScroll() const { return false; }

  /**
   * Is there anything ABOVE this that the wheel would scroll?
   *
   * The question a knob has to ask before it eats a wheel event. Inside a
   * scrolling editor, a user reaching for a parameter further down scrolls,
   * the pointer passes over a knob, and the next notch moves that knob instead
   * of the list -- an accidental parameter change, reported to the host as
   * automation, while the user was only navigating.
   */
  bool hasScrollableAncestor() const {
    for (const Component* c = parent_; c != nullptr; c = c->parent_)
      if (c->wouldScroll()) return true;
    return false;
  }

  // ── Keyboard ─────────────────────────────────────────────────────────────

  /**
   * Whether this component can hold the keyboard focus.
   *
   * False by default, and deliberately: a knob that took focus would swallow
   * the Tab that was meant to reach the text field next to it, and a plugin
   * editor full of focusable knobs is one where Tab does nothing useful. Only
   * something that reads keys says yes.
   */
  bool wantsKeyboardFocus() const { return wantsFocus_; }
  /** Whether a click or Tab may give this component the keyboard focus.
   *  Turning it off does not take away a focus it already holds. */
  void setWantsKeyboardFocus(bool wants) { wantsFocus_ = wants; }

  /**
   * Handle a key, or say you did not.
   *
   * Returning false is not a formality: an unhandled key walks UP the tree, so
   * a text field can ignore Escape and let whatever contains it close a menu.
   * A component that returned true for everything would be a black hole for
   * every shortcut in the host.
   */
  virtual bool keyPressed(const KeyPress&) { return false; }

  /** This component took the keyboard focus, from a click, Tab or
   *  MouseRouter::setFocus. hasKeyboardFocus() is already true; the previous
   *  holder's focusLost has run; a repaint follows. */
  virtual void focusGained() {}
  /**
   * The keyboard focus left this component: for another component, for a click
   * on one that takes no focus, or because it or an ancestor was hidden. Where
   * a text field commits what was typed. hasKeyboardFocus() is already false,
   * and the handler may move the focus again or destroy this component.
   *
   * Not called when the component is removed from the tree or destroyed: the
   * focus is dropped silently then.
   */
  virtual void focusLost() {}

  /**
   * One sentence explaining this control, or empty for none.
   *
   * Virtual rather than a stored string, so a control whose meaning depends on
   * its state -- a button that says what it will do next, a slider that names
   * its current unit -- can answer with what is true now. A component that just
   * has a fixed one calls setTooltip and never overrides this.
   */
  virtual std::string tooltip() const { return tooltip_; }
  /** The fixed text tooltip() returns unless overridden; empty for none. */
  void setTooltip(std::string text) { tooltip_ = std::move(text); }

  /**
   * The cursor to show over this component.
   *
   * Virtual for the same reason tooltip() is: a control whose cursor depends on
   * where inside it the pointer is -- an edge that resizes, a body that drags
   * -- can answer with what is true now.
   */
  virtual MouseCursor cursor() const { return cursor_; }
  /** The cursor cursor() returns unless overridden. Default means "whatever
   *  the nearest ancestor with a cursor of its own shows". */
  void setCursor(MouseCursor c) { cursor_ = c; }

  // ── Drag and drop, inside the plugin ─────────────────────────────────────
  //
  // The machinery is on the router rather than in a separate container class,
  // because a drag is per-WINDOW state exactly like capture, hover and focus:
  // one thing being dragged, one target under it. The usual arrangement puts
  // it on a container the component tree has to be rooted in; here the
  // router already IS that root and already owns the other three.

  /**
   * Whether this component would accept the drag currently in flight.
   *
   * Asked on every move of the drag, of each component from the one under the
   * pointer upwards until one says yes -- so answer the same way for the same
   * drag, or the highlight flickers as enter and exit alternate. False by
   * default, so a component becomes a drop target deliberately.
   */
  virtual bool isInterestedInDrag(const DragSource& s) {
    // The defaults hand a TEXT drag to a TextDragAndDropTarget, so a field
    // that accepts dropped text needs no item-drag code at all. Anything that
    // overrides these has taken the drag over and decides for itself.
    TextDragAndDropTarget* t = textTargetFor(s);
    return t != nullptr && t->isInterestedInTextDrag(s.description);
  }

  /** An internal drag (MouseRouter::startDrag) came over this component and
   *  isInterestedInDrag said yes: the moment to light up a highlight. The
   *  default passes a text drag to asTextDragAndDropTarget(). */
  virtual void itemDragEnter(const DragSource& s) {
    if (TextDragAndDropTarget* t = textTargetFor(s)) t->textDragEnter(s.description, rootToLocal(s.position));
  }
  /** Each move while this component is the drag's target, `position` in its own
   *  coordinates. */
  virtual void itemDragMove(const DragSource& s, Point position) {
    if (TextDragAndDropTarget* t = textTargetFor(s)) t->textDragMove(s.description, position);
  }
  /** The drag left this component, was cancelled, or is about to be dropped on
   *  it -- itemDropped follows in that case, so a highlight goes off here. */
  virtual void itemDragExit(const DragSource& s) {
    if (TextDragAndDropTarget* t = textTargetFor(s)) t->textDragExit(s.description);
  }

  /** `position` is in this component's own coordinates. */
  virtual void itemDropped(const DragSource& s, Point position) {
    if (TextDragAndDropTarget* t = textTargetFor(s)) t->textDropped(s.description, position);
  }

  /** This component as a text drop target, or null -- see
   *  TextDragAndDropTarget. Override to return `this`. */
  virtual TextDragAndDropTarget* asTextDragAndDropTarget() { return nullptr; }

  // ── Files, from outside the plugin ───────────────────────────────────────

  /**
   * Whether this component would accept these files.
   *
   * Answered BEFORE the drop, because the platform asks: Windows wants to know
   * whether to show a copy cursor, and X11's XDND requires a reply before the
   * user lets go. A target that only found out at drop time would leave the
   * desktop showing "no entry" over a component that was going to accept.
   */
  virtual bool isInterestedInFileDrag(const std::vector<std::string>&) { return false; }

  /** Files dragged from outside came over this component and
   *  isInterestedInFileDrag said yes. The point is in this component's
   *  coordinates. No call follows for each move. */
  virtual void fileDragEnter(const std::vector<std::string>&, Point) {}
  /** The file drag left this component, was abandoned, or is about to be
   *  dropped on it (filesDropped follows). */
  virtual void fileDragExit() {}
  /** The files were dropped on this component; `position` is in its own
   *  coordinates. */
  virtual void filesDropped(const std::vector<std::string>& files, Point position) {
    (void) files;
    (void) position;
  }

  /** Whether this component itself holds the keyboard focus (not merely
   *  something inside it). */
  bool hasKeyboardFocus() const { return hasFocus_; }

  // ── What this component IS ───────────────────────────────────────────────
  //
  // Separate from what it looks like, and the only part of a component a
  // screen reader can use. See accessible_info.h for why the roles are a
  // closed set and what is deliberately not here yet.

  /**
   * Describe this component.
   *
   * Built on demand, not stored: the value of a knob is already in the knob,
   * and a second copy updated by a callback is a second copy that can be stale
   * at the moment somebody asks.
   *
   * The default answers Unknown with whatever name was set, which is right for
   * a bare Component -- a layout container has no role a reader should
   * announce. Controls override it.
   */
  virtual AccessibleInfo accessibleInfo() const {
    AccessibleInfo info;
    info.name = accessibleName_;
    info.description = accessibleDescription_;
    info.focusable = wantsFocus_;
    info.focused = hasFocus_;
    info.enabled = true;
    return info;
  }

  /** What this control is called. A knob whose name nobody set is announced as
   *  "slider, -6.0 dB" with no way to know WHICH slider, so the editor sets one
   *  per parameter from the same label it draws. */
  void setAccessibleName(std::string name) { accessibleName_ = std::move(name); }
  /** The name setAccessibleName set; empty by default. */
  const std::string& accessibleName() const { return accessibleName_; }

  /**
   * A reader asking this control to act: press it, step it, open its menu.
   *
   * False for "I did nothing", which a bridge passes on -- NSAccessibility's
   * perform methods and UIA's patterns both report it, and a reader told an
   * action succeeded when nothing happened announces a change that is not
   * there. Controls override this to go through their OWN key and click
   * paths, so the host sees exactly the edits it would from a keyboard.
   */
  virtual bool performAccessibleAction(AccessibleAction action) {
    (void) action;
    return false;
  }

  /** A reader setting a ranged control directly, in the range accessibleInfo
   *  reports. False when this control takes no such value. */
  virtual bool setAccessibleValue(double value) {
    (void) value;
    return false;
  }

  /** A reader typing a whole value into a field (or naming a choice). False
   *  when this control takes no text. */
  virtual bool setAccessibleText(const std::string& text) {
    (void) text;
    return false;
  }

  /** A longer explanation a reader offers after the name: what the control
   *  does, not what it is called. Empty by default. */
  void setAccessibleDescription(std::string text) { accessibleDescription_ = std::move(text); }
  /** The text setAccessibleDescription set. */
  const std::string& accessibleDescription() const { return accessibleDescription_; }

  /**
   * Leave this component out of the tree a reader walks -- but NOT its
   * children.
   *
   * The distinction is the whole reason this is separate from setVisible: a
   * layout container is not worth describing and its contents certainly are,
   * where a hidden panel's contents are not merely unannounced, they are not
   * there.
   */
  void setAccessibilityIgnored(bool ignored) { accessibilityIgnored_ = ignored; }
  /** Whether setAccessibilityIgnored(true) left this component out. */
  bool isAccessibilityIgnored() const { return accessibilityIgnored_; }

private:
  friend class MouseRouter;
  friend class ComponentListener;
  friend class MouseListener;

  template <typename T>
  static void eraseFrom(std::vector<T*>& v, const void* item) {
    v.erase(std::remove_if(v.begin(), v.end(), [item](T* x) { return (const void*) x == item; }),
            v.end());
  }

  /** Own paint, then the children -- all of them, or only those `damage`
   *  touches. The clip to this component is already in place. */
  void paintContents(Graphics& g, const RectangleList* damage) {
    {
      Graphics::ScopedState own(g);
      if (ComponentPaintObserver* o = detail::paintObserver()) {
        o->willPaint(*this, g);
        paint(g);
        o->didPaint(*this, g);
      } else {
        paint(g);
      }
    }
    for (Component* c : children_) {
      if (!damage) {
        Graphics::ScopedState childScope(g);
        g.translate(c->bounds_.x, c->bounds_.y);
        g.clipTo(c->localBounds());
        if (!g.isClippedOut()) c->paintTree(g);
        continue;
      }
      // Asked FIRST, in this component's coordinates where the damage and the
      // child's bounds both live: a child the damage misses gets nothing
      // built for it. The translated list below is a vector, and building it
      // for every child before asking cost an allocation per control per
      // frame -- 224 to repaint one control in two hundred.
      if (!c->visible_ || !damage->intersects(c->bounds_)) continue;
      // The damage moves into the CHILD's coordinates, which is a translation
      // by the child's position -- the opposite direction to the transform,
      // and getting the sign wrong here paints the right components with the
      // wrong parts of them.
      RectangleList childDamage;
      for (const Rect& r : damage->rects())
        childDamage.add({r.x - c->bounds_.x, r.y - c->bounds_.y, r.w, r.h});

      Graphics::ScopedState childScope(g);
      g.translate(c->bounds_.x, c->bounds_.y);
      g.clipTo(c->localBounds());
      if (!g.isClippedOut()) c->paintTree(g, childDamage);
    }
  }

  /** `area` in `from`'s coordinates, carried up the tree into each cached
   *  ancestor's -- `from` included. */
  static void invalidateCachesFrom(Component* from, Rect area) {
    for (Component* c = from; c != nullptr; c = c->parent_) {
      if (c->cachedImage_) c->cachedImage_->invalidate(area);
      area.x += c->bounds_.x;
      area.y += c->bounds_.y;
    }
  }

  /** The damage a move leaves: marked like repaint(), but only the caches
   *  ABOVE this one are stale -- its own picture has not changed. */
  void moveDamage() {
    if (parent_) invalidateCachesFrom(parent_, bounds_);
    if (!isShowing()) return;
    const Point topLeft = localToRoot({0.0f, 0.0f});
    root()->damage_.add({topLeft.x, topLeft.y, bounds_.w, bounds_.h});
  }

  /** The text target a drag should go to, if it is a text drag. */
  TextDragAndDropTarget* textTargetFor(const DragSource& s) {
    return s.isText ? asTextDragAndDropTarget() : nullptr;
  }

  /**
   * Call `fn` for each listener, stopping if the component is destroyed.
   *
   * Over a snapshot, each one checked against the LIVE list before it is
   * called: a listener that removes another -- or itself -- is the ordinary
   * case, and walking the vector it edits is how a callback ends up called
   * on something already gone.
   */
  template <typename Fn>
  void notifyComponentListeners(const detail::DeletionWatch& alive, Fn&& fn) {
    const std::vector<ComponentListener*> snapshot = componentListeners_;
    for (ComponentListener* l : snapshot) {
      if (alive.deleted) return;
      if (std::find(componentListeners_.begin(), componentListeners_.end(), l) ==
          componentListeners_.end())
        continue;
      fn(*l);
    }
  }

  /** After `child` was added or removed: it and everything in it hear that
   *  their parents changed, and this component hears its children did. */
  void hierarchyChanged(Component* child) {
    detail::DeletionWatch alive(this);
    child->sendParentHierarchyChanged();
    if (alive.deleted || componentListeners_.empty()) return;
    notifyComponentListeners(alive,
                             [&](ComponentListener& l) { l.componentChildrenChanged(*this); });
  }

  Rect bounds_;
  Component* parent_ = nullptr;
  std::vector<Component*> children_;
  RectangleList damage_;
  bool visible_ = true;
  bool interceptsMouse_ = true;
  bool wantsFocus_ = false;
  bool hasFocus_ = false;
  bool accessibilityIgnored_ = false;
  std::string accessibleName_;
  std::string accessibleDescription_;
  std::string tooltip_;
  std::string componentID_;
  MouseCursor cursor_ = MouseCursor::Default;
  std::unique_ptr<CachedComponentImage> cachedImage_;
  std::vector<ComponentListener*> componentListeners_;
  std::vector<std::pair<MouseListener*, bool>> mouseListeners_;
  bool dying_ = false;
};

/**
 * Turns raw pointer input into component events.
 *
 * Kept out of Component because it is per-WINDOW state -- who has capture,
 * where the button went down, which component the pointer is over -- and a
 * copy of it inside every component would be state with one true owner and a
 * thousand places to be wrong.
 */
class MouseRouter {
public:
  /** Dispatch input into `root`'s tree: one router per window. `root` is not
   *  owned and must stay alive while input is delivered. Registers in
   *  liveRouters(), so destroyed components are forgotten automatically. */
  explicit MouseRouter(Component& root) : root_(&root) {
    liveRouters().push_back(this);
    // A router is what puts a tree in a window -- the peer, in the
    // reference's terms -- so its arrival is a hierarchy change for
    // everything in the tree: a ComponentMovementWatcher hears its
    // component's peer appear.
    root.sendParentHierarchyChanged();
  }

  /** The component this router dispatches into. Used by routerFor() below to
   *  answer "which router owns me", which a component needs when it has to ASK
   *  for the keyboard rather than wait to be given it. */
  Component* rootComponent() const { return root_; }

  /** Ends a press still in progress -- the captured component gets a mouseUp
   *  just outside itself and a drag is cancelled -- then unregisters. */
  ~MouseRouter() {
    // A press the window will never see released is ended here, as a cancel.
    // The router goes when its window closes, and a host closes an editor
    // whenever it likes -- a shortcut pressed mid-drag, a track deleted --
    // with the button still down. The release then reached nobody: a slider
    // left the host's gesture open (beginEdit with no endEdit, so the host
    // went on treating the parameter as held) and a key on the on-screen
    // keyboard never sent its note-off, a note stuck sounding with no editor
    // left to stop it. Before unregistering, so a handler that destroys a
    // component still has it forgotten here.
    if (rootAlive_) cancelPress();
    std::vector<MouseRouter*>& all = liveRouters();
    all.erase(std::remove(all.begin(), all.end(), this), all.end());
    // Unregistered FIRST, so a watcher asking routerFor() hears that the
    // window has gone. Only if the root outlived the router: forget() clears
    // the flag when it is destroyed first.
    if (rootAlive_) root_->sendParentHierarchyChanged();
  }

  /**
   * What the next events come from: a pen, a finger, their pressure and tilt.
   *
   * A peer that knows sets this immediately before calling mouseDown, Move or
   * Up and resets it after; makeEvent copies it into every MouseEvent. Kept
   * on the router rather than added as parameters so that every existing
   * caller -- and every test that drives a router by hand -- keeps compiling
   * and keeps delivering plain mouse events.
   */
  void setPointerInput(const PointerInput& input) { input_ = input; }
  /** Back to a plain mouse with no pressure, after a peer's pen or touch event. */
  void resetPointerInput() { input_ = PointerInput{}; }
  /** What the next events are stamped with. */
  const PointerInput& pointerInput() const { return input_; }

  /** Non-copyable: two routers registered from one construction would each be
   *  told about destroyed components, and only one of them would be
   *  unregistered. */
  MouseRouter(const MouseRouter&) = delete;
  MouseRouter& operator=(const MouseRouter&) = delete;  ///< Deleted: see the copy constructor.

  /**
   * A button went down at `rootPos`, in the root's logical coordinates. Called
   * by a peer, and by a test driving the tree by hand.
   *
   * The left button captures the component under the point, moves the keyboard
   * focus to it (or away from whatever held it, if it takes none), then tells
   * MouseListeners and calls its mouseDown. `rightButton` calls contextMenu on
   * that component instead, with no capture and no change of focus.
   */
  void mouseDown(Point rootPos, int clickCount = 1, bool shift = false, bool ctrl = false,
                 bool alt = false, bool rightButton = false) {
    Component* target = root_->hitTest(rootPos);

    // The right button never captures and never starts a drag. It asks the
    // component under it for a menu and stops -- see Component::contextMenu.
    // Focus is left exactly where it was: a right-click on a knob while typing
    // in a text field should not take the keyboard away from the field.
    if (rightButton) {
      if (!target) return;
      MouseEvent e = makeEvent(target, rootPos, clickCount, shift, ctrl, alt);
      e.rightButton = true;
      if (detail::mouseListenerAttachments() > 0) {
        detail::DeletionWatch alive(target);
        tellListeners(target, e, [&](MouseListener& l) { l.mouseDown(e); });
        if (alive.deleted) return;
      }
      target->contextMenu(e);
      return;
    }

    captured_ = target;
    down_ = true;
    // A click moves the focus, which is what every desktop does and what a
    // user expects when they click into a text field. A click on something
    // that does not want focus takes it away from whatever had it -- also
    // expected: clicking a knob should stop the keyboard going to a field.
    setFocus(target && target->wantsKeyboardFocus() ? target : nullptr);
    // setFocus ran focusLost() on whatever HAD the focus -- application code,
    // which can destroy the component just clicked (a value box committing on
    // focus loss rebuilds the panel the click landed in). forgetEverywhere
    // nulls captured_ when that component dies, so the ONLY safe way to know
    // the target survived is to read the guarded member back, never the local.
    // Everything below therefore goes through captured_, not target.
    if (!captured_) return;
    downLocal_ = captured_->rootToLocal(rootPos);
    const MouseEvent e = makeEvent(captured_, rootPos, clickCount, shift, ctrl, alt);
    // Listeners first; captured_ is the guarded member, re-read after them.
    tellListeners(captured_, e, [&](MouseListener& l) { l.mouseDown(e); });
    if (captured_) captured_->mouseDown(e);
  }

  /**
   * A move with the button held goes to the CAPTURED component, wherever the
   * pointer is.
   *
   * This is the rule sliders live on. Without it a fast drag hands the rest
   * of the gesture to whatever is next door and the control sticks part-way.
   */
  void mouseMove(Point rootPos, bool shift = false, bool ctrl = false, bool alt = false) {
    if (down_) {
      if (captured_) {
        const MouseEvent e = makeEvent(captured_, rootPos, 1, shift, ctrl, alt);
        tellListeners(captured_, e, [&](MouseListener& l) { l.mouseDrag(e); });
        if (captured_) captured_->mouseDrag(e);
      }
      // AFTER the component's own mouseDrag, because that is where a drag is
      // usually started -- doing it first would miss the frame the drag began
      // on and the target under the pointer would light up one move late.
      if (dragging_) updateDragTarget(rootPos);
      return;
    }
    Component* over = root_->hitTest(rootPos);
    if (over != hovered_) {
      // The guarded member is updated to `over` BEFORE mouseExit runs, so if a
      // handler frees a component, forgetEverywhere nulls the member and the
      // reads below see it. mouseEnter can free the thing just entered, so the
      // trailing mouseMove goes through hovered_ (re-read) rather than the
      // stale `over` local -- the dangling deref the audit found.
      Component* previousHover = hovered_;
      hovered_ = over;
      if (previousHover) {
        const MouseEvent e = makeEvent(previousHover, rootPos, 0, shift, ctrl, alt);
        detail::DeletionWatch alive(previousHover);
        tellListeners(previousHover, e, [&](MouseListener& l) { l.mouseExit(e); });
        if (!alive.deleted) previousHover->mouseExit(e);
      }
      if (hovered_) {
        const MouseEvent e = makeEvent(hovered_, rootPos, 0, shift, ctrl, alt);
        tellListeners(hovered_, e, [&](MouseListener& l) { l.mouseEnter(e); });
        if (hovered_) hovered_->mouseEnter(e);
      }
    }
    if (hovered_) {
      const MouseEvent e = makeEvent(hovered_, rootPos, 0, shift, ctrl, alt);
      tellListeners(hovered_, e, [&](MouseListener& l) { l.mouseMove(e); });
      if (hovered_) hovered_->mouseMove(e);
    }
  }

  /** The button came up at `rootPos`: an internal drag is dropped on its target
   *  first, then the captured component gets mouseUp, capture is released and
   *  hover is worked out again. */
  void mouseUp(Point rootPos, bool shift = false, bool ctrl = false, bool alt = false) {
    // The drop happens BEFORE the source's own mouseUp. A slider ending a
    // gesture and a drag being dropped are two different endings to one press,
    // and the drop is the one the user meant -- letting the source finish
    // first means its onDragEnd fires while the drag is still notionally in
    // flight.
    if (dragging_) {
      Component* target = dragTarget_;
      DragSource what = drag_;
      dragging_ = false;
      dragTarget_ = nullptr;
      drag_ = DragSource{};
      if (target) {
        target->itemDragExit(what);
        target->itemDropped(what, target->rootToLocal(rootPos));
      }
    }
    if (captured_) {
      const MouseEvent e = makeEvent(captured_, rootPos, 1, shift, ctrl, alt);
      tellListeners(captured_, e, [&](MouseListener& l) { l.mouseUp(e); });
      if (captured_) captured_->mouseUp(e);
    }
    down_ = false;
    captured_ = nullptr;
    // The pointer may have ended somewhere else entirely, so hover is worked
    // out again rather than assumed to be where the drag started.
    Component* over = root_->hitTest(rootPos);
    if (over != hovered_) {
      // The member takes `over` BEFORE the exit runs, as in mouseMove: an exit
      // handler that destroys the control under the pointer then has its
      // destructor null the member, and the enter below goes nowhere instead
      // of into freed memory. Assigned after, it held the freed control.
      Component* previousHover = hovered_;
      hovered_ = over;
      if (previousHover) {
        const MouseEvent e = makeEvent(previousHover, rootPos, 0, shift, ctrl, alt);
        detail::DeletionWatch alive(previousHover);
        tellListeners(previousHover, e, [&](MouseListener& l) { l.mouseExit(e); });
        if (!alive.deleted) previousHover->mouseExit(e);
      }
      if (hovered_) {
        const MouseEvent e = makeEvent(hovered_, rootPos, 0, shift, ctrl, alt);
        tellListeners(hovered_, e, [&](MouseListener& l) { l.mouseEnter(e); });
        if (hovered_) hovered_->mouseEnter(e);
      }
    }
  }

  /**
   * The wheel turned at `rootPos`. `delta` is in notches, positive for the
   * wheel turned away from the user (up); Windows may report fractions from a
   * fine-grained wheel, and a macOS trackpad's travel arrives a notch at a
   * time.
   *
   * Offered to the component under the pointer, never to a captured one, and
   * up its parents until one's mouseWheel returns true. MouseListeners hear it
   * once.
   */
  void mouseWheel(Point rootPos, float delta, bool shift = false, bool ctrl = false,
                  bool alt = false) {
    // To whatever is under the pointer, never to the captured component: a
    // wheel during a drag is a separate gesture and belongs where it points.
    Component* over = root_->hitTest(rootPos);
    if (over && detail::mouseListenerAttachments() > 0) {
      // Once, for the component under the pointer -- not once per level of
      // the bubbling below, or a listener on an editor would count a single
      // notch as many.
      const MouseEvent e = makeEvent(over, rootPos, 0, shift, ctrl, alt);
      detail::DeletionWatch alive(over);
      tellListeners(over, e, [&](MouseListener& l) { l.mouseWheelMove(e, delta); });
      if (alive.deleted) return;
    }
    for (Component* c = over; c != nullptr; c = c->parent())
      if (c->mouseWheel(makeEvent(c, rootPos, 0, shift, ctrl, alt), delta)) break;
  }

  /** Called when the pointer leaves the window, so a hover highlight does not
   *  stay lit after the user has gone. */
  void mouseExitWindow() {
    if (hovered_ && !down_) {
      MouseEvent e;
      e.eventComponent = hovered_;
      Component* leaving = hovered_;
      hovered_ = nullptr;
      detail::DeletionWatch alive(leaving);
      tellListeners(leaving, e, [&](MouseListener& l) { l.mouseExit(e); });
      if (!alive.deleted) leaving->mouseExit(e);
    }
  }

  /** The component that has the mouse during a press, or null. */
  Component* captured() const { return captured_; }
  /** The deepest component under the pointer, as of the last move or release;
   *  null outside the window. */
  Component* hovered() const { return hovered_; }

  /**
   * What the pointer should look like right now.
   *
   * Whatever is CAPTURED wins while a button is down: a knob being dragged
   * keeps its cursor even when the pointer has left it, which is the same rule
   * the drag itself follows.
   *
   * Otherwise it is inherited up the tree, so a panel can set one for
   * everything inside it without every child repeating it.
   */
  MouseCursor cursorForPointer() const {
    Component* from = (down_ && captured_) ? captured_ : hovered_;
    for (Component* c = from; c != nullptr; c = c->parent())
      if (c->cursor() != MouseCursor::Default) return c->cursor();
    return MouseCursor::Default;
  }

  // ── Keyboard focus ───────────────────────────────────────────────────────
  //
  // Per-WINDOW state, like capture and hover, and here for the same reason: a
  // copy inside every component would be one truth with a thousand places to
  // be wrong.

  /** The component holding the keyboard focus, or null. */
  Component* focused() const { return focused_; }

  // ── Drag and drop ────────────────────────────────────────────────────────

  /**
   * Begin a drag. Called from a component's own mouseDrag, once it has decided
   * the gesture is a drag rather than a click.
   *
   * The source keeps mouse capture throughout -- it already has it, having
   * been pressed -- so moves keep arriving even over a target. The router
   * looks up the target itself rather than letting capture decide, which is
   * the one place a drag deliberately differs from every other gesture.
   */
  void startDrag(const std::string& description, Component* source, Point origin) {
    dragging_ = true;
    drag_.description = description;
    drag_.source = source;
    drag_.origin = origin;
    drag_.position = origin;
    drag_.isText = false;
    dragTarget_ = nullptr;
  }

  /** The same, carrying `text` the user selected: offered to every
   *  TextDragAndDropTarget under the pointer, and to any component that
   *  handles item drags and checks DragSource::isText itself. */
  void startTextDrag(const std::string& text, Component* source, Point origin) {
    startDrag(text, source, origin);
    drag_.isText = true;
  }

  /** Whether an internal drag started by startDrag is in flight. */
  bool isDragging() const { return dragging_; }
  /** What is being dragged; empty when nothing is. */
  const DragSource& dragSource() const { return drag_; }
  /** The component that accepted the drag and is under the pointer, or null. */
  Component* dragTarget() const { return dragTarget_; }

  /** Abandon the drag without a drop: the target hears itemDragExit and nothing
   *  else. The press itself goes on, so the source still gets its mouseUp. */
  void cancelDrag() {
    if (!dragging_) return;
    if (dragTarget_) dragTarget_->itemDragExit(drag_);
    dragging_ = false;
    dragTarget_ = nullptr;
    drag_ = DragSource{};
  }

  // ── Files dragged in from outside ────────────────────────────────────────
  //
  // Driven by the peers, which are the only things that hear from the window
  // system. Kept here beside the internal drag so both find their target the
  // same way, rather than each having its own idea of what is under the
  // pointer.

  /** Returns whether anything under the pointer would take them, which is what
   *  a platform needs to answer before the user lets go. */
  bool fileDragMove(const std::vector<std::string>& files, Point rootPos) {
    Component* over = findFileTarget(files, rootPos);
    if (over != fileTarget_) {
      if (fileTarget_) fileTarget_->fileDragExit();
      fileTarget_ = over;
      if (fileTarget_) fileTarget_->fileDragEnter(files, fileTarget_->rootToLocal(rootPos));
    }
    return fileTarget_ != nullptr;
  }

  /** The platform's file drag left the window or was abandoned: the current
   *  target, if any, hears fileDragExit. */
  void fileDragExit() {
    if (!fileTarget_) return;
    fileTarget_->fileDragExit();
    fileTarget_ = nullptr;
  }

  /** Drop `files` at `rootPos` on the nearest component under it that is
   *  interested, after closing its enter/exit pair. Returns whether one took
   *  them. */
  bool filesDropped(const std::vector<std::string>& files, Point rootPos) {
    Component* over = findFileTarget(files, rootPos);
    // The enter/exit pair is closed BEFORE the drop, so a target's highlight is
    // already off when its handler runs -- otherwise a handler that opens a
    // dialog leaves the highlight lit behind it.
    fileDragExit();
    if (!over) return false;
    over->filesDropped(files, over->rootToLocal(rootPos));
    return true;
  }

  /**
   * Move the keyboard focus.
   *
   * Null clears it. A component that does not want focus is refused rather
   * than quietly accepted, or a stray click on a knob would take the keyboard
   * away from the field the user was typing in.
   *
   * ── The new focus is recorded BEFORE the old one is told ──────────────────
   *
   * focusLost() runs application code, and application code calls back in
   * here. That is not exotic -- it is the ordinary path: a ValueBox commits
   * what was typed when its editor loses focus, and committing ends the edit,
   * and ending the edit clears the focus.
   *
   * Written the obvious way round -- tell the old one, then record the new --
   * that sequence does not terminate. focusLost() is called while focused_
   * still points at the editor, so the editor's own "am I still focused?"
   * guard says yes, so it calls setFocus(nullptr), which sees focused_ still
   * pointing at the editor and calls focusLost() again. It recurses until the
   * stack runs out.
   *
   * That was a real crash and not a theoretical one: double-click a value box
   * to type in it, then click anywhere else, and the plugin took the host down
   * with it. Nothing caught it because no test had ever opened a value editor
   * and then clicked away from it -- it was found by a window test written for
   * something else entirely, which crashed one assertion after the one it was
   * added for.
   *
   * Recording focused_ first makes the re-entrant call see the world as it
   * will be. The editor's guard then correctly answers "no, the focus has
   * already moved", and the recursion never starts.
   */
  void setFocus(Component* c) {
    if (c == focused_) return;
    if (c && !c->wantsKeyboardFocus()) return;

    Component* previous = focused_;
    focused_ = c;

    if (previous) {
      // repaint() BEFORE focusLost(), not after. focusLost() is application
      // code and a transient (a popup that closes when it loses focus) can
      // destroy `previous` from inside it -- and previous is a local nothing
      // nulls. Marking it dirty while it is certainly alive, then making
      // focusLost the LAST thing that touches it, removes the dangling repaint.
      previous->hasFocus_ = false;
      previous->repaint();
      previous->focusLost();
    }
    // Only if focusLost did not move it again. A commit that opens a dialog,
    // or one that clears the focus outright, has already decided where the
    // focus belongs -- and taking it back would undo that decision.
    if (c && focused_ == c) {
      c->hasFocus_ = true;
      c->focusGained();
      c->repaint();
    }
  }

  /**
   * A last look at a key nothing in the tree wanted.
   *
   * Where a command manager hangs. The ORDER matters and is the whole reason
   * this is a fallback rather than a first pass: a text field must get Ctrl+A
   * for select-all, and a global "select all items" command that took it first
   * would steal it out from under the caret in every field in the editor.
   */
  std::function<bool(const KeyPress&)> onUnhandledKey;

  /**
   * Deliver a key.
   *
   * To the focused component first, then UP its parents until something
   * handles it. That walk is what lets a text field ignore Escape and have the
   * dialog around it close instead -- and it is why keyPressed returns a bool
   * rather than being void.
   *
   * With nothing focused the root still gets a look, so a shortcut works
   * before anything has been clicked on. A Tab nobody took moves the focus;
   * after that, onUnhandledKey gets the key.
   */
  bool keyPressed(const KeyPress& key) {
    for (Component* c = focused_ ? focused_ : root_; c != nullptr; c = c->parent())
      if (c->keyPressed(key)) return true;
    // Tab nobody took is traversal. Every field declines it on the grounds
    // that traversal is the router's job, and nothing here did it: Tab moved
    // focus nowhere and the keyboard could not get out of the first field.
    // With Ctrl or Alt it is somebody's shortcut, and with nothing focusable
    // it goes on to the host.
    if (key.keyCode == KeyPress::Tab && !key.ctrlDown && !key.altDown &&
        focusNext(key.shiftDown))
      return true;
    if (onUnhandledKey) return onUnhandledKey(key);
    return false;
  }

  /**
   * Tab to the next component that wants focus, wrapping at the end.
   *
   * In tree order, which is the order components were added, which is the
   * order they were laid out in -- so Tab goes down the editor the way the eye
   * does. Nothing here reads positions: a traversal that sorted by coordinate
   * would reorder itself every time a host resized the window.
   */
  bool focusNext(bool backwards = false) {
    std::vector<Component*> order;
    collectFocusable(root_, order);
    if (order.empty()) return false;

    size_t at = 0;
    bool found = false;
    for (size_t i = 0; i < order.size(); ++i)
      if (order[i] == focused_) {
        at = i;
        found = true;
        break;
      }
    size_t next;
    if (!found) next = backwards ? order.size() - 1 : 0;
    else if (backwards) next = (at + order.size() - 1) % order.size();
    else next = (at + 1) % order.size();
    setFocus(order[next]);
    return true;
  }

  /** Forget a component that is going away. A router still pointing at a
   *  destroyed component delivers the next drag into freed memory. */
  void forget(Component* c) {
    if (root_ == c) rootAlive_ = false;
    if (captured_ == c) {
      captured_ = nullptr;
      down_ = false;
    }
    if (hovered_ == c) hovered_ = nullptr;
    if (dragTarget_ == c) dragTarget_ = nullptr;
    if (fileTarget_ == c) fileTarget_ = nullptr;
    if (drag_.source == c) {
      dragging_ = false;
      drag_ = DragSource{};
    }
    if (focused_ == c) {
      // Cleared WITHOUT calling focusLost: the component is on its way out and
      // may already be part-destroyed, so calling a virtual on it is the bug
      // this method exists to prevent.
      c->hasFocus_ = false;
      focused_ = nullptr;
    }
  }

private:
  /** End the press in progress without a release: a drag in flight is
   *  cancelled rather than dropped, and the captured component gets its
   *  mouseUp at a point just outside itself -- so a button does not fire,
   *  and a control that ends a gesture or a note on release ends it. */
  void cancelPress() {
    cancelDrag();
    if (!down_ || !captured_) {
      down_ = false;
      return;
    }
    const Point outside = captured_->localToRoot({-1.0f, -1.0f});
    const MouseEvent e = makeEvent(captured_, outside, 1, false, false, false);
    tellListeners(captured_, e, [&](MouseListener& l) { l.mouseUp(e); });
    if (captured_) captured_->mouseUp(e);
    down_ = false;
    captured_ = nullptr;
  }

  MouseEvent makeEvent(Component* target, Point rootPos, int clickCount, bool shift, bool ctrl,
                       bool alt) const {
    MouseEvent e;
    e.position = target->rootToLocal(rootPos);
    e.rootPosition = rootPos;
    e.downPosition = (target == captured_) ? downLocal_ : e.position;
    e.clickCount = clickCount;
    e.shiftDown = shift;
    e.ctrlDown = ctrl;
    e.altDown = alt;
    e.sourceType = input_.type;
    e.sourceIndex = input_.index;
    e.pressure = input_.pressure;
    e.orientation = input_.orientation;
    e.pen = input_.pen;
    e.eventComponent = target;
    return e;
  }

  /**
   * Every MouseListener that should hear an event delivered to `target`: its
   * own, and those of each ancestor that asked for nested events.
   *
   * Snapshotted before any is called, and each checked against the live set
   * before its call -- a listener's handler may destroy another listener, or
   * the component; what it cannot do is make this call into freed memory.
   * Nothing at all happens while no listener is attached anywhere.
   */
  template <typename Fn>
  void tellListeners(Component* target, const MouseEvent& e, Fn&& fn) {
    (void) e;
    if (!target || detail::mouseListenerAttachments() <= 0) return;
    std::vector<MouseListener*> told;
    for (Component* c = target; c != nullptr; c = c->parent())
      for (const auto& entry : c->mouseListeners())
        if (c == target || entry.second) told.push_back(entry.first);
    const std::vector<MouseListener*>& live = detail::liveMouseListeners();
    for (MouseListener* l : told)
      if (std::find(live.begin(), live.end(), l) != live.end()) fn(*l);
  }

  void updateDragTarget(Point rootPos) {
    drag_.position = rootPos;
    Component* hit = root_->hitTest(rootPos);
    // Up the tree until something is interested. A knob inside a slot panel is
    // what the pointer is over, and the SLOT is what accepts the drop -- so a
    // search that stopped at the topmost component would make every panel
    // undroppable the moment it had a control in it.
    Component* target = nullptr;
    for (Component* c = hit; c != nullptr; c = c->parent())
      if (c != drag_.source && c->isInterestedInDrag(drag_)) {
        target = c;
        break;
      }

    if (target != dragTarget_) {
      if (dragTarget_) dragTarget_->itemDragExit(drag_);
      dragTarget_ = target;
      if (dragTarget_) dragTarget_->itemDragEnter(drag_);
    }
    if (dragTarget_) dragTarget_->itemDragMove(drag_, dragTarget_->rootToLocal(rootPos));
  }

  Component* findFileTarget(const std::vector<std::string>& files, Point rootPos) {
    Component* hit = root_->hitTest(rootPos);
    for (Component* c = hit; c != nullptr; c = c->parent())
      if (c->isInterestedInFileDrag(files)) return c;
    return nullptr;
  }

  static void collectFocusable(Component* c, std::vector<Component*>& out) {
    if (!c || !c->isVisible()) return;
    if (c->wantsKeyboardFocus()) out.push_back(c);
    for (Component* child : c->children()) collectFocusable(child, out);
  }

  Component* root_;
  Component* focused_ = nullptr;
  Component* dragTarget_ = nullptr;
  Component* fileTarget_ = nullptr;
  DragSource drag_;
  bool dragging_ = false;
  Component* captured_ = nullptr;
  Component* hovered_ = nullptr;
  Point downLocal_;
  bool down_ = false;
  PointerInput input_;
  bool rootAlive_ = true;
};

/**
 * Tell every live router that this component is going away.
 *
 * Out of line because it needs MouseRouter to be complete, and MouseRouter
 * needs Component. Defined here rather than in a third file because it is four
 * lines and splitting a header to hold them would be worse.
 */
inline void Component::forgetEverywhere(Component* c) {
  for (MouseRouter* router : liveRouters()) router->forget(c);
}

inline void Component::releaseFocusFrom(Component* c) {
  // Over a copy, each checked against the live list: focusLost is the
  // owner's code and may close a window, router and all. `c` is compared,
  // never followed -- the same handler may have destroyed it.
  const std::vector<MouseRouter*> routers = liveRouters();
  for (MouseRouter* router : routers) {
    const std::vector<MouseRouter*>& live = liveRouters();
    if (std::find(live.begin(), live.end(), router) == live.end()) continue;
    for (const Component* f = router->focused(); f != nullptr; f = f->parent())
      if (f == c) {
        router->setFocus(nullptr);
        break;
      }
  }
}

// ── Listener lifetimes ──────────────────────────────────────────────────────
//
// Both directions are kept, so either side may be destroyed first: the
// component's destructor removes itself from its listeners' lists, and a
// listener's destructor removes itself from its components'.

inline ComponentListener::~ComponentListener() {
  const std::vector<Component*> watched = watching_;
  for (Component* c : watched) c->removeComponentListener(this);
}

inline MouseListener::MouseListener() { detail::liveMouseListeners().push_back(this); }

inline MouseListener::~MouseListener() {
  const std::vector<Component*> watched = watching_;
  for (Component* c : watched) c->removeMouseListener(this);
  std::vector<MouseListener*>& live = detail::liveMouseListeners();
  live.erase(std::remove(live.begin(), live.end(), this), live.end());
}

/**
 * The router dispatching into the tree `c` belongs to, or null.
 *
 * A component normally has no business knowing about the router: input arrives
 * at it and it responds. The exception is FOCUS, which is a request rather than
 * a response -- a readout that turns into a text field on a double click has to
 * hand the keyboard to that field, and there is nothing else that can.
 *
 * Found by walking to the root and matching it against the live routers, rather
 * than by storing a pointer in every component. There is one router per window
 * and a handful of windows; the walk is the depth of the tree and happens when
 * somebody clicks, not when anything paints.
 */
inline MouseRouter* routerFor(const Component* c) {
  if (!c) return nullptr;
  const Component* root = c;
  while (root->parent()) root = root->parent();
  for (MouseRouter* router : liveRouters())
    if (router->rootComponent() == root) return router;
  return nullptr;
}

} // namespace gfx
} // namespace sonore

// The standard cache, which needs Component complete: see cached_image.h.
#include "cached_image.h"
