// SPDX-License-Identifier: Apache-2.0
//
// The hooks a component offers to things that are not it: listeners for its
// movement and its mouse events, a cached image to stand in for its painting,
// a target for dragged text, and an observer of every paint.
//
// ── Why these are a header of their own ─────────────────────────────────────
//
// They are interfaces component.h has to know -- Component calls into each of
// them -- and nothing else. Kept apart, the tree itself stays readable as the
// tree, and the machinery built on these (ComponentMovementWatcher,
// MouseInactivityDetector, StandardCachedComponentImage, the relative
// positioner, the text-drag dispatcher, ComponentPaintDiagnostics) is found
// by following one include rather than by reading the whole of component.h.
//
// ── The lifetime rule they all share ────────────────────────────────────────
//
// Nothing here is owned by the component it is attached to, and either side
// may be destroyed first. The component keeps the list of what is attached to
// it and the listener keeps the list of what it is attached to; each side's
// destructor clears the other's entry. A caller that iterates listeners does
// so over a snapshot, checking each against the live list before calling it,
// and a detail::DeletionWatch tells it when the component itself has gone.
#pragma once

#include <string>
#include <vector>

#include "geometry.h"
#include "graphics.h"

namespace sonore {
namespace gfx {

class Component;
struct MouseEvent;

/**
 * Told when a component moves, shows, hides, changes parent or goes away.
 *
 * The notification half of what the reference calls a ComponentListener, and
 * the one piece the tree did not have: a component could react to its OWN
 * resize (resized()) but nothing else could. ComponentMovementWatcher, the
 * relative positioner and the text-drag dispatcher are all built on it.
 *
 * The component does not own its listeners, and either side may go first: a
 * listener destroyed while attached detaches itself, and a component destroyed
 * while watched tells its listeners componentBeingDeleted and forgets them.
 */
class ComponentListener {
public:
  ComponentListener() = default;
  /** Not copyable: the copy would hold a list of watched components none of
   *  which lists it. */
  ComponentListener(const ComponentListener&) = delete;
  ComponentListener& operator=(const ComponentListener&) = delete;  ///< Deleted: see the copy constructor.
  /** Defined after Component, which it has to reach into. */
  virtual ~ComponentListener();

  /**
   * The component's setBounds changed its position (`wasMoved`), its size
   * (`wasResized`) or both. Sent after its resized() ran, with bounds() already
   * the new one.
   *
   * Not sent when an ANCESTOR moves: bounds are relative to the parent and did
   * not change. Something that tracks where a component is in the window
   * watches the ancestors too, which is what ComponentMovementWatcher does.
   */
  virtual void componentMovedOrResized(Component&, bool wasMoved, bool wasResized) {
    (void) wasMoved;
    (void) wasResized;
  }
  /** setVisible changed the flag. Not sent for an ancestor's change -- a
   *  watcher that cares about those attaches to the ancestors too. */
  virtual void componentVisibilityChanged(Component&) {}
  /** This component, or one of its ancestors, was added to or taken out of a
   *  parent -- or the window it is in appeared. */
  virtual void componentParentHierarchyChanged(Component&) {}
  /** A direct child was added to or removed from the component. Not sent when
   *  the children are only reordered (toFront, moveChildToBack). */
  virtual void componentChildrenChanged(Component&) {}
  /** From inside the destructor: the derived parts are gone, so only the
   *  Component's own accessors may be used. */
  virtual void componentBeingDeleted(Component&) {}

protected:
  /** Every component this listener is attached to, kept by the components
   *  themselves -- the one list that cannot hold a component already gone. */
  const std::vector<Component*>& watchedComponents() const { return watching_; }

private:
  friend class Component;
  std::vector<Component*> watching_;
};

/**
 * Sees the mouse events delivered to a component -- and, if asked, to
 * everything inside it.
 *
 * The router delivers an event to ONE component, the one under the pointer or
 * the one holding capture. A listener is how something that is not that
 * component hears it too: an inactivity detector watching a whole editor, a
 * viewport scrolling when its content is dragged. Listeners are told BEFORE
 * the component itself, so a listener cannot be starved by a handler that
 * destroys the component it ran on.
 */
class MouseListener {
public:
  /** Registers in the list of live listeners, so a dispatch whose handler has
   *  destroyed this listener skips it instead of calling freed memory. */
  MouseListener();
  /** Not copyable: the copy would believe it watched components that do not
   *  list it. */
  MouseListener(const MouseListener&) = delete;
  MouseListener& operator=(const MouseListener&) = delete;  ///< Deleted: see the copy constructor.
  /** Detaches from every component it was added to. */
  virtual ~MouseListener();

  /**
   * A move with no button held.
   *
   * Like every hook here, called with the same event just BEFORE the
   * component's own handler -- for the component this listener was added to
   * and, with `nestedChildren`, for anything inside it, `eventComponent` saying
   * which. Positions are in that component's coordinates, not the watched one's.
   */
  virtual void mouseMove(const MouseEvent&) {}
  /** The pointer came over the component, or over one inside it for a
   *  listener added with `nestedChildren`. */
  virtual void mouseEnter(const MouseEvent&) {}
  /** The pointer left the component. */
  virtual void mouseExit(const MouseEvent&) {}
  /** A button went down. Unlike Component::mouseDown this also hears the right
   *  button, with `rightButton` set, just before the component's contextMenu. */
  virtual void mouseDown(const MouseEvent&) {}
  /** A move with the button held, to the component that was pressed. */
  virtual void mouseDrag(const MouseEvent&) {}
  /** The button came up, to the component that was pressed. */
  virtual void mouseUp(const MouseEvent&) {}
  /** The wheel turned over the component, `delta` in notches as for
   *  Component::mouseWheel. Heard once per event, for the component under the
   *  pointer, before the wheel is offered up its parents. */
  virtual void mouseWheelMove(const MouseEvent&, float delta) { (void) delta; }

private:
  friend class Component;
  std::vector<Component*> watching_;
};

/**
 * A component's pixels, kept so it can be drawn again without being painted.
 *
 * The interface; StandardCachedComponentImage below is the implementation
 * setBufferedToImage installs. Owned by the component. paint() is called in
 * place of the component's whole subtree being painted, with the Graphics
 * already clipped to the component and translated into its coordinates.
 */
class CachedComponentImage {
public:
  /** Virtual: a component owns its cache through this interface. */
  virtual ~CachedComponentImage() = default;
  /** Draw the cached pixels in place of the component's subtree; an
   *  implementation renders them again first when they are stale. `g` is
   *  clipped to the component and in its coordinates. */
  virtual void paint(Graphics& g) = 0;
  /** Everything is stale. Returns false if the cache could not be kept. */
  virtual bool invalidateAll() = 0;
  /** `area` is in the component's own coordinates. */
  virtual bool invalidate(const Rect& area) = 0;
  /** Drop the pixels; the next paint renders them again. */
  virtual void releaseResources() = 0;
};

/**
 * Something that accepts TEXT dragged onto it -- from a field in the same
 * editor, or from another application.
 *
 * A component opts in by deriving from this as well and answering
 * asTextDragAndDropTarget() with `this` -- the asWidget() pattern, because the
 * SDK builds without RTTI. Internal drags reach it through the router's drag
 * machinery (MouseRouter::startTextDrag); drags from outside reach it through
 * a TextDragDispatcher that a platform's drop target drives.
 *
 * Positions are in the receiving component's own coordinates.
 */
class TextDragAndDropTarget {
public:
  virtual ~TextDragAndDropTarget() = default;
  /** Whether this target would take `text`. Asked as the pointer moves over it,
   *  from the component under the pointer upwards, before any other call. */
  virtual bool isInterestedInTextDrag(const std::string& text) = 0;
  /** The text drag came over this target: the moment to show where it would
   *  land. */
  virtual void textDragEnter(const std::string& text, Point position) {
    (void) text;
    (void) position;
  }
  /** Each move while the text drag is over this target. */
  virtual void textDragMove(const std::string& text, Point position) {
    (void) text;
    (void) position;
  }
  /** The text drag left, was abandoned, or is about to be dropped here --
   *  textDropped follows in that case. */
  virtual void textDragExit(const std::string& text) { (void) text; }
  /** The text was dropped at `position`. The enter/exit pair is already closed. */
  virtual void textDropped(const std::string& text, Point position) = 0;
};

/**
 * Told about every component's paint, for as long as one is installed -- what
 * ComponentPaintDiagnostics hangs on. Null by default, and then painting costs
 * one pointer test per component.
 */
class ComponentPaintObserver {
public:
  virtual ~ComponentPaintObserver() = default;
  /** Just before the component's own paint(), with `g` as paint() will get it. */
  virtual void willPaint(Component&, Graphics&) {}
  /** Just after the component's own paint(), before its children paint. */
  virtual void didPaint(Component&, Graphics&) {}
  /** After a ROOT component's whole tree, still inside its clip -- the point
   *  at which a repaint-flash overlay goes on top of everything. */
  virtual void didPaintTree(Component&, Graphics&) {}
};

namespace detail {

/** The installed paint observer, or null. */
inline ComponentPaintObserver*& paintObserver() {
  static ComponentPaintObserver* observer = nullptr;
  return observer;
}

/** How many mouse listeners are attached anywhere. Zero means the router
 *  skips the listener walk entirely, which is every editor that never asked
 *  for one. */
inline int& mouseListenerAttachments() {
  static int count = 0;
  return count;
}

/** Every MouseListener alive. A dispatch snapshots the listeners it will call
 *  and checks each is still here before calling it, so one listener's handler
 *  destroying another is a skipped call rather than a call into freed memory. */
inline std::vector<MouseListener*>& liveMouseListeners() {
  static std::vector<MouseListener*> all;
  return all;
}

/**
 * "Was this component destroyed while I was calling out?"
 *
 * A listener's handler is user code and may delete the component it is being
 * told about. The watch is a stack-allocated record on an intrusive list; a
 * component's destructor marks every watch on it, and the caller checks its
 * own record after each call-out instead of touching the component. No
 * allocation, no weak pointers, and nothing per component.
 */
struct DeletionWatch {
  explicit DeletionWatch(const Component* c) : watched(c), next(head()) { head() = this; }
  ~DeletionWatch() {
    for (DeletionWatch** p = &head(); *p; p = &(*p)->next)
      if (*p == this) {
        *p = next;
        break;
      }
  }
  DeletionWatch(const DeletionWatch&) = delete;
  DeletionWatch& operator=(const DeletionWatch&) = delete;

  static DeletionWatch*& head() {
    static DeletionWatch* first = nullptr;
    return first;
  }
  static void componentDying(const Component* c) {
    for (DeletionWatch* w = head(); w; w = w->next)
      if (w->watched == c) w->deleted = true;
  }

  const Component* watched;
  DeletionWatch* next;
  bool deleted = false;
};

} // namespace detail

} // namespace gfx
} // namespace sonore
