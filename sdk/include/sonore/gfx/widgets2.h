// SPDX-License-Identifier: Apache-2.0
//
// A range slider, and a button with a picture on it.
//
// ── Why a range is not two sliders ──────────────────────────────────────────
//
// A crossover's band edges, a sample's start and end, a compressor's range: all
// of them are a PAIR that must stay ordered. Two ordinary sliders side by side
// let a user drag the low end past the high one, and every plugin that does
// that has a bug report about it. Keeping the pair ordered is the entire reason
// this is one control.
//
// ── Why an icon button waited for SVG ───────────────────────────────────────
//
// Because until there was a way to describe a shape that stays sharp at any
// size, an icon button meant a bitmap -- and a plugin editor is resized by the
// host to whatever it likes.
#pragma once

#include <functional>
#include <memory>
#include <string>

#include "bitmap.h"
#include "effects2d.h"
#include "svg.h"
#include "widgets.h"

namespace sonore {
namespace gfx {

/**
 * Two values on one track.
 *
 * Both are normalised 0..1, like Slider, so the same ParamInfo curves apply to
 * each end.
 */
class RangeSlider : public Widget {
public:
  static constexpr float kThumb = 12.0f;
  /** The closest the two ends may come. Zero would let them land on the same
   *  value, where neither can be grabbed again because they are on top of each
   *  other. */
  static constexpr float kMinimumGap = 0.001f;

  RangeSlider() { setCursor(MouseCursor::DragHorizontal); }

  std::function<void(float low, float high)> onRangeChange;
  std::function<void()> onDragStart;
  std::function<void()> onDragEnd;

  float lowValue() const { return low_; }
  float highValue() const { return high_; }

  void setRange(float low, float high, bool notify = true) {
    float a = clamp01(low), b = clamp01(high);
    // Swapped rather than refused. A caller setting them the wrong way round
    // means a range, and rejecting it would leave the control showing the old
    // one with no indication why.
    if (a > b) {
      const float t = a;
      a = b;
      b = t;
    }
    if (b - a < kMinimumGap) b = a + kMinimumGap > 1.0f ? 1.0f : a + kMinimumGap;
    if (a == low_ && b == high_) return;
    low_ = a;
    high_ = b;
    repaint();
    if (notify && onRangeChange) onRangeChange(low_, high_);
  }

  void paint(Graphics& g) override {
    lookAndFeel().drawRangeSlider(g, localBounds(), low_, high_, stateFor(dragging_ != Drag::None));
  }

  void mouseDown(const MouseEvent& e) override {
    if (!isEnabled()) return;
    const float at = positionToValue(e.position.x);
    const float lowX = valueToPosition(low_), highX = valueToPosition(high_);

    // Whichever thumb is NEARER, and the middle drags both. Picking by "is it
    // left of the low thumb" instead makes the region between them dead.
    const float toLow = std::fabs(e.position.x - lowX);
    const float toHigh = std::fabs(e.position.x - highX);
    if (toLow <= kThumb && toLow <= toHigh) dragging_ = Drag::Low;
    else if (toHigh <= kThumb) dragging_ = Drag::High;
    else if (at > low_ && at < high_) dragging_ = Drag::Both;
    else dragging_ = at < low_ ? Drag::Low : Drag::High;

    grabOffset_ = at;
    grabLow_ = low_;
    grabHigh_ = high_;
    if (onDragStart) onDragStart();
    applyDrag(at);
  }

  void mouseDrag(const MouseEvent& e) override {
    if (!isEnabled() || dragging_ == Drag::None) return;
    applyDrag(positionToValue(e.position.x));
  }

  void mouseUp(const MouseEvent&) override {
    if (dragging_ == Drag::None) return;
    dragging_ = Drag::None;
    if (onDragEnd) onDragEnd();
    repaint();
  }

private:
  enum class Drag { None, Low, High, Both };

  static float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

  float valueToPosition(float v) const {
    const Rect b = localBounds();
    const float span = b.w - kThumb;
    return b.x + kThumb * 0.5f + v * (span > 0.0f ? span : 0.0f);
  }

  float positionToValue(float x) const {
    const Rect b = localBounds();
    const float span = b.w - kThumb;
    if (span <= 0.0f) return 0.0f;
    return clamp01((x - b.x - kThumb * 0.5f) / span);
  }

  void applyDrag(float at) {
    switch (dragging_) {
      case Drag::Low:
        // Pushed against the other end rather than through it. A range whose
        // ends can cross is the bug this control exists to prevent.
        setRange(std::min(at, high_ - kMinimumGap), high_);
        break;
      case Drag::High:
        setRange(low_, std::max(at, low_ + kMinimumGap));
        break;
      case Drag::Both: {
        // The whole range moves, keeping its WIDTH -- and stops at the ends
        // rather than being squashed against them.
        const float width = grabHigh_ - grabLow_;
        float newLow = grabLow_ + (at - grabOffset_);
        if (newLow < 0.0f) newLow = 0.0f;
        if (newLow + width > 1.0f) newLow = 1.0f - width;
        setRange(newLow, newLow + width);
        break;
      }
      default:
        break;
    }
  }

  float low_ = 0.0f, high_ = 1.0f;
  float grabOffset_ = 0.0f, grabLow_ = 0.0f, grabHigh_ = 1.0f;
  Drag dragging_ = Drag::None;
};

/**
 * A button whose face is a shape rather than a word.
 *
 * Takes SVG source rather than a Path, because that is how icons arrive -- from
 * a designer, from an icon set, as text a generated plugin can embed in a
 * header. The parse happens once at setIcon, not per frame.
 */
class IconButton : public Widget {
public:
  IconButton() { setCursor(MouseCursor::Pointing); }

  std::function<void()> onClick;

  /** Parsed once and kept. A button re-parsing its icon thirty times a second
   *  would be the most expensive thing in an editor. */
  void setIcon(const std::string& svg) {
    icon_ = Drawable::parse(svg);
    repaint();
  }

  const Drawable& icon() const { return icon_; }
  bool hasIcon() const { return !icon_.isEmpty(); }

  void setToggleable(bool canToggle) { toggleable_ = canToggle; }
  bool isToggled() const { return toggled_; }
  void setToggled(bool on, bool notify = false) {
    if (on == toggled_) return;
    toggled_ = on;
    repaint();
    if (notify && onClick) onClick();
  }

  /** How much of the button the icon occupies. The rest is the hit area, which
   *  is why a small icon can still have a comfortable target. */
  void setIconInset(float inset) {
    inset_ = inset < 0.0f ? 0.0f : inset;
    repaint();
  }

  void paint(Graphics& g) override {
    LookAndFeel& lf = lookAndFeel();
    const WidgetState state = stateFor(pressed_);
    lf.drawIconButtonBackground(g, localBounds(), state, toggled_);
    if (!icon_.isEmpty()) icon_.draw(g, localBounds().reduced(inset_));
  }

  void mouseDown(const MouseEvent&) override {
    if (!isEnabled()) return;
    pressed_ = true;
    repaint();
  }

  void mouseUp(const MouseEvent& e) override {
    if (!isEnabled() || !pressed_) return;
    pressed_ = false;
    repaint();
    // Releasing OUTSIDE cancels, which is what every button on every desktop
    // does and the only way to change your mind after pressing.
    if (!localBounds().contains(e.position)) return;
    if (toggleable_) toggled_ = !toggled_;
    if (onClick) onClick();
  }

  void mouseEnter(const MouseEvent&) override {
    hovered_ = true;
    repaint();
  }
  void mouseExit(const MouseEvent&) override {
    hovered_ = false;
    pressed_ = false;
    repaint();
  }

private:
  WidgetState stateFor(bool pressed) const {
    WidgetState s;
    s.hovered = hovered_;
    s.pressed = pressed;
    s.toggled = toggled_;
    s.enabled = isEnabled();
    return s;
  }

  Drawable icon_;
  float inset_ = 6.0f;
  bool pressed_ = false, hovered_ = false, toggled_ = false, toggleable_ = false;
};


/**
 * A picture.
 *
 * Trivial, and missing until now, which meant every plugin wanting to show its
 * own logo had to subclass Component and write a paint(). That is four lines
 * anybody can write and four lines everybody writes slightly differently --
 * usually without the placement rule, so the logo stretches when the editor is
 * resized.
 *
 * The image is NOT owned. A plugin's artwork is decoded once and drawn by
 * however many components want it, and a component that took a copy would
 * decode a megabyte of PNG per instance.
 */
class ImageComponent : public Widget {
public:
  ImageComponent() {
    // A picture is not a control. Clicks pass through to whatever is behind,
    // which is what makes a logo safe to put over a panel.
    setInterceptsMouse(false);
  }

  void setImage(const Bitmap* image) {
    image_ = image;
    repaint();
  }

  const Bitmap* image() const { return image_; }

  /** How the picture fits its box. Contain by default -- the one that never
   *  distorts, which for a logo is the only acceptable answer. */
  void setPlacement(RectanglePlacement placement) {
    placement_ = placement;
    repaint();
  }

  void paint(Graphics& g) override {
    if (!image_ || image_->isEmpty()) return;
    const Rect source(0.0f, 0.0f, (float) image_->width(), (float) image_->height());
    g.drawImage(*image_, placement_.apply(source, localBounds()));
  }

  AccessibleInfo accessibleInfo() const override {
    AccessibleInfo info = baseInfo(AccessibleRole::Label);
    // Left OUT of the tree unless somebody named it. A decorative image with no
    // name is noise to a reader; one with a name is a picture that means
    // something, and the caller is the only one who knows which this is.
    return info;
  }

private:
  const Bitmap* image_ = nullptr;
  RectanglePlacement placement_;
};

/**
 * A button that looks like a link.
 *
 * "Manual", "Report a bug", "Buy the full version". Every plugin has one or two
 * and every one of them was a Button restyled by hand.
 *
 * It does NOT open the URL itself, and that is deliberate rather than lazy.
 * Launching a browser from inside a plugin means ShellExecute or its
 * equivalents, from a process the user did not start and a thread the host
 * owns -- and a host that is mid-render when a shell call blocks is a host
 * that drops audio. The owner gets the URL and decides; a standalone can open
 * it immediately, and a plugin can put it behind a confirmation.
 */
class HyperlinkButton : public Widget {
public:
  HyperlinkButton() {
    setWantsKeyboardFocus(true);
    setCursor(MouseCursor::Pointing);
  }

  void setText(std::string text) {
    text_ = std::move(text);
    repaint();
  }
  const std::string& text() const { return text_; }

  void setUrl(std::string url) { url_ = std::move(url); }
  const std::string& url() const { return url_; }

  /** What to do about it. Given the URL, so a caller can open it, copy it, or
   *  ask first. */
  std::function<void(const std::string&)> onFollow;

  void paint(Graphics& g) override {
    LookAndFeel& lf = lookAndFeel();
    g.setColour(lf.accent());
    const Rect b = localBounds();
    lf.drawLabel(g, b, text_, font(), Justify::Left, stateFor(false));
    // Underlined only while hovered, which is the convention a link follows on
    // every platform and the only thing distinguishing it from coloured text.
    if (hovered_) {
      const float width = font().stringWidth(text_);
      const float baseline = b.y + b.h * 0.5f + font().lineHeight() * 0.35f;
      g.setColour(lf.accent());
      g.fillRect(Rect(b.x, baseline, width, 1.0f));
    }
    paintFocusRing(g);
  }

  void mouseEnter(const MouseEvent&) override {
    hovered_ = true;
    repaint();
  }

  void mouseExit(const MouseEvent&) override {
    hovered_ = false;
    repaint();
  }

  void mouseUp(const MouseEvent& e) override {
    if (!isEnabled() || !localBounds().contains(e.position)) return;
    follow();
  }

  bool keyPressed(const KeyPress& key) override {
    if (!isEnabled()) return false;
    if (!key.is(KeyPress::Return) && key.character != (uint32_t) ' ') return false;
    follow();
    return true;
  }

  AccessibleInfo accessibleInfo() const override {
    AccessibleInfo info = baseInfo(AccessibleRole::Button);
    if (info.name.empty()) info.name = text_;
    // The URL as the description, so a reader can say where it goes. A link
    // announced only by its text is one a user has to follow to find out.
    if (info.description.empty()) info.description = url_;
    return info;
  }

private:
  void follow() {
    if (onFollow) onFollow(url_);
  }

  std::string text_, url_;
  bool hovered_ = false;
};

} // namespace gfx
} // namespace sonore
