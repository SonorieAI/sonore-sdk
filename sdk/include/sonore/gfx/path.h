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
enum class FillRule { NonZero, /**< inside where the winding is not zero */ EvenOdd /**< inside where crossed an odd number of times */ };

/**
 * A flattened contour, and whether the author closed it.
 *
 * The FILL path does not care -- an open contour has an inside and gets one.
 * The STROKE path cares completely: an open contour gets end caps and a
 * closed one gets a join where it meets itself. Losing that distinction
 * during flattening is why this carries a flag rather than a bare point list.
 */
struct Contour {
  /** The contour flattened to straight segments, in order. */
  std::vector<Point> points;
  /** Whether close() ended it, so a stroke joins its ends rather than capping
   *  them. */
  bool closed = false;
};

/**
 * A shape as contours of lines and curves: what Graphics fills, strokes and
 * clips to, and what glyphs and SVG shapes become. The reference's Path, in
 * this SDK's terms; the file comment explains why it is the one primitive and
 * why flattening waits for the transform.
 *
 * Built with moveTo / lineTo / quadTo / cubicTo / close, or the add* shapes.
 * Angles are in radians from twelve o'clock, clockwise (see addArc). Drawing
 * with no moveTo first starts a contour where asked rather than joining the
 * previous one. A plain value: copy it, keep it across paints, build it on
 * any thread -- it holds no reference to a Graphics or a Bitmap.
 */
class Path {
public:
  /** Remove every contour; the path is as if new. */
  void clear() {
    verbs_.clear();
    points_.clear();
    start_ = {};
    current_ = {};
    hasCurrent_ = false;
    afterClose_ = false;
  }

  /** Whether nothing has been added since construction or clear(). */
  bool isEmpty() const { return verbs_.empty(); }

  /** Everything in `other`, after what is here: its contours as they are. */
  void append(const Path& other) {
    if (other.verbs_.empty()) return;
    verbs_.insert(verbs_.end(), other.verbs_.begin(), other.verbs_.end());
    points_.insert(points_.end(), other.points_.begin(), other.points_.end());
    start_ = other.start_;
    current_ = other.current_;
    hasCurrent_ = other.hasCurrent_;
    afterClose_ = other.afterClose_;
  }

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

  /** Start a new contour at (x, y). */
  void moveTo(float x, float y) {
    verbs_.push_back(Verb::Move);
    points_.push_back({x, y});
    start_ = {x, y};
    current_ = {x, y};
    hasCurrent_ = true;
    afterClose_ = false;
  }

  /** A straight segment to (x, y). With no contour started, starts one there
   *  instead; after close(), continues from the closed contour's start. */
  void lineTo(float x, float y) {
    // A lineTo with no preceding moveTo would otherwise start a contour from
    // wherever the last one ended, joining two shapes that were meant to be
    // separate. Starting at the point asked for is the only harmless answer.
    if (!hasCurrent_) return moveTo(x, y);
    beginAfterClose();
    verbs_.push_back(Verb::Line);
    points_.push_back({x, y});
    current_ = {x, y};
  }

  /** A quadratic curve to (x, y), pulled towards the control point (cx, cy).
   *  With no contour started, one starts at the control point. */
  void quadTo(float cx, float cy, float x, float y) {
    if (!hasCurrent_) moveTo(cx, cy);
    beginAfterClose();
    verbs_.push_back(Verb::Quad);
    points_.push_back({cx, cy});
    points_.push_back({x, y});
    current_ = {x, y};
  }

  /** A cubic curve to (x, y) with control points (c1x, c1y) and (c2x, c2y).
   *  With no contour started, one starts at the first control point. */
  void cubicTo(float c1x, float c1y, float c2x, float c2y, float x, float y) {
    if (!hasCurrent_) moveTo(c1x, c1y);
    beginAfterClose();
    verbs_.push_back(Verb::Cubic);
    points_.push_back({c1x, c1y});
    points_.push_back({c2x, c2y});
    points_.push_back({x, y});
    current_ = {x, y};
  }

  /** Close the contour with a segment back to its start; the next segment
   *  begins a new contour there. Ignored with no contour started. */
  void close() {
    if (!hasCurrent_) return;
    verbs_.push_back(Verb::Close);
    current_ = start_;
    afterClose_ = true;
  }

  // ── Shapes, all built from the above ──────────────────────────────────────

  /** A closed rectangle, clockwise from the top-left; nothing for an empty
   *  one. */
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

  /** A closed rectangle with corners of `radius`, held to 0..half the shorter
   *  side; 0 gives addRect. Nothing for an empty rectangle. */
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
    float sweep = toRadians - fromRadians;
    // An angle that is not a number has no arc, and a sweep of a billion
    // turns is a cubic per quarter of each: the step count below was cast
    // from whatever the sweep was, which for inf or NaN is undefined, and for
    // a huge one asked for that many curves. 64 whole turns is the most
    // drawn; past that more of the same circle adds nothing. The turns are
    // dropped WHOLE, so the arc still ends where the sweep put it: cut flat
    // at 64 turns it ended back at its start, and a path continued from
    // there -- a lineTo, a joined arc, a close -- went from the wrong place.
    if (!(std::fabs(fromRadians) <= 3.0e38f) || !(std::fabs(sweep) <= 3.0e38f)) return;
    const float kTurn = 6.28318531f, kMostSweep = 64.0f * kTurn;
    if (std::fabs(sweep) > kMostSweep)
      sweep = std::copysign(kMostSweep - kTurn + std::fmod(std::fabs(sweep), kTurn), sweep);
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
    //
    // The scale in the direction it stretches MOST, not the square root of
    // the determinant: under an uneven scale or a skew that is an average
    // which undercounts the long axis by the square root of the anisotropy.
    // A circle under a 1:16 scale strayed 0.48 px from the curve and one under
    // skewX(83) 0.53 px, against the 0.2 asked for -- the facets show on an
    // SVG stretched with preserveAspectRatio="none" or skewed. A turn, an
    // even scale and a mirror stretch every direction alike, and keep the
    // number they always had, so their curves flatten exactly as before.
    const float even = transform.scaleFactor();
    const float most = largestStretch(transform);
    const float scale = most > even * 1.0001f ? most : even;
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

  /** Drawing on after close() opens a new contour at the closed one's start,
   *  as SVG's Z and PostScript's closepath define it. Without the explicit
   *  move the flattener began the next contour at its first END point, a
   *  corner short, and a curve from the point before the close. */
  void beginAfterClose() {
    if (afterClose_) moveTo(start_.x, start_.y);
  }

  /** How much the transform stretches a length in the direction it stretches
   *  most: the larger singular value of its 2x2 part. In double, so a scale
   *  near the top of a float's range squares without overflowing; a NaN
   *  comes back as NaN, and flatten() then keeps the even scale. */
  static float largestStretch(const Transform& t) {
    const double a = t.a, b = t.b, c = t.c, d = t.d;
    const double sum = a * a + b * b + c * c + d * d;
    const double det = a * d - b * c;
    const double gap = sum * sum - 4.0 * det * det;
    return (float) std::sqrt(0.5 * (sum + std::sqrt(gap > 0.0 ? gap : 0.0)));
  }

  /** 4/3 * (sqrt(2) - 1): the cubic control distance for a quarter circle. */
  static constexpr float kArc = 0.5522847498f;

  /** Depth limit, so a degenerate curve -- NaN control points, or a cusp --
   *  cannot subdivide for ever and hang the UI thread. */
  static constexpr int kMaxDepth = 16;

  /**
   * The finest flatness a float can SHOW at these coordinates, in the units
   * the test runs in: m * 2^-22 for a largest |coordinate| m, which is two
   * units in the last place at the bottom of a binade and four at the top.
   * (2^-20 took over from the tolerance about 2e5 device pixels out, which a
   * scrolled canvas reaches: a radius-8 circle at x = 1e6 under scale 2 came
   * out an octagon, 1.2 px off. At 2^-22 it is 0.3 px off, everything nearer
   * -- 6e5 at scale 2, 3e4 at scale 16 -- is exactly what it was before
   * there was a floor, and every case the floor exists for still stops: 5
   * points for the circle that took 262,145.)
   *
   * The flatness test subtracts points, and what it computes carries a few
   * units of rounding in the last place of the largest coordinate however
   * small the curve has become. Under that floor it measures rounding, not
   * shape, and a curve whose tolerance sits below it never passes: a 3 px
   * circle at y = 23,173,946, where a float steps by 2, split to the full
   * depth -- 262,145 points -- and a fuzzed SVG stroked a curve flung to 3e11
   * with round joins, one such circle at each of its points: billions of
   * points for a 96-pixel icon. So the tolerance is never finer than the
   * floor. Where the coordinates are small enough to be drawn -- any shape
   * near its own origin at any scale -- the floor is far under the tolerance
   * and changes nothing; it decides only where splitting further could find
   * nothing but noise. (Cutting every curve past 4e6 device pixels to its
   * chord, which this replaces, also flattened the on-screen arc of a shape
   * whose far end was that far away: a zoomed envelope, a huge rounded box.)
   *
   * Returns infinity when a coordinate is not finite: no subdivision makes
   * sense of NaN, so such a curve is its chord at once.
   */
  static float noiseFloor(const Point* p, int n) {
    float m = 0.0f;
    for (int i = 0; i < n; ++i) {
      const float ax = std::fabs(p[i].x), ay = std::fabs(p[i].y);
      if (!(ax <= 3.4e38f) || !(ay <= 3.4e38f)) return HUGE_VALF;
      if (ax > m) m = ax;
      if (ay > m) m = ay;
    }
    return m * (1.0f / 4194304.0f);
  }

  static void subdivideQuad(Point p0, Point p1, Point p2, float tol, const Transform& t,
                            std::vector<Point>& out, int depth) {
    // Flatness by how far the control point sits off the chord. Cheap, and
    // conservative in the direction that costs pixels rather than looks.
    const Point mid{(p0.x + p2.x) * 0.5f, (p0.y + p2.y) * 0.5f};
    const float dx = p1.x - mid.x, dy = p1.y - mid.y;
    const Point hull[3] = {p0, p1, p2};
    const float noise = noiseFloor(hull, 3);
    const float reach = tol > noise ? tol : noise;
    if (depth >= kMaxDepth || noise == HUGE_VALF || (dx * dx + dy * dy) <= reach * reach) {
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
    // Flatness by how far each control point sits from where a straight
    // line would put it, a third and two thirds along the chord. The curve
    // minus the chord is 3t(1-t)^2 e1 + 3t^2(1-t) e2 for those offsets e1,
    // e2, so it never strays more than 3/4 of the larger one. Measured from
    // the chord's MIDPOINT instead, a curve with its controls near the thirds
    // -- every circle, every straight cubic -- never passed, and split down to
    // 1.2 px pieces.
    const float d1x = p1.x - (2.0f * p0.x + p3.x) / 3.0f, d1y = p1.y - (2.0f * p0.y + p3.y) / 3.0f;
    const float d2x = p2.x - (p0.x + 2.0f * p3.x) / 3.0f, d2y = p2.y - (p0.y + 2.0f * p3.y) / 3.0f;
    const float worst = (d1x * d1x + d1y * d1y) > (d2x * d2x + d2y * d2y)
                            ? (d1x * d1x + d1y * d1y)
                            : (d2x * d2x + d2y * d2y);
    const Point hull[4] = {p0, p1, p2, p3};
    const float noise = noiseFloor(hull, 4);
    const float fine = tol * (4.0f / 3.0f);
    const float reach = fine > noise ? fine : noise;
    if (depth >= kMaxDepth || noise == HUGE_VALF || worst <= reach * reach) {
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
  bool afterClose_ = false;
};

} // namespace gfx
} // namespace sonore
