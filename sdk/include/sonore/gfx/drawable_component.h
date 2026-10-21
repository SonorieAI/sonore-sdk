// SPDX-License-Identifier: Apache-2.0
//
// DrawableComponent and OwningDrawableComponent: a component that IS a
// picture, scaled into its bounds.
//
// ── Why a component, when Drawable::draw already takes a rectangle ─────────
//
// Drawing an SVG from a parent's paint() is two lines, and for artwork that is
// part of a background it is still the right answer. A component earns its
// place when the picture has to behave like a thing in the tree: be laid out by
// a FlexBox or Grid with everything else, be shown and hidden, be the child a
// ConcertinaPanel or a TabbedComponent holds, sit in front of some controls and
// behind others. ImageComponent already does that for a Bitmap; this is the
// same for a Drawable, with the same placement rules (RectanglePlacement in
// effects2d.h) so the two cannot disagree about what "contain" means.
//
// ── Referencing and owning ─────────────────────────────────────────────────
//
// DrawableComponent REFERENCES its Drawable, like every container in this SDK
// references what it shows: an icon set parsed once is drawn by twenty
// components, and copying it into each would be twenty copies of the same
// shapes. OwningDrawableComponent holds its own, for the one-off picture
// parsed from a string where nobody else needs to keep it alive.
//
// ── Placement ───────────────────────────────────────────────────────────────
//
// By default the picture is fitted the way RectanglePlacement says -- Contain
// and centred unless told otherwise -- against the Drawable's viewBox. The SVG's
// own preserveAspectRatio can be used instead (setUsesDrawablePlacement), which
// is what a designer who set it in the file expects. Stretch is honoured for
// real: the picture is drawn through a non-uniform scale rather than handed to
// Drawable::draw, which would keep the aspect ratio regardless.
#pragma once

#include <memory>
#include <string>

#include "effects2d.h"
#include "svg.h"
#include "widgets.h"

namespace sonore {
namespace gfx {

/**
 * A component that shows a Drawable, fitted into its bounds. See the file
 * comment for when this beats drawing the picture from a parent's paint(),
 * and for the placement rules.
 *
 * The Drawable is REFERENCED: it must outlive this component, or be replaced
 * first with setDrawable. OwningDrawableComponent is the one that keeps its
 * own. Clicks pass through to whatever is behind it. Message thread only.
 */
class DrawableComponent : public Widget {
public:
  /** Shows nothing until setDrawable. Clicks pass through it. */
  DrawableComponent() {
    // A picture is not a control: clicks fall through to what is behind it,
    // as they do for ImageComponent.
    setInterceptsMouse(false);
  }

  /** Shows `drawable`, which is referenced and must outlive this component. */
  explicit DrawableComponent(const Drawable* drawable) : DrawableComponent() {
    drawable_ = drawable;
  }

  /** Referenced, not owned: it must outlive this component, or be replaced
   *  first. Null shows nothing. */
  void setDrawable(const Drawable* drawable) {
    drawable_ = drawable;
    repaint();
  }

  /** What is shown, owned or not. */
  virtual const Drawable* drawable() const { return drawable_; }

  /** How the drawing fits the component's bounds; turns off
   *  setUsesDrawablePlacement. */
  void setPlacement(RectanglePlacement placement) {
    placement_ = placement;
    usesDrawablePlacement_ = false;
    repaint();
  }
  /** How the drawing fits; Contain, centred, by default. */
  const RectanglePlacement& placement() const { return placement_; }

  /** Use the SVG's own preserveAspectRatio instead of setPlacement's. */
  void setUsesDrawablePlacement(bool use) {
    usesDrawablePlacement_ = use;
    repaint();
  }
  /** Whether the SVG's own preserveAspectRatio decides the placement. */
  bool usesDrawablePlacement() const { return usesDrawablePlacement_; }

  /** The colour currentColor is drawn in -- what an icon drawn in
   *  currentColor takes on. Unset, icons keep their own fallback. */
  void setCurrentColour(Colour c) {
    currentColour_ = c;
    hasCurrentColour_ = true;
    repaint();
  }
  /** Back to the icons' own fallback for currentColor. */
  void clearCurrentColour() {
    hasCurrentColour_ = false;
    repaint();
  }

  /** Where the picture's viewBox lands inside `area` under the current
   *  placement, in this component's coordinates. Empty with no picture. */
  Rect drawableArea(const Rect& area) const {
    const Drawable* d = drawable();
    if (!d || d->isEmpty() || area.isEmpty()) return {};
    const Rect box = d->viewBox();
    if (!(box.w > 0.0f && box.h > 0.0f)) return area;
    if (usesDrawablePlacement_) return area;
    return placement_.apply(Rect(0.0f, 0.0f, box.w, box.h), area);
  }

  void paint(Graphics& g) override {
    const Drawable* d = drawable();
    if (!d || d->isEmpty()) return;
    const Rect area = localBounds();
    if (usesDrawablePlacement_) {
      drawInto(g, *d, area);
      return;
    }
    const Rect box = d->viewBox();
    if (!(box.w > 0.0f && box.h > 0.0f)) {
      drawInto(g, *d, area);
      return;
    }
    const Rect target = placement_.apply(Rect(0.0f, 0.0f, box.w, box.h), area);
    if (target.isEmpty()) return;
    // Drawn as the viewBox's own size under a scale to the target -- which is
    // what makes Stretch stretch: Drawable::draw into the target directly
    // would apply the file's aspect rule and letterbox it.
    Graphics::ScopedState state(g);
    g.addTransform(Transform(target.w / box.w, 0.0f, 0.0f, target.h / box.h, target.x, target.y));
    drawInto(g, *d, Rect(0.0f, 0.0f, box.w, box.h));
  }

  AccessibleInfo accessibleInfo() const override {
    // Left out of a reader's tree unless somebody named it: a decorative
    // picture is noise, a named one is an image that means something.
    return baseInfo(AccessibleRole::Label);
  }

private:
  void drawInto(Graphics& g, const Drawable& d, const Rect& area) const {
    if (hasCurrentColour_) d.draw(g, area, currentColour_);
    else d.draw(g, area);
  }

  const Drawable* drawable_ = nullptr;
  RectanglePlacement placement_;
  Colour currentColour_;
  bool hasCurrentColour_ = false;
  bool usesDrawablePlacement_ = false;
};

/** The same component, holding its own Drawable. */
class OwningDrawableComponent : public DrawableComponent {
public:
  /** Shows nothing until setOwnedDrawable. */
  OwningDrawableComponent() = default;

  /** Shows `drawable`, which it keeps. */
  explicit OwningDrawableComponent(Drawable drawable) { setOwnedDrawable(std::move(drawable)); }

  /** Take this one; the previous one is destroyed. */
  void setOwnedDrawable(Drawable drawable) {
    owned_ = std::make_unique<Drawable>(std::move(drawable));
    repaint();
  }

  /** Parse an SVG and keep the result. Returns false -- and keeps showing
   *  whatever it had -- if nothing visible could be parsed. */
  bool setSvg(const std::string& svg, std::string* errorOut = nullptr) {
    Drawable parsed = Drawable::parse(svg);
    if (errorOut) *errorOut = parsed.error();
    if (parsed.isEmpty()) return false;
    setOwnedDrawable(std::move(parsed));
    return true;
  }

  const Drawable* drawable() const override {
    return owned_ ? owned_.get() : DrawableComponent::drawable();
  }

  /** Give the picture up, or null if none is held. */
  std::unique_ptr<Drawable> releaseDrawable() {
    repaint();
    return std::move(owned_);
  }

private:
  std::unique_ptr<Drawable> owned_;
};

} // namespace gfx
} // namespace sonore
