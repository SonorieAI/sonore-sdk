// SPDX-License-Identifier: Apache-2.0
//
// Points, rectangles, lines and affine transforms.
//
// ── Why float and not int ───────────────────────────────────────────────────
//
// A knob at 63.5 pixels is a real position, not a rounding error. Every
// coordinate here is float, and rounding happens ONCE, in the rasteriser,
// where the subpixel remainder becomes antialiasing coverage. Rounding
// earlier throws that coverage away and the result is the jagged look that
// tells a user a plugin was drawn by hand.
//
// The one place integers appear is a pixel rectangle -- a clip region or a
// damaged area -- and that type is spelled differently so the two cannot be
// confused.
#pragma once

#include <cmath>
#include <cstdint>

namespace sonore {
namespace gfx {

struct Point {
  float x = 0.0f, y = 0.0f;

  Point() = default;
  Point(float px, float py) : x(px), y(py) {}

  Point operator+(Point o) const { return {x + o.x, y + o.y}; }
  Point operator-(Point o) const { return {x - o.x, y - o.y}; }
  Point operator*(float s) const { return {x * s, y * s}; }

  float distanceTo(Point o) const {
    const float dx = x - o.x, dy = y - o.y;
    return std::sqrt(dx * dx + dy * dy);
  }
};

/**
 * A rectangle as position and SIZE, not as two corners.
 *
 * Deliberate: half the rectangle bugs anyone writes come from a right/bottom
 * that is sometimes inclusive and sometimes not. With a width there is no
 * such question -- an empty rectangle has width 0 and there is nothing to get
 * off by one.
 */
struct Rect {
  float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;

  Rect() = default;
  Rect(float rx, float ry, float rw, float rh) : x(rx), y(ry), w(rw), h(rh) {}

  float right() const { return x + w; }
  float bottom() const { return y + h; }
  Point centre() const { return {x + w * 0.5f, y + h * 0.5f}; }
  bool isEmpty() const { return w <= 0.0f || h <= 0.0f; }

  bool contains(Point p) const {
    return p.x >= x && p.x < x + w && p.y >= y && p.y < y + h;
  }

  /** Inset on every side. A negative amount grows it, which is what a caller
   *  wanting a halo around something means. */
  Rect reduced(float amount) const {
    return {x + amount, y + amount, w - amount * 2.0f, h - amount * 2.0f};
  }

  Rect reduced(float dx, float dy) const {
    return {x + dx, y + dy, w - dx * 2.0f, h - dy * 2.0f};
  }

  Rect translated(float dx, float dy) const { return {x + dx, y + dy, w, h}; }

  /** The overlap, or an empty rectangle. Never a negative size: a caller that
   *  multiplied a negative width by a colour would write outside its buffer. */
  Rect intersection(const Rect& o) const {
    const float l = x > o.x ? x : o.x;
    const float t = y > o.y ? y : o.y;
    const float r = right() < o.right() ? right() : o.right();
    const float b = bottom() < o.bottom() ? bottom() : o.bottom();
    if (r <= l || b <= t) return {};
    return {l, t, r - l, b - t};
  }

  /** The smallest rectangle containing both. An EMPTY rectangle contributes
   *  nothing rather than dragging a corner to the origin -- a union that
   *  started from a default-constructed accumulator would otherwise always
   *  reach (0,0). */
  Rect united(const Rect& o) const {
    if (isEmpty()) return o;
    if (o.isEmpty()) return *this;
    const float l = x < o.x ? x : o.x;
    const float t = y < o.y ? y : o.y;
    const float r = right() > o.right() ? right() : o.right();
    const float b = bottom() > o.bottom() ? bottom() : o.bottom();
    return {l, t, r - l, b - t};
  }

  /** The largest rectangle of the given aspect that fits, centred. What a
   *  round knob wants from a square-ish slot. */
  Rect withAspect(float aspect) const {
    if (aspect <= 0.0f || isEmpty()) return {};
    float nw = w, nh = w / aspect;
    if (nh > h) {
      nh = h;
      nw = h * aspect;
    }
    return {x + (w - nw) * 0.5f, y + (h - nh) * 0.5f, nw, nh};
  }
};

/** A rectangle in whole pixels: a clip region, a damaged area, a buffer. */
struct PixelRect {
  int x = 0, y = 0, w = 0, h = 0;

  PixelRect() = default;
  PixelRect(int px, int py, int pw, int ph) : x(px), y(py), w(pw), h(ph) {}

  int right() const { return x + w; }
  int bottom() const { return y + h; }
  bool isEmpty() const { return w <= 0 || h <= 0; }

  PixelRect intersection(const PixelRect& o) const {
    const int l = x > o.x ? x : o.x;
    const int t = y > o.y ? y : o.y;
    const int r = right() < o.right() ? right() : o.right();
    const int b = bottom() < o.bottom() ? bottom() : o.bottom();
    if (r <= l || b <= t) return {};
    return {l, t, r - l, b - t};
  }

  /** The whole pixels a float rectangle touches -- floor of the left edge,
   *  ceil of the right. Rounding both ends the same way loses the pixel a
   *  shape only partly covers, and that pixel is where the antialiasing is. */
  static PixelRect enclosing(const Rect& r) {
    if (r.isEmpty()) return {};
    const int l = (int) std::floor(r.x);
    const int t = (int) std::floor(r.y);
    const int rr = (int) std::ceil(r.right());
    const int b = (int) std::ceil(r.bottom());
    return {l, t, rr - l, b - t};
  }
};

/**
 * A 2x3 affine transform, in the order [a c e; b d f].
 *
 * Same layout as every other 2D graphics API, so a reader who knows one knows
 * this. Composition is `then`: `a.then(b)` applies a FIRST -- which is the
 * order people say it in, and the opposite of matrix multiplication written
 * left to right. Getting that backwards is the classic transform bug, so it
 * is named rather than left to an operator.
 */
struct Transform {
  float a = 1.0f, b = 0.0f, c = 0.0f, d = 1.0f, e = 0.0f, f = 0.0f;

  Transform() = default;
  Transform(float ta, float tb, float tc, float td, float te, float tf)
      : a(ta), b(tb), c(tc), d(td), e(te), f(tf) {}

  static Transform translation(float dx, float dy) { return {1, 0, 0, 1, dx, dy}; }
  static Transform scaling(float sx, float sy) { return {sx, 0, 0, sy, 0, 0}; }

  /**
   * The transform that undoes this one, and whether there is one.
   *
   * Needed by anything that maps a DESTINATION pixel back to a source -- which
   * is how a rotated image is drawn, because walking the source and scattering
   * into the destination leaves holes wherever the mapping stretches.
   *
   * A singular transform -- a zero scale, which is what a collapsed component
   * produces -- has no inverse, and returns false rather than infinities that
   * then propagate into a sampling loop as NaN coordinates.
   */
  bool invert(Transform* out) const {
    const float determinant = a * d - b * c;
    if (determinant == 0.0f || !(determinant == determinant)) return false;
    const float inverse = 1.0f / determinant;
    if (!out) return true;
    out->a = d * inverse;
    out->b = -b * inverse;
    out->c = -c * inverse;
    out->d = a * inverse;
    out->e = (c * f - d * e) * inverse;
    out->f = (b * e - a * f) * inverse;
    return true;
  }

  static Transform rotation(float radians) {
    const float s = std::sin(radians), co = std::cos(radians);
    return {co, s, -s, co, 0, 0};
  }

  /** Rotation about a point rather than the origin, which is what every
   *  rotating control actually wants. */
  static Transform rotationAbout(float radians, Point centre) {
    return translation(-centre.x, -centre.y)
        .then(rotation(radians))
        .then(translation(centre.x, centre.y));
  }

  Point apply(Point p) const { return {a * p.x + c * p.y + e, b * p.x + d * p.y + f}; }

  /** This transform, then `next`. */
  Transform then(const Transform& n) const {
    return {a * n.a + b * n.c,         a * n.b + b * n.d,
            c * n.a + d * n.c,         c * n.b + d * n.d,
            e * n.a + f * n.c + n.e,   e * n.b + f * n.d + n.f};
  }

  bool isIdentity() const {
    return a == 1.0f && b == 0.0f && c == 0.0f && d == 1.0f && e == 0.0f && f == 0.0f;
  }

  /** How much this scales lengths, as one number.
   *
   *  The square root of the determinant's magnitude -- the factor by which
   *  AREA changes, square-rooted back to a length. Used to decide how finely
   *  to flatten a curve: a path scaled up 4x needs finer segments or its
   *  curves become visible polygons. */
  float scaleFactor() const {
    const float det = std::fabs(a * d - b * c);
    return std::sqrt(det > 0.0f ? det : 0.0f);
  }
};

} // namespace gfx
} // namespace sonore
