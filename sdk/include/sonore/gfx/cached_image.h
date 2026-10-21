// SPDX-License-Identifier: Apache-2.0
//
// StandardCachedComponentImage: what Component::setBufferedToImage installs.
//
// Included at the END of component.h, after Component is complete -- the
// cache paints the component, and the component's setBufferedToImage makes
// the cache, so each needs the other whole. Including this header on its own
// works too: it includes component.h first, whose tail include of this file
// is then skipped by #pragma once.
#pragma once

#include <memory>

#include "component.h"

namespace sonore {
namespace gfx {

/**
 * The cache setBufferedToImage installs: the component's subtree rendered once
 * into a Bitmap at DEVICE resolution, then composited from it.
 *
 * ── Why device pixels, and why the sub-pixel offset is part of the key ──
 *
 * A component at x = 10.25 under a 1.5 scale lands at device x = 15.375. Its
 * antialiased edges are decided by that .375. A cache rendered at logical
 * resolution and scaled up would blur; one rendered at device resolution but
 * at a whole-pixel origin would put every edge in a slightly different place
 * from where painting directly puts it. So the cache is rendered with the
 * very transform the direct paint would use, shifted by WHOLE pixels only --
 * and when the scale or the fractional part of the position changes, it is
 * rendered again. The result is the direct paint's pixels, composited: exact
 * for an opaque component, and within a level of rounding for a translucent
 * one (8-bit "over" is associative only up to rounding).
 *
 * ── What invalidates it ──
 *
 * repaint() of the component or of anything inside it (Component::repaint
 * walks up to every cached ancestor), a child being added, removed or moved,
 * and a resize. NOT a move of the component itself: its picture did not
 * change, only where it goes. Invalidation is by rectangle, and only the
 * invalid rectangles are re-rendered, through the same damage-skipping walk
 * the window uses.
 *
 * Rotation or shear in the transform cannot be cached this way -- the device
 * footprint would no longer be a rectangle of the same pixels -- and paints
 * directly instead, every time.
 */
class StandardCachedComponentImage : public CachedComponentImage {
public:
  /** A cache for `owner`, which owns it (through setBufferedToImage) and so
   *  outlives it. Empty until the first paint renders it. */
  explicit StandardCachedComponentImage(Component& owner) : owner_(owner) {}

  void paint(Graphics& g) override {
    const Graphics::State& st = g.state();
    const Transform& t = st.transform;
    if (t.b != 0.0f || t.c != 0.0f || !(t.a > 0.0f) || !(t.d > 0.0f)) {
      owner_.paintTreeUncached(g);
      return;
    }
    const Rect local = owner_.localBounds();
    const PixelRect area =
        PixelRect::enclosing(Rect(t.e, t.f, local.w * t.a, local.h * t.d));
    if (area.isEmpty()) return;

    const float fracX = t.e - (float) area.x, fracY = t.f - (float) area.y;
    if (!valid_ || area.w != cache_.width() || area.h != cache_.height() || t.a != scaleX_ ||
        t.d != scaleY_ || fracX != fracX_ || fracY != fracY_) {
      cache_.resize(area.w, area.h); // and cleared to transparent
      scaleX_ = t.a;
      scaleY_ = t.d;
      fracX_ = fracX;
      fracY_ = fracY;
      invalid_.clear();
      render(Transform(t.a, 0.0f, 0.0f, t.d, fracX, fracY), cache_.bounds(), nullptr);
      valid_ = true;
    } else if (!invalid_.isEmpty()) {
      const RectangleList stale = invalid_;
      invalid_.clear();
      for (const Rect& r : stale.rects()) {
        const PixelRect dev =
            PixelRect::enclosing(Rect(fracX + r.x * t.a, fracY + r.y * t.d, r.w * t.a, r.h * t.d))
                .intersection(cache_.bounds());
        if (dev.isEmpty()) continue;
        for (int y = dev.y; y < dev.bottom(); ++y)
          for (int x = dev.x; x < dev.right(); ++x) cache_.setPixel(x, y, PremulColour{});
        // What was CLEARED, back in the component's units -- the outward
        // rounding takes in the edge pixels of things `r` does not touch,
        // and they have to be painted back as well. Backing::render's rule.
        RectangleList damage;
        damage.add(Rect(((float) dev.x - fracX) / t.a, ((float) dev.y - fracY) / t.d,
                        (float) dev.w / t.a, (float) dev.h / t.d));
        render(Transform(t.a, 0.0f, 0.0f, t.d, fracX, fracY), dev, &damage);
      }
    }

    // Composited 1:1 at the device position, through the caller's clip and
    // clip mask -- drawImage's inner loop without the resampling, because
    // there is nothing to resample.
    Bitmap& target = g.target();
    const PixelRect visible = st.clip.intersection(area).intersection(target.bounds());
    const detail::CoverageMask* mask = st.mask.get();
    for (int y = visible.y; y < visible.bottom(); ++y)
      for (int x = visible.x; x < visible.right(); ++x) {
        const PremulColour c = cache_.pixelAt(x - area.x, y - area.y);
        if (c.a == 0) continue;
        target.blend(x, y, c, mask ? mask->at(x, y) : (uint8_t) 255);
      }
  }

  bool invalidateAll() override {
    valid_ = false;
    return true;
  }

  bool invalidate(const Rect& area) override {
    const Rect r = area.intersection(owner_.localBounds());
    if (r.isEmpty()) return true;
    // Past a handful of rectangles, re-rendering the lot costs less than
    // walking the tree once per rectangle.
    if (invalid_.rects().size() >= 8) return invalidateAll();
    invalid_.add(r);
    return true;
  }

  void releaseResources() override {
    cache_.resize(0, 0);
    valid_ = false;
  }

  /** How many times anything was rendered into the cache: what a test
   *  checks to know a paint came from it rather than from the component. */
  int renderCount() const { return renders_; }
  /** The cached pixels, in device pixels at the last transform's scale. */
  const Bitmap& bitmap() const { return cache_; }

private:
  void render(const Transform& t, const PixelRect& clip, const RectangleList* damage) {
    ++renders_;
    Graphics cg(cache_);
    cg.clipTo(clip);
    cg.addTransform(t);
    owner_.paintTreeUncached(cg, damage);
  }

  Component& owner_;
  Bitmap cache_;
  RectangleList invalid_;
  float scaleX_ = 0.0f, scaleY_ = 0.0f, fracX_ = 0.0f, fracY_ = 0.0f;
  bool valid_ = false;
  int renders_ = 0;
};

inline void Component::setBufferedToImage(bool shouldBeBuffered) {
  if (shouldBeBuffered == (cachedImage_ != nullptr)) return;
  if (shouldBeBuffered) setCachedComponentImage(std::make_unique<StandardCachedComponentImage>(*this));
  else setCachedComponentImage(nullptr);
}

} // namespace gfx
} // namespace sonore
