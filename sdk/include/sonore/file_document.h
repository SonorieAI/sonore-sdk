// SPDX-License-Identifier: Apache-2.0
//
// A document that lives in a file: open, save, save as, and the questions
// that go around them.
//
// ── What it is for ──────────────────────────────────────────────────────────
//
// Every application that edits a file has the same dozen paths, and the bugs
// are in the paths nobody tries: closing a changed document and pressing
// Cancel; Save on a document that was never saved; Save As over a file that
// exists; a save that fails half way and must leave the document pointing at
// the file it CAME from, still marked changed. This class owns those paths.
// A subclass supplies four things -- the title, how to load, how to save,
// and (optionally) where the last document was -- exactly as the reference
// asks.
//
// A PLUGIN does not use this: its state is saved into the host's session and
// its dirty flag reported to the host (clap_host_state::mark_dirty), which is
// the format's job. This is for the standalone and for applications.
//
// ── The questions are hooks ────────────────────────────────────────────────
//
// "Save changes?", "Overwrite?", the open and save choosers and the failure
// message are virtual functions that answer through a callback. The defaults
// are the native ones -- a message box (MessageBoxW, NSAlert, a GTK dialog),
// and FileDialog for the choosers -- which answer before they return. An
// application whose questions are drawn inside its own window overrides
// them; gfx/alert.h's AlertOverlay is exactly that shape:
//
//   void askToSaveChanges(std::function<void(SaveChoice)> reply) override {
//     alert.show("Closing document...", "Save the changes to \"" +
//                getDocumentTitle() + "\"?", {"Save", "Discard", "Cancel"},
//                [reply](int i) { reply(i == 0 ? SaveChoice::save
//                                     : i == 1 ? SaveChoice::discard
//                                              : SaveChoice::cancel); });
//   }
//
// Every operation has an Async form that finishes when the hooks have
// answered, however late. The plain forms return the result, and are for
// hooks that answer at once: if a hook has NOT answered by the time it
// returns, the plain form reports userCancelledSave (or a failed Result) and
// the late answer is then ignored -- a save the caller was told did not
// happen must not happen behind its back. A document deleted before a hook
// answers is never called back either.
//
// ── Recent files ───────────────────────────────────────────────────────────
//
// setLastDocumentOpened() is called after every successful load and save,
// and getLastDocumentOpened() is where the choosers start. By default they
// keep the last path and, if one is attached, feed a RecentlyOpenedFilesList
// (recent_files.h) -- which is the hook an "Open Recent" menu needs.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "broadcaster.h"
#include "file_dialog.h"
#include "recent_files.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#if defined(_MSC_VER)
#pragma comment(lib, "user32.lib")
#endif
#elif defined(__APPLE__)
#include <objc/message.h>
#include <objc/runtime.h>
#elif defined(__linux__)
#include <dlfcn.h>
#endif

namespace sonore {
namespace detail {
namespace docprompt {

enum class Kind { information, warning, question };

/**
 * A native message box with one to three buttons; returns the index of the
 * button pressed, or -1 if nothing could be shown (no GTK, no AppKit) --
 * which every caller treats as the cautious answer.
 *
 * Windows' MessageBox cannot relabel its buttons, so there the labels are
 * Yes / No / Cancel (or OK) and the message is phrased to suit.
 */
inline int ask(Kind kind, const std::string& title, const std::string& message,
               const std::vector<std::string>& labels) {
  if (labels.empty() || labels.size() > 3) return -1;
#if defined(_WIN32)
  UINT type = labels.size() == 1 ? MB_OK : (labels.size() == 2 ? MB_YESNO : MB_YESNOCANCEL);
  type |= kind == Kind::warning ? MB_ICONWARNING
                                : (kind == Kind::question ? MB_ICONQUESTION : MB_ICONINFORMATION);
  // Owned by whatever window of this thread is active, so it is modal to the
  // application rather than floating free behind it.
  const int answer = MessageBoxW(GetActiveWindow(), pathio::widen(message.c_str()).c_str(),
                                 pathio::widen(title.c_str()).c_str(), type | MB_SETFOREGROUND);
  switch (answer) {
    case IDOK:
    case IDYES: return 0;
    case IDNO: return 1;
    case IDCANCEL: return labels.size() == 3 ? 2 : (labels.size() == 1 ? 0 : 1);
    default: return -1;
  }
#elif defined(__APPLE__)
  using detail::recent::objcCall;
  detail::recent::AutoreleasePool pool;
  Class alertClass = objc_getClass("NSAlert");
  Class stringClass = objc_getClass("NSString");
  if (!alertClass || !stringClass) return -1;
  auto ns = [stringClass](const std::string& s) {
    return objcCall<id>((id) stringClass, "stringWithUTF8String:", s.c_str());
  };
  id alert = objcCall<id>(objcCall<id>((id) alertClass, "alloc"), "init");
  if (!alert) return -1;
  // NSAlertStyleWarning 0, Informational 1, Critical 2.
  objcCall<void>(alert, "setAlertStyle:", (unsigned long) (kind == Kind::information ? 1 : 0));
  objcCall<void>(alert, "setMessageText:", ns(title));
  objcCall<void>(alert, "setInformativeText:", ns(message));
  for (const std::string& label : labels) objcCall<id>(alert, "addButtonWithTitle:", ns(label));
  // NSAlertFirstButtonReturn is 1000, and the rest follow it.
  const long response = objcCall<long>(alert, "runModal");
  objcCall<void>(alert, "release");
  const long index = response - 1000;
  return index >= 0 && index < (long) labels.size() ? (int) index : -1;
#elif defined(__linux__)
  // GTK at run time, as file_dialog.h loads it: a hard link would stop the
  // application starting at all on a machine without GTK.
  void* gtk = dlopen("libgtk-3.so.0", RTLD_LAZY | RTLD_GLOBAL);
  if (!gtk) return -1;
  using InitCheckFn = int (*)(int*, char***);
  using MessageNewFn = void* (*) (void*, int, int, int, const char*, ...);
  using AddButtonFn = void* (*) (void*, const char*, int);
  using SetTitleFn = void (*)(void*, const char*);
  using RunFn = int (*)(void*);
  using DestroyFn = void (*)(void*);
  using PendingFn = int (*)();
  using IterateFn = int (*)(int);
  auto initCheck = (InitCheckFn) dlsym(gtk, "gtk_init_check");
  auto messageNew = (MessageNewFn) dlsym(gtk, "gtk_message_dialog_new");
  auto addButton = (AddButtonFn) dlsym(gtk, "gtk_dialog_add_button");
  auto setTitle = (SetTitleFn) dlsym(gtk, "gtk_window_set_title");
  auto run = (RunFn) dlsym(gtk, "gtk_dialog_run");
  auto destroy = (DestroyFn) dlsym(gtk, "gtk_widget_destroy");
  auto pending = (PendingFn) dlsym(gtk, "gtk_events_pending");
  auto iterate = (IterateFn) dlsym(gtk, "gtk_main_iteration_do");
  if (!messageNew || !addButton || !run || !destroy) return -1;
  // Harmless when GTK is already running; without it, the first dialog of a
  // program that never touched GTK crashes inside gtk_message_dialog_new.
  if (initCheck && !initCheck(nullptr, nullptr)) return -1;
  // GTK_DIALOG_MODAL = 1; GTK_MESSAGE_INFO 0, WARNING 1, QUESTION 2;
  // GTK_BUTTONS_NONE = 0, so the buttons are ours, with our labels.
  const int messageType = kind == Kind::information ? 0 : (kind == Kind::warning ? 1 : 2);
  void* dialog = messageNew(nullptr, 1, messageType, 0, "%s", message.c_str());
  if (!dialog) return -1;
  if (setTitle) setTitle(dialog, title.c_str());
  for (size_t i = 0; i < labels.size(); ++i) addButton(dialog, labels[i].c_str(), (int) i + 1);
  const int response = run(dialog);
  destroy(dialog);
  if (pending && iterate)
    for (int i = 0; i < 32 && pending(); ++i) iterate(0);
  return response >= 1 && response <= (int) labels.size() ? response - 1 : -1;
#else
  (void) kind;
  (void) title;
  (void) message;
  return -1;
#endif
}

inline bool hasExtension(const std::string& path) {
  const size_t slash = path.find_last_of("/\\");
  const size_t dot = path.find_last_of('.');
  return dot != std::string::npos && (slash == std::string::npos || dot > slash + 1);
}

inline std::string withExtension(const std::string& path, const std::string& extension) {
  if (extension.empty() || hasExtension(path)) return path;
  return path + (extension[0] == '.' ? extension : "." + extension);
}

inline std::string parentOf(const std::string& path) {
  const size_t cut = path.find_last_of("/\\");
  return cut == std::string::npos ? std::string() : path.substr(0, cut);
}

/** A title made into a file name: characters no filesystem takes become '_'. */
inline std::string legalFileName(const std::string& title) {
  std::string out;
  for (char c : title) {
    const unsigned char u = (unsigned char) c;
    const bool bad = u < 0x20 || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
                     c == '"' || c == '<' || c == '>' || c == '|';
    out.push_back(bad ? '_' : c);
  }
  while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
  return out;
}

/** "Song.mix", or if that exists "Song (2).mix", "Song (3).mix"... */
inline std::string nonexistentSibling(const std::string& path) {
  if (!detail::recent::pathExists(path)) return path;
  const size_t slash = path.find_last_of("/\\");
  const size_t dot = path.find_last_of('.');
  const bool hasDot = dot != std::string::npos && (slash == std::string::npos || dot > slash + 1);
  const std::string stem = hasDot ? path.substr(0, dot) : path;
  const std::string ext = hasDot ? path.substr(dot) : std::string();
  for (int n = 2; n < 10000; ++n) {
    const std::string candidate = stem + " (" + std::to_string(n) + ")" + ext;
    if (!detail::recent::pathExists(candidate)) return candidate;
  }
  return path;
}

} // namespace docprompt
} // namespace detail

/**
 * A document that lives in one file, with every open, save and save-as path
 * and the questions around them. Mirrors the reference's class of the same
 * name; the file comment covers the hooks, the Async forms and recent files.
 *
 * A subclass implements getDocumentTitle, loadDocument and saveDocument, and
 * may override the question hooks and the last-document pair; it calls
 * changed() on every edit. Changes reach listeners through the
 * ChangeBroadcaster base, which its owner dispatches. MAIN THREAD: the
 * default hooks are native modal dialogs. For the standalone and for
 * applications, never a plugin, whose state belongs to the host.
 */
class FileBasedDocument : public ChangeBroadcaster {
public:
  /** The reference's three outcomes of a save, in its order. */
  enum SaveResult {
    savedOk = 0,        ///< saved -- or nothing needed saving, or the user chose to discard
    userCancelledSave,  ///< a question or a chooser was cancelled; nothing was written
    failedToWriteToFile ///< saveDocument() failed, or there was no file and asking was not allowed
  };

  /** The answer to "save changes?". */
  enum class SaveChoice {
    save,    ///< save first, asking for a file if the document has none
    discard, ///< go ahead without saving
    cancel   ///< neither: the operation ends as userCancelledSave
  };

  /** Success, or failure with a message for the user. */
  class Result {
  public:
    /** Success, with no message. */
    static Result ok() { return Result(std::string(), true); }
    /** Failure, with what to tell the user. An empty message becomes
     *  "Unknown error", so there is always something to show. */
    static Result fail(std::string message) {
      return Result(message.empty() ? std::string("Unknown error") : std::move(message), false);
    }
    /** True for ok(). */
    bool wasOk() const { return ok_; }
    /** True for fail(). */
    bool failed() const { return !ok_; }
    /** The message given to fail(); "" for ok(). */
    const std::string& getErrorMessage() const { return error_; }

  private:
    Result(std::string error, bool ok) : error_(std::move(error)), ok_(ok) {}
    std::string error_;
    bool ok_ = true;
  };

  /**
   * `fileExtension` (".song") is added to a saved name that has none;
   * `fileWildcard` ("*.song") filters the choosers; the titles head them.
   */
  FileBasedDocument(std::string fileExtension, std::string fileWildcard,
                    std::string openFileDialogTitle, std::string saveFileDialogTitle)
      : fileExtension_(std::move(fileExtension)),
        fileWildcard_(std::move(fileWildcard)),
        openTitle_(std::move(openFileDialogTitle)),
        saveTitle_(std::move(saveFileDialogTitle)),
        alive_(std::make_shared<bool>(true)) {}

  /** An Async operation still waiting for a hook's answer is dropped: its
   *  callback is never called, and a late answer changes nothing. */
  ~FileBasedDocument() override = default;

  ///@{
  /** Not copyable: answers to an operation in progress are tied to this
   *  object. */
  FileBasedDocument(const FileBasedDocument&) = delete;
  FileBasedDocument& operator=(const FileBasedDocument&) = delete;
  ///@}

  // ── State ─────────────────────────────────────────────────────────────────

  /** True after changed() or setChangedFlag(true), until a load or a save
   *  succeeds. */
  bool hasChangedSinceSaved() const { return changedSinceSave_; }

  /** Mark the document changed: call it on every edit. Tells listeners. */
  virtual void changed() {
    changedSinceSave_ = true;
    sendChangeMessage();
  }

  /** Sets or clears the flag by hand. Listeners are told only when it
   *  actually changes. */
  void setChangedFlag(bool hasChanged) {
    if (changedSinceSave_ == hasChanged) return;
    changedSinceSave_ = hasChanged;
    sendChangeMessage();
  }

  /** The file this document is, or empty for one never saved. */
  const std::string& getFile() const { return documentFile_; }

  /** Point the document at another file WITHOUT loading or saving it -- a
   *  "this is now called" -- which counts as a change. */
  void setFile(const std::string& newFile) {
    if (documentFile_ == newFile) return;
    documentFile_ = newFile;
    changed();
  }

  /** The extension given to the constructor, ".song". */
  const std::string& getFileExtension() const { return fileExtension_; }
  /** The wildcard given to the constructor, "*.song"; empty means the
   *  choosers show every file. */
  const std::string& getFileWildcard() const { return fileWildcard_; }

  /** A list the default recent-files hook feeds; not owned. `registerNatively`
   *  also tells the OS's own list (recent_files.h). */
  void setRecentFilesList(RecentlyOpenedFilesList* list, bool registerNatively = false) {
    recentFiles_ = list;
    registerNatively_ = registerNatively;
  }

  // ── Plain forms (see the header for when they are right) ──────────────────

  /** Load a file. On failure the document keeps the file it had and, if
   *  asked, the user is told why. */
  Result loadFrom(const std::string& file, bool showMessageOnFailure) {
    return runSync<Result>(Result::fail("Not finished"), [&](std::shared_ptr<Flow> flow,
                                                             std::function<void(Result)> done) {
      loadFromImpl(flow, file, showMessageOnFailure, std::move(done));
    });
  }

  /** Ask for a file, then load it. */
  Result loadFromUserSpecifiedFile(bool showMessageOnFailure) {
    return runSync<Result>(Result::fail("User cancelled"), [&](std::shared_ptr<Flow> flow,
                                                               std::function<void(Result)> done) {
      loadFromUserSpecifiedFileImpl(flow, showMessageOnFailure, std::move(done));
    });
  }

  /** Save to the document's file; with no file, ask for one if allowed. */
  SaveResult save(bool askUserForFileIfNotSpecified, bool showMessageOnFailure) {
    return runSync<SaveResult>(userCancelledSave, [&](std::shared_ptr<Flow> flow,
                                                      std::function<void(SaveResult)> done) {
      saveAsImpl(flow, documentFile_, false, askUserForFileIfNotSpecified, showMessageOnFailure,
                 std::move(done));
    });
  }

  /** The close-a-window question: unchanged is savedOk at once; otherwise
   *  ask, and save, discard (savedOk) or cancel as answered. */
  SaveResult saveIfNeededAndUserAgrees() {
    return runSync<SaveResult>(userCancelledSave, [&](std::shared_ptr<Flow> flow,
                                                      std::function<void(SaveResult)> done) {
      saveIfNeededImpl(flow, std::move(done));
    });
  }

  /** Save to `newFile` and make it the document's file. */
  SaveResult saveAs(const std::string& newFile, bool warnAboutOverwritingExistingFiles,
                    bool askUserForFileIfNotSpecified, bool showMessageOnFailure) {
    return runSync<SaveResult>(userCancelledSave, [&](std::shared_ptr<Flow> flow,
                                                      std::function<void(SaveResult)> done) {
      saveAsImpl(flow, newFile, warnAboutOverwritingExistingFiles, askUserForFileIfNotSpecified,
                 showMessageOnFailure, std::move(done));
    });
  }

  /** Ask where, then save there. */
  SaveResult saveAsInteractive(bool warnAboutOverwritingExistingFiles) {
    return runSync<SaveResult>(userCancelledSave, [&](std::shared_ptr<Flow> flow,
                                                      std::function<void(SaveResult)> done) {
      saveAsInteractiveImpl(flow, warnAboutOverwritingExistingFiles, std::move(done));
    });
  }

  // ── Async forms: the callback runs when the hooks have answered ───────────

  ///@{
  /** The plain form of the same name, finishing in `callback` instead of
   *  returning. It runs once the hooks have answered: before this returns
   *  with the default, modal hooks; later with hooks that answer later. It
   *  is never called if the document is destroyed first. Main thread. */
  void loadFromAsync(const std::string& file, bool showMessageOnFailure,
                     std::function<void(Result)> callback) {
    loadFromImpl(newFlow(), file, showMessageOnFailure, std::move(callback));
  }

  void loadFromUserSpecifiedFileAsync(bool showMessageOnFailure,
                                      std::function<void(Result)> callback) {
    loadFromUserSpecifiedFileImpl(newFlow(), showMessageOnFailure, std::move(callback));
  }

  void saveAsync(bool askUserForFileIfNotSpecified, bool showMessageOnFailure,
                 std::function<void(SaveResult)> callback) {
    saveAsImpl(newFlow(), documentFile_, false, askUserForFileIfNotSpecified, showMessageOnFailure,
               std::move(callback));
  }

  void saveIfNeededAndUserAgreesAsync(std::function<void(SaveResult)> callback) {
    saveIfNeededImpl(newFlow(), std::move(callback));
  }

  void saveAsAsync(const std::string& newFile, bool warnAboutOverwritingExistingFiles,
                   bool askUserForFileIfNotSpecified, bool showMessageOnFailure,
                   std::function<void(SaveResult)> callback) {
    saveAsImpl(newFlow(), newFile, warnAboutOverwritingExistingFiles, askUserForFileIfNotSpecified,
               showMessageOnFailure, std::move(callback));
  }

  void saveAsInteractiveAsync(bool warnAboutOverwritingExistingFiles,
                              std::function<void(SaveResult)> callback) {
    saveAsInteractiveImpl(newFlow(), warnAboutOverwritingExistingFiles, std::move(callback));
  }
  ///@}

protected:
  // ── What a subclass supplies ──────────────────────────────────────────────

  /** What the user calls this document: "Untitled", or the file's name. */
  virtual std::string getDocumentTitle() = 0;

  /** Read `file` into the document. */
  virtual Result loadDocument(const std::string& file) = 0;

  /** Write the document to `file`. */
  virtual Result saveDocument(const std::string& file) = 0;

  /** Where the choosers start. Default: the last file loaded or saved, else
   *  the top of the attached recent list. */
  virtual std::string getLastDocumentOpened() {
    if (!lastOpened_.empty()) return lastOpened_;
    if (recentFiles_ && recentFiles_->getNumFiles() > 0) return recentFiles_->getFile(0);
    return std::string();
  }

  /** Called after every successful load and save. Default: remember it, and
   *  put it at the top of the attached recent list. */
  virtual void setLastDocumentOpened(const std::string& file) {
    lastOpened_ = file;
    if (recentFiles_) recentFiles_->addFile(file);
    if (registerNatively_) RecentlyOpenedFilesList::registerRecentFileNatively(file);
  }

  /** The name a Save As offers. Default: with the extension, and numbered
   *  if that file already exists. */
  virtual std::string getSuggestedSaveAsFile(const std::string& defaultFile) {
    return detail::docprompt::nonexistentSibling(
        detail::docprompt::withExtension(defaultFile, fileExtension_));
  }

  // ── The questions (hooks) ─────────────────────────────────────────────────

  /** "Do you want to save the changes?" Default: a native box. Cancel is the
   *  answer when there is no way to ask -- nothing is lost by it. */
  virtual void askToSaveChanges(std::function<void(SaveChoice)> reply) {
    const int answer = detail::docprompt::ask(
        detail::docprompt::Kind::question, "Closing document...",
        "Do you want to save the changes to \"" + getDocumentTitle() + "\"?",
        {"Save", "Discard changes", "Cancel"});
    reply(answer == 0 ? SaveChoice::save : (answer == 1 ? SaveChoice::discard : SaveChoice::cancel));
  }

  /** "Replace the existing file?" Default: a native box; no means no. */
  virtual void askToOverwrite(const std::string& file, std::function<void(bool)> reply) {
    const int answer = detail::docprompt::ask(
        detail::docprompt::Kind::warning, "File already exists",
        "There's already a file called:\n\n" + file + "\n\nAre you sure you want to overwrite it?",
        {"Overwrite", "Cancel"});
    reply(answer == 0);
  }

  /** Pick a file to open; empty means cancelled. Default: FileDialog. */
  virtual void chooseFileToOpen(const std::string& startingPath,
                                std::function<void(std::string)> reply) {
    reply(FileDialog::openFile(openTitle_, filters(), startingPath));
  }

  /** Pick where to save; empty means cancelled. Default: FileDialog, which
   *  adds the extension and asks before replacing a file itself. */
  virtual void chooseFileToSave(const std::string& suggestedFile,
                                std::function<void(std::string)> reply) {
    reply(FileDialog::saveFile(saveTitle_, filters(), detail::recent::leafName(suggestedFile),
                               detail::docprompt::parentOf(suggestedFile)));
  }

  /** Tell the user something failed. Default: a native box. */
  virtual void showFailure(const std::string& title, const std::string& message) {
    detail::docprompt::ask(detail::docprompt::Kind::warning, title, message, {"OK"});
  }

private:
  /** One operation's thread through the hooks: abandoned when a plain form
   *  gave up waiting, and dead when the document is gone. */
  struct Flow {
    std::weak_ptr<bool> document;
    bool abandoned = false;
    bool live() const { return !abandoned && !document.expired(); }
  };

  std::shared_ptr<Flow> newFlow() const {
    auto flow = std::make_shared<Flow>();
    flow->document = alive_;
    return flow;
  }

  template <typename T, typename Start>
  T runSync(T ifUnanswered, Start start) {
    struct Box {
      bool done = false;
      T value;
      explicit Box(T v) : value(std::move(v)) {}
    };
    auto box = std::make_shared<Box>(ifUnanswered);
    auto flow = newFlow();
    start(flow, [box](T value) {
      box->value = std::move(value);
      box->done = true;
    });
    if (!box->done) flow->abandoned = true; // a late answer must change nothing
    return box->done ? box->value : ifUnanswered;
  }

  std::vector<FileFilter> filters() const {
    if (fileWildcard_.empty()) return {};
    return {FileFilter{fileWildcard_, fileWildcard_}};
  }

  std::string quoted(const std::string& s) const { return "\"" + s + "\""; }

  void loadFromImpl(std::shared_ptr<Flow> flow, const std::string& file,
                    bool showMessageOnFailure, std::function<void(Result)> done) {
    if (!flow->live()) return;
    const std::string oldFile = documentFile_;
    documentFile_ = file;
    Result result = Result::fail("The file doesn't exist");
    if (!file.empty() && detail::recent::pathExists(file) && !isDirectoryPath(file)) {
      result = loadDocument(file);
      if (result.wasOk()) {
        setChangedFlag(false);
        setLastDocumentOpened(file);
        if (done) done(result);
        return;
      }
    }
    // Back to the file it had: a failed load must not leave the document
    // claiming to be the file it could not read.
    documentFile_ = oldFile;
    if (showMessageOnFailure)
      showFailure("Failed to open file...", "There was an error while trying to load the file:\n" +
                                                file + "\n\n" + result.getErrorMessage());
    if (done) done(result);
  }

  void loadFromUserSpecifiedFileImpl(std::shared_ptr<Flow> flow, bool showMessageOnFailure,
                                     std::function<void(Result)> done) {
    if (!flow->live()) return;
    chooseFileToOpen(getLastDocumentOpened(),
                     [this, flow, showMessageOnFailure, done](std::string chosen) {
                       if (!flow->live()) return;
                       if (chosen.empty()) {
                         if (done) done(Result::fail("User cancelled"));
                         return;
                       }
                       loadFromImpl(flow, chosen, showMessageOnFailure, done);
                     });
  }

  void saveIfNeededImpl(std::shared_ptr<Flow> flow, std::function<void(SaveResult)> done) {
    if (!flow->live()) return;
    if (!hasChangedSinceSaved()) {
      if (done) done(savedOk);
      return;
    }
    askToSaveChanges([this, flow, done](SaveChoice choice) {
      if (!flow->live()) return;
      if (choice == SaveChoice::save) {
        saveAsImpl(flow, documentFile_, false, true, true, done);
      } else if (done) {
        done(choice == SaveChoice::discard ? savedOk : userCancelledSave);
      }
    });
  }

  void saveAsImpl(std::shared_ptr<Flow> flow, const std::string& newFile,
                  bool warnAboutOverwritingExistingFiles, bool askUserForFileIfNotSpecified,
                  bool showMessageOnFailure, std::function<void(SaveResult)> done) {
    if (!flow->live()) return;
    if (newFile.empty()) {
      if (askUserForFileIfNotSpecified) {
        saveAsInteractiveImpl(flow, true, std::move(done));
        return;
      }
      // Nowhere to save to, and not allowed to ask: a failure, not a cancel.
      if (done) done(failedToWriteToFile);
      return;
    }
    if (warnAboutOverwritingExistingFiles && detail::recent::pathExists(newFile)) {
      askToOverwrite(newFile, [this, flow, newFile, showMessageOnFailure, done](bool overwrite) {
        if (!flow->live()) return;
        if (!overwrite) {
          if (done) done(userCancelledSave);
          return;
        }
        writeTo(newFile, showMessageOnFailure, done);
      });
      return;
    }
    writeTo(newFile, showMessageOnFailure, std::move(done));
  }

  void writeTo(const std::string& newFile, bool showMessageOnFailure,
               std::function<void(SaveResult)> done) {
    const std::string oldFile = documentFile_;
    documentFile_ = newFile;
    const Result result = saveDocument(newFile);
    if (result.wasOk()) {
      setChangedFlag(false);
      setLastDocumentOpened(newFile);
      sendChangeMessage(); // the file changed even if the flag did not
      if (done) done(savedOk);
      return;
    }
    // A failed save leaves the document where it was: the old file, still
    // changed. Pointing it at the file that could not be written is how a
    // second Save silently goes to the wrong place.
    documentFile_ = oldFile;
    if (showMessageOnFailure)
      showFailure("Error writing to file...", "An error occurred while trying to save " +
                                                  quoted(getDocumentTitle()) + " to the file:\n" +
                                                  newFile + "\n\n" + result.getErrorMessage());
    sendChangeMessage();
    if (done) done(failedToWriteToFile);
  }

  void saveAsInteractiveImpl(std::shared_ptr<Flow> flow, bool warnAboutOverwritingExistingFiles,
                             std::function<void(SaveResult)> done) {
    if (!flow->live()) return;
    std::string base = documentFile_;
    if (base.empty()) {
      std::string title = detail::docprompt::legalFileName(getDocumentTitle());
      if (title.empty()) title = "unnamed";
      const std::string folder = detail::docprompt::parentOf(getLastDocumentOpened());
      base = folder.empty() ? title : folder + pathSeparatorChar() + title;
    }
    const std::string suggested = getSuggestedSaveAsFile(base);
    chooseFileToSave(suggested, [this, flow, warnAboutOverwritingExistingFiles,
                                 done](std::string chosen) {
      if (!flow->live()) return;
      if (chosen.empty()) {
        if (done) done(userCancelledSave);
        return;
      }
      // A chooser that did not add the extension: added here, and since the
      // chooser's own overwrite check was about the OTHER name, asked again
      // if the full one exists.
      const std::string full = detail::docprompt::withExtension(chosen, fileExtension_);
      const bool askAgain = warnAboutOverwritingExistingFiles && full != chosen;
      saveAsImpl(flow, full, askAgain, false, true, done);
    });
  }

  static bool isDirectoryPath(const std::string& path) {
    return pathio::isDirectoryAt(path.c_str());
  }

  static char pathSeparatorChar() {
#if defined(_WIN32)
    return '\\';
#else
    return '/';
#endif
  }

  std::string fileExtension_, fileWildcard_, openTitle_, saveTitle_;
  std::string documentFile_;
  std::string lastOpened_;
  bool changedSinceSave_ = false;
  RecentlyOpenedFilesList* recentFiles_ = nullptr;
  bool registerNatively_ = false;
  /** Expires with the document, so an answer arriving afterwards is dropped. */
  std::shared_ptr<bool> alive_;
};

} // namespace sonore
