// SPDX-License-Identifier: Apache-2.0
//
// A list with columns you can sort and resize.
//
// ── What it is for ──────────────────────────────────────────────────────────
//
// A preset browser with Name, Author and Category. A sample list with Name,
// Length and Rate. Anything where one string per row is not enough and the user
// wants to reorder by whichever column they are thinking in.
//
// ── It does not reorder your data ───────────────────────────────────────────
//
// Clicking a header reports which column and which direction, and the OWNER
// re-sorts its own rows and says so. That is the conventional contract and the
// only
// one without an ambiguity: a table that kept its own permutation would have to
// decide whether getCellText(3) means the fourth row of the model or the fourth
// row on screen, and every bug in every such table is that question answered
// differently in two places.
//
// The cost is five lines of std::sort in the caller. The benefit is that a
// numeric column sorts numerically, which a table sorting its own cell TEXT
// cannot do -- "10" comes before "9" and nobody notices until the list is long
// enough to matter.
//
// ── Virtual, like everything else here ─────────────────────────────────────
//
// Rows are drawn from a callback, on demand. Four hundred presets paint the
// twelve on screen.
#pragma once

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "lookandfeel.h"
#include "widgets.h"

namespace sonore {
namespace gfx {

struct TableColumn {
  std::string title;
  float width = 120.0f;
  float minWidth = 40.0f;
  /** A column nobody can sort by -- a preview button, an icon. Clicking its
   *  header does nothing rather than sorting by something meaningless. */
  bool sortable = true;
};

class TableListBox : public Widget {
public:
  TableListBox() { setWantsKeyboardFocus(true); }

  void setColumns(std::vector<TableColumn> columns) {
    columns_ = std::move(columns);
    repaint();
  }

  const std::vector<TableColumn>& columns() const { return columns_; }

  /** How many rows the model has. The table never asks for a row outside
   *  this, which is what lets a caller resize its data and call updateContent
   *  without a window of rows pointing at nothing. */
  void setNumRows(int rows) {
    numRows_ = rows < 0 ? 0 : rows;
    if (selected_ >= numRows_) selected_ = numRows_ - 1;
    clampScroll();
    repaint();
  }

  int numRows() const { return numRows_; }

  /** What to draw in one cell. Called only for the rows on screen. */
  std::function<std::string(int row, int column)> getCellText;

  /** The header was clicked. Re-sort YOUR rows and call updateContent().
   *  `forwards` alternates on each click of the same column. */
  std::function<void(int column, bool forwards)> onSortChanged;

  std::function<void(int row)> onSelectionChange;
  /** A double click or Return. "Load this one". */
  std::function<void(int row)> onRowChosen;

  /** The data changed underneath. Redraws and re-clamps, keeping the scroll
   *  position -- a browser that jumped to the top every time a preset was
   *  renamed would be one nobody could use. */
  void updateContent() {
    clampScroll();
    repaint();
  }

  void setRowHeight(float height) {
    rowHeight_ = height > 4.0f ? height : 4.0f;
    clampScroll();
    repaint();
  }
  float rowHeight() const { return rowHeight_; }

  void setHeaderHeight(float height) {
    headerHeight_ = height < 0.0f ? 0.0f : height;
    clampScroll();
    repaint();
  }
  float headerHeight() const { return headerHeight_; }

  int selectedRow() const { return selected_; }

  void setSelectedRow(int row, bool notify = true) {
    const int clamped = numRows_ == 0 ? -1
                                      : (row < 0 ? -1 : (row >= numRows_ ? numRows_ - 1 : row));
    if (clamped == selected_) return;
    selected_ = clamped;
    scrollToShow(selected_);
    repaint();
    if (notify && onSelectionChange) onSelectionChange(selected_);
  }

  /** Which column the user last asked to sort by, and which way. -1 for none.
   *  Kept so a caller restoring a saved view can put the arrow back. */
  int sortColumn() const { return sortColumn_; }
  bool sortForwards() const { return sortForwards_; }

  void setSort(int column, bool forwards, bool notify = true) {
    sortColumn_ = column;
    sortForwards_ = forwards;
    repaint();
    if (notify && onSortChanged) onSortChanged(sortColumn_, sortForwards_);
  }

  float contentHeight() const { return (float) numRows_ * rowHeight_; }

  void setScrollPosition(float y) {
    scroll_ = y;
    clampScroll();
    repaint();
  }
  float scrollPosition() const { return scroll_; }

  /** Where a column starts, in this component's coordinates. */
  float columnX(int index) const {
    float x = 0.0f;
    for (int i = 0; i < index && i < (int) columns_.size(); ++i) x += columns_[(size_t) i].width;
    return x;
  }

  void resized() override { clampScroll(); }

  void paint(Graphics& g) override {
    LookAndFeel& lf = lookAndFeel();
    const Rect area = localBounds();
    g.setColour(lf.panel());
    g.fillRect(area);

    // ── The header ──
    if (headerHeight_ > 0.0f) {
      const Rect header(0.0f, 0.0f, area.w, headerHeight_);
      g.setColour(lf.background());
      g.fillRect(header);
      float x = 0.0f;
      for (size_t i = 0; i < columns_.size(); ++i) {
        const TableColumn& column = columns_[i];
        const Rect cell(x + kCellPad, 0.0f, column.width - kCellPad * 2.0f, headerHeight_);
        lf.drawLabel(g, cell, column.title, font(), Justify::Left, stateFor(false));
        if ((int) i == sortColumn_) drawSortArrow(g, cell, sortForwards_, lf);
        x += column.width;
        g.setColour(lf.outline());
        g.fillRect(Rect(x - 1.0f, 0.0f, 1.0f, headerHeight_));
      }
      g.setColour(lf.outline());
      g.fillRect(Rect(0.0f, headerHeight_ - 1.0f, area.w, 1.0f));
    }

    // ── The rows on screen, and no others ──
    if (numRows_ > 0 && rowHeight_ > 0.0f) {
      const float viewTop = headerHeight_;
      const float viewHeight = area.h - viewTop;
      const int first = (int) (scroll_ / rowHeight_);
      const int last = (int) ((scroll_ + viewHeight) / rowHeight_);
      for (int row = first; row <= last && row < numRows_; ++row) {
        if (row < 0) continue;
        const float y = viewTop + (float) row * rowHeight_ - scroll_;
        const Rect rowArea(0.0f, y, area.w, rowHeight_);
        // Clipped to below the header: a row scrolled halfway under it would
        // otherwise draw over the column titles.
        Graphics::ScopedState scope(g);
        g.clipTo(Rect(0.0f, viewTop, area.w, viewHeight));
        if (g.isClippedOut()) continue;

        lf.drawListRow(g, rowArea, "", font(), row == selected_);
        if (!getCellText) continue;
        float x = 0.0f;
        for (size_t c = 0; c < columns_.size(); ++c) {
          const Rect cell(x + kCellPad, y, columns_[c].width - kCellPad * 2.0f, rowHeight_);
          lf.drawLabel(g, cell, getCellText(row, (int) c), font(), Justify::Left,
                       stateFor(false));
          x += columns_[c].width;
        }
      }
    }
    paintFocusRing(g);
  }

  void mouseDown(const MouseEvent& e) override {
    if (!isEnabled()) return;

    if (e.position.y < headerHeight_) {
      // A drag on a boundary resizes; a click anywhere else in the header
      // sorts. The boundary is the LAST few pixels of a column, which is where
      // every table in every application puts it.
      const int boundary = boundaryAt(e.position.x);
      if (boundary >= 0) {
        draggingColumn_ = boundary;
        dragStartWidth_ = columns_[(size_t) boundary].width;
        return;
      }
      const int column = columnAt(e.position.x);
      if (column >= 0 && columns_[(size_t) column].sortable) {
        // The same column again reverses; a different one starts forwards,
        // because "sort by author" almost always means A to Z the first time.
        if (column == sortColumn_) setSort(column, !sortForwards_);
        else setSort(column, true);
      }
      return;
    }

    const int row = rowAt(e.position.y);
    if (row < 0) return;
    setSelectedRow(row);
    if (e.clickCount >= 2 && onRowChosen) onRowChosen(row);
  }

  void mouseDrag(const MouseEvent& e) override {
    if (draggingColumn_ < 0) return;
    TableColumn& column = columns_[(size_t) draggingColumn_];
    const float wanted = dragStartWidth_ + (e.position.x - e.downPosition.x);
    column.width = wanted < column.minWidth ? column.minWidth : wanted;
    repaint();
  }

  void mouseUp(const MouseEvent&) override { draggingColumn_ = -1; }

  bool mouseWheel(const MouseEvent&, float delta) override {
    if (numRows_ == 0) return false;
    setScrollPosition(scroll_ - delta * rowHeight_ * 3.0f);
    return true;
  }

  bool keyPressed(const KeyPress& key) override {
    if (!isEnabled() || numRows_ == 0) return false;
    if (key.is(KeyPress::Down)) {
      setSelectedRow(selected_ < 0 ? 0 : selected_ + 1);
      return true;
    }
    if (key.is(KeyPress::Up)) {
      setSelectedRow(selected_ < 0 ? numRows_ - 1 : selected_ - 1);
      return true;
    }
    if (key.is(KeyPress::Home)) {
      setSelectedRow(0);
      return true;
    }
    if (key.is(KeyPress::End)) {
      setSelectedRow(numRows_ - 1);
      return true;
    }
    if (key.is(KeyPress::PageDown)) {
      setSelectedRow((selected_ < 0 ? 0 : selected_) + rowsPerPage());
      return true;
    }
    if (key.is(KeyPress::PageUp)) {
      setSelectedRow((selected_ < 0 ? 0 : selected_) - rowsPerPage());
      return true;
    }
    if (key.is(KeyPress::Return) && selected_ >= 0) {
      if (onRowChosen) onRowChosen(selected_);
      return true;
    }
    return false;
  }

  AccessibleInfo accessibleInfo() const override {
    AccessibleInfo info = baseInfo(AccessibleRole::Table);
    if (numRows_ > 0) {
      info.hasRange = true;
      info.minValue = 0.0;
      info.maxValue = (double) numRows_ - 1.0;
      info.currentValue = (double) (selected_ < 0 ? 0 : selected_);
      // The FIRST cell of the selected row, which is the name in every table
      // anybody builds. Reading the whole row would announce four columns of
      // metadata before the user hears which preset they are on.
      if (selected_ >= 0 && getCellText) info.value = getCellText(selected_, 0);
    }
    return info;
  }

  static constexpr float kCellPad = 6.0f;
  /** How close to a boundary counts as grabbing it. */
  static constexpr float kResizeGrab = 4.0f;

private:
  int columnAt(float x) const {
    float left = 0.0f;
    for (size_t i = 0; i < columns_.size(); ++i) {
      const float right = left + columns_[i].width;
      if (x >= left && x < right) return (int) i;
      left = right;
    }
    return -1;
  }

  /** The column whose RIGHT edge is under `x`, or -1. */
  int boundaryAt(float x) const {
    float right = 0.0f;
    for (size_t i = 0; i < columns_.size(); ++i) {
      right += columns_[i].width;
      if (x >= right - kResizeGrab && x <= right + kResizeGrab) return (int) i;
    }
    return -1;
  }

  int rowAt(float y) const {
    if (rowHeight_ <= 0.0f) return -1;
    const int row = (int) ((y - headerHeight_ + scroll_) / rowHeight_);
    return (row >= 0 && row < numRows_) ? row : -1;
  }

  int rowsPerPage() const {
    const int n = (int) ((localBounds().h - headerHeight_) / rowHeight_);
    return n > 1 ? n : 1;
  }

  void scrollToShow(int row) {
    if (row < 0) return;
    const float viewHeight = localBounds().h - headerHeight_;
    const float top = (float) row * rowHeight_;
    if (top < scroll_) scroll_ = top;
    else if (top + rowHeight_ > scroll_ + viewHeight) scroll_ = top + rowHeight_ - viewHeight;
    clampScroll();
  }

  void clampScroll() {
    const float most = contentHeight() - (localBounds().h - headerHeight_);
    if (scroll_ > most) scroll_ = most;
    if (scroll_ < 0.0f) scroll_ = 0.0f;
  }

  static void drawSortArrow(Graphics& g, const Rect& cell, bool forwards, LookAndFeel& lf) {
    const float size = 3.5f;
    const float cx = cell.right() - size - 2.0f;
    const float cy = cell.y + cell.h * 0.5f;
    Path p;
    if (forwards) {
      p.moveTo(cx - size, cy + size * 0.5f);
      p.lineTo(cx + size, cy + size * 0.5f);
      p.lineTo(cx, cy - size * 0.7f);
    } else {
      p.moveTo(cx - size, cy - size * 0.5f);
      p.lineTo(cx + size, cy - size * 0.5f);
      p.lineTo(cx, cy + size * 0.7f);
    }
    p.close();
    g.setColour(lf.accent());
    g.fillPath(p);
  }

  std::vector<TableColumn> columns_;
  int numRows_ = 0;
  int selected_ = -1;
  int sortColumn_ = -1;
  bool sortForwards_ = true;
  int draggingColumn_ = -1;
  float dragStartWidth_ = 0.0f;
  float rowHeight_ = 20.0f;
  float headerHeight_ = 22.0f;
  float scroll_ = 0.0f;
};

} // namespace gfx
} // namespace sonore
