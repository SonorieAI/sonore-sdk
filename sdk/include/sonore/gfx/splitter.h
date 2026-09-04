// SPDX-License-Identifier: Apache-2.0
//
// Panes the user can resize, and the bar between them.
//
// ── What it is for ──────────────────────────────────────────────────────────
//
// A sample browser down the left and the sample's detail on the right. A preset
// list above and its parameters below. The moment an editor has two regions
// whose right proportion depends on what the user is doing, a fixed split is
// wrong for somebody -- and the somebody is usually the one with a long preset
// name or a deep folder tree.
//
// FlexBox already lays a row out. What it cannot do is let the USER move the
// boundary and have that survive the next resize, which is the whole of what
// this adds.
//
// ── Sizes, and what a negative one means ───────────────────────────────────
//
// The usual convention, because it is genuinely the clearest: a
// POSITIVE size is pixels, a NEGATIVE one is a fraction of the total. So
// (-0.25, -0.75, -0.5) is "between a quarter and three quarters, preferring
// half", and it stays that when the window is resized -- where a pixel
// preference would drift to nothing on a small editor and to a stripe on a
// large one.
//
// ── The algorithm ───────────────────────────────────────────────────────────
//
// The same freeze loop FlexBox uses, for the same reason: start everything at
// its preferred size, share the difference among the items that can still
// move, freeze whatever hits a limit, and go round again. The naive
// alternative -- distribute once, then clamp -- leaves the clamped items'
// share unspent, so a row of panes comes up short by exactly the amount its
// constrained members refused.
#pragma once

#include <cmath>
#include <functional>
#include <vector>

#include "widgets.h"

namespace sonore {
namespace gfx {

class StretchableLayoutManager {
public:
  /**
   * Describe one item.
   *
   * Negative means a fraction of the total; positive means pixels. A resizer
   * bar is an ordinary item with all three the same, which is what makes it
   * stay the width it is while everything around it moves.
   */
  void setItemLayout(int index, double minSize, double maxSize, double preferredSize) {
    if (index < 0) return;
    if (index >= (int) items_.size()) items_.resize((size_t) index + 1);
    Item& item = items_[(size_t) index];
    item.minSize = minSize;
    item.maxSize = maxSize;
    item.preferred = preferredSize;
    item.set = true;
  }

  int numItems() const { return (int) items_.size(); }

  /** The size item `index` was last given, in pixels. */
  double itemSize(int index) const {
    if (index < 0 || index >= (int) sizes_.size()) return 0.0;
    return sizes_[(size_t) index];
  }

  /**
   * Work out every item's size for a total of `total` pixels.
   *
   * Called by layOutComponents, and separately by anything that needs the
   * numbers without components -- a test, or a caller drawing regions itself.
   */
  void layOut(double total) {
    sizes_.assign(items_.size(), 0.0);
    if (items_.empty() || total <= 0.0) return;

    // Fractions resolve against the total ONCE, here, so everything below is
    // in pixels and there is no second place that has to remember which
    // convention a number is in.
    std::vector<double> minimum(items_.size()), maximum(items_.size()),
        preferred(items_.size());
    for (size_t i = 0; i < items_.size(); ++i) {
      const Item& item = items_[i];
      minimum[i] = resolve(item.minSize, total);
      maximum[i] = resolve(item.maxSize, total);
      preferred[i] = resolve(item.preferred, total);
      if (maximum[i] < minimum[i]) maximum[i] = minimum[i];
      // A remembered size from a previous drag wins over the preference: that
      // is what makes a boundary the user moved stay where they put it.
      if (item.explicitSize >= 0.0) preferred[i] = item.explicitSize;
      sizes_[i] = preferred[i] < minimum[i] ? minimum[i]
                                            : (preferred[i] > maximum[i] ? maximum[i]
                                                                         : preferred[i]);
    }

    // ── The freeze loop ──
    std::vector<bool> frozen(items_.size(), false);
    for (int pass = 0; pass < 64; ++pass) {
      double used = 0.0;
      for (double s : sizes_) used += s;
      const double slack = total - used;
      if (std::fabs(slack) < 0.01) break;

      // Share among the items that can still move IN THE NEEDED DIRECTION. An
      // item already at its maximum cannot take more, and counting it in the
      // divisor is what leaves the row short.
      double movable = 0.0;
      for (size_t i = 0; i < sizes_.size(); ++i) {
        if (frozen[i]) continue;
        if (slack > 0.0 ? sizes_[i] < maximum[i] : sizes_[i] > minimum[i]) movable += 1.0;
      }
      if (movable <= 0.0) break;

      const double share = slack / movable;
      bool anyFroze = false;
      for (size_t i = 0; i < sizes_.size(); ++i) {
        if (frozen[i]) continue;
        if (!(slack > 0.0 ? sizes_[i] < maximum[i] : sizes_[i] > minimum[i])) continue;
        double wanted = sizes_[i] + share;
        if (wanted > maximum[i]) {
          wanted = maximum[i];
          frozen[i] = true;
          anyFroze = true;
        } else if (wanted < minimum[i]) {
          wanted = minimum[i];
          frozen[i] = true;
          anyFroze = true;
        }
        sizes_[i] = wanted;
      }
      if (!anyFroze) break; // everything moved freely; the slack is spent
    }
  }

  /**
   * Lay components out in a row or a column.
   *
   * `components` may contain nulls -- a gap that takes space and draws nothing
   * is a legitimate item, and refusing it would make the caller invent a
   * component for it.
   */
  void layOutComponents(Component* const* components, int count, float x, float y, float width,
                        float height, bool vertically, bool resizeOtherDimension) {
    layOut(vertically ? height : width);
    float position = vertically ? y : x;
    for (int i = 0; i < count && i < (int) sizes_.size(); ++i) {
      const float size = (float) sizes_[(size_t) i];
      if (Component* c = components[i]) {
        if (vertically)
          c->setBounds({x, position, resizeOtherDimension ? width : c->bounds().w, size});
        else
          c->setBounds({position, y, size, resizeOtherDimension ? height : c->bounds().h});
      }
      position += size;
    }
  }

  /**
   * Drag the bar at `barIndex` so its left (or top) edge lands at
   * `newPosition`.
   *
   * The panes either SIDE of the bar move -- barIndex - 1 grows and
   * barIndex + 1 gives up the same amount. Taking the bar's own index rather
   * than a pane's is what makes this callable straight from the bar, and it is
   * the arithmetic I got wrong first: pairing item N with item N+1 pairs a pane
   * with the BAR, so a hundred-pixel drag moved the boundary by the six pixels
   * the bar could spare.
   *
   * Both panes are pinned to what they end up at, so the next layout keeps the
   * split rather than snapping back to the preference. That is the difference
   * between a bar that resizes and one that appears to.
   */
  void setBoundaryPosition(int barIndex, double newPosition, double total) {
    const int firstIndex = barIndex - 1;
    const int secondIndex = barIndex + 1;
    if (firstIndex < 0 || secondIndex >= (int) items_.size()) return;
    layOut(total);

    double before = 0.0;
    for (int i = 0; i < firstIndex; ++i) before += sizes_[(size_t) i];

    const double pairTotal = sizes_[(size_t) firstIndex] + sizes_[(size_t) secondIndex];
    double first = newPosition - before;

    const double firstMin = resolve(items_[(size_t) firstIndex].minSize, total);
    const double firstMax = resolve(items_[(size_t) firstIndex].maxSize, total);
    const double secondMin = resolve(items_[(size_t) secondIndex].minSize, total);
    const double secondMax = resolve(items_[(size_t) secondIndex].maxSize, total);

    // Clamped by BOTH panes' limits: a bar respecting only the one it is
    // growing would shove the other below its minimum, and the next layout
    // would silently take the space back.
    if (first < firstMin) first = firstMin;
    if (first > firstMax) first = firstMax;
    if (pairTotal - first < secondMin) first = pairTotal - secondMin;
    if (pairTotal - first > secondMax) first = pairTotal - secondMax;
    if (first < 0.0) first = 0.0;

    items_[(size_t) firstIndex].explicitSize = first;
    items_[(size_t) secondIndex].explicitSize = pairTotal - first;
  }

  /** Forget a dragged position and go back to the preference. What a "reset
   *  layout" command does. */
  void clearExplicitSizes() {
    for (Item& item : items_) item.explicitSize = -1.0;
  }

private:
  struct Item {
    double minSize = 0.0, maxSize = 0.0, preferred = 0.0;
    /** What a drag pinned this to, or -1. */
    double explicitSize = -1.0;
    bool set = false;
  };

  /** Negative is a fraction of the total; positive is pixels. */
  static double resolve(double value, double total) {
    return value < 0.0 ? -value * total : value;
  }

  std::vector<Item> items_;
  std::vector<double> sizes_;
};

/**
 * The bar you drag.
 *
 * It owns nothing and lays nothing out: it tells the manager where the boundary
 * now is and asks its owner to lay out again. Which keeps the one piece of
 * state -- where the split is -- in the manager, where the next resize will
 * look for it.
 */
class StretchableLayoutResizerBar : public Widget {
public:
  StretchableLayoutResizerBar(StretchableLayoutManager* manager, int itemIndex, bool vertical)
      : manager_(manager), index_(itemIndex), vertical_(vertical) {
    // The cursor is the only thing that says this thin strip is draggable at
    // all. Without it a resizable editor looks exactly like a fixed one.
    setCursor(vertical ? MouseCursor::DragHorizontal : MouseCursor::DragVertical);
  }

  /** Called after a drag, with the totals to lay out against. Whatever owns
   *  the panes does the laying out; this only knows the boundary moved. */
  std::function<void()> onMoved;

  void mouseDown(const MouseEvent&) override {
    if (Component* p = parent()) totalAtDown_ = vertical_ ? p->bounds().w : p->bounds().h;
    startPosition_ = vertical_ ? bounds().x : bounds().y;
  }

  void mouseDrag(const MouseEvent& e) override {
    if (!manager_) return;
    const float moved = vertical_ ? (e.position.x - e.downPosition.x)
                                  : (e.position.y - e.downPosition.y);
    manager_->setBoundaryPosition(index_, (double) (startPosition_ + moved),
                                  (double) totalAtDown_);
    if (onMoved) onMoved();
  }

  void paint(Graphics& g) override {
    LookAndFeel& lf = lookAndFeel();
    g.setColour(lf.outline());
    // A line down the middle rather than a filled strip: the bar has to be
    // wide enough to grab -- several pixels -- and a several-pixel line looks
    // like a border somebody got wrong.
    const Rect b = localBounds();
    if (vertical_) g.fillRect(Rect(b.x + b.w * 0.5f - 0.5f, b.y, 1.0f, b.h));
    else g.fillRect(Rect(b.x, b.y + b.h * 0.5f - 0.5f, b.w, 1.0f));
  }

  AccessibleInfo accessibleInfo() const override {
    // A separator with no name and no value. Announcing it as a control would
    // put a thing in the tab order that a reader has no way to move.
    AccessibleInfo info = baseInfo(AccessibleRole::Unknown);
    return info;
  }

  static constexpr float kThickness = 6.0f;

private:
  StretchableLayoutManager* manager_;
  int index_;
  bool vertical_;
  float startPosition_ = 0.0f;
  float totalAtDown_ = 0.0f;
};

} // namespace gfx
} // namespace sonore
