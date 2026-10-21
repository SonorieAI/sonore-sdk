// SPDX-License-Identifier: Apache-2.0
//
// Colour that varies across a shape.
//
// Every panel, every knob body and every meter in a plugin is a gradient. Flat
// fills are what makes a generated interface look generated, and a gradient is
// the cheapest thing that stops it.
//
// ── Where the interpolation happens ─────────────────────────────────────────
//
// In STRAIGHT alpha, always. Premultiplied is right for compositing and wrong
// for interpolation: a fade from opaque red to transparent, premultiplied,
// passes through dark red and then black, because the transparent end's colour
// channels have already been multiplied to zero. The same fade in straight
// alpha stays red the whole way and only loses opacity, which is what anyone
// asking for a fade meant. Premultiplication happens per pixel, after.
//
// ── Where the coordinates are ───────────────────────────────────────────────
//
// A gradient is authored in USER space, next to the rectangle it fills. The
// rasteriser works in DEVICE space. transformed() moves one to the other once,
// per fill, rather than inverting a transform per pixel.
#pragma once

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

#include "colour.h"
#include "geometry.h"

namespace sonore {
namespace gfx {

/** A colour at a proportion along the gradient. 0 is point1, 1 is point2. */
struct ColourStop {
  float position = 0.0f;  ///< 0..1 along the gradient
  Colour colour;  ///< the colour there, straight alpha
};

/** What a gradient does past its ends. */
enum class GradientSpread {
  Pad,     ///< the end colours carry on: the usual, and the default
  Reflect, ///< back and forth, 0..1..0..1
  Repeat   ///< 0..1, 0..1
};

/**
 * A linear or radial ramp between two points, with any number of stops.
 *
 * A colour gradient in the shape that matters: two endpoints, a flag for
 * radial, and stops in between. Named constructors for the two cases that are
 * ninety per cent of use, because `ColourGradient(a, {0,0}, b, {0,h}, false)`
 * is four chances to write the wrong thing.
 */
class ColourGradient {
public:
  /** Empty: no stops, so a fill with it draws nothing. */
  ColourGradient() = default;

  /** `from` at `p1` and `to` at `p2`, in the user space of the fill it is used
   *  for. Radial: centred on `p1`, reaching `to` at the distance of `p2`. */
  ColourGradient(Colour from, Point p1, Colour to, Point p2, bool radial = false)
      : point1_(p1), point2_(p2), radial_(radial) {
    stops_.push_back({0.0f, from});
    stops_.push_back({1.0f, to});
  }

  /** Top to bottom, which is what a panel and a knob body both want. */
  static ColourGradient vertical(Colour top, float y1, Colour bottom, float y2) {
    return ColourGradient(top, Point(0.0f, y1), bottom, Point(0.0f, y2));
  }

  /** Left to right, from `x1` to `x2`. */
  static ColourGradient horizontal(Colour left, float x1, Colour right, float x2) {
    return ColourGradient(left, Point(x1, 0.0f), right, Point(x2, 0.0f));
  }

  /** Centre outwards. `radius` is where the second colour is reached. */
  static ColourGradient radial(Colour centre, Point at, float radius, Colour edge) {
    return ColourGradient(centre, at, edge, Point(at.x + radius, at.y), /*radial=*/true);
  }

  /**
   * A colour partway along.
   *
   * Positions outside 0..1 are clamped in rather than rejected: a caller
   * building stops from a data range can hand over 1.0000001, and refusing it
   * would lose a stop over a rounding error.
   */
  void addStop(float position, Colour c) {
    // Written so a NaN counts as 0. It failed both tests of the old spelling
    // and was stored, and a NaN among the keys breaks the ordering std::sort
    // requires -- which is undefined behaviour, not a misplaced stop.
    const float p = position > 0.0f ? (position < 1.0f ? position : 1.0f) : 0.0f;
    // After every stop already at its position: two stops at one position are
    // a hard edge, and which colour is on which side of it is the order they
    // were added in -- std::sort could swap them, and a red-to-blue edge came
    // out blue-to-red. Inserted where it belongs rather than appended and the
    // whole list sorted again: that made a gradient of n stops cost n sorts,
    // and an SVG builds its gradients a stop at a time -- 20,000 stops took
    // 1.3 s to read, and a 1,000-stop ramp filling 4,000 shapes 21 s. Stops
    // given in order, as SVG's are, now go on the end.
    const auto at = std::upper_bound(stops_.begin(), stops_.end(), p,
                                     [](float v, const ColourStop& s) { return v < s.position; });
    stops_.insert(at, ColourStop{p, c});
  }

  /** No stops at all: for a caller -- an SVG's `<stop>` list -- that supplies
   *  every one itself, including the first and the last. */
  void clearStops() { stops_.clear(); }

  /** What happens past the ends; Pad unless set. */
  void setSpread(GradientSpread s) { spread_ = s; }
  /** What happens past the ends. */
  GradientSpread spread() const { return spread_; }

  /**
   * Where a radial gradient's 0 is, when it is not the centre: a highlight
   * pushed towards one side of a knob. The ramp still ends on the circle
   * around point1 through point2; it starts here instead.
   *
   * A focal point on or outside the circle makes the gradient a cone, as a
   * canvas's two-point gradient and SVG 2 both say -- and what lies outside
   * the cone is not painted at all.
   */
  void setFocalPoint(Point f) {
    focal_ = f;
    hasFocal_ = true;
  }

  /** Whether it radiates from point1 rather than running along a line. */
  bool isRadial() const { return radial_; }
  /** Where position 0 is (a radial gradient's centre). */
  Point point1() const { return point1_; }
  /** Where position 1 is (for a radial one, a point on its outer circle). */
  Point point2() const { return point2_; }
  /** How many stops there are, ends included. */
  int numStops() const { return (int) stops_.size(); }
  /** Stop `i` in position order; `i` must be in range, as it is not checked. */
  const ColourStop& stopAt(int i) const { return stops_[(size_t) i]; }

  /** Empty gradients happen -- a default-constructed member, a stop list a
   *  caller never filled -- and drawing nothing is better than reading past
   *  the end of an empty vector. */
  bool isEmpty() const { return stops_.empty(); }

  /**
   * The same gradient in another coordinate space. Called once per fill.
   *
   * A move, a turn and an even scale keep a circle a circle and a ramp's
   * isolines square to its axis, so for those the points move and nothing
   * else changes. A skew or an uneven scale does not: a radial gradient in a
   * box twice as wide as it is tall is an ellipse, which two points cannot
   * say. Such a gradient keeps its own coordinates and the map back into
   * them, applied per pixel -- an SVG's objectBoundingBox and its
   * gradientTransform are both that.
   */
  ColourGradient transformed(const Transform& t) const {
    ColourGradient out = *this;
    if (!mapped_ && isSimilarity(t)) {
      out.point1_ = t.apply(point1_);
      out.point2_ = t.apply(point2_);
      out.focal_ = t.apply(focal_);
      return out;
    }
    out.mapped_ = true;
    out.forward_ = mapped_ ? forward_.then(t) : t;
    // A transform that flattens the plane has no way back; every point then
    // lands on the gradient's start, which is one colour -- and the shape it
    // fills has been flattened to nothing as well.
    if (!out.forward_.invert(&out.inverse_)) out.inverse_ = Transform(0, 0, 0, 0, 0, 0);
    return out;
  }

  /**
   * How far along a point is, 0 to 1.
   *
   * Linear: the projection onto the axis, which is the dot product over the
   * axis length squared -- no square root, and it is per pixel.
   *
   * Radial: distance from point1 over the radius, which does need one.
   */
  float positionOf(Point p) const {
    if (mapped_) p = inverse_.apply(p);
    const float dx = point2_.x - point1_.x;
    const float dy = point2_.y - point1_.y;
    if (radial_) {
      const float radius = std::sqrt(dx * dx + dy * dy);
      if (!(radius > 0.0f)) return 0.0f;
      if (hasFocal_ && (focal_.x != point1_.x || focal_.y != point1_.y))
        return focalPosition(p, radius);
      const float ox = p.x - point1_.x, oy = p.y - point1_.y;
      return std::sqrt(ox * ox + oy * oy) / radius;
    }
    const float lengthSquared = dx * dx + dy * dy;
    // A zero-length gradient is one colour, not a division by zero.
    if (!(lengthSquared > 0.0f)) return 0.0f;
    return ((p.x - point1_.x) * dx + (p.y - point1_.y) * dy) / lengthSquared;
  }

  /** The colour at a proportion. Past either end it is that end's colour --
   *  Clamping is the convention, and a repeating ramp is a different
   *  feature. */
  Colour lookup(float t) const {
    if (stops_.empty()) return Colour();
    // Folded into 0..1 first. A NaN or an infinity folds to NaN, which the
    // test below sends to the first stop, as it did before.
    if (spread_ == GradientSpread::Repeat) {
      t -= std::floor(t);
    } else if (spread_ == GradientSpread::Reflect) {
      t = std::fmod(std::fabs(t), 2.0f);
      if (t > 1.0f) t = 2.0f - t;
    }
    if (!(t > stops_.front().position)) return stops_.front().colour;
    if (t >= stops_.back().position) return stops_.back().colour;

    for (size_t i = 1; i < stops_.size(); ++i) {
      const ColourStop& hi = stops_[i];
      if (t > hi.position) continue;
      const ColourStop& lo = stops_[i - 1];
      const float span = hi.position - lo.position;
      // Two stops at the same position is a hard edge, not a divide by zero.
      if (!(span > 0.0f)) return hi.colour;
      const float f = (t - lo.position) / span;
      auto mix = [f](uint8_t a, uint8_t b) {
        return (uint8_t) ((float) a + ((float) b - (float) a) * f + 0.5f);
      };
      // Straight alpha, deliberately. See the note at the top of this file.
      return Colour(mix(lo.colour.r, hi.colour.r), mix(lo.colour.g, hi.colour.g),
                    mix(lo.colour.b, hi.colour.b), mix(lo.colour.a, hi.colour.a));
    }
    return stops_.back().colour;
  }

  /**
   * Which coordinate alone the colour depends on, for a ramp that runs along
   * an axis: 1 for x (a linear ramp whose points share a y -- left to right),
   * 2 for y (they share an x -- top to bottom), 0 for anything else: radial,
   * at an angle, mapped through a skew, or of no length.
   *
   * For 1 and 2 positionOf multiplies the other coordinate's offset by an
   * exact zero, so every pixel of a column, or of a row, gets the same colour
   * -- a sum's zero may change sign, and a position of 0 or -0 looks up the
   * same colour under every spread. A fill asks this to work a colour out
   * once per column or per row rather than once per pixel.
   */
  int varyingAxis() const {
    if (radial_ || mapped_) return 0;
    const float dx = point2_.x - point1_.x, dy = point2_.y - point1_.y;
    if (dy == 0.0f && dx != 0.0f) return 1;
    if (dx == 0.0f && dy != 0.0f) return 2;
    return 0;
  }

  /** The colour at a point -- transparent where the gradient does not reach,
   *  outside a focal gradient's cone. */
  Colour colourAt(Point p) const {
    const float t = positionOf(p);
    if (t != t) return Colour(0, 0, 0, 0);
    return lookup(t);
  }

private:
  /** A turn, an even scale and a move, with or without a mirror -- the
   *  transforms that keep circles circles. Tested with a tolerance, because a
   *  rotation composed with a scale is not exactly equal in its two
   *  diagonals once rounded. */
  static bool isSimilarity(const Transform& t) {
    const float size = std::fabs(t.a) + std::fabs(t.b) + std::fabs(t.c) + std::fabs(t.d);
    const float tolerance = size * 1.0e-5f;
    const bool turn = std::fabs(t.a - t.d) <= tolerance && std::fabs(t.b + t.c) <= tolerance;
    const bool mirror = std::fabs(t.a + t.d) <= tolerance && std::fabs(t.b - t.c) <= tolerance;
    return (turn || mirror) && size > 0.0f;
  }

  /**
   * Where p is along a ramp from the focal point out to the circle -- or NaN
   * where the ramp never reaches.
   *
   * The gradient is a family of circles, the one at t centred t of the way
   * from the focal point to the centre with t times the radius; p's position
   * is the largest t whose circle passes through it. With d the step from
   * the focal point to the centre and q from the focal point to p, that t
   * solves |q - t d| = t r:
   *
   *     t^2 (|d|^2 - r^2) - 2 t (q.d) + |q|^2 = 0
   *
   * Inside the circle the leading term is negative and there is exactly one
   * positive root. On the circle it vanishes and the one root is linear.
   * Outside, the circles sweep a cone, and a point beyond its sides is on
   * none of them -- not painted, as a canvas's two-point gradient leaves it.
   */
  float focalPosition(Point p, float radius) const {
    const float dx = point1_.x - focal_.x, dy = point1_.y - focal_.y;
    const float qx = p.x - focal_.x, qy = p.y - focal_.y;
    const float a = dx * dx + dy * dy - radius * radius;
    const float b = qx * dx + qy * dy;
    const float c = qx * qx + qy * qy;
    const float none = std::numeric_limits<float>::quiet_NaN();
    if (!(c > 0.0f)) return 0.0f; // the focal point itself
    // A focal point on the circle, to within rounding: one root.
    if (std::fabs(a) <= 1.0e-6f * radius * radius) return b > 0.0f ? c / (2.0f * b) : none;
    const float disc = b * b - a * c;
    if (disc < 0.0f) return none;
    const float root = std::sqrt(disc);
    const float t1 = (b + root) / a, t2 = (b - root) / a;
    const float t = t1 > t2 ? t1 : t2;
    return t >= 0.0f ? t : none;
  }

  Point point1_, point2_;
  Point focal_;
  bool radial_ = false;
  bool hasFocal_ = false;
  bool mapped_ = false;
  GradientSpread spread_ = GradientSpread::Pad;
  Transform forward_, inverse_;
  std::vector<ColourStop> stops_;
};

} // namespace gfx
} // namespace sonore
