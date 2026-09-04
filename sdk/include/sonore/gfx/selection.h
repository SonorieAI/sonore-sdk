// SPDX-License-Identifier: Apache-2.0
//
// Choosing several things, and moving them.
//
// ── What it is for ──────────────────────────────────────────────────────────
//
// A step sequencer where you drag a box round eight steps. A modular patch
// where nodes move. An envelope with points you can grab. The moment an editor
// has more than one of something, "which ones" and "move them" stop being
// obvious and start being the two things everybody implements slightly
// differently.
//
// ── The click rules ─────────────────────────────────────────────────────────
//
// These are not arbitrary and they are not ours: a plain click selects only the
// thing clicked, Ctrl toggles it, and a drag on empty space starts a lasso.
// Every user already knows them from every other application, and a plugin that
// invented its own would be one people have to learn.
//
// The one worth stating: a plain click on something ALREADY selected must not
// collapse the selection to it, or dragging a group of four becomes impossible
// -- the press deselects the other three before the drag begins. So the
// collapse happens on RELEASE, and only if nothing was dragged.
#pragma once

#include <algorithm>
#include <functional>
#include <vector>

#include "component.h"
#include "lookandfeel.h"
#include "widgets.h"

namespace sonore {
namespace gfx {

/**
 * The set of things currently chosen.
 *
 * A vector rather than a set: a selection is a handful of items, ORDER matters
 * for "the first one" and "the last one", and a caller iterating it wants the
 * order they clicked in rather than whatever a hash produced.
 */
template <typename ItemType>
class SelectedItemSet {
public:
  using Items = std::vector<ItemType>;

  const Items& items() const { return items_; }
  int size() const { return (int) items_.size(); }
  bool isEmpty() const { return items_.empty(); }

  bool isSelected(const ItemType& item) const {
    return std::find(items_.begin(), items_.end(), item) != items_.end();
  }

  /** Called whenever the set changes -- once per change, not once per item, so
   *  selecting forty things repaints once. */
  std::function<void()> onChange;

  void selectOnly(const ItemType& item) {
    if (items_.size() == 1 && items_[0] == item) return;
    items_.clear();
    items_.push_back(item);
    changed();
  }

  void addToSelection(const ItemType& item) {
    if (isSelected(item)) return;
    items_.push_back(item);
    changed();
  }

  void deselect(const ItemType& item) {
    const auto at = std::find(items_.begin(), items_.end(), item);
    if (at == items_.end()) return;
    items_.erase(at);
    changed();
  }

  void toggleSelection(const ItemType& item) {
    if (isSelected(item)) deselect(item);
    else addToSelection(item);
  }

  void deselectAll() {
    if (items_.empty()) return;
    items_.clear();
    changed();
  }

  /** Replace the whole selection at once. One notification, which is what a
   *  lasso covering forty items needs. */
  void setSelection(Items items) {
    if (items == items_) return;
    items_ = std::move(items);
    changed();
  }

  /**
   * What a mouse-DOWN on `item` should do.
   *
   * Ctrl toggles. Otherwise, if the item is already selected, NOTHING -- see
   * the header: collapsing here makes a group impossible to drag, because the
   * press would deselect the rest before the drag started.
   *
   * Returns whether the caller should collapse to this item on release if no
   * drag happened, so the plain-click-on-a-selected-item case still ends up
   * selecting only it.
   */
  bool addToSelectionOnMouseDown(const ItemType& item, bool ctrlDown) {
    if (ctrlDown) {
      toggleSelection(item);
      return false;
    }
    if (isSelected(item)) return true; // decide on release
    selectOnly(item);
    return false;
  }

  /** The other half: a plain click that turned out not to be a drag. */
  void addToSelectionOnMouseUp(const ItemType& item, bool ctrlDown, bool wasDragged) {
    if (ctrlDown || wasDragged) return;
    selectOnly(item);
  }

private:
  void changed() {
    if (onChange) onChange();
  }

  Items items_;
};

/**
 * The rubber band.
 *
 * Owns only the rectangle and its drawing. WHAT it covers is the owner's
 * business -- this has no idea what the items are, where they are, or how to
 * compare them, and a version that did would need a model interface for every
 * kind of thing anybody ever lassoes.
 */
class LassoComponent : public Widget {
public:
  LassoComponent() {
    // It is drawn over everything and must not take clicks: the press that
    // started it went to whatever is underneath, and the release has to reach
    // there too.
    setInterceptsMouse(false);
    setVisible(false);
    setAccessibilityIgnored(true);
  }

  /** Called on every drag with the current rectangle, in the LASSO's own
   *  coordinates -- which is its parent's, since it covers it. */
  std::function<void(const Rect& area, bool addToExisting)> onLassoChanged;
  std::function<void()> onLassoEnded;

  void beginLasso(Point start, bool addToExisting) {
    if (Component* p = parent()) setBounds({0.0f, 0.0f, p->bounds().w, p->bounds().h});
    start_ = start;
    current_ = start;
    addToExisting_ = addToExisting;
    active_ = true;
    setVisible(true);
    repaint();
  }

  void dragLasso(Point to) {
    if (!active_) return;
    current_ = to;
    repaint();
    if (onLassoChanged) onLassoChanged(area(), addToExisting_);
  }

  void endLasso() {
    if (!active_) return;
    active_ = false;
    setVisible(false);
    repaint();
    if (onLassoEnded) onLassoEnded();
  }

  bool isActive() const { return active_; }

  /** Normalised, so dragging up-and-left gives the same rectangle as
   *  down-and-right. Every lasso that forgets this selects nothing in three of
   *  the four directions. */
  Rect area() const {
    const float x = start_.x < current_.x ? start_.x : current_.x;
    const float y = start_.y < current_.y ? start_.y : current_.y;
    const float w = start_.x < current_.x ? current_.x - start_.x : start_.x - current_.x;
    const float h = start_.y < current_.y ? current_.y - start_.y : start_.y - current_.y;
    return {x, y, w, h};
  }

  void paint(Graphics& g) override {
    if (!active_) return;
    const Rect r = area();
    if (r.w < 1.0f || r.h < 1.0f) return;
    LookAndFeel& lf = lookAndFeel();
    const Colour accent = lf.accent();
    // A translucent fill and a solid edge, which is what every rubber band
    // looks like -- the fill is what makes it read as an area rather than as
    // four lines somebody drew.
    g.setColour(Colour(accent.r, accent.g, accent.b, 40));
    g.fillRect(r);
    g.setColour(accent);
    g.drawRect(r, 1.0f);
  }

private:
  Point start_, current_;
  bool active_ = false;
  bool addToExisting_ = false;
};

/**
 * Moving a component with the mouse.
 *
 * Works in ROOT coordinates, which is the whole trick. A dragger that used the
 * event's local position would move the component, which moves the coordinate
 * system the next event is reported in, which moves the component again -- the
 * classic runaway where a dragged thing accelerates away from the pointer.
 */
class ComponentDragger {
public:
  void startDraggingComponent(Component* component, const MouseEvent& e) {
    if (!component) return;
    startBounds_ = component->bounds();
    startRoot_ = e.rootPosition;
    dragging_ = true;
  }

  /**
   * `keepWithin` is in the component's PARENT's coordinates, and empty means
   * anywhere. Constraining is not optional in practice: a node dragged outside
   * its panel is one the user cannot reach again.
   */
  void dragComponent(Component* component, const MouseEvent& e, Rect keepWithin = {}) {
    if (!component || !dragging_) return;
    const float dx = e.rootPosition.x - startRoot_.x;
    const float dy = e.rootPosition.y - startRoot_.y;
    float x = startBounds_.x + dx;
    float y = startBounds_.y + dy;

    if (keepWithin.w > 0.0f && keepWithin.h > 0.0f) {
      // Clamped so the whole component stays inside, and the LEFT edge wins
      // where it cannot: a component larger than the area it is confined to
      // should sit at the corner rather than jump to the far side.
      if (x + startBounds_.w > keepWithin.right()) x = keepWithin.right() - startBounds_.w;
      if (y + startBounds_.h > keepWithin.bottom()) y = keepWithin.bottom() - startBounds_.h;
      if (x < keepWithin.x) x = keepWithin.x;
      if (y < keepWithin.y) y = keepWithin.y;
    }

    component->setBounds({x, y, startBounds_.w, startBounds_.h});
  }

  void endDrag() { dragging_ = false; }
  bool isDragging() const { return dragging_; }

  /** How far the pointer has travelled since the press. What a caller uses to
   *  decide whether a click was a click or the beginning of a drag -- below a
   *  few pixels it is a click, and treating every press as a drag makes a
   *  selection impossible to make with a trackpad. */
  float distanceFrom(const MouseEvent& e) const {
    const float dx = e.rootPosition.x - startRoot_.x;
    const float dy = e.rootPosition.y - startRoot_.y;
    return dx * dx + dy * dy > 0.0f ? std::sqrt(dx * dx + dy * dy) : 0.0f;
  }

private:
  Rect startBounds_;
  Point startRoot_;
  bool dragging_ = false;
};

} // namespace gfx
} // namespace sonore
