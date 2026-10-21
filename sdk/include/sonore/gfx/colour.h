// SPDX-License-Identifier: Apache-2.0
//
// Colour, and blending.
//
// ── Premultiplied, and why that decision is load-bearing ────────────────────
//
// A colour is stored as four separate 8-bit channels, but everything the
// rasteriser composites is PREMULTIPLIED: the colour channels already carry
// their alpha. That is not an optimisation, it is the only way the arithmetic
// stays correct.
//
// Compositing two half-transparent layers with straight alpha requires a
// divide per pixel to recover the colour, and where alpha is zero there is
// nothing to recover -- so a fully transparent pixel has no defined colour
// and blending through it drags whatever garbage was in the channels into
// the result. Premultiplied has no such case: zero alpha means zero
// everywhere, and `over` is one multiply and one add per channel.
//
// So Colour is the type a caller writes, and PremulColour is the type the
// rasteriser blends. The conversion is explicit and one-directional at the
// point of use, because a premultiplied value that gets treated as straight
// is a picture that looks washed out in a way nobody can locate.
#pragma once

#include <cmath>
#include <cstdint>

namespace sonore {
namespace gfx {

/** Straight (non-premultiplied) 8-bit RGBA: what a caller writes. */
struct Colour {
  uint8_t r = 0, /**< red, 0..255 */ g = 0, /**< green */ b = 0, /**< blue */ a = 255; ///< 0 transparent, 255 opaque

  /** Opaque black. */
  Colour() = default;
  /** Straight channels, 0..255 each; opaque unless `ca` says otherwise. */
  Colour(uint8_t cr, uint8_t cg, uint8_t cb, uint8_t ca = 255) : r(cr), g(cg), b(cb), a(ca) {}

  /** 0xAARRGGBB, the order these are written in every design tool. */
  static Colour fromARGB(uint32_t argb) {
    return {(uint8_t) ((argb >> 16) & 0xFF), (uint8_t) ((argb >> 8) & 0xFF),
            (uint8_t) (argb & 0xFF), (uint8_t) ((argb >> 24) & 0xFF)};
  }

  /** 0xRRGGBB, opaque. The spelling a stylesheet uses. */
  static Colour fromRGB(uint32_t rgb) { return fromARGB(0xFF000000u | rgb); }

  /** 0xAARRGGBB: the inverse of fromARGB. */
  uint32_t toARGB() const {
    return ((uint32_t) a << 24) | ((uint32_t) r << 16) | ((uint32_t) g << 8) | (uint32_t) b;
  }

  /** Alpha 0: nothing shows. */
  bool isTransparent() const { return a == 0; }
  /** Alpha 255: nothing shows through. */
  bool isOpaque() const { return a == 255; }

  /** The same colour with its alpha replaced by `newAlpha`. */
  Colour withAlpha(uint8_t newAlpha) const { return {r, g, b, newAlpha}; }

  /** Scale the existing alpha rather than replace it, for fading something
   *  that is already partly transparent. */
  Colour withMultipliedAlpha(float factor) const {
    // Written so a NaN -- a fade computed from 0/0 -- counts as zero: it
    // failed both tests of the old spelling and reached the conversion to
    // uint8_t, which for a NaN is undefined. The same in the two below.
    const float f = factor > 0.0f ? (factor < 1.0f ? factor : 1.0f) : 0.0f;
    return {r, g, b, (uint8_t) ((float) a * f + 0.5f)};
  }

  /**
   * Toward another colour, `amount` of the way.
   *
   * Interpolates the STRAIGHT channels, which is what a caller drawing a
   * gradient between two solid colours expects. Interpolating premultiplied
   * channels instead makes a fade to transparent pass through black, which is
   * the grey halo seen around badly composited artwork.
   */
  Colour interpolatedTo(Colour other, float amount) const {
    const float t = amount > 0.0f ? (amount < 1.0f ? amount : 1.0f) : 0.0f;
    auto mix = [t](uint8_t x, uint8_t y) {
      return (uint8_t) ((float) x + ((float) y - (float) x) * t + 0.5f);
    };
    return {mix(r, other.r), mix(g, other.g), mix(b, other.b), mix(a, other.a)};
  }

  /** Lighter or darker, keeping the hue. Negative darkens. What a
   *  LookAndFeel wants for a hover or pressed state without a second colour
   *  in the palette. */
  Colour brightened(float amount) const {
    if (std::isnan(amount)) amount = 0.0f;
    auto shift = [amount](uint8_t v) {
      const float f = (float) v + 255.0f * amount;
      return (uint8_t) (f < 0.0f ? 0.0f : (f > 255.0f ? 255.0f : f));
    };
    return {shift(r), shift(g), shift(b), a};
  }

  /** brightened(-amount), spelled so a caller reading a gradient can see which
   *  end is which without decoding a minus sign. */
  Colour darkened(float amount) const { return brightened(-amount); }

  /**
   * Perceived brightness, 0..1.
   *
   * Rec. 601 weights: green counts for nearly three quarters of what the eye
   * reads as brightness and blue for barely a fourteenth. A plain average
   * calls pure blue and pure green equally bright, and a LookAndFeel picking
   * text colour from that puts black text on navy.
   */
  float brightness() const {
    return (0.299f * (float) r + 0.587f * (float) g + 0.114f * (float) b) / 255.0f;
  }

  /** Black or white, whichever can be read on this. */
  Colour contrasting() const {
    return brightness() > 0.5f ? Colour(0, 0, 0) : Colour(255, 255, 255);
  }

  /** All four channels equal. */
  bool operator==(const Colour& o) const {
    return r == o.r && g == o.g && b == o.b && a == o.a;
  }
  /** Any channel differs. */
  bool operator!=(const Colour& o) const { return !(*this == o); }
};

/** Premultiplied 8-bit RGBA: what the rasteriser blends. */
struct PremulColour {
  uint8_t r = 0, /**< red times alpha */ g = 0, /**< green times alpha */ b = 0, /**< blue times alpha */ a = 0; ///< alpha

  /** Fully transparent: every channel 0. */
  PremulColour() = default;
  /** Channels as given, already premultiplied: no colour channel should
   *  exceed `pa`. */
  PremulColour(uint8_t pr, uint8_t pg, uint8_t pb, uint8_t pa) : r(pr), g(pg), b(pb), a(pa) {}

  /** `c` with its colour channels multiplied by its alpha, rounded to nearest. */
  static PremulColour from(Colour c) {
    // +127 before the divide is round-to-nearest rather than truncation.
    // Truncating loses up to one level per channel per composite, and a
    // gradient drawn as a hundred stacked blends visibly banded.
    auto mul = [](uint8_t v, uint8_t alpha) {
      const uint32_t x = (uint32_t) v * (uint32_t) alpha + 127u;
      return (uint8_t) ((x + (x >> 8)) >> 8);
    };
    return {mul(c.r, c.a), mul(c.g, c.a), mul(c.b, c.a), c.a};
  }

  /** Back to straight alpha, for reading a rendered buffer. Undefined where
   *  alpha is zero, so that case returns fully transparent rather than a
   *  division by zero. */
  Colour toStraight() const {
    if (a == 0) return {0, 0, 0, 0};
    auto div = [this](uint8_t v) {
      const uint32_t x = ((uint32_t) v * 255u + (uint32_t) a / 2u) / (uint32_t) a;
      return (uint8_t) (x > 255u ? 255u : x);
    };
    return {div(r), div(g), div(b), a};
  }
};

/**
 * `src` over `dst`, both premultiplied.
 *
 * The whole compositing model in four multiplies. `coverage` is the
 * rasteriser's antialiasing, 0..255, applied to the source before blending --
 * so a shape covering a third of a pixel contributes a third of its colour
 * AND a third of its alpha, which is what makes an edge look smooth instead
 * of merely translucent.
 */
inline PremulColour over(PremulColour src, PremulColour dst, uint8_t coverage = 255) {
  if (coverage == 0) return dst;
  auto scale = [](uint8_t v, uint32_t by) {
    const uint32_t x = (uint32_t) v * by + 127u;
    return (uint8_t) ((x + (x >> 8)) >> 8);
  };
  if (coverage != 255) {
    src = {scale(src.r, coverage), scale(src.g, coverage), scale(src.b, coverage),
           scale(src.a, coverage)};
  }
  const uint32_t inv = 255u - (uint32_t) src.a;
  return {(uint8_t) (src.r + scale(dst.r, inv)), (uint8_t) (src.g + scale(dst.g, inv)),
          (uint8_t) (src.b + scale(dst.b, inv)), (uint8_t) (src.a + scale(dst.a, inv))};
}

/** A few colours the default LookAndFeel is built from, so a plugin that
 *  names none of its own still looks deliberate rather than grey. */
namespace palette {
/** 0x0D1014: the window's ground. */
inline Colour background() { return Colour::fromRGB(0x0D1014); }
/** 0x131922: a control's body. */
inline Colour panel() { return Colour::fromRGB(0x131922); }
/** 0x26313F: edges and tracks. */
inline Colour outline() { return Colour::fromRGB(0x26313F); }
/** 0xDCE5EE: text. */
inline Colour text() { return Colour::fromRGB(0xDCE5EE); }
/** 0x7D8B9C: secondary and disabled text. */
inline Colour dimText() { return Colour::fromRGB(0x7D8B9C); }
/** 0x4EA3FF: a control's value, focus, selection. */
inline Colour accent() { return Colour::fromRGB(0x4EA3FF); }
/** 0xE5A13A: something destructive, such as a close button being pressed. */
inline Colour warning() { return Colour::fromRGB(0xE5A13A); }
} // namespace palette

} // namespace gfx
} // namespace sonore
