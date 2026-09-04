// SPDX-License-Identifier: Apache-2.0
//
// Choosing a file from inside the editor.
//
// ── Why not just use the system dialog ─────────────────────────────────────
//
// file_dialog.h opens the OS chooser, and for "load an impulse response" that
// is right: it is the dialog the user knows, it has their places and their
// recent folders, and it costs nothing to maintain.
//
// It is the wrong tool for a sampler. A browser that lives IN the editor stays
// open while you audition, keeps its position between plugins, can preview on
// selection, and does not steal focus from the host every time you look at
// another file. Every sampler worth using has one, and none of them use the OS
// dialog for it.
//
// ── What this is made of ────────────────────────────────────────────────────
//
// TreeView over listDirectory, and almost nothing else. The tree is already
// lazy and already virtual, so a hundred-thousand-file library costs one
// readdir per folder the user actually opens. What is here is the wiring, and
// the handful of decisions that wiring has to make.
//
// ── The synthetic root ──────────────────────────────────────────────────────
//
// On Windows there is no single top: a library on D: cannot be reached from C:
// by going up. So when no start folder is given, the browser's root has one
// child per drive. On POSIX it has one child, "/". Either way the user can
// always get to everything, which is what a browser that traps you on one
// volume cannot say.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "../files.h"
#include "treeview.h"

namespace sonore {
namespace gfx {

class FileBrowser : public Widget {
public:
  FileBrowser() {
    addChild(&tree_);
    tree_.onNeedChildren = [this](TreeNode& node) { fill(node); };
    tree_.onSelectionChange = [this](TreeNode* node) {
      if (onSelectionChange) onSelectionChange(node ? node->id : std::string());
    };
    tree_.onItemChosen = [this](TreeNode* node) {
      if (node && onFileChosen) onFileChosen(node->id);
    };
    setRoots({});
  }

  /**
   * Start somewhere in particular, or nowhere.
   *
   * An empty path gives the synthetic root -- every drive on Windows, "/"
   * elsewhere. A path that is a FILE starts at its folder, because "show me
   * this file" is what a caller restoring a saved selection means and refusing
   * it would make them do the arithmetic themselves.
   */
  void setRootPath(const std::string& path) {
    if (path.empty()) {
      setRoots({});
      return;
    }
    setRoots({isDirectory(path) ? path : parentPath(path)});
  }

  /** Several starting points at once -- a sampler with a factory library and a
   *  user folder wants both at the top and neither above the other. */
  void setRoots(std::vector<std::string> paths) {
    roots_ = std::move(paths);
    if (roots_.empty()) roots_ = rootPaths();
    rebuildRoot();
  }

  /** Which files to show. Folders are never filtered out -- excluding one would
   *  hide everything underneath it. An empty list shows every file. */
  void setFileFilter(std::vector<std::string> extensions) {
    filter_ = std::move(extensions);
    rebuildRoot();
  }

  void setShowHidden(bool show) {
    showHidden_ = show;
    rebuildRoot();
  }

  /** Show FOLDERS only, for a caller choosing a directory rather than a file. */
  void setDirectoriesOnly(bool only) {
    directoriesOnly_ = only;
    rebuildRoot();
  }

  /** The selected path, or empty. */
  std::string selectedPath() const {
    TreeNode* node = tree_.selectedNode();
    return node ? node->id : std::string();
  }

  bool selectionIsDirectory() const {
    TreeNode* node = tree_.selectedNode();
    return node && node->mightHaveChildren;
  }

  std::function<void(const std::string&)> onSelectionChange;
  /** A double click or Return on a FILE. What a browser means by "load this". */
  std::function<void(const std::string&)> onFileChosen;

  /**
   * Read everything again.
   *
   * A folder open on screen while somebody drops a file into it should show the
   * file. Nothing here watches the filesystem -- that is a per-platform API
   * with a thread behind it, and a plugin that held a directory watch on a
   * network share would keep it awake -- so this is what a Refresh button
   * calls, and what a browser should call when its editor is shown again.
   *
   * The OPEN folders are reopened afterwards, and the selection is restored if
   * its path still exists. Without that, refreshing collapses the tree to the
   * top and loses the user's place, which makes the button worse than useless.
   */
  void refresh() {
    std::vector<std::string> openPaths;
    collectOpen(&root_, openPaths);
    const std::string wasSelected = selectedPath();

    rebuildRoot();
    for (const std::string& path : openPaths) openPath(path);
    if (!wasSelected.empty()) selectPath(wasSelected);
  }

  /**
   * Open every folder down to `path`, so a caller can restore a saved
   * selection.
   *
   * Returns whether it got there. False for a path that no longer exists,
   * which is the ordinary case when a project is opened on another machine.
   */
  bool selectPath(const std::string& path) {
    if (path.empty()) return false;
    TreeNode* node = openPath(path);
    if (!node) return false;
    tree_.setSelectedNode(node);
    return true;
  }

  TreeView& tree() { return tree_; }

  void resized() override { tree_.setBounds(localBounds()); }

  AccessibleInfo accessibleInfo() const override {
    // The browser itself is a container; the tree inside it is what a reader
    // reads. Announcing both would put an unnamed level in front of the list.
    AccessibleInfo info = baseInfo(AccessibleRole::Unknown);
    return info;
  }

private:
  /** Fill a node from the filesystem. One readdir, and only for a folder the
   *  user has actually opened. */
  void fill(TreeNode& node) {
    const std::vector<FileEntry> entries =
        listDirectory(node.id, filter_, /*includeDirectories=*/true, showHidden_);
    for (const FileEntry& e : entries) {
      if (directoriesOnly_ && !e.isDirectory) continue;
      node.addChild(e.name, e.path, e.isDirectory);
    }
  }

  void rebuildRoot() {
    root_.clearChildren();
    for (const std::string& path : roots_) {
      // A root's name is its whole path when it has no last component -- "C:\"
      // and "/" would otherwise appear as an empty row.
      std::string label = fileName(path);
      if (label.empty()) label = path;
      root_.addChild(label, path, true);
    }
    // The root is not drawn; setRoot both installs it and flattens.
    tree_.setRoot(&root_);
    // ONE root opens itself. With several -- the drives, or a factory folder
    // beside a user one -- opening the first would be a guess about which the
    // user wanted, and the wrong guess scrolls the other off the bottom.
    if (root_.children.size() == 1) tree_.setOpen(root_.children.front().get(), true);
  }

  /** Walk down `path`, opening as it goes. Returns the node, or null. */
  TreeNode* openPath(const std::string& path) {
    TreeNode* best = nullptr;
    for (const auto& child : root_.children) {
      TreeNode* found = openWithin(child.get(), path);
      if (found) best = found;
    }
    return best;
  }

  TreeNode* openWithin(TreeNode* node, const std::string& path) {
    if (node->id == path) return node;
    // Only descend where the path is actually BELOW this node. Comparing
    // prefixes without the separator would make "/lib/Drums2" look like it is
    // inside "/lib/Drums".
    if (path.size() <= node->id.size()) return nullptr;
    if (path.compare(0, node->id.size(), node->id) != 0) return nullptr;
    const char after = path[node->id.size()];
    if (!isSeparator(after) && !isSeparator(node->id.back())) return nullptr;

    if (!node->mightHaveChildren) return nullptr;
    tree_.setOpen(node, true);
    for (const auto& child : node->children)
      if (TreeNode* found = openWithin(child.get(), path)) return found;
    return nullptr;
  }

  static void collectOpen(TreeNode* node, std::vector<std::string>& out) {
    for (const auto& child : node->children) {
      if (child->isOpen) out.push_back(child->id);
      collectOpen(child.get(), out);
    }
  }

  TreeView tree_;
  TreeNode root_;
  std::vector<std::string> roots_;
  std::vector<std::string> filter_;
  bool showHidden_ = false;
  bool directoriesOnly_ = false;
};

} // namespace gfx
} // namespace sonore
