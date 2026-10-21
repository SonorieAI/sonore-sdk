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

#include "../simd.h" // which vector unit, if any: blendRun uses SSE2's
#include "colour.h"
#include "geometry.h"
#include "gradient.h"
#include "path.h"

namespace sonore {
namespace gfx {

/** A premultiplied RGBA8 image, row-major, tightly packed. */
class Bitmap {
public:
  /** Empty: 0 by 0, no pixels. */
  Bitmap() = default;
  /** `w` by `h` pixels, all transparent; a negative size is 0. Allocates. */
  Bitmap(int w, int h) { resize(w, h); }

  /** Reallocate to `w` by `h` (a negative size is 0) with every pixel
   *  transparent: the old contents are not kept, and data() moves. */
  void resize(int w, int h) {
    width_ = w < 0 ? 0 : w;
    height_ = h < 0 ? 0 : h;
    pixels_.assign((size_t) width_ * (size_t) height_ * 4u, 0);
  }

  /** In pixels. */
  int width() const { return width_; }
  /** In pixels. */
  int height() const { return height_; }
  /** Whether it has no pixels. */
  bool isEmpty() const { return width_ <= 0 || height_ <= 0; }
  /** (0, 0, width, height). */
  PixelRect bounds() const { return {0, 0, width_, height_}; }

  /** The pixels: four bytes each, R G B A premultiplied, rows top to bottom with
   *  no padding (offsetOf finds one). Valid until the next resize. */
  uint8_t* data() { return pixels_.data(); }
  /** The same, read-only. */
  const uint8_t* data() const { return pixels_.data(); }
  /** width * height * 4. */
  size_t sizeInBytes() const { return pixels_.size(); }

  /** Row-major byte offset. Public because a blitter needs it. */
  size_t offsetOf(int x, int y) const { return ((size_t) y * (size_t) width_ + (size_t) x) * 4u; }

  /** Set every pixel to `c`, premultiplied on the way in -- replaced, not
   *  blended. */
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

  /** Write `c` at (x, y), replacing what was there. Out of range does nothing. */
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
  /** The texture; not owned, and must outlive the fill. Null is no tile. */
  const Bitmap* image = nullptr;
  /** Where the image's top-left corner sits, in DEVICE pixels. Moving this is
   *  how a texture scrolls with the thing it is painted on rather than staying
   *  nailed to the window. */
  float originX = 0.0f, originY = 0.0f;  ///< the same, vertically
  /** Device pixels per image pixel. 1 is natural size. */
  float scale = 1.0f;
  /** 0..255, multiplied into every sample. */
  uint8_t opacity = 255;

  /** No image, an empty one, or a scale of 0 or less: nothing to fill with. */
  bool isEmpty() const { return !image || image->isEmpty() || scale <= 0.0f; }

  /** The texel under the device point (x, y), the image repeating in every
   *  direction, with `opacity` applied; transparent when empty. */
  PremulColour sample(float x, float y) const {
    if (isEmpty()) return {};
    const float inImageX = (x - originX) / scale;
    const float inImageY = (y - originY) / scale;

    // An origin or a scale gone to NaN or infinity -- a texture scrolled by
    // an offset that overflowed -- samples nothing.
    if (!std::isfinite(inImageX) || !std::isfinite(inImageY)) return {};

    // A TRUE modulo, not the remainder operator: for a negative coordinate C's
    // % gives a negative answer, and a texture would mirror rather than repeat
    // everywhere left of or above its origin. The remainder is taken in float,
    // where it is exact, BEFORE the conversion: converting a coordinate past
    // INT_MAX -- a scale of 1e-30 makes one of every pixel -- is undefined.
    const int w = image->width(), h = image->height();
    int sx = (int) std::fmod(std::floor(inImageX), (float) w);
    int sy = (int) std::fmod(std::floor(inImageY), (float) h);
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

namespace detail {

/**
 * How much of each DEVICE pixel a clip lets through, 0 to 255, inside `area`
 * -- and nothing outside it.
 *
 * What a path clip leaves behind (Graphics::clipToPath) and what a mask does
 * (Graphics::endMask). A rectangle clip is a PixelRect and costs nothing per
 * pixel; anything rounder than that is a coverage per pixel, multiplied into
 * the coverage of whatever is drawn under it -- so a rounded panel clipping an
 * image keeps an antialiased corner rather than a staircase.
 */
struct CoverageMask {
  PixelRect area;
  std::vector<uint8_t> coverage; // area.w * area.h, row-major

  uint8_t at(int x, int y) const {
    if (x < area.x || y < area.y || x >= area.right() || y >= area.bottom()) return 0;
    return coverage[(size_t) (y - area.y) * (size_t) area.w + (size_t) (x - area.x)];
  }
};

/** Two coverages, one inside the other: 255 x 255 is 255, and anything x 0 is 0. */
inline uint8_t multiplyCoverage(uint8_t a, uint8_t b) {
  return (uint8_t) (((unsigned) a * (unsigned) b + 127u) / 255u);
}

/**
 * `src` over the `n` pixels from `p` on, at full coverage: what Bitmap::blend
 * does to each of them, without its bounds test and its call per pixel.
 *
 * A wash over a backdrop, a panel at 72%, the inside of any translucent shape
 * -- most of the pixels a repaint blends are runs under one colour, and they
 * went through blend one call at a time: two thirds of a whole editor's
 * repaint. The arithmetic is `over`'s, written out here rather than called:
 * MSVC stopped inlining `over` into the rasteriser's loops when they called
 * it from a second place, and a call per pixel is what this removes. Checked
 * against `over` for every channel value, alpha and coverage (sdk_tests,
 * "[gfx blend runs]"). An opaque colour is stored, which is what `over` gives
 * at full coverage. Four pixels at a time where simd.h finds SSE2; the
 * scalar loop finishes the run, and is all of it on ARM, in WebAssembly and
 * under SONORE_NO_SIMD. The caller has clipped `p .. p + n` to the bitmap.
 */
inline void blendRun(uint8_t* p, int n, PremulColour src) {
  int i = 0;
#if defined(SONORE_SIMD_SSE)
  // Four pixels at a time. Every step of `over`'s channel arithmetic fits a
  // 16-bit lane -- v * inv + 127 is at most 65152, and adding its top byte at
  // most 65406 -- so this is the scalar loop below, exactly, sixteen channels
  // at once; the add of the source wraps as the uint8_t sum does.
  const uint32_t word = (uint32_t) src.r | ((uint32_t) src.g << 8) | ((uint32_t) src.b << 16) |
                        ((uint32_t) src.a << 24);
  const __m128i source = _mm_set1_epi32((int) word);
  if (src.a == 255) {
    for (; i + 4 <= n; i += 4, p += 16) _mm_storeu_si128((__m128i*) p, source);
  } else {
    const __m128i zero = _mm_setzero_si128();
    const __m128i invLanes = _mm_set1_epi16((short) (255 - src.a));
    const __m128i half = _mm_set1_epi16(127);
    for (; i + 4 <= n; i += 4, p += 16) {
      const __m128i d = _mm_loadu_si128((const __m128i*) p);
      __m128i lo = _mm_add_epi16(_mm_mullo_epi16(_mm_unpacklo_epi8(d, zero), invLanes), half);
      __m128i hi = _mm_add_epi16(_mm_mullo_epi16(_mm_unpackhi_epi8(d, zero), invLanes), half);
      lo = _mm_srli_epi16(_mm_add_epi16(lo, _mm_srli_epi16(lo, 8)), 8);
      hi = _mm_srli_epi16(_mm_add_epi16(hi, _mm_srli_epi16(hi, 8)), 8);
      _mm_storeu_si128((__m128i*) p, _mm_add_epi8(_mm_packus_epi16(lo, hi), source));
    }
  }
#endif
  if (src.a == 255) {
    for (; i < n; ++i, p += 4) {
      p[0] = src.r;
      p[1] = src.g;
      p[2] = src.b;
      p[3] = src.a;
    }
    return;
  }
  const uint32_t inv = 255u - (uint32_t) src.a;
  auto scale = [inv](uint8_t v) {
    const uint32_t x = (uint32_t) v * inv + 127u;
    return (uint8_t) ((x + (x >> 8)) >> 8);
  };
  for (; i < n; ++i, p += 4) {
    p[0] = (uint8_t) (src.r + scale(p[0]));
    p[1] = (uint8_t) (src.g + scale(p[1]));
    p[2] = (uint8_t) (src.b + scale(p[2]));
    p[3] = (uint8_t) (src.a + scale(p[3]));
  }
}

/** One pixel, `src` over it at `coverage` (not 0): Bitmap::blend for a pixel
 *  the caller already knows is inside the bitmap, with `over` written out for
 *  the reason blendRun gives. */
inline void blendPixel(uint8_t* p, PremulColour src, uint8_t coverage) {
  auto scale = [](uint8_t v, uint32_t by) {
    const uint32_t x = (uint32_t) v * by + 127u;
    return (uint8_t) ((x + (x >> 8)) >> 8);
  };
  if (coverage != 255) {
    src = PremulColour(scale(src.r, coverage), scale(src.g, coverage), scale(src.b, coverage),
                       scale(src.a, coverage));
  }
  const uint32_t inv = 255u - (uint32_t) src.a;
  p[0] = (uint8_t) (src.r + scale(p[0], inv));
  p[1] = (uint8_t) (src.g + scale(p[1], inv));
  p[2] = (uint8_t) (src.b + scale(p[2], inv));
  p[3] = (uint8_t) (src.a + scale(p[3], inv));
}

/**
 * `src` over the `n` pixels from `p` on, each at its own coverage from
 * `coverage` (0 leaves a pixel alone): a row of a filled shape, or of a
 * glyph's cached coverage. Runs at full coverage go to blendRun and the rest
 * a pixel at a time, so the rule is `over`'s as Bitmap::blend applies it.
 *
 * One call a row rather than one a pixel. MSVC does not inline blendPixel
 * into the rasteriser's large functions, and text -- every pixel a glyph
 * touches -- paid a call for each.
 */
inline void blendCoverage(uint8_t* p, const uint8_t* coverage, int n, PremulColour src) {
  for (int i = 0; i < n; ++i) {
    const uint8_t c = coverage[i];
    if (c == 0) continue;
    if (c != 255) {
      blendPixel(p + (size_t) i * 4u, src, c);
      continue;
    }
    int j = i + 1;
    while (j < n && coverage[j] == 255) ++j;
    blendRun(p + (size_t) i * 4u, j - i, src);
    i = j - 1;
  }
}

} // namespace detail

/**
 * Fills paths into a Bitmap.
 *
 * Holds its scratch buffers between calls: a UI redraw fills hundreds of
 * paths, and allocating an edge list per fill would put the allocator on the
 * repaint path. Which also makes one Rasteriser single-threaded state: each
 * Graphics owns its own, and two threads filling at once need two. The
 * antialiasing it does is described in the file comment.
 */
class Rasteriser {
public:
  /** Sub-rows per pixel row. Sixteen gives 17 distinct coverage levels
   *  vertically, which is past what an 8-bit channel resolves on the edges
   *  where vertical sampling is what you see. */
  static constexpr int kSubRows = 16;

  /**
   * Fill `path`, transformed, with `colour` -- or with a gradient or a tile
   * -- clipped to `clip`.
   *
   * Nothing outside `clip` or outside the bitmap is touched -- checked by a
   * test that fills a shape far off-screen and compares the whole buffer.
   * A `clip` with a negative width means the whole bitmap.
   *
   * `gradient` is in DEVICE space -- Graphics transforms it once per fill
   * rather than inverting the transform per pixel -- and null means the flat
   * colour. One rasteriser rather than two: coverage, winding and the span
   * arithmetic are the hard parts and having a second copy of them that only
   * gradients exercise is how the two drift.
   *
   * `mask`, when there is one, is a clip finer than `clip`: each pixel's
   * coverage is multiplied by the mask's there.
   */
  void fill(Bitmap& target, const Path& path, const Transform& transform, Colour colour,
            FillRule rule = FillRule::NonZero, PixelRect clip = {-1, -1, -1, -1},
            const ColourGradient* gradient = nullptr, const TiledImage* tile = nullptr,
            const detail::CoverageMask* mask = nullptr) {
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
    sideEdges_.clear();
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
        // Nowhere near a row that will be scanned: such an edge crosses no
        // sample, so it changes no pixel -- and kept, every sub-row of every
        // row walked it again. The same pixels either way, for a fraction of
        // the work when most of a shape is off the region.
        if ((std::max)(a.y, b.y) < (float) region.y - 1.0f ||
            (std::min)(a.y, b.y) > (float) region.bottom() + 1.0f)
          continue;
        const float lo = (std::min)(a.y, b.y), hi = (std::max)(a.y, b.y);
        const int winding = a.y < b.y ? 1 : -1;
        top = (std::min)(top, lo);
        bottom = (std::max)(bottom, hi);
        // Wholly beside the region, a pixel and more clear of it: such an
        // edge's crossings are never an end of a span that is drawn. One to
        // the left only adds its winding to everything right of it; one to
        // the right only ends the last span, at a point past the region's
        // edge where that span is cut anyway. Each is counted per sub-row
        // instead of walked -- see below. "Beside" is judged by where the
        // crossing arithmetic puts the edge's ends, not where they are: a
        // crossing is a.x + t * run with t in 0..1, which however it rounds
        // lies between a.x and a.x + run -- and far out, a.x + run is not
        // b.x. An edge whose run overflowed stays with the others.
        const float run = b.x - a.x;
        const float farEnd = a.x + run;
        if (std::isfinite(run) && (std::max)(a.x, farEnd) < (float) region.x - 1.0f) {
          sideEdges_.push_back({lo, hi, winding, true});
          continue;
        }
        if (std::isfinite(run) && (std::min)(a.x, farEnd) > (float) region.right() + 1.0f) {
          sideEdges_.push_back({lo, hi, winding, false});
          continue;
        }
        edges_.push_back({a, b, winding, lo, hi});
      }
    }
    if (edges_.empty() && sideEdges_.empty()) return;

    // Clamped BEFORE the conversion: a float past INT_MAX cast to int is
    // undefined, and the region is the only range that can be drawn anyway.
    top = (std::max)(top, (float) region.y - 1.0f);
    bottom = (std::min)(bottom, (float) region.bottom() + 1.0f);
    int y0 = (int) std::floor(top);
    int y1 = (int) std::ceil(bottom);
    y0 = (std::max)(y0, region.y);
    y1 = (std::min)(y1, region.bottom());
    if (y1 <= y0) return;

    const PremulColour src = PremulColour::from(colour);
    const float subStep = 1.0f / (float) kSubRows;

    // A linear gradient along an axis has one colour per row, or per column:
    // see prepareGradient. 2 is top to bottom.
    const int axis = gradient ? prepareGradient(*gradient, region) : 0;

    // One pixel, at a coverage. A tile takes the same per-pixel hook a
    // gradient does, sampled at the same pixel CENTRE -- two ways of deciding
    // a colour per pixel, one place that decides it, so the two cannot
    // disagree about where a pixel is. A flat opaque colour at full coverage
    // is a plain store: `over` gives exactly the source there.
    uint8_t* const pixels = target.data();
    auto paint = [&](int x, int y, uint8_t cov) {
      if (mask) {
        cov = detail::multiplyCoverage(cov, mask->at(x, y));
        if (cov == 0) return;
      }
      if (tile) {
        const PremulColour here = tile->sample((float) x + 0.5f, (float) y + 0.5f);
        if (here.a == 0) return;
        target.blend(x, y, here, cov);
        return;
      }
      if (!gradient) {
        if (cov == 255 && src.a == 255) {
          uint8_t* p = pixels + target.offsetOf(x, y);
          p[0] = src.r;
          p[1] = src.g;
          p[2] = src.b;
          p[3] = src.a;
          return;
        }
        target.blend(x, y, src, cov);
        return;
      }
      // The pixel CENTRE, matching where coverage is decided. Sampling a
      // corner shifts the whole ramp half a pixel, which shows as a seam
      // where two gradient-filled shapes meet. (Transparent is alpha 0 in
      // either form, so the premultiplied colour says it.)
      const PremulColour here = gradientColour(x, y);
      if (here.a == 0) return;
      target.blend(x, y, here, cov);
    };

    // A rectangle on whole pixels -- a window's background, a panel, every
    // fillAll -- covers each pixel it touches completely: its two edges are
    // vertical at whole x, and every sub-row of every row it spans lies
    // inside it. The scan below adds sixteen sixteenths to each of those
    // pixels to arrive at 255, and that was most of the cost of painting a
    // whole editor: 17 of 24 ms for a 24-row generic editor at 2x was its
    // background. Here each gets 255 directly; the sum was exact, so the
    // pixels are the same.
    //
    // A flat colour there, opaque or not, is blended a row at a time
    // (detail::blendRun): the wash over a backdrop is one of these.
    //
    // Its top and bottom need not be on whole pixels. Every pixel of a row
    // such a box crosses is covered by the same number of that row's
    // sub-rows -- those whose sample lies in the box, by the scan's own test
    // on the scan's own samples -- and the scan sums exactly that many
    // sixteenths into each; here the number is counted once a row and the
    // row painted as the scan paints one. A waveform is a column like this
    // per pixel, a meter's bar one more, and the scan spent sixteen sub-rows
    // of edge arithmetic on every row of each: 30 us for a 1-pixel column
    // 100 rows tall, and 40 ms to draw a thumbnail 1200 pixels wide.
    const bool solid = !tile && !gradient && !mask;
    // A gradient with one colour per row, under no mask, is a flat colour
    // for the length of each row.
    const bool solidRows = axis == 2 && !mask;
    {
      int left = 0, right = 0;
      float boxTop = 0.0f, boxBottom = 0.0f;
      if (verticalBox(region, left, right, boxTop, boxBottom)) {
        if (row_.size() < (size_t) region.w) row_.resize((size_t) region.w, 0);
        for (int py = y0; py < y1; ++py) {
          int inside = 0;
          for (int sub = 0; sub < kSubRows; ++sub) {
            const float sy = (float) py + ((float) sub + 0.5f) * subStep;
            if (boxTop <= sy && sy < boxBottom) ++inside;
          }
          if (inside == 0) continue;
          if (inside == kSubRows) {
            if (solid || solidRows) {
              const PremulColour c = solid ? src : rowGradientColour(py);
              if (c.a != 0) detail::blendRun(pixels + target.offsetOf(left, py), right - left, c);
              continue;
            }
            for (int px = left; px < right; ++px) paint(px, py, 255);
            continue;
          }
          // Part of the row: the scan's coverage for it, converted as the
          // scan converts it, and painted as the scan paints a row.
          const float c = (float) inside * subStep;
          const uint8_t cov = (uint8_t) (c * 255.0f + 0.5f);
          if (solid || solidRows) {
            const PremulColour rowSrc = solid ? src : rowGradientColour(py);
            if (rowSrc.a == 0) continue;
            std::fill(row_.begin(), row_.begin() + (right - left), cov);
            detail::blendCoverage(pixels + target.offsetOf(left, py), row_.data(), right - left, rowSrc);
            continue;
          }
          for (int px = left; px < right; ++px) paint(px, py, cov);
        }
        return;
      }
    }

    // Coverage accumulates in 0..kSubRows per pixel, then scales to 0..255.
    // Grown, not cleared: every row puts back to zero what its spans
    // touched, so the rows are zero between fills, and clearing the width
    // of a 4K target for each of twenty thousand small shapes showed.
    if (coverage_.size() < (size_t) region.w) coverage_.resize((size_t) region.w, 0.0f);
    if (steps_.size() < (size_t) region.w + 1u) steps_.resize((size_t) region.w + 1u, 0.0f);
    if (row_.size() < (size_t) region.w) row_.resize((size_t) region.w, 0);

    // An ACTIVE edge list. Every sub-row walked every edge, so the cost was
    // rows x 16 x edges: a stroke is thousands of edges -- each segment a
    // quad, each round join a circle -- and on a 400-pixel canvas a 400-point
    // waveform stroked two pixels wide took 17 ms, 97 ms at 24 pixels with
    // round joins, and a knob's arc 2.4 ms. Sorted by where they start, an
    // edge joins the list when the scan reaches its top and leaves it below
    // its bottom; the scan only moves down, so each edge is admitted once and
    // dropped once. Now 1.6 ms, 21 ms and 0.4 ms -- for the same crossings,
    // from the same arithmetic, sorted by x as before: the same pixels
    // exactly (tests/fuzz_graphics --hash, 20000 frames, old against new).
    //
    // The list is kept in x order from one sub-row to the next. A sixteenth
    // of a pixel further down, edges change order only where two of them
    // cross, so the last sub-row's order is almost right and an insertion
    // sort puts it right in about one pass -- where sorting from scratch was
    // the largest cost left in a line of text. Ties between equal x may land
    // in another order, and that changes nothing: the span between them is
    // empty, and the winding after the last of them is the same sum.
    std::sort(edges_.begin(), edges_.end(), [](const Edge& l, const Edge& r) { return l.lo < r.lo; });
    crossings_.clear();
    size_t nextEdge = 0;

    // The edges beside the region, as counts per sub-row. A stroke far wider
    // than its canvas, or a shape scrolled mostly out of view, puts thousands
    // of edges across every row and all of them outside: walking each per
    // sub-row was 130 million crossings and over a second for one stroke the
    // fuzzer drew, to find that none of them was in the region. An edge is
    // in the sub-rows whose sample satisfies lo <= sy < hi -- the same test,
    // on the same sample values, as the walk below -- so the counts are
    // exact: winding and number from the left, number on the right.
    const size_t subRowCount = (size_t) (y1 - y0) * (size_t) kSubRows;
    subRowY_.resize(subRowCount);
    for (int py = y0; py < y1; ++py)
      for (int sub = 0; sub < kSubRows; ++sub)
        subRowY_[(size_t) (py - y0) * kSubRows + (size_t) sub] =
            (float) py + ((float) sub + 0.5f) * subStep;
    sideCounts_.assign(subRowCount + 1, SideCount{});
    for (const SideEdge& e : sideEdges_) {
      const size_t from = (size_t) (std::lower_bound(subRowY_.begin(), subRowY_.end(), e.lo) - subRowY_.begin());
      const size_t to = (size_t) (std::lower_bound(subRowY_.begin(), subRowY_.end(), e.hi) - subRowY_.begin());
      if (from >= to) continue;
      SideCount& start = sideCounts_[from];
      SideCount& end = sideCounts_[to];
      if (e.left) {
        start.leftWinding += e.winding;
        end.leftWinding -= e.winding;
        ++start.left;
        --end.left;
      } else {
        ++start.right;
        --end.right;
      }
    }
    SideCount beside{};

    for (int py = y0; py < y1; ++py) {
      // The coverage row is cleared, and read back, only where spans landed:
      // a label on a wide editor touches a few dozen of its pixels.
      touchedFrom_ = region.w;
      touchedTo_ = 0;

      for (int sub = 0; sub < kSubRows; ++sub) {
        // Sampled at the CENTRE of each sub-row. Sampling at the top edge
        // biases every shape upward by half a sub-row, which shows as a
        // one-pixel drift on a shape that should be centred.
        const size_t subRow = (size_t) (py - y0) * kSubRows + (size_t) sub;
        const float sy = subRowY_[subRow];
        beside.leftWinding += sideCounts_[subRow].leftWinding;
        beside.left += sideCounts_[subRow].left;
        beside.right += sideCounts_[subRow].right;
        // Half-open in y, lo <= sy < hi: a vertex shared by two edges must be
        // counted once, or the winding number jumps by two where they meet
        // and a NonZero fill grows a notch. Admitted at lo, dropped at hi.
        while (nextEdge < edges_.size() && edges_[nextEdge].lo <= sy) {
          crossings_.push_back({0.0f, edges_[nextEdge].winding, nextEdge});
          ++nextEdge;
        }
        size_t kept = 0;
        for (size_t k = 0; k < crossings_.size(); ++k) {
          Crossing c = crossings_[k];
          const Edge& e = edges_[c.edge];
          if (!(e.hi > sy)) continue;
          const float t = (sy - e.a.y) / (e.b.y - e.a.y);
          c.x = e.a.x + t * (e.b.x - e.a.x);
          crossings_[kept++] = c;
        }
        crossings_.resize(kept);
        if (kept + (size_t) beside.left + (size_t) beside.right < 2) continue;
        sortCrossings();

        // Inside or out between one crossing and the next: the winding, or
        // for EvenOdd the number of crossings, of everything to the left --
        // the edges beside the region first, whose crossings lie left of any
        // span that is drawn. A span is cut to the region, so it may start
        // from a pixel left of it and, if an edge to the right ends it, run
        // to a pixel right of it.
        int winding = beside.leftWinding;
        int crossed = beside.left;
        float from = (float) region.x - 1.0f;
        auto inside = [&] { return rule == FillRule::NonZero ? winding != 0 : (crossed & 1) != 0; };
        for (const Crossing& c : crossings_) {
          if (inside()) addSpan(from, c.x, region, subStep);
          winding += c.winding;
          ++crossed;
          from = c.x;
        }
        if (beside.right > 0 && inside()) addSpan(from, (float) region.right() + 1.0f, region, subStep);
      }

      // The row's coverage first: what its spans' ends put in each pixel,
      // and the whole pixels between, summed from their steps -- multiples
      // of a sub-row's weight, exact in a float.
      float whole = 0.0f;
      for (int i = touchedFrom_; i < touchedTo_; ++i) {
        whole += steps_[(size_t) i];
        steps_[(size_t) i] = 0.0f;
        const float c = coverage_[(size_t) i] + whole;
        coverage_[(size_t) i] = 0.0f;
        row_[(size_t) i] = c <= 0.0f ? (uint8_t) 0 : (uint8_t) ((c > 1.0f ? 1.0f : c) * 255.0f + 0.5f);
      }
      // Then painted: in a flat colour, the row at once (detail::blendCoverage
      // -- a run of whole pixels blended as one, stored when the colour is
      // opaque, and an edge pixel in place; the region is inside the bitmap),
      // and through `paint` a pixel at a time otherwise.
      if (touchedFrom_ >= touchedTo_) continue;
      PremulColour rowSrc = src;
      bool flatRow = solid;
      if (solidRows) {
        rowSrc = rowGradientColour(py);
        if (rowSrc.a == 0) continue; // a transparent row of the ramp paints nothing
        flatRow = true;
      }
      if (flatRow) {
        detail::blendCoverage(pixels + target.offsetOf(region.x + touchedFrom_, py), row_.data() + touchedFrom_,
                              touchedTo_ - touchedFrom_, rowSrc);
        continue;
      }
      for (int i = touchedFrom_; i < touchedTo_; ++i) {
        const uint8_t cov = row_[(size_t) i];
        if (cov != 0) paint(region.x + i, py, cov);
      }
    }
  }

  /**
   * Coverage worked out elsewhere -- `width` by `height` of it, its top-left
   * at (left, top) in the target's pixels -- painted as fill paints the
   * coverage it rasterises: through the clip and the mask, with the colour,
   * the gradient or the tile. What text drawn from cached glyph coverage
   * comes through.
   *
   * The per-pixel rule is fill's, written out again rather than shared:
   * fill's own lambda, moved into a function both call, cost fill a third
   * of its speed on an editor's knobs and panels -- the compiler kept
   * reloading the paint state through a pointer the pixel stores might
   * alias. Change one, change both.
   */
  void paintCoverage(Bitmap& target, const uint8_t* coverage, int width, int height, int left, int top,
                     Colour colour, PixelRect clip = {-1, -1, -1, -1}, const ColourGradient* gradient = nullptr,
                     const TiledImage* tile = nullptr, const detail::CoverageMask* mask = nullptr) {
    if (target.isEmpty() || coverage == nullptr || width <= 0 || height <= 0) return;
    if (!gradient && !tile && colour.isTransparent()) return;
    if (gradient && gradient->isEmpty()) return;
    if (tile && tile->isEmpty()) return;
    const PixelRect region = (clip.w < 0) ? target.bounds() : clip.intersection(target.bounds());
    const PixelRect area = PixelRect{left, top, width, height}.intersection(region);
    if (area.isEmpty()) return;
    const PremulColour src = PremulColour::from(colour);
    uint8_t* const pixels = target.data();
    auto paint = [&](int x, int y, uint8_t cov) {
      if (mask) {
        cov = detail::multiplyCoverage(cov, mask->at(x, y));
        if (cov == 0) return;
      }
      if (tile) {
        const PremulColour here = tile->sample((float) x + 0.5f, (float) y + 0.5f);
        if (here.a == 0) return;
        target.blend(x, y, here, cov);
        return;
      }
      if (!gradient) {
        if (cov == 255 && src.a == 255) {
          uint8_t* p = pixels + target.offsetOf(x, y);
          p[0] = src.r;
          p[1] = src.g;
          p[2] = src.b;
          p[3] = src.a;
          return;
        }
        target.blend(x, y, src, cov);
        return;
      }
      const Colour here = gradient->colourAt(Point((float) x + 0.5f, (float) y + 0.5f));
      if (here.isTransparent()) return;
      target.blend(x, y, PremulColour::from(here), cov);
    };
    // A flat colour under no mask -- nearly all text -- is blended in place,
    // row by row, by the same rule: through `paint` it was a call to
    // Bitmap::blend and its bounds test for every pixel a glyph touches, and
    // that was most of the cost of drawing a page of code. The area is
    // inside the bitmap.
    if (!mask && !tile && !gradient) {
      for (int y = area.y; y < area.bottom(); ++y)
        detail::blendCoverage(pixels + target.offsetOf(area.x, y),
                              coverage + (size_t) (y - top) * (size_t) width + (size_t) (area.x - left), area.w, src);
      return;
    }
    for (int y = area.y; y < area.bottom(); ++y) {
      const uint8_t* row = coverage + (size_t) (y - top) * (size_t) width;
      for (int x = area.x; x < area.right(); ++x) {
        const uint8_t c = row[x - left];
        if (c != 0) paint(x, y, c);
      }
    }
  }

private:
  /** One non-horizontal edge of the flattened path, in device pixels, with
   *  +1 or -1 for the direction it was wound. */
  struct Edge {
    Point a, b;
    int winding;
    float lo, hi; // its extent in y, top and bottom
  };
  /** Where one edge crosses the current sub-row: the active list is these,
   *  kept in x order, and the spans are filled between them. */
  struct Crossing {
    float x;
    int winding;
    size_t edge; // the edge in edges_ it is the crossing of
  };

  /**
   * Get a fill's gradient ready; what ColourGradient::varyingAxis says.
   *
   * A linear gradient along an axis -- a panel's top-to-bottom shading, a
   * colour square's left-to-right ramp -- has one colour per row, or per
   * column. Each is worked out once, by the same colourAt at the same pixel
   * centre the per-pixel rule asks, rather than once per pixel: the stops'
   * search and the mix were most of a gradient fill. A left-to-right ramp's
   * colours are worked out here, for the columns the shape can reach (its
   * edges' extent, out to the region's side where an edge stands beside it);
   * a top-to-bottom one's as each row comes, and the fill then blends a
   * row's runs in its colour as it does a flat colour's.
   *
   * Members and functions of their own rather than state in fill(): fill's
   * per-pixel closure captured them, and the flat fills that never use them
   * got slower for it.
   */
  int prepareGradient(const ColourGradient& gradient, const PixelRect& region) {
    gradient_ = &gradient;
    gradientAxis_ = gradient.varyingAxis();
    rowY_ = region.y - 1;
    regionX_ = region.x;
    columnsFrom_ = columnsTo_ = 0;
    if (gradientAxis_ != 1) return gradientAxis_;
    float lo = 1e30f, hi = -1e30f;
    for (const Edge& e : edges_) {
      lo = (std::min)(lo, (std::min)(e.a.x, e.b.x));
      hi = (std::max)(hi, (std::max)(e.a.x, e.b.x));
    }
    bool sideLeft = false, sideRight = false;
    for (const SideEdge& e : sideEdges_) (e.left ? sideLeft : sideRight) = true;
    // Clamped in float before the conversion, like every coordinate here.
    columnsFrom_ = region.x;
    columnsTo_ = region.right();
    if (!edges_.empty() && !sideLeft && lo > (float) region.x)
      columnsFrom_ = (int) std::floor((std::min)(lo, (float) region.right()));
    if (!edges_.empty() && !sideRight && hi < (float) region.right())
      columnsTo_ = (int) std::ceil((std::max)(hi, (float) columnsFrom_)) + 1;
    columnsTo_ = (std::min)(columnsTo_, region.right());
    if (columnsTo_ < columnsFrom_) columnsTo_ = columnsFrom_;
    gradientColours_.resize((size_t) (columnsTo_ - columnsFrom_));
    for (int x = columnsFrom_; x < columnsTo_; ++x)
      gradientColours_[(size_t) (x - columnsFrom_)] =
          PremulColour::from(gradient.colourAt(Point((float) x + 0.5f, (float) region.y + 0.5f)));
    return gradientAxis_;
  }

  /** A top-to-bottom ramp's colour on row `y`, worked out once per row. */
  PremulColour rowGradientColour(int y) {
    if (y != rowY_) {
      rowY_ = y;
      rowColour_ = PremulColour::from(gradient_->colourAt(Point((float) regionX_ + 0.5f, (float) y + 0.5f)));
    }
    return rowColour_;
  }

  /** The gradient's colour at the centre of pixel (x, y), premultiplied. */
  PremulColour gradientColour(int x, int y) {
    if (gradientAxis_ == 2) return rowGradientColour(y);
    if (gradientAxis_ == 1 && x >= columnsFrom_ && x < columnsTo_)
      return gradientColours_[(size_t) (x - columnsFrom_)];
    return PremulColour::from(gradient_->colourAt(Point((float) x + 0.5f, (float) y + 0.5f)));
  }

  /** Whether the edges are one rectangle with its sides on whole pixels: two
   *  vertical edges (or edges beside the region, which stand at its sides)
   *  at whole x, over the same span of y, wound opposite ways -- inside under
   *  either rule. If so, the columns it covers within the region, and its
   *  top and bottom, which need not be whole. */
  bool verticalBox(const PixelRect& region, int& left, int& right, float& top, float& bottom) const {
    if (edges_.size() + sideEdges_.size() != 2) return false;
    float x[2], lo[2], hi[2];
    int winding[2], k = 0;
    for (const Edge& e : edges_) {
      if (e.a.x != e.b.x) return false;
      x[k] = e.a.x;
      lo[k] = e.lo;
      hi[k] = e.hi;
      winding[k++] = e.winding;
    }
    for (const SideEdge& e : sideEdges_) {
      x[k] = e.left ? (float) region.x : (float) region.right();
      lo[k] = e.lo;
      hi[k] = e.hi;
      winding[k++] = e.winding;
    }
    auto whole = [](float v) { return std::floor(v) == v && std::fabs(v) < 16777216.0f; };
    if (lo[0] != lo[1] || hi[0] != hi[1] || winding[0] != -winding[1] || x[0] == x[1]) return false;
    if (!whole(x[0]) || !whole(x[1])) return false;
    left = (std::max)((int) (std::min)(x[0], x[1]), region.x);
    right = (std::min)((int) (std::max)(x[0], x[1]), region.right());
    top = lo[0];
    bottom = hi[0];
    return left < right;
  }

  /** Into x order, from the order the last sub-row left. Insertion sort is
   *  linear on a list that is almost sorted; a list that is not -- a scribble
   *  of long edges crossing each other at once -- would make it quadratic,
   *  so past a few moves per entry it hands over to a full sort. */
  void sortCrossings() {
    const size_t n = crossings_.size();
    size_t moves = 0;
    for (size_t i = 1; i < n; ++i) {
      const Crossing c = crossings_[i];
      size_t j = i;
      while (j > 0 && c.x < crossings_[j - 1].x) {
        crossings_[j] = crossings_[j - 1];
        --j;
      }
      crossings_[j] = c;
      moves += i - j;
      if (moves > 4 * n) {
        std::sort(crossings_.begin(), crossings_.end(),
                  [](const Crossing& l, const Crossing& r) { return l.x < r.x; });
        return;
      }
    }
  }

  /** One horizontal span's EXACT contribution. The partial pixels at each end
   *  are what makes a near-vertical edge smooth; rounding them to whole
   *  pixels is the difference between an antialiased shape and a jagged one. */
  void addSpan(float x0, float x1, const PixelRect& region, float weight) {
    // Written as !(x1 > x0), not (x1 <= x0): a NaN fails BOTH orderings, and
    // the first spelling let it through to the int conversions below.
    if (!(x1 > x0)) return;
    x0 = (std::max)(x0, (float) region.x);
    x1 = (std::min)(x1, (float) region.right());
    if (!(x1 > x0)) return;

    const int first = (int) std::floor(x0);
    const int last = (int) std::ceil(x1) - 1;
    touchedFrom_ = (std::min)(touchedFrom_, first - region.x);
    touchedTo_ = (std::max)(touchedTo_, last - region.x + 1);
    if (first == last) {
      coverage_[(size_t) (first - region.x)] += (x1 - x0) * weight;
      return;
    }
    coverage_[(size_t) (first - region.x)] += ((float) (first + 1) - x0) * weight;
    // The pixels between, wholly covered, as a step up where they begin and
    // down where they end, summed once a row: an 800-pixel slider track was
    // 800 additions in each of sixteen sub-rows of each of its rows.
    if (last > first + 1) {
      steps_[(size_t) (first + 1 - region.x)] += weight;
      steps_[(size_t) (last - region.x)] -= weight;
    }
    coverage_[(size_t) (last - region.x)] += (x1 - (float) last) * weight;
  }

  std::vector<Contour> contours_;
  std::vector<Edge> edges_;
  /** An edge wholly left or right of the region, kept as its extent in y. */
  struct SideEdge {
    float lo, hi;
    int winding;
    bool left;
  };
  /** Per sub-row: the winding and number of crossings left of the region,
   *  and the number right of it -- stored as changes, summed down the rows. */
  struct SideCount {
    int leftWinding = 0, left = 0, right = 0;
  };
  std::vector<SideEdge> sideEdges_;
  std::vector<SideCount> sideCounts_;
  std::vector<float> subRowY_; // every sub-row's sample, top to bottom
  /** The ACTIVE list: one per edge the current sub-row crosses, in x order. */
  std::vector<Crossing> crossings_;
  std::vector<float> coverage_;
  /** The whole pixels the row's spans cover, as a step at each end. */
  std::vector<float> steps_;
  /** The row's coverage, 0 to 255, worked out before it is painted. */
  std::vector<uint8_t> row_;
  /** The gradient being filled, and what prepareGradient worked out for it:
   *  a left-to-right ramp's colour in each column the fill can reach, a
   *  top-to-bottom one's on the last row asked about. */
  const ColourGradient* gradient_ = nullptr;
  int gradientAxis_ = 0, columnsFrom_ = 0, columnsTo_ = 0, rowY_ = 0, regionX_ = 0;
  PremulColour rowColour_;
  std::vector<PremulColour> gradientColours_;
  /** The part of the coverage row this pixel row's spans touched. */
  int touchedFrom_ = 0, touchedTo_ = 0;
};

} // namespace gfx
} // namespace sonore
