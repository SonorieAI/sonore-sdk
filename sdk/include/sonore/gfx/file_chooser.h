// SPDX-License-Identifier: Apache-2.0
//
// The pieces of a file chooser drawn inside the editor.
//
// ── Next to file_browser.h and file_dialog.h ────────────────────────────────
//
// file_dialog.h opens the operating system's chooser, and for "load an
// impulse response" that is still the right call: it has the user's places,
// it reaches network volumes, and on macOS it is the only door out of the
// sandbox. file_browser.h is the sampler's browser: a tree that lives in the
// editor and stays open while you audition.
//
// This file is the rest of the reference's set, built on the same
// listDirectory:
//
//   FileListComponent     one folder as a list -- name, kind, size -- that is
//                         walked into and out of, with type-ahead
//   FileTreeComponent     FileBrowser rooted at a folder, with a preview hook
//   FilePreviewComponent  what a chooser calls when the selection moves
//   ImagePreviewComponent a preview that shows PNG and SVG files
//   FilenameComponent     a path field with a browse button and the recently
//                         used files behind a drop-down -- the control a
//                         plugin's settings page uses for "sample folder"
//   FileSearchPathListComponent  an editable list of folders, for a plugin
//                         that looks for content in several places
//   FileChooserDialogBox  all of it in a dialog over the editor: open, save
//                         (asking before it overwrites) or choose a folder
//
// ── The dialog is in the editor, like every dialog here ─────────────────────
//
// alert.h says why: a plugin's own top-level window can end up behind the
// host's, waiting on a click that cannot happen. The dialog is an Overlay,
// answers through a callback, and blocks nothing.
//
// ── Nothing watches the disk ────────────────────────────────────────────────
//
// A folder is read when it is shown and when refresh() is called. A watch is
// a per-platform API with a thread behind it, and a plugin holding one on a
// network share keeps the share awake -- file_browser.h made the same call.
#pragma once

#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "../files.h"
#include "../path_io.h"
#include "alert.h"
#include "containers.h"
#include "effects2d.h"
#include "file_browser.h"
#include "png.h"
#include "popup_overlay.h"
#include "svg.h"
#include "text_editor.h"
#include "widgets.h"

namespace sonore {
namespace gfx {

namespace detail {

/** "12 KB", "3.4 MB". For a list column, so a size reads at a glance. */
inline std::string chooserSizeText(uint64_t bytes) {
  char buffer[32];
  if (bytes < 1024) std::snprintf(buffer, sizeof(buffer), "%u B", (unsigned) bytes);
  else if (bytes < 1024ull * 1024ull) std::snprintf(buffer, sizeof(buffer), "%.0f KB", (double) bytes / 1024.0);
  else if (bytes < 1024ull * 1024ull * 1024ull)
    std::snprintf(buffer, sizeof(buffer), "%.1f MB", (double) bytes / (1024.0 * 1024.0));
  else std::snprintf(buffer, sizeof(buffer), "%.1f GB", (double) bytes / (1024.0 * 1024.0 * 1024.0));
  return buffer;
}

/** "C:\x", "\\server\x" or "/x": a path that does not need a folder in front. */
inline bool chooserIsAbsolute(const std::string& path) {
  if (path.empty()) return false;
  if (isSeparator(path[0])) return true;
  return path.size() >= 2 && path[1] == ':';
}

/** "*.wav;*.aif" or "wav, aif" to {".wav", ".aif"}. "*" or "*.*" means all. */
inline std::vector<std::string> chooserExtensions(const std::string& wildcard) {
  std::vector<std::string> out;
  std::string current;
  auto flush = [&]() {
    std::string e = current;
    current.clear();
    if (e.rfind("*.", 0) == 0) e.erase(0, 1);
    if (e.empty() || e == "*" || e == ".*") return;
    if (e[0] != '.') e.insert(e.begin(), '.');
    out.push_back(e);
  };
  for (char c : wildcard) {
    if (c == ';' || c == ',' || c == ' ' || c == '|') flush();
    else current += c;
  }
  flush();
  return out;
}

/** A whole file, by UTF-8 path, up to a limit -- a preview never needs more
 *  than an image is allowed to be, and a 4 GB file picked by mistake must not
 *  be read to find that out. */
inline bool chooserReadFile(const std::string& path, std::vector<uint8_t>& out, size_t limit) {
  out.clear();
  std::FILE* f = pathio::open(path.c_str(), "rb");
  if (!f) return false;
  uint8_t chunk[65536];
  size_t got = 0;
  bool ok = true;
  while ((got = std::fread(chunk, 1, sizeof(chunk), f)) > 0) {
    if (out.size() + got > limit) {
      ok = false;
      break;
    }
    out.insert(out.end(), chunk, chunk + got);
  }
  std::fclose(f);
  return ok;
}

} // namespace detail

/**
 * Something a chooser shows beside its list, told whenever the selected file
 * changes -- an empty path when nothing is selected.
 */
class FilePreviewComponent : public Widget {
public:
  /** The selected file's whole path; empty when nothing, or a folder, is
   *  selected. Called on the message thread each time the selection moves. */
  virtual void selectedFileChanged(const std::string& newSelectedFile) = 0;
};

/**
 * A preview of PNG and SVG files: the picture, scaled to fit without
 * distortion, with its name and size under it.
 */
class ImagePreviewComponent : public FilePreviewComponent {
public:
  /** Larger files are not read: a preview is not worth a stall. */
  static constexpr size_t kMaxFileBytes = 32u * 1024u * 1024u;

  /** Reads and decodes a `.png` or `.svg` file (the extension in any case) of
   *  at most kMaxFileBytes, then repaints. Any other file, or empty, shows no
   *  picture; a PNG that does not decode puts the decoder's error in the
   *  caption. The file is read here, on the calling thread. */
  void selectedFileChanged(const std::string& file) override {
    file_ = file;
    image_ = Bitmap();
    drawable_ = Drawable();
    caption_.clear();
    const std::string ext = fileExtension(file);
    std::vector<uint8_t> bytes;
    if ((ext == ".png" || ext == ".svg") && detail::chooserReadFile(file, bytes, kMaxFileBytes)) {
      if (ext == ".png") {
        PngImage decoded = PngDecoder::decode(bytes);
        if (decoded.ok()) {
          image_ = std::move(decoded.bitmap);
          caption_ = std::to_string(image_.width()) + " x " + std::to_string(image_.height());
        } else {
          caption_ = decoded.error;
        }
      } else {
        drawable_ = Drawable::parse(std::string(bytes.begin(), bytes.end()));
        caption_ = drawable_.isEmpty() ? "not a picture this can draw" : "SVG";
      }
    }
    repaint();
  }

  /** The path last given to selectedFileChanged, or empty. */
  const std::string& currentFile() const { return file_; }
  /** Whether there is a picture to draw: a decoded PNG, or an SVG that
   *  parsed to something. */
  bool hasImage() const { return !image_.isEmpty() || !drawable_.isEmpty(); }
  /** The decoded PNG; empty for an SVG or when there is no picture. */
  const Bitmap& image() const { return image_; }
  /** What is written under the picture: its size, or why there is none. */
  const std::string& caption() const { return caption_; }

  /** The picture fitted into the area above two centred lines: the file's
   *  name, then the caption. */
  void paint(Graphics& g) override {
    LookAndFeel& lf = lookAndFeel();
    const Rect area = localBounds();
    g.setColour(lf.background().darkened(0.02f));
    g.fillRoundedRect(area, 4.0f);
    const float textH = font().lineHeight() * 2.0f + 8.0f;
    const Rect pictureArea = Rect(area.x + 6.0f, area.y + 6.0f, area.w - 12.0f, area.h - textH - 12.0f);
    if (!image_.isEmpty() && !pictureArea.isEmpty()) {
      const Rect source(0.0f, 0.0f, (float) image_.width(), (float) image_.height());
      g.drawImage(image_, RectanglePlacement().apply(source, pictureArea));
    } else if (!drawable_.isEmpty() && !pictureArea.isEmpty()) {
      drawable_.draw(g, pictureArea, lf.text());
    }
    if (!file_.empty()) {
      g.setColour(lf.text());
      font().drawIn(g, fileName(file_), Rect(area.x + 4.0f, area.bottom() - textH, area.w - 8.0f, textH * 0.5f),
                    Justify::Centred);
      g.setColour(lf.dimText());
      font().drawIn(g, caption_, Rect(area.x + 4.0f, area.bottom() - textH * 0.5f, area.w - 8.0f, textH * 0.5f),
                    Justify::Centred);
    }
  }

  AccessibleInfo accessibleInfo() const override {
    AccessibleInfo info = baseInfo(AccessibleRole::Label);
    if (info.name.empty()) info.name = file_.empty() ? std::string("No file selected") : fileName(file_);
    info.value = caption_;
    return info;
  }

private:
  std::string file_;
  Bitmap image_;
  Drawable drawable_;
  std::string caption_;
};

/**
 * One folder's contents as a list.
 *
 * A ListBox underneath, so the keyboard, the scroll bar and the reader's
 * "row 12 of 40" are the list's own; this adds the folder, the rows' look,
 * walking in and out, and type-ahead -- pressing a letter selects the next
 * entry beginning with it, which is how anybody finds a file in a folder of
 * four hundred without the mouse.
 */
class FileListComponent : public Widget {
public:
  /** Empty until setDirectory(). */
  FileListComponent() {
    setAccessibilityIgnored(true);
    list_.withKeyboardFocus();
    list_.setAccessibleName("Files");
    list_.onSelectionChange = [this](int row) { selectionMoved(row); };
    list_.onRowChosen = [this](int row) { choose(row); };
    addChild(&list_);
  }

  /** Read a folder and show it. False, with nothing changed, for a path that
   *  is not a readable folder. */
  bool setDirectory(const std::string& directory) {
    if (directory.empty() || !isDirectory(directory)) return false;
    directory_ = directory;
    reload(std::string());
    if (onDirectoryChanged) onDirectoryChanged(directory_);
    return true;
  }
  /** The folder shown, as setDirectory was given it; empty before the first. */
  const std::string& directory() const { return directory_; }

  /** Up one folder, keeping the folder just left selected -- so going up and
   *  straight back in is two keys, not a hunt. */
  bool goUp() {
    const std::string up = parentPath(directory_);
    if (up.empty() || up == directory_) return false;
    const std::string was = directory_;
    if (!setDirectory(up)) return false;
    selectFile(was);
    return true;
  }

  /** Read the folder again, keeping the selection if its file is still there. */
  void refresh() { reload(selectedFile()); }

  /** List only files with one of these extensions (with or without the dot,
   *  any case); folders are always listed. Empty lists every file. Rereads
   *  the folder. */
  void setFileFilter(std::vector<std::string> extensions) {
    filter_ = std::move(extensions);
    refresh();
  }
  /** List hidden entries too: on Windows those marked hidden or system,
   *  elsewhere names starting with a dot. Off by default. Rereads the folder. */
  void setShowHidden(bool show) {
    showHidden_ = show;
    refresh();
  }
  /** List folders only, for choosing a directory. Rereads the folder. */
  void setDirectoriesOnly(bool only) {
    directoriesOnly_ = only;
    refresh();
  }

  /** The rows, as filtered: folders first, then files, each sorted by name
   *  ignoring ASCII case. */
  int numEntries() const { return (int) entries_.size(); }
  /** One row, 0-based. The index is not checked: keep it below numEntries(). */
  const FileEntry& entry(int index) const { return entries_[(size_t) index]; }

  /** The selected row, 0-based, or -1. */
  int selectedIndex() const { return list_.selectedRow(); }
  /** Select a row, clamped to the list; a negative index selects nothing. A
   *  change tells the preview and fires onSelectionChange. */
  void setSelectedIndex(int index) { list_.setSelectedRow(index); }
  /** The selected entry's whole path, or empty. */
  std::string selectedFile() const {
    const int row = list_.selectedRow();
    return row >= 0 && row < (int) entries_.size() ? entries_[(size_t) row].path : std::string();
  }
  /** Whether the selected entry is a folder; false when nothing is selected. */
  bool selectionIsDirectory() const {
    const int row = list_.selectedRow();
    return row >= 0 && row < (int) entries_.size() && entries_[(size_t) row].isDirectory;
  }
  /** Select an entry by path (or bare name). False if it is not in this folder. */
  bool selectFile(const std::string& path) {
    const std::string name = fileName(path);
    for (int i = 0; i < (int) entries_.size(); ++i)
      if (entries_[(size_t) i].path == path || entries_[(size_t) i].name == name) {
        list_.setSelectedRow(i);
        return true;
      }
    return false;
  }

  /** Told about every selection, so it can show the file. Not owned. */
  void setPreview(FilePreviewComponent* preview) { preview_ = preview; }

  /** Each time the selection moves: the entry's whole path, a folder's as
   *  well as a file's, or empty when nothing is selected (as after a reread). */
  std::function<void(const std::string& path)> onSelectionChange;
  /** A double click or Return on a FILE. A folder is walked into instead. */
  std::function<void(const std::string& path)> onFileChosen;
  /** After another folder is shown -- by setDirectory, goUp, or walking into
   *  one -- with its path. */
  std::function<void(const std::string& directory)> onDirectoryChanged;

  /** The ListBox underneath, for its look and its keys. Its onSelectionChange,
   *  onRowChosen and drawRow are this component's: replacing them cuts it off. */
  ListBox& list() { return list_; }

  void resized() override { list_.setBounds(localBounds()); }

  /** What the list did not want: Backspace goes up, a letter jumps. */
  bool keyPressed(const KeyPress& key) override {
    if (key.is(KeyPress::Backspace) && !key.ctrlDown) {
      goUp();
      return true;
    }
    if (key.isCharacter() && !key.ctrlDown && !key.altDown && key.character > ' ') {
      typeAhead(key.character);
      return true;
    }
    return false;
  }

  /** Each row's height, in logical pixels. */
  static constexpr float kRowHeight = 22.0f;

private:
  void reload(const std::string& keepSelected) {
    entries_ = listDirectory(directory_, filter_, true, showHidden_);
    if (directoriesOnly_) {
      std::vector<FileEntry> folders;
      for (FileEntry& e : entries_)
        if (e.isDirectory) folders.push_back(std::move(e));
      entries_ = std::move(folders);
    }
    std::vector<std::string> names;
    names.reserve(entries_.size());
    for (const FileEntry& e : entries_) names.push_back(e.name);
    list_.setSelectedRow(-1, false);
    // setItems for the names a reader is told, then the row drawing replaced
    // with one that shows what kind of thing each row is.
    list_.setItems(std::move(names));
    list_.setRowHeight(kRowHeight);
    list_.drawRow = [this](Graphics& g, int row, const Rect& area, bool selected) { drawEntry(g, row, area, selected); };
    if (!keepSelected.empty()) selectFile(keepSelected);
    else selectionMoved(-1);
    repaint();
  }

  void drawEntry(Graphics& g, int row, const Rect& area, bool selected) {
    if (row < 0 || row >= (int) entries_.size()) return;
    const FileEntry& e = entries_[(size_t) row];
    LookAndFeel& lf = lookAndFeel();
    lf.drawListRow(g, area, std::string(), font(), selected);
    lf.drawFileIcon(g, Rect(area.x + 4.0f, area.y, area.h, area.h), e.isDirectory);
    const float sizeW = e.isDirectory ? 0.0f : 70.0f;
    WidgetState st;
    lf.drawLabel(g, Rect(area.x + area.h + 8.0f, area.y, area.w - area.h - 12.0f - sizeW, area.h), e.name, font(),
                 Justify::Left, st);
    if (!e.isDirectory) {
      g.setColour(lf.dimText());
      font().drawIn(g, detail::chooserSizeText(e.sizeInBytes), Rect(area.right() - sizeW - 6.0f, area.y, sizeW, area.h),
                    Justify::Right);
    }
  }

  void selectionMoved(int row) {
    const std::string path = row >= 0 && row < (int) entries_.size() ? entries_[(size_t) row].path : std::string();
    const bool isFile = row >= 0 && row < (int) entries_.size() && !entries_[(size_t) row].isDirectory;
    if (preview_) preview_->selectedFileChanged(isFile ? path : std::string());
    if (onSelectionChange) onSelectionChange(path);
  }

  void choose(int row) {
    if (row < 0 || row >= (int) entries_.size()) return;
    const FileEntry chosen = entries_[(size_t) row];
    if (chosen.isDirectory) {
      setDirectory(chosen.path);
      return;
    }
    if (onFileChosen) onFileChosen(chosen.path);
  }

  void typeAhead(uint32_t c) {
    if (entries_.empty() || c > 0x7f) return;
    auto lower = [](uint32_t ch) { return (ch >= 'A' && ch <= 'Z') ? ch - 'A' + 'a' : ch; };
    const int n = (int) entries_.size();
    const int from = list_.selectedRow();
    for (int step = 1; step <= n; ++step) {
      const int i = ((from < 0 ? -1 : from) + step + n) % n;
      const std::string& name = entries_[(size_t) i].name;
      if (!name.empty() && lower((uint32_t) (unsigned char) name[0]) == lower(c)) {
        list_.setSelectedRow(i);
        return;
      }
    }
  }

  ListBox list_;
  std::string directory_;
  std::vector<FileEntry> entries_;
  std::vector<std::string> filter_;
  FilePreviewComponent* preview_ = nullptr;
  bool showHidden_ = false;
  bool directoriesOnly_ = false;
};

/**
 * FileBrowser rooted at one folder, with a preview hook.
 *
 * The tree itself is file_browser.h's -- lazy, one read per folder opened --
 * and this is the shape the reference gives it: a folder to show, a selected
 * file, and a preview told as the selection moves.
 */
class FileTreeComponent : public FileBrowser {
public:
  /** At the top of the disk -- every drive on Windows, "/" elsewhere -- until
   *  setDirectory(). */
  FileTreeComponent() {
    // Wrapped, not replaced: FileBrowser's own handler is what fires
    // onSelectionChange, which callers set.
    std::function<void(TreeNode*)> inner = tree().onSelectionChange;
    tree().onSelectionChange = [this, inner](TreeNode* node) {
      if (preview_) {
        const std::string path = node ? node->id : std::string();
        preview_->selectedFileChanged(!path.empty() && !isDirectory(path) ? path : std::string());
      }
      if (inner) inner(node);
    };
  }

  /** Rooted at `directory`, as setDirectory() roots it. */
  explicit FileTreeComponent(const std::string& directory) : FileTreeComponent() { setDirectory(directory); }

  /** Root the tree at a folder. A file roots it at the file's folder; an
   *  empty path at the top of the disk. */
  void setDirectory(const std::string& directory) { setRootPath(directory); }
  /** The selected path, a folder's or a file's, or empty. */
  std::string selectedFile() const { return selectedPath(); }
  /** Told about every selected FILE, and given empty for a folder or no
   *  selection. Not owned: it must outlive this tree, or be replaced by null
   *  before it goes. */
  void setPreview(FilePreviewComponent* preview) { preview_ = preview; }

private:
  FilePreviewComponent* preview_ = nullptr;
};

/**
 * Choosing a file or a folder, in a dialog over the editor.
 *
 * Add it to the editor's root (or let a FilenameComponent do so), then
 * show(). The answer arrives once, through the callback: the chosen path, or
 * empty when the user cancelled.
 */
class FileChooserDialogBox : public Overlay {
public:
  /** What the dialog chooses; the OK button reads Open, Save or Choose. */
  enum class Mode {
    /** An existing file. A typed name that is not there gets a "File not
     *  found" note instead of an answer. */
    openFile,
    /** A file to write, new or existing. An existing one is confirmed first
     *  unless the overwrite warning was turned off. */
    saveFile,
    /** A folder. Only folders are listed; with nothing typed or selected,
     *  the folder being shown is the answer. */
    chooseDirectory
  };

  /** The panel's largest width, in logical pixels. It shrinks to fit 8
   *  pixels inside the overlay's edges. */
  static constexpr float kMaxWidth = 640.0f;
  /** The panel's largest height, held inside the overlay like kMaxWidth. */
  static constexpr float kMaxHeight = 460.0f;
  /** The panel's inner margin and the gap between its rows. */
  static constexpr float kPad = 10.0f;
  /** The height of the Up, OK and Cancel buttons and the name field. */
  static constexpr float kRow = 26.0f;

  /**
   * `title` heads the panel and names the dialog to a screen reader;
   * `instructions`, when not empty, is a dimmer line under it.
   * `warnAboutOverwritingExistingFiles` is read in Mode::saveFile only.
   * Hidden until show().
   */
  explicit FileChooserDialogBox(std::string title = "Choose a file", std::string instructions = {},
                                Mode mode = Mode::openFile, bool warnAboutOverwritingExistingFiles = true)
      : title_(std::move(title)), instructions_(std::move(instructions)), mode_(mode),
        warnOverwrite_(warnAboutOverwritingExistingFiles) {
    setWantsKeyboardFocus(true);
    setAccessibleName(title_);
    onDismiss = [this]() { finish(std::string()); };

    up_.setLabel("Up");
    up_.setAccessibleName("Up one folder");
    up_.onClick = [this]() { list_.goUp(); };
    ok_.setLabel(mode_ == Mode::saveFile ? "Save" : (mode_ == Mode::chooseDirectory ? "Choose" : "Open"));
    ok_.onClick = [this]() { accept(); };
    cancel_.setLabel("Cancel");
    cancel_.onClick = [this]() { finish(std::string()); };
    filename_.setAccessibleName(mode_ == Mode::chooseDirectory ? "Folder" : "File name");
    filename_.onReturn = [this](const std::string&) { accept(); };

    list_.setDirectoriesOnly(mode_ == Mode::chooseDirectory);
    list_.onSelectionChange = [this](const std::string& path) {
      if (path.empty()) return;
      const bool folder = list_.selectionIsDirectory();
      if (mode_ == Mode::chooseDirectory || !folder) filename_.setText(fileName(path));
    };
    list_.onFileChosen = [this](const std::string& path) {
      filename_.setText(fileName(path));
      accept();
    };
    list_.onDirectoryChanged = [this](const std::string&) {
      if (mode_ == Mode::chooseDirectory) filename_.setText(std::string());
      repaint();
    };

    addChild(&up_);
    addChild(&list_);
    addChild(&filename_);
    addChild(&ok_);
    addChild(&cancel_);
    addChild(&question_);
  }

  /** Tab walks the dialog's own controls and stays inside it: the router's
   *  traversal would carry on into the editor underneath, which is the one
   *  place the keyboard must not go while a dialog is up. Escape cancels. */
  bool keyPressed(const KeyPress& key) override {
    if (key.is(KeyPress::Tab) && !key.ctrlDown && !key.altDown) {
      cycleFocus(key.shiftDown);
      return true;
    }
    return Overlay::keyPressed(key);
  }

  /** Only files with these extensions are listed (with their dot, any case);
   *  folders always are. Empty lists everything. */
  void setFileFilter(std::vector<std::string> extensions) { list_.setFileFilter(std::move(extensions)); }

  /** Shown beside the list and told about every file selected. Not owned. */
  void setPreviewComponent(FilePreviewComponent* preview) {
    if (preview_ && preview_->parent() == this) removeChild(preview_);
    preview_ = preview;
    list_.setPreview(preview);
    if (preview_) addChild(preview_);
    question_.toFront();
    resized();
  }

  /**
   * Show the dialog. `initial` is a folder to start in, or a file to start
   * beside (its name already in the field) -- or empty for the home folder.
   * False when the dialog has not been added to anything.
   */
  bool show(const std::string& initial, std::function<void(const std::string& chosen)> onResult) {
    if (!parent()) return false;
    onResult_ = std::move(onResult);
    std::string folder = initial, name;
    if (!folder.empty() && !isDirectory(folder)) {
      name = fileName(folder);
      folder = parentPath(folder);
    }
    if (folder.empty() || !isDirectory(folder)) folder = homeDirectory();
    if (folder.empty() || !isDirectory(folder)) {
      const std::vector<std::string> roots = rootPaths();
      folder = roots.empty() ? std::string() : roots.front();
    }
    list_.setDirectory(folder);
    filename_.setText(name);
    if (!name.empty()) list_.selectFile(name);
    toFront();
    coverParent();
    setVisible(true);
    resized();
    if (MouseRouter* r = routerFor(this)) r->setFocus(mode_ == Mode::saveFile ? (Component*) &filename_ : &list_.list());
    repaint();
    return true;
  }

  /** True from show() until the answer is given. */
  bool isShowing() const { return isVisible() && parent() != nullptr; }
  /** The mode it was made with. */
  Mode mode() const { return mode_; }
  /** The title it was made with. */
  const std::string& title() const { return title_; }

  /** Close with no answer, as Cancel does. */
  void dismiss() { finish(std::string()); }

  /** What OK does: open the typed or selected file, walk into a typed
   *  folder, ask before overwriting, or choose the folder. */
  void accept() {
    if (!isShowing()) return;
    const std::string dir = list_.directory();
    const std::string typed = filename_.getText();
    if (mode_ == Mode::chooseDirectory) {
      std::string chosen = typed.empty() ? (list_.selectionIsDirectory() ? list_.selectedFile() : dir)
                                         : (detail::chooserIsAbsolute(typed) ? typed : joinPath(dir, typed));
      if (isDirectory(chosen)) finish(chosen);
      return;
    }
    std::string path = typed.empty() ? (list_.selectionIsDirectory() ? std::string() : list_.selectedFile())
                                     : (detail::chooserIsAbsolute(typed) ? typed : joinPath(dir, typed));
    if (path.empty()) return;
    if (isDirectory(path)) {
      list_.setDirectory(path);
      filename_.setText(std::string());
      return;
    }
    if (mode_ == Mode::openFile) {
      if (fileExists(path)) finish(path);
      else question_.show("File not found", "\"" + fileName(path) + "\" is not in this folder.", {"OK"}, nullptr);
      return;
    }
    if (fileExists(path) && warnOverwrite_) {
      question_.show("Replace file?", "\"" + fileName(path) + "\" already exists. Replace it?", {"Replace", "Cancel"},
                     [this, path](int choice) {
                       if (choice == 0) finish(path);
                     });
      return;
    }
    finish(path);
  }

  // ── Its parts, for a caller arranging around it or a test driving it ─────

  /** The folder's list. Its callbacks are the dialog's; replacing them cuts
   *  the dialog off from it. */
  FileListComponent& fileList() { return list_; }
  /** The name field, which accept() reads before the selection. */
  TextEditor& filenameEditor() { return filename_; }
  /** Open, Save or Choose by mode; it runs accept(). */
  Button& okButton() { return ok_; }
  /** Closes with an empty answer, as dismiss() does. */
  Button& cancelButton() { return cancel_; }
  /** Goes up one folder. */
  Button& upButton() { return up_; }
  /** The overlay that says "File not found" and asks before replacing. */
  AlertOverlay& question() { return question_; }
  /** Whether that question is up; the dialog's answer waits for it. */
  bool isAskingAQuestion() const { return question_.isVisible(); }
  /** The folder the list shows. */
  std::string currentDirectory() const { return list_.directory(); }

  /** The panel, centred in the overlay: at most kMaxWidth by kMaxHeight and
   *  8 pixels inside its edges. */
  Rect panelBounds() const {
    const Rect b = localBounds();
    const float w = (std::min)(b.w - 16.0f, kMaxWidth), h = (std::min)(b.h - 16.0f, kMaxHeight);
    return {(b.w - w) * 0.5f, (b.h - h) * 0.5f, (std::max)(0.0f, w), (std::max)(0.0f, h)};
  }

  /** Top to bottom in the panel: the title and instructions, Up beside the
   *  folder's path, the list with the preview (if any) on its right -- at most
   *  200 wide, or 35% of the panel -- and the name field, OK and Cancel. */
  void resized() override {
    const Rect p = panelBounds();
    const float textH = font().lineHeight() + 4.0f;
    float y = p.y + kPad + textH + (instructions_.empty() ? 0.0f : textH);
    up_.setBounds({p.x + kPad, y, 50.0f, kRow});
    pathRow_ = Rect(p.x + kPad + 58.0f, y, p.w - kPad * 2.0f - 58.0f, kRow);
    y += kRow + kPad * 0.5f;
    const float bottom = p.bottom() - kPad - kRow;
    const float listH = (std::max)(0.0f, bottom - kPad - y);
    const float previewW = preview_ ? (std::min)(200.0f, p.w * 0.35f) : 0.0f;
    list_.setBounds({p.x + kPad, y, (std::max)(0.0f, p.w - kPad * 2.0f - previewW - (preview_ ? kPad : 0.0f)), listH});
    if (preview_) preview_->setBounds({p.right() - kPad - previewW, y, previewW, listH});
    cancel_.setBounds({p.right() - kPad - 80.0f, bottom, 80.0f, kRow});
    ok_.setBounds({p.right() - kPad * 2.0f - 160.0f, bottom, 80.0f, kRow});
    filename_.setBounds({p.x + kPad, bottom, (std::max)(0.0f, p.w - kPad * 4.0f - 160.0f), kRow});
    if (question_.isVisible()) {
      question_.coverParent();
      question_.resized();
    }
  }

  /** The dim, the panel, its title and instructions, and the shown folder's
   *  path beside Up. */
  void paint(Graphics& g) override {
    Overlay::paint(g);
    LookAndFeel& lf = lookAndFeel();
    const Rect p = panelBounds();
    g.setColour(lf.background());
    g.fillRoundedRect(p, 6.0f);
    g.setColour(lf.outline());
    g.drawRoundedRect(p, 6.0f, 1.0f);
    const float textH = font().lineHeight() + 4.0f;
    WidgetState st;
    lf.drawLabel(g, Rect(p.x + kPad, p.y + kPad, p.w - kPad * 2.0f, textH), title_, font(), Justify::Left, st);
    if (!instructions_.empty()) {
      st.enabled = false;
      lf.drawLabel(g, Rect(p.x + kPad, p.y + kPad + textH, p.w - kPad * 2.0f, textH), instructions_, font(),
                   Justify::Left, st);
    }
    g.setColour(lf.dimText());
    font().drawIn(g, list_.directory(), pathRow_, Justify::Left);
  }

  AccessibleInfo accessibleInfo() const override {
    AccessibleInfo info = baseInfo(AccessibleRole::Dialog);
    info.value = list_.directory();
    return info;
  }

private:
  void finish(const std::string& result) {
    if (!isVisible()) return;
    setVisible(false);
    if (MouseRouter* r = routerFor(this))
      if (isInside(r->focused())) r->setFocus(nullptr);
    repaint();
    std::function<void(const std::string&)> handler = std::move(onResult_);
    onResult_ = nullptr;
    if (handler) handler(result);
  }

  bool isInside(const Component* c) const {
    for (const Component* p = c; p; p = p->parent())
      if (p == this) return true;
    return false;
  }

  static void collectFocusable(Component* c, std::vector<Component*>& out) {
    if (!c->isVisible()) return;
    if (c->wantsKeyboardFocus()) out.push_back(c);
    for (Component* child : c->children()) collectFocusable(child, out);
  }

  void cycleFocus(bool backwards) {
    MouseRouter* router = routerFor(this);
    if (!router) return;
    std::vector<Component*> order;
    for (Component* child : children())
      if (child != &question_) collectFocusable(child, order);
    if (order.empty()) return;
    int at = -1;
    for (int i = 0; i < (int) order.size(); ++i)
      if (order[(size_t) i] == router->focused()) at = i;
    const int n = (int) order.size();
    const int next = at < 0 ? (backwards ? n - 1 : 0) : ((at + (backwards ? -1 : 1)) % n + n) % n;
    router->setFocus(order[(size_t) next]);
  }

  std::string title_, instructions_;
  Mode mode_;
  bool warnOverwrite_;
  Button up_, ok_, cancel_;
  FileListComponent list_;
  TextEditor filename_;
  AlertOverlay question_;
  FilePreviewComponent* preview_ = nullptr;
  Rect pathRow_;
  std::function<void(const std::string&)> onResult_;
};

namespace detail {

/** The drop-down arrow beside a filename field. */
class RecentFilesButton : public Widget {
public:
  RecentFilesButton() {
    setWantsKeyboardFocus(true);
    setAccessibleName("Recent files");
  }
  std::function<void(bool fromKeyboard)> onOpen;
  void paint(Graphics& g) override {
    lookAndFeel().drawComboBox(g, localBounds(), std::string(), font(), stateFor(pressed_));
    paintFocusRing(g);
  }
  void mouseDown(const MouseEvent&) override {
    if (!isEnabled()) return;
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

} // namespace detail

/**
 * A path in a field, a button to browse for it, and the last few chosen
 * behind a drop-down.
 *
 * Typing a path and pressing Return (or leaving the field) sets it; so does
 * dropping a file on it. The recently used list is the caller's to persist --
 * recentlyUsedFilenames() out, setRecentlyUsedFilenames() back in.
 */
class FilenameComponent : public Widget {
public:
  /**
   * `name` names the field to a screen reader. `currentFile` is shown without
   * firing onFileChanged or joining the recent list. `canEditFilename` false
   * makes the field read-only. `isDirectory` makes it a folder field: browsing
   * chooses a folder, only a dropped folder is taken, and no suffix is added.
   * `isForSaving` browses with a save dialog. `fileBrowserWildcard` filters
   * the browse dialog -- `*.wav;*.aif` or `wav, aif`; empty or `*` lists every
   * file. `enforcedSuffix` (`.wav` or `wav`) is appended to a file path whose
   * extension is not already it, in any case. `textWhenNothingSelected` is
   * the empty field's placeholder.
   */
  FilenameComponent(std::string name, const std::string& currentFile, bool canEditFilename = true,
                    bool isDirectory = false, bool isForSaving = false, std::string fileBrowserWildcard = {},
                    std::string enforcedSuffix = {}, std::string textWhenNothingSelected = {})
      : isDirectory_(isDirectory), isForSaving_(isForSaving), wildcard_(std::move(fileBrowserWildcard)),
        suffix_(std::move(enforcedSuffix)) {
    setAccessibleName(name);
    setAccessibilityIgnored(true);
    editor_.setAccessibleName(name);
    editor_.setPlaceholder(textWhenNothingSelected);
    editor_.setReadOnly(!canEditFilename);
    editor_.onReturn = [this](const std::string& text) { setCurrentFile(text, true, true); };
    editor_.onFocusLost = [this](const std::string& text) {
      if (text != current_) setCurrentFile(text, true, true);
    };
    recent_.onOpen = [this](bool fromKeyboard) { showRecentFilesMenu(fromKeyboard); };
    browse_.setLabel("...");
    browse_.setAccessibleName("Browse");
    browse_.onClick = [this]() { browse(); };
    addChild(&editor_);
    addChild(&recent_);
    addChild(&browse_);
    setCurrentFile(currentFile, false, false);
  }

  /** The path as set -- with the enforced suffix, if there is one. */
  const std::string& currentFile() const { return current_; }
  /** What the field holds now, which may be typed and not yet set. */
  std::string currentFileText() const { return editor_.getText(); }

  /** Set the path, with the enforced suffix added, and show it in the field.
   *  `addToRecentlyUsedList` puts it at the top of the recent list; `notify`
   *  fires onFileChanged if the path changed. The same path again only puts
   *  it back in the field. */
  void setCurrentFile(std::string path, bool addToRecentlyUsedList, bool notify = true) {
    if (!suffix_.empty() && !path.empty() && !isDirectory_) {
      std::string wanted = suffix_[0] == '.' ? suffix_ : "." + suffix_;
      if (fileExtension(path) != asciiLowered(wanted)) path += wanted;
    }
    if (addToRecentlyUsedList && !path.empty()) addRecentlyUsedFile(path);
    if (path == current_) {
      editor_.setText(current_);
      return;
    }
    current_ = std::move(path);
    editor_.setText(current_);
    if (notify && onFileChanged) onFileChanged(current_);
  }

  /** The path changed: by Return or leaving the field, a browse, the recent
   *  list, a drop, or setCurrentFile with `notify`. The new path, suffix
   *  included. */
  std::function<void(const std::string& path)> onFileChanged;
  /** Replace the default browse -- the in-editor dialog -- with your own:
   *  the system chooser from file_dialog.h, say. Call setCurrentFile with
   *  the answer. */
  std::function<void(FilenameComponent&)> onBrowse;

  /** False makes the field read-only: the path then changes only by
   *  browsing, the recent list or a drop. */
  void setFilenameIsEditable(bool editable) { editor_.setReadOnly(!editable); }
  /** The browse button's label, "..." by default. The button is sized to it
   *  at the next resized(). */
  void setBrowseButtonText(std::string text) { browse_.setLabel(std::move(text)); }
  /** Where browsing starts when the field is empty. */
  void setDefaultBrowseTarget(std::string path) { defaultTarget_ = std::move(path); }

  // ── Recently used ────────────────────────────────────────────────────────

  /** A copy of the recent list, most recent first, for the caller to save. */
  std::vector<std::string> recentlyUsedFilenames() const { return recent_files_; }
  /** Replace the recent list, in the order given. Empty entries are dropped,
   *  a repeated path appears once, and the list is cut to the maximum. */
  void setRecentlyUsedFilenames(std::vector<std::string> files) {
    recent_files_.clear();
    for (std::string& f : files)
      if (!f.empty()) addUnique(std::move(f), false);
    trimRecent();
  }
  /** Most recent first; a file already in the list moves to the top rather
   *  than appearing twice. */
  void addRecentlyUsedFile(const std::string& file) {
    if (file.empty()) return;
    addUnique(file, true);
    trimRecent();
  }
  /** How many recent files are kept: 20 by default, at least 1. A longer list
   *  loses its oldest. */
  void setMaxNumberOfRecentFiles(int maximum) {
    maxRecent_ = maximum < 1 ? 1 : maximum;
    trimRecent();
  }

  /** Drop the recent list down under the field; choosing an entry sets it,
   *  firing onFileChanged. `fromKeyboard` starts with the first entry lit,
   *  so Return chooses it. */
  void showRecentFilesMenu(bool fromKeyboard = false) {
    PopupMenu menu;
    if (recent_files_.empty()) menu.addItem(-1, "No recent files", false);
    for (size_t i = 0; i < recent_files_.size(); ++i) menu.addItem((int) i + 1, recent_files_[i]);
    recentMenu_.show(*this, menu, localBounds(), [this](int id) {
      if (id >= 1 && id <= (int) recent_files_.size()) setCurrentFile(recent_files_[(size_t) id - 1], true, true);
    }, PopupMenuOverlay::Placement::Under, fromKeyboard);
  }

  /** Browse: onBrowse if set, otherwise a FileChooserDialogBox over the
   *  editor, in the mode this field was made for. */
  void browse() {
    if (onBrowse) {
      onBrowse(*this);
      return;
    }
    Component* host = root();
    if (host == this) return;
    if (!dialog_) {
      const auto mode = isDirectory_ ? FileChooserDialogBox::Mode::chooseDirectory
                                     : (isForSaving_ ? FileChooserDialogBox::Mode::saveFile
                                                     : FileChooserDialogBox::Mode::openFile);
      dialog_.reset(new FileChooserDialogBox(isDirectory_ ? "Choose a folder" : (isForSaving_ ? "Save as" : "Choose a file"),
                                             std::string(), mode));
      dialog_->setFileFilter(detail::chooserExtensions(wildcard_));
    }
    if (dialog_->parent() != host) host->addChild(dialog_.get());
    dialog_->show(current_.empty() ? defaultTarget_ : current_, [this](const std::string& chosen) {
      if (!chosen.empty()) setCurrentFile(chosen, true, true);
    });
  }

  /** The dialog the first browse() made and added to the editor's root, or
   *  null before then (and always with onBrowse set). Owned by this. */
  FileChooserDialogBox* browserDialog() const { return dialog_.get(); }
  /** The path field. Its onReturn and onFocusLost are this component's. */
  TextEditor& editor() { return editor_; }
  /** The button that runs browse(). */
  Button& browseButton() { return browse_; }
  /** The recent files drop-down. */
  PopupMenuOverlay& recentFilesMenu() { return recentMenu_; }

  /** The field takes the width left of the recent-files arrow (22 wide) and
   *  the browse button (its label's width plus 16, at least 30). */
  void resized() override {
    const Rect b = localBounds();
    const float browseW = (std::max)(30.0f, font().stringWidth(browse_.label()) + 16.0f);
    const float recentW = 22.0f;
    browse_.setBounds({b.w - browseW, 0.0f, browseW, b.h});
    recent_.setBounds({b.w - browseW - recentW - 2.0f, 0.0f, recentW, b.h});
    editor_.setBounds({0.0f, 0.0f, (std::max)(0.0f, b.w - browseW - recentW - 4.0f), b.h});
  }

  bool isInterestedInFileDrag(const std::vector<std::string>& files) override {
    if (files.empty()) return false;
    return isDirectory_ ? sonore::isDirectory(files.front()) : !sonore::isDirectory(files.front());
  }
  /** The first dropped path becomes the file, if it is the right kind -- a
   *  folder for a folder field, a file otherwise -- and joins the recent list. */
  void filesDropped(const std::vector<std::string>& files, Point) override {
    if (!files.empty() && isInterestedInFileDrag(files)) setCurrentFile(files.front(), true, true);
  }

private:
  static std::string asciiLowered(std::string s) {
    for (char& c : s) c = asciiLowerChar(c);
    return s;
  }

  void addUnique(std::string file, bool atFront) {
    for (size_t i = 0; i < recent_files_.size(); ++i)
      if (recent_files_[i] == file) {
        recent_files_.erase(recent_files_.begin() + (long) i);
        break;
      }
    if (atFront) recent_files_.insert(recent_files_.begin(), std::move(file));
    else recent_files_.push_back(std::move(file));
  }

  void trimRecent() {
    if ((int) recent_files_.size() > maxRecent_) recent_files_.resize((size_t) maxRecent_);
  }

  bool isDirectory_, isForSaving_;
  std::string wildcard_, suffix_, defaultTarget_;
  std::string current_;
  std::vector<std::string> recent_files_;
  int maxRecent_ = 20;
  TextEditor editor_;
  detail::RecentFilesButton recent_;
  Button browse_;
  PopupMenuOverlay recentMenu_;
  std::unique_ptr<FileChooserDialogBox> dialog_;
};

/**
 * A list of folders to look in, edited in place: add, remove, change, and
 * move up or down -- the order is the search order.
 */
class FileSearchPathListComponent : public Widget {
public:
  /** Empty, with five buttons under the list: +, -, Change..., Up and Down.
   *  A double click or Return on a row changes that folder. */
  FileSearchPathListComponent() {
    setAccessibilityIgnored(true);
    list_.withKeyboardFocus();
    list_.setAccessibleName("Search folders");
    list_.onRowChosen = [this](int row) { changeDirectory(row); };
    const char* labels[5] = {"+", "-", "Change...", "Up", "Down"};
    const char* names[5] = {"Add folder", "Remove folder", "Change folder", "Move up", "Move down"};
    for (int i = 0; i < 5; ++i) {
      buttons_[i].setLabel(labels[i]);
      buttons_[i].setAccessibleName(names[i]);
      addChild(&buttons_[i]);
    }
    buttons_[0].onClick = [this]() { browseToAdd(); };
    buttons_[1].onClick = [this]() { removeDirectory(list_.selectedRow()); };
    buttons_[2].onClick = [this]() { changeDirectory(list_.selectedRow()); };
    buttons_[3].onClick = [this]() { moveSelected(-1); };
    buttons_[4].onClick = [this]() { moveSelected(+1); };
    addChild(&list_);
    refreshList();
  }

  /** The folders, in search order. */
  const std::vector<std::string>& path() const { return dirs_; }
  /** Replace the folders. Empty entries are dropped and a repeated folder is
   *  kept once, where it first appears. Does not fire onChange. */
  void setPath(std::vector<std::string> directories) {
    dirs_.clear();
    for (std::string& d : directories)
      if (!d.empty() && !contains(d)) dirs_.push_back(std::move(d));
    refreshList();
  }

  /** The folders joined with ';' -- the form a settings file keeps. */
  std::string pathString() const {
    std::string out;
    for (size_t i = 0; i < dirs_.size(); ++i) out += (i ? ";" : "") + dirs_[i];
    return out;
  }
  /** Split on ';' and setPath(): the form pathString() writes. Does not fire
   *  onChange. */
  void setPathString(const std::string& joined) {
    std::vector<std::string> parts;
    std::string current;
    for (char c : joined) {
      if (c == ';') {
        parts.push_back(current);
        current.clear();
      } else {
        current += c;
      }
    }
    parts.push_back(current);
    setPath(std::move(parts));
  }

  /** Fired whenever the list changes. */
  std::function<void()> onChange;
  /** Replace the default chooser (a FileChooserDialogBox picking a folder):
   *  call `answer` with the folder, or with empty to cancel. */
  std::function<void(std::function<void(const std::string&)> answer)> onBrowseForDirectory;

  /** Insert a folder at a 0-based `index` (-1, or out of range, appends),
   *  select it and fire onChange. False, with nothing changed, for an empty
   *  path or one already listed. The folder is not checked on disk. */
  bool addDirectory(const std::string& directory, int index = -1) {
    if (directory.empty() || contains(directory)) return false;
    if (index < 0 || index > (int) dirs_.size()) index = (int) dirs_.size();
    dirs_.insert(dirs_.begin() + (long) index, directory);
    refreshList();
    list_.setSelectedRow(index);
    changed();
    return true;
  }

  /** Remove the folder at a 0-based index, select the one now in its place
   *  (or the last), and fire onChange. Out of range does nothing. */
  void removeDirectory(int index) {
    if (index < 0 || index >= (int) dirs_.size()) return;
    dirs_.erase(dirs_.begin() + (long) index);
    refreshList();
    if (!dirs_.empty()) list_.setSelectedRow(index < (int) dirs_.size() ? index : (int) dirs_.size() - 1);
    changed();
  }

  /** Move a folder from one 0-based index to another, select it there and
   *  fire onChange. Nothing happens when either index is out of range or
   *  they are equal. */
  void moveDirectory(int from, int to) {
    if (from < 0 || from >= (int) dirs_.size() || to < 0 || to >= (int) dirs_.size() || from == to) return;
    std::string moving = dirs_[(size_t) from];
    dirs_.erase(dirs_.begin() + (long) from);
    dirs_.insert(dirs_.begin() + (long) to, std::move(moving));
    refreshList();
    list_.setSelectedRow(to);
    changed();
  }

  /** The list of folders. Its onRowChosen is this component's. */
  ListBox& list() { return list_; }
  /** "+": asks for a folder and appends it. */
  Button& addButton() { return buttons_[0]; }
  /** "-": removes the selected folder. */
  Button& removeButton() { return buttons_[1]; }
  /** "Change...": asks for a folder to replace the selected one. */
  Button& changeButton() { return buttons_[2]; }
  /** "Up": moves the selected folder one place earlier in the search. */
  Button& moveUpButton() { return buttons_[3]; }
  /** "Down": moves the selected folder one place later. */
  Button& moveDownButton() { return buttons_[4]; }
  /** The folder chooser the first + or Change... made and added to the
   *  editor's root, or null before then (and always with
   *  onBrowseForDirectory set). Owned by this. */
  FileChooserDialogBox* browserDialog() const { return dialog_.get(); }

  /** The list over one row of the five buttons, 26 high. */
  void resized() override {
    const Rect b = localBounds();
    const float rowH = 26.0f;
    list_.setBounds({0.0f, 0.0f, b.w, (std::max)(0.0f, b.h - rowH - 4.0f)});
    const float widths[5] = {30.0f, 30.0f, 80.0f, 50.0f, 56.0f};
    float x = 0.0f;
    for (int i = 0; i < 5; ++i) {
      buttons_[i].setBounds({x, b.h - rowH, widths[i], rowH});
      x += widths[i] + 4.0f;
    }
  }

  /** Delete removes the selected folder. */
  bool keyPressed(const KeyPress& key) override {
    if (key.is(KeyPress::Delete) && list_.selectedRow() >= 0) {
      removeDirectory(list_.selectedRow());
      return true;
    }
    return false;
  }

  /** Folders dropped on the list are added. */
  bool isInterestedInFileDrag(const std::vector<std::string>& files) override {
    for (const std::string& f : files)
      if (isDirectory(f)) return true;
    return false;
  }
  /** Each dropped folder is appended, as addDirectory() does; files are
   *  ignored. */
  void filesDropped(const std::vector<std::string>& files, Point) override {
    for (const std::string& f : files)
      if (isDirectory(f)) addDirectory(f);
  }

private:
  bool contains(const std::string& d) const {
    for (const std::string& existing : dirs_)
      if (existing == d) return true;
    return false;
  }

  void refreshList() {
    const int was = list_.selectedRow();
    list_.setItems(dirs_);
    if (was >= (int) dirs_.size()) list_.setSelectedRow((int) dirs_.size() - 1, false);
    repaint();
  }

  void changed() {
    if (onChange) onChange();
  }

  void moveSelected(int direction) {
    const int at = list_.selectedRow();
    moveDirectory(at, at + direction);
  }

  void askForDirectory(const std::string& start, std::function<void(const std::string&)> answer) {
    if (onBrowseForDirectory) {
      onBrowseForDirectory(std::move(answer));
      return;
    }
    Component* host = root();
    if (host == this) return;
    if (!dialog_) dialog_.reset(new FileChooserDialogBox("Choose a folder", std::string(),
                                                         FileChooserDialogBox::Mode::chooseDirectory));
    if (dialog_->parent() != host) host->addChild(dialog_.get());
    dialog_->show(start, std::move(answer));
  }

  void browseToAdd() {
    askForDirectory(dirs_.empty() ? std::string() : dirs_.back(), [this](const std::string& chosen) {
      if (!chosen.empty()) addDirectory(chosen);
    });
  }

  void changeDirectory(int index) {
    if (index < 0 || index >= (int) dirs_.size()) return;
    askForDirectory(dirs_[(size_t) index], [this, index](const std::string& chosen) {
      if (chosen.empty() || index >= (int) dirs_.size() || contains(chosen)) return;
      dirs_[(size_t) index] = chosen;
      refreshList();
      list_.setSelectedRow(index);
      changed();
    });
  }

  std::vector<std::string> dirs_;
  ListBox list_;
  Button buttons_[5];
  std::unique_ptr<FileChooserDialogBox> dialog_;
};

} // namespace gfx
} // namespace sonore
