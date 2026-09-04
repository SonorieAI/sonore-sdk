// SPDX-License-Identifier: Apache-2.0
//
// Bitmap: premultiplied RGBA8, and the antialiased rasteriser that fills it.
//
// ── Why this SDK rasterises rather than calling the platform ────────────────
//
// A framework draws through Direct2D, CoreGraphics and Cairo. This draws into
// a buffer and lets each platform blit it. The reason is testability.
//
// A generated interface cannot be checked by looking at it -- "attractive" is
// not assertable, and nobody here can see a Mac at all. But a rasteriser that
// writes into memory CAN be checked: fill a rectangle and count the pixels,
// fill a circle and compare opposite sides, fill something off-screen and
// prove the buffer did not change. Those tests run anywhere, in a second,
// with no window.
//
// It also means one renderer instead of three, so there is no bug that only
// appears on the platform nobody can build for.
//
// ── The antialiasing, and why 16 sub-rows ───────────────────────────────────
//
// Coverage is measured by sampling each pixel row at 16 evenly spaced heights
// and, within each, computing EXACT horizontal coverage of the spans. So the
// horizontal direction is continuous and the vertical is quantised to
// sixteenths.
//
// That asymmetry is deliberate and cheap: a near-horizontal edge -- the top of
// a slider track, the shoulder of a meter -- is where banding would be visible,
// and it is horizontal coverage that resolves it. Sixteen levels vertically is
// finer than an 8-bit channel can show on a near-vertical edge.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "colour.h"
#include "geometry.h"
#include "gradient.h"
#include "path.h"

namespace sonore {
namespace gfx {

/** A premultiplied RGBA8 image, row-major, tightly packed. */
class Bitmap {
public:
  Bitmap() = default;
  Bitmap(int w, int h) { resize(w, h); }

  void resize(int w, int h) {
    width_ = w < 0 ? 0 : w;
    height_ = h < 0 ? 0 : h;
    pixels_.assign((size_t) width_ * (size_t) height_ * 4u, 0);
  }

  int width() const { return width_; }
  int height() const { return height_; }
  bool isEmpty() const { return width_ <= 0 || height_ <= 0; }
  PixelRect bounds() const { return {0, 0, width_, height_}; }

  uint8_t* data() { return pixels_.data(); }
  const uint8_t* data() const { return pixels_.data(); }
  size_t sizeInBytes() const { return pixels_.size(); }

  /** Row-major byte offset. Public because a blitter needs it. */
  size_t offsetOf(int x, int y) const { return ((size_t) y * (size_t) width_ + (size_t) x) * 4u; }

  void clear(Colour c) {
    const PremulColour p = PremulColour::from(c);
    for (size_t i = 0; i < pixels_.size(); i += 4) {
      pixels_[i] = p.r;
      pixels_[i + 1] = p.g;
      pixels_[i + 2] = p.b;
      pixels_[i + 3] = p.a;
    }
  }

  /** Out of range reads back fully transparent rather than reading past the
   *  buffer. A test asking about a pixel that is not there wants an answer,
   *  not a crash. */
  PremulColour pixelAt(int x, int y) const {
    if (x < 0 || y < 0 || x >= width_ || y >= height_) return {};
    const size_t o = offsetOf(x, y);
    return {pixels_[o], pixels_[o + 1], pixels_[o + 2], pixels_[o + 3]};
  }

  void setPixel(int x, int y, PremulColour c) {
    if (x < 0 || y < 0 || x >= width_ || y >= height_) return;
    const size_t o = offsetOf(x, y);
    pixels_[o] = c.r;
    pixels_[o + 1] = c.g;
    pixels_[o + 2] = c.b;
    pixels_[o + 3] = c.a;
  }

  /** `src` composited over the pixel, with coverage. The one write path, so
   *  the bounds check lives in one place. */
  void blend(int x, int y, PremulColour src, uint8_t coverage) {
    if (coverage == 0 || x < 0 || y < 0 || x >= width_ || y >= height_) return;
    const size_t o = offsetOf(x, y);
    const PremulColour dst{pixels_[o], pixels_[o + 1], pixels_[o + 2], pixels_[o + 3]};
    const PremulColour result = over(src, dst, coverage);
    pixels_[o] = result.r;
    pixels_[o + 1] = result.g;
    pixels_[o + 2] = result.b;
    pixels_[o + 3] = result.a;
  }

private:
  int width_ = 0, height_ = 0;
  std::vector<uint8_t> pixels_;
};

/**
 * Fills paths into a Bitmap.
 *
 * Holds its scratch buffers between calls: a UI redraw fills hundreds of
 * paths, and allocating an edge list per fill would put the allocator on the
 * repaint path.
 */

/**
 * A bitmap repeated across the plane, in DEVICE space.
 *
 * A panel texture, a brushed-metal background, a grid. The alternative is
 * drawing the same small image in a double loop, which works and costs a
 * separate blit per tile plus the arithmetic to stop at the edges -- and gets
 * the edges wrong the first time somebody clips it to a rounded rectangle.
 *
 * NEAREST sampling, deliberately. A texture tiled at its natural size wants to
 * stay crisp, and bilinear at 1:1 blurs every pixel with its neighbours for no
 * benefit at all. A scaled tile is the unusual case and takes the same
 * treatment rather than a second code path with a different look.
 */
struct TiledImage {
  const Bitmap* image = nullptr;
  /** Where the image's top-left corner sits, in DEVICE pixels. Moving this is
   *  how a texture scrolls with the thing it is painted on rather than staying
   *  nailed to the window. */
  float originX = 0.0f, originY = 0.0f;
  /** Device pixels per image pixel. 1 is natural size. */
  float scale = 1.0f;
  uint8_t opacity = 255;

  bool isEmpty() const { return !image || image->isEmpty() || scale <= 0.0f; }

  PremulColour sample(float x, float y) const {
    if (isEmpty()) return {};
    const float inImageX = (x - originX) / scale;
    const float inImageY = (y - originY) / scale;

    // A TRUE modulo, not the remainder operator: for a negative coordinate C's
    // % gives a negative answer, and a texture would mirror rather than repeat
    // everywhere left of or above its origin.
    const int w = image->width(), h = image->height();
    int sx = (int) std::floor(inImageX) % w;
    int sy = (int) std::floor(inImageY) % h;
    if (sx < 0) sx += w;
    if (sy < 0) sy += h;

    PremulColour c = image->pixelAt(sx, sy);
    if (opacity != 255) {
      const uint32_t o = opacity;
      c.r = (uint8_t) ((c.r * o + 127) / 255);
      c.g = (uint8_t) ((c.g * o + 127) / 255);
      c.b = (uint8_t) ((c.b * o + 127) / 255);
      c.a = (uint8_t) ((c.a * o + 127) / 255);
    }
    return c;
  }
};

class Rasteriser {
public:
  /** Sub-rows per pixel row. Sixteen gives 17 distinct coverage levels
   *  vertically, which is past what an 8-bit channel resolves on the edges
   *  where vertical sampling is what you see. */
  static constexpr int kSubRows = 16;

  /**
   * Fill `path`, transformed, with `colour`, clipped to `clip`.
   *
   * Nothing outside `clip` or outside the bitmap is touched -- checked by a
   * test that fills a shape far off-screen and compares the whole buffer.
   */
  /**
   * Fill a path with one colour, or with a gradient.
   *
   * `gradient` is in DEVICE space -- Graphics transforms it once per fill
   * rather than inverting the transform per pixel -- and null means the flat
   * colour. One rasteriser rather than two: coverage, winding and the span
   * arithmetic are the hard parts and having a second copy of them that only
   * gradients exercise is how the two drift.
   */
  void fill(Bitmap& target, const Path& path, const Transform& transform, Colour colour,
            FillRule rule = FillRule::NonZero, PixelRect clip = {-1, -1, -1, -1},
            const ColourGradient* gradient = nullptr, const TiledImage* tile = nullptr) {
    // A transparent flat colour draws nothing; a gradient decides per pixel,
    // so its own transparency cannot be judged here.
    if (target.isEmpty()) return;
    if (!gradient && !tile && colour.isTransparent()) return;
    if (gradient && gradient->isEmpty()) return;
    if (tile && tile->isEmpty()) return;

    PixelRect region = (clip.w < 0) ? target.bounds() : clip.intersection(target.bounds());
    if (region.isEmpty()) return;

    path.flatten(transform, contours_);
    if (contours_.empty()) return;

    // Edges, with the sign that says which way the contour was wound. Purely
    // horizontal edges are dropped: they cross no scanline, and keeping them
    // would put two crossings at the same y and corrupt the winding count.
    edges_.clear();
    float top = 1e30f, bottom = -1e30f;
    for (const auto& contour : contours_) {
      // Filled as if closed whichever it is: an open contour still has an
      // inside, and a caller filling one means the shape they drew.
      const size_t n = contour.points.size();
      if (n < 3) continue;
      for (size_t i = 0; i < n; ++i) {
        const Point a = contour.points[i];
        const Point b = contour.points[(i + 1) % n];
        // A NaN or infinite coordinate -- an SVG with "1e999" in it, a
        // transform that overflowed -- is not geometry. NaN passes every
        // comparison below in the wrong direction: the edge was kept, the
        // crossing came out NaN, and addSpan turned it into INT_MIN and an
        // index a billion pixels off the row. The fuzzer found it the moment
        // the path parser stopped hanging on the same file.
        if (!std::isfinite(a.x) || !std::isfinite(a.y) || !std::isfinite(b.x) ||
            !std::isfinite(b.y))
          continue;
        if (a.y == b.y) continue;
        edges_.push_back({a, b, a.y < b.y ? 1 : -1});
        top = std::min(top, std::min(a.y, b.y));
        bottom = std::max(bottom, std::max(a.y, b.y));
      }
    }
    if (edges_.empty()) return;

    // Clamped BEFORE the conversion: a float past INT_MAX cast to int is
    // undefined, and the region is the only range that can be drawn anyway.
    top = std::max(top, (float) region.y - 1.0f);
    bottom = std::min(bottom, (float) region.bottom() + 1.0f);
    int y0 = (int) std::floor(top);
    int y1 = (int) std::ceil(bottom);
    y0 = std::max(y0, region.y);
    y1 = std::min(y1, region.bottom());
    if (y1 <= y0) return;

    const PremulColour src = PremulColour::from(colour);
    const float subStep = 1.0f / (float) kSubRows;
    // Coverage accumulates in 0..kSubRows per pixel, then scales to 0..255.
    coverage_.assign((size_t) region.w, 0.0f);

    for (int py = y0; py < y1; ++py) {
      std::fill(coverage_.begin(), coverage_.end(), 0.0f);

      for (int sub = 0; sub < kSubRows; ++sub) {
        // Sampled at the CENTRE of each sub-row. Sampling at the top edge
        // biases every shape upward by half a sub-row, which shows as a
        // one-pixel drift on a shape that should be centred.
        const float sy = (float) py + ((float) sub + 0.5f) * subStep;
        crossings_.clear();
        for (const Edge& e : edges_) {
          const float lo = std::min(e.a.y, e.b.y);
          const float hi = std::max(e.a.y, e.b.y);
          // Half-open in y: a vertex shared by two edges must be counted
          // once, or the winding number jumps by two where they meet and a
          // NonZero fill grows a notch.
          if (sy < lo || sy >= hi) continue;
          const float t = (sy - e.a.y) / (e.b.y - e.a.y);
          crossings_.push_back({e.a.x + t * (e.b.x - e.a.x), e.winding});
        }
        if (crossings_.size() < 2) continue;
        std::sort(crossings_.begin(), crossings_.end(),
                  [](const Crossing& l, const Crossing& r) { return l.x < r.x; });

        int winding = 0;
        for (size_t i = 0; i + 1 < crossings_.size(); ++i) {
          winding += crossings_[i].winding;
          const bool inside = rule == FillRule::NonZero
                                  ? winding != 0
                                  : ((int) (i + 1) & 1) != 0;
          if (!inside) continue;
          addSpan(crossings_[i].x, crossings_[i + 1].x, region, subStep);
        }
      }

      for (int i = 0; i < region.w; ++i) {
        const float c = coverage_[(size_t) i];
        if (c <= 0.0f) continue;
        const float clamped = c > 1.0f ? 1.0f : c;
        const uint8_t cov = (uint8_t) (clamped * 255.0f + 0.5f);
        // A tile takes the same per-pixel hook a gradient does, sampled at the
        // same pixel CENTRE -- two ways of deciding a colour per pixel, one
        // place that decides it, so the two cannot disagree about where a
        // pixel is.
        if (tile) {
          const PremulColour here =
              tile->sample((float) (region.x + i) + 0.5f, (float) py + 0.5f);
          if (here.a == 0) continue;
          target.blend(region.x + i, py, here, cov);
          continue;
        }
        if (!gradient) {
          target.blend(region.x + i, py, src, cov);
          continue;
        }
        // The pixel CENTRE, matching where coverage is decided. Sampling a
        // corner shifts the whole ramp half a pixel, which shows as a seam
        // where two gradient-filled shapes meet.
        const Colour here = gradient->colourAt(
            Point((float) (region.x + i) + 0.5f, (float) py + 0.5f));
        if (here.isTransparent()) continue;
        target.blend(region.x + i, py, PremulColour::from(here), cov);
      }
    }
  }

private:
  struct Edge {
    Point a, b;
    int winding;
  };
  struct Crossing {
    float x;
    int winding;
  };

  /** One horizontal span's EXACT contribution. The partial pixels at each end
   *  are what makes a near-vertical edge smooth; rounding them to whole
   *  pixels is the difference between an antialiased shape and a jagged one. */
  void addSpan(float x0, float x1, const PixelRect& region, float weight) {
    // Written as !(x1 > x0), not (x1 <= x0): a NaN fails BOTH orderings, and
    // the first spelling let it through to the int conversions below.
    if (!(x1 > x0)) return;
    x0 = std::max(x0, (float) region.x);
    x1 = std::min(x1, (float) region.right());
    if (!(x1 > x0)) return;

    const int first = (int) std::floor(x0);
    const int last = (int) std::ceil(x1) - 1;
    if (first == last) {
      coverage_[(size_t) (first - region.x)] += (x1 - x0) * weight;
      return;
    }
    coverage_[(size_t) (first - region.x)] += ((float) (first + 1) - x0) * weight;
    for (int px = first + 1; px < last; ++px) coverage_[(size_t) (px - region.x)] += weight;
    coverage_[(size_t) (last - region.x)] += (x1 - (float) last) * weight;
  }

  std::vector<Contour> contours_;
  std::vector<Edge> edges_;
  std::vector<Crossing> crossings_;
  std::vector<float> coverage_;
};

} // namespace gfx
} // namespace sonore
