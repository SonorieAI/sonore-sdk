// SPDX-License-Identifier: Apache-2.0
//
// KeyMappingEditorComponent: letting the user choose the keys.
//
// ── What it edits ───────────────────────────────────────────────────────────
//
// commands.h's CommandManager: named commands with a category and default
// keys, and the table of which keys reach which command. This lists the
// commands under their categories with their keys drawn as keycaps beside
// them. A click on "+" -- or Return on a row -- waits for a key and assigns
// it; a click on a keycap offers to change or remove it; Delete removes a
// row's last key; and "Reset to defaults" puts every key back where the
// plugin registered it. saveMappings() on the manager is what an owner
// writes to its settings when onMappingsChanged fires.
//
// ── A key belongs to one command ────────────────────────────────────────────
//
// CommandManager already enforces that -- assigning a key takes it from
// whoever had it -- and silently. That is right for code and wrong for a
// person: pressing Ctrl+S for "Save As" and finding "Save" has lost its key
// next time they reach for it is the bug report this editor would generate.
// So a key that is already taken is a QUESTION, asked in an AlertOverlay,
// naming the command that would lose it; "Reassign" moves it and "Cancel"
// changes nothing.
//
// ── Capturing a key ─────────────────────────────────────────────────────────
//
// While waiting, the editor holds the keyboard and the next key IS the
// answer -- Tab and Return included, since both are keys somebody may want.
// Escape alone cancels, which leaves Escape itself unassignable from here; a
// command that wants it registers it as a default. A click anywhere cancels
// too.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "../commands.h"
#include "alert.h"
#include "popup_overlay.h"
#include "viewport.h"
#include "widgets.h"

namespace sonore {
namespace gfx {

/**
 * A list of a CommandManager's commands, by category, where the user assigns
 * and removes their keys -- the reference's KeyMappingEditorComponent. The
 * file comment covers what each click and key does, and why a key that is
 * already taken is asked about rather than silently moved.
 *
 * Holds a REFERENCE to the manager, which must outlive it, and edits the
 * manager's table directly; persisting it is the owner's job, with
 * saveMappings() from onMappingsChanged. The list is read once on
 * construction: call refresh() after registering more commands. Message
 * thread only.
 */
class KeyMappingEditorComponent : public Widget {
public:
  /** A command row's height, logical units. */
  static constexpr float kRowHeight = 26.0f;
  /** A category heading's height, logical units. */
  static constexpr float kHeadingHeight = 24.0f;
  /** The strip under the list that holds "Reset to defaults"; not taken
   *  when the button is not shown. */
  static constexpr float kButtonRowHeight = 36.0f;
  /** The space between keycaps on a row. */
  static constexpr float kChipGap = 4.0f;
  /** The chip index of the "+" at the end of a row. */
  static constexpr int kAddChip = -2;

  /** An editor for `commands`, which must outlive it. Without
   *  `showResetToDefaultButton` the list takes the whole height and
   *  resetToDefaults() is still there to call. */
  explicit KeyMappingEditorComponent(CommandManager& commands, bool showResetToDefaultButton = true)
      : commands_(commands) {
    setWantsKeyboardFocus(true);
    setAccessibleName("Key mappings");
    addChild(&scrollBar_);
    scrollBar_.onScroll = [this](float y) {
      scroll_ = y;
      repaint();
    };
    reset_.setLabel("Reset to defaults");
    reset_.onClick = [this]() { confirmReset(); };
    if (showResetToDefaultButton) addChild(&reset_);
    addChild(&alert_);
    refresh();
  }

  /** Leave a command out of the list -- an internal one the user should not
   *  see. Asked on refresh(). */
  std::function<bool(int commandId)> shouldCommandBeIncluded;
  /** A command listed but not editable: its keys are shown and cannot be
   *  changed. */
  std::function<bool(int commandId)> isCommandReadOnly;
  /** Something was assigned, removed or reset. Save the manager's
   *  saveMappings() here. */
  std::function<void()> onMappingsChanged;

  /** Read the commands again. Called on construction; call it after
   *  registering more. */
  void refresh() {
    rows_.clear();
    std::vector<std::string> categories;
    const std::vector<int> ids = commands_.commandIds();
    for (int id : ids) {
      const CommandInfo* info = commands_.commandInfo(id);
      if (!info || (shouldCommandBeIncluded && !shouldCommandBeIncluded(id))) continue;
      bool known = false;
      for (const std::string& c : categories) known = known || c == info->category;
      if (!known) categories.push_back(info->category);
    }
    for (const std::string& category : categories) {
      Row heading;
      heading.heading = true;
      heading.text = category.empty() ? "Other" : category;
      rows_.push_back(heading);
      for (int id : ids) {
        const CommandInfo* info = commands_.commandInfo(id);
        if (!info || info->category != category) continue;
        if (shouldCommandBeIncluded && !shouldCommandBeIncluded(id)) continue;
        Row row;
        row.command = id;
        row.text = info->name;
        rows_.push_back(row);
      }
    }
    if (selected_ >= (int) rows_.size() || (selected_ >= 0 && rows_[(size_t) selected_].heading))
      selected_ = -1;
    updateScrollBar();
    repaint();
  }

  // ── What is on screen ────────────────────────────────────────────────────

  /** Headings and commands together: each category's heading ("Other" for
   *  an empty category), then its commands in the manager's order. */
  int numRows() const { return (int) rows_.size(); }
  /** `row` is a category heading; false out of range. */
  bool isHeading(int row) const { return row >= 0 && row < (int) rows_.size() && rows_[(size_t) row].heading; }
  /** The command on a row, or 0 for a heading. */
  int commandAtRow(int row) const {
    return row >= 0 && row < (int) rows_.size() ? rows_[(size_t) row].command : 0;
  }
  /** The row showing `commandId`, or -1 when it is not listed (left out by
   *  shouldCommandBeIncluded, or registered since the last refresh). */
  int rowForCommand(int commandId) const {
    for (int i = 0; i < (int) rows_.size(); ++i)
      if (rows_[(size_t) i].command == commandId && commandId != 0) return i;
    return -1;
  }
  /** "Save: Ctrl+S, F2" -- what a reader hears for the row. */
  std::string rowText(int row) const {
    if (row < 0 || row >= (int) rows_.size()) return {};
    const Row& r = rows_[(size_t) row];
    if (r.heading) return r.text;
    std::string out = r.text + ":";
    const std::vector<Shortcut> keys = commands_.shortcutsFor(r.command);
    if (keys.empty()) out += " no key";
    for (size_t i = 0; i < keys.size(); ++i) out += (i ? ", " : " ") + keys[i].describe();
    return out;
  }

  /** The scrolling list, in this component's coordinates: everything but
   *  the scroll bar and the button strip. */
  Rect listBounds() const {
    const Rect b = localBounds();
    const float bottom = reset_.parent() == this ? kButtonRowHeight : 0.0f;
    return {0.0f, 0.0f, b.w - (scrollBar_.isVisible() ? ScrollBar::kThickness : 0.0f),
            (std::max)(0.0f, b.h - bottom)};
  }

  /** Where a row is, in this component's coordinates (scrolled, so it may
   *  lie outside the list); empty out of range. */
  Rect rowBounds(int row) const {
    if (row < 0 || row >= (int) rows_.size()) return {};
    return {0.0f, topOf(row) - scroll_, listBounds().w, heightOf(row)};
  }

  /** A keycap on a row: its index among the command's keys, or kAddChip for
   *  the "+" after them. Empty for a heading or a chip that is not there. */
  Rect chipBounds(int row, int chip) const {
    if (row < 0 || row >= (int) rows_.size() || rows_[(size_t) row].heading) return {};
    const Rect r = rowBounds(row);
    const std::vector<Shortcut> keys = commands_.shortcutsFor(rows_[(size_t) row].command);
    float x = r.x + r.w * kNameShare;
    for (int i = 0; i < (int) keys.size(); ++i) {
      const float w = chipWidth(keys[(size_t) i].describe());
      if (i == chip) return {x, r.y + 3.0f, w, r.h - 6.0f};
      x += w + kChipGap;
    }
    if (chip == kAddChip && !readOnly(rows_[(size_t) row].command)) return {x, r.y + 3.0f, 24.0f, r.h - 6.0f};
    return {};
  }

  /** The selected command row, or -1. */
  int selectedRow() const { return selected_; }
  /** Select a command row and scroll it into view; a negative `row`
   *  selects none. A heading or an index past the end is ignored. */
  void setSelectedRow(int row) {
    if (row >= (int) rows_.size() || (row >= 0 && rows_[(size_t) row].heading)) return;
    if (row < 0) row = -1;
    if (row == selected_) return;
    selected_ = row;
    scrollToShow(row);
    repaint();
  }

  // ── Assigning ────────────────────────────────────────────────────────────

  /** Wait for a key for `commandId`. With `replacingIndex`, the key it
   *  presses replaces that one of the command's keys rather than joining
   *  them. */
  void startKeyCapture(int commandId, int replacingIndex = -1) {
    if (rowForCommand(commandId) < 0 || readOnly(commandId)) return;
    capturing_ = commandId;
    const std::vector<Shortcut> keys = commands_.shortcutsFor(commandId);
    replacing_ = replacingIndex >= 0 && replacingIndex < (int) keys.size() ? keys[(size_t) replacingIndex]
                                                                          : Shortcut();
    setSelectedRow(rowForCommand(commandId));
    if (MouseRouter* r = routerFor(this)) r->setFocus(this);
    repaint();
  }

  /** Stop waiting for a key, assigning nothing (Escape, a click, or the
   *  keyboard going elsewhere do this). */
  void cancelKeyCapture() {
    if (capturing_ == 0) return;
    capturing_ = 0;
    replacing_ = Shortcut();
    repaint();
  }

  /** Waiting for the key startKeyCapture() asked for. */
  bool isCapturingKey() const { return capturing_ != 0; }
  /** The command the awaited key is for, or 0. */
  int capturingCommand() const { return capturing_; }

  /**
   * Give `shortcut` to a command, asking first if another command has it.
   * What a captured key does; public so a caller can drive it directly.
   * `replacing`, when valid, is a key of the same command this one replaces.
   */
  void assignShortcut(int commandId, const Shortcut& shortcut, const Shortcut& replacing = Shortcut()) {
    if (!shortcut.isValid() || readOnly(commandId)) return;
    const int owner = commands_.commandForShortcut(shortcut);
    if (owner != 0 && owner != commandId) {
      const CommandInfo* ownerInfo = commands_.commandInfo(owner);
      const CommandInfo* info = commands_.commandInfo(commandId);
      const std::string ownerName = ownerInfo ? ownerInfo->name : std::string("another command");
      // Reassigning takes the key FROM its owner, and a read-only command's
      // keys are the ones that cannot change: said, not asked.
      if (readOnly(owner)) {
        alert_.show("Key already in use",
                    shortcut.describe() + " belongs to \"" + ownerName + "\", which cannot be changed.", {"OK"},
                    [this](int) {
                      if (MouseRouter* r = routerFor(this)) r->setFocus(this);
                    });
        return;
      }
      const std::string message = shortcut.describe() + " is already assigned to \"" + ownerName +
                                  "\". Reassign it to \"" + (info ? info->name : std::string()) + "\"?";
      pendingCommand_ = commandId;
      pendingShortcut_ = shortcut;
      pendingReplacing_ = replacing;
      alert_.show("Key already in use", message, {"Reassign", "Cancel"}, [this](int choice) {
        const int command = pendingCommand_;
        const Shortcut key = pendingShortcut_, replaced = pendingReplacing_;
        pendingCommand_ = 0;
        // The keyboard back first and the change LAST: apply() ends in
        // onMappingsChanged, the owner's code, which may destroy this editor.
        if (MouseRouter* r = routerFor(this)) r->setFocus(this);
        if (choice == 0) apply(command, key, replaced);
      });
      return;
    }
    apply(commandId, shortcut, replacing);
  }

  /** Take one key off a command. */
  void removeShortcut(int commandId, const Shortcut& shortcut) {
    if (readOnly(commandId)) return;
    commands_.removeShortcut(commandId, shortcut);
    changed();
  }

  /** Every key back to the plugin's defaults, asking first. */
  void confirmReset() {
    alert_.show("Reset key mappings", "Put every key back to its default? Keys you have changed will be lost.",
                {"Reset", "Cancel"}, [this](int choice) {
                  // Focus first: resetToDefaults ends in the owner's code.
                  if (MouseRouter* r = routerFor(this)) r->setFocus(this);
                  if (choice == 0) resetToDefaults();
                });
  }

  /** Every key back to the plugin's defaults now, without asking; calls
   *  onMappingsChanged. */
  void resetToDefaults() {
    commands_.resetToDefaults();
    changed();
  }

  /** A question (a key already taken, or the reset) is up and waiting. */
  bool isShowingQuestion() const { return alert_.isVisible(); }
  /** The overlay the questions are asked in, for a test or an owner that
   *  wants to answer or inspect one. */
  AlertOverlay& questionOverlay() { return alert_; }
  /** The overlay a keycap's Change and Remove menu opens in. */
  PopupMenuOverlay& keyMenu() { return keyMenu_; }
  /** The "Reset to defaults" button, owned by the editor; a child only
   *  when the constructor was asked to show it. */
  Button& resetButton() { return reset_; }

  // ── Component ────────────────────────────────────────────────────────────

  /** Places the reset button and the scroll bar, and keeps a question up
   *  covering the editor. */
  void resized() override {
    const Rect b = localBounds();
    if (reset_.parent() == this)
      reset_.setBounds({b.w - 150.0f, b.h - kButtonRowHeight + 5.0f, 144.0f, kButtonRowHeight - 10.0f});
    updateScrollBar();
    if (alert_.isVisible()) {
      alert_.coverParent();
      alert_.resized();
    }
  }

  /** The rows (headings, names, keycaps drawn by drawKeyMappingChip, the
   *  "+"), and while a key is awaited a prompt at the bottom of the list. */
  void paint(Graphics& g) override {
    LookAndFeel& lf = lookAndFeel();
    const Rect list = listBounds();
    g.setColour(lf.panel());
    g.fillRect(localBounds());
    clampScroll();
    {
      Graphics::ScopedState clip(g);
      g.clipTo(list);
      for (int i = 0; i < (int) rows_.size(); ++i) {
        const Rect r = rowBounds(i);
        if (r.bottom() < 0.0f || r.y > list.h) continue;
        const Row& row = rows_[(size_t) i];
        if (row.heading) {
          lf.drawListHeading(g, r, row.text, font());
          continue;
        }
        if (i == selected_) lf.drawListRow(g, r, std::string(), font(), true);
        WidgetState st;
        st.enabled = !readOnly(row.command);
        lf.drawLabel(g, Rect(r.x + 10.0f, r.y, r.w * kNameShare - 14.0f, r.h), row.text, font(), Justify::Left, st);
        const std::vector<Shortcut> keys = commands_.shortcutsFor(row.command);
        for (int k = 0; k < (int) keys.size(); ++k) {
          const bool lit = (i == hoverRow_ && k == hoverChip_) ||
                           (capturing_ == row.command && replacing_.isValid() && keys[(size_t) k] == replacing_);
          lf.drawKeyMappingChip(g, chipBounds(i, k), keys[(size_t) k].describe(), font(), lit);
        }
        const Rect add = chipBounds(i, kAddChip);
        if (!add.isEmpty())
          lf.drawKeyMappingChip(g, add, "+", font(), (i == hoverRow_ && hoverChip_ == kAddChip) ||
                                                         (capturing_ == row.command && !replacing_.isValid()));
      }
    }
    if (capturing_ != 0) paintCapturePrompt(g, lf);
    paintFocusRing(g);
  }

  /** Selects the row; "+" waits for a key, a keycap offers Change and
   *  Remove. While a key is awaited, any click only cancels. */
  void mouseDown(const MouseEvent& e) override {
    if (!isEnabled()) return;
    // A click while waiting for a key is a change of mind.
    if (capturing_ != 0) {
      cancelKeyCapture();
      return;
    }
    if (!listBounds().contains(e.position)) return;
    int chip = 0;
    const int row = hitTestRow(e.position, chip);
    if (row < 0 || rows_[(size_t) row].heading) return;
    setSelectedRow(row);
    const int command = rows_[(size_t) row].command;
    if (chip == kAddChip) {
      startKeyCapture(command);
    } else if (chip >= 0 && !readOnly(command)) {
      offerKeyMenu(row, chip);
    }
  }

  /** Tracks the keycap under the pointer, for its hover look. */
  void mouseMove(const MouseEvent& e) override {
    int chip = -1;
    const int row = listBounds().contains(e.position) ? hitTestRow(e.position, chip) : -1;
    if (row != hoverRow_ || chip != hoverChip_) {
      hoverRow_ = row;
      hoverChip_ = chip;
      repaint();
    }
  }

  /** Clears the hovered keycap. */
  void mouseExit(const MouseEvent& e) override {
    Widget::mouseExit(e);
    hoverRow_ = -1;
    hoverChip_ = -1;
  }

  bool mouseWheel(const MouseEvent&, float delta) override {
    if (delta == 0.0f || contentHeight() <= listBounds().h) return false;
    scroll_ -= delta * ScrollBar::kWheelStep;
    clampScroll();
    scrollBar_.setPosition(scroll_, false);
    repaint();
    return true;
  }

  bool keyPressed(const KeyPress& key) override {
    if (!isEnabled()) return false;
    if (capturing_ != 0) {
      if (key.is(KeyPress::Escape) && !key.ctrlDown && !key.altDown && !key.shiftDown) {
        cancelKeyCapture();
        return true;
      }
      const Shortcut s = shortcutFor(key);
      if (!s.isValid()) return true;
      const int command = capturing_;
      const Shortcut replacing = replacing_;
      capturing_ = 0;
      replacing_ = Shortcut();
      repaint();
      assignShortcut(command, s, replacing);
      return true;
    }
    if (rows_.empty()) return false;
    if (key.is(KeyPress::Down)) return step(+1, selected_);
    if (key.is(KeyPress::Up)) return step(-1, selected_ < 0 ? (int) rows_.size() : selected_);
    if (key.is(KeyPress::Home)) return step(+1, -1);
    if (key.is(KeyPress::End)) return step(-1, (int) rows_.size());
    if (key.is(KeyPress::Return) && selected_ >= 0) {
      startKeyCapture(rows_[(size_t) selected_].command);
      return true;
    }
    if ((key.is(KeyPress::Delete) || key.is(KeyPress::Backspace)) && selected_ >= 0) {
      const int command = rows_[(size_t) selected_].command;
      const std::vector<Shortcut> keys = commands_.shortcutsFor(command);
      if (!keys.empty()) removeShortcut(command, keys.back());
      return true;
    }
    return false;
  }

  /** Cancels a key capture, unless it was a question that took the
   *  keyboard. */
  void focusLost() override {
    // Clicking away while a key is awaited is the same change of mind as a
    // click on the list. The conflict question taking the keyboard is not.
    if (capturing_ != 0 && !alert_.isVisible()) cancelKeyCapture();
  }

  AccessibleInfo accessibleInfo() const override {
    AccessibleInfo info = baseInfo(AccessibleRole::List);
    if (!rows_.empty()) {
      info.hasRange = true;
      info.minValue = 0.0;
      info.maxValue = (double) rows_.size() - 1.0;
      info.currentValue = (double) (selected_ < 0 ? 0 : selected_);
      if (selected_ >= 0) info.value = rowText(selected_);
    }
    if (capturing_ != 0) info.description = "Press a key, or Escape to cancel";
    return info;
  }

  /** The key a KeyPress is, as a Shortcut. */
  static Shortcut shortcutFor(const KeyPress& key) {
    if (key.keyCode != KeyPress::None)
      return Shortcut::fromKeyCode(key.keyCode, key.ctrlDown, key.shiftDown, key.altDown);
    if (key.character != 0) return Shortcut::fromCharacter(key.character, key.ctrlDown, key.shiftDown, key.altDown);
    return Shortcut();
  }

private:
  static constexpr float kNameShare = 0.45f;

  /** One line of the list: a category heading (command 0), or a command and
   *  its name. The keys are not copied here; they are read from the manager
   *  each time, so they cannot disagree with it. */
  struct Row {
    bool heading = false;
    std::string text;
    int command = 0;
  };

  bool readOnly(int commandId) const { return isCommandReadOnly && isCommandReadOnly(commandId); }

  void apply(int commandId, const Shortcut& shortcut, const Shortcut& replacing) {
    if (replacing.isValid() && replacing != shortcut) commands_.removeShortcut(commandId, replacing);
    commands_.addShortcut(commandId, shortcut);
    changed();
  }

  void changed() {
    repaint();
    if (onMappingsChanged) onMappingsChanged();
  }

  float chipWidth(const std::string& text) const {
    const float w = font().stringWidth(text) + 16.0f;
    return w < 36.0f ? 36.0f : w;
  }

  float heightOf(int row) const { return rows_[(size_t) row].heading ? kHeadingHeight : kRowHeight; }
  float topOf(int row) const {
    float y = 0.0f;
    for (int i = 0; i < row; ++i) y += heightOf(i);
    return y;
  }
  float contentHeight() const { return topOf((int) rows_.size()); }

  int hitTestRow(Point p, int& chip) const {
    chip = -1;
    for (int i = 0; i < (int) rows_.size(); ++i) {
      if (!rowBounds(i).contains(p)) continue;
      if (!rows_[(size_t) i].heading) {
        const int count = (int) commands_.shortcutsFor(rows_[(size_t) i].command).size();
        for (int k = 0; k < count; ++k)
          if (chipBounds(i, k).contains(p)) chip = k;
        if (chipBounds(i, kAddChip).contains(p)) chip = kAddChip;
      }
      return i;
    }
    return -1;
  }

  void offerKeyMenu(int row, int chip) {
    const int command = rows_[(size_t) row].command;
    const std::vector<Shortcut> keys = commands_.shortcutsFor(command);
    if (chip < 0 || chip >= (int) keys.size()) return;
    const Shortcut key = keys[(size_t) chip];
    PopupMenu menu;
    menu.addSectionHeader(key.describe());
    menu.addItem(1, "Change this key...");
    menu.addItem(2, "Remove this key");
    keyMenu_.show(*this, menu, chipBounds(row, chip), [this, command, key](int id) {
      if (id == 2) {
        removeShortcut(command, key);
      } else if (id == 1) {
        // By the KEY the menu was opened on, as Remove is, and not by its
        // index then: if the command's keys changed while the menu was up,
        // that index is a neighbour, and Change replaced the wrong key.
        const std::vector<Shortcut> now = commands_.shortcutsFor(command);
        for (size_t i = 0; i < now.size(); ++i)
          if (now[i] == key) startKeyCapture(command, (int) i);
      }
    });
  }

  bool step(int direction, int from) {
    for (int i = from + direction; i >= 0 && i < (int) rows_.size(); i += direction)
      if (!rows_[(size_t) i].heading) {
        setSelectedRow(i);
        return true;
      }
    return true;
  }

  void paintCapturePrompt(Graphics& g, LookAndFeel& lf) {
    const CommandInfo* info = commands_.commandInfo(capturing_);
    const std::string text = "Press a key for \"" + (info ? info->name : std::string()) + "\" -- Escape cancels";
    const Rect list = listBounds();
    const float w = (std::min)(list.w - 20.0f, font().stringWidth(text) + 40.0f);
    const float h = font().lineHeight() + 22.0f;
    const Rect box((list.w - w) * 0.5f, list.h - h - 10.0f, w, h);
    g.setColour(lf.background());
    g.fillRoundedRect(box, 5.0f);
    g.setColour(lf.accent());
    g.drawRoundedRect(box, 5.0f, 1.5f);
    g.setColour(lf.text());
    font().drawIn(g, text, box, Justify::Centred);
  }

  void scrollToShow(int row) {
    if (row < 0) return;
    const float top = topOf(row), bottom = top + heightOf(row);
    const float h = listBounds().h;
    if (top < scroll_) scroll_ = top;
    else if (bottom > scroll_ + h) scroll_ = bottom - h;
    clampScroll();
    scrollBar_.setPosition(scroll_, false);
  }

  void clampScroll() {
    const float most = contentHeight() - listBounds().h;
    if (!(scroll_ <= most)) scroll_ = most;
    if (!(scroll_ >= 0.0f)) scroll_ = 0.0f;
  }

  void updateScrollBar() {
    const Rect b = localBounds();
    const float listH = (std::max)(0.0f, b.h - (reset_.parent() == this ? kButtonRowHeight : 0.0f));
    scrollBar_.setVisible(contentHeight() > listH);
    scrollBar_.setBounds({b.w - ScrollBar::kThickness, 0.0f, ScrollBar::kThickness, listH});
    clampScroll();
    scrollBar_.setRange(contentHeight(), listH, scroll_);
  }

  CommandManager& commands_;
  std::vector<Row> rows_;
  ScrollBar scrollBar_{true};
  Button reset_;
  AlertOverlay alert_;
  PopupMenuOverlay keyMenu_;
  float scroll_ = 0.0f;
  int selected_ = -1;
  int hoverRow_ = -1, hoverChip_ = -1;
  int capturing_ = 0;
  Shortcut replacing_;
  int pendingCommand_ = 0;
  Shortcut pendingShortcut_, pendingReplacing_;
};

} // namespace gfx
} // namespace sonore
