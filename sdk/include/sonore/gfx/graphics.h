// SPDX-License-Identifier: Apache-2.0
//
// Graphics: the drawing API a component paints through.
//
// ── What it is ──────────────────────────────────────────────────────────────
//
// A cursor over a Bitmap carrying a colour, a transform and a clip, with a
// stack so a component can change any of them and put them back. Everything
// else -- rectangles, ellipses, lines, strokes -- goes through the one
// rasteriser underneath, so there is one antialiasing quality and one place a
// bug can be.
//
// ── The state stack, and why it is not optional ─────────────────────────────
//
// A component paints its children by translating into each one's coordinates
// and clipping to its bounds. Without a stack, every component would have to
// restore what it changed, and the one that forgets corrupts the drawing of
// every sibling after it -- a bug that shows up in a component that is
// itself correct. ScopedState makes it structural: the restore happens when
// the scope ends, including on an early return.
//
// ── Clipping is a rectangle, and only a rectangle ───────────────────────────
//
// A framework clips to arbitrary regions. This clips to one PixelRect, because
// is what a component tree actually needs -- nested rectangles -- and an
// arbitrary clip means the rasteriser needs a coverage mask per pixel. If a
// rounded panel ever needs its children clipped to its corners, that is the
// point to add it, not before.
#pragma once

#include <vector>

#include <cmath>

#include "bitmap.h"
#include "colour.h"
#include "gradient.h"
#include "geometry.h"
#include "path.h"
#include "stroke.h"

namespace sonore {
namespace gfx {

class Graphics {
public:
  explicit Graphics(Bitmap& target) : target_(target) {
    state_.clip = target.bounds();
  }

  // ── State ────────────────────────────────────────────────────────────────

  struct State {
    Colour colour = palette::text();
    /** Set by setGradient, cleared by setColour. A gradient is a KIND of
     *  colour, so the two share a slot the way a brush does -- otherwise every
     *  fill call needs to be told which of the two to use. */
    ColourGradient gradient;
    bool useGradient = false;
    TiledImage tile;
    bool useTile = false;
    Transform transform;
    PixelRect clip;
  };

  const State& state() const { return state_; }

  void setColour(Colour c) {
    state_.colour = c;
    state_.useGradient = false;
    // BOTH cleared. Three ways of deciding a pixel's colour and only two of
    // them turned off is a fill that quietly ignores the colour it was handed
    // -- which is exactly the bug fillAll had with the gradient flag.
    state_.useTile = false;
  }
  Colour colour() const { return state_.colour; }

  /**
   * Fill with a ramp instead of a flat colour, until the next setColour.
   *
   * The gradient's points are in the CURRENT user space -- the same
   * coordinates the path is in -- so a component describing one across its own
   * bounds does not have to know what transform it is being drawn under.
   */
  void setGradient(const ColourGradient& g) {
    state_.gradient = g;
    state_.useGradient = !g.isEmpty();
    state_.useTile = false;
  }

  /**
   * Fill with a bitmap repeated across the plane, until the next setColour.
   *
   * `origin` is in the CURRENT user space and is transformed once, here -- so a
   * texture given an origin at a component's top-left scrolls with the
   * component rather than staying nailed to the window, which is what happens
   * when the origin is taken as a device coordinate.
   *
   * The same reasoning as the gradient next door, and it goes through the same
   * per-pixel hook in the rasteriser, so a tiled fill clips and antialiases
   * identically to every other fill rather than being a special blit with its
   * own edges.
   */
  void setTiledImageFill(const Bitmap& image, Point origin = {0.0f, 0.0f}, float scale = 1.0f,
                         uint8_t opacity = 255) {
    const Point deviceOrigin = state_.transform.apply(origin);
    state_.tile.image = &image;
    state_.tile.originX = deviceOrigin.x;
    state_.tile.originY = deviceOrigin.y;
    // The transform's own scale as well as the caller's: a texture inside a
    // component drawn at 200% should be twice the size, like everything else
    // in it, rather than staying at device resolution and looking half-size.
    state_.tile.scale = scale * state_.transform.scaleFactor();
    state_.tile.opacity = opacity;
    state_.useTile = !state_.tile.isEmpty();
    state_.useGradient = false;
  }

  bool hasTiledImage() const { return state_.useTile; }

  bool hasGradient() const { return state_.useGradient; }

  /** Applied BEFORE whatever is already there, so a child's transform is
   *  relative to its parent's -- which is the only order a component tree
   *  can compose in. */
  void addTransform(const Transform& t) { state_.transform = t.then(state_.transform); }

  void translate(float dx, float dy) { addTransform(Transform::translation(dx, dy)); }

  /** Narrows the clip; it can never widen. A component that could widen its
   *  clip could draw over its parent, which is the one thing a component
   *  tree must not allow. */
  void clipTo(const PixelRect& r) { state_.clip = state_.clip.intersection(r); }

  void clipTo(const Rect& r) {
    clipTo(PixelRect::enclosing(transformedBounds(r)));
  }

  bool isClippedOut() const { return state_.clip.isEmpty(); }

  void save() { stack_.push_back(state_); }

  void restore() {
    if (stack_.empty()) return;
    state_ = stack_.back();
    stack_.pop_back();
  }

  /** Save on construction, restore on destruction -- including on an early
   *  return or an exception. */
  class ScopedState {
  public:
    explicit ScopedState(Graphics& g) : g_(g) { g_.save(); }
    ~ScopedState() { g_.restore(); }
    ScopedState(const ScopedState&) = delete;
    ScopedState& operator=(const ScopedState&) = delete;

  private:
    Graphics& g_;
  };

  // ── Filling ──────────────────────────────────────────────────────────────

  /**
   * Fill the whole clip region.
   *
   * The clip is kept in DEVICE pixels, so this fills it with the transform
   * held at identity -- deliberately, and this is the whole subtlety of the
   * call. It used to build a Rect from the clip and hand it to fillRect, which
   * applies the current transform to it: under a translation the fill landed
   * displaced by exactly that translation, and the clip then hid the half that
   * ran off the end. A child at (10,20) filling itself painted at (20,40) and
   * covered the bottom-right three quarters of itself.
   *
   * Nothing looked broken, because the part it failed to cover showed the
   * colour underneath -- and every LookAndFeel here puts a dark panel on a dark
   * window. It reached GenericEditor, the Viewport and the tab strip, which is
   * to say the default editor of every generated plugin.
   *
   * The gradient still uses state_.transform: gradient points are given in USER
   * space, which is the one thing here that is not a device rectangle.
   */
  void fillAll() {
    if (isClippedOut()) return;
    scratch_.clear();
    scratch_.addRect(Rect((float) state_.clip.x, (float) state_.clip.y,
                          (float) state_.clip.w, (float) state_.clip.h));
    if (state_.useTile) {
      raster_.fill(target_, scratch_, Transform(), state_.colour, FillRule::NonZero, state_.clip,
                   nullptr, &state_.tile);
      return;
    }
    if (!state_.useGradient) {
      raster_.fill(target_, scratch_, Transform(), state_.colour, FillRule::NonZero, state_.clip);
      return;
    }
    const ColourGradient device = state_.gradient.transformed(state_.transform);
    raster_.fill(target_, scratch_, Transform(), state_.colour, FillRule::NonZero, state_.clip,
                 &device);
  }

  void fillAll(Colour c) {
    const Colour was = state_.colour;
    // The gradient flag too, not just the colour. Without it, fillAll(c) with a
    // gradient set paints the gradient and ignores the colour it was handed --
    // silently, because it draws something plausible either way.
    const bool wasGradient = state_.useGradient;
    state_.colour = c;
    state_.useGradient = false;
    fillAll();
    state_.colour = was;
    state_.useGradient = wasGradient;
  }

  void fillRect(const Rect& r) {
    if (r.isEmpty()) return;
    scratch_.clear();
    scratch_.addRect(r);
    fillPath(scratch_);
  }

  void fillRoundedRect(const Rect& r, float radius) {
    scratch_.clear();
    scratch_.addRoundedRect(r, radius);
    fillPath(scratch_);
  }

  void fillEllipse(const Rect& r) {
    scratch_.clear();
    scratch_.addEllipse(r);
    fillPath(scratch_);
  }

  void fillPath(const Path& p, FillRule rule = FillRule::NonZero) {
    if (isClippedOut()) return;
    if (state_.useTile) {
      raster_.fill(target_, p, state_.transform, state_.colour, rule, state_.clip, nullptr,
                   &state_.tile);
      return;
    }
    if (!state_.useGradient) {
      raster_.fill(target_, p, state_.transform, state_.colour, rule, state_.clip);
      return;
    }
    // Moved to device space ONCE per fill. The alternative is inverting the
    // transform for every pixel, which is the same answer at a few hundred
    // times the cost.
    const ColourGradient device = state_.gradient.transformed(state_.transform);
    raster_.fill(target_, p, state_.transform, state_.colour, rule, state_.clip, &device);
  }

  // ── Images ───────────────────────────────────────────────────────────────

  /**
   * Draw a bitmap into `destination`, scaling to fit.
   *
   * Only translation and scale are honoured, not rotation or shear. A rotated
   * image needs a resampling filter that antialiases along the rotated edges,
   * which is a different piece of work from this one -- and a plugin's artwork
   * is a logo or a panel texture, drawn square. A transform with rotation in it
   * draws the image at its axis-aligned bounding box rather than silently
   * skewing it, and this note is here so that is a decision rather than a
   * surprise.
   *
   * The source is PREMULTIPLIED, as everything in this SDK is, so compositing
   * is the same `over` the rasteriser uses and a semi-transparent logo lands at
   * the right brightness.
   */
  void drawImage(const Bitmap& source, const Rect& destination) {
    if (source.isEmpty() || destination.isEmpty() || isClippedOut()) return;

    // Into device space, through the current transform. The corners are enough
    // because only translate and scale are supported.
    const Point topLeft = state_.transform.apply({destination.x, destination.y});
    const Point bottomRight =
        state_.transform.apply({destination.right(), destination.bottom()});
    const float x0 = topLeft.x < bottomRight.x ? topLeft.x : bottomRight.x;
    const float y0 = topLeft.y < bottomRight.y ? topLeft.y : bottomRight.y;
    const float x1 = topLeft.x < bottomRight.x ? bottomRight.x : topLeft.x;
    const float y1 = topLeft.y < bottomRight.y ? bottomRight.y : topLeft.y;
    const float spanX = x1 - x0, spanY = y1 - y0;
    if (spanX <= 0.0f || spanY <= 0.0f) return;

    const PixelRect area = PixelRect::enclosing(Rect(x0, y0, spanX, spanY))
                               .intersection(state_.clip)
                               .intersection(target_.bounds());
    if (area.isEmpty()) return;

    const float scaleX = (float) source.width() / spanX;
    const float scaleY = (float) source.height() / spanY;

    for (int y = area.y; y < area.bottom(); ++y) {
      // Sampled at the pixel CENTRE, like everything else here. A corner sample
      // shifts the whole image half a pixel, which shows as a seam where two
      // images meet and as a soft edge on a logo that should be crisp.
      const float sourceY = ((float) y + 0.5f - y0) * scaleY;
      int sy = (int) sourceY;
      if (sy < 0) sy = 0;
      if (sy >= source.height()) sy = source.height() - 1;

      for (int x = area.x; x < area.right(); ++x) {
        const float sourceX = ((float) x + 0.5f - x0) * scaleX;
        int sx = (int) sourceX;
        if (sx < 0) sx = 0;
        if (sx >= source.width()) sx = source.width() - 1;

        const PremulColour c = source.pixelAt(sx, sy);
        if (c.a == 0) continue; // nothing to composite, and the common case in a logo
        target_.blend(x, y, c, 255);
      }
    }
  }

  /**
   * Draw a bitmap under an arbitrary transform -- rotation and shear included.
   *
   * `imageToUser` maps IMAGE pixels to the current user space, so an identity
   * transform draws it at its natural size at the origin. A rotating needle is
   * Transform::rotationAbout(angle, pivot); a knob cap is the same thing.
   *
   * ── Why this is a separate function from drawImage ──
   *
   * drawImage walks the destination rectangle and steps through the source with
   * two scale factors, which is fast and cannot express a rotation. This maps
   * every destination pixel BACK through the inverse and samples there --
   * necessary, because walking the source and scattering into the destination
   * leaves holes wherever the mapping stretches, which is the speckled look of
   * every hand-rolled rotation.
   *
   * BILINEAR, unlike drawImage's nearest. A rotated edge sampled with nearest
   * is a staircase, and the staircase is the thing people notice about a
   * rotating knob. Texels outside the image count as fully transparent, which
   * gives the outer edge its antialiasing for free -- the sample at a pixel
   * half outside comes back half-covered because half of what it averaged was
   * nothing.
   */
  void drawImageTransformed(const Bitmap& source, const Transform& imageToUser) {
    if (source.isEmpty() || isClippedOut()) return;

    const Transform full = imageToUser.then(state_.transform);
    Transform inverse;
    // A singular transform -- a zero scale, which a collapsed component
    // produces -- has no inverse, and pressing on would fill the sampling loop
    // with NaN coordinates.
    if (!full.invert(&inverse)) return;

    // The device bounding box of the image's four CORNERS. Not two, as
    // drawImage uses: under a rotation the axis-aligned box is decided by all
    // four, and taking the diagonal pair clips the other two off.
    const float w = (float) source.width(), h = (float) source.height();
    const Point corners[4] = {full.apply({0.0f, 0.0f}), full.apply({w, 0.0f}),
                              full.apply({w, h}), full.apply({0.0f, h})};
    float x0 = corners[0].x, y0 = corners[0].y, x1 = corners[0].x, y1 = corners[0].y;
    for (int i = 1; i < 4; ++i) {
      x0 = corners[i].x < x0 ? corners[i].x : x0;
      y0 = corners[i].y < y0 ? corners[i].y : y0;
      x1 = corners[i].x > x1 ? corners[i].x : x1;
      y1 = corners[i].y > y1 ? corners[i].y : y1;
    }

    const PixelRect area = PixelRect::enclosing(Rect(x0, y0, x1 - x0, y1 - y0))
                               .intersection(state_.clip)
                               .intersection(target_.bounds());
    if (area.isEmpty()) return;

    for (int y = area.y; y < area.bottom(); ++y) {
      for (int x = area.x; x < area.right(); ++x) {
        // The pixel CENTRE, like every other sampling decision here.
        const Point in = inverse.apply({(float) x + 0.5f, (float) y + 0.5f});
        // Half a texel of margin either side, which is the region a bilinear
        // sample can still see something in. Rejecting at the exact bounds
        // instead cuts the antialiased edge off and leaves a hard one.
        if (in.x < -0.5f || in.y < -0.5f || in.x > w - 0.5f + 1.0f ||
            in.y > h - 0.5f + 1.0f)
          continue;
        const PremulColour c = sampleBilinear(source, in.x - 0.5f, in.y - 0.5f);
        if (c.a == 0) continue;
        target_.blend(x, y, c, 255);
      }
    }
  }

  /** At its natural size, top-left at `position`. */
  void drawImageAt(const Bitmap& source, Point position) {
    drawImage(source, Rect(position.x, position.y, (float) source.width(),
                           (float) source.height()));
  }

  /**
   * Bilinear, with everything outside the image fully transparent.
   *
   * PREMULTIPLIED throughout, which is what makes the plain weighted average
   * correct: interpolating straight-alpha colours pulls a fading edge towards
   * whatever the transparent texel's colour channels happen to hold, and for a
   * cleared bitmap that is black -- the dark halo around every naively
   * resampled logo.
   */
  static PremulColour sampleBilinear(const Bitmap& source, float x, float y) {
    const int x0 = (int) std::floor(x), y0 = (int) std::floor(y);
    const float fx = x - (float) x0, fy = y - (float) y0;

    const PremulColour a = texel(source, x0, y0);
    const PremulColour b = texel(source, x0 + 1, y0);
    const PremulColour c = texel(source, x0, y0 + 1);
    const PremulColour d = texel(source, x0 + 1, y0 + 1);

    const float w00 = (1.0f - fx) * (1.0f - fy);
    const float w10 = fx * (1.0f - fy);
    const float w01 = (1.0f - fx) * fy;
    const float w11 = fx * fy;

    PremulColour out;
    out.r = (uint8_t) (a.r * w00 + b.r * w10 + c.r * w01 + d.r * w11 + 0.5f);
    out.g = (uint8_t) (a.g * w00 + b.g * w10 + c.g * w01 + d.g * w11 + 0.5f);
    out.b = (uint8_t) (a.b * w00 + b.b * w10 + c.b * w01 + d.b * w11 + 0.5f);
    out.a = (uint8_t) (a.a * w00 + b.a * w10 + c.a * w01 + d.a * w11 + 0.5f);
    return out;
  }

  /** Outside is transparent, not clamped. Clamping would smear the edge row
   *  outwards and give a rotated image a fringe of its own border colour. */
  static PremulColour texel(const Bitmap& source, int x, int y) {
    if (x < 0 || y < 0 || x >= source.width() || y >= source.height()) return {};
    return source.pixelAt(x, y);
  }

  // ── Stroking ─────────────────────────────────────────────────────────────

  /**
   * Strokes are built in DEVICE space.
   *
   * The path is flattened through the transform first and the pen applied
   * afterwards, so a 1-pixel line is one pixel wide whatever the transform
   * says. Applying the pen in path space instead makes stroke width scale
   * with the component, and a panel at 150% then has hairlines at 1.5 pixels
   * that land between pixel centres and look grey.
   */
  void strokePath(const Path& p, const StrokeStyle& style) {
    if (isClippedOut() || style.width <= 0.0f) return;
    p.flatten(state_.transform, contours_);
    if (contours_.empty()) return;
    outline_.clear();
    Stroker::stroke(contours_, style, outline_);
    if (!state_.useGradient) {
      raster_.fill(target_, outline_, Transform(), state_.colour, FillRule::NonZero, state_.clip);
      return;
    }
    // The outline is already in device space -- see the note above -- so the
    // gradient has to be too, or a stroked ramp would run in the wrong
    // direction the moment anything is translated.
    const ColourGradient device = state_.gradient.transformed(state_.transform);
    raster_.fill(target_, outline_, Transform(), state_.colour, FillRule::NonZero, state_.clip,
                 &device);
  }

  void strokePath(const Path& p, float width) { strokePath(p, StrokeStyle{width}); }

  void drawLine(float x1, float y1, float x2, float y2, float width = 1.0f) {
    scratch_.clear();
    scratch_.moveTo(x1, y1);
    scratch_.lineTo(x2, y2);
    strokePath(scratch_, width);
  }

  void drawRect(const Rect& r, float width = 1.0f) {
    scratch_.clear();
    scratch_.addRect(r);
    strokePath(scratch_, width);
  }

  void drawRoundedRect(const Rect& r, float radius, float width = 1.0f) {
    scratch_.clear();
    scratch_.addRoundedRect(r, radius);
    strokePath(scratch_, width);
  }

  void drawEllipse(const Rect& r, float width = 1.0f) {
    scratch_.clear();
    scratch_.addEllipse(r);
    strokePath(scratch_, width);
  }

  Bitmap& target() { return target_; }

private:
  Rect transformedBounds(const Rect& r) const {
    const Point a = state_.transform.apply({r.x, r.y});
    const Point b = state_.transform.apply({r.right(), r.y});
    const Point c = state_.transform.apply({r.right(), r.bottom()});
    const Point d = state_.transform.apply({r.x, r.bottom()});
    const float l = std::min(std::min(a.x, b.x), std::min(c.x, d.x));
    const float t = std::min(std::min(a.y, b.y), std::min(c.y, d.y));
    const float rr = std::max(std::max(a.x, b.x), std::max(c.x, d.x));
    const float bb = std::max(std::max(a.y, b.y), std::max(c.y, d.y));
    return {l, t, rr - l, bb - t};
  }

  Bitmap& target_;
  State state_;
  std::vector<State> stack_;
  Rasteriser raster_;
  Path scratch_, outline_;
  std::vector<Contour> contours_;
};

} // namespace gfx
} // namespace sonore
