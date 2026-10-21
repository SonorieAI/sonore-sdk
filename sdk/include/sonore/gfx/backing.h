// SPDX-License-Identifier: Apache-2.0
//
// The backing store a window peer paints into, and the one place scale lives.
//
// ── Why this exists ─────────────────────────────────────────────────────────
//
// All three peers -- Win32, X11, Cocoa -- had the same forty lines: hold a
// Bitmap, resize it when the window resizes, clear the ground, paint the tree,
// track whether the next frame is a full one, and hand the changed rectangles
// back to the platform. Three copies of one algorithm, which is the shape
// every drift in this SDK has had.
//
// It is pulled out here because of what had to change in it.
//
// ── HiDPI ───────────────────────────────────────────────────────────────────
//
// The wrappers already accept a scale from the host -- CLAP's gui.set_scale,
// VST3's IPlugViewContentScaleSupport, LV2's ui:scaleFactor -- and already
// resize the window to `logical * scale` DEVICE pixels. For the webview editor
// that is the whole job: the browser engine draws each CSS pixel as `scale`
// device pixels itself.
//
// The native editor had no such engine, and nothing was doing the scaling. The
// window doubled and the component tree was laid out to the new DEVICE size, so
// at 200% a 620-point editor became a 1240-unit editor drawn at 1240 device
// pixels: the same controls at the same pixel sizes, which on that display is
// HALF their intended physical size, with twice as many of them on screen. The
// interface did not scale. It got denser and smaller, which is the exact
// failure a plugin developer means by "it looks wrong on my 4K laptop".
//
// The fix is one idea consistently applied: the component tree works in LOGICAL
// units and knows nothing about scale, and this object is the only thing that
// knows the difference.
//
//   - the bitmap is `logical * scale` device pixels
//   - the content's bounds are the LOGICAL size
//   - the root paint runs under a scale transform, so every path, every stroke
//     and every glyph is rasterised at device resolution -- the text is SHARPER
//     at 200%, not bigger and blurrier, because the glyphs are outlines rather
//     than a stretched bitmap
//   - damage is tracked in logical units and converted to device rectangles
//     once, here, on the way out to the platform
//   - a pointer position arrives in device pixels and is divided on the way in
//
// ── The alternative that does not work ──────────────────────────────────────
//
// Painting at logical resolution and stretching the bitmap by `scale`. That is
// what a plugin does when it "supports HiDPI" by not supporting it, and it is
// worse than doing nothing: the interface is the right physical size and
// visibly soft, which readers notice on text long before they notice anything
// else.
#pragma once

#include <vector>

#include "bitmap.h"
#include "component.h"
#include "graphics.h"
#include "region.h"

namespace sonore {
namespace gfx {

/**
 * A window peer's backing store: the device-pixel bitmap, the scale, and the
 * paint of the component tree into it. See the file comment for why scale
 * lives here and nowhere else.
 *
 * The peer feeds it platform facts (setDeviceSize from a resize event,
 * setScale from the host, pointer positions through toLogical) and calls
 * render() when it is asked to paint; what comes back is the list of device
 * rectangles to blit, empty when nothing changed. The content component is not
 * owned and must outlive the Backing, or be replaced with setContent first.
 * UI thread only, like everything that touches the tree.
 */
class Backing {
public:
  /** The component filling the window. Not owned -- the peer's caller owns it,
   *  and outlives the window. */
  void setContent(Component* content) {
    content_ = content;
    fullRepaintNeeded_ = true;
    applyLayout();
  }

  /** The component filling the window, or null. */
  Component* content() const { return content_; }

  /** What to clear to before the tree paints. Below the tree, so it cannot come
   *  from a LookAndFeel -- there is no component to ask yet. */
  void setGround(Colour c) {
    ground_ = c;
    fullRepaintNeeded_ = true;
  }

  // ── Size and scale ───────────────────────────────────────────────────────

  /**
   * The client area, in DEVICE pixels -- which is what every platform reports.
   *
   * WM_SIZE, ConfigureNotify and -[NSView setFrame:] all speak device pixels
   * (on macOS after the backing-scale conversion the peer does), so this is the
   * setter a resize event calls and the logical size is derived.
   */
  void setDeviceSize(int width, int height) {
    if (width == deviceWidth_ && height == deviceHeight_) return;
    deviceWidth_ = width > 0 ? width : 0;
    deviceHeight_ = height > 0 ? height : 0;
    // Resized, which keeps the buffer it has when the new size fits in it --
    // a drag that grows the window by a few pixels a step reallocates only
    // now and then, because the buffer grows by half again when it is
    // outgrown. But shrunk to under half of what it holds, the bitmap is
    // made anew at this size: a window moved from a 200% monitor to a 100%
    // one went on holding the 200% frame, 9 MB for the showcase where 1.5
    // would do, for as long as it stayed open. One allocation for that move,
    // and none for a drag.
    const size_t pixels = (size_t) deviceWidth_ * (size_t) deviceHeight_;
    if (pixels * 2u < heldPixels_) {
      bitmap_ = Bitmap(deviceWidth_, deviceHeight_);
      heldPixels_ = pixels;
    } else {
      bitmap_.resize(deviceWidth_, deviceHeight_);
      if (pixels > heldPixels_) heldPixels_ = pixels;
    }
    // A resized bitmap has no previous frame to keep, so the next paint is a
    // full one. Not an optimisation to skip: the new pixels contain nothing.
    fullRepaintNeeded_ = true;
    applyLayout();
  }

  /**
   * Device pixels per logical pixel: 1.0, 1.25, 1.5, 2.0, 3.0.
   *
   * Clamped rather than trusted. A host that sends 0 -- and one does, when it
   * asks before it knows -- would divide the logical size by zero and lay the
   * tree out at infinity; 8 is past any display that exists and is there so a
   * garbage value cannot ask for a bitmap the size of memory.
   */
  void setScale(float scale) {
    const float clamped = scale > 0.05f ? (scale < 8.0f ? scale : 8.0f) : 1.0f;
    if (clamped == scale_) return;
    scale_ = clamped;
    // The LOGICAL size just changed even though the window did not, so the tree
    // has to be laid out again and every pixel is stale.
    fullRepaintNeeded_ = true;
    applyLayout();
  }

  /** Device pixels per logical pixel, 0.05..8; 1 until setScale. */
  float scale() const { return scale_; }
  /** The client area's width, device pixels. */
  int deviceWidth() const { return deviceWidth_; }
  /** The client area's height, device pixels. */
  int deviceHeight() const { return deviceHeight_; }

  /** The width the content is laid out to: device width over the scale. */
  float logicalWidth() const { return (float) deviceWidth_ / scale_; }
  /** The height the content is laid out to: device height over the scale. */
  float logicalHeight() const { return (float) deviceHeight_ / scale_; }

  /** The size to ASK the platform for, given a size in logical units. Rounded
   *  rather than truncated: at 150% a 621-point editor is 931.5, and truncating
   *  loses a pixel off the edge on every odd multiple. */
  static int toDevice(float logical, float scale) {
    return (int) (logical * scale + 0.5f);
  }

  /** The device-pixel frame render() paints into and the peer blits from. */
  Bitmap& bitmap() { return bitmap_; }
  /** The same, read-only. */
  const Bitmap& bitmap() const { return bitmap_; }

  /**
   * Give the pixels back: the memory, not only the size. For a peer whose
   * window has closed.
   *
   * A peer is a member of NativeEditor, and a wrapper keeps that for the
   * life of the plugin instance, so the bitmap of a closed editor stayed
   * allocated for as long as the plugin was in the session -- 6 MB for a
   * 780x500 editor opened once at 200%, in every instance whose editor had
   * ever been looked at. Resizing to zero would not return it, because a
   * vector keeps its capacity; the bitmap is replaced instead. The device
   * size goes back to zero with it, so the next open's setDeviceSize
   * allocates again even when the size is the same, and that open paints
   * in full, as a first one does.
   */
  void release() {
    bitmap_ = Bitmap();
    std::vector<PixelRect>().swap(changed_);
    deviceWidth_ = 0;
    deviceHeight_ = 0;
    heldPixels_ = 0;
    fullRepaintNeeded_ = true;
  }

  // ── Input ────────────────────────────────────────────────────────────────

  /**
   * A pointer position from the platform, in the tree's own units.
   *
   * The one direction that is easy to forget, and silent when it is: at 200%
   * the pointer would be reported at twice the coordinate the control is drawn
   * at, so every click would land on whatever is a screenful down and to the
   * right -- or on nothing, which reads as "the plugin ignores my mouse".
   */
  Point toLogical(int deviceX, int deviceY) const {
    return {(float) deviceX / scale_, (float) deviceY / scale_};
  }

  /** And back, for a peer that has to tell the platform where something is --
   *  a caret rectangle for an input method, a tooltip anchor. */
  Point toDevice(Point logical) const {
    return {logical.x * scale_, logical.y * scale_};
  }

  // ── Painting ─────────────────────────────────────────────────────────────

  /** Make the next render() paint everything, not only the damage. */
  void invalidateAll() { fullRepaintNeeded_ = true; }
  /** Whether the next render() paints everything: after a resize, a new scale,
   *  content or ground, or invalidateAll. */
  bool needsFullRepaint() const { return fullRepaintNeeded_; }

  /**
   * Paint whatever changed, and report the DEVICE rectangles that did.
   *
   * An empty result means nothing moved and the peer should not ask the
   * platform to blit -- which is what stops a 30 Hz editor clock from
   * repainting a still window thirty times a second.
   *
   * The ground under a damaged area has to be cleared before the tree paints
   * over it, or the previous frame shows through anything transparent.
   * Clearing the WHOLE bitmap would undo the saving, so it clears exactly the
   * damaged rectangles -- through the same conversion that produces the
   * rectangles handed back, so the cleared area and the blitted area are the
   * same pixels by construction rather than by two roundings that agree.
   */
  const std::vector<PixelRect>& render() {
    changed_.clear();
    if (!content_ || bitmap_.isEmpty()) return changed_;

    if (fullRepaintNeeded_) {
      Graphics g(bitmap_);
      g.fillAll(ground_);
      g.addTransform(Transform::scaling(scale_, scale_));
      content_->paintTree(g);
      fullRepaintNeeded_ = false;
      content_->clearDamage();
      changed_.push_back(bitmap_.bounds());
      return changed_;
    }

    const RectangleList damage = content_->damage();
    if (damage.isEmpty()) return changed_;

    {
      Graphics g(bitmap_);
      // One damaged rectangle at a time, cleared and then painted with the
      // clip HELD to it. The clip is what keeps a partial render partial:
      // paintTree skips the components the damage misses, but a component it
      // does visit paints all of itself -- and the root is always visited,
      // and a root paints its background over its whole bounds. With the
      // clip at the damage, that fill stays inside it; without, it covered
      // every control the damage had skipped, in the bitmap, where nothing
      // showed it until the platform copied the whole window. Then the
      // editor was one moved knob on an empty background.
      //
      // Overlapping rectangles are safe this way: each pass repaints
      // EVERYTHING that intersects its rectangle, in order, within it, so a
      // pass that clears an overlap also paints the overlap back.
      for (const Rect& r : damage.rects()) {
        const PixelRect device = deviceRectFor(r);
        if (device.isEmpty()) continue;
        Graphics::ScopedState scope(g);
        g.clipTo(device);
        if (g.isClippedOut()) continue;
        g.fillAll(ground_);
        g.addTransform(Transform::scaling(scale_, scale_));
        // The damage handed down is what was CLEARED -- the device rectangle,
        // rounded outward, back in logical units -- not `r` itself. At a
        // fractional scale the outward rounding takes in the edge pixel of a
        // neighbour that `r` does not touch; asked about `r`, paintTree would
        // skip that neighbour and leave its antialiased edge as bare ground.
        RectangleList one;
        one.add(Rect((float) device.x / scale_, (float) device.y / scale_,
                     (float) device.w / scale_, (float) device.h / scale_));
        content_->paintTree(g, one);
      }
    }
    content_->clearDamage();

    for (const Rect& r : damage.rects()) {
      const PixelRect device = deviceRectFor(r);
      if (!device.isEmpty()) changed_.push_back(device);
    }
    return changed_;
  }

  /**
   * A logical rectangle in device pixels, rounded OUTWARD.
   *
   * Outward because a control at a fractional device position antialiases into
   * the pixel beyond it: rounding inward would leave a bright edge of the
   * previous frame along one side of everything that moved, which is the trail
   * a dragged knob leaves behind it.
   */
  PixelRect deviceRectFor(const Rect& logical) const {
    const Rect scaled(logical.x * scale_, logical.y * scale_, logical.w * scale_,
                      logical.h * scale_);
    return PixelRect::enclosing(scaled).intersection(bitmap_.bounds());
  }

private:
  /** The tree is laid out to the LOGICAL size, which is the whole point: a
   *  component never learns what scale it is drawn at. */
  void applyLayout() {
    if (!content_) return;
    content_->setBounds({0.0f, 0.0f, logicalWidth(), logicalHeight()});
    content_->repaint();
  }

  Component* content_ = nullptr;
  Bitmap bitmap_;
  std::vector<PixelRect> changed_;
  Colour ground_ = palette::background();
  float scale_ = 1.0f;
  int deviceWidth_ = 0;
  int deviceHeight_ = 0;
  /** The most pixels the bitmap has been sized for since it was last made
   *  anew: what its buffer holds at least. See setDeviceSize. */
  size_t heldPixels_ = 0;
  bool fullRepaintNeeded_ = true;
};

} // namespace gfx
} // namespace sonore
