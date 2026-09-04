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
#include <vector>

#include "geometry.h"
#include "path.h"

namespace sonore {
namespace gfx {

enum class LineJoin { Miter, Round, Bevel };
enum class LineCap { Butt, Round, Square };

struct StrokeStyle {
  float width = 1.0f;
  LineJoin join = LineJoin::Miter;
  LineCap cap = LineCap::Butt;
  /**
   * How far a miter may extend, as a multiple of the stroke width, before it
   * degrades to a bevel.
   *
   * Without a limit, two nearly-parallel segments produce a spike that runs
   * off to infinity -- the tan of a half-angle approaching zero. Four is the
   * usual default and corresponds to a corner of about 29 degrees.
   */
  float miterLimit = 4.0f;
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
  /** `out` is appended to, so several strokes can share one fill. */
  static void stroke(const std::vector<Contour>& contours, const StrokeStyle& style, Path& out) {
    const float half = style.width * 0.5f;
    if (half <= 0.0f) return;

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
        if (pts.size() == 1 && style.cap == LineCap::Round)
          out.addEllipse(Rect(pts[0].x - half, pts[0].y - half, half * 2.0f, half * 2.0f));
        continue;
      }

      const size_t n = pts.size();
      const size_t segments = contour.closed ? n : n - 1;

      for (size_t i = 0; i < segments; ++i) {
        const Point a = pts[i];
        const Point b = pts[(i + 1) % n];
        const Point nrm = normal(a, b, half);
        out.moveTo(a.x + nrm.x, a.y + nrm.y);
        out.lineTo(b.x + nrm.x, b.y + nrm.y);
        out.lineTo(b.x - nrm.x, b.y - nrm.y);
        out.lineTo(a.x - nrm.x, a.y - nrm.y);
        out.close();
      }

      // Joins at every interior vertex, and at the seam too when closed.
      const size_t joints = contour.closed ? n : n - 2;
      for (size_t j = 0; j < joints; ++j) {
        const size_t iPrev = contour.closed ? (j + n - 1) % n : j;
        const size_t iAt = contour.closed ? j : j + 1;
        const size_t iNext = contour.closed ? (j + 1) % n : j + 2;
        addJoin(out, pts[iPrev], pts[iAt], pts[iNext], half, style);
      }

      if (!contour.closed) addCaps(out, pts, half, style.cap);
    }
  }

private:
  /** The left-hand normal of a->b, scaled. */
  static Point normal(Point a, Point b, float len) {
    const float dx = b.x - a.x, dy = b.y - a.y;
    const float d = std::sqrt(dx * dx + dy * dy);
    if (d <= 1e-9f) return {0.0f, 0.0f};
    return {-dy / d * len, dx / d * len};
  }

  static void addJoin(Path& out, Point prev, Point at, Point next, float half,
                      const StrokeStyle& style) {
    if (style.join == LineJoin::Round) {
      out.addEllipse(Rect(at.x - half, at.y - half, half * 2.0f, half * 2.0f));
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

    const Point p0{at.x + n0.x * side, at.y + n0.y * side};
    const Point p1{at.x + n1.x * side, at.y + n1.y * side};

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
            const float reach = half * miterLength;
            out.moveTo(at.x, at.y);
            out.lineTo(p0.x, p0.y);
            out.lineTo(at.x + bx * reach, at.y + by * reach);
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

  static void addCaps(Path& out, const std::vector<Point>& pts, float half, LineCap cap) {
    if (cap == LineCap::Butt) return;
    const size_t n = pts.size();
    addCap(out, pts[1], pts[0], half, cap);
    addCap(out, pts[n - 2], pts[n - 1], half, cap);
  }

  /** A cap at `end`, on a line arriving from `from`. */
  static void addCap(Path& out, Point from, Point end, float half, LineCap cap) {
    if (cap == LineCap::Round) {
      // A whole circle. Its inner half sits inside the segment quad, so under
      // the non-zero rule it costs nothing and saves computing a semicircle's
      // orientation -- which is one more place to get a sign wrong.
      out.addEllipse(Rect(end.x - half, end.y - half, half * 2.0f, half * 2.0f));
      return;
    }
    const float dx = end.x - from.x, dy = end.y - from.y;
    const float d = std::sqrt(dx * dx + dy * dy);
    if (d <= 1e-9f) return;
    const float ux = dx / d, uy = dy / d;
    const Point nrm{-uy * half, ux * half};
    const Point tip{end.x + ux * half, end.y + uy * half};
    out.moveTo(end.x + nrm.x, end.y + nrm.y);
    out.lineTo(tip.x + nrm.x, tip.y + nrm.y);
    out.lineTo(tip.x - nrm.x, tip.y - nrm.y);
    out.lineTo(end.x - nrm.x, end.y - nrm.y);
    out.close();
  }
};

} // namespace gfx
} // namespace sonore
