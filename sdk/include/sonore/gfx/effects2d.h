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
  Colour colour{0, 0, 0, 140};  ///< the shadow's colour; black at 55 % by default
  int radius = 6;  ///< the blur's radius, in the path's units; under 1 draws nothing
  Point offset{0.0f, 3.0f};  ///< how far the shadow sits from the shape, in the path's units

  /** A soft black shadow 3 units below the shape, blurred by 6. */
  DropShadow() = default;
  /** A shadow of colour `c`, blur radius `r`, moved by `o`. */
  DropShadow(Colour c, int r, Point o) : colour(c), radius(r), offset(o) {}

  /** Draw the shadow of `path` -- the shape itself is NOT drawn, so a caller
   *  fills it afterwards in whatever colour it likes. A shadow wider or
   *  taller than 4096 units is not drawn at all. */
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

  Fit fit = Fit::Contain;  ///< how the size is chosen
  /** -1 left/top, 0 centre, 1 right/bottom. */
  float alignX = 0.0f;
  float alignY = 0.0f;  ///< the same, vertically

  /** Where `source` (a size) ends up inside `destination`. */
  Rect apply(const Rect& source, const Rect& destination) const {
    if (source.w <= 0.0f || source.h <= 0.0f || destination.isEmpty()) return destination;

    float w = source.w, h = source.h;
    switch (fit) {
      case Fit::Contain: {
        const float scale = (std::min)(destination.w / source.w, destination.h / source.h);
        w = source.w * scale;
        h = source.h * scale;
        break;
      }
      case Fit::Cover: {
        const float scale = (std::max)(destination.w / source.w, destination.h / source.h);
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
 * All 148 CSS names, and transparent. Thirteen were once hand-rolled inside the
 * SVG parser; this is the one table, and the parser asks it.
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
  // A hand-written SVG says "steelblue" as readily as "#4682b4", and with
  // twenty-four names here every other one was drawn black.
  static const Named kTable[] = {
      {"transparent", 0x000000, 0}, {"aliceblue", 0xf0f8ff, 255}, {"antiquewhite", 0xfaebd7, 255},
      {"aqua", 0x00ffff, 255}, {"aquamarine", 0x7fffd4, 255}, {"azure", 0xf0ffff, 255},
      {"beige", 0xf5f5dc, 255}, {"bisque", 0xffe4c4, 255}, {"black", 0x000000, 255},
      {"blanchedalmond", 0xffebcd, 255}, {"blue", 0x0000ff, 255}, {"blueviolet", 0x8a2be2, 255},
      {"brown", 0xa52a2a, 255}, {"burlywood", 0xdeb887, 255}, {"cadetblue", 0x5f9ea0, 255},
      {"chartreuse", 0x7fff00, 255}, {"chocolate", 0xd2691e, 255}, {"coral", 0xff7f50, 255},
      {"cornflowerblue", 0x6495ed, 255}, {"cornsilk", 0xfff8dc, 255}, {"crimson", 0xdc143c, 255},
      {"cyan", 0x00ffff, 255}, {"darkblue", 0x00008b, 255}, {"darkcyan", 0x008b8b, 255},
      {"darkgoldenrod", 0xb8860b, 255}, {"darkgray", 0xa9a9a9, 255}, {"darkgreen", 0x006400, 255},
      {"darkgrey", 0xa9a9a9, 255}, {"darkkhaki", 0xbdb76b, 255}, {"darkmagenta", 0x8b008b, 255},
      {"darkolivegreen", 0x556b2f, 255}, {"darkorange", 0xff8c00, 255},
      {"darkorchid", 0x9932cc, 255}, {"darkred", 0x8b0000, 255}, {"darksalmon", 0xe9967a, 255},
      {"darkseagreen", 0x8fbc8f, 255}, {"darkslateblue", 0x483d8b, 255},
      {"darkslategray", 0x2f4f4f, 255}, {"darkslategrey", 0x2f4f4f, 255},
      {"darkturquoise", 0x00ced1, 255}, {"darkviolet", 0x9400d3, 255},
      {"deeppink", 0xff1493, 255}, {"deepskyblue", 0x00bfff, 255}, {"dimgray", 0x696969, 255},
      {"dimgrey", 0x696969, 255}, {"dodgerblue", 0x1e90ff, 255}, {"firebrick", 0xb22222, 255},
      {"floralwhite", 0xfffaf0, 255}, {"forestgreen", 0x228b22, 255}, {"fuchsia", 0xff00ff, 255},
      {"gainsboro", 0xdcdcdc, 255}, {"ghostwhite", 0xf8f8ff, 255}, {"gold", 0xffd700, 255},
      {"goldenrod", 0xdaa520, 255}, {"gray", 0x808080, 255}, {"grey", 0x808080, 255},
      {"green", 0x008000, 255}, {"greenyellow", 0xadff2f, 255}, {"honeydew", 0xf0fff0, 255},
      {"hotpink", 0xff69b4, 255}, {"indianred", 0xcd5c5c, 255}, {"indigo", 0x4b0082, 255},
      {"ivory", 0xfffff0, 255}, {"khaki", 0xf0e68c, 255}, {"lavender", 0xe6e6fa, 255},
      {"lavenderblush", 0xfff0f5, 255}, {"lawngreen", 0x7cfc00, 255},
      {"lemonchiffon", 0xfffacd, 255}, {"lightblue", 0xadd8e6, 255},
      {"lightcoral", 0xf08080, 255}, {"lightcyan", 0xe0ffff, 255},
      {"lightgoldenrodyellow", 0xfafad2, 255}, {"lightgray", 0xd3d3d3, 255},
      {"lightgreen", 0x90ee90, 255}, {"lightgrey", 0xd3d3d3, 255}, {"lightpink", 0xffb6c1, 255},
      {"lightsalmon", 0xffa07a, 255}, {"lightseagreen", 0x20b2aa, 255},
      {"lightskyblue", 0x87cefa, 255}, {"lightslategray", 0x778899, 255},
      {"lightslategrey", 0x778899, 255}, {"lightsteelblue", 0xb0c4de, 255},
      {"lightyellow", 0xffffe0, 255}, {"lime", 0x00ff00, 255}, {"limegreen", 0x32cd32, 255},
      {"linen", 0xfaf0e6, 255}, {"magenta", 0xff00ff, 255}, {"maroon", 0x800000, 255},
      {"mediumaquamarine", 0x66cdaa, 255}, {"mediumblue", 0x0000cd, 255},
      {"mediumorchid", 0xba55d3, 255}, {"mediumpurple", 0x9370db, 255},
      {"mediumseagreen", 0x3cb371, 255}, {"mediumslateblue", 0x7b68ee, 255},
      {"mediumspringgreen", 0x00fa9a, 255}, {"mediumturquoise", 0x48d1cc, 255},
      {"mediumvioletred", 0xc71585, 255}, {"midnightblue", 0x191970, 255},
      {"mintcream", 0xf5fffa, 255}, {"mistyrose", 0xffe4e1, 255}, {"moccasin", 0xffe4b5, 255},
      {"navajowhite", 0xffdead, 255}, {"navy", 0x000080, 255}, {"oldlace", 0xfdf5e6, 255},
      {"olive", 0x808000, 255}, {"olivedrab", 0x6b8e23, 255}, {"orange", 0xffa500, 255},
      {"orangered", 0xff4500, 255}, {"orchid", 0xda70d6, 255}, {"palegoldenrod", 0xeee8aa, 255},
      {"palegreen", 0x98fb98, 255}, {"paleturquoise", 0xafeeee, 255},
      {"palevioletred", 0xdb7093, 255}, {"papayawhip", 0xffefd5, 255},
      {"peachpuff", 0xffdab9, 255}, {"peru", 0xcd853f, 255}, {"pink", 0xffc0cb, 255},
      {"plum", 0xdda0dd, 255}, {"powderblue", 0xb0e0e6, 255}, {"purple", 0x800080, 255},
      {"rebeccapurple", 0x663399, 255}, {"red", 0xff0000, 255}, {"rosybrown", 0xbc8f8f, 255},
      {"royalblue", 0x4169e1, 255}, {"saddlebrown", 0x8b4513, 255}, {"salmon", 0xfa8072, 255},
      {"sandybrown", 0xf4a460, 255}, {"seagreen", 0x2e8b57, 255}, {"seashell", 0xfff5ee, 255},
      {"sienna", 0xa0522d, 255}, {"silver", 0xc0c0c0, 255}, {"skyblue", 0x87ceeb, 255},
      {"slateblue", 0x6a5acd, 255}, {"slategray", 0x708090, 255}, {"slategrey", 0x708090, 255},
      {"snow", 0xfffafa, 255}, {"springgreen", 0x00ff7f, 255}, {"steelblue", 0x4682b4, 255},
      {"tan", 0xd2b48c, 255}, {"teal", 0x008080, 255}, {"thistle", 0xd8bfd8, 255},
      {"tomato", 0xff6347, 255}, {"turquoise", 0x40e0d0, 255}, {"violet", 0xee82ee, 255},
      {"wheat", 0xf5deb3, 255}, {"white", 0xffffff, 255}, {"whitesmoke", 0xf5f5f5, 255},
      {"yellow", 0xffff00, 255}, {"yellowgreen", 0x9acd32, 255},
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
