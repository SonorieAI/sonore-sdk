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

#include <memory>
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

/**
 * The drawing context a component paints through: a cursor over a Bitmap
 * with a brush (colour, gradient or tiled image), a transform and a clip, and
 * a stack to put them back. See the file comment for why the stack is not
 * optional; ScopedState is the way to use it.
 *
 * Mirrors the reference's Graphics. Coordinates handed to it are in the
 * current USER space -- a component's own, once the tree has translated into
 * it -- and the clip is kept in device pixels. The clip only narrows: clipTo
 * a rectangle is free, clipToPath and masks cost a byte per pixel of their
 * area, and layers (beginLayer/endLayer) group drawing so it fades as one.
 *
 * Does not own the target Bitmap, which must outlive it. A Graphics is made
 * for one paint and thrown away; it is not shared between threads, and it
 * draws on whatever thread paints (the message thread for an editor).
 */
class Graphics {
public:
  /** Draw into `target`, with the clip at its bounds, the identity transform
   *  and the palette's text colour. */
  explicit Graphics(Bitmap& target) : target_(&target) {
    state_.clip = target.bounds();
  }

  // ── State ────────────────────────────────────────────────────────────────

  /** Everything save() keeps and restore() puts back: the brush, the
   *  transform and the clip, with the clip's mask when it has one. */
  struct State {
    /** The flat colour, straight (not premultiplied) alpha: the brush while
     *  neither `useGradient` nor `useTile` is set. */
    Colour colour = palette::text();
    /** Set by setGradient, cleared by setColour. A gradient is a KIND of
     *  colour, so the two share a slot the way a brush does -- otherwise every
     *  fill call needs to be told which of the two to use. */
    ColourGradient gradient;
    /** Fills use `gradient` rather than `colour`. */
    bool useGradient = false;
    /** The texture setTiledImageFill set, its origin already in device pixels.
     *  Points at a Bitmap it does not own. */
    TiledImage tile;
    /** Fills use `tile`; checked before `useGradient`. */
    bool useTile = false;
    /** User space to device pixels: what every coordinate handed to a fill goes
     *  through. */
    Transform transform;
    /** The clip, in device pixels of the current target. Empty means nothing more
     *  is drawn. */
    PixelRect clip;
    /** A clip finer than `clip` -- a path's, a mask's -- or none. Shared,
     *  because save() copies the state and the mask does not change once
     *  made: clipping further makes a new one. */
    std::shared_ptr<const detail::CoverageMask> mask;
  };

  /** Closes any layer or mask still open, so what was drawn into it arrives. */
  ~Graphics() {
    // A layer left open has drawn into a bitmap nobody would see. Closed here,
    // so what was drawn arrives -- the caller forgot an endLayer, and the
    // forgetting should not also lose the picture.
    while (!layers_.empty()) {
      if (layers_.back().forMask) endMask();
      else endLayer();
    }
  }

  /** Not copyable: it holds the target, the state stack and any open layers. */
  Graphics(const Graphics&) = delete;
  Graphics& operator=(const Graphics&) = delete;

  /** The current brush, transform and clip. */
  const State& state() const { return state_; }

  /** Fill with a flat colour (straight alpha) from now on; turns off any
   *  gradient or tiled image. */
  void setColour(Colour c) {
    state_.colour = c;
    state_.useGradient = false;
    // BOTH cleared. Three ways of deciding a pixel's colour and only two of
    // them turned off is a fill that quietly ignores the colour it was handed
    // -- which is exactly the bug fillAll had with the gradient flag.
    state_.useTile = false;
  }
  /** The flat colour, kept even while a gradient or a tile is the brush. */
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
   *
   * `image` is not copied: it must stay alive for as long as this is the
   * brush. `scale` is user units per image pixel, so 1 is natural size in the
   * component's own units; `opacity` is 0..255.
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

  /** Whether the brush is the tiled image setTiledImageFill set. */
  bool hasTiledImage() const { return state_.useTile; }

  /** Whether the brush is the gradient setGradient set. */
  bool hasGradient() const { return state_.useGradient; }

  /** Applied BEFORE whatever is already there, so a child's transform is
   *  relative to its parent's -- which is the only order a component tree
   *  can compose in. */
  void addTransform(const Transform& t) { state_.transform = t.then(state_.transform); }

  /** Move the origin by (dx, dy) in the current user space: what the tree does
   *  before it paints a child. */
  void translate(float dx, float dy) { addTransform(Transform::translation(dx, dy)); }

  /** Narrows the clip; it can never widen. A component that could widen its
   *  clip could draw over its parent, which is the one thing a component
   *  tree must not allow. */
  void clipTo(const PixelRect& r) { state_.clip = state_.clip.intersection(r); }

  /** Narrow the clip to `r`, given in user space: taken through the transform
   *  (its bounding box, under a rotation) and rounded OUTWARD to whole device
   *  pixels. */
  void clipTo(const Rect& r) {
    clipTo(PixelRect::enclosing(transformedBounds(r)));
  }

  /** Whether the clip is empty, so nothing drawn would show: a paint() can
   *  return early. */
  bool isClippedOut() const { return state_.clip.isEmpty(); }

  /** Push the brush, transform and clip, for restore() to put back.
   *  ScopedState does both. */
  void save() { stack_.push_back(state_); }

  /** Put back what the last save() pushed. Does nothing with nothing saved, and
   *  cannot pop past the state an open layer or mask began with: endLayer and
   *  endMask own that one. */
  void restore() {
    // Not past the state a layer saved when it began: that one belongs to
    // endLayer, which has to put back the target the layer replaced along
    // with it.
    if (stack_.size() <= layerBase()) return;
    state_ = stack_.back();
    stack_.pop_back();
  }

  // ── Clipping to a shape ──────────────────────────────────────────────────

  /**
   * Narrow the clip to a path, antialiased, until the next restore().
   *
   * A rounded panel with an image in it, a circular meter, a waveform kept
   * inside its rounded frame. Everything drawn after this has its coverage
   * multiplied by the path's at each pixel, so the edge is as smooth as a
   * filled path's would be -- not the staircase of a clip that is in or out.
   *
   * It costs a byte per pixel of the path's bounds, made once per call, and
   * a multiply per pixel drawn while it lasts. A rectangle on whole pixels
   * wants clipTo(Rect), which costs neither.
   */
  void clipToPath(const Path& path, FillRule rule = FillRule::NonZero) {
    if (isClippedOut()) return;
    path.flatten(state_.transform, contours_);
    float left = 1e30f, top = 1e30f, right = -1e30f, bottom = -1e30f;
    for (const Contour& c : contours_)
      for (const Point& q : c.points) {
        if (!std::isfinite(q.x) || !std::isfinite(q.y)) continue;
        left = (std::min)(left, q.x);
        top = (std::min)(top, q.y);
        right = (std::max)(right, q.x);
        bottom = (std::max)(bottom, q.y);
      }
    const PixelRect area =
        right > left && bottom > top
            ? PixelRect::enclosing(Rect(left, top, right - left, bottom - top))
                  .intersection(state_.clip)
                  .intersection(target_->bounds())
            : PixelRect{};
    if (area.isEmpty()) {
      clipOut();
      return;
    }
    // Filled white into a scratch bitmap the size of the area: its alpha is
    // then exactly the coverage the rasteriser gives the path, edge for edge
    // the same as fillPath would draw it.
    maskScratch_.resize(area.w, area.h); // and cleared
    raster_.fill(maskScratch_, path,
                 state_.transform.then(Transform::translation((float) -area.x, (float) -area.y)),
                 Colour(255, 255, 255, 255), rule);
    auto mask = std::make_shared<detail::CoverageMask>();
    mask->area = area;
    mask->coverage.resize((size_t) area.w * (size_t) area.h);
    const uint8_t* pixels = maskScratch_.data();
    for (int y = 0; y < area.h; ++y)
      for (int x = 0; x < area.w; ++x) {
        const uint8_t here = pixels[maskScratch_.offsetOf(x, y) + 3];
        mask->coverage[(size_t) y * (size_t) area.w + (size_t) x] =
            state_.mask ? detail::multiplyCoverage(here, state_.mask->at(area.x + x, area.y + y))
                        : here;
      }
    state_.clip = area;
    state_.mask = std::move(mask);
  }

  // ── Layers ───────────────────────────────────────────────────────────────

  /**
   * Draw what follows into a layer of its own, then put the layer down at
   * `opacity` when endLayer() comes.
   *
   * The difference from drawing at that opacity directly is where things
   * overlap. A half-transparent group of shapes drawn shape by shape shows
   * every overlap darker -- the fill through its own stroke, one segment of a
   * meter through the next. Drawn into a layer, the group is one picture and
   * fades as one. An SVG's group opacity is exactly this.
   *
   * The layer is the size of the current clip, so clip first: a layer around
   * an icon costs the icon's pixels, not the window's. Every save() inside
   * is undone by endLayer(), and a restore() cannot reach past it.
   */
  void beginLayer(float opacity = 1.0f) { pushLayer(opacity, false); }

  /** Close the layer beginLayer opened and put it down at its opacity, through
   *  the clip and mask in force before it began; the state beginLayer saved is
   *  back. Does nothing unless the innermost open layer is one (not a mask). */
  void endLayer() {
    if (layers_.empty() || layers_.back().forMask) return;
    Layer layer = popLayer();
    const float o = layer.opacity;
    const uint8_t alpha = (uint8_t) (o * 255.0f + 0.5f);
    if (alpha == 0) return;
    const detail::CoverageMask* mask = state_.mask.get();
    const PixelRect area = layer.area.intersection(state_.clip).intersection(target_->bounds());
    for (int y = area.y; y < area.bottom(); ++y)
      for (int x = area.x; x < area.right(); ++x) {
        const PremulColour c = layer.bitmap->pixelAt(x - layer.area.x, y - layer.area.y);
        if (c.a == 0) continue;
        const uint8_t cov = mask ? detail::multiplyCoverage(alpha, mask->at(x, y)) : alpha;
        target_->blend(x, y, c, cov);
      }
  }

  /** Where a mask's coverage comes from: how opaque it is, or how light. */
  enum class MaskFrom { Alpha, /**< how opaque what was drawn is */ Luminance /**< how light, times its alpha */ };

  /**
   * Draw a mask: what is drawn between this and endMask() is not shown, it
   * becomes a clip -- the coverage of everything drawn after, until the next
   * restore(). A soft vignette, a fade at the end of a scrolling list, an
   * SVG's `<mask>` and `<clipPath>`.
   *
   * Like a layer, it is the size of the current clip; anything outside what
   * the mask draws is clipped away.
   */
  void beginMask() { pushLayer(1.0f, true); }

  /** Close the mask beginMask opened: what was drawn becomes the coverage of
   *  everything drawn next, multiplied into any mask already in force, until a
   *  restore() of a state saved before beginMask. Does nothing unless the
   *  innermost open layer is a mask. */
  void endMask(MaskFrom from = MaskFrom::Alpha) {
    if (layers_.empty() || !layers_.back().forMask) return;
    Layer layer = popLayer();
    const PixelRect area = layer.area.intersection(state_.clip).intersection(target_->bounds());
    if (area.isEmpty()) {
      clipOut();
      return;
    }
    auto mask = std::make_shared<detail::CoverageMask>();
    mask->area = area;
    mask->coverage.resize((size_t) area.w * (size_t) area.h);
    for (int y = area.y; y < area.bottom(); ++y)
      for (int x = area.x; x < area.right(); ++x) {
        const PremulColour c = layer.bitmap->pixelAt(x - layer.area.x, y - layer.area.y);
        // Luminance of the PREMULTIPLIED colour is the luminance of the
        // straight colour times its alpha -- which is what a luminance mask
        // asks for, in one step. The weights are the sRGB ones SVG names.
        uint8_t here = c.a;
        if (from == MaskFrom::Luminance) {
          const float l = 0.2125f * (float) c.r + 0.7154f * (float) c.g + 0.0721f * (float) c.b;
          here = (uint8_t) (l > 255.0f ? 255.0f : l + 0.5f);
        }
        if (state_.mask) here = detail::multiplyCoverage(here, state_.mask->at(x, y));
        mask->coverage[(size_t) (y - area.y) * (size_t) area.w + (size_t) (x - area.x)] = here;
      }
    state_.clip = area;
    state_.mask = std::move(mask);
  }

  /** Save on construction, restore on destruction -- including on an early
   *  return or an exception. */
  class ScopedState {
  public:
    /** Calls g.save(); `g` must outlive this object. */
    explicit ScopedState(Graphics& g) : g_(g) { g_.save(); }
    /** Calls restore() on the Graphics it saved. */
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
    paintPath(scratch_, Transform(), FillRule::NonZero);
  }

  /** Fill the whole clip with `c` and put the brush back as it was: a
   *  gradient or a tiled image set before is set again after. */
  void fillAll(Colour c) {
    const Colour was = state_.colour;
    // The gradient flag and the tile's too, not just the colour. Without them,
    // fillAll(c) with a gradient or a tile set paints that and ignores the
    // colour it was handed -- silently, because it draws something plausible
    // either way. The tile was the one left out, after the gradient was fixed.
    const bool wasGradient = state_.useGradient;
    const bool wasTile = state_.useTile;
    state_.colour = c;
    state_.useGradient = false;
    state_.useTile = false;
    fillAll();
    state_.colour = was;
    state_.useGradient = wasGradient;
    state_.useTile = wasTile;
  }

  /** Fill `r`, in user space, with the current brush; fractional edges are
   *  antialiased. An empty rectangle draws nothing. */
  void fillRect(const Rect& r) {
    if (r.isEmpty()) return;
    scratch_.clear();
    scratch_.addRect(r);
    fillPath(scratch_);
  }

  /** Fill `r` with corners of `radius` user units, clamped to half the shorter
   *  side; 0 or less is a plain rectangle. */
  void fillRoundedRect(const Rect& r, float radius) {
    scratch_.clear();
    scratch_.addRoundedRect(r, radius);
    fillPath(scratch_);
  }

  /** Fill the ellipse that fits `r`. */
  void fillEllipse(const Rect& r) {
    scratch_.clear();
    scratch_.addEllipse(r);
    fillPath(scratch_);
  }

  /** Fill `p`, in user space, with the current brush, inside or outside decided
   *  by `rule`. */
  void fillPath(const Path& p, FillRule rule = FillRule::NonZero) {
    if (isClippedOut()) return;
    paintPath(p, state_.transform, rule);
  }

  /**
   * Coverage worked out elsewhere -- a line of text's, from cached glyphs --
   * painted as fillPath paints a path's, with the colour, gradient or tile,
   * through the clip and the clip mask. `coverage` is `width` by `height`
   * bytes, row by row, its top-left at (left, top) in DEVICE pixels: the
   * transform has already been applied by whoever made it.
   */
  void fillCoverage(const uint8_t* coverage, int width, int height, int left, int top) {
    if (isClippedOut()) return;
    const detail::CoverageMask* mask = state_.mask.get();
    if (state_.useTile) {
      raster_.paintCoverage(*target_, coverage, width, height, left, top, state_.colour, state_.clip, nullptr,
                            &state_.tile, mask);
      return;
    }
    if (!state_.useGradient) {
      raster_.paintCoverage(*target_, coverage, width, height, left, top, state_.colour, state_.clip, nullptr,
                            nullptr, mask);
      return;
    }
    const ColourGradient device = state_.gradient.transformed(state_.transform);
    raster_.paintCoverage(*target_, coverage, width, height, left, top, state_.colour, state_.clip, &device,
                          nullptr, mask);
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
    // Written to fail for NaN, and an infinite span -- a corner at infinity --
    // has no pixels to sample either.
    if (!(spanX > 0.0f && spanY > 0.0f) || !std::isfinite(spanX) || !std::isfinite(spanY)) return;

    const PixelRect area = PixelRect::enclosing(Rect(x0, y0, spanX, spanY))
                               .intersection(state_.clip)
                               .intersection(target_->bounds());
    if (area.isEmpty()) return;

    const float scaleX = (float) source.width() / spanX;
    const float scaleY = (float) source.height() / spanY;

    // Which source column each destination column samples, worked out once
    // rather than once a row: every row asks the same question of the same x.
    // Sampled at the pixel CENTRE, like everything else here. A corner sample
    // shifts the whole image half a pixel, which shows as a seam where two
    // images meet and as a soft edge on a logo that should be crisp.
    // Clamped in float BEFORE the conversion: a destination a hair tall
    // scales by a factor past INT_MAX, and converting that is undefined.
    const int lastColumn = source.width() - 1;
    const int lastRow = source.height() - 1;
    columns_.resize((size_t) area.w);
    for (int x = area.x; x < area.right(); ++x) {
      const float sourceX = ((float) x + 0.5f - x0) * scaleX;
      columns_[(size_t) (x - area.x)] =
          !(sourceX > 0.0f) ? 0 : sourceX >= (float) lastColumn ? lastColumn : (int) sourceX;
    }

    // The pixels are written in place: the area is inside the target and the
    // samples inside the source, which is what blend and pixelAt would test
    // for each one. An opaque sample under no mask is stored -- `over` at
    // full coverage gives exactly the source -- and is most of a backdrop.
    const detail::CoverageMask* mask = state_.mask.get();
    const uint8_t* const sourcePixels = source.data();
    for (int y = area.y; y < area.bottom(); ++y) {
      const float sourceY = ((float) y + 0.5f - y0) * scaleY;
      const int sy = !(sourceY > 0.0f) ? 0 : sourceY >= (float) lastRow ? lastRow : (int) sourceY;
      const uint8_t* const sourceRow = sourcePixels + source.offsetOf(0, sy);
      uint8_t* p = target_->data() + target_->offsetOf(area.x, y);
      const int* column = columns_.data();
      if (!mask) {
        for (int i = 0; i < area.w; ++i, p += 4) {
          const uint8_t* s = sourceRow + (size_t) column[i] * 4u;
          if (s[3] == 0) continue; // nothing to composite, and the common case in a logo
          if (s[3] == 255) {
            std::memcpy(p, s, 4);
            continue;
          }
          detail::blendPixel(p, PremulColour(s[0], s[1], s[2], s[3]), 255);
        }
        continue;
      }
      for (int i = 0; i < area.w; ++i, p += 4) {
        const uint8_t* s = sourceRow + (size_t) column[i] * 4u;
        if (s[3] == 0) continue;
        const uint8_t cov = mask->at(area.x + i, y);
        if (cov == 0) continue;
        if (cov == 255 && s[3] == 255) {
          std::memcpy(p, s, 4);
          continue;
        }
        detail::blendPixel(p, PremulColour(s[0], s[1], s[2], s[3]), cov);
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
                               .intersection(target_->bounds());
    if (area.isEmpty()) return;

    for (int y = area.y; y < area.bottom(); ++y) {
      for (int x = area.x; x < area.right(); ++x) {
        // The pixel CENTRE, like every other sampling decision here.
        const Point in = inverse.apply({(float) x + 0.5f, (float) y + 0.5f});
        // Half a texel of margin either side, which is the region a bilinear
        // sample can still see something in. Rejecting at the exact bounds
        // instead cuts the antialiased edge off and leaves a hard one. Written
        // as what is INSIDE, so a NaN -- a transform with an infinity in it
        // inverts to one -- is outside, not converted to an int.
        if (!(in.x >= -0.5f && in.y >= -0.5f && in.x <= w - 0.5f + 1.0f &&
              in.y <= h - 0.5f + 1.0f))
          continue;
        const PremulColour c = sampleBilinear(source, in.x - 0.5f, in.y - 0.5f);
        if (c.a == 0) continue;
        target_->blend(x, y, c, state_.mask ? state_.mask->at(x, y) : (uint8_t) 255);
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
   * How far the farthest pixel that can be drawn is from the flattened path:
   * the diagonal of the box around both, the clip and the path.
   *
   * Once half a stroke's width is more than this, every pixel inside the
   * clip is within reach of the line and a wider stroke covers the same
   * ones. strokePath cuts the width to 4096 times this. A wider one did not
   * DRAW the same: at 1e29 adding half the width to a point swallowed the
   * point (a float keeps 24 bits), so every segment's end ran through the
   * origin rather than its vertex and one stroke put tens of thousands of
   * edges across the canvas, a second for a single call; and half the width
   * squared, in the miter test, overflowed and turned every miter into a
   * bevel. The one piece whose far edge can come nearer than half the width
   * is the bevel at a corner that turns almost straight back, and it reaches
   * in from 4096 times the distance only within a few hundredths of a degree
   * of a full reversal.
   */
  float strokeReach() const {
    float left = (float) state_.clip.x, top = (float) state_.clip.y;
    float right = (float) state_.clip.right(), bottom = (float) state_.clip.bottom();
    for (const Contour& c : contours_)
      for (const Point& q : c.points) {
        if (!std::isfinite(q.x) || !std::isfinite(q.y)) continue;
        left = (std::min)(left, q.x);
        top = (std::min)(top, q.y);
        right = (std::max)(right, q.x);
        bottom = (std::max)(bottom, q.y);
      }
    return std::sqrt((right - left) * (right - left) + (bottom - top) * (bottom - top));
  }

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
    // Written to fail for a NaN width too, which has nothing to draw.
    if (isClippedOut() || !(style.width > 0.0f)) return;
    p.flatten(state_.transform, contours_);
    if (!style.dashes.empty() && Stroker::dash(contours_, style.dashes, style.dashOffset, dashed_))
      contours_.swap(dashed_);
    if (contours_.empty()) return;
    outline_.clear();
    const float reach = strokeReach();
    StrokeStyle cut = style;
    const float widest = 2.0f * 4096.0f * (reach + 1.0f);
    if (cut.width > widest) cut.width = widest;
    Stroker::stroke(contours_, cut, outline_, reach);
    // The outline is in device space already -- see the note above -- and
    // paintPath moves the gradient and the texture there too: a stroked ramp
    // runs the right way when anything is translated, and a tiled stroke is
    // tiled rather than drawn in the plain colour underneath.
    paintPath(outline_, Transform(), FillRule::NonZero);
  }

  /** A plain stroke `width` device pixels wide: miter joins, butt caps, solid. */
  void strokePath(const Path& p, float width) { strokePath(p, StrokeStyle{width}); }

  /** A line from (x1, y1) to (x2, y2) in user space, with butt ends. `width`
   *  is in DEVICE pixels and not scaled by the transform, as for strokePath. */
  void drawLine(float x1, float y1, float x2, float y2, float width = 1.0f) {
    scratch_.clear();
    scratch_.moveTo(x1, y1);
    scratch_.lineTo(x2, y2);
    strokePath(scratch_, width);
  }

  /** Stroke the outline of `r`, centred on its edges, `width` device pixels
   *  wide: a 1-pixel line along a whole-pixel edge is spread over the two
   *  pixels either side of it. */
  void drawRect(const Rect& r, float width = 1.0f) {
    scratch_.clear();
    scratch_.addRect(r);
    strokePath(scratch_, width);
  }

  /** Stroke the outline fillRoundedRect would fill, `width` device pixels
   *  wide. */
  void drawRoundedRect(const Rect& r, float radius, float width = 1.0f) {
    scratch_.clear();
    scratch_.addRoundedRect(r, radius);
    strokePath(scratch_, width);
  }

  /** Stroke the ellipse that fits `r`, `width` device pixels wide. */
  void drawEllipse(const Rect& r, float width = 1.0f) {
    scratch_.clear();
    scratch_.addEllipse(r);
    strokePath(scratch_, width);
  }

  /** The bitmap being drawn into: an open layer's or mask's while one is open,
   *  not necessarily the one this Graphics was made with. */
  Bitmap& target() { return *target_; }

private:
  /**
   * The one place a fill reaches the rasteriser, whatever the brush: a
   * colour, a gradient or a texture, through the clip and the mask. Three
   * copies of this, one per kind of fill, is how the tiled stroke once drew
   * in plain colour -- one of them had not been told about textures.
   *
   * `transform` takes the path to device space. The gradient is moved there
   * ONCE per fill: the alternative is inverting the transform per pixel,
   * the same answer at a few hundred times the cost.
   */
  void paintPath(const Path& p, const Transform& transform, FillRule rule) {
    const detail::CoverageMask* mask = state_.mask.get();
    if (state_.useTile) {
      raster_.fill(*target_, p, transform, state_.colour, rule, state_.clip, nullptr,
                   &state_.tile, mask);
      return;
    }
    if (!state_.useGradient) {
      raster_.fill(*target_, p, transform, state_.colour, rule, state_.clip, nullptr, nullptr,
                   mask);
      return;
    }
    const ColourGradient device = state_.gradient.transformed(state_.transform);
    raster_.fill(*target_, p, transform, state_.colour, rule, state_.clip, &device, nullptr,
                 mask);
  }

  /** Nothing more can be drawn until the next restore(). */
  void clipOut() {
    state_.clip = PixelRect{};
    state_.mask.reset();
  }

  /** An open layer or mask: the bitmap drawn into instead of the target,
   *  the target it replaced, where on that target it goes down, and the
   *  depth of the state stack when it began. */
  struct Layer {
    std::unique_ptr<Bitmap> bitmap;
    Bitmap* below = nullptr;
    PixelRect area;
    float opacity = 1.0f;
    size_t depth = 0; // the stack's size before the layer's own save()
    bool forMask = false;
  };

  /** The saved states that belong to the open layer, which restore() may
   *  not pop: the one it saved when it began, and those under it. */
  size_t layerBase() const { return layers_.empty() ? 0 : layers_.back().depth + 1; }

  void pushLayer(float opacity, bool forMask) {
    save();
    Layer layer;
    layer.depth = stack_.size() - 1;
    layer.below = target_;
    layer.forMask = forMask;
    // Written so a NaN is 0: nothing drawn, rather than a NaN alpha.
    layer.opacity = opacity > 0.0f ? (opacity < 1.0f ? opacity : 1.0f) : 0.0f;
    layer.area = state_.clip.intersection(target_->bounds());
    if (layer.area.isEmpty()) layer.area = PixelRect{};
    layer.bitmap = std::make_unique<Bitmap>(layer.area.w > 0 ? layer.area.w : 0,
                                            layer.area.h > 0 ? layer.area.h : 0);
    target_ = layer.bitmap.get();
    // Everything that was in device space moves with the layer's origin: the
    // transform's output, the clip, a texture's origin. The mask stays with
    // the state underneath -- the layer is put down through it.
    const float dx = (float) -layer.area.x, dy = (float) -layer.area.y;
    state_.transform = state_.transform.then(Transform::translation(dx, dy));
    state_.clip = PixelRect{0, 0, layer.area.w, layer.area.h};
    state_.mask.reset();
    state_.tile.originX += dx;
    state_.tile.originY += dy;
    layers_.push_back(std::move(layer));
  }

  /** The layer closed: the target and the state it began with put back. */
  Layer popLayer() {
    Layer layer = std::move(layers_.back());
    layers_.pop_back();
    target_ = layer.below;
    while (stack_.size() > layer.depth + 1) stack_.pop_back();
    state_ = stack_.back();
    stack_.pop_back();
    return layer;
  }

  Rect transformedBounds(const Rect& r) const {
    const Point a = state_.transform.apply({r.x, r.y});
    const Point b = state_.transform.apply({r.right(), r.y});
    const Point c = state_.transform.apply({r.right(), r.bottom()});
    const Point d = state_.transform.apply({r.x, r.bottom()});
    const float l = (std::min)((std::min)(a.x, b.x), (std::min)(c.x, d.x));
    const float t = (std::min)((std::min)(a.y, b.y), (std::min)(c.y, d.y));
    const float rr = (std::max)((std::max)(a.x, b.x), (std::max)(c.x, d.x));
    const float bb = (std::max)((std::max)(a.y, b.y), (std::max)(c.y, d.y));
    return {l, t, rr - l, bb - t};
  }

  Bitmap* target_;
  State state_;
  std::vector<State> stack_;
  Rasteriser raster_;
  Path scratch_, outline_;
  std::vector<Contour> contours_, dashed_;
  std::vector<Layer> layers_;
  Bitmap maskScratch_;
  std::vector<int> columns_; // drawImage's source column per destination column
};

} // namespace gfx
} // namespace sonore
