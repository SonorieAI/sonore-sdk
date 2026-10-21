// SPDX-License-Identifier: Apache-2.0
//
// A menu bar, and the same menus as a list for a narrow editor.
//
// ── Driven by a model, not built from components ────────────────────────────
//
// The menus are asked for when they open. A MenuBarModel names the top-level
// menus and builds each PopupMenu on demand, so a menu's ticks and greyed
// items are worked out from the plugin's state at the moment somebody looks
// -- not cached when the editor was built and wrong by the time anyone opens
// it. The same model drives a MenuBarComponent and a BurgerMenuComponent, so
// an editor that swaps one for the other when it gets narrow has one place
// where its menus are defined.
//
// ── The keyboard is the hard part ───────────────────────────────────────────
//
// F10 enters the bar with the first name lit; Left and Right walk the names;
// Down, Return or a letter opens one. With a menu open, Left and Right move to
// the neighbouring menu and Escape steps back to the bar rather than out of it
// -- a second Escape leaves. Alt with a menu's initial (or the letter after an
// '&' in its name) opens it from anywhere. That is the desktop's contract and
// every step of it is a test here.
//
// One thing the peers cannot give: a tap of Alt ON ITS OWN. A bare modifier
// press is not a key event on any of the three platforms this SDK targets, so
// the route in from nothing is F10 or Alt+letter. Neither reaches a bar that
// is not an ancestor of the focused component by itself, either -- keys walk
// UP from the focus -- so the owner offers unhandled keys with
// handleShortcutKey(), from MouseRouter::onUnhandledKey, beside its commands.
//
// ── Inside the editor ───────────────────────────────────────────────────────
//
// The menus open in a PopupMenuOverlay (popup_overlay.h), which is why moving
// the mouse along the bar with a menu open switches menus and why the bar
// hears Left and Right while a menu has the keyboard.
#pragma once

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "popup_overlay.h"
#include "viewport.h"

namespace sonore {
namespace gfx {

class MenuBarModel;

namespace detail {

/** What a MenuBarModel tells the components showing it. */
class MenuModelListener {
public:
  virtual ~MenuModelListener() = default;
  virtual void menuModelChanged() = 0;
  /** The model is being destroyed: forget it, and do not call it again. */
  virtual void menuModelGone() = 0;
};

/** The name as drawn: an '&' marks the mnemonic and is not shown ("&&" is a
 *  literal ampersand). */
inline std::string menuDisplayName(const std::string& name) {
  std::string out;
  for (size_t i = 0; i < name.size(); ++i) {
    if (name[i] == '&') {
      if (i + 1 < name.size() && name[i + 1] == '&') {
        out += '&';
        ++i;
      }
      continue;
    }
    out += name[i];
  }
  return out;
}

/** The letter Alt opens this menu with, lowercased: the one after an '&', or
 *  the first letter or digit. */
inline uint32_t menuMnemonic(const std::string& name) {
  for (size_t i = 0; i + 1 < name.size(); ++i)
    if (name[i] == '&' && name[i + 1] != '&') {
      const unsigned char c = (unsigned char) name[i + 1];
      return (c >= 'A' && c <= 'Z') ? (uint32_t) (c - 'A' + 'a') : (uint32_t) c;
    }
  for (unsigned char c : name) {
    if (c >= 'A' && c <= 'Z') return (uint32_t) (c - 'A' + 'a');
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) return (uint32_t) c;
  }
  return 0;
}

} // namespace detail

/**
 * The menus: their names, and what is in each.
 *
 * Abstract, and named as the reference names it, because porting a menu
 * means porting these four functions and nothing else. The model must outlive
 * nothing: destroying it tells every component showing it, which then shows
 * nothing rather than calling into freed memory.
 */
class MenuBarModel {
public:
  MenuBarModel() = default;
  MenuBarModel(const MenuBarModel&) = delete;
  MenuBarModel& operator=(const MenuBarModel&) = delete;

  /** Tells every MenuBarComponent and BurgerMenuComponent showing the model
   *  to forget it: an open menu closes and they show nothing. */
  virtual ~MenuBarModel() {
    // By index from the back: a listener forgetting the model removes itself
    // from this vector, and nothing it does can make the walk read past it.
    while (!listeners_.empty()) {
      detail::MenuModelListener* l = listeners_.back();
      listeners_.pop_back();
      l->menuModelGone();
    }
  }

  /** The top-level names, left to right. An '&' before a letter makes it the
   *  one Alt opens the menu with. */
  virtual std::vector<std::string> getMenuBarNames() = 0;

  /** The menu behind one name, built now. Item ids must be non-zero: zero is
   *  "nothing chosen", as it is everywhere a PopupMenu answers. */
  virtual PopupMenu getMenuForIndex(int topLevelMenuIndex, const std::string& menuName) = 0;

  /** Somebody chose an item. Called after the menu is down. */
  virtual void menuItemSelected(int menuItemId, int topLevelMenuIndex) = 0;

  /** The bar went active (a menu opened, or F10 entered it) or idle again.
   *  For a plugin that pauses something while its menus are being read. */
  virtual void menuBarActivated(bool isActive) { (void) isActive; }

  /** The names changed: every component showing this model re-reads them.
   *  The menus themselves need no call -- they are built when opened. */
  void menuItemsChanged() {
    for (size_t i = 0; i < listeners_.size(); ++i) listeners_[i]->menuModelChanged();
  }

private:
  friend class MenuBarComponent;
  friend class BurgerMenuComponent;

  void addMenuListener(detail::MenuModelListener* l) {
    for (detail::MenuModelListener* existing : listeners_)
      if (existing == l) return;
    listeners_.push_back(l);
  }
  void removeMenuListener(detail::MenuModelListener* l) {
    for (size_t i = 0; i < listeners_.size(); ++i)
      if (listeners_[i] == l) {
        listeners_.erase(listeners_.begin() + (long) i);
        return;
      }
  }

  std::vector<detail::MenuModelListener*> listeners_;
};

/**
 * The bar along the top.
 *
 * Takes the keyboard only while it is being driven from it -- entered with
 * F10 or Alt -- and gives it back afterwards. A bar that was always focusable
 * would be a stop in every Tab cycle through the editor for a control the
 * mouse reaches in one move.
 */
class MenuBarComponent : public Widget, private detail::MenuModelListener {
public:
  /** A height that suits the bar, logical units; the bar draws at whatever
   *  height it is given. */
  static constexpr float kDefaultHeight = 24.0f;
  /** Space either side of a name, logical units. */
  static constexpr float kItemPadding = 10.0f;
  /** The narrowest a name's slot gets, padding included. */
  static constexpr float kMinItemWidth = 28.0f;

  /** A bar showing `model` (not owned; null shows nothing until
   *  setModel). The model may be destroyed first: the bar then empties. */
  explicit MenuBarComponent(MenuBarModel* model = nullptr) {
    overlay_.onPressOutside = [this](Point rootPos) { return pressOutside(rootPos); };
    overlay_.onMoveOutside = [this](Point rootPos) { moveOutside(rootPos); };
    overlay_.onSideKey = [this](int direction) {
      if (names_.empty() || open_ < 0) return false;
      const int n = (int) names_.size();
      showMenu(((open_ + direction) % n + n) % n, true);
      return true;
    };
    setModel(model);
  }

  /** Stops listening to the model, telling it menuBarActivated(false) if
   *  the bar was active. */
  ~MenuBarComponent() override {
    if (model_) {
      model_->removeMenuListener(this);
      if (active_) model_->menuBarActivated(false);
    }
  }

  /** Show another model (not owned), or none with null. An open menu
   *  closes first, choosing nothing; the names are read again at once. */
  void setModel(MenuBarModel* model) {
    if (model == model_) return;
    closeMenu();
    if (model_) model_->removeMenuListener(this);
    model_ = model;
    if (model_) model_->addMenuListener(this);
    refreshNames();
  }

  /** The model shown, or null (none set, or it was destroyed). */
  MenuBarModel* model() const { return model_; }

  /** How many top-level names the bar shows. */
  int numMenus() const { return (int) names_.size(); }
  /** The names as the model gave them, '&' and all. */
  const std::vector<std::string>& menuNames() const { return names_; }

  /** Where a name sits, in this component's coordinates. Measured from the
   *  font, so a long name gets the room it needs. */
  Rect itemBounds(int index) const {
    if (index < 0 || index >= (int) names_.size()) return {};
    float x = 0.0f;
    for (int i = 0; i < index; ++i) x += itemWidth(i);
    return {x, 0.0f, itemWidth(index), localBounds().h};
  }

  /** The name under a point in this component's coordinates, or -1. */
  int itemAt(Point p) const {
    if (p.y < 0.0f || p.y >= localBounds().h) return -1;
    for (int i = 0; i < (int) names_.size(); ++i)
      if (itemBounds(i).contains(p)) return i;
    return -1;
  }

  /** Open a menu. `fromKeyboard` lights its first item, so Return chooses. */
  void showMenu(int index, bool fromKeyboard = false) {
    if (!model_ || index < 0 || index >= (int) names_.size()) return;
    const std::string name = names_[(size_t) index];
    PopupMenu menu = model_->getMenuForIndex(index, name);
    // getMenuForIndex is the plugin's code and may have changed the names.
    if (!model_ || index >= (int) names_.size()) return;
    open_ = index;
    highlight_ = index;
    setActive(true);
    overlay_.show(*this, menu, itemBounds(index),
                  [this, index](int id) { menuClosed(index, id); },
                  PopupMenuOverlay::Placement::Under, fromKeyboard);
    repaint();
  }

  /** Close whatever is open, choosing nothing. */
  void closeMenu() {
    if (overlay_.isOpen()) overlay_.dismiss(0);
  }

  /** One of the bar's menus is showing. */
  bool isMenuOpen() const { return open_ >= 0 && overlay_.isOpen(); }
  /** The 0-based index of the menu showing, or -1. */
  int openMenuIndex() const { return isMenuOpen() ? open_ : -1; }
  /** The name lit by the mouse or the keyboard, or -1. */
  int highlightedIndex() const { return highlight_; }
  /** Entered from the keyboard and still being driven from it. */
  bool isKeyboardActive() const { return keyboardActive_; }

  /** The overlay the menus open in, for a test or an owner that wants to
   *  inspect what is showing. */
  PopupMenuOverlay& menuOverlay() { return overlay_; }

  /**
   * Offer a key nothing else took.
   *
   * F10 enters the bar (or leaves it); Alt plus a menu's letter opens that
   * menu. Everything else is declined, so this sits in front of a command
   * manager in MouseRouter::onUnhandledKey without hiding its shortcuts:
   *
   *   router.onUnhandledKey = [&](const KeyPress& k) {
   *     return bar.handleShortcutKey(k) || commands.keyPressed(k);
   *   };
   */
  bool handleShortcutKey(const KeyPress& key) {
    if (!isEnabled() || names_.empty() || !isShowing()) return false;
    if (key.is(KeyPress::F10) && !key.ctrlDown && !key.altDown && !key.shiftDown) {
      if (keyboardActive_) {
        closeMenu();
        leaveKeyboardMode();
        setActive(false);
      } else {
        enterKeyboardMode(0);
      }
      return true;
    }
    if (key.altDown && !key.ctrlDown && key.isCharacter()) {
      const int index = indexForMnemonic(key.character);
      if (index < 0) return false;
      enterKeyboardMode(index);
      showMenu(index, true);
      return true;
    }
    return false;
  }

  // ── Events ───────────────────────────────────────────────────────────────

  /** The look and feel's drawMenuBarBackground, then drawMenuBarItem for
   *  each name (without its '&'), lit and open as they are. */
  void paint(Graphics& g) override {
    LookAndFeel& lf = lookAndFeel();
    lf.drawMenuBarBackground(g, localBounds());
    for (int i = 0; i < (int) names_.size(); ++i)
      lf.drawMenuBarItem(g, itemBounds(i), detail::menuDisplayName(names_[(size_t) i]), font(),
                         i == highlight_, i == openMenuIndex());
  }

  /** A press on a name opens its menu (nothing while disabled). */
  void mouseDown(const MouseEvent& e) override {
    if (!isEnabled()) return;
    const int index = itemAt(e.position);
    if (index < 0) return;
    // A press on the name whose menu is open closes it. The overlay covers
    // the bar while a menu is open, so this only happens when the press
    // arrived some other way -- a test, or a peer's synthetic click.
    if (index == openMenuIndex()) {
      closeMenu();
      return;
    }
    showMenu(index, false);
  }

  /** The press that opened a menu is still the bar's: forwarded, so sliding
   *  down into the menu lights items and letting go over one chooses it. */
  void mouseDrag(const MouseEvent& e) override {
    if (isMenuOpen()) overlay_.trackPointer(e.rootPosition);
  }

  /** Letting go of the opening press over an item chooses it. */
  void mouseUp(const MouseEvent& e) override {
    if (isMenuOpen()) overlay_.releasePointer(e.rootPosition);
  }

  /** Lights the name under the pointer, unless a menu is open or the bar
   *  is being driven from the keyboard. */
  void mouseMove(const MouseEvent& e) override {
    if (keyboardActive_ || isMenuOpen()) return;
    const int over = itemAt(e.position);
    if (over != highlight_) {
      highlight_ = over;
      repaint();
    }
  }

  /** Unlights the name, with the same exceptions as mouseMove. */
  void mouseExit(const MouseEvent& e) override {
    Widget::mouseExit(e);
    if (keyboardActive_ || isMenuOpen()) return;
    if (highlight_ != -1) {
      highlight_ = -1;
      repaint();
    }
  }

  /** In keyboard mode only: Left and Right walk the names, Down, Return or
   *  Space opens the lit one, a letter opens the menu it marks, Escape or
   *  F10 leaves. Tab and Ctrl combinations are declined; every other key is
   *  swallowed while the bar has the keyboard. */
  bool keyPressed(const KeyPress& key) override {
    if (!keyboardActive_ || names_.empty()) return false;
    const int n = (int) names_.size();
    if (key.is(KeyPress::Left) || key.is(KeyPress::Right)) {
      const int step = key.is(KeyPress::Left) ? -1 : 1;
      highlight_ = (((highlight_ < 0 ? 0 : highlight_) + step) % n + n) % n;
      repaint();
      return true;
    }
    if (key.is(KeyPress::Down) || key.is(KeyPress::Return) ||
        (key.character == ' ' && !key.ctrlDown)) {
      showMenu(highlight_ < 0 ? 0 : highlight_, true);
      return true;
    }
    if (key.is(KeyPress::Escape) || (key.is(KeyPress::F10) && !key.ctrlDown && !key.altDown)) {
      leaveKeyboardMode();
      setActive(false);
      return true;
    }
    if (key.is(KeyPress::Tab) || key.ctrlDown) return false;
    if (key.isCharacter()) {
      const int index = indexForMnemonic(key.character);
      if (index >= 0) showMenu(index, true);
      return true;
    }
    return true;
  }

  /** Ends keyboard mode when something other than the bar's own menu takes
   *  the keyboard. */
  void focusLost() override {
    // The open menu takes the keyboard and hands it back on Escape; anything
    // else taking it -- a click elsewhere, a Tab -- ends keyboard mode.
    if (!keyboardActive_ || overlay_.isOpen()) return;
    keyboardActive_ = false;
    highlight_ = -1;
    setWantsKeyboardFocus(false);
    setActive(false);
    repaint();
  }

  AccessibleInfo accessibleInfo() const override {
    AccessibleInfo info = baseInfo(AccessibleRole::Group);
    if (info.name.empty()) info.name = "Menu bar";
    const int shown = openMenuIndex() >= 0 ? openMenuIndex() : highlight_;
    if (shown >= 0 && shown < (int) names_.size())
      info.value = detail::menuDisplayName(names_[(size_t) shown]);
    return info;
  }

private:
  void menuModelChanged() override { refreshNames(); }

  void menuModelGone() override {
    model_ = nullptr;
    closeMenu();
    names_.clear();
    open_ = highlight_ = -1;
    active_ = false;
    repaint();
  }

  void refreshNames() {
    names_ = model_ ? model_->getMenuBarNames() : std::vector<std::string>();
    if (highlight_ >= (int) names_.size()) highlight_ = names_.empty() ? -1 : 0;
    if (open_ >= (int) names_.size()) closeMenu();
    repaint();
  }

  float itemWidth(int index) const {
    const float w = font().stringWidth(detail::menuDisplayName(names_[(size_t) index])) +
                    kItemPadding * 2.0f;
    return w < kMinItemWidth ? kMinItemWidth : w;
  }

  int indexForMnemonic(uint32_t c) const {
    const uint32_t wanted = (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c;
    for (int i = 0; i < (int) names_.size(); ++i)
      if (detail::menuMnemonic(names_[(size_t) i]) == wanted) return i;
    return -1;
  }

  void setActive(bool active) {
    if (active == active_) return;
    active_ = active;
    if (model_) model_->menuBarActivated(active);
  }

  void enterKeyboardMode(int index) {
    MouseRouter* router = routerFor(this);
    if (!keyboardActive_) focusBefore_ = router ? router->focused() : nullptr;
    keyboardActive_ = true;
    highlight_ = index;
    setWantsKeyboardFocus(true);
    if (router) router->setFocus(this);
    setActive(true);
    repaint();
  }

  /** Back to the component that had the keyboard before F10 -- if it is still
   *  in the tree. It may have been destroyed while the menus were up, and a
   *  stale pointer handed to setFocus would be a write into freed memory. */
  void leaveKeyboardMode() {
    if (!keyboardActive_) return;
    keyboardActive_ = false;
    highlight_ = -1;
    Component* restore = focusBefore_;
    focusBefore_ = nullptr;
    MouseRouter* router = routerFor(this);
    if (router && router->focused() == this)
      router->setFocus(detail::overlayTreeContains(root(), restore) && restore != this ? restore
                                                                                    : nullptr);
    setWantsKeyboardFocus(false);
    repaint();
  }

  /** The overlay's answer. State first, the model's handler LAST: it is the
   *  plugin's code and may do anything, including rebuild the editor. */
  void menuClosed(int index, int id) {
    open_ = -1;
    MenuBarModel* model = model_;
    if (id != 0) {
      leaveKeyboardMode();
      setActive(false);
    } else if (!keyboardActive_) {
      highlight_ = -1;
      setActive(false);
    }
    repaint();
    if (id != 0 && model) model->menuItemSelected(id, index);
  }

  bool pressOutside(Point rootPos) {
    const int index = itemAt(rootToLocal(rootPos));
    if (index < 0 || index == open_) return false; // closes the menu
    showMenu(index, false);
    return true;
  }

  void moveOutside(Point rootPos) {
    const int index = itemAt(rootToLocal(rootPos));
    if (index >= 0 && index != open_) showMenu(index, false);
  }

  MenuBarModel* model_ = nullptr;
  std::vector<std::string> names_;
  PopupMenuOverlay overlay_;
  Component* focusBefore_ = nullptr;
  int open_ = -1;
  int highlight_ = -1;
  bool keyboardActive_ = false;
  bool active_ = false;
};

/**
 * The same menus as one scrolling list: each top-level name a heading, its
 * items under it.
 *
 * For an editor too narrow for a bar -- usually inside a SidePanel opened by
 * a burger button, which is the arrangement the name comes from. Re-reads the
 * model whenever it says so and whenever refresh() is called, because a
 * list that is always visible cannot build its contents on open the way a
 * drop-down does.
 */
class BurgerMenuComponent : public Widget, private detail::MenuModelListener {
public:
  /** A heading row's height (a top-level name or a menu's own header),
   *  logical units. */
  static constexpr float kHeadingHeight = 26.0f;
  /** An item row's height, logical units. */
  static constexpr float kItemHeight = 28.0f;
  /** A separator row's height, logical units. */
  static constexpr float kSeparatorHeight = 9.0f;

  /** A list showing `model` (not owned; null shows nothing until
   *  setModel). It wants the keyboard focus, for the arrow keys. */
  explicit BurgerMenuComponent(MenuBarModel* model = nullptr) {
    setWantsKeyboardFocus(true);
    addChild(&scrollBar_);
    scrollBar_.onScroll = [this](float y) {
      scroll_ = y;
      repaint();
    };
    setModel(model);
  }

  /** Stops listening to the model. */
  ~BurgerMenuComponent() override {
    if (model_) model_->removeMenuListener(this);
  }

  /** Show another model (not owned), or none with null; the rows are
   *  rebuilt at once, building every one of its menus. */
  void setModel(MenuBarModel* model) {
    if (model == model_) return;
    if (model_) model_->removeMenuListener(this);
    model_ = model;
    if (model_) model_->addMenuListener(this);
    refresh();
  }
  /** The model shown, or null (none set, or it was destroyed). */
  MenuBarModel* model() const { return model_; }

  /** Rebuild the rows from the model: names AND items, since every item is
   *  on screen at once. */
  void refresh() {
    rows_.clear();
    if (model_) {
      const std::vector<std::string> names = model_->getMenuBarNames();
      for (int top = 0; top < (int) names.size(); ++top) {
        Row heading;
        heading.kind = Row::Heading;
        heading.text = detail::menuDisplayName(names[(size_t) top]);
        heading.topLevel = top;
        rows_.push_back(heading);
        const PopupMenu menu = model_->getMenuForIndex(top, names[(size_t) top]);
        for (int i = 0; i < menu.numItems(); ++i) {
          const PopupItem& item = menu.itemAt(i);
          Row row;
          row.kind = item.isSeparator ? Row::Separator : (item.isHeader ? Row::Heading : Row::Item);
          row.text = item.text;
          row.id = item.id;
          row.topLevel = top;
          row.enabled = item.enabled && !item.isHeader && !item.isSeparator;
          row.ticked = item.ticked;
          rows_.push_back(row);
        }
      }
    }
    if (selected_ >= (int) rows_.size() || (selected_ >= 0 && !isChoosable(selected_)))
      selected_ = -1;
    updateScrollBar();
    repaint();
  }

  /** Every row: one heading per top-level name, then that menu's items,
   *  headers and separators. */
  int numRows() const { return (int) rows_.size(); }
  /** A row's text as drawn (a top-level name without its '&'); `row` must
   *  be in 0..numRows()-1, it is not checked. */
  const std::string& rowText(int row) const { return rows_[(size_t) row].text; }
  /** A top-level name or a menu's own header: drawn as a heading, never
   *  chosen. `row` must be in range, unchecked. */
  bool isHeading(int row) const { return rows_[(size_t) row].kind == Row::Heading; }
  /** The item id the row chooses; 0 for a heading or a separator. `row`
   *  must be in range, unchecked. */
  int rowItemId(int row) const { return rows_[(size_t) row].id; }

  /** Where a row is, in this component's coordinates (scrolled). */
  Rect rowBounds(int row) const {
    if (row < 0 || row >= (int) rows_.size()) return {};
    return {0.0f, topOf(row) - scroll_, contentWidth(), heightOf(row)};
  }

  /** The row under a point in this component's coordinates (scrolled), or
   *  -1 off the rows or over the scroll bar. */
  int rowAt(Point p) const {
    if (!localBounds().contains(p) || p.x >= contentWidth()) return -1;
    const float y = p.y + scroll_;
    float top = 0.0f;
    for (int i = 0; i < (int) rows_.size(); ++i) {
      const float h = heightOf(i);
      if (y >= top && y < top + h) return i;
      top += h;
    }
    return -1;
  }

  /** The lit row (by the pointer or the arrow keys), or -1. */
  int selectedRow() const { return selected_; }
  /** Light a row and scroll it into view (with its menu's heading, for a
   *  menu's first item); -1 lights none. A heading, a separator or a
   *  disabled item is refused and nothing changes. */
  void setSelectedRow(int row) {
    if (row >= 0 && (row >= (int) rows_.size() || !isChoosable(row))) return;
    if (row == selected_) return;
    selected_ = row;
    scrollToShow(row);
    repaint();
  }

  /** After the model has been told, for an owner that closes the drawer the
   *  list sits in. */
  std::function<void(int menuItemId, int topLevelMenuIndex)> onItemInvoked;

  /** Choose a row, exactly as a click on it would. */
  bool invokeRow(int row) {
    if (row < 0 || row >= (int) rows_.size() || !isChoosable(row)) return false;
    const Row chosen = rows_[(size_t) row];
    // The model's handler on a COPY of the row: it may call menuItemsChanged,
    // which rebuilds rows_ under this call. It may also destroy this list --
    // a command that closes the panel the list is in -- so the owner's hook
    // after it is read only if the list is still here.
    detail::DeletionWatch alive(this);
    if (model_) model_->menuItemSelected(chosen.id, chosen.topLevel);
    if (alive.deleted) return true;
    if (onItemInvoked) onItemInvoked(chosen.id, chosen.topLevel);
    return true;
  }

  // ── Events ───────────────────────────────────────────────────────────────

  /** Shows the scroll bar when the rows are taller than the list. */
  void resized() override { updateScrollBar(); }

  /** The look and feel's panel colour, then each visible row: headings by
   *  drawListHeading, separators and items as a popup menu draws them. */
  void paint(Graphics& g) override {
    LookAndFeel& lf = lookAndFeel();
    const Rect area = localBounds();
    g.setColour(lf.panel());
    g.fillRect(area);
    clampScroll();
    for (int i = 0; i < (int) rows_.size(); ++i) {
      const Rect row = rowBounds(i);
      if (row.bottom() < 0.0f || row.y > area.h) continue;
      Graphics::ScopedState scope(g);
      g.clipTo(row);
      if (g.isClippedOut()) continue;
      const Row& r = rows_[(size_t) i];
      if (r.kind == Row::Heading) {
        lf.drawListHeading(g, row, r.text, font());
      } else if (r.kind == Row::Separator) {
        lf.drawPopupSeparator(g, row);
      } else {
        PopupItem item;
        item.id = r.id;
        item.text = r.text;
        item.enabled = r.enabled;
        item.ticked = r.ticked;
        lf.drawPopupItem(g, row, item, font(), i == selected_ || i == pressed_);
      }
    }
    paintFocusRing(g);
  }

  /** Presses and lights a choosable row; nothing is chosen until the
   *  release. */
  void mouseDown(const MouseEvent& e) override {
    if (!isEnabled()) return;
    const int row = rowAt(e.position);
    pressed_ = (row >= 0 && isChoosable(row)) ? row : -1;
    if (pressed_ >= 0) setSelectedRow(pressed_);
    repaint();
  }

  /** Chooses the pressed row (invokeRow) if the release is over it. */
  void mouseUp(const MouseEvent& e) override {
    const int was = pressed_;
    pressed_ = -1;
    repaint();
    // On the release, and only over the row that was pressed: a press that
    // slides off is a change of mind, as it is on a button.
    if (was >= 0 && rowAt(e.position) == was) invokeRow(was);
  }

  /** Lights the choosable row under the pointer. */
  void mouseMove(const MouseEvent& e) override {
    const int row = rowAt(e.position);
    if (row >= 0 && isChoosable(row) && row != selected_) setSelectedRow(row);
  }

  bool mouseWheel(const MouseEvent&, float delta) override {
    if (delta == 0.0f || contentHeight() <= localBounds().h) return false;
    scroll_ -= delta * ScrollBar::kWheelStep;
    clampScroll();
    scrollBar_.setPosition(scroll_, false);
    repaint();
    return true;
  }

  /** Up, Down, Home and End move between choosable rows (taken even at
   *  the ends); Return or Space chooses the lit one. */
  bool keyPressed(const KeyPress& key) override {
    if (!isEnabled() || rows_.empty()) return false;
    if (key.is(KeyPress::Down)) return step(+1, selected_);
    if (key.is(KeyPress::Up)) return step(-1, selected_ < 0 ? (int) rows_.size() : selected_);
    if (key.is(KeyPress::Home)) return step(+1, -1);
    if (key.is(KeyPress::End)) return step(-1, (int) rows_.size());
    if (key.is(KeyPress::Return) || (key.character == ' ' && !key.ctrlDown)) {
      if (selected_ < 0) return false;
      invokeRow(selected_);
      return true;
    }
    return false;
  }

  AccessibleInfo accessibleInfo() const override {
    AccessibleInfo info = baseInfo(AccessibleRole::List);
    if (info.name.empty()) info.name = "Menu";
    if (!rows_.empty()) {
      info.hasRange = true;
      info.minValue = 0.0;
      info.maxValue = (double) rows_.size() - 1.0;
      info.currentValue = (double) (selected_ < 0 ? 0 : selected_);
      if (selected_ >= 0) info.value = rows_[(size_t) selected_].text;
    }
    return info;
  }

private:
  /** One line of the flattened list: a top-level name or a menu's own header
   *  (Heading), a choosable item with its id, or a separator. `topLevel` is
   *  the menu it came from, which is what menuItemSelected is told. */
  struct Row {
    enum Kind { Heading, Item, Separator };
    Kind kind = Item;
    std::string text;
    int id = 0;
    int topLevel = 0;
    bool enabled = true;
    bool ticked = false;
  };

  void menuModelChanged() override { refresh(); }
  void menuModelGone() override {
    model_ = nullptr;
    refresh();
  }

  bool isChoosable(int row) const {
    const Row& r = rows_[(size_t) row];
    return r.kind == Row::Item && r.enabled;
  }

  float heightOf(int row) const {
    const Row::Kind k = rows_[(size_t) row].kind;
    return k == Row::Heading ? kHeadingHeight : (k == Row::Separator ? kSeparatorHeight : kItemHeight);
  }

  float topOf(int row) const {
    float y = 0.0f;
    for (int i = 0; i < row; ++i) y += heightOf(i);
    return y;
  }

  float contentHeight() const { return topOf((int) rows_.size()); }
  float contentWidth() const {
    return localBounds().w - (scrollBar_.isVisible() ? ScrollBar::kThickness : 0.0f);
  }

  bool step(int direction, int from) {
    const int n = (int) rows_.size();
    for (int i = from + direction; i >= 0 && i < n; i += direction)
      if (isChoosable(i)) {
        setSelectedRow(i);
        return true;
      }
    return true; // at the end: the key is still the list's, not the editor's
  }

  void scrollToShow(int row) {
    if (row < 0) return;
    const float top = topOf(row), bottom = top + heightOf(row);
    // The heading above the first item of a menu comes into view with it --
    // an item shown without its menu's name is an item out of context.
    const float wantTop = (row > 0 && rows_[(size_t) row - 1].kind == Row::Heading)
                              ? top - heightOf(row - 1)
                              : top;
    if (wantTop < scroll_) scroll_ = wantTop;
    else if (bottom > scroll_ + localBounds().h) scroll_ = bottom - localBounds().h;
    clampScroll();
    scrollBar_.setPosition(scroll_, false);
  }

  void clampScroll() {
    const float most = contentHeight() - localBounds().h;
    if (!(scroll_ <= most)) scroll_ = most;
    if (!(scroll_ >= 0.0f)) scroll_ = 0.0f;
  }

  void updateScrollBar() {
    const Rect area = localBounds();
    scrollBar_.setVisible(contentHeight() > area.h);
    scrollBar_.setBounds({area.w - ScrollBar::kThickness, 0.0f, ScrollBar::kThickness, area.h});
    clampScroll();
    scrollBar_.setRange(contentHeight(), area.h, scroll_);
  }

  MenuBarModel* model_ = nullptr;
  std::vector<Row> rows_;
  ScrollBar scrollBar_{true};
  float scroll_ = 0.0f;
  int selected_ = -1;
  int pressed_ = -1;
};

} // namespace gfx
} // namespace sonore
