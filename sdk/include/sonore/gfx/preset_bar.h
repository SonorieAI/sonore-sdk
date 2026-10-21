// SPDX-License-Identifier: Apache-2.0
//
// The preset bar: the strip along the top of an editor that says which
// preset is playing and does everything a user does with presets.
//
//   [ < ][ > ][ Warm Glue *                      v ][ Save ]
//
// The name, with " *" when the sound has moved since the preset was loaded or
// saved; previous and next through factory and user presets as one list; and
// one menu -- Save, Save As, Rename, Delete, Duplicate, the factory list, the
// user's list, Import, Export, Show Folder, Rescan. Everything it does goes
// through the instance's PresetManager (user_presets.h), which is what puts a
// preset on the audio thread by the same path a host's session restore takes.
//
// ── Asking, without a dialog ────────────────────────────────────────────────
//
// "Save as what?", "Replace it?", "Delete it?" are questions, and a plugin
// asks questions with overlays drawn inside its own editor (alert.h says why:
// an OS dialog from a plugin can land behind the host and hang it). The bar
// attaches them to the editor's root when it asks, so they cover the whole
// editor and not a 28-pixel strip. Only Import and Export open the OS's file
// dialog -- choosing a file is what that dialog is for, and every plugin that
// loads a sample already does it -- and both can be replaced (chooseFile), so
// a test drives them with no dialog at all.
//
// ── One undo step ───────────────────────────────────────────────────────────
//
// A load can be wrapped (wrapLoad): an editor with an undo history -- an
// AudioProcessorValueTreeState -- hands the load to editWholeState, and
// loading a preset becomes one step that Undo takes back whole, parameters
// and the plugin's own state together. An editor without one loads directly.
//
// ── Headless ────────────────────────────────────────────────────────────────
//
// It is a Widget and nothing else: the menu is a PopupMenuOverlay, the
// questions are overlays, and the clock is tick(). tests/user_presets_test.cpp
// renders it into a Bitmap and drives every item.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "../file_dialog.h"
#include "../user_presets.h"
#include "alert.h"
#include "popup_overlay.h"
#include "text_editor.h"
#include "widgets.h"

namespace sonore {
namespace gfx {

namespace detail {

/** The preset's name, drawn as a drop-down: clicking it opens the menu. */
class PresetNameField : public Widget {
public:
  PresetNameField() {
    setWantsKeyboardFocus(true);
    setAccessibleName("Preset");
  }

  void setText(const std::string& text) {
    if (text == text_) return;
    text_ = text;
    repaint();
  }
  const std::string& text() const { return text_; }
  /** What a reader hears for the value: "Warm Glue, modified" rather than
   *  the asterisk, which a reader reads out as "star". */
  void setSpokenValue(const std::string& text) { spoken_ = text; }

  std::function<void()> onClick;

  void paint(Graphics& g) override {
    lookAndFeel().drawComboBox(g, localBounds(), text_, font(), stateFor(pressed_));
    paintFocusRing(g);
  }

  void mouseDown(const MouseEvent&) override {
    if (!isEnabled()) return;
    pressed_ = true;
    repaint();
  }

  void mouseUp(const MouseEvent& e) override {
    const bool was = pressed_;
    pressed_ = false;
    repaint();
    if (was && isEnabled() && localBounds().contains(e.position) && onClick) onClick();
  }

  bool keyPressed(const KeyPress& key) override {
    if (!isEnabled()) return false;
    if (key.is(KeyPress::Return) || key.character == ' ' || key.is(KeyPress::Down)) {
      if (onClick) onClick();
      return true;
    }
    return false;
  }

  AccessibleInfo accessibleInfo() const override {
    AccessibleInfo info = baseInfo(AccessibleRole::ComboBox);
    info.value = spoken_.empty() ? text_ : spoken_;
    if (isEnabled()) info.actions |= accessibleActionBit(AccessibleAction::Press);
    return info;
  }

  bool performAccessibleAction(AccessibleAction action) override {
    if (action == AccessibleAction::Press && isEnabled() && onClick) {
      onClick();
      return true;
    }
    return Widget::performAccessibleAction(action);
  }

private:
  std::string text_, spoken_;
  bool pressed_ = false;
};

/**
 * A question with a line to type into -- "Save preset as", "Rename preset" --
 * as an overlay over the editor, in the arrangement AlertOverlay uses.
 *
 * The answer is checked BEFORE the prompt goes away: `accept` returns
 * whether it was acceptable, and a refusal stays up with its reason under
 * the field, because a name rejected after the box closed is a name the user
 * has to type again from nothing. `after` runs once the prompt is down -- so
 * a question it asks next (the name exists: replace it?) is not covered by a
 * prompt still on its way out.
 */
class PresetNamePrompt : public Overlay {
public:
  PresetNamePrompt() {
    setWantsKeyboardFocus(true);
    onDismiss = [this]() { finish(false); };
    ok_.onClick = [this]() { finish(true); };
    cancel_.onClick = [this]() { finish(false); };
    field_.onReturn = [this](const std::string&) { finish(true); };
    field_.setAccessibleName("Name");
    addChild(&field_);
    addChild(&ok_);
    addChild(&cancel_);
  }

  /** `accept(text)` on OK: false keeps the prompt up (after setMessage).
   *  `after(ok, text)` once it is down, for OK and Cancel alike. */
  void show(const std::string& title, const std::string& text, const std::string& okLabel,
            std::function<bool(const std::string& text)> accept,
            std::function<void(bool ok, const std::string& text)> after) {
    title_ = title;
    message_.clear();
    accept_ = std::move(accept);
    after_ = std::move(after);
    ok_.setLabel(okLabel);
    cancel_.setLabel("Cancel");
    field_.setText(text);
    field_.selectAll();
    coverParent();
    setVisible(true);
    resized();
    if (MouseRouter* r = routerFor(this)) r->setFocus(&field_);
    repaint();
  }

  bool isShowing() const { return isVisible(); }
  /** Why the last answer was refused, shown under the field. */
  void setMessage(const std::string& message) {
    message_ = message;
    repaint();
  }
  const std::string& message() const { return message_; }
  const std::string& title() const { return title_; }
  TextEditor& field() { return field_; }
  Button& okButton() { return ok_; }
  Button& cancelButton() { return cancel_; }

  AccessibleInfo accessibleInfo() const override {
    AccessibleInfo info = baseInfo(AccessibleRole::Dialog);
    if (info.name.empty()) info.name = title_;
    info.value = message_;
    return info;
  }

  Rect panelBounds() const {
    const Rect b = localBounds();
    const float w = b.w * 0.8f < kMaxWidth ? b.w * 0.8f : kMaxWidth;
    const float h = kPad * 4.0f + kLine * 2.0f + kFieldHeight + kButtonHeight;
    return {(b.w - w) * 0.5f, (b.h - h) * 0.5f, w, h};
  }

  void resized() override {
    const Rect panel = panelBounds();
    field_.setBounds({panel.x + kPad, panel.y + kPad + kLine + kPad * 0.5f, panel.w - kPad * 2.0f, kFieldHeight});
    const float y = panel.bottom() - kPad - kButtonHeight;
    cancel_.setBounds({panel.right() - kPad - kButtonWidth, y, kButtonWidth, kButtonHeight});
    ok_.setBounds({panel.right() - kPad * 1.5f - kButtonWidth * 2.0f, y, kButtonWidth, kButtonHeight});
  }

  void paint(Graphics& g) override {
    Overlay::paint(g);
    LookAndFeel& lf = lookAndFeel();
    const Rect panel = panelBounds();
    g.setColour(lf.background());
    g.fillRoundedRect(panel, 6.0f);
    g.setColour(lf.outline());
    g.drawRoundedRect(panel, 6.0f, 1.0f);
    lf.drawLabel(g, Rect(panel.x + kPad, panel.y + kPad, panel.w - kPad * 2.0f, kLine), title_, font(),
                 Justify::Left, stateFor(false));
    if (!message_.empty()) {
      g.setColour(Colour::fromRGB(0xff6b6b));
      font().drawIn(g, message_,
                    Rect(panel.x + kPad, field_.bounds().bottom() + kPad * 0.25f, panel.w - kPad * 2.0f, kLine),
                    Justify::Left);
    }
  }

  bool keyPressed(const KeyPress& key) override {
    if (key.is(KeyPress::Return)) {
      finish(true);
      return true;
    }
    return Overlay::keyPressed(key);
  }

private:
  void finish(bool ok) {
    if (!isVisible()) return;
    const std::string text = field_.getText();
    if (ok && accept_) {
      // A copy runs, not the member: accepting may show this prompt again,
      // which replaces accept_ -- the function executing.
      std::function<bool(const std::string&)> accept = accept_;
      if (!accept(text)) return; // refused: stays up, with its reason
    }
    setVisible(false);
    if (MouseRouter* r = routerFor(this)) {
      const Component* focus = r->focused();
      if (focus == this || focus == &field_) r->setFocus(nullptr);
    }
    repaint();
    std::function<void(bool, const std::string&)> after = std::move(after_);
    accept_ = nullptr;
    after_ = nullptr;
    if (after) after(ok, text);
  }

  static constexpr float kMaxWidth = 380.0f;
  static constexpr float kPad = 14.0f;
  static constexpr float kLine = 20.0f;
  static constexpr float kFieldHeight = 26.0f;
  static constexpr float kButtonHeight = 26.0f;
  static constexpr float kButtonWidth = 84.0f;

  std::string title_, message_;
  TextEditor field_;
  Button ok_, cancel_;
  std::function<bool(const std::string&)> accept_;
  std::function<void(bool, const std::string&)> after_;
};

} // namespace detail

/**
 * The strip along the top of an editor that names the current preset and
 * does everything a user does with presets, through a PresetManager. The
 * file comment covers the layout, the questions it asks, and how a load
 * becomes one undo step.
 *
 * The manager is not owned and must outlive the bar (or be replaced with
 * setManager); with none the bar shows nothing to do. Call tick() from the
 * editor's frame clock: it is what notices the sound changing and presets
 * appearing in the folder, and it reads that folder on the calling thread.
 * Message thread only; kHeight is the height it is designed for.
 */
class PresetBar : public Widget {
public:
  /** The height the bar is laid out for, logical pixels. */
  static constexpr float kHeight = 30.0f;

  /** The menu's ids. Above anything an editor's own menus use, so a menu bar
   *  can put these items in its own Presets menu and hand the choice back
   *  through perform(). Factory presets are kFactoryBase + i, the user's
   *  kUserBase + i. */
  enum MenuIds {
    kSave = 10001,  ///< save over the current user preset (saveOrSaveAs)
    kSaveAs,  ///< save under a new name (beginSaveAs)
    kRename,  ///< rename the current user preset (beginRename)
    kDelete,  ///< delete it, asking first (beginDelete)
    kDuplicate,  ///< copy it (duplicateCurrent)
    kImport,  ///< copy a preset file into the folder and load it (importFile)
    kExport,  ///< write the current sound to a file (exportCurrent)
    kShowFolder,  ///< show the preset folder (openFolder)
    kRescan,  ///< read the folder again
    kFactoryBase = 11000,  ///< plus i: load factory preset i
    kUserBase = 20000,  ///< plus i: load user preset i
  };

  /** A bar for `manager`, which is not owned; null shows nothing to do until
   *  setManager. */
  explicit PresetBar(PresetManager* manager = nullptr) : manager_(manager) {
    setAccessibleName("Presets");
    previous_.setTooltip("Previous preset");
    next_.setTooltip("Next preset");
    save_.setTooltip("Save the current sound");
    name_.setTooltip("Presets: load, save, rename, import, export");
    previous_.setAccessibleName("Previous preset");
    next_.setAccessibleName("Next preset");
    previous_.onClick = [this]() { step(-1); };
    next_.onClick = [this]() { step(+1); };
    save_.onClick = [this]() { saveOrSaveAs(); };
    name_.onClick = [this]() { openMenu(); };
    addChild(&previous_);
    addChild(&next_);
    addChild(&name_);
    addChild(&save_);
    refresh();
  }

  /** Dismisses its menu if it is open; its overlays leave the editor as they
   *  are destroyed. */
  ~PresetBar() override {
    // The overlays may be in the editor's root rather than in this bar; they
    // leave it as they are destroyed, which Component's destructor does.
    menu_.dismiss(0);
  }

  /** Not copyable: its buttons' callbacks point at this object. */
  PresetBar(const PresetBar&) = delete;
  PresetBar& operator=(const PresetBar&) = delete;

  /** Work through `manager` from now on (not owned; null for none), and show
   *  its state at once. */
  void setManager(PresetManager* manager) {
    manager_ = manager;
    seenRevision_ = ~(uint64_t) 0;
    refresh();
  }
  /** The manager in use, or null. */
  PresetManager* manager() const { return manager_; }

  // ── What an editor may replace ──────────────────────────────────────────

  /**
   * Wraps every load. Given a label ("Load preset \"Warm Glue\"") and the load
   * itself; returns whether it worked. An editor with an undo history makes
   * the load one step of it:
   *
   *   bar.wrapLoad = [&apvts](const std::string& label, const std::function<bool(std::string*)>& load,
   *                           std::string* why) { return apvts.editWholeState(label, load, why); };
   */
  std::function<bool(const std::string& label, const std::function<bool(std::string*)>& load, std::string* why)>
      wrapLoad;

  /** A file to import (`forSaving` false) or to export to (true). Defaults
   *  to the OS's file dialog; empty means cancelled. */
  std::function<std::string(bool forSaving, const std::string& suggestedName)> chooseFile;

  /** Open the preset folder. Defaults to showFolderInFileManager. */
  std::function<bool(const std::string& folder)> showFolder;

  // ── The clock ───────────────────────────────────────────────────────────

  /**
   * One UI frame. The "modified" marker is re-judged a few times a second
   * rather than every frame -- it hashes the plugin's state -- and at once
   * whenever the manager says something changed. Every couple of seconds the
   * folder is read again, so a preset copied in from outside appears in the
   * list without anybody asking.
   */
  void tick() {
    ++ticks_;
    if (!manager_) return;
    if (ticks_ % kRescanTicks == 0 && !busy()) manager_->rescan();
    if (manager_->revision() != seenRevision_ || ticks_ % kDirtyTicks == 0) refresh();
  }

  /** What the bar says now: the name, " *" when modified. */
  std::string displayedName() const { return name_.text(); }
  /** Whether the name is marked modified: the sound differs from the preset
   *  loaded, as of the last check. */
  bool showsModified() const { return dirty_; }

  // ── The menu ────────────────────────────────────────────────────────────

  /** Every item, built now: what is enabled and ticked is decided by the
   *  state at the moment somebody looks. */
  PopupMenu buildMenu() const {
    PopupMenu menu;
    if (!manager_) return menu;
    const bool user = manager_->currentIsUser();
    menu.addItem(kSave, "Save", user && manager_->isDirty());
    menu.addItem(kSaveAs, "Save As...");
    menu.addItem(kRename, "Rename...", user);
    menu.addItem(kDelete, "Delete...", user);
    menu.addItem(kDuplicate, "Duplicate", user);
    const int current = manager_->currentIndex();
    if (manager_->numFactory() > 0) {
      menu.addSeparator();
      menu.addSectionHeader("Factory");
      for (int i = 0; i < manager_->numFactory(); ++i)
        menu.addItem(kFactoryBase + i, manager_->itemAt(i).name, true, current == i);
    }
    menu.addSeparator();
    menu.addSectionHeader("User");
    const int factory = manager_->numFactory();
    if (manager_->numUser() == 0) menu.addItem(kUserBase - 1, "(none saved yet)", false);
    for (int u = 0; u < manager_->numUser() && u < 100000; ++u)
      menu.addItem(kUserBase + u, manager_->itemAt(factory + u).name, true, current == factory + u);
    menu.addSeparator();
    menu.addItem(kImport, "Import...");
    menu.addItem(kExport, "Export...");
    menu.addItem(kShowFolder, "Show Folder", !manager_->store().folder().empty());
    menu.addItem(kRescan, "Rescan Folder");
    return menu;
  }

  /** Do what menu item `id` says. False when the id is not one of these. */
  bool perform(int id) {
    if (!manager_) return false;
    if (id >= kUserBase && id < kUserBase + 100000) {
      loadItem(manager_->numFactory() + (id - kUserBase));
      return true;
    }
    if (id >= kFactoryBase && id < kFactoryBase + manager_->numFactory()) {
      loadItem(id - kFactoryBase);
      return true;
    }
    switch (id) {
      case kSave: saveOrSaveAs(); return true;
      case kSaveAs: beginSaveAs(); return true;
      case kRename: beginRename(); return true;
      case kDelete: beginDelete(); return true;
      case kDuplicate: duplicateCurrent(); return true;
      case kImport: importFile(); return true;
      case kExport: exportCurrent(); return true;
      case kShowFolder: openFolder(); return true;
      case kRescan:
        manager_->rescan();
        refresh();
        return true;
      default: return false;
    }
  }

  /** Drop the menu down under the name. The folder is read first, so the
   *  list is the folder as it is now. */
  void openMenu() {
    if (!manager_) return;
    manager_->rescan();
    refresh();
    menu_.show(name_, buildMenu(), name_.localBounds(), [this](int id) {
      if (id != 0) perform(id);
    });
  }

  // ── The actions ─────────────────────────────────────────────────────────

  /** Load item `index` of the combined list, through wrapLoad. A failure
   *  is shown -- unless `whyOut` is given, and then it is only reported. */
  bool loadItem(int index, std::string* whyOut = nullptr) {
    if (!manager_) return false;
    const PresetManager::Item item = manager_->itemAt(index);
    if (item.origin == PresetManager::Origin::Unset) return false;
    std::string why;
    PresetManager* m = manager_;
    auto load = [m, index](std::string* w) { return m->load(index, w); };
    const std::string label = "Load preset \"" + item.name + "\"";
    const bool ok = wrapLoad ? wrapLoad(label, load, &why) : load(&why);
    refresh();
    if (!ok && whyOut) *whyOut = why;
    else if (!ok) showError("\"" + item.name + "\" could not be loaded", why);
    return ok;
  }

  /**
   * Previous (-1) or next (+1), through factory and user as one list.
   *
   * A preset that will not load -- damaged, or from a newer version -- is
   * stepped OVER, not stopped at: otherwise one bad file in the folder is a
   * wall that next and previous can never get past, and pressing next looks
   * like doing nothing. Only when nothing at all loads is anything said.
   */
  bool step(int direction) {
    if (!manager_) return false;
    manager_->rescan();
    const int n = manager_->count();
    if (n == 0) return false;
    const int current = manager_->currentIndex();
    std::string first;
    for (int k = 1; k <= n; ++k) {
      const int target =
          current < 0 ? (direction > 0 ? k - 1 : n - k) : ((current + direction * k) % n + n) % n;
      std::string why;
      if (loadItem(target, &why)) return true;
      if (first.empty()) first = "\"" + manager_->itemAt(target).name + "\": " + why;
    }
    showError("No preset could be loaded", first);
    return false;
  }

  /** Save over the current user preset, or ask for a name when there is
   *  none to save over -- the one button does what the user meant. */
  void saveOrSaveAs() {
    if (!manager_) return;
    if (!manager_->currentIsUser()) {
      beginSaveAs();
      return;
    }
    std::string why;
    if (!manager_->saveCurrent(&why)) showError("The preset could not be saved", why);
    refresh();
  }

  /** Ask for a name -- the current preset's, or "New Preset", suggested -- and
   *  save the current sound under it; a name already used asks whether to
   *  replace it or keep both. Returns at once; the prompt finishes the job. */
  void beginSaveAs() {
    if (!manager_) return;
    const std::string suggested = manager_->hasSelection() ? manager_->currentName() : std::string("New Preset");
    showPrompt(
        "Save preset as", suggested, "Save",
        [this](const std::string& text) {
          if (!cleanPresetName(text).empty()) return true;
          prompt_.setMessage("A preset needs a name.");
          return false;
        },
        [this](bool ok, const std::string& text) {
          if (!ok) return;
          const std::string name = cleanPresetName(text);
          if (!manager_->store().nameExists(name)) {
            finishSaveAs(name, UserPresetStore::IfExists::Fail);
            return;
          }
          // A clash is a question, not an error: replacing a preset is
          // something people mean to do, and keeping both is the other
          // thing they mean.
          showAlert("A preset called \"" + name + "\" already exists.",
                    "Replace it with the current sound, or keep both?", {"Replace", "Keep both", "Cancel"},
                    [this, name](int choice) {
                      if (choice == 0) finishSaveAs(name, UserPresetStore::IfExists::Replace);
                      else if (choice == 1) finishSaveAs(name, UserPresetStore::IfExists::KeepBoth);
                    });
        });
  }

  /** Ask for a new name for the current user preset and rename it, refusing an
   *  empty or taken name in the prompt. Does nothing for a factory preset. */
  void beginRename() {
    if (!manager_ || !manager_->currentIsUser()) return;
    const std::string file = fileOf(manager_->currentIndex());
    showPrompt(
        "Rename preset", manager_->currentName(), "Rename",
        [this, file](const std::string& text) {
          const std::string name = cleanPresetName(text);
          if (name.empty()) {
            prompt_.setMessage("A preset needs a name.");
            return false;
          }
          if (name == manager_->currentName()) return true;
          // Found again by its file at every try: a refused rename can have
          // rescanned the folder under the prompt (see beginDelete).
          const int index = indexOfFile(file);
          if (index < 0) {
            prompt_.setMessage("The preset is no longer in the folder.");
            return false;
          }
          if (manager_->store().nameExists(name, index - manager_->numFactory())) {
            prompt_.setMessage("A preset called \"" + name + "\" already exists.");
            return false;
          }
          std::string why;
          if (!manager_->rename(index, name, &why)) {
            prompt_.setMessage(why);
            return false;
          }
          return true;
        },
        [this](bool, const std::string&) { refresh(); });
  }

  /**
   * Delete the current preset, asking first.
   *
   * The preset is held by its FILE across the questions, never by its place
   * in the list. A refused trash rescans the folder, and whatever changed it
   * while the question was up -- another instance saving a preset, a sync
   * client -- moves everything after it along: "Delete permanently" by the
   * index taken before deleted the neighbour, for good, under a question
   * naming this one.
   */
  void beginDelete() {
    if (!manager_ || !manager_->currentIsUser()) return;
    const std::string file = fileOf(manager_->currentIndex());
    const std::string name = manager_->currentName();
    showAlert("Delete \"" + name + "\"?", std::string("It goes to the ") + trashName() + ".", {"Delete", "Cancel"},
              [this, file, name](int choice) {
                if (choice != 0) return;
                std::string why;
                const int index = indexOfFile(file);
                if (index < 0) {
                  showError("\"" + name + "\" could not be deleted", "it is no longer in the folder");
                  refresh();
                  return;
                }
                if (manager_->remove(index, UserPresetStore::Removal::ToTrash, &why)) {
                  refresh();
                  return;
                }
                // Not deleted for good behind the user's back: asked again.
                showAlert("\"" + name + "\" could not be moved to the " + trashName() + ".",
                          why + "\nDelete it permanently instead?", {"Delete permanently", "Cancel"},
                          [this, file, name](int again) {
                            if (again != 0) return;
                            std::string reason = "it is no longer in the folder";
                            const int now = indexOfFile(file);
                            if (now < 0 || !manager_->remove(now, UserPresetStore::Removal::Permanently, &reason))
                              showError("\"" + name + "\" could not be deleted", reason);
                            refresh();
                          });
              });
  }

  /** Copy the current user preset beside it; does nothing for a factory one.
   *  A failure is shown, not thrown. */
  void duplicateCurrent() {
    if (!manager_ || !manager_->currentIsUser()) return;
    std::string why;
    if (!manager_->duplicate(manager_->currentIndex(), &why)) showError("The preset could not be duplicated", why);
    refresh();
  }

  /** Ask for a preset file (through chooseFile), copy it into the folder and
   *  load it. Cancelling does nothing; a failure is shown. */
  void importFile() {
    if (!manager_) return;
    const std::string path = choose(false, std::string());
    if (path.empty()) return;
    std::string why, copied;
    if (!manager_->store().importFile(path, &copied, &why)) {
      showError("That file could not be imported", why);
      return;
    }
    manager_->rescan();
    const int u = manager_->store().indexOfPath(copied);
    if (u >= 0) loadItem(manager_->numFactory() + u);
    refresh();
  }

  /** Ask where to write the current sound (through chooseFile) and write it
   *  there as a preset file. Cancelling does nothing; a failure is shown. */
  void exportCurrent() {
    if (!manager_) return;
    const std::string name = manager_->hasSelection() ? manager_->currentName() : std::string("Preset");
    const std::string path = choose(true, presetFileStem(name) + userPresetExtension());
    if (path.empty()) return;
    std::string why;
    if (!manager_->exportCurrent(path, &why)) showError("The preset could not be exported", why);
  }

  /** Show the preset folder in the system's file manager, making it first if
   *  it is not there yet. A failure is shown. */
  void openFolder() {
    if (!manager_) return;
    const std::string folder = manager_->store().folder();
    // Made first: "Show Folder" before the first save is how a user finds
    // out where presets will go, and a folder that is not there cannot be
    // shown.
    if (folder.empty() || !ensureDirectory(folder)) {
      showError("There is no preset folder", "This plugin has no place for presets on this machine.");
      return;
    }
    const bool shown = showFolder ? showFolder(folder) : showFolderInFileManager(folder);
    if (!shown) showError("The folder could not be opened", folder);
  }

  // ── For tests, and for an editor that lays the bar out itself ───────────

  /** The button that loads the previous preset. */
  Button& previousButton() { return previous_; }
  /** The button that loads the next preset. */
  Button& nextButton() { return next_; }
  /** The button that saves (saveOrSaveAs). */
  Button& saveButton() { return save_; }
  /** The name in the middle, which opens the menu when clicked. */
  Widget& nameField() { return name_; }
  /** The preset menu, drawn as an overlay in the editor. */
  PopupMenuOverlay& menuOverlay() { return menu_; }
  /** The question or message showing now (or last shown). */
  AlertOverlay& alert() { return alerts_[current_]; }
  /** The name prompt that Save As and Rename show. */
  detail::PresetNamePrompt& prompt() { return prompt_; }
  /** The last thing that went wrong, as shown. */
  const std::string& lastError() const { return lastError_; }

  // ── Drawing ─────────────────────────────────────────────────────────────

  void paint(Graphics& g) override {
    LookAndFeel& lf = lookAndFeel();
    g.setColour(lf.panel().darkened(0.15f));
    g.fillRect(localBounds());
    g.setColour(lf.outline());
    g.drawLine(0.0f, localBounds().h - 0.5f, localBounds().w, localBounds().h - 0.5f, 1.0f);
  }

  void resized() override {
    const Rect b = localBounds();
    const float pad = 4.0f, h = b.h - pad * 2.0f;
    const float arrow = h + 4.0f, saveW = 64.0f;
    previous_.setBounds({pad, pad, arrow, h});
    next_.setBounds({pad + arrow + 2.0f, pad, arrow, h});
    const float nameX = pad + arrow * 2.0f + 8.0f;
    const float nameW = b.w - nameX - saveW - pad * 2.0f;
    name_.setBounds({nameX, pad, nameW > 40.0f ? nameW : 40.0f, h});
    save_.setBounds({b.w - pad - saveW, pad, saveW, h});
  }

  AccessibleInfo accessibleInfo() const override {
    AccessibleInfo info = baseInfo(AccessibleRole::Group);
    return info;
  }

private:
  static constexpr uint64_t kDirtyTicks = 8;   // about four times a second
  static constexpr uint64_t kRescanTicks = 60; // about every two seconds

  static const char* trashName() {
#if defined(_WIN32)
    return "Recycle Bin";
#else
    return "Trash";
#endif
  }

  bool busy() const {
    return menu_.isOpen() || alerts_[0].isShowing() || alerts_[1].isShowing() || prompt_.isShowing();
  }

  /** The file behind item `index` of the combined list; empty for a factory
   *  preset or none. */
  std::string fileOf(int index) const {
    const int u = manager_ ? index - manager_->numFactory() : -1;
    if (u < 0 || u >= (int) manager_->store().entries().size()) return std::string();
    return manager_->store().entries()[(size_t) u].path;
  }

  /** Where that file is in the combined list as last scanned, or -1. */
  int indexOfFile(const std::string& file) const {
    if (!manager_ || file.empty()) return -1;
    const int u = manager_->store().indexOfPath(file);
    return u < 0 ? -1 : manager_->numFactory() + u;
  }

  void refresh() {
    if (!manager_) {
      name_.setText("No presets");
      name_.setEnabled(false);
      previous_.setEnabled(false);
      next_.setEnabled(false);
      save_.setEnabled(false);
      dirty_ = false;
      return;
    }
    seenRevision_ = manager_->revision();
    dirty_ = manager_->isDirty();
    const std::string name = manager_->hasSelection() ? manager_->currentName() : std::string("No preset");
    name_.setText(dirty_ ? name + " *" : name);
    name_.setSpokenValue(dirty_ ? name + ", modified" : name);
    name_.setEnabled(true);
    const bool any = manager_->count() > 0;
    previous_.setEnabled(any);
    next_.setEnabled(any);
    save_.setEnabled(true);
  }

  std::string choose(bool forSaving, const std::string& suggested) {
    if (chooseFile) return chooseFile(forSaving, suggested);
    const std::vector<FileFilter> filters = {
        {"Presets", std::string("*") + userPresetExtension() + ";*.vstpreset"},
        {"All files", "*.*"}};
    const std::string folder = manager_ ? manager_->store().folder() : std::string();
    if (forSaving)
      return FileDialog::saveFile("Export preset", {{"Presets", std::string("*") + userPresetExtension()},
                                                     {"VST3 preset", "*.vstpreset"}},
                                  suggested, folder);
    return FileDialog::openFile("Import preset", filters, folder);
  }

  void finishSaveAs(const std::string& name, UserPresetStore::IfExists ifExists) {
    std::string why;
    if (!manager_->saveAs(name, ifExists, &why)) showError("\"" + name + "\" could not be saved", why);
    refresh();
  }

  /** The prompt, in the editor's root. */
  void showPrompt(const std::string& title, const std::string& text, const std::string& okLabel,
                  std::function<bool(const std::string&)> accept,
                  std::function<void(bool, const std::string&)> after) {
    attach(&prompt_);
    prompt_.show(title, text, okLabel, std::move(accept), std::move(after));
  }

  /** Two overlays, used in turn. A question asked from the answer to
   *  another -- "could not move it to the trash: delete it for good?" --
   *  is asked in the OTHER one: showing it in the same overlay would destroy
   *  the button whose click is still running. */
  void showAlert(const std::string& title, const std::string& message, std::vector<std::string> buttons,
                 std::function<void(int)> onChoice) {
    current_ ^= 1;
    AlertOverlay& alert = alerts_[current_];
    attach(&alert);
    alert.show(title, message, std::move(buttons), std::move(onChoice));
  }

  void showError(const std::string& title, const std::string& why) {
    lastError_ = why.empty() ? title : title + ": " + why;
    showAlert(title, why, {"OK"}, [](int) {});
  }

  /** Overlays live in the editor's ROOT, so they cover all of it. */
  void attach(Component* overlay) {
    Component* host = root();
    if (overlay->parent() != host) host->addChild(overlay);
    else overlay->toFront();
    if (Widget* w = overlay->asWidget()) {
      w->setFont(font());
      w->setLookAndFeel(&lookAndFeel());
    }
  }

  PresetManager* manager_ = nullptr;
  Button previous_{"<"}, next_{">"}, save_{"Save"};
  detail::PresetNameField name_;
  PopupMenuOverlay menu_;
  AlertOverlay alerts_[2];
  int current_ = 0;
  detail::PresetNamePrompt prompt_;
  std::string lastError_;
  uint64_t seenRevision_ = ~(uint64_t) 0;
  uint64_t ticks_ = 0;
  bool dirty_ = false;
};

} // namespace gfx
} // namespace sonore
