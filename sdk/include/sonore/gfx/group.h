// SPDX-License-Identifier: Apache-2.0
//
// A titled box around a set of controls.
//
// ── Why a component rather than a painted rectangle ────────────────────────
//
// Because the alternative is what every editor without one does: draw a line,
// draw a caption, and then lay the controls out to coordinates that have to
// agree with where the line was drawn. Two places holding the same number, and
// the second one is inside a paint() nobody re-reads.
//
// A GroupComponent owns the inside. Its children are positioned relative to it,
// contentBounds() says where they may go, and moving the group moves everything
// in it -- which is the difference between a decoration and a container.
//
// ── The gap in the outline ─────────────────────────────────────────────────
//
// The border is drawn with a gap where the caption sits, rather than the
// caption being drawn over a complete border. Over a solid background those
// look identical; over a gradient, a texture, or anything a LookAndFeel puts
// behind the panel, drawing the caption over the line leaves a rectangle of
// panel colour with the line visible through the letters' gaps.
#pragma once

#include <string>
#include <utility>

#include "widgets.h"

namespace sonore {
namespace gfx {

class GroupComponent : public Widget {
public:
  explicit GroupComponent(std::string title = {}) : title_(std::move(title)) {
    // A group is a frame, not a control. Clicks belong to whatever is inside
    // it, and a group that swallowed them would make its own children unusable
    // wherever they did not cover it completely.
    setInterceptsMouse(false);
  }

  void setTitle(std::string t) {
    if (t == title_) return;
    title_ = std::move(t);
    repaint();
  }

  const std::string& title() const { return title_; }

  /** A Group, named by its caption -- which is the whole point of a caption.
   *  A reader entering a group says its name, so "Filter: cutoff, resonance"
   *  costs one word and replaces two names nobody would otherwise connect. */
  AccessibleInfo accessibleInfo() const override {
    AccessibleInfo info = baseInfo(AccessibleRole::Group);
    if (info.name.empty()) info.name = title_;
    return info;
  }

  /** Corner radius. Zero is a square box, which suits a dense panel. */
  void setCornerRadius(float radius) {
    corner_ = radius < 0.0f ? 0.0f : radius;
    repaint();
  }

  /**
   * Where children may be laid out, in this component's own coordinates.
   *
   * Inset by the border and by the caption's height at the top, so a caller
   * that lays out into this rectangle cannot collide with the title -- which is
   * the arithmetic that goes wrong when the two are separate.
   */
  Rect contentBounds() const {
    const float top = title_.empty() ? kInset : headerHeight();
    const Rect b = localBounds();
    const float w = b.w - kInset * 2.0f;
    const float h = b.h - top - kInset;
    return {kInset, top, w > 0.0f ? w : 0.0f, h > 0.0f ? h : 0.0f};
  }

  /** The height the caption occupies, for a caller sizing a group to fit. */
  float headerHeight() const {
    return title_.empty() ? kInset : font().lineHeight() + kInset;
  }

  void paint(Graphics& g) override {
    const Rect b = localBounds();
    const float lineY = title_.empty() ? 0.5f : font().lineHeight() * 0.5f;
    const Rect frame(0.5f, lineY, b.w - 1.0f, b.h - lineY - 1.0f);
    if (frame.w <= 0.0f || frame.h <= 0.0f) return;

    LookAndFeel& lf = lookAndFeel();
    g.setColour(lf.outline());

    if (title_.empty()) {
      g.drawRoundedRect(frame, corner_, 1.0f);
      return;
    }

    // The caption's width, plus a little air either side, is the gap.
    const float textWidth = font().stringWidth(title_);
    const float gapStart = kInset;
    const float gapEnd = gapStart + textWidth + kGapPadding * 2.0f;

    // Four sides, with the top drawn as two runs around the gap. Drawn as
    // explicit segments rather than as a rounded rectangle with a hole,
    // because there is no such thing and faking one means clipping -- which
    // would also clip whatever a LookAndFeel had drawn behind.
    Path outline;
    outline.moveTo(gapEnd < frame.right() ? gapEnd : frame.right(), frame.y);
    outline.lineTo(frame.right() - corner_, frame.y);
    if (corner_ > 0.0f) outline.quadTo(frame.right(), frame.y, frame.right(), frame.y + corner_);
    outline.lineTo(frame.right(), frame.bottom() - corner_);
    if (corner_ > 0.0f)
      outline.quadTo(frame.right(), frame.bottom(), frame.right() - corner_, frame.bottom());
    outline.lineTo(frame.x + corner_, frame.bottom());
    if (corner_ > 0.0f) outline.quadTo(frame.x, frame.bottom(), frame.x, frame.bottom() - corner_);
    outline.lineTo(frame.x, frame.y + corner_);
    if (corner_ > 0.0f) outline.quadTo(frame.x, frame.y, frame.x + corner_, frame.y);
    outline.lineTo(gapStart, frame.y);
    g.strokePath(outline, 1.0f);

    // The caption, on the line's own row, in the gap that was left for it.
    lf.drawLabel(g, Rect(gapStart + kGapPadding, 0.0f, textWidth, font().lineHeight()), title_,
                 font(), Justify::Left, stateFor(false));
  }

  static constexpr float kInset = 8.0f;
  static constexpr float kGapPadding = 4.0f;

private:
  std::string title_;
  float corner_ = 4.0f;
};

} // namespace gfx
} // namespace sonore
