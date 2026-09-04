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
#include <vector>

#include "colour.h"
#include "geometry.h"

namespace sonore {
namespace gfx {

/** A colour at a proportion along the gradient. 0 is point1, 1 is point2. */
struct ColourStop {
  float position = 0.0f;
  Colour colour;
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
  ColourGradient() = default;

  ColourGradient(Colour from, Point p1, Colour to, Point p2, bool radial = false)
      : point1_(p1), point2_(p2), radial_(radial) {
    stops_.push_back({0.0f, from});
    stops_.push_back({1.0f, to});
  }

  /** Top to bottom, which is what a panel and a knob body both want. */
  static ColourGradient vertical(Colour top, float y1, Colour bottom, float y2) {
    return ColourGradient(top, Point(0.0f, y1), bottom, Point(0.0f, y2));
  }

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
    const float p = position < 0.0f ? 0.0f : (position > 1.0f ? 1.0f : position);
    stops_.push_back({p, c});
    std::sort(stops_.begin(), stops_.end(),
              [](const ColourStop& a, const ColourStop& b) { return a.position < b.position; });
  }

  bool isRadial() const { return radial_; }
  Point point1() const { return point1_; }
  Point point2() const { return point2_; }
  int numStops() const { return (int) stops_.size(); }
  const ColourStop& stopAt(int i) const { return stops_[(size_t) i]; }

  /** Empty gradients happen -- a default-constructed member, a stop list a
   *  caller never filled -- and drawing nothing is better than reading past
   *  the end of an empty vector. */
  bool isEmpty() const { return stops_.empty(); }

  /** The same gradient in another coordinate space. Called once per fill. */
  ColourGradient transformed(const Transform& t) const {
    ColourGradient out = *this;
    out.point1_ = t.apply(point1_);
    out.point2_ = t.apply(point2_);
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
    const float dx = point2_.x - point1_.x;
    const float dy = point2_.y - point1_.y;
    if (radial_) {
      const float radius = std::sqrt(dx * dx + dy * dy);
      if (!(radius > 0.0f)) return 0.0f;
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

  Colour colourAt(Point p) const { return lookup(positionOf(p)); }

private:
  Point point1_, point2_;
  bool radial_ = false;
  std::vector<ColourStop> stops_;
};

} // namespace gfx
} // namespace sonore
