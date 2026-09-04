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
#include <string>
#include <vector>

#include "graphics.h"
#include "region.h"

namespace sonore {
namespace gfx {

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
  int clickCount = 1;
  bool shiftDown = false;
  bool ctrlDown = false;
  bool altDown = false;
  /** The RIGHT button. Delivered to contextMenu() and never to mouseDown, so
   *  no control has to remember to check it before starting a drag. */
  bool rightButton = false;

  float dragDistanceX() const { return position.x - downPosition.x; }
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
    None = 0,
    Backspace = 0x10000,
    Tab,
    Return,
    Escape,
    Delete,
    Left,
    Right,
    Up,
    Down,
    Home,
    End,
    PageUp,
    PageDown,
  };

  int keyCode = None;
  /** The typed character as a Unicode code point, or 0 for a key that types
   *  nothing. NOT a byte: a plugin used outside an English keyboard layout
   *  gets accented characters here, and truncating to char would mangle them. */
  uint32_t character = 0;
  bool shiftDown = false;
  bool ctrlDown = false;
  bool altDown = false;

  bool isCharacter() const { return character != 0; }
  bool is(int code) const { return keyCode == code; }
};

class Component;
class MouseRouter;

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
  Default,
  Pointing,     // a hand: this is clickable
  DragVertical, // a knob or a vertical fader
  DragHorizontal,
  ResizeCorner,    // the diagonal, for a corner grip
  ResizeLeftRight, // a vertical edge
  ResizeUpDown,    // a horizontal edge
  Text,         // an I-beam, over a text field
  Wait,
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
  std::string description;
  Component* source = nullptr;
  /** Where the drag started, in root coordinates. A target that wants to know
   *  which half of itself was aimed at gets the CURRENT position in the event;
   *  this is for a source that needs to undo a move. */
  Point origin;
};

class Component {
public:
  virtual ~Component() {
    // EVERY router is told, before anything else happens. A router holds raw
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

  Component() = default;
  // A component is a NODE: its parent lists its address and every router may
  // hold it. Copying or moving one would duplicate that linkage into an object
  // the tree has never heard of, and assigning over a live one overwrote
  // parent_ with null while the parent still listed it -- so the destructor
  // skipped its own removal and the parent later wrote into a dead object.
  // UBSan found that in the drag-and-drop test (`left = DropZone{}`); deleting
  // the operations turns a silent dangling pointer into a compile error.
  Component(const Component&) = delete;
  Component& operator=(const Component&) = delete;
  Component(Component&&) = delete;
  Component& operator=(Component&&) = delete;

  // ── Geometry ─────────────────────────────────────────────────────────────

  /** In the parent's coordinates. */
  const Rect& bounds() const { return bounds_; }

  /** Origin at zero: what paint() and mouse positions are in. */
  Rect localBounds() const { return {0.0f, 0.0f, bounds_.w, bounds_.h}; }

  void setBounds(const Rect& r) {
    const bool sizeChanged = r.w != bounds_.w || r.h != bounds_.h;
    if (r.x == bounds_.x && r.y == bounds_.y && !sizeChanged) return;
    // BOTH rectangles are damaged. Repainting only the new one leaves the
    // old position on screen, which is the classic smear a moved control
    // leaves behind.
    repaint();
    bounds_ = r;
    repaint();
    if (sizeChanged) resized();
  }

  void setSize(float w, float h) { setBounds({bounds_.x, bounds_.y, w, h}); }

  // ── The tree ─────────────────────────────────────────────────────────────

  Component* parent() const { return parent_; }
  int numChildren() const { return (int) children_.size(); }
  /** For a walk over the tree -- focus traversal is the first caller.
   *  Const so nothing outside can reorder what paint and hit-test depend on. */
  const std::vector<Component*>& children() const { return children_; }
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

  Component* root() {
    Component* c = this;
    while (c->parent_) c = c->parent_;
    return c;
  }

  // ── Visibility ───────────────────────────────────────────────────────────

  bool isVisible() const { return visible_; }

  void setVisible(bool shouldBeVisible) {
    if (visible_ == shouldBeVisible) return;
    visible_ = shouldBeVisible;
    repaint();
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
  bool interceptsMouse() const { return interceptsMouse_; }

  // ── Coordinates ──────────────────────────────────────────────────────────

  Point localToParent(Point p) const { return {p.x + bounds_.x, p.y + bounds_.y}; }

  Point localToRoot(Point p) const {
    for (const Component* c = this; c->parent_; c = c->parent_) p = c->localToParent(p);
    return p;
  }

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

  virtual void paint(Graphics&) {}
  virtual void resized() {}

  /**
   * Paint this component and everything inside it.
   *
   * Each child is translated into its own coordinates and CLIPPED to its
   * bounds, so a component that draws outside itself is trimmed rather than
   * scribbling on its siblings. That clip is why a component can be written
   * without knowing what is next to it.
   */
  /**
   * Paint everything.
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

    {
      Graphics::ScopedState own(g);
      paint(g);
    }
    for (Component* c : children_) {
      Graphics::ScopedState childScope(g);
      g.translate(c->bounds_.x, c->bounds_.y);
      g.clipTo(c->localBounds());
      if (!g.isClippedOut()) c->paintTree(g);
    }
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

    {
      Graphics::ScopedState own(g);
      paint(g);
    }
    for (Component* c : children_) {
      // The damage moves into the CHILD's coordinates, which is a translation
      // by the child's position -- the opposite direction to the transform,
      // and getting the sign wrong here paints the right components with the
      // wrong parts of them.
      RectangleList childDamage;
      for (const Rect& r : damage.rects())
        childDamage.add({r.x - c->bounds_.x, r.y - c->bounds_.y, r.w, r.h});

      Graphics::ScopedState childScope(g);
      g.translate(c->bounds_.x, c->bounds_.y);
      g.clipTo(c->localBounds());
      if (!g.isClippedOut()) c->paintTree(g, childDamage);
    }
  }

  // ── Damage ───────────────────────────────────────────────────────────────

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
    if (!isShowing()) return;
    const Point topLeft = localToRoot({area.x, area.y});
    root()->damage_.add({topLeft.x, topLeft.y, area.w, area.h});
  }

  bool isDirty() const { return !damage_.isEmpty(); }
  const RectangleList& damage() const { return damage_; }
  /** Everything damaged, as one rectangle -- for a peer that can only blit a
   *  single region. */
  Rect damageBounds() const { return damage_.bounds(); }
  void clearDamage() { damage_.clear(); }

  // ── Mouse delivery ───────────────────────────────────────────────────────

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
  virtual void mouseDrag(const MouseEvent&) {}
  virtual void mouseUp(const MouseEvent&) {}
  virtual void mouseMove(const MouseEvent&) {}
  virtual void mouseEnter(const MouseEvent&) {}
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

  virtual void focusGained() {}
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
  void setTooltip(std::string text) { tooltip_ = std::move(text); }

  /**
   * The cursor to show over this component.
   *
   * Virtual for the same reason tooltip() is: a control whose cursor depends on
   * where inside it the pointer is -- an edge that resizes, a body that drags
   * -- can answer with what is true now.
   */
  virtual MouseCursor cursor() const { return cursor_; }
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
   * Asked once when the pointer enters, not on every move: a target that
   * changed its mind mid-hover would flicker its highlight. False by default,
   * so a component becomes a drop target deliberately.
   */
  virtual bool isInterestedInDrag(const DragSource&) { return false; }

  virtual void itemDragEnter(const DragSource&) {}
  virtual void itemDragMove(const DragSource&, Point) {}
  virtual void itemDragExit(const DragSource&) {}

  /** `position` is in this component's own coordinates. */
  virtual void itemDropped(const DragSource&, Point position) { (void) position; }

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

  virtual void fileDragEnter(const std::vector<std::string>&, Point) {}
  virtual void fileDragExit() {}
  virtual void filesDropped(const std::vector<std::string>& files, Point position) {
    (void) files;
    (void) position;
  }

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
  const std::string& accessibleName() const { return accessibleName_; }

  void setAccessibleDescription(std::string text) { accessibleDescription_ = std::move(text); }
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
  bool isAccessibilityIgnored() const { return accessibilityIgnored_; }

private:
  friend class MouseRouter;

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
  MouseCursor cursor_ = MouseCursor::Default;
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
  explicit MouseRouter(Component& root) : root_(&root) {
    liveRouters().push_back(this);
  }

  /** The component this router dispatches into. Used by routerFor() below to
   *  answer "which router owns me", which a component needs when it has to ASK
   *  for the keyboard rather than wait to be given it. */
  Component* rootComponent() const { return root_; }

  ~MouseRouter() {
    std::vector<MouseRouter*>& all = liveRouters();
    all.erase(std::remove(all.begin(), all.end(), this), all.end());
  }

  // Non-copyable: two routers registered from one construction would each be
  // told about destroyed components, and only one of them would be unregistered.
  MouseRouter(const MouseRouter&) = delete;
  MouseRouter& operator=(const MouseRouter&) = delete;

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
    captured_->mouseDown(makeEvent(captured_, rootPos, clickCount, shift, ctrl, alt));
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
      if (captured_) captured_->mouseDrag(makeEvent(captured_, rootPos, 1, shift, ctrl, alt));
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
      if (previousHover) previousHover->mouseExit(makeEvent(previousHover, rootPos, 0, shift, ctrl, alt));
      if (hovered_) hovered_->mouseEnter(makeEvent(hovered_, rootPos, 0, shift, ctrl, alt));
    }
    if (hovered_) hovered_->mouseMove(makeEvent(hovered_, rootPos, 0, shift, ctrl, alt));
  }

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
    if (captured_) captured_->mouseUp(makeEvent(captured_, rootPos, 1, shift, ctrl, alt));
    down_ = false;
    captured_ = nullptr;
    // The pointer may have ended somewhere else entirely, so hover is worked
    // out again rather than assumed to be where the drag started.
    Component* over = root_->hitTest(rootPos);
    if (over != hovered_) {
      if (hovered_) hovered_->mouseExit(makeEvent(hovered_, rootPos, 0, shift, ctrl, alt));
      hovered_ = over;
      if (hovered_) hovered_->mouseEnter(makeEvent(hovered_, rootPos, 0, shift, ctrl, alt));
    }
  }

  void mouseWheel(Point rootPos, float delta, bool shift = false, bool ctrl = false,
                  bool alt = false) {
    // To whatever is under the pointer, never to the captured component: a
    // wheel during a drag is a separate gesture and belongs where it points.
    Component* over = root_->hitTest(rootPos);
    for (Component* c = over; c != nullptr; c = c->parent())
      if (c->mouseWheel(makeEvent(c, rootPos, 0, shift, ctrl, alt), delta)) break;
  }

  /** Called when the pointer leaves the window, so a hover highlight does not
   *  stay lit after the user has gone. */
  void mouseExitWindow() {
    if (hovered_ && !down_) {
      MouseEvent e;
      hovered_->mouseExit(e);
      hovered_ = nullptr;
    }
  }

  Component* captured() const { return captured_; }
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
    dragTarget_ = nullptr;
  }

  bool isDragging() const { return dragging_; }
  const DragSource& dragSource() const { return drag_; }
  Component* dragTarget() const { return dragTarget_; }

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

  void fileDragExit() {
    if (!fileTarget_) return;
    fileTarget_->fileDragExit();
    fileTarget_ = nullptr;
  }

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

  /** Null clears it. A component that does not want focus is refused rather
   *  than quietly accepted, or a stray click on a knob would take the keyboard
   *  away from the field the user was typing in. */
  /**
   * Move the keyboard focus.
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
   * Deliver a key.
   *
   * To the focused component first, then UP its parents until something
   * handles it. That walk is what lets a text field ignore Escape and have the
   * dialog around it close instead -- and it is why keyPressed returns a bool
   * rather than being void.
   *
   * With nothing focused the root still gets a look, so a shortcut works
   * before anything has been clicked on.
   */
  /**
   * A last look at a key nothing in the tree wanted.
   *
   * Where a command manager hangs. The ORDER matters and is the whole reason
   * this is a fallback rather than a first pass: a text field must get Ctrl+A
   * for select-all, and a global "select all items" command that took it first
   * would steal it out from under the caret in every field in the editor.
   */
  std::function<bool(const KeyPress&)> onUnhandledKey;

  bool keyPressed(const KeyPress& key) {
    for (Component* c = focused_ ? focused_ : root_; c != nullptr; c = c->parent())
      if (c->keyPressed(key)) return true;
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
    return e;
  }

  void updateDragTarget(Point rootPos) {
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
