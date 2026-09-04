// SPDX-License-Identifier: Apache-2.0
//
// A tree you can open, close and walk.
//
// ── What it is for ──────────────────────────────────────────────────────────
//
// Presets in folders. Samples in a library. Anything with a hierarchy, which
// a ListBox flattens into a list that is either enormous or wrong.
//
// ── Lazy, because the alternative is unusable ──────────────────────────────
//
// A sample library is a hundred thousand files. Building the whole tree to draw
// twelve rows means walking a drive before the editor appears, and on a network
// share it means an editor that does not appear. So a node's children are built
// the first time it is OPENED, through a callback the owner supplies -- and a
// node can say it MIGHT have children before anyone knows whether it does,
// which is what lets a folder show a twisty without being read first.
//
// ── Virtual, like ListBox ──────────────────────────────────────────────────
//
// The open nodes are flattened into a row list, and only the rows on screen are
// drawn. A tree with four hundred visible rows paints the twelve you can see.
// The flattening is recomputed when something opens or closes, not per frame.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "lookandfeel.h"
#include "widgets.h"

namespace sonore {
namespace gfx {

/**
 * One node.
 *
 * Owns its children, which is the arrangement that makes closing a folder free
 * -- the subtree goes with it -- and the one place a caller has to be careful:
 * a pointer to a node is only valid while its parent stays open.
 */
struct TreeNode {
  std::string text;
  /** Whatever the owner needs to identify this node -- a path, an id, an index
   *  into their own data. Not interpreted here. */
  std::string id;

  /** Whether to draw a twisty. Set BEFORE the children are known: a folder is
   *  openable because it is a folder, not because somebody has already read
   *  it. */
  bool mightHaveChildren = false;
  bool isOpen = false;
  /** Set once the callback has run, so it runs once per node rather than once
   *  per repaint. */
  bool childrenBuilt = false;

  std::vector<std::unique_ptr<TreeNode>> children;
  TreeNode* parent = nullptr;

  TreeNode* addChild(std::string childText, std::string childId = {},
                     bool childMightHaveChildren = false) {
    auto node = std::unique_ptr<TreeNode>(new TreeNode());
    node->text = std::move(childText);
    node->id = std::move(childId);
    node->mightHaveChildren = childMightHaveChildren;
    node->parent = this;
    children.push_back(std::move(node));
    return children.back().get();
  }

  void clearChildren() {
    children.clear();
    childrenBuilt = false;
  }

  int depth() const {
    int d = 0;
    for (const TreeNode* p = parent; p; p = p->parent) ++d;
    return d;
  }
};

class TreeView : public Widget {
public:
  TreeView() { setWantsKeyboardFocus(true); }

  /**
   * The root, which is NOT drawn.
   *
   * Its children are the top level. A visible root would put one row above
   * everything that the user must open before they see anything, and every
   * file browser that shows "/" as a row is a browser somebody has to click
   * twice to start using.
   */
  void setRoot(TreeNode* root) {
    root_ = root;
    selected_ = nullptr;
    scroll_ = 0.0f;
    rebuild();
  }

  TreeNode* root() const { return root_; }

  /** Fill in a node's children. Called once per node, the first time it opens.
   *  A node whose callback adds nothing is a folder that turned out to be
   *  empty, and its twisty goes away. */
  std::function<void(TreeNode&)> onNeedChildren;

  std::function<void(TreeNode*)> onSelectionChange;
  /** A double click, or Return. What a browser uses for "load this one". */
  std::function<void(TreeNode*)> onItemChosen;

  void setRowHeight(float height) {
    rowHeight_ = height > 4.0f ? height : 4.0f;
    rebuild();
  }
  float rowHeight() const { return rowHeight_; }

  /** How far each level is indented. */
  void setIndent(float indent) {
    indent_ = indent > 0.0f ? indent : 0.0f;
    repaint();
  }

  TreeNode* selectedNode() const { return selected_; }

  void setSelectedNode(TreeNode* node, bool notify = true) {
    if (node == selected_) return;
    selected_ = node;
    scrollToShow(indexOf(node));
    repaint();
    if (notify && onSelectionChange) onSelectionChange(selected_);
  }

  /** How many rows are currently visible in the tree -- not on screen, but
   *  reachable without opening anything further. */
  int numVisibleRows() const { return (int) rows_.size(); }

  TreeNode* nodeAtRow(int row) const {
    if (row < 0 || row >= (int) rows_.size()) return nullptr;
    return rows_[(size_t) row];
  }

  /** Open or close, building children on the way in. Returns whether anything
   *  changed, so a caller driving this from a key can tell. */
  bool setOpen(TreeNode* node, bool open) {
    if (!node || !node->mightHaveChildren) return false;
    if (node->isOpen == open) return false;
    node->isOpen = open;
    if (open && !node->childrenBuilt) {
      node->childrenBuilt = true;
      if (onNeedChildren) onNeedChildren(*node);
      // A folder that turned out to be empty stops offering a twisty. Leaving
      // it would give the user something to click that does nothing, forever.
      if (node->children.empty()) {
        node->mightHaveChildren = false;
        node->isOpen = false;
      }
    }
    rebuild();
    return true;
  }

  /** Recompute the flattened row list. Called when the tree changes underneath
   *  us -- a folder rescanned, an item removed. */
  void rebuild() {
    rows_.clear();
    if (root_)
      for (const auto& child : root_->children) flatten(child.get());
    clampScroll();
    // A selection whose node is no longer visible -- its parent closed, or it
    // was removed -- would otherwise be drawn nowhere and returned to callers
    // as though it were still there.
    if (selected_ && indexOf(selected_) < 0) selected_ = nullptr;
    repaint();
  }

  float contentHeight() const { return (float) rows_.size() * rowHeight_; }

  void setScrollPosition(float y) {
    scroll_ = y;
    clampScroll();
    repaint();
  }
  float scrollPosition() const { return scroll_; }

  void resized() override { clampScroll(); }

  void paint(Graphics& g) override {
    LookAndFeel& lf = lookAndFeel();
    const Rect area = localBounds();
    g.setColour(lf.panel());
    g.fillRect(area);

    if (rows_.empty()) {
      paintFocusRing(g);
      return;
    }

    // Only the rows on screen. The whole point of flattening rather than
    // walking: a four-hundred-row tree draws the twelve that are visible.
    const int first = (int) (scroll_ / rowHeight_);
    const int last = (int) ((scroll_ + area.h) / rowHeight_);
    for (int i = first; i <= last && i < (int) rows_.size(); ++i) {
      if (i < 0) continue;
      TreeNode* node = rows_[(size_t) i];
      const float y = (float) i * rowHeight_ - scroll_;
      const Rect row(0.0f, y, area.w, rowHeight_);
      const float x = (float) node->depth() * indent_;

      lf.drawListRow(g, row, "", font(), node == selected_);

      if (node->mightHaveChildren) drawTwisty(g, Rect(x, y, kTwistyWidth, rowHeight_),
                                              node->isOpen, lf);

      const Rect textArea(x + kTwistyWidth, y, area.w - x - kTwistyWidth, rowHeight_);
      lf.drawLabel(g, textArea, node->text, font(), Justify::Left, stateFor(false));
    }
    paintFocusRing(g);
  }

  void mouseDown(const MouseEvent& e) override {
    if (!isEnabled()) return;
    const int row = rowAt(e.position.y);
    TreeNode* node = nodeAtRow(row);
    if (!node) return;

    // The twisty opens; the rest of the row selects. Two targets in one row,
    // which is what every tree does and what a user expects -- clicking a
    // folder's NAME should select it, not open it.
    const float x = (float) node->depth() * indent_;
    if (node->mightHaveChildren && e.position.x >= x && e.position.x < x + kTwistyWidth) {
      setOpen(node, !node->isOpen);
      return;
    }

    setSelectedNode(node);
    if (e.clickCount >= 2) {
      // A double click on a folder opens it; on a leaf it chooses it. The same
      // gesture meaning "go in" either way.
      if (node->mightHaveChildren) setOpen(node, !node->isOpen);
      else if (onItemChosen) onItemChosen(node);
    }
  }

  bool mouseWheel(const MouseEvent&, float delta) override {
    if (rows_.empty()) return false;
    setScrollPosition(scroll_ - delta * rowHeight_ * 3.0f);
    return true;
  }

  /**
   * The keys a tree has always used.
   *
   * Right on a closed folder opens it; on an OPEN one it steps into the first
   * child. Left on an open folder closes it; on anything else it steps out to
   * the parent. That pair is what makes a tree navigable without a mouse, and
   * getting it wrong -- Right always opening, say -- traps the user in a folder
   * they cannot leave from the keyboard.
   */
  bool keyPressed(const KeyPress& key) override {
    if (!isEnabled() || rows_.empty()) return false;
    const int current = indexOf(selected_);

    if (key.is(KeyPress::Down)) {
      moveSelection(current < 0 ? 0 : current + 1);
      return true;
    }
    if (key.is(KeyPress::Up)) {
      moveSelection(current < 0 ? (int) rows_.size() - 1 : current - 1);
      return true;
    }
    if (key.is(KeyPress::Home)) {
      moveSelection(0);
      return true;
    }
    if (key.is(KeyPress::End)) {
      moveSelection((int) rows_.size() - 1);
      return true;
    }
    if (key.is(KeyPress::PageDown)) {
      moveSelection((current < 0 ? 0 : current) + rowsPerPage());
      return true;
    }
    if (key.is(KeyPress::PageUp)) {
      moveSelection((current < 0 ? 0 : current) - rowsPerPage());
      return true;
    }
    if (key.is(KeyPress::Right)) {
      if (!selected_) return false;
      if (selected_->mightHaveChildren && !selected_->isOpen) {
        setOpen(selected_, true);
      } else if (selected_->isOpen && !selected_->children.empty()) {
        setSelectedNode(selected_->children.front().get());
      }
      return true;
    }
    if (key.is(KeyPress::Left)) {
      if (!selected_) return false;
      if (selected_->isOpen) setOpen(selected_, false);
      else if (selected_->parent && selected_->parent != root_)
        setSelectedNode(selected_->parent);
      return true;
    }
    if (key.is(KeyPress::Return)) {
      if (!selected_) return false;
      if (selected_->mightHaveChildren) setOpen(selected_, !selected_->isOpen);
      else if (onItemChosen) onItemChosen(selected_);
      return true;
    }
    return false;
  }

  AccessibleInfo accessibleInfo() const override {
    AccessibleInfo info = baseInfo(AccessibleRole::List);
    // A range over the VISIBLE rows, and the selected row's text as the value.
    // Not one element per node: a reader enumerating a sample library would
    // never stop, and the rows a user can reach are the ones currently open.
    if (!rows_.empty()) {
      info.hasRange = true;
      info.minValue = 0.0;
      info.maxValue = (double) rows_.size() - 1.0;
      const int index = indexOf(selected_);
      info.currentValue = (double) (index < 0 ? 0 : index);
      if (index >= 0) info.value = rows_[(size_t) index]->text;
    }
    return info;
  }

  static constexpr float kTwistyWidth = 18.0f;

private:
  void flatten(TreeNode* node) {
    rows_.push_back(node);
    if (!node->isOpen) return;
    for (const auto& child : node->children) flatten(child.get());
  }

  int indexOf(const TreeNode* node) const {
    if (!node) return -1;
    for (size_t i = 0; i < rows_.size(); ++i)
      if (rows_[i] == node) return (int) i;
    return -1;
  }

  int rowAt(float y) const {
    if (rowHeight_ <= 0.0f) return -1;
    const int row = (int) ((y + scroll_) / rowHeight_);
    return (row >= 0 && row < (int) rows_.size()) ? row : -1;
  }

  int rowsPerPage() const {
    const int n = (int) (localBounds().h / rowHeight_);
    return n > 1 ? n : 1;
  }

  void moveSelection(int index) {
    if (rows_.empty()) return;
    const int clamped = index < 0 ? 0 : (index >= (int) rows_.size() ? (int) rows_.size() - 1
                                                                    : index);
    setSelectedNode(rows_[(size_t) clamped]);
  }

  /** The smallest scroll that brings a row into view. A tree that jumped to
   *  centre the selection would move the whole list under a user who pressed
   *  Down once. */
  void scrollToShow(int row) {
    if (row < 0) return;
    const float top = (float) row * rowHeight_;
    const float bottom = top + rowHeight_;
    if (top < scroll_) scroll_ = top;
    else if (bottom > scroll_ + localBounds().h) scroll_ = bottom - localBounds().h;
    clampScroll();
  }

  void clampScroll() {
    const float most = contentHeight() - localBounds().h;
    if (scroll_ > most) scroll_ = most;
    if (scroll_ < 0.0f) scroll_ = 0.0f;
  }

  /** A triangle, pointing right when closed and down when open. Drawn rather
   *  than drawn FROM an icon, because a tree with no artwork is a tree that
   *  works in a plugin that ships no artwork. */
  static void drawTwisty(Graphics& g, const Rect& area, bool open, LookAndFeel& lf) {
    const float size = 4.0f;
    const float cx = area.x + area.w * 0.5f;
    const float cy = area.y + area.h * 0.5f;
    Path p;
    if (open) {
      p.moveTo(cx - size, cy - size * 0.6f);
      p.lineTo(cx + size, cy - size * 0.6f);
      p.lineTo(cx, cy + size * 0.8f);
    } else {
      p.moveTo(cx - size * 0.6f, cy - size);
      p.lineTo(cx + size * 0.8f, cy);
      p.lineTo(cx - size * 0.6f, cy + size);
    }
    p.close();
    g.setColour(lf.dimText());
    g.fillPath(p);
  }

  TreeNode* root_ = nullptr;
  TreeNode* selected_ = nullptr;
  std::vector<TreeNode*> rows_;
  float rowHeight_ = 20.0f;
  float indent_ = 14.0f;
  float scroll_ = 0.0f;
};

} // namespace gfx
} // namespace sonore
