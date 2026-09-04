// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: asking the user for a file.
//
// A sampler that cannot load a sample is not a sampler. A convolver needs an
// impulse response, a preset system needs somewhere to save to, and a plugin
// with a "Load…" button that does nothing is worse than one without the
// button. This is the piece of a file chooser a plugin actually uses.
//
// The page cannot do it. <input type="file"> is disabled by default in an
// embedded WebView2, behaves differently again under WebKitGTK, and gives a
// browser sandbox's idea of a file rather than a path a DSP can open. So the
// dialog is native, on every platform, and returns a real path.
//
// MAIN THREAD, and MODAL. The call does not return until the user is done,
// which blocks the host's UI the way every blocking file dialog does, and
// the same way every DAW expects. Never from the audio thread and never from
// a timer that the host is also waiting on.
//
// WHAT IS VERIFIED: the Windows path is exercised on this machine. The Cocoa
// and GTK paths are written against their documented APIs. The parts that
// decide anything (filters, extensions, defaults) are shared across all three
// and are tested.
#pragma once

#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shobjidl.h>

// FOS_PICKFOLDER lives behind an NTDDI version gate, and nothing in this SDK
// sets one -- so which Windows SDK headers a build happens to pick up decides
// whether the name exists. Its VALUE has been fixed since Vista, so defining
// it when it is absent costs nothing and beats making every consumer set a
// version macro to get a folder picker.
#ifndef FOS_PICKFOLDER
#define FOS_PICKFOLDER 0x00000020
#endif
#ifndef FOS_FORCEFILESYSTEM
#define FOS_FORCEFILESYSTEM 0x00000040
#endif
#ifndef FOS_OVERWRITEPROMPT
#define FOS_OVERWRITEPROMPT 0x00000002
#endif
#ifndef FOS_FILEMUSTEXIST
#define FOS_FILEMUSTEXIST 0x00001000
#endif
#ifndef FOS_PATHMUSTEXIST
#define FOS_PATHMUSTEXIST 0x00000800
#endif
#elif defined(__APPLE__)
#include <objc/message.h>
#include <objc/runtime.h>
#elif defined(__linux__)
#include <dlfcn.h>
#endif

namespace sonore {

/** One line of a file filter: what to call it and what it matches. */
struct FileFilter {
  /** Shown to the user, e.g. "Audio files". */
  std::string label;
  /** Semicolon-separated patterns, e.g. "*.wav;*.aiff;*.flac". */
  std::string patterns;
};

/**
 * Split a pattern list into bare extensions, lower-cased and without dots.
 *
 * "*.wav;*.AIFF; *.flac" becomes {"wav", "aiff", "flac"}. Every platform
 * wants this in a different shape -- Windows takes the patterns verbatim,
 * Cocoa wants bare extensions, GTK wants patterns again -- so the parsing
 * happens once here rather than three times badly.
 *
 * "*" and "*.*" yield nothing, which is the honest answer: "every file" is
 * not an extension list, and a caller that turned it into one would end up
 * appending ".*" to a saved filename.
 */
inline std::vector<std::string> filterExtensions(const std::string& patterns) {
  std::vector<std::string> out;
  std::string current;
  auto flush = [&out, &current]() {
    // Strip whatever leads up to the extension: "*.wav", ".wav" and "wav" are
    // all the same request written three ways.
    const size_t dot = current.find_last_of('.');
    std::string ext = dot == std::string::npos ? current : current.substr(dot + 1);
    std::string trimmed;
    for (char c : ext) {
      if (c == ' ' || c == '\t' || c == '*') continue;
      trimmed.push_back((char) (c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c));
    }
    if (!trimmed.empty()) out.push_back(trimmed);
    current.clear();
  };
  for (char c : patterns) {
    if (c == ';' || c == ',') flush();
    else current.push_back(c);
  }
  flush();
  return out;
}

/**
 * Give a filename the extension it should have, if it has none.
 *
 * A save dialog returns exactly what the user typed. Somebody who types
 * "my kit" and presses Save means "my kit.wav", and a plugin that writes the
 * extensionless file has produced something their file manager cannot open
 * and they cannot find again.
 *
 * A name that ALREADY ends in one of the accepted extensions is left alone,
 * including when it is not the first one -- saving "kick.aiff" through a
 * dialog whose first filter is .wav must not produce "kick.aiff.wav".
 */
inline std::string ensureExtension(const std::string& path,
                                   const std::vector<std::string>& extensions) {
  if (path.empty() || extensions.empty()) return path;
  const size_t slash = path.find_last_of("/\\");
  const size_t dot = path.find_last_of('.');
  const bool hasExtension = dot != std::string::npos &&
                            (slash == std::string::npos || dot > slash + 1);
  if (hasExtension) {
    std::string have = path.substr(dot + 1);
    for (char& c : have)
      if (c >= 'A' && c <= 'Z') c = (char) (c - 'A' + 'a');
    for (const std::string& e : extensions)
      if (have == e) return path;
    // An extension that is not one of ours is still an extension the user
    // typed on purpose. Appending would give them "kick.aif.wav"; replacing
    // would throw away what they asked for. Left exactly as typed.
    return path;
  }
  return path + "." + extensions.front();
}

#if defined(_WIN32)
namespace detail {

/** UTF-8 to UTF-16 for the paths and labels Windows takes. */
inline std::wstring widen(const std::string& s) {
  if (s.empty()) return std::wstring();
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int) s.size(), nullptr, 0);
  std::wstring out((size_t) (n > 0 ? n : 0), L'\0');
  if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int) s.size(), &out[0], n);
  return out;
}

inline std::string narrow(const wchar_t* s) {
  if (!s || !*s) return std::string();
  const int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
  if (n <= 1) return std::string();
  std::string out((size_t) (n - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, s, -1, &out[0], n, nullptr, nullptr);
  return out;
}

/** COM, initialised only if it is not already. A host has almost always done
 *  it on this thread, and calling CoUninitialize on a thread we did not
 *  initialise tears down the host's apartment. */
struct ComScope {
  bool owned = false;
  ComScope() {
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    // S_FALSE means already initialised on this thread with the same model --
    // still a successful call that must be balanced. RPC_E_CHANGED_MODE means
    // the host chose a different model, which is fine to work inside and must
    // NOT be balanced.
    owned = (hr == S_OK || hr == S_FALSE);
  }
  ~ComScope() {
    if (owned) CoUninitialize();
  }
};

enum class Mode { Open, Save, Folder };

inline std::string runDialog(Mode mode, const std::string& title,
                             const std::vector<FileFilter>& filters,
                             const std::string& startingPath,
                             const std::string& suggestedName, void* parentWindow) {
  ComScope com;
  IFileDialog* dialog = nullptr;
  const CLSID clsid = (mode == Mode::Save) ? CLSID_FileSaveDialog : CLSID_FileOpenDialog;
  const IID iid = (mode == Mode::Save) ? IID_IFileSaveDialog : IID_IFileOpenDialog;
  if (FAILED(CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, iid, (void**) &dialog)) ||
      !dialog)
    return std::string();

  DWORD options = 0;
  dialog->GetOptions(&options);
  options |= FOS_FORCEFILESYSTEM; // no virtual items a DSP cannot open
  if (mode == Mode::Folder) options |= FOS_PICKFOLDER;
  if (mode == Mode::Open) options |= FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST;
  if (mode == Mode::Save) options |= FOS_OVERWRITEPROMPT;
  dialog->SetOptions(options);

  if (!title.empty()) dialog->SetTitle(widen(title).c_str());

  // The filter strings must outlive the call: COMDLG_FILTERSPEC holds bare
  // pointers into them.
  std::vector<std::wstring> storage;
  std::vector<COMDLG_FILTERSPEC> specs;
  if (mode != Mode::Folder && !filters.empty()) {
    storage.reserve(filters.size() * 2);
    for (const FileFilter& f : filters) {
      storage.push_back(widen(f.label));
      storage.push_back(widen(f.patterns.empty() ? std::string("*.*") : f.patterns));
    }
    for (size_t i = 0; i < filters.size(); ++i)
      specs.push_back({storage[i * 2].c_str(), storage[i * 2 + 1].c_str()});
    dialog->SetFileTypes((UINT) specs.size(), specs.data());
    dialog->SetFileTypeIndex(1); // one-based, and zero is an error rather than a default
  }

  if (!suggestedName.empty()) dialog->SetFileName(widen(suggestedName).c_str());

  if (!startingPath.empty()) {
    IShellItem* folder = nullptr;
    if (SUCCEEDED(SHCreateItemFromParsingName(widen(startingPath).c_str(), nullptr,
                                              IID_IShellItem, (void**) &folder)) &&
        folder) {
      // SetFolder, not SetDefaultFolder: the caller is saying where to open
      // THIS time, which should win over wherever the user was last.
      dialog->SetFolder(folder);
      folder->Release();
    }
  }

  const HRESULT shown = dialog->Show((HWND) parentWindow);
  std::string result;
  if (SUCCEEDED(shown)) {
    IShellItem* item = nullptr;
    if (SUCCEEDED(dialog->GetResult(&item)) && item) {
      PWSTR path = nullptr;
      if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
        result = narrow(path);
        CoTaskMemFree(path);
      }
      item->Release();
    }
  }
  dialog->Release();
  return result;
}

} // namespace detail
#endif // _WIN32

#if defined(__linux__)
namespace detail {

enum class Mode { Open, Save, Folder };

/**
 * GTK, loaded at run time exactly as the webview backend loads it.
 *
 * Linking it would make every LV2 bundle depend on GTK whether or not the
 * plugin ever opens a dialog, and a host without it would then fail to load
 * the plugin at all rather than failing to open a file browser.
 */
inline std::string runDialog(Mode mode, const std::string& title,
                             const std::vector<FileFilter>& filters,
                             const std::string& startingPath,
                             const std::string& suggestedName, void* /*parentWindow*/) {
  void* gtk = dlopen("libgtk-3.so.0", RTLD_LAZY | RTLD_GLOBAL);
  if (!gtk) return std::string();
  auto sym = [gtk](const char* name) { return dlsym(gtk, name); };

  using NewFn = void* (*) (const char*, void*, int, const char*, int, ...);
  using RunFn = int (*)(void*);
  using DestroyFn = void (*)(void*);
  using FilenameFn = char* (*) (void*);
  using SetFolderFn = int (*)(void*, const char*);
  using SetNameFn = void (*)(void*, const char*);
  using AddFilterFn = void (*)(void*, void*);
  using FilterNewFn = void* (*) ();
  using FilterNameFn = void (*)(void*, const char*);
  using FilterPatternFn = void (*)(void*, const char*);
  using PendingFn = int (*)();
  using IterateFn = void (*)(int);
  using FreeFn = void (*)(void*);

  auto dialogNew = (NewFn) sym("gtk_file_chooser_dialog_new");
  auto run = (RunFn) sym("gtk_dialog_run");
  auto destroy = (DestroyFn) sym("gtk_widget_destroy");
  auto filename = (FilenameFn) sym("gtk_file_chooser_get_filename");
  auto setFolder = (SetFolderFn) sym("gtk_file_chooser_set_current_folder");
  auto setName = (SetNameFn) sym("gtk_file_chooser_set_current_name");
  auto addFilter = (AddFilterFn) sym("gtk_file_chooser_add_filter");
  auto filterNew = (FilterNewFn) sym("gtk_file_filter_new");
  auto filterName = (FilterNameFn) sym("gtk_file_filter_set_name");
  auto filterPattern = (FilterPatternFn) sym("gtk_file_filter_add_pattern");
  auto pending = (PendingFn) sym("gtk_events_pending");
  auto iterate = (IterateFn) sym("gtk_main_iteration_do");
  if (!dialogNew || !run || !destroy || !filename) return std::string();

  // GTK_FILE_CHOOSER_ACTION_{OPEN,SAVE,SELECT_FOLDER} = 0, 1, 2.
  const int action = mode == Mode::Open ? 0 : (mode == Mode::Save ? 1 : 2);
  const char* accept = mode == Mode::Save ? "_Save" : "_Open";
  // GTK_RESPONSE_ACCEPT = -3, GTK_RESPONSE_CANCEL = -6.
  void* dialog = dialogNew(title.empty() ? "Choose a file" : title.c_str(), nullptr, action,
                           "_Cancel", -6, accept, -3, (void*) nullptr);
  if (!dialog) return std::string();

  if (!startingPath.empty() && setFolder) setFolder(dialog, startingPath.c_str());
  if (!suggestedName.empty() && setName && mode == Mode::Save)
    setName(dialog, suggestedName.c_str());
  if (mode != Mode::Folder && filterNew && filterName && filterPattern && addFilter) {
    for (const FileFilter& f : filters) {
      void* filter = filterNew();
      if (!filter) continue;
      filterName(filter, f.label.c_str());
      std::string one;
      for (size_t i = 0; i <= f.patterns.size(); ++i) {
        const bool end = i == f.patterns.size();
        if (!end && f.patterns[i] != ';' && f.patterns[i] != ',') {
          if (f.patterns[i] != ' ') one.push_back(f.patterns[i]);
          continue;
        }
        if (!one.empty()) filterPattern(filter, one.c_str());
        one.clear();
      }
      addFilter(dialog, filter);
    }
  }

  const int response = run(dialog);
  std::string result;
  if (response == -3) {
    if (char* chosen = filename(dialog)) {
      result = chosen;
      if (auto gfree = (FreeFn) dlsym(RTLD_DEFAULT, "g_free")) gfree(chosen);
    }
  }
  destroy(dialog);
  // The window is destroyed but its X resources are not gone until the loop
  // runs. Without this the dialog leaves a hole on screen until the next
  // editor tick, which reads as a crash.
  if (pending && iterate)
    for (int i = 0; i < 32 && pending(); ++i) iterate(0);
  return result;
}

} // namespace detail
#endif // __linux__

#if defined(__APPLE__)
namespace detail {
enum class Mode { Open, Save, Folder };

// NSOpenPanel / NSSavePanel through the Objective-C RUNTIME, so this stays a
// plain C++ header and a generated project needs no .mm file and no
// mixed-language build -- the same choice webview_cocoa.h makes.
//
// This used to be a DECLARATION here and a promise that the Cocoa backend
// defined it. It did not. The promise type-checked on every machine and only
// the LINKER could catch it, on the one platform that had never been linked:
// the first macOS CI build failed with an undefined symbol for all three
// entry points. It lives here now, beside the Windows and Linux bodies, so
// the header carries its own weight on every platform it claims.

/** objc_msgSend must be called through a correctly-typed function pointer:
 *  it is variadic in the header and NOT variadic in the ABI, and arm64 passes
 *  the two kinds differently. */
template <typename Ret, typename... Args>
inline Ret objcMsg(id target, SEL selector, Args... args) {
  using Fn = Ret (*)(id, SEL, Args...);
  return reinterpret_cast<Fn>(objc_msgSend)(target, selector, args...);
}
inline id objcCls(const char* name) { return (id) objc_getClass(name); }
inline id nsStr(const std::string& s) {
  return objcMsg<id>(objcCls("NSString"), sel_registerName("stringWithUTF8String:"), s.c_str());
}
inline std::string fromNsStr(id str) {
  if (!str) return std::string();
  const char* utf8 = objcMsg<const char*>(str, sel_registerName("UTF8String"));
  return utf8 ? std::string(utf8) : std::string();
}

inline std::string runDialog(Mode mode, const std::string& title,
                             const std::vector<FileFilter>& filters,
                             const std::string& startingPath,
                             const std::string& suggestedName, void* parentWindow) {
  (void) parentWindow; // modal, not sheet: see the note at the end.
  id pool = objcMsg<id>(objcMsg<id>(objcCls("NSAutoreleasePool"), sel_registerName("alloc")),
                        sel_registerName("init"));
  if (!pool) return std::string();

  const bool saving = mode == Mode::Save;
  id panel = saving
                 ? objcMsg<id>(objcCls("NSSavePanel"), sel_registerName("savePanel"))
                 : objcMsg<id>(objcCls("NSOpenPanel"), sel_registerName("openPanel"));
  if (!panel) {
    objcMsg<void>(pool, sel_registerName("drain"));
    return std::string();
  }

  if (!title.empty())
    objcMsg<void>(panel, sel_registerName("setMessage:"), nsStr(title));

  if (!saving) {
    const bool folder = mode == Mode::Folder;
    objcMsg<void>(panel, sel_registerName("setCanChooseFiles:"), (BOOL) !folder);
    objcMsg<void>(panel, sel_registerName("setCanChooseDirectories:"), (BOOL) folder);
    objcMsg<void>(panel, sel_registerName("setAllowsMultipleSelection:"), (BOOL) NO);
  } else if (!suggestedName.empty()) {
    objcMsg<void>(panel, sel_registerName("setNameFieldStringValue:"), nsStr(suggestedName));
  }

  if (!startingPath.empty()) {
    id url = objcMsg<id>(objcCls("NSURL"), sel_registerName("fileURLWithPath:"),
                         nsStr(startingPath));
    if (url) objcMsg<void>(panel, sel_registerName("setDirectoryURL:"), url);
  }

  // Extensions, not patterns: filterExtensions already produces exactly the
  // shape Cocoa wants. A folder picker takes none, and neither does an empty
  // filter list -- setting an empty array would hide every file.
  if (mode != Mode::Folder && !filters.empty()) {
    id types = objcMsg<id>(objcMsg<id>(objcCls("NSMutableArray"), sel_registerName("alloc")),
                           sel_registerName("init"));
    bool any = false;
    for (const FileFilter& filter : filters)
      for (const std::string& ext : filterExtensions(filter.patterns))
        if (ext != "*") {
          objcMsg<void>(types, sel_registerName("addObject:"), nsStr(ext));
          any = true;
        }
    if (any) objcMsg<void>(panel, sel_registerName("setAllowedFileTypes:"), types);
    objcMsg<void>(types, sel_registerName("release"));
  }

  // NSModalResponseOK is 1. runModal returns NSInteger, which is long here.
  const long response = objcMsg<long>(panel, sel_registerName("runModal"));
  std::string chosen;
  if (response == 1) {
    id url = objcMsg<id>(panel, sel_registerName("URL"));
    if (url) chosen = fromNsStr(objcMsg<id>(url, sel_registerName("path")));
  }
  objcMsg<void>(pool, sel_registerName("drain"));
  return chosen;
}

// parentWindow is accepted and ignored on purpose. A sheet
// (beginSheetModalForWindow:) is the nicer presentation, but it is
// ASYNCHRONOUS and this API returns the path, so honouring it would mean
// pumping a nested run loop inside somebody else DAW -- the class of trick
// that deadlocks hosts. A modal panel is what every plugin format's file
// browser does in practice.
} // namespace detail
#endif

/**
 * Ask the user for a file.
 *
 * Returns the chosen path, or an empty string if they cancelled or there is
 * no way to ask. Those two are deliberately the same answer: a plugin has to
 * handle "no file" either way, and a caller that treated "cannot ask" as an
 * error would show an error box on a machine that simply has no GTK.
 */
struct FileDialog {
  static std::string openFile(const std::string& title,
                              const std::vector<FileFilter>& filters = {},
                              const std::string& startingPath = std::string(),
                              void* parentWindow = nullptr) {
#if defined(_WIN32) || defined(__linux__) || defined(__APPLE__)
    return detail::runDialog(detail::Mode::Open, title, filters, startingPath, std::string(),
                             parentWindow);
#else
    (void) title; (void) filters; (void) startingPath; (void) parentWindow;
    return std::string();
#endif
  }

  /**
   * Ask where to save. The extension is added if the user did not type one --
   * "my kit" means "my kit.wav", and writing the extensionless file gives
   * them something their file manager cannot open and they cannot find again.
   */
  static std::string saveFile(const std::string& title,
                              const std::vector<FileFilter>& filters = {},
                              const std::string& suggestedName = std::string(),
                              const std::string& startingPath = std::string(),
                              void* parentWindow = nullptr) {
#if defined(_WIN32) || defined(__linux__) || defined(__APPLE__)
    std::string path = detail::runDialog(detail::Mode::Save, title, filters, startingPath,
                                         suggestedName, parentWindow);
    if (path.empty() || filters.empty()) return path;
    return ensureExtension(path, filterExtensions(filters.front().patterns));
#else
    (void) title; (void) filters; (void) suggestedName; (void) startingPath;
    (void) parentWindow;
    return std::string();
#endif
  }

  static std::string chooseFolder(const std::string& title,
                                  const std::string& startingPath = std::string(),
                                  void* parentWindow = nullptr) {
#if defined(_WIN32) || defined(__linux__) || defined(__APPLE__)
    return detail::runDialog(detail::Mode::Folder, title, {}, startingPath, std::string(),
                             parentWindow);
#else
    (void) title; (void) startingPath; (void) parentWindow;
    return std::string();
#endif
  }

  /**
   * Open the dialog a page asked for, by name.
   *
   * "open", "save", "folder" -- and anything else means open, because a typo
   * should show a file browser rather than nothing.
   *
   * This mapping was written out three times, once per format, and each copy
   * also built the reply to the page by hand. Three copies of an
   * if/else-if is three chances to add a fourth mode to two of them.
   */
  static std::string byMode(const std::string& mode, void* parentWindow = nullptr) {
    if (mode == "save")
      return saveFile("Save", audioFilters(), std::string(), std::string(), parentWindow);
    if (mode == "folder") return chooseFolder("Choose a folder", std::string(), parentWindow);
    return openFile("Open", audioFilters(), std::string(), parentWindow);
  }

  /** The filters a plugin that reads audio almost always wants, so every one
   *  of them does not spell the list out and get it subtly different. Matches
   *  what audiofile.h can actually decode -- offering a format the reader
   *  refuses is worse than not offering it. */
  static std::vector<FileFilter> audioFilters() {
    return {{"Audio files", "*.wav;*.aiff;*.aif;*.flac;*.mp3;*.ogg"},
            {"WAV", "*.wav"},
            {"All files", "*.*"}};
  }
};

} // namespace sonore
