// SPDX-License-Identifier: Apache-2.0
//
// ComponentMovementWatcher: told when a component moves on screen -- because it
// moved, or because ANY of its parents did -- when it is resized, when it might
// have been shown or hidden, and when the window it is in appears or goes.
//
// ── Why the component's own listener is not enough ──────────────────────────
//
// ComponentListener::componentMovedOrResized fires when a component's bounds
// change, and bounds are relative to the parent. A knob inside a panel that
// slides in from the side has not moved at all by that measure, and is
// nevertheless somewhere else on the screen. Anything tracking a component's
// place in the WINDOW -- a native child view laid over it, a tooltip or popup
// anchored to it, an accessibility rectangle, an overlay drawn in another tree
// -- needs to hear about every ancestor's move too, and has to re-attach
// itself whenever the chain of ancestors changes. That bookkeeping is this
// class.
//
// ── What counts as a move ───────────────────────────────────────────────────
//
// The component's top-left in its ROOT's coordinates, compared with the last
// time one was reported; its size likewise. So a parent resized without
// moving the child reports nothing, a parent moved reports wasMoved, and a
// re-parenting that happens to leave it at the same place reports nothing --
// what the watcher's users care about is where it IS, not what happened.
//
// "Peer" here is the window's MouseRouter: what puts a tree in a window.
// componentPeerChanged fires when routerFor(component) changes -- the tree was
// put in a window, taken out of one, or the component moved between trees.
#pragma once

#include <algorithm>
#include <functional>
#include <vector>

#include "component.h"

namespace sonore {
namespace gfx {

/**
 * Watches one component's place on screen, through every ancestor -- the
 * reference's ComponentMovementWatcher. The file comment says what counts as
 * a move and what "peer" means here.
 *
 * Derive and override the three protected functions, or set the matching
 * onMovedOrResized / onPeerChanged / onVisibilityChanged. It does not own the
 * component, and survives it: getComponent() turns null when the component is
 * destroyed, and the watcher can be destroyed at any time after. The
 * notifications arrive inside the setBounds, addChild or setVisible call that
 * caused them -- message thread only.
 */
class ComponentMovementWatcher : private ComponentListener {
public:
  /** Watch `componentToWatch` (not owned) and its ancestors. Where it is now
   *  is the starting point: nothing is reported for it. Null watches
   *  nothing. */
  explicit ComponentMovementWatcher(Component* componentToWatch) : component_(componentToWatch) {
    if (component_) {
      lastPosition_ = component_->localToRoot({0.0f, 0.0f});
      lastSize_ = Point(component_->bounds().w, component_->bounds().h);
      lastPeer_ = routerFor(component_);
      lastShowing_ = component_->isShowing();
    }
    registerChain();
  }

  /** Stops listening; the component may already be gone. */
  ~ComponentMovementWatcher() override = default; // ComponentListener detaches itself

  /** Not copyable: it is a listener on each component of the chain. */
  ComponentMovementWatcher(const ComponentMovementWatcher&) = delete;
  /** Not assignable, for the same reason. */
  ComponentMovementWatcher& operator=(const ComponentMovementWatcher&) = delete;

  /** Null once the watched component has been destroyed. */
  Component* getComponent() const { return component_; }

  /** The same three notifications, for a caller with no class to derive. */
  std::function<void(bool wasMoved, bool wasResized)> onMovedOrResized;
  /** As componentPeerChanged(). */
  std::function<void()> onPeerChanged;
  /** As componentVisibilityChanged(). */
  std::function<void()> onVisibilityChanged;

protected:
  /** Its top-left in its root's coordinates changed (`wasMoved`), or its
   *  size did (`wasResized`); at least one is true. The default calls
   *  onMovedOrResized. */
  virtual void componentMovedOrResized(bool wasMoved, bool wasResized) {
    if (onMovedOrResized) onMovedOrResized(wasMoved, wasResized);
  }
  /** routerFor(component) changed: its tree was put in a window, taken out
   *  of one, or it moved to another tree. The default calls onPeerChanged. */
  virtual void componentPeerChanged() {
    if (onPeerChanged) onPeerChanged();
  }
  /** Its isShowing() changed: it or an ancestor was shown or hidden, or it
   *  moved under one that is. The default calls onVisibilityChanged. */
  virtual void componentVisibilityChanged() {
    if (onVisibilityChanged) onVisibilityChanged();
  }

private:
  /** Listen to the component and every ancestor -- and to nothing else. */
  void registerChain() {
    std::vector<Component*> chain;
    for (Component* c = component_; c != nullptr; c = c->parent()) chain.push_back(c);
    // What this is attached to is read back from the components' own record
    // (watchedComponents), never kept in a list of its own: a separate list
    // could hold a component that was destroyed, or that refused the
    // attachment because it was being destroyed.
    const std::vector<Component*> attached = watchedComponents();
    for (Component* c : attached)
      if (std::find(chain.begin(), chain.end(), c) == chain.end()) c->removeComponentListener(this);
    for (Component* c : chain) c->addComponentListener(this);
  }

  void checkPeer() {
    MouseRouter* now = component_ ? routerFor(component_) : nullptr;
    if (now == lastPeer_) return;
    lastPeer_ = now;
    componentPeerChanged();
  }

  void checkMovement() {
    if (!component_) return;
    const Point pos = component_->localToRoot({0.0f, 0.0f});
    const Point size(component_->bounds().w, component_->bounds().h);
    const bool moved = pos.x != lastPosition_.x || pos.y != lastPosition_.y;
    const bool resized = size.x != lastSize_.x || size.y != lastSize_.y;
    lastPosition_ = pos;
    lastSize_ = size;
    if (moved || resized) componentMovedOrResized(moved, resized);
  }

  void checkVisibility() {
    if (!component_) return;
    const bool showing = component_->isShowing();
    if (showing == lastShowing_) return;
    lastShowing_ = showing;
    componentVisibilityChanged();
  }

  // ── ComponentListener ────────────────────────────────────────────────────

  void componentMovedOrResized(Component&, bool, bool) override { checkMovement(); }

  void componentParentHierarchyChanged(Component&) override {
    registerChain();
    checkPeer();
    checkMovement();
    checkVisibility();
  }

  void componentVisibilityChanged(Component&) override { checkVisibility(); }

  void componentBeingDeleted(Component& c) override {
    if (&c != component_) return;
    component_ = nullptr;
    // The watched component's ancestors are no longer of interest.
    const std::vector<Component*> attached = watchedComponents();
    for (Component* other : attached)
      if (other != &c) other->removeComponentListener(this);
  }

  Component* component_;
  Point lastPosition_, lastSize_;
  MouseRouter* lastPeer_ = nullptr;
  bool lastShowing_ = false;
};

} // namespace gfx
} // namespace sonore
