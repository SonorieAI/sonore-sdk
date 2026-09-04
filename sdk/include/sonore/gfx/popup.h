// SPDX-License-Identifier: Apache-2.0
//
// PopupMenu: a list that opens over everything, takes the mouse, and closes on
// the next click wherever it lands.
//
// ── Why this is two objects ─────────────────────────────────────────────────
//
// PopupMenu is a list of items with no window and no platform in it. Content is
// a Component that draws that list and says which item a point is over. Both
// can be checked with a Bitmap and no window at all -- which matters more here
// than anywhere else in the native UI, because a popup is the one control a
// person cannot inspect while it is open: the moment they alt-tab to look at
// it, it dismisses.
//
// The window, the grab and the dismissal live in the peers, and they are the
// only part that needs a display to test.
//
// ── What a popup has to get right ───────────────────────────────────────────
//
// It must sit above everything, including the host's own window, which means a
// top-level override-redirect window rather than a child. It must hold the
// mouse until it is dismissed, or a click meant for the menu goes to whatever
// is behind it. It must close on a click ANYWHERE, including outside itself and
// outside the plugin. And it must fit on the screen: a menu opened near the
// bottom edge that runs off it is a menu whose last items cannot be reached.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "font.h"
#include "lookandfeel.h"
#include "displays.h"
#include "popup_item.h"
#include "widgets.h"

namespace sonore {
namespace gfx {

/**
 * The list itself: no window, no platform, no drawing.
 *
 * `id` is what a caller gets back, and zero is reserved for "dismissed" as it
 * is everywhere -- a menu whose items start at 1 can tell "nothing chosen"
 * from "the first item", and one that starts at 0 cannot.
 */
class PopupMenu {
public:
  static constexpr float kItemHeight = 24.0f;
  static constexpr float kSeparatorHeight = 9.0f;
  static constexpr float kHeaderHeight = 22.0f;
  /** The same numbers the LookAndFeel draws to. Two copies of a layout
   *  constant is a menu whose text does not line up with its ticks. */
  static constexpr float kPaddingX = LookAndFeel::kPopupPadX;
  static constexpr float kPaddingY = 5.0f;
  static constexpr float kTickWidth = LookAndFeel::kPopupTickWidth;
  static constexpr float kMinWidth = 90.0f;

  void addItem(int id, std::string text, bool enabled = true, bool ticked = false) {
    PopupItem item;
    item.id = id;
    item.text = std::move(text);
    item.enabled = enabled;
    item.ticked = ticked;
    items_.push_back(std::move(item));
  }

  /** Ignored when it would be the first row or would follow another: a menu
   *  that opens with a rule across the top, or shows two in a row, is what
   *  happens when items are added conditionally. */
  void addSeparator() {
    if (items_.empty() || items_.back().isSeparator) return;
    PopupItem item;
    item.isSeparator = true;
    item.enabled = false;
    items_.push_back(item);
  }

  void addSectionHeader(std::string text) {
    PopupItem item;
    item.isHeader = true;
    item.enabled = false;
    item.text = std::move(text);
    items_.push_back(std::move(item));
  }

  void clear() { items_.clear(); }
  int numItems() const { return (int) items_.size(); }
  const PopupItem& itemAt(int i) const { return items_[(size_t) i]; }
  bool isEmpty() const { return items_.empty(); }

  float heightOf(int index) const {
    const PopupItem& item = items_[(size_t) index];
    return item.isSeparator ? kSeparatorHeight : (item.isHeader ? kHeaderHeight : kItemHeight);
  }

  float preferredHeight() const {
    float h = kPaddingY * 2.0f;
    for (int i = 0; i < numItems(); ++i) h += heightOf(i);
    return h;
  }

  /** Wide enough for the longest label. A menu that ellipsised its items would
   *  be a menu whose items cannot be told apart, which is the only job it has. */
  float preferredWidth(const Font& font) const {
    float widest = 0.0f;
    for (const PopupItem& item : items_) {
      if (item.isSeparator) continue;
      const float w = font.stringWidth(item.text);
      if (w > widest) widest = w;
    }
    const float total = widest + kTickWidth + kPaddingX * 2.0f;
    return total < kMinWidth ? kMinWidth : total;
  }

  /** The item under a point in the menu's own coordinates, or -1. Separators
   *  and headers are never returned: they are not choices. */
  int indexAt(Point p, float width) const {
    if (p.x < 0.0f || p.x >= width) return -1;
    float y = kPaddingY;
    for (int i = 0; i < numItems(); ++i) {
      const float h = heightOf(i);
      if (p.y >= y && p.y < y + h) {
        const PopupItem& item = items_[(size_t) i];
        if (item.isSeparator || item.isHeader || !item.enabled) return -1;
        return i;
      }
      y += h;
    }
    return -1;
  }

  float topOf(int index) const {
    float y = kPaddingY;
    for (int i = 0; i < index; ++i) y += heightOf(i);
    return y;
  }

  /**
   * Where to put the menu so all of it is on the screen it is opening on.
   *
   * `screen` is a RECTANGLE, not a size, and that is the whole point: on a
   * multi-monitor desktop a screen to the left of or above the primary starts
   * at NEGATIVE coordinates, and every "does it fit" test written against
   * 0..width is wrong there and right on a single monitor -- which is why that
   * version survived until somebody looked.
   *
   * Flipped ABOVE the anchor rather than merely pushed up when it will not fit
   * below: pushing up leaves the menu covering the control that opened it, and
   * the first thing a person does is move the mouse to where they were looking.
   */
  static Point placeOnScreen(float anchorX, float anchorY, float anchorHeight, float menuW,
                             float menuH, const Rect& screen) {
    float x = anchorX;
    float y = anchorY + anchorHeight;

    if (y + menuH > screen.bottom()) {
      const float above = anchorY - menuH;
      y = above >= screen.y ? above : screen.bottom() - menuH;
    }
    if (x + menuW > screen.right()) x = screen.right() - menuW;
    // Clamped to the SCREEN's own origin, not to zero. A menu on a monitor
    // whose x starts at -1920 would otherwise be dragged onto the primary.
    if (x < screen.x) x = screen.x;
    if (y < screen.y) y = screen.y;
    return {x, y};
  }

  /** The screen the anchor is on, worked out for the caller. What almost every
   *  caller wants, and the version that cannot be got wrong. */
  static Point placeOnScreen(float anchorX, float anchorY, float anchorHeight, float menuW,
                             float menuH) {
    const Display& screen = Displays::containing({anchorX, anchorY});
    return placeOnScreen(anchorX, anchorY, anchorHeight, menuW, menuH, screen.workArea);
  }

private:
  std::vector<PopupItem> items_;
};

/**
 * The menu as a Component.
 *
 * Owns nothing: the PopupMenu it draws must outlive it, which the peers arrange
 * by keeping both in the same object.
 */
class PopupContent : public Widget {
public:
  PopupContent(const PopupMenu& menu, Font font) : menu_(menu) { setFont(font); }

  /** The chosen id, or 0 for dismissed -- called exactly once, whichever
   *  happens. A caller that only handles the choice leaks whatever it opened
   *  the menu to decide. */
  std::function<void(int)> onDismiss;

  int highlighted() const { return highlighted_; }

  void paint(Graphics& g) override {
    LookAndFeel& lf = lookAndFeel();
    const Rect area = localBounds();
    lf.drawPopupBackground(g, area);

    float y = PopupMenu::kPaddingY;
    for (int i = 0; i < menu_.numItems(); ++i) {
      const PopupItem& item = menu_.itemAt(i);
      const float h = menu_.heightOf(i);
      const Rect row(0.0f, y, area.w, h);
      if (item.isSeparator) lf.drawPopupSeparator(g, row);
      else lf.drawPopupItem(g, row, item, font(), i == highlighted_);
      y += h;
    }
  }

  void mouseMove(const MouseEvent& e) override {
    const int over = menu_.indexAt(e.position, localBounds().w);
    if (over == highlighted_) return;
    highlighted_ = over;
    repaint();
  }

  void mouseExit(const MouseEvent&) override {
    if (highlighted_ < 0) return;
    highlighted_ = -1;
    repaint();
  }

  /**
   * A press anywhere ends the menu.
   *
   * On the PRESS, not the release. A menu opened by a press-and-hold is closed
   * by the release of that same press in every desktop toolkit, and one that
   * waited for a second release would need the user to click twice.
   */
  void mouseDown(const MouseEvent& e) override {
    const int over = menu_.indexAt(e.position, localBounds().w);
    dismissWith(over >= 0 ? menu_.itemAt(over).id : 0);
  }

  /** Called by a peer when the mouse goes down somewhere else entirely, or the
   *  window loses its grab. */
  void dismissWith(int id) {
    if (dismissed_) return;
    dismissed_ = true;
    if (onDismiss) onDismiss(id);
  }

  bool isDismissed() const { return dismissed_; }

private:
  const PopupMenu& menu_;
  int highlighted_ = -1;
  bool dismissed_ = false;
};

} // namespace gfx
} // namespace sonore
