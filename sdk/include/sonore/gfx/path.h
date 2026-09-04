// SPDX-License-Identifier: Apache-2.0
//
// Path: the shape everything is drawn from.
//
// ── One primitive, not many ─────────────────────────────────────────────────
//
// A rounded rectangle, a knob's pointer, a meter's cap and a piece of text
// are all the same thing to the rasteriser: a set of closed contours to be
// filled. Having one primitive means one rasteriser, one clipping rule, one
// antialiasing quality, and one place where a bug can be.
//
// ── Flattening, and the tolerance that decides quality ──────────────────────
//
// Curves are subdivided into line segments before rasterising. How finely is
// the only quality knob that matters: too coarse and a knob's arc becomes a
// visible polygon, too fine and a UI redraw costs more than the audio.
//
// The tolerance is in DEVICE pixels, not path units, so it has to be applied
// after the transform. A path scaled up four times needs four times the
// segments to look the same -- which is why flattening takes the transform
// rather than being done once when the path is built.
#pragma once

#include <cmath>
#include <cstddef>
#include <vector>

#include "geometry.h"

namespace sonore {
namespace gfx {

/**
 * Which parts of a self-overlapping path are inside.
 *
 *   NonZero  a region is inside if the contours around it wind a net
 *            non-zero number of times. Two overlapping circles drawn the same
 *            direction make one solid blob.
 *   EvenOdd  inside if crossed an odd number of times. The same two circles
 *            leave a HOLE where they overlap.
 *
 * NonZero is the default because it is what somebody drawing a shape means.
 * EvenOdd exists because it is the only way to punch a hole without caring
 * which direction the inner contour was wound -- which matters for glyphs and
 * for shapes a generator produced without thinking about winding.
 */
enum class FillRule { NonZero, EvenOdd };

/**
 * A flattened contour, and whether the author closed it.
 *
 * The FILL path does not care -- an open contour has an inside and gets one.
 * The STROKE path cares completely: an open contour gets end caps and a
 * closed one gets a join where it meets itself. Losing that distinction
 * during flattening is why this carries a flag rather than a bare point list.
 */
struct Contour {
  std::vector<Point> points;
  bool closed = false;
};

class Path {
public:
  void clear() {
    verbs_.clear();
    points_.clear();
    start_ = {};
    current_ = {};
    hasCurrent_ = false;
  }

  bool isEmpty() const { return verbs_.empty(); }

  /**
   * The same path with every point moved through `t`.
   *
   * Baked in rather than applied at draw time, which is what a caller wants
   * when the transform is FIXED -- an SVG's group transforms, say. Drawing
   * under a transform is still the right answer when it changes per frame; this
   * is for when it does not, and saves re-transforming a logo thirty times a
   * second.
   *
   * Control points move with the rest, which is correct for any affine
   * transform: a Bezier through transformed control points IS the transformed
   * Bezier. That is not true of a stroke, which is why stroking still happens
   * after flattening.
   */
  Path transformed(const Transform& t) const {
    Path out;
    out.verbs_ = verbs_;
    out.points_.reserve(points_.size());
    for (const Point& p : points_) out.points_.push_back(t.apply(p));
    return out;
  }

  /** The raw control points, for a caller measuring rather than drawing --
   *  computing a bounding box without flattening, for instance. */
  const std::vector<Point>& points() const { return points_; }

  void moveTo(float x, float y) {
    verbs_.push_back(Verb::Move);
    points_.push_back({x, y});
    start_ = {x, y};
    current_ = {x, y};
    hasCurrent_ = true;
  }

  void lineTo(float x, float y) {
    // A lineTo with no preceding moveTo would otherwise start a contour from
    // wherever the last one ended, joining two shapes that were meant to be
    // separate. Starting at the point asked for is the only harmless answer.
    if (!hasCurrent_) return moveTo(x, y);
    verbs_.push_back(Verb::Line);
    points_.push_back({x, y});
    current_ = {x, y};
  }

  void quadTo(float cx, float cy, float x, float y) {
    if (!hasCurrent_) moveTo(cx, cy);
    verbs_.push_back(Verb::Quad);
    points_.push_back({cx, cy});
    points_.push_back({x, y});
    current_ = {x, y};
  }

  void cubicTo(float c1x, float c1y, float c2x, float c2y, float x, float y) {
    if (!hasCurrent_) moveTo(c1x, c1y);
    verbs_.push_back(Verb::Cubic);
    points_.push_back({c1x, c1y});
    points_.push_back({c2x, c2y});
    points_.push_back({x, y});
    current_ = {x, y};
  }

  void close() {
    if (!hasCurrent_) return;
    verbs_.push_back(Verb::Close);
    current_ = start_;
  }

  // ── Shapes, all built from the above ──────────────────────────────────────

  void addRect(const Rect& r) {
    if (r.isEmpty()) return;
    moveTo(r.x, r.y);
    lineTo(r.right(), r.y);
    lineTo(r.right(), r.bottom());
    lineTo(r.x, r.bottom());
    close();
  }

  /**
   * A circle or ellipse, as four cubic arcs.
   *
   * kArc is the control-point distance that makes a cubic match a quarter
   * circle: 4/3 * (sqrt(2) - 1). The error is about one part in 5000 of the
   * radius, which is invisible at any size a UI uses and exact enough that a
   * symmetry test passes.
   */
  void addEllipse(const Rect& r) {
    if (r.isEmpty()) return;
    const float rx = r.w * 0.5f, ry = r.h * 0.5f;
    const float cx = r.x + rx, cy = r.y + ry;
    const float kx = rx * kArc, ky = ry * kArc;
    moveTo(cx, cy - ry);
    cubicTo(cx + kx, cy - ry, cx + rx, cy - ky, cx + rx, cy);
    cubicTo(cx + rx, cy + ky, cx + kx, cy + ry, cx, cy + ry);
    cubicTo(cx - kx, cy + ry, cx - rx, cy + ky, cx - rx, cy);
    cubicTo(cx - rx, cy - ky, cx - kx, cy - ry, cx, cy - ry);
    close();
  }

  void addRoundedRect(const Rect& r, float radius) {
    if (r.isEmpty()) return;
    // Clamped to half the shorter side. A larger radius than that is not a
    // rounder rectangle, it is corners that overlap and cross their own
    // outline -- which under NonZero fills as a bow tie.
    const float maxR = (r.w < r.h ? r.w : r.h) * 0.5f;
    float rad = radius < 0.0f ? 0.0f : (radius > maxR ? maxR : radius);
    if (rad <= 0.0f) return addRect(r);
    const float k = rad * kArc;
    moveTo(r.x + rad, r.y);
    lineTo(r.right() - rad, r.y);
    cubicTo(r.right() - rad + k, r.y, r.right(), r.y + rad - k, r.right(), r.y + rad);
    lineTo(r.right(), r.bottom() - rad);
    cubicTo(r.right(), r.bottom() - rad + k, r.right() - rad + k, r.bottom(), r.right() - rad,
            r.bottom());
    lineTo(r.x + rad, r.bottom());
    cubicTo(r.x + rad - k, r.bottom(), r.x, r.bottom() - rad + k, r.x, r.bottom() - rad);
    lineTo(r.x, r.y + rad);
    cubicTo(r.x, r.y + rad - k, r.x + rad - k, r.y, r.x + rad, r.y);
    close();
  }

  /**
   * An arc, angles in radians, zero at twelve o'clock and increasing
   * clockwise.
   *
   * Twelve o'clock and clockwise because that is how every rotary control in
   * audio is described -- "from seven o'clock to five o'clock" -- and
   * converting from mathematical convention at every call site is where the
   * sign errors live.
   */
  void addArc(const Rect& bounds, float fromRadians, float toRadians, bool startNewContour = true) {
    if (bounds.isEmpty()) return;
    const float rx = bounds.w * 0.5f, ry = bounds.h * 0.5f;
    const Point c = bounds.centre();
    auto at = [&](float angle) {
      return Point{c.x + rx * std::sin(angle), c.y - ry * std::cos(angle)};
    };
    const float sweep = toRadians - fromRadians;
    // One cubic per quarter turn or less: past that a single cubic's error
    // grows fast enough to see.
    const int steps = (int) std::ceil(std::fabs(sweep) / (3.14159265f * 0.5f));
    const int n = steps < 1 ? 1 : steps;
    const float step = sweep / (float) n;

    if (startNewContour) moveTo(at(fromRadians).x, at(fromRadians).y);
    else lineTo(at(fromRadians).x, at(fromRadians).y);

    for (int i = 0; i < n; ++i) {
      const float a0 = fromRadians + step * (float) i;
      const float a1 = a0 + step;
      // The control-point distance for an arbitrary sweep, which reduces to
      // kArc at a quarter turn.
      const float k = 4.0f / 3.0f * std::tan((a1 - a0) * 0.25f);
      const Point p0 = at(a0), p1 = at(a1);
      const Point t0{rx * std::cos(a0), ry * std::sin(a0)};
      const Point t1{rx * std::cos(a1), ry * std::sin(a1)};
      cubicTo(p0.x + k * t0.x, p0.y + k * t0.y, p1.x - k * t1.x, p1.y - k * t1.y, p1.x, p1.y);
    }
  }

  /** The corners, without flattening curves -- so a curve that bulges outside
   *  its control points reports a slightly generous box. Used for damage
   *  regions and early rejection, where generous is safe and expensive is
   *  not. */
  Rect controlPointBounds() const {
    if (points_.empty()) return {};
    float l = points_[0].x, t = points_[0].y, r = l, b = t;
    for (const Point& p : points_) {
      if (p.x < l) l = p.x;
      if (p.x > r) r = p.x;
      if (p.y < t) t = p.y;
      if (p.y > b) b = p.y;
    }
    return {l, t, r - l, b - t};
  }

  /**
   * Flatten to closed polygons in DEVICE space.
   *
   * Every contour comes back closed, whether or not close() was called: an
   * unclosed contour has no inside, and a caller filling one means the shape
   * it drew rather than nothing at all.
   */
  void flatten(const Transform& transform, std::vector<Contour>& out,
               float tolerance = 0.2f) const {
    out.clear();
    if (verbs_.empty()) return;

    // Curve subdivision is measured in device pixels, so the transform's
    // scale decides how many segments a curve needs. A path drawn at 4x with
    // the tolerance it had at 1x shows its polygons.
    const float scale = transform.scaleFactor();
    const float tol = tolerance / (scale > 1e-6f ? scale : 1e-6f);

    std::vector<Point> contour;
    bool closed = false;
    size_t pi = 0;
    Point cursor{};

    auto flush = [&]() {
      // Two points is a line: no area to fill, but a real thing to STROKE.
      if (contour.size() >= 2) out.push_back(Contour{contour, closed});
      contour.clear();
      closed = false;
    };

    for (Verb v : verbs_) {
      switch (v) {
        case Verb::Move:
          flush();
          cursor = points_[pi++];
          contour.push_back(transform.apply(cursor));
          break;
        case Verb::Line:
          cursor = points_[pi++];
          contour.push_back(transform.apply(cursor));
          break;
        case Verb::Quad: {
          const Point c = points_[pi++], e = points_[pi++];
          subdivideQuad(cursor, c, e, tol, transform, contour, 0);
          cursor = e;
          break;
        }
        case Verb::Cubic: {
          const Point c1 = points_[pi++], c2 = points_[pi++], e = points_[pi++];
          subdivideCubic(cursor, c1, c2, e, tol, transform, contour, 0);
          cursor = e;
          break;
        }
        case Verb::Close:
          closed = true;
          flush();
          break;
      }
    }
    flush();
  }

private:
  enum class Verb : uint8_t { Move, Line, Quad, Cubic, Close };

  /** 4/3 * (sqrt(2) - 1): the cubic control distance for a quarter circle. */
  static constexpr float kArc = 0.5522847498f;

  /** Depth limit, so a degenerate curve -- NaN control points, or a cusp --
   *  cannot subdivide for ever and hang the UI thread. */
  static constexpr int kMaxDepth = 16;

  static void subdivideQuad(Point p0, Point p1, Point p2, float tol, const Transform& t,
                            std::vector<Point>& out, int depth) {
    // Flatness by how far the control point sits off the chord. Cheap, and
    // conservative in the direction that costs pixels rather than looks.
    const Point mid{(p0.x + p2.x) * 0.5f, (p0.y + p2.y) * 0.5f};
    const float dx = p1.x - mid.x, dy = p1.y - mid.y;
    if (depth >= kMaxDepth || (dx * dx + dy * dy) <= tol * tol) {
      out.push_back(t.apply(p2));
      return;
    }
    const Point a{(p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f};
    const Point b{(p1.x + p2.x) * 0.5f, (p1.y + p2.y) * 0.5f};
    const Point m{(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f};
    subdivideQuad(p0, a, m, tol, t, out, depth + 1);
    subdivideQuad(m, b, p2, tol, t, out, depth + 1);
  }

  static void subdivideCubic(Point p0, Point p1, Point p2, Point p3, float tol, const Transform& t,
                             std::vector<Point>& out, int depth) {
    const float d1x = p1.x - (p0.x + p3.x) * 0.5f, d1y = p1.y - (p0.y + p3.y) * 0.5f;
    const float d2x = p2.x - (p0.x + p3.x) * 0.5f, d2y = p2.y - (p0.y + p3.y) * 0.5f;
    const float worst = (d1x * d1x + d1y * d1y) > (d2x * d2x + d2y * d2y)
                            ? (d1x * d1x + d1y * d1y)
                            : (d2x * d2x + d2y * d2y);
    if (depth >= kMaxDepth || worst <= tol * tol) {
      out.push_back(t.apply(p3));
      return;
    }
    const Point a{(p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f};
    const Point b{(p1.x + p2.x) * 0.5f, (p1.y + p2.y) * 0.5f};
    const Point c{(p2.x + p3.x) * 0.5f, (p2.y + p3.y) * 0.5f};
    const Point ab{(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f};
    const Point bc{(b.x + c.x) * 0.5f, (b.y + c.y) * 0.5f};
    const Point m{(ab.x + bc.x) * 0.5f, (ab.y + bc.y) * 0.5f};
    subdivideCubic(p0, a, ab, m, tol, t, out, depth + 1);
    subdivideCubic(m, bc, c, p3, tol, t, out, depth + 1);
  }

  std::vector<Verb> verbs_;
  std::vector<Point> points_;
  Point start_{}, current_{};
  bool hasCurrent_ = false;
};

} // namespace gfx
} // namespace sonore
