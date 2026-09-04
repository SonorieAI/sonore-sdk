// SPDX-License-Identifier: Apache-2.0
//
// Blur, drop shadows, rectangle fitting, and the named colours.
//
// ── Why these are together ──────────────────────────────────────────────────
//
// A drop shadow IS a blurred silhouette, so the blur has to exist first. And
// the other two came out of the same audit for the same reason: both were
// hand-rolled inside something that needed them -- aspect-preserving fit inside
// Drawable::draw, thirteen colour names inside the SVG parser -- which is
// always the sign that a thing belongs somewhere shared.
#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "bitmap.h"
#include "colour.h"
#include "geometry.h"
#include "graphics.h"

namespace sonore {
namespace gfx {

/**
 * A box blur, run three times, which is a very good Gaussian.
 *
 * A true Gaussian kernel of radius r costs 2r multiplies per pixel per axis;
 * this costs one add and one subtract per pixel per pass REGARDLESS of radius,
 * because each step only adds the pixel entering the window and subtracts the
 * one leaving. That is what makes a shadow affordable: a radius-16 blur on a
 * 600x400 editor is fifteen million multiply-accumulates the honest way, and
 * under two million adds this way.
 */
class Blur {
public:
  /**
   * In place, on premultiplied RGBA.
   *
   * Premultiplied is what makes this correct: blurring straight alpha bleeds
   * the colour of fully transparent pixels into their neighbours, which for a
   * shadow means a grey halo around it.
   */
  static void apply(Bitmap& bitmap, int radius) {
    if (radius < 1 || bitmap.isEmpty()) return;
    const int w = bitmap.width(), h = bitmap.height();
    const size_t count = (size_t) w * (size_t) h * 4u;

    // ── Why the passes run in fixed point ─────────────────────────────────
    //
    // The first version of this divided 8-bit values by the window size at
    // every pass, in integers. That annihilates the signal: one pixel of alpha
    // 255 blurred at radius 6 is 255/13 = 19, then 19/13 = 1, then 1/13 = 0 --
    // the whole shadow gone, and at radius 3 it kept 41 of 255. The tests
    // caught both, because they asked whether ENERGY was conserved rather than
    // whether the picture looked softer.
    //
    // So the six passes run on 24.8 fixed point in a 32-bit accumulator, and
    // the only rounding is the one at the end. The window sum peaks at about
    // 255 * 256 * 33, which is 2.1 million and nowhere near overflowing.
    std::vector<uint32_t> a(count), b(count);
    for (size_t i = 0; i < count; ++i) a[i] = (uint32_t) bitmap.data()[i] << 8;

    for (int pass = 0; pass < 3; ++pass) {
      blurHorizontal(a.data(), b.data(), w, h, radius);
      blurVertical(b.data(), a.data(), w, h, radius);
    }

    uint8_t* out = bitmap.data();
    for (size_t i = 0; i < count; ++i) {
      const uint32_t v = (a[i] + 128u) >> 8; // round, not truncate
      out[i] = (uint8_t) (v > 255u ? 255u : v);
    }
  }

private:
  /** A running sum over a sliding window: one add and one subtract per pixel,
   *  whatever the radius. Three of these converge on a Gaussian to within about
   *  three per cent, which is the central limit theorem doing the work a
   *  2r-multiply kernel would otherwise have to. */
  static void blurHorizontal(const uint32_t* src, uint32_t* dst, int w, int h, int radius) {
    const uint32_t window = (uint32_t) (radius * 2 + 1);
    const uint32_t half = window / 2;
    for (int y = 0; y < h; ++y) {
      const uint32_t* row = src + (size_t) y * (size_t) w * 4u;
      uint32_t* out = dst + (size_t) y * (size_t) w * 4u;
      uint32_t sum[4] = {0, 0, 0, 0};

      // The edges are CLAMPED rather than treated as transparent, so a blur of
      // something reaching the edge does not fade out along it.
      for (int i = -radius; i <= radius; ++i) {
        const int at = clampIndex(i, w);
        for (int c = 0; c < 4; ++c) sum[c] += row[(size_t) at * 4u + (size_t) c];
      }
      for (int x = 0; x < w; ++x) {
        for (int c = 0; c < 4; ++c) out[(size_t) x * 4u + (size_t) c] = (sum[c] + half) / window;
        const int leaving = clampIndex(x - radius, w);
        const int entering = clampIndex(x + radius + 1, w);
        for (int c = 0; c < 4; ++c) {
          sum[c] += row[(size_t) entering * 4u + (size_t) c];
          sum[c] -= row[(size_t) leaving * 4u + (size_t) c];
        }
      }
    }
  }

  static void blurVertical(const uint32_t* src, uint32_t* dst, int w, int h, int radius) {
    const uint32_t window = (uint32_t) (radius * 2 + 1);
    const uint32_t half = window / 2;
    const size_t stride = (size_t) w * 4u;
    for (int x = 0; x < w; ++x) {
      uint32_t sum[4] = {0, 0, 0, 0};
      for (int i = -radius; i <= radius; ++i) {
        const int at = clampIndex(i, h);
        for (int c = 0; c < 4; ++c)
          sum[c] += src[(size_t) at * stride + (size_t) x * 4u + (size_t) c];
      }
      for (int y = 0; y < h; ++y) {
        for (int c = 0; c < 4; ++c)
          dst[(size_t) y * stride + (size_t) x * 4u + (size_t) c] = (sum[c] + half) / window;
        const int leaving = clampIndex(y - radius, h);
        const int entering = clampIndex(y + radius + 1, h);
        for (int c = 0; c < 4; ++c) {
          sum[c] += src[(size_t) entering * stride + (size_t) x * 4u + (size_t) c];
          sum[c] -= src[(size_t) leaving * stride + (size_t) x * 4u + (size_t) c];
        }
      }
    }
  }

  static int clampIndex(int i, int limit) { return i < 0 ? 0 : (i >= limit ? limit - 1 : i); }
};

/**
 * A shadow under a shape.
 *
 * Drawn by rasterising the shape into a scratch bitmap in the shadow's colour,
 * blurring that, and compositing it -- which is what a shadow IS, and is why
 * this needs the blur above rather than a clever approximation.
 *
 * The scratch is only as big as the shape plus the blur's reach, so a small
 * shadow on a large editor costs a small bitmap.
 */
struct DropShadow {
  Colour colour{0, 0, 0, 140};
  int radius = 6;
  Point offset{0.0f, 3.0f};

  DropShadow() = default;
  DropShadow(Colour c, int r, Point o) : colour(c), radius(r), offset(o) {}

  /** Draw the shadow of `path` -- the shape itself is NOT drawn, so a caller
   *  fills it afterwards in whatever colour it likes. */
  void drawForPath(Graphics& g, const Path& path) const {
    if (radius < 1 || colour.a == 0) return;

    // The area the shadow can reach: the path's own extent, moved by the
    // offset, grown by the blur's radius. Three box passes of radius r reach
    // about 1.5r, so the margin is generous rather than exact -- a shadow
    // clipped at its own edge shows a hard line where it should fade out.
    Rect extent = boundsOfPath(path);
    if (extent.w <= 0.0f || extent.h <= 0.0f) return;
    const float margin = (float) radius * 3.0f + 2.0f;
    extent = Rect(extent.x + offset.x - margin, extent.y + offset.y - margin,
                  extent.w + margin * 2.0f, extent.h + margin * 2.0f);

    const PixelRect area = PixelRect::enclosing(extent);
    if (area.w <= 0 || area.h <= 0 || area.w > 4096 || area.h > 4096) return;

    Bitmap scratch(area.w, area.h);
    scratch.clear(Colour(0, 0, 0, 0));
    {
      Graphics sg(scratch);
      sg.setColour(colour);
      // Into the scratch's own coordinates, which is the path moved by the
      // offset and by where the scratch sits.
      sg.addTransform(Transform::translation(offset.x - (float) area.x,
                                             offset.y - (float) area.y));
      sg.fillPath(path);
    }
    Blur::apply(scratch, radius);
    g.drawImageAt(scratch, {(float) area.x, (float) area.y});
  }

  /** The common case: a shadow under a rounded rectangle. */
  void drawForRoundedRect(Graphics& g, const Rect& r, float cornerRadius) const {
    Path path;
    path.addRoundedRect(r, cornerRadius);
    drawForPath(g, path);
  }

private:
  static Rect boundsOfPath(const Path& path) {
    if (path.points().empty()) return {};
    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    for (const Point& p : path.points()) {
      if (p.x < x0) x0 = p.x;
      if (p.y < y0) y0 = p.y;
      if (p.x > x1) x1 = p.x;
      if (p.y > y1) y1 = p.y;
    }
    return Rect(x0, y0, x1 - x0, y1 - y0);
  }
};

/**
 * How one rectangle is placed inside another.
 *
 * Hand-rolled inside Drawable::draw before this existed, and WaveformView and
 * drawImage want the same rules -- which is what a shared thing is for.
 */
struct RectanglePlacement {
  enum class Fit {
    /** Scaled to fit entirely inside, aspect preserved. Letterboxed. */
    Contain,
    /** Scaled to cover it entirely, aspect preserved. Cropped. */
    Cover,
    /** Stretched. Loses the aspect ratio, and is what a logo must never get. */
    Stretch,
    /** Not scaled at all, just placed. */
    None,
  };

  Fit fit = Fit::Contain;
  /** -1 left/top, 0 centre, 1 right/bottom. */
  float alignX = 0.0f;
  float alignY = 0.0f;

  /** Where `source` (a size) ends up inside `destination`. */
  Rect apply(const Rect& source, const Rect& destination) const {
    if (source.w <= 0.0f || source.h <= 0.0f || destination.isEmpty()) return destination;

    float w = source.w, h = source.h;
    switch (fit) {
      case Fit::Contain: {
        const float scale = std::min(destination.w / source.w, destination.h / source.h);
        w = source.w * scale;
        h = source.h * scale;
        break;
      }
      case Fit::Cover: {
        const float scale = std::max(destination.w / source.w, destination.h / source.h);
        w = source.w * scale;
        h = source.h * scale;
        break;
      }
      case Fit::Stretch:
        return destination;
      case Fit::None:
        break;
    }

    // alignX of -1 puts it at the left, 0 centres, 1 puts it at the right.
    const float freeX = destination.w - w, freeY = destination.h - h;
    const float x = destination.x + freeX * (alignX + 1.0f) * 0.5f;
    const float y = destination.y + freeY * (alignY + 1.0f) * 0.5f;
    return Rect(x, y, w, h);
  }
};

/**
 * The named colours.
 *
 * The set an icon or a stylesheet actually uses, not the 148 CSS names. Thirteen
 * of them were hand-rolled inside the SVG parser; this is the same table, in
 * one place, with a few more that people write.
 */
namespace colours {

/** Returns false for a name that is not in the table, so a caller can tell "not
 *  a name" from "the name for black". */
inline bool byName(const std::string& name, Colour* out) {
  struct Named {
    const char* name;
    uint32_t rgb;
    uint8_t alpha;
  };
  static const Named kTable[] = {
      {"transparent", 0x000000, 0},   {"black", 0x000000, 255},
      {"white", 0xffffff, 255},       {"red", 0xff0000, 255},
      {"green", 0x008000, 255},       {"lime", 0x00ff00, 255},
      {"blue", 0x0000ff, 255},        {"yellow", 0xffff00, 255},
      {"cyan", 0x00ffff, 255},        {"aqua", 0x00ffff, 255},
      {"magenta", 0xff00ff, 255},     {"fuchsia", 0xff00ff, 255},
      {"grey", 0x808080, 255},        {"gray", 0x808080, 255},
      {"silver", 0xc0c0c0, 255},      {"orange", 0xffa500, 255},
      {"purple", 0x800080, 255},      {"navy", 0x000080, 255},
      {"teal", 0x008080, 255},        {"olive", 0x808000, 255},
      {"maroon", 0x800000, 255},      {"pink", 0xffc0cb, 255},
      {"brown", 0xa52a2a, 255},       {"gold", 0xffd700, 255},
  };
  for (const Named& named : kTable)
    if (name == named.name) {
      *out = Colour::fromRGB(named.rgb);
      out->a = named.alpha;
      return true;
    }
  return false;
}

} // namespace colours

} // namespace gfx
} // namespace sonore
