// SPDX-License-Identifier: Apache-2.0
//
// A set of rectangles, and the reason it exists: partial repaint.
//
// ── What was wrong ──────────────────────────────────────────────────────────
//
// Component damage was ONE rectangle, united. Two knobs at opposite corners of
// an editor moving at once produced a damaged area covering everything between
// them -- which for a wide editor is the whole thing. window_win32.h has said
// so in a comment since it was written: "the damaged rectangle is currently the
// whole window rather than the union the tree reports -- correct, and wasteful.
// The damage IS tracked; using it is the optimisation, and it is not free."
//
// It matters more here than in an application. A plugin editor repaints at 30
// Hz for as long as it is open, inside a process that is also running an audio
// callback every few milliseconds, on the same CPU. Time spent rasterising
// pixels nobody asked for is time the audio thread might have wanted.
//
// ── Why a list rather than a proper region ──────────────────────────────────
//
// A real region algebra -- subtract, xor, canonical decomposition -- is a
// substantial piece of computational geometry, and a plugin editor's damage is
// a handful of rectangles that are usually disjoint already. So: a list, with
// merging of anything that overlaps or touches, and a cap after which it gives
// up and unions everything. The cap is the honest part: past a certain count,
// testing each rectangle costs more than redrawing the lot.
#pragma once

#include <algorithm>
#include <vector>

#include "geometry.h"

namespace sonore {
namespace gfx {

class RectangleList {
public:
  /**
   * Past this many rectangles it collapses to a single bounding box.
   *
   * Eight is chosen from what an editor actually damages in one frame: a
   * handful of controls under the mouse, a meter, a readout. Beyond that the
   * per-rectangle intersection test costs more than the pixels it saves, and a
   * list that grew without limit would turn a busy frame into a linear scan per
   * component per paint.
   */
  static constexpr int kMaxRectangles = 8;

  bool isEmpty() const { return rects_.empty(); }
  int size() const { return (int) rects_.size(); }
  const Rect& at(int i) const { return rects_[(size_t) i]; }
  const std::vector<Rect>& rects() const { return rects_; }

  void clear() { rects_.clear(); }

  /**
   * Add a rectangle, merging it with anything it overlaps or touches.
   *
   * Merging on TOUCH as well as overlap matters: two damaged rows of a list
   * that share an edge are one repaint, and keeping them separate costs two
   * clip setups to save nothing.
   */
  void add(const Rect& r) {
    if (r.w <= 0.0f || r.h <= 0.0f) return;

    Rect merged = r;
    // Repeatedly: absorb anything the growing rectangle now touches. One pass
    // is not enough -- absorbing A can make it touch B, which it did not touch
    // before.
    bool absorbedSomething = true;
    while (absorbedSomething) {
      absorbedSomething = false;
      for (size_t i = 0; i < rects_.size();) {
        if (touchesOrOverlaps(merged, rects_[i])) {
          merged = merged.united(rects_[i]);
          rects_.erase(rects_.begin() + (long) i);
          absorbedSomething = true;
        } else {
          ++i;
        }
      }
    }

    rects_.push_back(merged);
    if ((int) rects_.size() > kMaxRectangles) collapse();
  }

  void add(const RectangleList& other) {
    for (const Rect& r : other.rects_) add(r);
  }

  /** The single rectangle containing everything. What a caller uses when it
   *  cannot do anything with a list. */
  Rect bounds() const {
    if (rects_.empty()) return {};
    Rect out = rects_[0];
    for (size_t i = 1; i < rects_.size(); ++i) out = out.united(rects_[i]);
    return out;
  }

  /** Whether any rectangle touches `r`. What a paint uses to decide whether a
   *  component is worth visiting at all. */
  bool intersects(const Rect& r) const {
    for (const Rect& mine : rects_)
      if (overlaps(mine, r)) return true;
    return false;
  }

  float totalArea() const {
    float sum = 0.0f;
    for (const Rect& r : rects_) sum += r.w * r.h;
    return sum;
  }

  /** Everything, as one. Called when the list has grown past its cap, and by a
   *  caller that would rather have one rectangle. */
  void collapse() {
    if (rects_.size() <= 1) return;
    const Rect all = bounds();
    rects_.clear();
    rects_.push_back(all);
  }

private:
  static bool overlaps(const Rect& a, const Rect& b) {
    return a.x < b.right() && b.x < a.right() && a.y < b.bottom() && b.y < a.bottom();
  }

  /** Overlapping, or sharing an edge. The tolerance is a pixel: two rectangles
   *  a fraction apart are not worth two clip setups, and floating point means
   *  "exactly adjacent" is not a thing that reliably happens. */
  static bool touchesOrOverlaps(const Rect& a, const Rect& b) {
    const float slack = 1.0f;
    return a.x < b.right() + slack && b.x < a.right() + slack &&
           a.y < b.bottom() + slack && b.y < a.bottom() + slack;
  }

  std::vector<Rect> rects_;
};

} // namespace gfx
} // namespace sonore
