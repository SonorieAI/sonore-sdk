// SPDX-License-Identifier: Apache-2.0
//
// Dragging an editor's edges, not just the corner in the bottom right.
//
// ── What was here before ────────────────────────────────────────────────────
//
// A ResizableCorner and a BoundsConstrainer, in tooltip.h, used by nothing at
// all. Written, correct as far as they went, and wired into no editor -- so no
// plugin built with this SDK has ever shown the user a grip. The classes
// existed and the capability did not, which is the failure this file is the
// second half of.
//
// ── Why a plugin editor cannot drag every edge ──────────────────────────────
//
// A plugin editor does not own its window. The host does: it decides where the
// frame sits and how big it is, and the only correct thing an editor can do is
// ASK, through clap_gui_request_resize or IPlugFrame::resizeView.
//
// That asymmetry has a consequence people get wrong. Dragging the RIGHT edge
// asks for a wider window and the top-left corner stays put, which the host can
// honour. Dragging the LEFT edge asks for a wider window whose top-left corner
// MOVES -- and a plugin cannot move its own frame. Wired up naively, dragging
// the left edge grows the window to the right, which is the opposite of what
// the hand did.
//
// So the zones are individually enableable, the drag reports an ORIGIN SHIFT
// as well as a size, and a plugin editor turns the left and top edges off while
// a standalone window -- which really does own its frame -- leaves them on.
//
// ── The constrainer and the descriptor must not disagree ────────────────────
//
// The limits the drag enforces and the limits the host was told have to be the
// same numbers, or the user drags to a size the host then refuses and the
// window snaps back under their hand. constrainerFor() builds one from the
// other so there is nowhere for them to drift apart.
#pragma once

#include <functional>

#include "../plugin.h"
#include "tooltip.h"
#include "widgets.h"

namespace sonore {
namespace gfx {

/** Which part of the border the pointer is over. */
enum class ResizeZone {
  None,
  Left,
  Right,
  Top,
  Bottom,
  TopLeft,
  TopRight,
  BottomLeft,
  BottomRight,
};

/**
 * A BoundsConstrainer carrying what the PLUGIN declared.
 *
 * The one place the two constraint models meet. EditorConstraints is what every
 * host is told; BoundsConstrainer is what a drag obeys. Two hand-written copies
 * of the same four numbers is how a user ends up dragging to a size the host
 * refuses, and watching the window snap back.
 */
inline BoundsConstrainer constrainerFor(const EditorConstraints& limits) {
  BoundsConstrainer out;
  out.minWidth = (float) limits.minWidth;
  out.minHeight = (float) limits.minHeight;
  out.maxWidth = (float) limits.maxWidth;
  out.maxHeight = (float) limits.maxHeight;
  // A fixed axis becomes a minimum equal to its maximum, which is what
  // BoundsConstrainer::isFixedSize already means -- so a fixed editor reports
  // itself fixed to anything that asks, without a second flag to keep in step.
  if (!limits.resizableHorizontally) out.minWidth = out.maxWidth;
  if (!limits.resizableVertically) out.minHeight = out.maxHeight;
  return out;
}

/**
 * The draggable frame around an editor.
 *
 * Transparent to the mouse everywhere except its border, so it can sit OVER the
 * interface without stealing clicks from the controls underneath. That is the
 * whole reason it is a border rather than a component behind everything: an
 * editor whose sliders stopped working near the edge would be a worse trade
 * than no resizing at all.
 */
class ResizableBorder : public Widget {
public:
  /** How far in from the edge still counts as a grab. Eight rather than four:
   *  a four-pixel target is one people miss, and missing it means grabbing the
   *  control underneath instead. */
  static constexpr float kBorder = 8.0f;
  /** The square at each corner, where two edges are grabbed at once. */
  static constexpr float kCorner = 18.0f;

  ResizableBorder() {
    // It draws nothing. Anything painted over the edge of every editor would
    // be a design decision imposed on every plugin built with this.
    setInterceptsMouse(true);
    // A frame is not a thing a reader needs told about, and announcing one
    // would put an unnamed level in front of the whole interface.
    setAccessibilityIgnored(true);
  }

  BoundsConstrainer constrainer;

  /**
   * The size the user is asking for, and how far the top-left would have to
   * move to make it look like the edge they grabbed is the one that moved.
   *
   * dx/dy are zero unless a left or top edge was dragged. A plugin editor
   * ignores them because it cannot move its own frame -- and does not enable
   * those edges in the first place, so they stay zero anyway.
   */
  std::function<void(float dx, float dy, float width, float height)> onResize;

  /** Left and top OFF is the plugin-editor case: an editor cannot move its own
   *  frame, so an edge that would have to is an edge that lies. */
  void setDraggableEdges(bool left, bool top, bool right, bool bottom) {
    left_ = left;
    top_ = top;
    right_ = right;
    bottom_ = bottom;
  }

  /** The size the editor is NOW, which a drag is measured from. */
  void setCurrentSize(float width, float height) {
    currentW_ = width;
    currentH_ = height;
  }

  /**
   * Which zone a point is in. Public because it is the whole geometry of this
   * class, and a hit region is far easier to get wrong than to test.
   */
  ResizeZone zoneAt(Point p) const {
    const Rect area = localBounds();
    if (area.isEmpty()) return ResizeZone::None;
    // Corners first: inside a corner square BOTH edges are being grabbed, and
    // testing edges first would make every corner behave as one edge.
    const bool nearLeft = left_ && p.x < kCorner;
    const bool nearRight = right_ && p.x >= area.w - kCorner;
    const bool nearTop = top_ && p.y < kCorner;
    const bool nearBottom = bottom_ && p.y >= area.h - kCorner;
    if (nearLeft && nearTop) return ResizeZone::TopLeft;
    if (nearRight && nearTop) return ResizeZone::TopRight;
    if (nearLeft && nearBottom) return ResizeZone::BottomLeft;
    if (nearRight && nearBottom) return ResizeZone::BottomRight;

    if (left_ && p.x < kBorder) return ResizeZone::Left;
    if (right_ && p.x >= area.w - kBorder) return ResizeZone::Right;
    if (top_ && p.y < kBorder) return ResizeZone::Top;
    if (bottom_ && p.y >= area.h - kBorder) return ResizeZone::Bottom;
    return ResizeZone::None;
  }

  /**
   * Only the border takes the mouse.
   *
   * Everything else falls through to the interface underneath, which is what
   * lets this sit on top of an editor rather than behind it.
   */
  bool hitTestPoint(Point p) const override { return zoneAt(p) != ResizeZone::None; }

  void mouseMove(const MouseEvent& e) override { setCursorForZone(zoneAt(e.position)); }

  void mouseDown(const MouseEvent& e) override {
    zone_ = zoneAt(e.position);
    if (zone_ == ResizeZone::None) return;
    dragStart_ = e.rootPosition;
    startW_ = currentW_;
    startH_ = currentH_;
    dragging_ = true;
  }

  void mouseDrag(const MouseEvent& e) override {
    if (!dragging_) return;
    const float dx = e.rootPosition.x - dragStart_.x;
    const float dy = e.rootPosition.y - dragStart_.y;

    float w = startW_, h = startH_;
    // A left or top drag grows the window by the NEGATIVE of the pointer
    // movement: dragging the left edge leftwards is a smaller x and a wider
    // window.
    switch (zone_) {
      case ResizeZone::Left: w = startW_ - dx; break;
      case ResizeZone::Right: w = startW_ + dx; break;
      case ResizeZone::Top: h = startH_ - dy; break;
      case ResizeZone::Bottom: h = startH_ + dy; break;
      case ResizeZone::TopLeft: w = startW_ - dx; h = startH_ - dy; break;
      case ResizeZone::TopRight: w = startW_ + dx; h = startH_ - dy; break;
      case ResizeZone::BottomLeft: w = startW_ - dx; h = startH_ + dy; break;
      case ResizeZone::BottomRight: w = startW_ + dx; h = startH_ + dy; break;
      case ResizeZone::None: return;
    }

    constrainer.constrain(&w, &h);

    // The origin shift is worked out AFTER constraining, from the size that
    // was actually allowed. Taking it from the raw pointer movement would slide
    // the window sideways while it refused to grow, which is the bug that makes
    // a left-edge drag feel broken at the minimum size.
    float shiftX = 0.0f, shiftY = 0.0f;
    if (zone_ == ResizeZone::Left || zone_ == ResizeZone::TopLeft ||
        zone_ == ResizeZone::BottomLeft)
      shiftX = startW_ - w;
    if (zone_ == ResizeZone::Top || zone_ == ResizeZone::TopLeft ||
        zone_ == ResizeZone::TopRight)
      shiftY = startH_ - h;

    if (w == currentW_ && h == currentH_ && shiftX == 0.0f && shiftY == 0.0f) return;
    currentW_ = w;
    currentH_ = h;
    // Reported on every move rather than on release. A host told only at the
    // end would show the old window for the whole drag, which reads as the
    // grip not working.
    if (onResize) onResize(shiftX, shiftY, w, h);
  }

  void mouseUp(const MouseEvent&) override {
    dragging_ = false;
    zone_ = ResizeZone::None;
  }

private:
  void setCursorForZone(ResizeZone zone) {
    switch (zone) {
      case ResizeZone::Left:
      case ResizeZone::Right: setCursor(MouseCursor::ResizeLeftRight); break;
      case ResizeZone::Top:
      case ResizeZone::Bottom: setCursor(MouseCursor::ResizeUpDown); break;
      case ResizeZone::TopLeft:
      case ResizeZone::BottomRight:
      case ResizeZone::TopRight:
      case ResizeZone::BottomLeft: setCursor(MouseCursor::ResizeCorner); break;
      case ResizeZone::None: setCursor(MouseCursor::Default); break;
    }
  }

  Point dragStart_;
  float currentW_ = 0.0f, currentH_ = 0.0f;
  float startW_ = 0.0f, startH_ = 0.0f;
  ResizeZone zone_ = ResizeZone::None;
  bool dragging_ = false;
  bool left_ = true, top_ = true, right_ = true, bottom_ = true;
};

} // namespace gfx
} // namespace sonore
