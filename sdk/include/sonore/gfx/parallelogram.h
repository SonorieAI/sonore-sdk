// SPDX-License-Identifier: Apache-2.0
//
// Parallelogram: a rectangle after an affine transform, as three corners.
//
// ── Why three corners and not a transform ───────────────────────────────────
//
// They are the same information, and drawImageTransformed takes the
// transform. What the corners give is the other direction: a place stated as
// where the picture's corners LAND -- a label's backing plate skewed to sit
// on a drawn panel, a cover image mapped onto the face of a 3D box, the
// handles of a free-transform tool -- with the transform worked out from
// them (transformFrom) rather than composed by hand from a rotation, a shear
// and two scales nobody can read back. And a shape that a transform made is
// one that can be asked things a transform cannot answer directly: does it
// contain this point, how big is it, what rectangle bounds it.
//
// The fourth corner is never stored: it is topRight + bottomLeft - topLeft,
// so a Parallelogram cannot be built that is not one.
#pragma once

#include <cmath>

#include "geometry.h"
#include "path.h"

namespace sonore {
namespace gfx {

/**
 * A rectangle after an affine transform, kept as three of its corners. The
 * reference's Parallelogram, in float; the file comment says why corners and
 * not a transform, and why the fourth is never stored.
 *
 * A plain value with no invariants to keep: any three points are a valid
 * parallelogram, and one whose corners lie on a line is empty and contains
 * nothing (toRelative answers false for it).
 */
struct Parallelogram {
  /** The three stored corners, in whatever coordinates the caller uses: the
   *  images of a rectangle's top-left, top-right and bottom-left. */
  Point topLeft,
      topRight,   ///< The image of the rectangle's top-right corner.
      bottomLeft; ///< The image of the rectangle's bottom-left corner.

  /** All three corners at the origin: an empty parallelogram. */
  Parallelogram() = default;
  /** From three corners; the fourth is `tr + bl - tl`. */
  Parallelogram(Point tl, Point tr, Point bl) : topLeft(tl), topRight(tr), bottomLeft(bl) {}
  /** A rectangle's own corners: the parallelogram with no turn in it. */
  explicit Parallelogram(const Rect& r)
      : topLeft(r.x, r.y), topRight(r.right(), r.y), bottomLeft(r.x, r.bottom()) {}

  /** The fourth corner, worked out: topRight + bottomLeft - topLeft. */
  Point getBottomRight() const {
    return {topRight.x + bottomLeft.x - topLeft.x, topRight.y + bottomLeft.y - topLeft.y};
  }

  /** The length of the top edge, topLeft to topRight. */
  float getWidth() const { return topLeft.distanceTo(topRight); }
  /** The length of the left edge, topLeft to bottomLeft. */
  float getHeight() const { return topLeft.distanceTo(bottomLeft); }

  /** Its area: the cross product of its two edges. */
  float area() const {
    const float ux = topRight.x - topLeft.x, uy = topRight.y - topLeft.y;
    const float vx = bottomLeft.x - topLeft.x, vy = bottomLeft.y - topLeft.y;
    return std::fabs(ux * vy - uy * vx);
  }

  /** No area: its edges lie along one line, or one of them has no length. */
  bool isEmpty() const { return !(area() > 0.0f); }

  /** A point given as a fraction along each edge: (0,0) the top-left
   *  corner, (1,1) the bottom-right, (0.5,0.5) the centre. */
  Point getRelativePoint(Point relative) const {
    return {topLeft.x + (topRight.x - topLeft.x) * relative.x + (bottomLeft.x - topLeft.x) * relative.y,
            topLeft.y + (topRight.y - topLeft.y) * relative.x + (bottomLeft.y - topLeft.y) * relative.y};
  }

  /** The other way: where `p` is as fractions along the edges. False for an
   *  empty parallelogram, which has no inside to measure in. */
  bool toRelative(Point p, Point& relative) const {
    const float ux = topRight.x - topLeft.x, uy = topRight.y - topLeft.y;
    const float vx = bottomLeft.x - topLeft.x, vy = bottomLeft.y - topLeft.y;
    const float det = ux * vy - uy * vx;
    if (!(std::fabs(det) > 0.0f) || !std::isfinite(det)) return false;
    const float dx = p.x - topLeft.x, dy = p.y - topLeft.y;
    relative = {(dx * vy - dy * vx) / det, (ux * dy - uy * dx) / det};
    return std::isfinite(relative.x) && std::isfinite(relative.y);
  }

  /** Inside, as Rect::contains is: the top and left edges in, the bottom
   *  and right out -- so two parallelograms sharing an edge do not both
   *  claim a point on it. */
  bool contains(Point p) const {
    Point r;
    if (!toRelative(p, r)) return false;
    return r.x >= 0.0f && r.x < 1.0f && r.y >= 0.0f && r.y < 1.0f;
  }

  /** The smallest upright rectangle holding all four corners. */
  Rect getBoundingBox() const {
    const Point br = getBottomRight();
    const float x0 = (std::fmin)((std::fmin)(topLeft.x, topRight.x), (std::fmin)(bottomLeft.x, br.x));
    const float x1 = (std::fmax)((std::fmax)(topLeft.x, topRight.x), (std::fmax)(bottomLeft.x, br.x));
    const float y0 = (std::fmin)((std::fmin)(topLeft.y, topRight.y), (std::fmin)(bottomLeft.y, br.y));
    const float y1 = (std::fmax)((std::fmax)(topLeft.y, topRight.y), (std::fmax)(bottomLeft.y, br.y));
    return {x0, y0, x1 - x0, y1 - y0};
  }

  /** An affine transform of a parallelogram is a parallelogram: its three
   *  corners transformed say all of it. */
  Parallelogram transformedBy(const Transform& t) const {
    return {t.apply(topLeft), t.apply(topRight), t.apply(bottomLeft)};
  }

  /** Moved by (dx, dy), its shape kept. */
  Parallelogram translated(float dx, float dy) const {
    return {topLeft + Point(dx, dy), topRight + Point(dx, dy), bottomLeft + Point(dx, dy)};
  }

  /**
   * The transform that puts `source` -- a picture's bounds, usually -- onto
   * this: its top-left corner to topLeft, top-right to topRight, bottom-left
   * to bottomLeft. Hand it to Graphics::drawImageTransformed with
   * Rect(0, 0, w, h) for the picture. An empty source has no such transform
   * and gives the identity.
   */
  Transform transformFrom(const Rect& source) const {
    if (!(source.w != 0.0f) || !(source.h != 0.0f)) return Transform();
    const float a = (topRight.x - topLeft.x) / source.w, b = (topRight.y - topLeft.y) / source.w;
    const float c = (bottomLeft.x - topLeft.x) / source.h, d = (bottomLeft.y - topLeft.y) / source.h;
    return {a, b, c, d, topLeft.x - a * source.x - c * source.y, topLeft.y - b * source.x - d * source.y};
  }

  /** Its outline, clockwise on screen from the top-left corner. */
  Path toPath() const {
    Path p;
    const Point br = getBottomRight();
    p.moveTo(topLeft.x, topLeft.y);
    p.lineTo(topRight.x, topRight.y);
    p.lineTo(br.x, br.y);
    p.lineTo(bottomLeft.x, bottomLeft.y);
    p.close();
    return p;
  }

  /** The same three corners, compared exactly. */
  bool operator==(const Parallelogram& o) const {
    return topLeft.x == o.topLeft.x && topLeft.y == o.topLeft.y && topRight.x == o.topRight.x &&
           topRight.y == o.topRight.y && bottomLeft.x == o.bottomLeft.x && bottomLeft.y == o.bottomLeft.y;
  }
  /** Any corner differs. */
  bool operator!=(const Parallelogram& o) const { return !(*this == o); }
};

} // namespace gfx
} // namespace sonore
