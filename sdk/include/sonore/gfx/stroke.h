// SPDX-License-Identifier: Apache-2.0
//
// Stroking: turning a path into the outline of a pen drawn along it.
//
// ── The approach, and why it is not the textbook one ────────────────────────
//
// The classic method builds ONE continuous outline: walk up the left side of
// the path, round the end, walk back down the right. It produces a minimal
// polygon, and it is where every stroking bug in every graphics library
// lives -- the outline self-intersects at tight corners, the inner side of a
// sharp bend folds through itself, and the fill rule then eats a notch out of
// the line.
//
// This instead emits a QUAD PER SEGMENT plus a shape per joint, all into one
// path, and fills it with the non-zero rule. Overlaps are simply inside. A
// tight corner is two quads that overlap, which fills once and looks right;
// the pathological case of the textbook method does not exist.
//
// The cost is more edges for the rasteriser. That is the correct trade for a
// UI: a plugin's interface is a few hundred short strokes per repaint, and
// correctness at corners is visible while the extra edges are not.
//
// ── Joins and caps ──────────────────────────────────────────────────────────
//
// Only the OUTER side of a bend needs a join: the inner side is already
// covered by the two segment quads overlapping. Getting that backwards draws
// a wedge sticking into the middle of the line.
#pragma once

#include <cmath>
#include <limits>
#include <utility>
#include <vector>

#include "geometry.h"
#include "path.h"

namespace sonore {
namespace gfx {

/** How a stroke turns a corner. */
enum class LineJoin { Miter, /**< a sharp point, up to the miter limit */ Round, /**< rounded */ Bevel /**< cut off flat */ };
/** How a stroke ends an open contour. */
enum class LineCap { Butt, /**< flat, at the end point */ Round, /**< a half circle past it */ Square /**< flat, half the width past it */ };

/**
 * The pen a path is stroked with: width, how corners join, how open ends are
 * capped, and an optional dash pattern. The reference's PathStrokeType, by
 * another name.
 *
 * Graphics::strokePath applies it in DEVICE space, after the transform, so
 * `width` and the dash lengths are device pixels and do not grow when the
 * drawing is scaled -- see the note on strokePath for why. A plain value; a
 * width that is zero, negative or not a number draws nothing.
 */
struct StrokeStyle {
  float width = 1.0f;  ///< in device pixels, as strokePath applies it
  LineJoin join = LineJoin::Miter;  ///< how corners are drawn
  LineCap cap = LineCap::Butt;  ///< how open ends are drawn
  /**
   * How far a miter may extend, as a multiple of the stroke width, before it
   * degrades to a bevel.
   *
   * Without a limit, two nearly-parallel segments produce a spike that runs
   * off to infinity -- the tan of a half-angle approaching zero. Four is the
   * usual default and corresponds to a corner of about 29 degrees.
   */
  float miterLimit = 4.0f;
  /**
   * Dashes: on, off, on, off... -- lengths in the units of `width`, so in
   * device pixels for Graphics::strokePath. Empty is a solid line. An odd
   * count is repeated to make it even, as SVG and CSS both say, and a
   * negative or all-zero pattern draws solid.
   */
  // Braced, so StrokeStyle{width} -- the common way to make one -- is not a
  // missing-field-initializers warning in every TU that draws, an error where
  // warnings are errors.
  std::vector<float> dashes{};
  /** How far into the pattern each contour starts. */
  float dashOffset = 0.0f;
};

/**
 * Builds the fillable outline of a stroked path.
 *
 * Works on the FLATTENED contours, so curves are already segments and a
 * stroked curve is a chain of stroked lines. The flattening tolerance
 * therefore controls stroke quality too, which is why it follows the
 * transform.
 */
class Stroker {
public:
  /** The most pieces dash() cuts one set of contours into; a finer pattern
   *  is drawn solid. */
  static constexpr double kMaxDashes = 200000.0;

  /** The contours, cut into dashes: each dash an open contour of its own
   *  -- which the stroke then caps at both ends, as a dash is drawn.
   *
   *  The pattern starts again at the start of each contour, and a closed
   *  contour's closing edge is dashed with the rest. Returns false for a
   *  pattern that draws a solid line: empty, all zero, a negative or
   *  non-finite length -- and one so fine against the path that it would
   *  cut more than kMaxDashes pieces: a pattern finer than a pixel shows
   *  as a grey line, and a dash every 1e-9 of a pixel along a long path is
   *  a request for memory and not for a picture. */
  static bool dash(const std::vector<Contour>& in, const std::vector<float>& dashes, float offset,
                   std::vector<Contour>& out) {
    out.clear();
    if (dashes.empty()) return false;
    std::vector<double> pattern(dashes.begin(), dashes.end());
    if (pattern.size() % 2 != 0) pattern.insert(pattern.end(), dashes.begin(), dashes.end());
    double cycle = 0.0;
    for (const double v : pattern) {
      if (!(v >= 0.0) || !std::isfinite(v)) return false;
      cycle += v;
    }
    if (!(cycle > 0.0) || !std::isfinite(cycle)) return false;

    double length = 0.0;
    for (const Contour& c : in) {
      const size_t n = c.points.size();
      for (size_t i = 0; i + 1 < n; ++i) length += distance(c.points[i], c.points[i + 1]);
      if (c.closed && n > 1) length += distance(c.points[n - 1], c.points[0]);
    }
    if (!std::isfinite(length)) return false;
    if (length / cycle * (double) pattern.size() > kMaxDashes) return false;

    double phase = std::fmod((double) offset, cycle);
    if (!std::isfinite(phase)) phase = 0.0;
    if (phase < 0.0) phase += cycle;

    const size_t count = pattern.size();
    for (const Contour& c : in) {
      const size_t n = c.points.size();
      if (n < 2) continue;
      // Into the pattern by the offset. The pattern's total is positive, so
      // this ends however many of its entries are zero.
      size_t index = 0;
      double remaining = pattern[0];
      double skip = phase;
      while (skip > 0.0) {
        if (skip >= remaining) {
          skip -= remaining;
          index = (index + 1) % count;
          remaining = pattern[index];
        } else {
          remaining -= skip;
          skip = 0.0;
        }
      }
      bool on = index % 2 == 0;
      const bool startsOn = on;
      const size_t firstDash = out.size();
      bool broken = false; // the pattern turned off somewhere along the contour
      Contour current;
      if (on) current.points.push_back(c.points[0]);

      const size_t segments = c.closed ? n : n - 1;
      for (size_t i = 0; i < segments; ++i) {
        const Point a = c.points[i];
        const Point b = c.points[(i + 1) % n];
        const double span = distance(a, b);
        double at = 0.0;
        // Every boundary of the pattern inside this segment. A zero-length
        // dash is a start and an end at one point: the stroke draws it as a
        // dot with a round cap, which is how a dotted line is made.
        while (span - at > remaining) {
          at += remaining;
          const float t = (float) (at / span);
          const Point q{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
          current.points.push_back(q);
          if (on) {
            out.push_back(std::move(current));
            current = Contour{};
            broken = true;
          }
          index = (index + 1) % count;
          remaining = pattern[index];
          on = !on;
        }
        remaining -= span - at;
        if (on) current.points.push_back(b);
      }
      if (on && !current.points.empty()) {
        // A closed contour has no ends, so a dash still on at its close runs
        // on into the one that began it, round the corner it started from --
        // as browsers and resvg draw it. Left as two dashes, each was capped
        // there and the corner between them never joined: a dashed rectangle
        // had a square bitten out of its first corner. (A dash that never
        // ends stays open where the contour starts, as theirs does.)
        if (c.closed && startsOn && broken && firstDash < out.size()) {
          std::vector<Point>& first = out[firstDash].points;
          current.points.insert(current.points.end(), first.begin() + 1, first.end());
          first.swap(current.points);
        } else {
          out.push_back(std::move(current));
        }
      }
    }
    return true;
  }

  /** Stroke the flattened `contours` with `style` into `out`, as filled
   *  outlines. `out` is appended to, so several strokes can share one fill.
   *
   *  `reach` is how far from the path the farthest pixel that will be drawn
   *  can be. A round join or cap wider than that covers every one of them,
   *  as the square around it does -- and the square is four edges, where the
   *  circle, flattened at that radius, is thousands. */
  static void stroke(const std::vector<Contour>& contours, const StrokeStyle& style, Path& out,
                     float reach = std::numeric_limits<float>::infinity()) {
    const float half = style.width * 0.5f;
    if (!(half > 0.0f)) return;

    for (const Contour& contour : contours) {
      // Consecutive duplicates have no direction, and a zero-length segment
      // would produce a normal of NaN that silently poisons the whole
      // outline. Removed here rather than guarded at every use.
      std::vector<Point> pts;
      for (const Point& p : contour.points)
        if (pts.empty() || p.distanceTo(pts.back()) > 1e-6f) pts.push_back(p);
      if (contour.closed && pts.size() > 1 && pts.front().distanceTo(pts.back()) <= 1e-6f)
        pts.pop_back();

      if (pts.size() < 2) {
        // A single point. Meaningless with a butt cap, and a dot with a round
        // one -- which is what a caller drawing a zero-length stroke means.
        if (pts.size() == 1 && style.cap == LineCap::Round) addDot(out, pts[0], half, reach);
        continue;
      }

      const size_t n = pts.size();
      const size_t segments = contour.closed ? n : n - 1;

      // Every piece below winds the same way as Path::addEllipse -- clockwise
      // on screen, positive area -- because the pieces are unioned under the
      // non-zero rule, and two that overlap with opposite windings cancel:
      // a quad wound against the cap and join circles left holes wherever
      // they overlapped it.
      for (size_t i = 0; i < segments; ++i) {
        const Point a = pts[i];
        const Point b = pts[(i + 1) % n];
        const Point nrm = normal(a, b, half);
        out.moveTo(a.x - nrm.x, a.y - nrm.y);
        out.lineTo(b.x - nrm.x, b.y - nrm.y);
        out.lineTo(b.x + nrm.x, b.y + nrm.y);
        out.lineTo(a.x + nrm.x, a.y + nrm.y);
        out.close();
      }

      // Joins at every interior vertex, and at the seam too when closed.
      const size_t joints = contour.closed ? n : n - 2;
      for (size_t j = 0; j < joints; ++j) {
        const size_t iPrev = contour.closed ? (j + n - 1) % n : j;
        const size_t iAt = contour.closed ? j : j + 1;
        const size_t iNext = contour.closed ? (j + 1) % n : j + 2;
        addJoin(out, pts[iPrev], pts[iAt], pts[iNext], half, style, reach);
      }

      if (!contour.closed) addCaps(out, pts, half, style.cap, reach);
    }
  }

private:
  static double distance(Point a, Point b) {
    const double dx = (double) b.x - (double) a.x, dy = (double) b.y - (double) a.y;
    return std::sqrt(dx * dx + dy * dy);
  }

  /** The left-hand normal of a->b, scaled. */
  static Point normal(Point a, Point b, float len) {
    const float dx = b.x - a.x, dy = b.y - a.y;
    const float d = std::sqrt(dx * dx + dy * dy);
    if (d <= 1e-9f) return {0.0f, 0.0f};
    return {-dy / d * len, dx / d * len};
  }

  /** A disc of radius `half`, or past `reach` the square around it: both
   *  cover every pixel that will be drawn, and they wind the same way. */
  static void addDot(Path& out, Point at, float half, float reach) {
    const Rect around(at.x - half, at.y - half, half * 2.0f, half * 2.0f);
    if (half > reach) out.addRect(around);
    else out.addEllipse(around);
  }

  static void addJoin(Path& out, Point prev, Point at, Point next, float half,
                      const StrokeStyle& style, float reach) {
    if (style.join == LineJoin::Round) {
      addDot(out, at, half, reach);
      return;
    }

    const Point n0 = normal(prev, at, half);
    const Point n1 = normal(at, next, half);

    // Which side bends OUTWARD. The inner side is already covered by the two
    // segment quads overlapping; adding a wedge there puts a spur inside the
    // line.
    const float cross = (at.x - prev.x) * (next.y - at.y) - (at.y - prev.y) * (next.x - at.x);
    if (std::fabs(cross) < 1e-9f) return; // straight through: no join needed
    const float side = cross > 0.0f ? -1.0f : 1.0f;

    // Named so the wedge from `at` through p0 to p1 winds clockwise like
    // everything else; which offset comes first depends on the bend.
    Point p0{at.x + n0.x * side, at.y + n0.y * side};
    Point p1{at.x + n1.x * side, at.y + n1.y * side};
    if ((p0.x - at.x) * (p1.y - at.y) - (p0.y - at.y) * (p1.x - at.x) < 0.0f) std::swap(p0, p1);

    if (style.join == LineJoin::Miter) {
      // Where the two offset edges would meet. cos of the half-angle between
      // the segments comes from the dot of the unit normals.
      const float dot = (n0.x * n1.x + n0.y * n1.y) / (half * half);
      const float clamped = dot < -1.0f ? -1.0f : (dot > 1.0f ? 1.0f : dot);
      const float halfAngleCos = std::sqrt((1.0f + clamped) * 0.5f);
      if (halfAngleCos > 1e-4f) {
        const float miterLength = 1.0f / halfAngleCos;
        if (miterLength <= style.miterLimit) {
          // Bisector direction, out to the miter point.
          float bx = p0.x - at.x + p1.x - at.x;
          float by = p0.y - at.y + p1.y - at.y;
          const float bl = std::sqrt(bx * bx + by * by);
          if (bl > 1e-9f) {
            bx /= bl;
            by /= bl;
            const float tip = half * miterLength;
            out.moveTo(at.x, at.y);
            out.lineTo(p0.x, p0.y);
            out.lineTo(at.x + bx * tip, at.y + by * tip);
            out.lineTo(p1.x, p1.y);
            out.close();
            return;
          }
        }
      }
      // Past the limit, or too degenerate to compute: fall through to a
      // bevel, which is what the limit is FOR.
    }

    out.moveTo(at.x, at.y);
    out.lineTo(p0.x, p0.y);
    out.lineTo(p1.x, p1.y);
    out.close();
  }

  static void addCaps(Path& out, const std::vector<Point>& pts, float half, LineCap cap,
                      float reach) {
    if (cap == LineCap::Butt) return;
    const size_t n = pts.size();
    addCap(out, pts[1], pts[0], half, cap, reach);
    addCap(out, pts[n - 2], pts[n - 1], half, cap, reach);
  }

  /** A cap at `end`, on a line arriving from `from`. */
  static void addCap(Path& out, Point from, Point end, float half, LineCap cap, float reach) {
    if (cap == LineCap::Round) {
      // A whole circle. Its inner half sits inside the segment quad, so under
      // the non-zero rule it costs nothing and saves computing a semicircle's
      // orientation -- which is one more place to get a sign wrong.
      addDot(out, end, half, reach);
      return;
    }
    const float dx = end.x - from.x, dy = end.y - from.y;
    const float d = std::sqrt(dx * dx + dy * dy);
    if (d <= 1e-9f) return;
    const float ux = dx / d, uy = dy / d;
    const Point nrm{-uy * half, ux * half};
    const Point tip{end.x + ux * half, end.y + uy * half};
    out.moveTo(end.x - nrm.x, end.y - nrm.y);
    out.lineTo(tip.x - nrm.x, tip.y - nrm.y);
    out.lineTo(tip.x + nrm.x, tip.y + nrm.y);
    out.lineTo(end.x + nrm.x, end.y + nrm.y);
    out.close();
  }
};

} // namespace gfx
} // namespace sonore
