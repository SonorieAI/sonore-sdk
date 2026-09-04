// SPDX-License-Identifier: Apache-2.0
//
// A window onto something bigger than itself, and the bar that moves it.
//
// ── Why a plugin needs one ──────────────────────────────────────────────────
//
// GenericEditor lays out one row per parameter. A plugin with forty of them
// makes an editor taller than a screen, and a host given that size either
// clips it or refuses it -- either way the last parameters cannot be reached.
// The same is true of any list a plugin wants to show: presets, samples,
// modulation slots.
//
// ── How the scroll actually happens ─────────────────────────────────────────
//
// By moving the viewed component to a NEGATIVE position inside the viewport,
// and letting the component tree do the rest. paintTree already translates by a
// child's bounds and clips to them before recursing, and hitTest already
// subtracts the same offset -- so scrolling needs no special case in either.
// A viewport that reimplemented clipping would be a second set of rules to keep
// in step with the first.
#pragma once

#include <algorithm>
#include <functional>

#include "widgets.h"

namespace sonore {
namespace gfx {

/**
 * A scroll bar.
 *
 * Usable on its own -- a mixer strip wants one without a viewport around it --
 * which is why it takes a range rather than a component.
 */
class ScrollBar : public Widget {
public:
  AccessibleInfo accessibleInfo() const override {
    AccessibleInfo info = baseInfo(AccessibleRole::ScrollBar);
    info.hasRange = true;
    info.minValue = 0.0;
    info.maxValue = (double) maxPosition();
    info.currentValue = (double) position_;
    return info;
  }

  explicit ScrollBar(bool vertical = true) : vertical_(vertical) {}

  bool isVertical() const { return vertical_; }
  void setVertical(bool v) {
    vertical_ = v;
    repaint();
  }

  /**
   * `total` is how much there is, `visible` is how much fits, `position` is
   * where the visible part starts. All in the same units, which for a viewport
   * are pixels.
   */
  void setRange(float total, float visible, float position) {
    total_ = total > 0.0f ? total : 0.0f;
    visible_ = visible > 0.0f ? visible : 0.0f;
    setPosition(position);
  }

  void setPosition(float position, bool notify = true) {
    const float clamped = clampPosition(position);
    if (clamped == position_) return;
    position_ = clamped;
    repaint();
    if (notify && onScroll) onScroll(position_);
  }

  float position() const { return position_; }
  float maxPosition() const { return total_ > visible_ ? total_ - visible_ : 0.0f; }

  /** Nothing to scroll. A bar drawn in this state is a control that cannot be
   *  moved, which reads as broken rather than as "everything fits". */
  bool isRedundant() const { return total_ <= visible_ || visible_ <= 0.0f; }

  std::function<void(float)> onScroll;

  void paint(Graphics& g) override {
    if (isRedundant()) return;
    lookAndFeel().drawScrollBar(g, localBounds(), thumbBounds(), vertical_, hovered_ || dragging_);
  }

  void mouseDown(const MouseEvent& e) override {
    if (isRedundant()) return;
    const Rect thumb = thumbBounds();
    if (thumb.contains(e.position)) {
      dragging_ = true;
      grabOffset_ = (vertical_ ? e.position.y - thumb.y : e.position.x - thumb.x);
      repaint();
      return;
    }
    // A click on the TRACK pages towards it rather than jumping there. Jumping
    // loses the reader's place in a long list; a page keeps the last line of
    // the old view as the first of the new.
    const float here = vertical_ ? e.position.y : e.position.x;
    const float thumbStart = vertical_ ? thumb.y : thumb.x;
    setPosition(position_ + (here < thumbStart ? -visible_ : visible_));
  }

  void mouseDrag(const MouseEvent& e) override {
    if (!dragging_ || isRedundant()) return;
    const Rect area = localBounds();
    const float length = vertical_ ? area.h : area.w;
    const float thumbLength = thumbLengthFor(length);
    const float travel = length - thumbLength;
    if (travel <= 0.0f) return;
    const float at = (vertical_ ? e.position.y : e.position.x) - grabOffset_;
    setPosition(at / travel * maxPosition());
  }

  void mouseUp(const MouseEvent&) override {
    if (!dragging_) return;
    dragging_ = false;
    repaint();
  }

  void mouseEnter(const MouseEvent&) override {
    hovered_ = true;
    repaint();
  }

  void mouseExit(const MouseEvent&) override {
    hovered_ = false;
    repaint();
  }

  bool mouseWheel(const MouseEvent&, float delta) override {
    // A redundant bar does NOT claim the wheel. It is a bar with nothing to
    // scroll, and swallowing the event would stop whatever is around it from
    // scrolling instead.
    if (isRedundant() || delta == 0.0f) return false;
    setPosition(position_ - delta * kWheelStep);
    return true;
  }

  static constexpr float kWheelStep = 48.0f;
  static constexpr float kMinThumb = 18.0f;
  static constexpr float kThickness = 10.0f;

private:
  float clampPosition(float p) const {
    const float top = maxPosition();
    return p < 0.0f ? 0.0f : (p > top ? top : p);
  }

  float thumbLengthFor(float length) const {
    if (total_ <= 0.0f) return length;
    // Proportional, but never shorter than kMinThumb: a thumb four pixels tall
    // on a very long list is a target nobody can grab.
    const float proportional = length * (visible_ / total_);
    return std::max(kMinThumb, std::min(length, proportional));
  }

  Rect thumbBounds() const {
    const Rect area = localBounds();
    const float length = vertical_ ? area.h : area.w;
    const float thumbLength = thumbLengthFor(length);
    const float top = maxPosition();
    const float fraction = top > 0.0f ? position_ / top : 0.0f;
    const float at = fraction * (length - thumbLength);
    return vertical_ ? Rect(area.x, area.y + at, area.w, thumbLength)
                     : Rect(area.x + at, area.y, thumbLength, area.h);
  }

  bool vertical_ = true;
  float total_ = 0.0f, visible_ = 0.0f, position_ = 0.0f;
  float grabOffset_ = 0.0f;
  bool dragging_ = false;
  bool hovered_ = false;
};

/**
 * A clipped window onto a larger component, with bars.
 *
 * The viewed component is NOT owned. A viewport that owned it would decide the
 * lifetime of something a caller usually keeps a pointer to, and every existing
 * container here is non-owning for the same reason.
 */
class Viewport : public Widget {
public:

  Viewport() {
    // Left out of the accessible tree, children kept. A viewport is scrolling
    // machinery: it has no name, no value and nothing to say, and every level
    // of unnamed nesting between a reader and a control is a level the user
    // has to walk down through before reaching anything.
    //
    // Its scroll bars are NOT ignored -- those do have a position, and a reader
    // that cannot say "there is more below" is hiding half the editor.
    setAccessibilityIgnored(true);
    vertical_.setVertical(true);
    horizontal_.setVertical(false);
    addChild(&vertical_);
    addChild(&horizontal_);
    vertical_.onScroll = [this](float y) { setViewPosition(viewX_, y); };
    horizontal_.onScroll = [this](float x) { setViewPosition(x, viewY_); };
  }

  /** Pass null to detach. The component keeps whatever size it had: a viewport
   *  that resized its content to fit would be doing the opposite of its job. */
  void setViewedComponent(Component* viewed) {
    if (viewed_ == viewed) return;
    if (viewed_) removeChild(viewed_);
    viewed_ = viewed;
    if (viewed_) {
      addChild(viewed_);
      // Behind the bars, so a bar is never hidden under content. Children paint
      // in order and hit-test in reverse, so being first is both.
      moveChildToBack(viewed_);
    }
    viewX_ = viewY_ = 0.0f;
    updateBars();
    repaint();
  }

  Component* viewedComponent() const { return viewed_; }

  float viewPositionX() const { return viewX_; }
  float viewPositionY() const { return viewY_; }

  void setViewPosition(float x, float y) {
    const float clampedX = clamp(x, maxScrollX());
    const float clampedY = clamp(y, maxScrollY());
    if (clampedX == viewX_ && clampedY == viewY_) return;
    viewX_ = clampedX;
    viewY_ = clampedY;
    if (viewed_) {
      // The whole mechanism: a negative position, and the component tree's own
      // translate-and-clip does the rest.
      const Rect b = viewed_->bounds();
      viewed_->setBounds({-viewX_, -viewY_, b.w, b.h});
    }
    // notify=false, or the bar's callback comes straight back in here.
    vertical_.setPosition(viewY_, false);
    horizontal_.setPosition(viewX_, false);
    repaint();
  }

  /** Scroll the smallest amount that brings `area` (in the viewed component's
   *  coordinates) into view. What a focused control calls when Tab reaches it
   *  below the fold. */
  void scrollToShow(const Rect& area) {
    const Rect visible = visibleArea();
    float x = viewX_, y = viewY_;
    if (area.y < viewY_) y = area.y;
    else if (area.bottom() > viewY_ + visible.h) y = area.bottom() - visible.h;
    if (area.x < viewX_) x = area.x;
    else if (area.right() > viewX_ + visible.w) x = area.right() - visible.w;
    setViewPosition(x, y);
  }

  /** The content changed size without the viewport doing so. Called after a
   *  re-layout: detaching and reattaching would work and would also throw the
   *  scroll position away, which for a host dragging a resize handle means the
   *  list jumping back to the top on every mouse move. */
  void contentResized() { updateBars(); }

  bool isVerticalBarVisible() const { return !vertical_.isRedundant(); }
  bool isHorizontalBarVisible() const { return !horizontal_.isRedundant(); }

  /** The part of the viewport the content actually gets, bars excluded. */
  Rect visibleArea() const {
    const Rect area = localBounds();
    const float w = area.w - (needsVerticalBar() ? ScrollBar::kThickness : 0.0f);
    const float h = area.h - (needsHorizontalBar() ? ScrollBar::kThickness : 0.0f);
    return {0.0f, 0.0f, w > 0.0f ? w : 0.0f, h > 0.0f ? h : 0.0f};
  }

  void paint(Graphics& g) override { g.fillAll(lookAndFeel().background()); }

  void resized() override { updateBars(); }

  /** The wheel scrolls the CONTENT, wherever inside the viewport it happens.
   *  A wheel that only worked over the bar would be a wheel nobody uses. */
  /** Yes while there is somewhere to go -- the same test mouseWheel makes, so
   *  a control asking "would anything above me scroll?" gets the answer the
   *  wheel would actually produce rather than a different one. */
  bool wouldScroll() const override { return maxScrollY() > 0.0f || maxScrollX() > 0.0f; }

  bool mouseWheel(const MouseEvent&, float delta) override {
    if (delta == 0.0f) return false;
    // A viewport with nothing to scroll passes the wheel on, so a viewport
    // inside a viewport behaves the way people expect: the inner one takes it
    // while it can, and the outer one takes over at the end.
    if (maxScrollY() > 0.0f) {
      setViewPosition(viewX_, viewY_ - delta * ScrollBar::kWheelStep);
      return true;
    }
    if (maxScrollX() > 0.0f) {
      setViewPosition(viewX_ - delta * ScrollBar::kWheelStep, viewY_);
      return true;
    }
    return false;
  }

private:
  static float clamp(float v, float top) { return v < 0.0f ? 0.0f : (v > top ? top : v); }

  float contentWidth() const { return viewed_ ? viewed_->bounds().w : 0.0f; }
  float contentHeight() const { return viewed_ ? viewed_->bounds().h : 0.0f; }

  /**
   * Whether each bar is needed.
   *
   * Asked TOGETHER, because each one steals space from the other: content that
   * fits horizontally by ten pixels stops fitting once a vertical bar appears.
   * Deciding them one at a time gives a viewport that flickers a bar in and out
   * at one particular size, which is a bug people report as "it jitters".
   */
  bool needsVerticalBar() const {
    const Rect area = localBounds();
    if (contentHeight() > area.h) return true;
    // The other bar would take a strip off the bottom; does the content still
    // fit in what is left?
    if (contentWidth() > area.w) return contentHeight() > area.h - ScrollBar::kThickness;
    return false;
  }

  bool needsHorizontalBar() const {
    const Rect area = localBounds();
    if (contentWidth() > area.w) return true;
    if (contentHeight() > area.h) return contentWidth() > area.w - ScrollBar::kThickness;
    return false;
  }

  float maxScrollX() const {
    const float over = contentWidth() - visibleArea().w;
    return over > 0.0f ? over : 0.0f;
  }

  float maxScrollY() const {
    const float over = contentHeight() - visibleArea().h;
    return over > 0.0f ? over : 0.0f;
  }

  void updateBars() {
    const Rect area = localBounds();
    const bool wantV = needsVerticalBar(), wantH = needsHorizontalBar();
    const Rect visible = visibleArea();

    vertical_.setVisible(wantV);
    horizontal_.setVisible(wantH);
    vertical_.setBounds({area.w - ScrollBar::kThickness, 0.0f, ScrollBar::kThickness, visible.h});
    horizontal_.setBounds({0.0f, area.h - ScrollBar::kThickness, visible.w, ScrollBar::kThickness});
    vertical_.setRange(contentHeight(), visible.h, viewY_);
    horizontal_.setRange(contentWidth(), visible.w, viewX_);

    // Re-clamped: shrinking the content, or growing the viewport, can leave the
    // position past the end -- which shows as a blank strip below the last row.
    setViewPosition(viewX_, viewY_);
  }

  Component* viewed_ = nullptr;
  ScrollBar vertical_{true};
  ScrollBar horizontal_{false};
  float viewX_ = 0.0f, viewY_ = 0.0f;
};

} // namespace gfx
} // namespace sonore
