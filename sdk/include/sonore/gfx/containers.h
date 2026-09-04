// SPDX-License-Identifier: Apache-2.0
//
// A list, a set of tabs, and a progress bar.
//
// ── Why these three ─────────────────────────────────────────────────────────
//
// A ListBox is what a preset browser IS -- also a sample list, a modulation
// slot list, a device list. A synth with an oscillator page, a filter page and
// an envelope page is the ordinary shape of an instrument editor, and that is a
// TabbedComponent. And loading a sample library is the one thing a plugin does
// that takes long enough to need a progress bar.
//
// ── The list is virtual ─────────────────────────────────────────────────────
//
// ListBox draws rows on demand from a model rather than owning a component per
// row. A preset browser with four thousand presets would otherwise build four
// thousand components, each with bounds and a paint method, to show twelve of
// them. Everything below the fold costs nothing here.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "layout.h"
#include "viewport.h"
#include "widgets.h"

namespace sonore {
namespace gfx {

/**
 * A scrolling list of rows, drawn on demand.
 *
 * The model is two callbacks rather than a subclass to implement: how many rows
 * there are, and how to draw one. A plugin's list is usually a vector it
 * already has, and making it inherit from something to show that vector is
 * ceremony with no payoff.
 */
class ListBox : public Widget {
public:
  static constexpr float kDefaultRowHeight = 22.0f;

  ListBox() {
    scrollBar_.setVertical(true);
    addChild(&scrollBar_);
    scrollBar_.onScroll = [this](float y) {
      scrollY_ = y;
      repaint();
    };
  }

  /** How many rows the model has. */
  std::function<int()> getNumRows;
  /** Draw one. `area` is in the LIST's coordinates, already scrolled. */
  std::function<void(Graphics&, int row, const Rect& area, bool selected)> drawRow;
  /** Fired on a click, and on a keyboard move. -1 means nothing is selected. */
  std::function<void(int row)> onSelectionChange;
  /** Fired on a double click, which is how a preset is loaded. */
  std::function<void(int row)> onRowChosen;

  /** The simple case: a list of strings, drawn by the LookAndFeel. Supplying
   *  this replaces drawRow, so a caller that only has names writes one line. */
  void setItems(std::vector<std::string> items) {
    items_ = std::move(items);
    getNumRows = [this]() { return (int) items_.size(); };
    drawRow = [this](Graphics& g, int row, const Rect& area, bool selected) {
      if (row < 0 || row >= (int) items_.size()) return;
      lookAndFeel().drawListRow(g, area, items_[(size_t) row], font(), selected);
    };
    // BOTH ends, and -1 stays -1.
    //
    // A fresh list selects NOTHING, which is the convention and is honest:
    // a preset browser that highlighted row zero on open would be claiming a
    // preset is loaded when none is. Replacing a shorter list clamps to the
    // last row instead of jumping to the first, so a user who was near the
    // bottom stays near the bottom.
    //
    // The first version tested only the upper bound -- the same shape as a
    // ComboBox bug fixed in this SDK long ago, and it took a test asserting the
    // wrong behaviour for me to look at it twice.
    if (items_.empty()) selected_ = -1;
    else if (selected_ >= (int) items_.size()) selected_ = (int) items_.size() - 1;
    updateScrollBar();
    repaint();
  }

  const std::vector<std::string>& items() const { return items_; }

  void setRowHeight(float height) {
    rowHeight_ = height > 1.0f ? height : 1.0f;
    updateScrollBar();
    repaint();
  }
  float rowHeight() const { return rowHeight_; }

  int numRows() const { return getNumRows ? getNumRows() : 0; }

  int selectedRow() const { return selected_; }

  AccessibleInfo accessibleInfo() const override {
    AccessibleInfo info = baseInfo(AccessibleRole::List);
    // A range rather than a list of children, because the rows are DRAWN from
    // a model rather than owned as components -- there is nothing for a
    // traversal to walk. "Row 12 of 400" is what a reader can say, and it is
    // what a four-hundred-preset browser should say rather than reading four
    // hundred items.
    const int rows = numRows();
    if (rows > 0) {
      info.hasRange = true;
      info.minValue = 0.0;
      info.maxValue = (double) rows - 1.0;
      info.currentValue = (double) (selected_ < 0 ? 0 : selected_);
      // Only when the rows came from setItems. A list driven by a caller's own
      // getNumRows/drawRow pair has no text this class can read -- and
      // inventing "row 3" for it would be a value that is not what is on
      // screen, which is the one thing a reader must never say.
      if (selected_ >= 0 && selected_ < (int) items_.size())
        info.value = items_[(size_t) selected_];
    }
    return info;
  }

  void setSelectedRow(int row, bool notify = true) {
    const int count = numRows();
    const int clamped = count == 0 ? -1 : (row < 0 ? -1 : (row >= count ? count - 1 : row));
    if (clamped == selected_) return;
    selected_ = clamped;
    scrollToShow(selected_);
    repaint();
    if (notify && onSelectionChange) onSelectionChange(selected_);
  }

  /** The smallest scroll that brings a row into view. A list that jumped to
   *  centre the row every time would lose the reader's place. */
  void scrollToShow(int row) {
    if (row < 0) return;
    const float top = (float) row * rowHeight_;
    const float visible = localBounds().h;
    if (top < scrollY_) scrollY_ = top;
    else if (top + rowHeight_ > scrollY_ + visible) scrollY_ = top + rowHeight_ - visible;
    clampScroll();
    scrollBar_.setPosition(scrollY_, false);
    repaint();
  }

  /** The row at a point in this component's coordinates, or -1. */
  int rowAt(Point p) const {
    if (!localBounds().contains(p)) return -1;
    const int row = (int) ((p.y + scrollY_) / rowHeight_);
    return (row >= 0 && row < numRows()) ? row : -1;
  }

  void paint(Graphics& g) override {
    LookAndFeel& lf = lookAndFeel();
    const Rect area = localBounds();
    g.setColour(lf.background().darkened(0.02f));
    g.fillRect(area);
    if (!drawRow) return;

    // Only the rows that are actually visible. A list of four thousand presets
    // draws the twelve on screen, which is the entire point of not building a
    // component per row.
    const int count = numRows();
    const int first = (int) (scrollY_ / rowHeight_);
    const int last = (int) ((scrollY_ + area.h) / rowHeight_) + 1;
    for (int row = first < 0 ? 0 : first; row < count && row <= last; ++row) {
      const Rect rowArea(0.0f, (float) row * rowHeight_ - scrollY_,
                         area.w - (scrollBar_.isVisible() ? ScrollBar::kThickness : 0.0f),
                         rowHeight_);
      Graphics::ScopedState scope(g);
      g.clipTo(rowArea);
      if (g.isClippedOut()) continue;
      drawRow(g, row, rowArea, row == selected_);
    }
  }

  void resized() override { updateScrollBar(); }

  void mouseDown(const MouseEvent& e) override {
    if (!isEnabled()) return;
    const int row = rowAt(e.position);
    if (row < 0) return;
    setSelectedRow(row);
    if (e.clickCount >= 2 && onRowChosen) onRowChosen(row);
  }

  bool mouseWheel(const MouseEvent&, float delta) override {
    if (delta == 0.0f || !hasScroll()) return false;
    scrollY_ -= delta * ScrollBar::kWheelStep;
    clampScroll();
    scrollBar_.setPosition(scrollY_, false);
    repaint();
    return true;
  }

  /** Arrow keys move the selection, which is what makes a list usable without
   *  the mouse -- and what makes a preset browser auditionable one at a time. */
  bool keyPressed(const KeyPress& key) override {
    const int count = numRows();
    if (count == 0) return false;
    switch (key.keyCode) {
      case KeyPress::Up:
        setSelectedRow(selected_ <= 0 ? 0 : selected_ - 1);
        return true;
      case KeyPress::Down:
        setSelectedRow(selected_ < 0 ? 0 : selected_ + 1);
        return true;
      case KeyPress::Home:
        setSelectedRow(0);
        return true;
      case KeyPress::End:
        setSelectedRow(count - 1);
        return true;
      case KeyPress::PageUp:
      case KeyPress::PageDown: {
        const int page = (int) (localBounds().h / rowHeight_);
        const int step = page > 1 ? page - 1 : 1; // one row of overlap keeps the place
        setSelectedRow(selected_ + (key.keyCode == KeyPress::PageUp ? -step : step));
        return true;
      }
      case KeyPress::Return:
        if (selected_ >= 0 && onRowChosen) onRowChosen(selected_);
        return true;
      default:
        return false;
    }
  }

  ListBox& withKeyboardFocus() {
    setWantsKeyboardFocus(true);
    return *this;
  }

private:
  bool hasScroll() const { return contentHeight() > localBounds().h; }
  float contentHeight() const { return (float) numRows() * rowHeight_; }

  void clampScroll() {
    const float most = contentHeight() - localBounds().h;
    if (scrollY_ > most) scrollY_ = most;
    if (scrollY_ < 0.0f) scrollY_ = 0.0f;
  }

  void updateScrollBar() {
    const Rect area = localBounds();
    const bool needed = hasScroll();
    scrollBar_.setVisible(needed);
    scrollBar_.setBounds({area.w - ScrollBar::kThickness, 0.0f, ScrollBar::kThickness, area.h});
    scrollBar_.setRange(contentHeight(), area.h, scrollY_);
    clampScroll();
  }

  std::vector<std::string> items_;
  ScrollBar scrollBar_{true};
  float rowHeight_ = kDefaultRowHeight;
  float scrollY_ = 0.0f;
  int selected_ = -1;
};

/**
 * Pages behind a row of tabs.
 *
 * The pages are NOT owned, for the same reason nothing else here owns its
 * content: a plugin builds its panels as members and a container that took them
 * over would decide a lifetime the plugin already decided.
 */
class TabbedComponent : public Widget {
public:
  static constexpr float kTabHeight = 26.0f;

  std::function<void(int index)> onTabChanged;

  void addTab(std::string name, Component* page) {
    tabs_.push_back(Tab{std::move(name), page});
    if (page) addChild(page);
    if (current_ < 0) setCurrentTab(0, false);
    else applyVisibility();
    resized();
    repaint();
  }

  int numTabs() const { return (int) tabs_.size(); }
  int currentTab() const { return current_; }
  const std::string& tabName(int index) const { return tabs_[(size_t) index].name; }

  void setCurrentTab(int index, bool notify = true) {
    if (index < 0 || index >= (int) tabs_.size() || index == current_) return;
    current_ = index;
    applyVisibility();
    resized();
    repaint();
    if (notify && onTabChanged) onTabChanged(current_);
  }

  /** Where a tab's button is, in this component's coordinates. */
  Rect tabBounds(int index) const {
    if (tabs_.empty() || index < 0 || index >= (int) tabs_.size()) return {};
    const float w = localBounds().w / (float) tabs_.size();
    return {(float) index * w, 0.0f, w, kTabHeight};
  }

  void paint(Graphics& g) override {
    LookAndFeel& lf = lookAndFeel();
    g.fillAll(lf.background());
    for (size_t i = 0; i < tabs_.size(); ++i)
      lf.drawTab(g, tabBounds((int) i), tabs_[i].name, font(), (int) i == current_,
                 (int) i == hovered_);
  }

  void resized() override {
    const Rect area = localBounds();
    const Rect body(0.0f, kTabHeight, area.w, area.h - kTabHeight);
    for (Tab& tab : tabs_)
      if (tab.page) tab.page->setBounds(body);
  }

  void mouseDown(const MouseEvent& e) override {
    if (!isEnabled() || e.position.y > kTabHeight) return;
    for (size_t i = 0; i < tabs_.size(); ++i)
      if (tabBounds((int) i).contains(e.position)) {
        setCurrentTab((int) i);
        return;
      }
  }

  void mouseMove(const MouseEvent& e) override {
    int over = -1;
    if (e.position.y <= kTabHeight)
      for (size_t i = 0; i < tabs_.size(); ++i)
        if (tabBounds((int) i).contains(e.position)) over = (int) i;
    if (over == hovered_) return;
    hovered_ = over;
    repaint();
  }

  void mouseExit(const MouseEvent&) override {
    if (hovered_ < 0) return;
    hovered_ = -1;
    repaint();
  }

private:
  struct Tab {
    std::string name;
    Component* page = nullptr;
  };

  /** Only the current page is visible, so the others cost nothing to paint and
   *  cannot be clicked through. */
  void applyVisibility() {
    for (size_t i = 0; i < tabs_.size(); ++i)
      if (tabs_[i].page) tabs_[i].page->setVisible((int) i == current_);
  }

  std::vector<Tab> tabs_;
  int current_ = -1;
  int hovered_ = -1;
};

/**
 * A bar that fills.
 *
 * Reads its value through a pointer to a double rather than being told, because
 * the thing making progress is usually on another thread -- a sample library
 * loading -- and a bar that had to be pushed at would need that thread to know
 * about the UI.
 */
class ProgressBar : public Widget {
public:
  /** `source` must outlive this. A null one shows an indeterminate bar, which
   *  is the honest picture of "something is happening and nobody knows how
   *  much is left". */
  void setSource(const double* source) {
    source_ = source;
    repaint();
  }

  void setText(std::string text) {
    text_ = std::move(text);
    repaint();
  }

  double progress() const {
    if (!source_) return -1.0;
    const double v = *source_;
    return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
  }

  bool isIndeterminate() const { return source_ == nullptr; }

  /** Called from the editor's tick. Returns whether the picture changed, so a
   *  caller repaints only when it did rather than thirty times a second. */
  bool tick() {
    const double now = progress();
    if (now == lastDrawn_) return false;
    lastDrawn_ = now;
    repaint();
    return true;
  }

  void paint(Graphics& g) override {
    lookAndFeel().drawProgressBar(g, localBounds(), progress(), text_, font());
  }

private:
  const double* source_ = nullptr;
  std::string text_;
  double lastDrawn_ = -2.0;
};

} // namespace gfx
} // namespace sonore
