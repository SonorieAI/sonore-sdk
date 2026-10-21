// SPDX-License-Identifier: Apache-2.0
//
// Toolbar: a row of buttons the user can rearrange.
//
// ── The pieces, as the reference has them ───────────────────────────────────
//
// A ToolbarItemFactory knows every item a toolbar may hold and which of them
// it starts with, and makes one on request. A Toolbar holds the items it was
// given in order, lays them out along its length -- separators, fixed spacers
// and flexible spacers that soak up whatever room is left -- and, when they do
// not fit, puts the ones past the end behind a chevron that opens them as a
// menu. A ToolbarItemPalette shows everything the factory can make, and items
// are dragged from it onto the toolbar, along the toolbar to reorder, and back
// onto it to remove. toString() and restoreFromString() carry the arrangement
// to a settings file and back.
//
// ── Why the toolbar OWNS its items ──────────────────────────────────────────
//
// Nothing else in this SDK owns its children; a plugin keeps its controls as
// members and adds them. A customisable toolbar cannot work that way: the
// user creates items by dragging them out of the palette and destroys them by
// dragging them back, so the items come from the factory and their lifetime is
// the toolbar's. That is also the reference's arrangement, and for the same
// reason.
//
// ── Editing ─────────────────────────────────────────────────────────────────
//
// While a toolbar is being customised its items must not DO anything -- a
// press on Save while rearranging is a drag, not a save. Each item gets a
// transparent handle laid over it that takes the mouse and starts the drag,
// so an item that is a slider or a combo box is rearranged the same way as a
// button, and none of them has to know what mode it is in.
//
// The customisation dialog is an Overlay inside the editor, like everything
// modal here (alert.h says why). It leaves a hole over the toolbar itself --
// not dimmed, and transparent to the mouse -- because the toolbar is where
// the palette's items are dropped.
#pragma once

#include <cmath>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "alert.h"
#include "popup_overlay.h"
#include "svg.h"
#include "viewport.h"
#include "widgets.h"

namespace sonore {
namespace gfx {

class Toolbar;
class ToolbarItemPalette;

/** How a toolbar's buttons show themselves. Toolbar::iconsOnly and friends
 *  name the same values, as the reference spells them. */
enum class ToolbarItemStyle {
  iconsOnly,     ///< The icon alone; a button with no icon shows its label.
  iconsWithText, ///< The icon with the label under it (the default).
  textOnly       ///< The label alone.
};

/**
 * One thing on a toolbar.
 *
 * Abstract: a subclass says how big it wants to be along the toolbar and
 * draws itself. ToolbarButton is the one most toolbars are made of.
 */
class ToolbarItemComponent : public Widget {
public:
  /** Normal on a toolbar in use; editable while being customised or while
   *  sitting in the palette, where a press starts a drag instead. */
  enum class EditingMode {
    normalMode,        ///< In use: the item does what it does.
    editableOnToolbar, ///< On a toolbar being customised: a press drags it.
    editableOnPalette  ///< In a ToolbarItemPalette: a press drags a copy out.
  };

  /** `itemId` is the factory's id for it (what toString() saves);
   *  `labelText` is its label and accessible name; `isBeingUsedAsAButton`
   *  lets the overflow menu offer it as a choice. */
  ToolbarItemComponent(int itemId, std::string labelText, bool isBeingUsedAsAButton)
      : itemId_(itemId), label_(std::move(labelText)), isButton_(isBeingUsedAsAButton) {
    handle_.item = this;
    if (!label_.empty()) setAccessibleName(label_);
  }

  /** The factory's id for this item; negative for a separator or spacer. */
  int itemId() const { return itemId_; }
  /** The label: drawn by a button that shows text, and the overflow menu's
   *  entry. */
  const std::string& textLabel() const { return label_; }
  /** Change the label. The accessible name set by the constructor is not
   *  changed with it. */
  void setTextLabel(std::string text) {
    label_ = std::move(text);
    repaint();
  }
  /** Listed as an enabled entry in the overflow menu (when the item is
   *  enabled); false lists it greyed out. */
  bool isBeingUsedAsAButton() const { return isButton_; }

  /** The toolbar this item is on, or null -- in a palette, or not yet added. */
  Toolbar* toolbar() const { return toolbar_; }
  /** The style its toolbar gave it (iconsWithText until then). */
  ToolbarItemStyle style() const { return style_; }
  /** Set by the toolbar and the palette to the toolbar's style; a direct
   *  call is overwritten the next time they set it. */
  void setStyle(ToolbarItemStyle s) {
    style_ = s;
    repaint();
  }

  /** The mode the toolbar or the palette put it in. */
  EditingMode editingMode() const { return editing_; }
  /** Outside normalMode a transparent handle covers the item, so a press
   *  starts a drag and never reaches the item or its children. Set by the
   *  toolbar and the palette. */
  void setEditingMode(EditingMode mode) {
    if (mode == editing_) return;
    editing_ = mode;
    if (mode == EditingMode::normalMode) {
      removeChild(&handle_);
    } else {
      addChild(&handle_);
      handle_.setBounds(localBounds());
    }
    repaint();
  }

  /**
   * How long this item wants to be along the toolbar, and how short and long
   * it may go, given the toolbar's thickness. Return false to be laid out at
   * the toolbar's thickness, square.
   */
  virtual bool toolbarItemSizes(float toolbarThickness, bool isToolbarVertical, float& preferredSize,
                                float& minSize, float& maxSize) = 0;

  /** Draw the item. `width` and `height` are the item's own. */
  virtual void paintButtonArea(Graphics& g, float width, float height, bool isMouseOver,
                               bool isMouseDown) = 0;

  /** The item moved or changed size; child components go here. */
  virtual void contentAreaChanged(const Rect& newArea) = 0;

  /** Do what this item does, because it was chosen from the overflow menu.
   *  False for an item that cannot be worked from a menu -- a slider on a
   *  toolbar is not a menu item -- and it is listed greyed out. */
  virtual bool triggerFromMenu() { return false; }

  /** Calls paintButtonArea, then outlines the item while it is editable. */
  void paint(Graphics& g) override {
    paintButtonArea(g, localBounds().w, localBounds().h, hovered_, pressed_);
    // An item being rearranged is outlined, so the user can see what a press
    // will pick up -- most usefully a spacer, which is otherwise invisible.
    if (editing_ != EditingMode::normalMode) {
      LookAndFeel& lf = lookAndFeel();
      g.setColour(lf.accent().withMultipliedAlpha(dragging_ ? 0.9f : 0.45f));
      g.drawRoundedRect(localBounds().reduced(1.0f), 3.0f, 1.0f);
    }
  }

  /** Keeps the drag handle over the item and calls contentAreaChanged with
   *  the item's local bounds. */
  void resized() override {
    if (editing_ != EditingMode::normalMode) handle_.setBounds(localBounds());
    contentAreaChanged(localBounds());
  }

  /** What a drag of this item carries. */
  std::string dragDescription() const { return kDragPrefix + std::to_string(itemId_); }

  /** The start of every toolbar item drag's description, followed by the
   *  item id; what Toolbar::isInterestedInDrag looks for. */
  static constexpr const char* kDragPrefix = "sonore-toolbar-item:";

protected:
  /** Held down on the item: what paint() passes to paintButtonArea as
   *  `isMouseDown`. A subclass that handles the mouse keeps it up to date. */
  bool pressed_ = false;

private:
  friend class Toolbar;
  friend class ToolbarItemPalette;

  /** Laid over an item while it can be rearranged: takes the press, and turns
   *  a press that moves into a drag of the ITEM -- the source a drop target
   *  sees is the item, never this. */
  class DragHandle : public Component {
  public:
    ToolbarItemComponent* item = nullptr;
    /** Defined below the dialog: a press here hands the keyboard to the
     *  customisation dialog, so Escape still closes it after the user has
     *  been dragging things about. */
    void mouseDown(const MouseEvent&) override;
    void mouseDrag(const MouseEvent& e) override {
      if (started_ || !item) return;
      if (std::fabs(e.dragDistanceX()) + std::fabs(e.dragDistanceY()) < 4.0f) return;
      if (MouseRouter* r = routerFor(this)) {
        started_ = true;
        item->dragging_ = true;
        item->repaint();
        r->startDrag(item->dragDescription(), item, e.rootPosition);
      }
    }
    void mouseUp(const MouseEvent&) override {
      started_ = false;
      if (item) {
        item->dragging_ = false;
        item->repaint();
      }
    }
    MouseCursor cursor() const override { return MouseCursor::Pointing; }

  private:
    bool started_ = false;
  };

  /** The open customisation dialog this item is being edited in, or null. */
  Component* customisationDialog() const;

  int itemId_;
  std::string label_;
  bool isButton_;
  Toolbar* toolbar_ = nullptr;
  ToolbarItemStyle style_ = ToolbarItemStyle::iconsWithText;
  EditingMode editing_ = EditingMode::normalMode;
  bool dragging_ = false;
  DragHandle handle_;
};

/** What a toolbar is made from. */
class ToolbarItemFactory {
public:
  /** The ids a toolbar makes itself and every factory may list. A factory's
   *  own items take ids from 0 up; no other negative id is made. */
  enum SpecialItemIds {
    separatorBarId = -1,   ///< A thin bar between groups of items.
    spacerId = -2,         ///< A fixed gap, half the toolbar's thickness.
    flexibleSpacerId = -3, ///< A gap sharing whatever room the items leave.
  };

  /** A factory may be destroyed through a pointer to this base. A toolbar
   *  does not keep it; a palette and an open customisation dialog do, and
   *  must not outlive it. */
  virtual ~ToolbarItemFactory() = default;

  /** Every item this factory can make, in the order the palette shows them.
   *  Include the special ids to offer separators and spacers. */
  virtual void getAllToolbarItemIds(std::vector<int>& ids) = 0;

  /** The arrangement a fresh toolbar, or "Reset to defaults", gets. */
  virtual void getDefaultItemSet(std::vector<int>& ids) = 0;

  /** A new item, or null for an id this factory does not know. The special
   *  ids never reach here: the toolbar makes those. */
  virtual std::unique_ptr<ToolbarItemComponent> createItem(int itemId) = 0;
};

namespace detail {

/** A separator, a fixed gap, or a gap that grows. */
class ToolbarSpacerItem : public ToolbarItemComponent {
public:
  explicit ToolbarSpacerItem(int id)
      : ToolbarItemComponent(id,
                             id == ToolbarItemFactory::separatorBarId
                                 ? "Separator"
                                 : (id == ToolbarItemFactory::spacerId ? "Spacer" : "Flexible spacer"),
                             false) {
    setInterceptsMouse(false);
    setAccessibilityIgnored(true);
  }

  bool toolbarItemSizes(float thickness, bool, float& preferred, float& minSize, float& maxSize) override {
    if (editingMode() == EditingMode::editableOnPalette) {
      // In the palette a gap is shown as a box big enough to grab.
      preferred = minSize = maxSize = thickness * 1.5f;
      return true;
    }
    if (itemId() == ToolbarItemFactory::separatorBarId) {
      preferred = minSize = maxSize = (std::max)(6.0f, thickness * 0.2f);
    } else if (itemId() == ToolbarItemFactory::spacerId) {
      preferred = minSize = maxSize = thickness * 0.5f;
    } else {
      preferred = minSize = 0.0f;
      maxSize = 1.0e6f;
    }
    return true;
  }

  void paintButtonArea(Graphics& g, float w, float h, bool, bool) override {
    LookAndFeel& lf = lookAndFeel();
    const bool vertical = toolbar() != nullptr && isVerticalToolbar();
    if (itemId() == ToolbarItemFactory::separatorBarId)
      lf.drawToolbarSeparator(g, Rect(0.0f, 0.0f, w, h), vertical);
    if (editingMode() == EditingMode::editableOnPalette) {
      g.setColour(lf.dimText());
      font().drawIn(g, textLabel(), Rect(0.0f, 0.0f, w, h), Justify::Centred);
    }
  }

  void contentAreaChanged(const Rect&) override {}

private:
  bool isVerticalToolbar() const;
};

/** The chevron that opens what did not fit. */
class ToolbarOverflowButton : public Widget {
public:
  ToolbarOverflowButton() {
    setWantsKeyboardFocus(true);
    setAccessibleName("More items");
    setCursor(MouseCursor::Pointing);
  }
  std::function<void(bool fromKeyboard)> onOpen;
  bool vertical = false;

  void paint(Graphics& g) override {
    lookAndFeel().drawToolbarOverflowButton(g, localBounds(), vertical, stateFor(pressed_));
    paintFocusRing(g);
  }
  void mouseDown(const MouseEvent&) override {
    pressed_ = true;
    repaint();
    if (onOpen) onOpen(false);
  }
  void mouseUp(const MouseEvent&) override {
    pressed_ = false;
    repaint();
  }
  bool keyPressed(const KeyPress& key) override {
    if (!key.is(KeyPress::Return) && !key.is(KeyPress::Down) && key.character != ' ') return false;
    if (onOpen) onOpen(true);
    return true;
  }
  AccessibleInfo accessibleInfo() const override { return baseInfo(AccessibleRole::Button); }

private:
  bool pressed_ = false;
};

class ToolbarCustomiser;

} // namespace detail

/**
 * A button with an icon, a label or both, for a toolbar.
 *
 * The icon is SVG, as IconButton's is, and drawn in the look-and-feel's icon
 * colour where it uses currentColor. A second icon may be given for the
 * toggled state.
 */
class ToolbarButton : public ToolbarItemComponent {
public:
  /** `iconSvg` and `toggledIconSvg` are SVG documents, parsed now (an
   *  empty or unparsable one is no icon); a button with no icon shows its
   *  label whatever the style. */
  ToolbarButton(int itemId, std::string labelText, const std::string& iconSvg = {},
                const std::string& toggledIconSvg = {})
      : ToolbarItemComponent(itemId, std::move(labelText), true) {
    setWantsKeyboardFocus(true);
    if (!iconSvg.empty()) icon_ = Drawable::parse(iconSvg);
    if (!toggledIconSvg.empty()) toggledIcon_ = Drawable::parse(toggledIconSvg);
  }

  /** Called by click(): a click, Return or Space, or the overflow menu;
   *  after the toggle flips for a toggleable button. */
  std::function<void()> onClick;

  /** Make a click flip the toggled state (false by default). */
  void setToggleable(bool canToggle) { toggleable_ = canToggle; }
  /** What setToggleable() set. */
  bool isToggleable() const { return toggleable_; }
  /** The toggled state, drawn lit and with the toggled icon if there is
   *  one. */
  bool isToggled() const { return toggled_; }
  /** Set the toggled state (whether or not the button is toggleable);
   *  `notify` calls onClick when it changed. */
  void setToggled(bool on, bool notify = false) {
    if (on == toggled_) return;
    toggled_ = on;
    repaint();
    if (notify && onClick) onClick();
  }

  /** What a click does: toggle if this toggles, then onClick. The same path
   *  for a click, a key and the overflow menu, so each fires exactly once. */
  void click() {
    if (!isEnabled()) return;
    if (toggleable_) toggled_ = !toggled_;
    repaint();
    if (onClick) onClick();
  }

  bool triggerFromMenu() override {
    if (!isEnabled()) return false;
    click();
    return true;
  }

  bool toolbarItemSizes(float thickness, bool vertical, float& preferred, float& minSize,
                        float& maxSize) override {
    const float textW = font().stringWidth(textLabel());
    const bool showIcon = style() != ToolbarItemStyle::textOnly && hasIcon();
    const bool showText = style() != ToolbarItemStyle::iconsOnly || !hasIcon();
    float size = thickness;
    // A horizontal toolbar's items grow along it to fit their label; a
    // vertical one's are as long as they are thick, and a long label is
    // shortened rather than pushing the toolbar wider.
    if (showText && !vertical) size = (std::max)(showIcon ? thickness : 0.0f, textW + 16.0f);
    preferred = minSize = maxSize = size;
    return true;
  }

  void paintButtonArea(Graphics& g, float w, float h, bool isMouseOver, bool isMouseDown) override {
    LookAndFeel& lf = lookAndFeel();
    WidgetState st;
    st.hovered = isMouseOver && editingMode() == EditingMode::normalMode;
    st.pressed = isMouseDown && editingMode() == EditingMode::normalMode;
    st.toggled = toggled_;
    st.enabled = isEnabled();
    const Rect area(0.0f, 0.0f, w, h);
    lf.drawToolbarButtonBackground(g, area, st);

    const bool showIcon = style() != ToolbarItemStyle::textOnly && hasIcon();
    const bool showText = style() != ToolbarItemStyle::iconsOnly || !hasIcon();
    const float textH = showText && showIcon ? (std::min)(h * 0.36f, font().lineHeight() + 2.0f) : 0.0f;
    if (showIcon) {
      const Drawable& shown = (toggled_ && !toggledIcon_.isEmpty()) ? toggledIcon_ : icon_;
      const float side = (std::min)(w, h - textH) - 8.0f;
      if (side > 0.0f)
        shown.draw(g, Rect((w - side) * 0.5f, (h - textH - side) * 0.5f, side, side),
                   lf.iconButtonColour(st, toggled_));
    }
    if (showText) {
      g.setColour(st.enabled ? lf.text() : lf.dimText());
      const Rect textArea = showIcon ? Rect(2.0f, h - textH - 2.0f, w - 4.0f, textH) : area.reduced(4.0f, 0.0f);
      font().drawIn(g, textLabel(), textArea, Justify::Centred);
    }
    if (hasKeyboardFocus()) lf.drawFocusRing(g, area);
  }

  void contentAreaChanged(const Rect&) override {}

  /** Presses the button (not while disabled); nothing happens until the
   *  release. */
  void mouseDown(const MouseEvent&) override {
    if (!isEnabled()) return;
    pressed_ = true;
    repaint();
  }
  /** Shows the button pressed only while the pointer is over it. */
  void mouseDrag(const MouseEvent& e) override {
    const bool inside = localBounds().contains(e.position);
    if (inside != pressed_) {
      pressed_ = inside;
      repaint();
    }
  }
  /** A release over the pressed button clicks it; elsewhere, nothing. */
  void mouseUp(const MouseEvent& e) override {
    const bool was = pressed_;
    pressed_ = false;
    repaint();
    if (was && localBounds().contains(e.position)) click();
  }

  bool keyPressed(const KeyPress& key) override {
    if (!isEnabled() || editingMode() != EditingMode::normalMode) return false;
    if (!key.is(KeyPress::Return) && key.character != (uint32_t) ' ') return false;
    click();
    return true;
  }

  AccessibleInfo accessibleInfo() const override {
    AccessibleInfo info = baseInfo(toggleable_ ? AccessibleRole::ToggleButton : AccessibleRole::Button);
    if (info.name.empty()) info.name = textLabel();
    if (toggleable_) info.value = toggled_ ? "on" : "off";
    return info;
  }

  /** The constructor's `iconSvg` parsed to something drawable. */
  bool hasIcon() const { return !icon_.isEmpty(); }

private:
  Drawable icon_, toggledIcon_;
  bool toggleable_ = false, toggled_ = false;
};

/**
 * The bar.
 *
 * Horizontal by default; setVertical stands it down the side, where its
 * thickness is its width.
 */
class Toolbar : public Widget {
public:
  // The reference's names for the styles, on the class.
  /** ToolbarItemStyle::iconsOnly. */
  static constexpr ToolbarItemStyle iconsOnly = ToolbarItemStyle::iconsOnly;
  /** ToolbarItemStyle::iconsWithText. */
  static constexpr ToolbarItemStyle iconsWithText = ToolbarItemStyle::iconsWithText;
  /** ToolbarItemStyle::textOnly. */
  static constexpr ToolbarItemStyle textOnly = ToolbarItemStyle::textOnly;

  /** What the customisation dialog offers, OR-ed into
   *  showCustomisationDialog's `optionFlags`. With none of the three style
   *  choices the dialog has no style box. */
  enum CustomisationFlags {
    allowIconsOnlyChoice = 1,           ///< "Icons only" in the style box.
    allowIconsWithTextChoice = 2,       ///< "Icons with text" in the style box.
    allowTextOnlyChoice = 4,            ///< "Text only" in the style box.
    showResetToDefaultsButton = 8,      ///< A button that restores the factory's default set.
    allCustomisationOptionsEnabled = 15, ///< All of the above.
  };

  /** The chevron's length along the toolbar, logical units; it is as
   *  thick as the toolbar. */
  static constexpr float kOverflowButtonSize = 22.0f;

  // Both out of line, below the dialog's class: a member that owns it must
  // see it complete wherever the toolbar is built or destroyed.
  /** An empty horizontal toolbar in the iconsWithText style. */
  Toolbar();
  /** Destroys its items and any customisation dialog. A palette the caller
   *  made for it must be destroyed first: it holds a reference to it. */
  ~Toolbar() override;

  // ── Shape ────────────────────────────────────────────────────────────────

  /** Stand it down the side (true) or along the top (false, the default);
   *  the items are laid out again. */
  void setVertical(bool vertical) {
    if (vertical == vertical_) return;
    vertical_ = vertical;
    overflowButton_.vertical = vertical;
    resized();
    repaint();
  }
  /** What setVertical() set. */
  bool isVertical() const { return vertical_; }
  /** Across the toolbar, logical units: the height of a horizontal one, the
   *  width of a vertical one. Items are this thick. */
  float thickness() const { return vertical_ ? localBounds().w : localBounds().h; }
  /** Along the toolbar, logical units: the room the items are laid out in. */
  float length() const { return vertical_ ? localBounds().h : localBounds().w; }

  /** Give every item this style and lay them out again; calls
   *  onItemsChanged. Items added later take it too. */
  void setStyle(ToolbarItemStyle style) {
    if (style == style_) return;
    style_ = style;
    for (auto& item : items_) item->setStyle(style);
    resized();
    repaint();
    changed();
  }
  /** The style items are given (iconsWithText by default). */
  ToolbarItemStyle style() const { return style_; }

  // ── Items ────────────────────────────────────────────────────────────────

  /** Every item on the toolbar, those behind the chevron included. */
  int numItems() const { return (int) items_.size(); }
  /** The id of the item at `index` (0-based, along the toolbar); 0 when
   *  `index` is out of range. */
  int itemId(int index) const {
    return index >= 0 && index < (int) items_.size() ? items_[(size_t) index]->itemId() : 0;
  }
  /** The item at `index`, owned by the toolbar; null out of range. */
  ToolbarItemComponent* itemComponent(int index) const {
    return index >= 0 && index < (int) items_.size() ? items_[(size_t) index].get() : nullptr;
  }
  /** The 0-based index of `item` on this toolbar, or -1. */
  int indexOfItem(const Component* item) const {
    for (size_t i = 0; i < items_.size(); ++i)
      if (items_[i].get() == item) return (int) i;
    return -1;
  }

  /** Make an item with the factory and put it at `insertIndex` (-1: the end).
   *  False when the factory did not make one. */
  bool addItem(ToolbarItemFactory& factory, int itemId, int insertIndex = -1) {
    std::unique_ptr<ToolbarItemComponent> item = makeItem(factory, itemId);
    if (!item) return false;
    insertItem(std::move(item), insertIndex);
    return true;
  }

  /** Append the factory's default set (getDefaultItemSet), skipping ids it
   *  does not make. Items already on the toolbar stay. */
  void addDefaultItems(ToolbarItemFactory& factory) {
    std::vector<int> ids;
    factory.getDefaultItemSet(ids);
    for (int id : ids) addItem(factory, id);
  }

  /** Take an item off, destroying it. */
  void removeToolbarItem(int index) { removeAndReturnItem(index); }

  /** Take an item off and hand it back. */
  std::unique_ptr<ToolbarItemComponent> removeAndReturnItem(int index) {
    if (index < 0 || index >= (int) items_.size()) return nullptr;
    std::unique_ptr<ToolbarItemComponent> item = std::move(items_[(size_t) index]);
    items_.erase(items_.begin() + (long) index);
    removeChild(item.get());
    item->toolbar_ = nullptr;
    item->setVisible(true);
    item->setEditingMode(ToolbarItemComponent::EditingMode::normalMode);
    resized();
    repaint();
    changed();
    return item;
  }

  /** Remove and destroy every item; calls onItemsChanged once per item. */
  void clear() {
    while (!items_.empty()) removeAndReturnItem((int) items_.size() - 1);
  }

  /** Move an item to a new place along the toolbar. */
  void moveItem(int from, int to) {
    if (from < 0 || from >= (int) items_.size()) return;
    if (to < 0 || to > (int) items_.size()) to = (int) items_.size();
    // `to` is an INSERTION index counted before the item is lifted out, so
    // moving forwards lands one earlier once it has been.
    if (to > from) --to;
    if (to == from) return;
    std::unique_ptr<ToolbarItemComponent> item = std::move(items_[(size_t) from]);
    items_.erase(items_.begin() + (long) from);
    items_.insert(items_.begin() + (long) to, std::move(item));
    resized();
    repaint();
    changed();
  }

  /** Fired whenever the arrangement or the style changes, so an owner can
   *  save toString() somewhere. */
  std::function<void()> onItemsChanged;

  // ── Remembering the arrangement ──────────────────────────────────────────

  /** "TB:" and the item ids in order, as the reference writes it -- so a
   *  saved layout reads the same either side of a port. */
  std::string toString() const {
    std::string out = "TB:";
    for (size_t i = 0; i < items_.size(); ++i) {
      if (i > 0) out += ' ';
      out += std::to_string(items_[i]->itemId());
    }
    return out;
  }

  /**
   * Rebuild from toString()'s text. False, with the toolbar untouched, for
   * text that is not one. An id the factory no longer makes is skipped
   * rather than failing the load: a plugin that dropped a button must not
   * lose the rest of somebody's arrangement over it.
   */
  bool restoreFromString(ToolbarItemFactory& factory, const std::string& saved) {
    if (saved.compare(0, 3, "TB:") != 0) return false;
    std::vector<int> ids;
    const char* p = saved.c_str() + 3;
    while (*p) {
      while (*p == ' ') ++p;
      if (!*p) break;
      char* end = nullptr;
      const long id = std::strtol(p, &end, 10);
      if (end == p) return false;
      if (*end != ' ' && *end != '\0') return false;
      if (id > -1000000 && id < 1000000) ids.push_back((int) id);
      p = end;
    }
    clear();
    for (int id : ids) addItem(factory, id);
    return true;
  }

  // ── Overflow ─────────────────────────────────────────────────────────────

  /** How many items did not fit and are behind the chevron. */
  int numOverflowItems() const { return (int) items_.size() - firstHidden_; }
  /** Some items did not fit and the chevron is showing. */
  bool hasOverflow() const { return firstHidden_ < (int) items_.size(); }
  /** The index of the first item behind the chevron, or numItems(). */
  int firstOverflowIndex() const { return firstHidden_; }
  /** The chevron, in the toolbar's coordinates, at the far end; meaningful
   *  only while hasOverflow(). */
  Rect overflowButtonBounds() const { return overflowButton_.bounds(); }
  /** The overlay the overflow menu opens in, for a test or an owner that
   *  wants to inspect what is showing. */
  PopupMenuOverlay& overflowMenu() { return overflowMenu_; }

  /** Open the menu of items that did not fit. Ids are item index plus one. */
  void showOverflowMenu(bool fromKeyboard = false) {
    if (!hasOverflow()) return;
    PopupMenu menu;
    // Which item each id meant, by address: the toolbar may change while the
    // menu is up, and the index alone then triggered a neighbour.
    std::vector<const Component*> listed;
    for (int i = firstHidden_; i < (int) items_.size(); ++i) {
      ToolbarItemComponent* item = items_[(size_t) i].get();
      listed.push_back(item);
      if (item->itemId() == ToolbarItemFactory::separatorBarId) menu.addSeparator();
      else if (item->itemId() < 0) continue; // a gap means nothing in a menu
      else menu.addItem(i + 1, item->textLabel(), item->isBeingUsedAsAButton() && item->isEnabled());
    }
    overflowMenu_.show(overflowButton_, menu, overflowButton_.localBounds(),
                       [this, listed, first = firstHidden_](int id) {
                         const int at = id - 1 - first;
                         if (at < 0 || at >= (int) listed.size()) return;
                         // Looked up, never dereferenced: an item removed
                         // since is simply not found.
                         const int index = indexOfItem(listed[(size_t) at]);
                         if (index >= 0) items_[(size_t) index]->triggerFromMenu();
                       },
                       vertical_ ? PopupMenuOverlay::Placement::Beside : PopupMenuOverlay::Placement::Under,
                       fromKeyboard);
  }

  // ── Customising ──────────────────────────────────────────────────────────

  /** Put every item into (or out of) the mode where a press drags it. */
  void setEditingActive(bool active) {
    if (active == editing_) return;
    editing_ = active;
    for (auto& item : items_)
      item->setEditingMode(active ? ToolbarItemComponent::EditingMode::editableOnToolbar
                                  : ToolbarItemComponent::EditingMode::normalMode);
    repaint();
  }
  /** Items are in the mode where a press drags them (setEditingActive, or
   *  while the customisation dialog is up). */
  bool isEditingActive() const { return editing_; }

  /** The palette and the style choice, in a dialog over the editor. */
  void showCustomisationDialog(ToolbarItemFactory& factory,
                               int optionFlags = allCustomisationOptionsEnabled);
  /** The customisation dialog is up. */
  bool isCustomisationDialogShowing() const;
  /** Take the dialog down (Done and Escape do this) and end editing mode.
   *  Safe to call from inside the dialog: it is freed at the next open or
   *  close, not now. Does nothing when no dialog is up. */
  void closeCustomisationDialog();
  /** The palette inside the customisation dialog, or null when it is not up. */
  ToolbarItemPalette* customisationPalette() const;

  /** Where an item dropped at `p` (this component's coordinates) would go. */
  int insertionIndexFor(Point p) const {
    const float along = vertical_ ? p.y : p.x;
    for (int i = 0; i < firstHidden_ && i < (int) items_.size(); ++i) {
      const Rect b = items_[(size_t) i]->bounds();
      const float mid = vertical_ ? b.centre().y : b.centre().x;
      if (along < mid) return i;
    }
    return firstHidden_;
  }

  // ── Drag and drop ────────────────────────────────────────────────────────

  bool isInterestedInDrag(const DragSource& source) override {
    return editing_ && source.description.compare(0, std::string(ToolbarItemComponent::kDragPrefix).size(),
                                                  ToolbarItemComponent::kDragPrefix) == 0;
  }

  /** Moves the insertion mark to where a drop at `p` would land. */
  void itemDragMove(const DragSource&, Point p) override {
    const int at = insertionIndexFor(p);
    if (at != insertAt_) {
      insertAt_ = at;
      repaint();
    }
  }

  /** Hides the insertion mark. */
  void itemDragExit(const DragSource&) override {
    insertAt_ = -1;
    repaint();
  }

  /** An item of this toolbar moves to the drop point; one from a palette is
   *  made there, or moved there if a regular item with its id is already on
   *  the toolbar. */
  void itemDropped(const DragSource& source, Point p) override;

  // ── Component ────────────────────────────────────────────────────────────

  /** Lays the items out along the length: preferred sizes, spare room to
   *  the flexible spacers, and what does not fit behind the chevron. */
  void resized() override { layoutItems(); }

  /** The look and feel's drawToolbarBackground; the items draw themselves. */
  void paint(Graphics& g) override {
    LookAndFeel& lf = lookAndFeel();
    lf.drawToolbarBackground(g, localBounds(), vertical_);
  }

  /** The insertion mark goes over the items, so it is drawn by a child that
   *  is always on top rather than by paint(), which is underneath them. */
  Rect insertionMarkBounds() const {
    if (insertAt_ < 0) return {};
    float at = 0.0f;
    if (insertAt_ < (int) items_.size() && insertAt_ < firstHidden_) {
      const Rect b = items_[(size_t) insertAt_]->bounds();
      at = vertical_ ? b.y : b.x;
    } else if (firstHidden_ > 0 && !items_.empty()) {
      const Rect b = items_[(size_t) (firstHidden_ - 1)]->bounds();
      at = vertical_ ? b.bottom() : b.right();
    }
    return vertical_ ? Rect(2.0f, at - 1.0f, thickness() - 4.0f, 2.0f)
                     : Rect(at - 1.0f, 2.0f, 2.0f, thickness() - 4.0f);
  }

  AccessibleInfo accessibleInfo() const override { return baseInfo(AccessibleRole::Group); }

private:
  friend class ToolbarItemComponent;
  friend class ToolbarItemPalette;

  /** Draws the insertion mark above the items. */
  class InsertionMark : public Component {
  public:
    Toolbar* owner = nullptr;
    InsertionMark() {
      setInterceptsMouse(false);
      setAccessibilityIgnored(true);
    }
    void paint(Graphics& g) override {
      if (!owner) return;
      const Rect mark = owner->insertionMarkBounds();
      if (!mark.isEmpty()) owner->lookAndFeel().drawToolbarInsertionMark(g, mark);
    }
  };

  std::unique_ptr<ToolbarItemComponent> makeItem(ToolbarItemFactory& factory, int itemId) {
    if (itemId == ToolbarItemFactory::separatorBarId || itemId == ToolbarItemFactory::spacerId ||
        itemId == ToolbarItemFactory::flexibleSpacerId)
      return std::unique_ptr<ToolbarItemComponent>(new detail::ToolbarSpacerItem(itemId));
    if (itemId < 0) return nullptr;
    return factory.createItem(itemId);
  }

  void insertItem(std::unique_ptr<ToolbarItemComponent> item, int index) {
    if (index < 0 || index > (int) items_.size()) index = (int) items_.size();
    ToolbarItemComponent* raw = item.get();
    raw->toolbar_ = this;
    raw->setStyle(style_);
    raw->setEditingMode(editing_ ? ToolbarItemComponent::EditingMode::editableOnToolbar
                                 : ToolbarItemComponent::EditingMode::normalMode);
    items_.insert(items_.begin() + (long) index, std::move(item));
    addChild(raw);
    // The chevron and the mark stay above every item.
    overflowButton_.toFront();
    if (mark_.parent() == this) mark_.toFront();
    resized();
    repaint();
    changed();
  }

  void changed() {
    if (onItemsChanged) onItemsChanged();
  }

  /**
   * Lay the items out along the length.
   *
   * Preferred sizes first. Room left over goes to the flexible spacers, in
   * equal shares. When the preferred sizes do not fit, the items that fit
   * before the chevron stay and the rest are hidden behind it -- hidden, not
   * squashed: a button squashed below its icon is a button nobody can read.
   */
  void layoutItems() {
    const float len = length(), thick = thickness();
    const size_t n = items_.size();
    std::vector<float> pref(n), lo(n), hi(n);
    float total = 0.0f;
    int flexible = 0;
    for (size_t i = 0; i < n; ++i) {
      float p = thick, mn = thick, mx = thick;
      if (!items_[i]->toolbarItemSizes(thick, vertical_, p, mn, mx)) p = mn = mx = thick;
      if (!(p >= 0.0f)) p = 0.0f;
      pref[i] = p;
      lo[i] = mn;
      hi[i] = mx;
      total += p;
      if (hi[i] > p) ++flexible;
    }

    firstHidden_ = (int) n;
    float extraEach = 0.0f;
    if (total > len + 0.01f) {
      const float room = len - kOverflowButtonSize;
      float used = 0.0f;
      int fit = 0;
      while (fit < (int) n && used + pref[(size_t) fit] <= room + 0.01f) used += pref[(size_t) fit++];
      firstHidden_ = fit;
    } else if (flexible > 0) {
      extraEach = (len - total) / (float) flexible;
    }

    float at = 0.0f;
    for (size_t i = 0; i < n; ++i) {
      ToolbarItemComponent* item = items_[i].get();
      if ((int) i >= firstHidden_) {
        item->setVisible(false);
        continue;
      }
      float size = pref[i];
      if (hi[i] > pref[i]) size = (std::min)(hi[i], pref[i] + extraEach);
      item->setVisible(true);
      item->setBounds(vertical_ ? Rect(0.0f, at, thick, size) : Rect(at, 0.0f, size, thick));
      at += size;
    }

    const bool overflow = firstHidden_ < (int) n;
    overflowButton_.setVisible(overflow);
    if (overflow)
      overflowButton_.setBounds(vertical_ ? Rect(0.0f, len - kOverflowButtonSize, thick, kOverflowButtonSize)
                                          : Rect(len - kOverflowButtonSize, 0.0f, kOverflowButtonSize, thick));
    if (mark_.parent() == this) mark_.setBounds(localBounds());
  }

  std::vector<std::unique_ptr<ToolbarItemComponent>> items_;
  std::vector<ToolbarItemPalette*> palettes_;
  detail::ToolbarOverflowButton overflowButton_;
  PopupMenuOverlay overflowMenu_;
  InsertionMark mark_;
  std::unique_ptr<detail::ToolbarCustomiser> customiser_;
  /** A closed dialog, kept until the next open or close: it is closed from
   *  its own Done button and Escape key, and destroying it there would free
   *  the button whose click is still on the stack. */
  std::unique_ptr<detail::ToolbarCustomiser> retired_;
  ToolbarItemStyle style_ = ToolbarItemStyle::iconsWithText;
  int firstHidden_ = 0;
  int insertAt_ = -1;
  bool vertical_ = false;
  bool editing_ = false;
};

inline bool detail::ToolbarSpacerItem::isVerticalToolbar() const {
  return toolbar() && toolbar()->isVertical();
}

/**
 * Every item the factory can make, to be dragged onto a toolbar.
 *
 * Items dragged off the toolbar and dropped here are removed from it. A
 * regular item already on the toolbar can still be dragged from here, and is
 * MOVED to where it lands rather than added twice: two Save buttons on one
 * toolbar is never what anybody meant. Separators and spacers can be added as
 * often as wanted.
 */
class ToolbarItemPalette : public Widget {
public:
  /** The space between items and around them, logical units. */
  static constexpr float kGap = 6.0f;

  /** One item of each id getAllToolbarItemIds lists, made now and owned
   *  by the palette. Both references are kept: the factory and the toolbar
   *  must outlive the palette. */
  ToolbarItemPalette(ToolbarItemFactory& factory, Toolbar& toolbar) : factory_(factory), toolbar_(toolbar) {
    setAccessibleName("Toolbar items");
    toolbar_.palettes_.push_back(this);
    viewport_.setViewedComponent(&content_);
    addChild(&viewport_);
    std::vector<int> ids;
    factory_.getAllToolbarItemIds(ids);
    for (int id : ids) {
      std::unique_ptr<ToolbarItemComponent> item = toolbar_.makeItem(factory_, id);
      if (!item) continue;
      item->setStyle(toolbar_.style());
      item->setEditingMode(ToolbarItemComponent::EditingMode::editableOnPalette);
      content_.addChild(item.get());
      items_.push_back(std::move(item));
    }
  }

  /** Tells the toolbar it no longer accepts drops from this palette. */
  ~ToolbarItemPalette() override {
    auto& list = toolbar_.palettes_;
    for (size_t i = 0; i < list.size(); ++i)
      if (list[i] == this) {
        list.erase(list.begin() + (long) i);
        break;
      }
  }

  /** The factory the items came from; a drop on the toolbar makes the new
   *  item with it. */
  ToolbarItemFactory& factory() const { return factory_; }
  /** The toolbar items are dragged onto and off. */
  Toolbar& toolbar() const { return toolbar_; }
  /** How many items the palette shows (ids the factory could not make are
   *  left out). */
  int numItems() const { return (int) items_.size(); }
  /** The palette's item at `index` (0-based, in getAllToolbarItemIds
   *  order); null out of range. */
  ToolbarItemComponent* itemComponent(int index) const {
    return index >= 0 && index < (int) items_.size() ? items_[(size_t) index].get() : nullptr;
  }
  /** `c` is one of the palette's own items, the source of a drag from it. */
  bool ownsItem(const Component* c) const {
    for (const auto& item : items_)
      if (item.get() == c) return true;
    return false;
  }

  /** Items flow left to right and wrap, each at the toolbar's thickness. */
  void resized() override {
    // Inset, so the drop outline drawn by paint() shows around it.
    viewport_.setBounds(localBounds().reduced(2.0f));
    const float thick = (std::max)(24.0f, toolbar_.thickness());
    const float width = viewport_.bounds().w - ScrollBar::kThickness;
    float x = kGap, y = kGap;
    for (auto& item : items_) {
      item->setStyle(toolbar_.style());
      float p = thick, mn = thick, mx = thick;
      if (!item->toolbarItemSizes(thick, false, p, mn, mx)) p = thick;
      if (p < thick) p = thick;
      if (x + p > width && x > kGap) {
        x = kGap;
        y += thick + kGap;
      }
      item->setBounds({x, y, p, thick});
      x += p + kGap;
    }
    content_.setBounds({0.0f, 0.0f, (std::max)(0.0f, width), y + thick + kGap});
  }

  /** Outlined while a toolbar item is dragged over it: the outline is the
   *  only sign that letting go here removes the item. */
  void paint(Graphics& g) override {
    LookAndFeel& lf = lookAndFeel();
    g.fillAll(lf.panel());
    if (dropHover_) {
      g.setColour(lf.accent());
      g.drawRect(localBounds().reduced(1.0f), 2.0f);
    }
  }

  /** Takes drags of the toolbar's own items only. */
  bool isInterestedInDrag(const DragSource& source) override {
    return toolbar_.indexOfItem(source.source) >= 0;
  }
  /** Shows the drop outline. */
  void itemDragEnter(const DragSource&) override {
    dropHover_ = true;
    repaint();
  }
  /** Hides the drop outline. */
  void itemDragExit(const DragSource&) override {
    dropHover_ = false;
    repaint();
  }
  /** An item from the toolbar dropped here comes off it. */
  void itemDropped(const DragSource& source, Point) override {
    dropHover_ = false;
    const int index = toolbar_.indexOfItem(source.source);
    if (index >= 0) toolbar_.removeToolbarItem(index);
    repaint();
  }

  AccessibleInfo accessibleInfo() const override { return baseInfo(AccessibleRole::Group); }

private:
  ToolbarItemFactory& factory_;
  Toolbar& toolbar_;
  Viewport viewport_;
  Component content_;
  std::vector<std::unique_ptr<ToolbarItemComponent>> items_;
  bool dropHover_ = false;
};

namespace detail {

/** The customisation dialog: the palette, the style choice, Reset and Done. */
class ToolbarCustomiser : public Overlay {
public:
  ToolbarCustomiser(Toolbar& toolbar, ToolbarItemFactory& factory, int flags)
      : toolbar_(toolbar), factory_(factory), flags_(flags), palette_(factory, toolbar) {
    setWantsKeyboardFocus(true);
    setDimAlpha(110);
    setAccessibleName("Customise toolbar");
    onDismiss = [this]() { toolbar_.closeCustomisationDialog(); };
    addChild(&palette_);

    const struct {
      int flag;
      ToolbarItemStyle style;
      const char* name;
    } choices[] = {{Toolbar::allowIconsOnlyChoice, ToolbarItemStyle::iconsOnly, "Icons only"},
                   {Toolbar::allowIconsWithTextChoice, ToolbarItemStyle::iconsWithText, "Icons with text"},
                   {Toolbar::allowTextOnlyChoice, ToolbarItemStyle::textOnly, "Text only"}};
    std::vector<std::string> names;
    for (const auto& c : choices)
      if (flags_ & c.flag) {
        names.push_back(c.name);
        styles_.push_back(c.style);
      }
    if (!styles_.empty()) {
      styleBox_.setItems(names);
      for (size_t i = 0; i < styles_.size(); ++i)
        if (styles_[i] == toolbar_.style()) styleBox_.setSelectedIndex((int) i, false);
      styleBox_.setAccessibleName("Toolbar style");
      styleBox_.onChange = [this](int index) {
        if (index >= 0 && index < (int) styles_.size()) toolbar_.setStyle(styles_[(size_t) index]);
        palette_.resized();
      };
      addChild(&styleBox_);
    }
    if (flags_ & Toolbar::showResetToDefaultsButton) {
      reset_.onClick = [this]() {
        toolbar_.clear();
        toolbar_.addDefaultItems(factory_);
      };
      addChild(&reset_);
    }
    done_.onClick = [this]() { toolbar_.closeCustomisationDialog(); };
    addChild(&done_);
  }

  ToolbarItemPalette& palette() { return palette_; }

  /** Where the toolbar is, in this overlay's coordinates: not dimmed, and not
   *  in the way of the mouse, because it is where things are dropped. */
  Rect hole() const {
    const Rect b = toolbar_.localBounds();
    const Point p = toolbar_.localToRoot({0.0f, 0.0f});
    const Point o = localToRoot({0.0f, 0.0f});
    return {p.x - o.x, p.y - o.y, b.w, b.h};
  }

  Rect panelBounds() const {
    const Rect area = localBounds();
    const Rect h = hole();
    Rect free = area;
    if (toolbar_.isVertical()) {
      const float left = h.x - area.x, right = area.right() - h.right();
      free = left > right ? Rect(area.x, area.y, left, area.h) : Rect(h.right(), area.y, right, area.h);
    } else {
      const float above = h.y - area.y, below = area.bottom() - h.bottom();
      free = above > below ? Rect(area.x, area.y, area.w, above) : Rect(area.x, h.bottom(), area.w, below);
    }
    const float w = (std::min)(free.w - 16.0f, 560.0f), ht = (std::min)(free.h - 16.0f, 380.0f);
    return {free.x + (free.w - w) * 0.5f, free.y + (free.h - ht) * 0.5f, (std::max)(0.0f, w), (std::max)(0.0f, ht)};
  }

  void resized() override {
    const Rect panel = panelBounds();
    const float pad = 10.0f, rowH = 26.0f;
    const float textH = font().lineHeight() + 6.0f;
    palette_.setBounds({panel.x + pad, panel.y + pad + textH, panel.w - pad * 2.0f,
                        (std::max)(0.0f, panel.h - pad * 3.0f - textH - rowH)});
    const float rowY = panel.bottom() - pad - rowH;
    float x = panel.x + pad;
    if (styleBox_.parent() == this) {
      styleBox_.setBounds({x, rowY, 140.0f, rowH});
      x += 150.0f;
    }
    if (reset_.parent() == this) reset_.setBounds({x, rowY, 130.0f, rowH});
    done_.setBounds({panel.right() - pad - 80.0f, rowY, 80.0f, rowH});
  }

  bool hitTestPoint(Point p) const override { return !hole().contains(p); }

  void paint(Graphics& g) override {
    const Rect a = localBounds(), h = hole();
    if (dim_ > 0) {
      // Everything but the toolbar, as four bands around it.
      g.setColour(Colour(0, 0, 0, dim_));
      g.fillRect(Rect(a.x, a.y, a.w, h.y - a.y));
      g.fillRect(Rect(a.x, h.bottom(), a.w, a.bottom() - h.bottom()));
      g.fillRect(Rect(a.x, h.y, h.x - a.x, h.h));
      g.fillRect(Rect(h.right(), h.y, a.right() - h.right(), h.h));
    }
    LookAndFeel& lf = lookAndFeel();
    const Rect panel = panelBounds();
    g.setColour(lf.background());
    g.fillRoundedRect(panel, 6.0f);
    g.setColour(lf.outline());
    g.drawRoundedRect(panel, 6.0f, 1.0f);
    g.setColour(lf.text());
    font().drawIn(g, "Drag items onto the toolbar to add them, along it to move them, and back here to remove them.",
                  Rect(panel.x + 10.0f, panel.y + 6.0f, panel.w - 20.0f, font().lineHeight() + 6.0f),
                  Justify::Left);
  }

  AccessibleInfo accessibleInfo() const override { return baseInfo(AccessibleRole::Dialog); }

private:
  Toolbar& toolbar_;
  ToolbarItemFactory& factory_;
  int flags_;
  ToolbarItemPalette palette_;
  ComboBox styleBox_;
  std::vector<ToolbarItemStyle> styles_;
  Button reset_{"Reset to defaults"};
  Button done_{"Done"};
};

} // namespace detail

inline Toolbar::Toolbar() {
  setAccessibleName("Toolbar");
  overflowButton_.onOpen = [this](bool fromKeyboard) { showOverflowMenu(fromKeyboard); };
  addChild(&overflowButton_);
  overflowButton_.setVisible(false);
}

inline Toolbar::~Toolbar() {
  // The dialogs first: their palettes unregister from this toolbar on the way
  // out, and the list they unregister from is a member that has to still be
  // here for it. A palette a caller made itself must not outlive the toolbar
  // it was made for -- it holds a reference to it.
  customiser_.reset();
  retired_.reset();
}

inline void Toolbar::showCustomisationDialog(ToolbarItemFactory& factory, int optionFlags) {
  closeCustomisationDialog();
  retired_.reset();
  Component* host = root();
  if (host == this) return; // nowhere to put a dialog but on top of itself
  customiser_.reset(new detail::ToolbarCustomiser(*this, factory, optionFlags));
  host->addChild(customiser_.get());
  customiser_->coverParent();
  customiser_->setVisible(true);
  customiser_->resized();
  setEditingActive(true);
  if (!mark_.parent()) {
    mark_.owner = this;
    addChild(&mark_);
    mark_.setBounds(localBounds());
  }
  if (MouseRouter* r = routerFor(this)) r->setFocus(customiser_.get());
}

inline bool Toolbar::isCustomisationDialogShowing() const { return customiser_ != nullptr; }

inline ToolbarItemPalette* Toolbar::customisationPalette() const {
  return customiser_ ? &customiser_->palette() : nullptr;
}

inline void Toolbar::closeCustomisationDialog() {
  if (!customiser_) return;
  // Detached now, destroyed later. This is reached from the dialog's own
  // Done button and Escape key, so the dialog is still on the stack: taking
  // it out of the tree makes the router forget everything inside it -- the
  // release that follows goes nowhere -- and keeping the object alive until
  // the next open or close means nothing returns into freed memory.
  retired_ = std::move(customiser_);
  retired_->setVisible(false);
  if (Component* p = retired_->parent()) p->removeChild(retired_.get());
  setEditingActive(false);
  insertAt_ = -1;
  if (mark_.parent() == this) removeChild(&mark_);
  repaint();
}

inline void ToolbarItemComponent::DragHandle::mouseDown(const MouseEvent&) {
  started_ = false;
  // The router has just taken the keyboard away -- a handle does not want it
  // -- which would leave the open dialog deaf to Escape. Given back to the
  // dialog: the palette's items sit inside it, and a toolbar item's toolbar
  // knows it.
  MouseRouter* router = routerFor(this);
  if (!router || !item) return;
  if (Component* dialog = item->customisationDialog()) router->setFocus(dialog);
}

inline Component* ToolbarItemComponent::customisationDialog() const {
  // On a toolbar, its own dialog. In a palette, the palette is inside the
  // dialog: the nearest ancestor that takes the keyboard is it.
  if (toolbar_) return toolbar_->customiser_.get();
  for (Component* c = parent(); c; c = c->parent())
    if (c->wantsKeyboardFocus()) return c;
  return nullptr;
}

inline void Toolbar::itemDropped(const DragSource& source, Point p) {
  const int at = insertionIndexFor(p);
  insertAt_ = -1;
  repaint();
  const int existing = indexOfItem(source.source);
  if (existing >= 0) {
    moveItem(existing, at);
    return;
  }
  for (ToolbarItemPalette* palette : palettes_) {
    if (!palette->ownsItem(source.source)) continue;
    const int id = std::atoi(source.description.c_str() + std::string(ToolbarItemComponent::kDragPrefix).size());
    // A regular item that is already here MOVES rather than appearing twice.
    // Regular is 0 and up: only the negative ids are separators and spacers,
    // and makeItem asks the factory for 0. `id > 0` added an item 0 twice.
    if (id >= 0)
      for (int i = 0; i < (int) items_.size(); ++i)
        if (items_[(size_t) i]->itemId() == id) {
          moveItem(i, at);
          return;
        }
    addItem(palette->factory(), id, at);
    return;
  }
}

} // namespace gfx
} // namespace sonore
