// SPDX-License-Identifier: Apache-2.0
//
// A PopupMenu shown INSIDE the editor, and driven from the keyboard.
//
// ── Why there is a second way to show a menu ────────────────────────────────
//
// native_editor.h's PopupWindow puts a menu in a real top-level window, which
// is right for a ComboBox: the menu may hang past the editor's edge, and the
// peer holds the grab that closes it. It is the wrong shape for a menu BAR. A
// bar's menus are entered with F10 and walked with the arrows -- Left and
// Right move to the neighbouring menu, which means the bar has to hear keys
// while its menu is open -- and moving the mouse along the bar with a menu
// open switches menus without a click. A separate window with its own grab
// makes both of those a conversation between two peers.
//
// So this is alert.h's arrangement instead: an Overlay over the editor's root
// that swallows every event, with the list drawn where the menu goes. The
// owner (a menu bar, a toolbar's overflow, a file field's recent list) hears
// what happens OUTSIDE the list through three callbacks, which is all a bar
// needs to switch menus.
//
// The cost is the one alert.h names: an overlay cannot extend past the
// editor's edge. A menu taller than the editor scrolls instead of running off
// it, so its last items stay reachable -- which is the rule popup.h's
// placeOnScreen exists to keep.
//
// ── Choosing on release ─────────────────────────────────────────────────────
//
// A menu opened by pressing a bar item and dragged down is chosen by letting
// go over an item, in every desktop toolkit. The press belongs to the bar --
// the router captured it before the menu existed -- so the bar forwards the
// drag and the release here (trackPointer, releasePointer). A release over
// nothing leaves the menu open, which is what turns the same press into a
// click-to-open.
#pragma once

#include <functional>
#include <string>
#include <utility>

#include "alert.h"
#include "popup.h"

namespace sonore {
namespace gfx {

namespace detail {

/** Whether `target` is still somewhere in `root`'s tree. Compares addresses
 *  and never dereferences `target`, so it is safe to ask about a pointer that
 *  may have been destroyed since it was taken -- a destroyed component leaves
 *  its parent's list on the way out. What makes "give the keyboard back to
 *  whoever had it" safe when the menu was up long enough for that to go. */
inline bool overlayTreeContains(const Component* root, const Component* target) {
  if (!root || !target) return false;
  if (root == target) return true;
  for (const Component* c : root->children())
    if (overlayTreeContains(c, target)) return true;
  return false;
}

} // namespace detail

/**
 * A PopupMenu drawn inside the editor, over its root, and walked from the
 * keyboard -- what a menu bar opens, where a ComboBox uses a real popup
 * window. The file comment says why there are two, and how choosing on
 * release works.
 *
 * The OWNER keeps one of these as a member and calls show() against itself;
 * the overlay adds itself to the owner's root while open and takes itself
 * out on dismiss(), so it must outlive every menu it shows. The answer
 * arrives on the show() call's handler, exactly once, after the overlay is
 * down. It cannot reach past the editor's edge: a long menu scrolls. Message
 * thread only.
 */
class PopupMenuOverlay : public Overlay {
public:
  // Not "Below" and "Above": X.h defines both as macros (window stacking
  // order), and the Linux build of any editor that also includes the X11
  // peer would read Placement::Below as Placement::1.
  enum class Placement {
    /** Under the anchor, or above it when there is no room below. */
    Under,
    /** To the right of the anchor, or its left: a vertical toolbar's overflow. */
    Beside,
  };

  /** Closed until show(). Not dimmed: only the list is drawn. Escape,
   *  through Overlay's onDismiss, closes it with 0. */
  PopupMenuOverlay() {
    setDimAlpha(0);
    setWantsKeyboardFocus(true);
    onDismiss = [this]() { dismiss(0); };
  }

  /**
   * Open `menu` against `anchor`, a rectangle in `owner`'s coordinates.
   *
   * The overlay attaches itself to owner's ROOT and covers it, so the menu can
   * be drawn anywhere in the editor and a press anywhere closes it. The menu
   * is COPIED: the owner usually builds it on the spot and lets it go.
   *
   * `onChosen` runs exactly once, with the chosen id or 0 -- after the overlay
   * is already down, so a handler that opens another menu opens it cleanly.
   * With `highlightFirst` the first item starts lit, which is what a menu
   * opened from the keyboard wants: the next Return chooses something.
   *
   * Shown again while open, the new menu REPLACES the old one and the old
   * handler is dropped uncalled. Only the owner holding this overlay can do
   * that, and it is switching menus rather than dismissing one -- a bar
   * sliding from File to Edit must not report "nothing chosen" in between.
   */
  void show(Component& owner, const PopupMenu& menu, const Rect& anchor,
            std::function<void(int)> onChosen, Placement placement = Placement::Under,
            bool highlightFirst = false) {
    menu_ = menu;
    onChosen_ = std::move(onChosen);
    owner_ = &owner;
    highlighted_ = -1;
    scroll_ = 0.0f;
    if (Widget* w = owner.asWidget()) {
      setFont(w->font());
      setLookAndFeel(&w->lookAndFeel());
    }

    Component* host = owner.root();
    if (!isOpen()) {
      MouseRouter* router = routerFor(&owner);
      focusBefore_ = router ? router->focused() : nullptr;
    }
    if (parent() != host) host->addChild(this);
    else toFront();
    coverParent();

    const Point topLeft = owner.localToRoot({anchor.x, anchor.y});
    anchor_ = Rect(topLeft.x, topLeft.y, anchor.w, anchor.h);
    placeMenu(placement);
    if (highlightFirst) moveHighlight(+1, -1);
    setVisible(true);
    if (MouseRouter* r = routerFor(this)) r->setFocus(this);
    repaint();
  }

  /** Between show() and dismiss(): shown, and attached to the owner's root. */
  bool isOpen() const { return isVisible() && parent() != nullptr; }

  /** Close with an answer: an item's id, or 0 for nothing. Safe to call when
   *  already closed. */
  void dismiss(int id) {
    if (!isVisible()) return;
    // Asked BEFORE hiding, which takes the keyboard away from the menu.
    MouseRouter* router = routerFor(this);
    const bool hadFocus = router && router->focused() == this;
    setVisible(false);
    Component* owner = owner_;
    owner_ = nullptr;
    Component* host = parent();
    Component* before = focusBefore_;
    focusBefore_ = nullptr;
    if (host) host->removeChild(this);
    // The owner gets the keyboard back if it can hold it -- a menu bar entered
    // with F10 keeps walking with the arrows after Escape. Otherwise whoever
    // had it before the menu opened does, if they are still in the tree: a
    // field's recent-files list opened from the keyboard must not leave the
    // keyboard nowhere when it closes.
    if (router && hadFocus && host) {
      // Both asked about by address first: an overlay held apart from its
      // owner can outlive it, and a pointer to a destroyed owner is not one
      // to call wantsKeyboardFocus on.
      const Component* top = host->root();
      if (owner && detail::overlayTreeContains(top, owner) && owner->wantsKeyboardFocus())
        router->setFocus(owner);
      else if (before != this && detail::overlayTreeContains(top, before))
        router->setFocus(before);
    }
    // Moved out before it runs: the handler may show this overlay again,
    // which replaces onChosen_ -- the function that is executing.
    std::function<void(int)> handler = std::move(onChosen_);
    onChosen_ = nullptr;
    if (handler) handler(id);
  }

  /** The copy show() made of the menu; still the last one after it closes. */
  const PopupMenu& menu() const { return menu_; }
  /** The 0-based index of the lit item, or -1 for none. */
  int highlighted() const { return highlighted_; }
  /** The id of the lit item, or 0. */
  int highlightedId() const {
    return highlighted_ >= 0 && highlighted_ < menu_.numItems() ? menu_.itemAt(highlighted_).id : 0;
  }

  /** Light item `index` (0-based) and scroll it into view. -1, an index past
   *  the end, or a separator, header or disabled item lights nothing. */
  void setHighlighted(int index) {
    if (index >= 0 && (index >= menu_.numItems() || !isChoosable(index))) index = -1;
    if (index == highlighted_) return;
    highlighted_ = index;
    scrollToShow(index);
    repaint();
  }

  /** Where the list is, in the overlay's (the root's) coordinates. */
  Rect menuBounds() const { return menuBounds_; }
  /** How far the list is scrolled, in logical units: 0 unless the menu is
   *  taller than the root it is shown in. */
  float scrollOffset() const { return scroll_; }

  /** Where item `index` is, in the overlay's coordinates (scrolled); empty
   *  out of range. For a test that wants to click an item without repeating
   *  the layout. */
  Rect itemBounds(int index) const {
    if (index < 0 || index >= menu_.numItems()) return {};
    return {menuBounds_.x, menuBounds_.y + menu_.topOf(index) - scroll_, menuBounds_.w,
            menu_.heightOf(index)};
  }

  // ── What happens outside the list, for the owner ─────────────────────────

  /** A press outside the list, in root coordinates. Return true to say the
   *  owner dealt with it (a bar switching menus); false closes the menu. */
  std::function<bool(Point)> onPressOutside;
  /** The pointer moving outside the list, in root coordinates: how a bar
   *  follows the mouse from one name to the next with a menu open. */
  std::function<void(Point)> onMoveOutside;
  /** Left (-1) or Right (+1) with the menu open. Return true if the owner
   *  used it; a bar opens the neighbouring menu. */
  std::function<bool(int direction)> onSideKey;

  /** The owner's press, still held, moving: lights whatever is under it. */
  void trackPointer(Point rootPos) {
    if (!isOpen()) return;
    const int over = itemAt(rootPos);
    if (over >= 0 || menuBounds_.contains(rootPos)) setHighlighted(over);
    else if (onMoveOutside) onMoveOutside(rootPos);
  }

  /** The owner's press let go. Over an item, that item is chosen; anywhere
   *  else the menu stays open, which is a click-to-open. */
  void releasePointer(Point rootPos) {
    if (!isOpen()) return;
    const int over = itemAt(rootPos);
    if (over >= 0) dismiss(menu_.itemAt(over).id);
  }

  // ── Events ───────────────────────────────────────────────────────────────

  /** The list at menuBounds(), drawn by the look-and-feel's popup methods;
   *  the rest of the overlay is clear. */
  void paint(Graphics& g) override {
    Overlay::paint(g);
    if (menuBounds_.isEmpty()) return;
    LookAndFeel& lf = lookAndFeel();
    Graphics::ScopedState scope(g);
    g.clipTo(menuBounds_);
    g.translate(menuBounds_.x, menuBounds_.y);
    const Rect local(0.0f, 0.0f, menuBounds_.w, menuBounds_.h);
    lf.drawPopupBackground(g, local);
    for (int i = 0; i < menu_.numItems(); ++i) {
      const float top = menu_.topOf(i) - scroll_;
      const float h = menu_.heightOf(i);
      if (top + h < 0.0f || top > local.h) continue;
      const Rect row(0.0f, top, local.w, h);
      const PopupItem& item = menu_.itemAt(i);
      if (item.isSeparator) lf.drawPopupSeparator(g, row);
      else lf.drawPopupItem(g, row, item, font(), i == highlighted_);
    }
  }

  /** As trackPointer(): lights the item under the pointer, and reports a
   *  move outside the list to onMoveOutside. */
  void mouseMove(const MouseEvent& e) override { trackPointer(e.position); }

  /** A drag over the list lights the item under the pointer. */
  void mouseDrag(const MouseEvent& e) override {
    if (menuBounds_.contains(e.position)) setHighlighted(itemAt(e.position));
  }

  /** In the list: lights the item, which the release chooses. Outside it:
   *  offered to onPressOutside, and the menu closes with 0 unless that
   *  returns true. */
  void mouseDown(const MouseEvent& e) override {
    if (menuBounds_.contains(e.position)) {
      // Lit on the press, chosen on the release -- a press that slides off
      // the item before letting go is a change of mind.
      setHighlighted(itemAt(e.position));
      return;
    }
    if (onPressOutside && onPressOutside(e.position)) return;
    dismiss(0);
  }

  /** Chooses the item under the pointer when it is the lit one; any other
   *  release leaves the menu open. */
  void mouseUp(const MouseEvent& e) override {
    if (!isOpen()) return;
    const int over = itemAt(e.position);
    if (over >= 0 && over == highlighted_) dismiss(menu_.itemAt(over).id);
  }

  bool mouseWheel(const MouseEvent&, float delta) override {
    if (overflowHeight() > 0.0f && delta != 0.0f) {
      scroll_ -= delta * PopupMenu::kItemHeight;
      clampScroll();
      repaint();
    }
    return true;
  }

  bool keyPressed(const KeyPress& key) override {
    if (!isOpen()) return false;
    if (key.is(KeyPress::Escape)) {
      dismiss(0);
      return true;
    }
    if (key.is(KeyPress::Down)) moveHighlight(+1, highlighted_);
    else if (key.is(KeyPress::Up)) moveHighlight(-1, highlighted_);
    else if (key.is(KeyPress::Home)) moveHighlight(+1, -1);
    else if (key.is(KeyPress::End)) moveHighlight(-1, menu_.numItems());
    else if (key.is(KeyPress::Left) || key.is(KeyPress::Right)) {
      if (onSideKey) onSideKey(key.is(KeyPress::Left) ? -1 : +1);
    } else if (key.is(KeyPress::Return) || (key.character == ' ' && !key.ctrlDown)) {
      if (highlighted_ >= 0) dismiss(menu_.itemAt(highlighted_).id);
    } else if (key.isCharacter() && !key.ctrlDown && !key.altDown) {
      typeAhead(key.character);
    }
    // Everything is swallowed while a menu is up, as Overlay does: a shortcut
    // reaching the editor underneath an open menu is a second action from
    // one keystroke.
    return true;
  }

  AccessibleInfo accessibleInfo() const override {
    AccessibleInfo info = baseInfo(AccessibleRole::List);
    if (info.name.empty()) info.name = "menu";
    if (menu_.numItems() > 0) {
      info.hasRange = true;
      info.minValue = 0.0;
      info.maxValue = (double) menu_.numItems() - 1.0;
      info.currentValue = (double) (highlighted_ < 0 ? 0 : highlighted_);
      if (highlighted_ >= 0) info.value = menu_.itemAt(highlighted_).text;
    }
    return info;
  }

private:
  bool isChoosable(int index) const {
    const PopupItem& item = menu_.itemAt(index);
    return !item.isSeparator && !item.isHeader && item.enabled;
  }

  /** The choosable item under a point in the overlay's coordinates, or -1. */
  int itemAt(Point p) const {
    if (!menuBounds_.contains(p)) return -1;
    return menu_.indexAt({p.x - menuBounds_.x, p.y - menuBounds_.y + scroll_}, menuBounds_.w);
  }

  /** The next choosable item from `from` in `direction`, wrapping once. */
  void moveHighlight(int direction, int from) {
    const int n = menu_.numItems();
    if (n == 0) return;
    int i = from;
    for (int step = 0; step < n; ++step) {
      i += direction;
      if (i < 0) i = n - 1;
      if (i >= n) i = 0;
      if (isChoosable(i)) {
        setHighlighted(i);
        return;
      }
    }
  }

  /** The next item whose text starts with this letter, after the lit one --
   *  so pressing it again walks through every item with that initial. */
  void typeAhead(uint32_t c) {
    auto lower = [](uint32_t ch) { return (ch >= 'A' && ch <= 'Z') ? ch - 'A' + 'a' : ch; };
    const int n = menu_.numItems();
    for (int step = 1; step <= n; ++step) {
      const int i = ((highlighted_ < 0 ? -1 : highlighted_) + step + n) % n;
      const std::string& text = menu_.itemAt(i).text;
      if (!isChoosable(i) || text.empty()) continue;
      if (lower((uint32_t) (unsigned char) text[0]) == lower(c)) {
        setHighlighted(i);
        return;
      }
    }
  }

  void placeMenu(Placement placement) {
    const Rect area = localBounds();
    float w = menu_.preferredWidth(font());
    if (placement == Placement::Under && w < anchor_.w) w = anchor_.w;
    if (w > area.w) w = area.w;
    float h = menu_.preferredHeight();
    if (h > area.h) h = area.h;

    float x, y;
    if (placement == Placement::Under) {
      x = anchor_.x;
      y = anchor_.bottom();
      if (y + h > area.bottom()) y = (anchor_.y - h >= area.y) ? anchor_.y - h : area.bottom() - h;
    } else {
      x = anchor_.right();
      if (x + w > area.right()) x = anchor_.x - w;
      y = anchor_.y;
      if (y + h > area.bottom()) y = area.bottom() - h;
    }
    if (x + w > area.right()) x = area.right() - w;
    if (x < area.x) x = area.x;
    if (y < area.y) y = area.y;
    menuBounds_ = Rect(x, y, w, h);
  }

  float overflowHeight() const { return menu_.preferredHeight() - menuBounds_.h; }

  void clampScroll() {
    const float most = overflowHeight();
    if (!(scroll_ <= most)) scroll_ = most;
    if (!(scroll_ >= 0.0f)) scroll_ = 0.0f;
  }

  void scrollToShow(int index) {
    if (index < 0 || overflowHeight() <= 0.0f) return;
    const float top = menu_.topOf(index);
    const float bottom = top + menu_.heightOf(index);
    if (top - PopupMenu::kPaddingY < scroll_) scroll_ = top - PopupMenu::kPaddingY;
    else if (bottom + PopupMenu::kPaddingY > scroll_ + menuBounds_.h)
      scroll_ = bottom + PopupMenu::kPaddingY - menuBounds_.h;
    clampScroll();
  }

  PopupMenu menu_;
  std::function<void(int)> onChosen_;
  Component* owner_ = nullptr;
  Component* focusBefore_ = nullptr;
  Rect anchor_;
  Rect menuBounds_;
  int highlighted_ = -1;
  float scroll_ = 0.0f;
};

} // namespace gfx
} // namespace sonore
