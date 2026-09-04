// SPDX-License-Identifier: Apache-2.0
//
// Tooltips, and the corner you drag to resize an editor.
//
// ── Why tooltips matter more in a plugin ────────────────────────────────────
//
// A plugin has almost no room for words. Every control is a knob with a
// four-letter label, and the difference between "Drive" meaning input gain and
// "Drive" meaning saturation amount is a sentence nobody has space to print. In
// an application you would open a manual; nobody opens a manual for a plugin.
//
// ── Why the clock is passed in ──────────────────────────────────────────────
//
// A tooltip is defined by TIME: it appears after the pointer has been still for
// a while and disappears after a while longer. Reading a clock inside would make
// every one of those rules untestable except by sleeping, and a test that sleeps
// is a test that is slow and flaky at once. So `now` is an argument, the peer
// passes its own clock, and a test passes whatever it likes.
#pragma once

#include <functional>
#include <string>

#include "component.h"
#include "font.h"
#include "lookandfeel.h"
#include "widgets.h"

namespace sonore {
namespace gfx {

/**
 * Decides when a tooltip should be up, and what it should say.
 *
 * Owns no window. The peer that has one asks this what to show and does the
 * showing -- the same split as everywhere else in this UI, and the reason all
 * the timing rules below can be checked with no display at all.
 */
class TooltipManager {
public:
  /** How long the pointer has to be still before one appears. Long enough not
   *  to flash while somebody is moving across the editor, short enough to feel
   *  like an answer rather than a delay. */
  static constexpr double kDelaySeconds = 0.7;
  /** And how long it stays. A tooltip that never left would sit over the
   *  control it describes for as long as the mouse did. */
  static constexpr double kHoldSeconds = 6.0;

  /**
   * Called on every UI tick with the current hover target and the time.
   *
   * Returns true when what should be on screen has CHANGED, so a caller knows
   * to open or close a window rather than doing it every frame.
   */
  bool update(Component* hovered, Point rootPosition, double now) {
    // A move resets the clock. Without that, a tooltip appears mid-drag across
    // the editor for whatever the pointer happened to be passing over.
    const bool moved = std::fabs(rootPosition.x - lastPosition_.x) > kMoveTolerance ||
                       std::fabs(rootPosition.y - lastPosition_.y) > kMoveTolerance;
    if (moved) lastPosition_ = rootPosition;

    std::string wanted = hovered ? tooltipFor(hovered) : std::string();

    if (hovered != target_ || moved || wanted != pending_) {
      target_ = hovered;
      pending_ = std::move(wanted);
      since_ = now;
      // Anything showing goes away the moment the pointer moves to something
      // else, without waiting for the delay again -- a tooltip left over from
      // the previous control is worse than none.
      if (!visible_.empty() && pending_ != visible_) {
        visible_.clear();
        return true;
      }
      return false;
    }

    if (pending_.empty()) return false;

    if (visible_.empty()) {
      if (now - since_ < kDelaySeconds) return false;
      visible_ = pending_;
      shownAt_ = rootPosition;
      shownSince_ = now;
      return true;
    }

    if (now - shownSince_ >= kHoldSeconds) {
      visible_.clear();
      return true;
    }
    return false;
  }

  /** Nothing is hovered any more -- the pointer left the window, or a menu
   *  opened over everything. */
  bool clear() {
    target_ = nullptr;
    pending_.clear();
    if (visible_.empty()) return false;
    visible_.clear();
    return true;
  }

  bool isVisible() const { return !visible_.empty(); }
  const std::string& text() const { return visible_; }
  /** Where the pointer was when it appeared, in root coordinates. It does NOT
   *  follow the pointer: a tooltip that moved would be a thing to chase. */
  Point position() const { return shownAt_; }

  /** The size a window would have to be. Height is one line plus padding,
   *  because a tooltip is one sentence -- anything longer belongs in a manual
   *  nobody is going to read either. */
  Rect boundsFor(const Font& font) const {
    const float w = font.isValid() ? font.stringWidth(visible_) : (float) visible_.size() * 7.0f;
    return {0.0f, 0.0f, w + kPaddingX * 2.0f, font.isValid() ? font.height() + kPaddingY * 2.0f
                                                             : 20.0f};
  }

  static constexpr float kPaddingX = 8.0f;
  static constexpr float kPaddingY = 4.0f;
  /** How far the pointer may drift and still count as still. A mouse held by a
   *  human is never perfectly still, and a tolerance of zero means a tooltip
   *  that only appears for people using a trackpad with their hand off it. */
  static constexpr float kMoveTolerance = 2.0f;

private:
  /** Up the tree, so a knob with no tooltip of its own inherits the one on the
   *  panel around it -- which is how a group of related controls gets one
   *  explanation instead of six. */
  static std::string tooltipFor(Component* c) {
    for (Component* p = c; p != nullptr; p = p->parent()) {
      std::string t = p->tooltip();
      if (!t.empty()) return t;
    }
    return {};
  }

  Component* target_ = nullptr;
  std::string pending_, visible_;
  Point lastPosition_, shownAt_;
  double since_ = 0.0, shownSince_ = 0.0;
};

/**
 * Limits on how big an editor may be, and how it must be shaped.
 *
 * A host resizing a plugin editor is not asking politely -- it sets a size and
 * expects the plugin to cope. The constrainer is what a wrapper answers
 * adjust_size with, so a host learns the limits BEFORE it commits to a window
 * rather than after.
 */
class BoundsConstrainer {
public:
  float minWidth = 1.0f, minHeight = 1.0f;
  /** Zero means unbounded. */
  float maxWidth = 0.0f, maxHeight = 0.0f;
  /** Zero means free. Set it and the height follows the width. */
  float aspectRatio = 0.0f;

  void setSizeLimits(float minW, float minH, float maxW, float maxH) {
    minWidth = minW;
    minHeight = minH;
    maxWidth = maxW;
    maxHeight = maxH;
  }

  /** The nearest size that satisfies every limit. */
  void constrain(float* width, float* height) const {
    float w = *width, h = *height;
    if (w < minWidth) w = minWidth;
    if (h < minHeight) h = minHeight;
    if (maxWidth > 0.0f && w > maxWidth) w = maxWidth;
    if (maxHeight > 0.0f && h > maxHeight) h = maxHeight;

    if (aspectRatio > 0.0f) {
      // The WIDTH leads, because a host dragging a corner usually cares about
      // fitting a rack of a certain width, and because picking one keeps this
      // deterministic. Then re-clamped: forcing the ratio can push the height
      // back out of range, and a constrainer that returned a size violating its
      // own limits would be worse than one with no ratio at all.
      h = w / aspectRatio;
      if (h < minHeight) {
        h = minHeight;
        w = h * aspectRatio;
      }
      if (maxHeight > 0.0f && h > maxHeight) {
        h = maxHeight;
        w = h * aspectRatio;
      }
      if (w < minWidth) w = minWidth;
      if (maxWidth > 0.0f && w > maxWidth) w = maxWidth;
    }

    *width = w;
    *height = h;
  }

  bool isFixedSize() const {
    return maxWidth > 0.0f && maxHeight > 0.0f && maxWidth == minWidth && maxHeight == minHeight;
  }
};

/**
 * The grip in the corner.
 *
 * Reports a WANTED size rather than resizing anything itself. A plugin editor
 * does not own its window -- the host does -- so the only correct thing a grip
 * can do is ask, through clap_gui_request_resize or the VST3 equivalent, and
 * let the host decide.
 */
class ResizableCorner : public Widget {
public:
  static constexpr float kSize = 16.0f;

  /** Called with the size the user is asking for, already constrained. */
  std::function<void(float width, float height)> onResize;

  BoundsConstrainer constrainer;

  ResizableCorner() { setCursor(MouseCursor::ResizeCorner); }

  /** The size the editor is now, which a drag is relative to. */
  void setCurrentSize(float width, float height) {
    currentW_ = width;
    currentH_ = height;
  }

  void mouseDown(const MouseEvent& e) override {
    dragStart_ = e.rootPosition;
    startW_ = currentW_;
    startH_ = currentH_;
    dragging_ = true;
  }

  void mouseDrag(const MouseEvent& e) override {
    if (!dragging_) return;
    float w = startW_ + (e.rootPosition.x - dragStart_.x);
    float h = startH_ + (e.rootPosition.y - dragStart_.y);
    constrainer.constrain(&w, &h);
    // Reported on every move, not on release. A host that only learned the
    // size at the end would show the old window while the user dragged, which
    // reads as the grip not working.
    if (w == currentW_ && h == currentH_) return;
    currentW_ = w;
    currentH_ = h;
    if (onResize) onResize(w, h);
  }

  void mouseUp(const MouseEvent&) override { dragging_ = false; }

  void paint(Graphics& g) override {
    lookAndFeel().drawResizableCorner(g, localBounds(), hovered_ || dragging_);
  }

  void mouseEnter(const MouseEvent&) override {
    hovered_ = true;
    repaint();
  }
  void mouseExit(const MouseEvent&) override {
    hovered_ = false;
    repaint();
  }

private:
  Point dragStart_;
  float currentW_ = 0.0f, currentH_ = 0.0f;
  float startW_ = 0.0f, startH_ = 0.0f;
  bool dragging_ = false;
  bool hovered_ = false;
};

} // namespace gfx
} // namespace sonore
