// SPDX-License-Identifier: Apache-2.0
//
// The "Open Recent" list: most recent first, a fixed length, and the OS told.
//
// ── What it is for ──────────────────────────────────────────────────────────
//
// An application that opens documents keeps the last few, puts them in a
// menu, and remembers them across runs. The rules are small and easy to get
// subtly wrong, which is the reason for a class: re-opening a file moves it
// to the TOP rather than adding it twice; the list never grows past its
// limit; a file deleted since is offered for removal rather than failing
// when chosen; and the menu ids map back to the list by index, so a menu
// built with some files skipped still answers with the right one.
//
// ── Paths compared as the filesystem compares them ─────────────────────────
//
// "C:\Music\Kit.wav" and "c:/music/kit.wav" are one file on Windows, and on a
// default (case-insensitive) macOS volume "Kit.wav" and "kit.wav" are too. A
// list that compared bytes would show the same document twice. Linux compares
// exactly, as its filesystems do.
//
// ── The OS's own list ───────────────────────────────────────────────────────
//
// registerRecentFileNatively() puts a file where the OS shows recent
// documents: SHAddToRecentDocs on Windows (the Recent folder, jump lists),
// NSDocumentController on macOS (the Dock menu, "Open Recent"). Linux's
// desktop list belongs to GTK's recent manager, which needs a running GTK main
// loop to write anything; the reference does nothing there, and nor does this.
// Windows is RUN here; the macOS path is compiled only.
//
// Not thread-safe: an application's menu lives on the message thread.
#pragma once

#include <algorithm>
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
#include <objbase.h>
#include <shlobj.h>
#if defined(_MSC_VER)
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")
#endif
#elif defined(__APPLE__)
#include <objc/message.h>
#include <objc/runtime.h>
#include <sys/stat.h>
#else
#include <sys/stat.h>
#endif

#include "path_io.h"

namespace sonore {
namespace detail {
namespace recent {

inline char lowerAscii(char c) { return (c >= 'A' && c <= 'Z') ? (char) (c - 'A' + 'a') : c; }

/** One file, however its path was spelled. See the header. */
inline bool samePath(const std::string& a, const std::string& b) {
#if defined(_WIN32) || defined(__APPLE__)
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    char x = a[i], y = b[i];
#if defined(_WIN32)
    if (x == '/') x = '\\';
    if (y == '/') y = '\\';
#endif
    if (lowerAscii(x) != lowerAscii(y)) return false;
  }
  return true;
#else
  return a == b;
#endif
}

inline bool pathExists(const std::string& path) {
  if (path.empty()) return false;
#if defined(_WIN32)
  return GetFileAttributesW(pathio::widenPath(path.c_str()).c_str()) != INVALID_FILE_ATTRIBUTES;
#else
  struct stat st {};
  return ::stat(path.c_str(), &st) == 0;
#endif
}

inline std::string leafName(const std::string& path) {
  const size_t cut = path.find_last_of("/\\");
  return cut == std::string::npos ? path : path.substr(cut + 1);
}

#if defined(_WIN32)
/** The shell's Recent folder, where SHAddToRecentDocs leaves its shortcuts. */
inline std::wstring recentFolder() {
  PWSTR folder = nullptr;
  std::wstring out;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Recent, 0, nullptr, &folder)) && folder) out = folder;
  if (folder) CoTaskMemFree(folder);
  return out;
}

/** Where a shortcut points, through IShellLink, or empty. */
inline std::wstring shortcutTarget(const std::wstring& link) {
  std::wstring target;
  const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  IShellLinkW* shell = nullptr;
  if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW,
                                 (void**) &shell)) &&
      shell) {
    IPersistFile* file = nullptr;
    if (SUCCEEDED(shell->QueryInterface(IID_IPersistFile, (void**) &file)) && file) {
      if (SUCCEEDED(file->Load(link.c_str(), STGM_READ))) {
        wchar_t buffer[MAX_PATH * 4] = {0};
        if (SUCCEEDED(shell->GetPath(buffer, MAX_PATH * 4, nullptr, SLGP_RAWPATH))) target = buffer;
      }
      file->Release();
    }
    shell->Release();
  }
  // Balanced only if this call initialised it: a host's apartment is the
  // host's (the same rule file_dialog.h follows).
  if (init == S_OK || init == S_FALSE) CoUninitialize();
  return target;
}
#endif

#if defined(__APPLE__)
template <typename Ret, typename... Args>
Ret objcCall(id target, const char* selector, Args... args) {
  using Fn = Ret (*)(id, SEL, Args...);
  return reinterpret_cast<Fn>(objc_msgSend)(target, sel_registerName(selector), args...);
}

inline id documentController() {
  Class cls = objc_getClass("NSDocumentController");
  if (!cls) return (id) 0;
  return objcCall<id>((id) cls, "sharedDocumentController");
}

inline id fileUrl(const std::string& path) {
  Class stringClass = objc_getClass("NSString");
  Class urlClass = objc_getClass("NSURL");
  if (!stringClass || !urlClass) return (id) 0;
  id text = objcCall<id>((id) stringClass, "stringWithUTF8String:", path.c_str());
  if (!text) return (id) 0;
  return objcCall<id>((id) urlClass, "fileURLWithPath:", text);
}

/** An autorelease pool for the length of a scope: every Cocoa call above
 *  returns autoreleased objects, and outside an application's run loop
 *  nothing else would drain them. */
struct AutoreleasePool {
  id pool = (id) 0;
  AutoreleasePool() {
    if (Class cls = objc_getClass("NSAutoreleasePool"))
      pool = objcCall<id>(objcCall<id>((id) cls, "alloc"), "init");
  }
  ~AutoreleasePool() {
    if (pool) objcCall<void>(pool, "drain");
  }
};
#endif

} // namespace recent
} // namespace detail

/**
 * An application's "Open Recent" list: UTF-8 paths, most recent first, eight
 * at most unless setMaxNumberOfItems() says otherwise. Mirrors the
 * reference's class of the same name, and the file comment has its rules.
 *
 * Holds paths only, and saves as text (toString / restoreFromString) for the
 * application's settings file. The two static calls also tell the OS's own
 * list. MESSAGE THREAD: nothing is locked.
 */
class RecentlyOpenedFilesList {
public:
  /** Empty, holding at most eight. */
  RecentlyOpenedFilesList() = default;

  /** The longest the list may be. Shrinking drops the OLDEST. Below one is
   *  taken as one: a recent-files list that can hold nothing is a bug. */
  void setMaxNumberOfItems(int maxItems) {
    maxItems_ = maxItems < 1 ? 1 : maxItems;
    if ((int) files_.size() > maxItems_) files_.resize((size_t) maxItems_);
  }
  /** The longest the list may be: 8 unless set, never below 1. */
  int getMaxNumberOfItems() const { return maxItems_; }

  /** How many files the list holds now. */
  int getNumFiles() const { return (int) files_.size(); }

  /** The file at `index`, 0 being the most recent; empty out of range. */
  std::string getFile(int index) const {
    if (index < 0 || index >= (int) files_.size()) return std::string();
    return files_[(size_t) index];
  }

  /** Every file, most recent first. Valid until the list is changed. */
  const std::vector<std::string>& getAllFilenames() const { return files_; }

  /** Empties the list. The OS's own list is not touched. */
  void clear() { files_.clear(); }

  /** Put a file at the top. Already in the list: moved, not repeated. */
  void addFile(const std::string& path) {
    if (path.empty()) return;
    removeFile(path);
    files_.insert(files_.begin(), path);
    setMaxNumberOfItems(maxItems_);
  }

  /** Removes `path` if it is in the list. On Windows and macOS a spelling
   *  that differs only in ASCII case is the same file (and on Windows either
   *  separator); on Linux the path must match exactly. */
  void removeFile(const std::string& path) {
    files_.erase(std::remove_if(files_.begin(), files_.end(),
                                [&path](const std::string& f) {
                                  return detail::recent::samePath(f, path);
                                }),
                 files_.end());
  }

  /** Drop every entry whose file is no longer there. */
  void removeNonExistentFiles() {
    files_.erase(std::remove_if(files_.begin(), files_.end(),
                                [](const std::string& f) { return !detail::recent::pathExists(f); }),
                 files_.end());
  }

  /**
   * Add the files to a menu, as items `baseItemId + index`.
   *
   * The id is the file's index in the WHOLE list, skipped ones included, so
   * `getFile(chosenId - baseItemId)` is always the file that was clicked.
   * `Menu` is gfx::PopupMenu or anything with addItem(int, std::string) --
   * a template so this header does not drag the GUI into a document class.
   * Returns how many items were added.
   */
  template <typename Menu>
  int createPopupMenuItems(Menu& menu, int baseItemId, bool showFullPaths,
                           bool dontAddNonExistentFiles,
                           const std::vector<std::string>* filesToAvoid = nullptr) const {
    int added = 0;
    for (size_t i = 0; i < files_.size(); ++i) {
      const std::string& file = files_[i];
      if (dontAddNonExistentFiles && !detail::recent::pathExists(file)) continue;
      bool avoid = false;
      if (filesToAvoid)
        for (const std::string& other : *filesToAvoid)
          if (detail::recent::samePath(file, other)) avoid = true;
      if (avoid) continue;
      menu.addItem(baseItemId + (int) i,
                   showFullPaths ? file : detail::recent::leafName(file));
      ++added;
    }
    return added;
  }

  /** One path per line, most recent first: what a settings file stores. */
  std::string toString() const {
    std::string out;
    for (size_t i = 0; i < files_.size(); ++i) {
      if (i) out.push_back('\n');
      out += files_[i];
    }
    return out;
  }

  /** The other half of toString(). Blank lines, repeats and anything past the
   *  limit are dropped -- the string may have been edited by hand, or saved
   *  by a build with a longer limit. */
  void restoreFromString(const std::string& text) {
    files_.clear();
    std::string line;
    auto flush = [this, &line]() {
      while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
      if (!line.empty() && (int) files_.size() < maxItems_) {
        bool seen = false;
        for (const std::string& f : files_)
          if (detail::recent::samePath(f, line)) seen = true;
        if (!seen) files_.push_back(line);
      }
      line.clear();
    };
    for (char c : text) {
      if (c == '\n') flush();
      else line.push_back(c);
    }
    flush();
  }

  // ── The OS's own recent-documents list ────────────────────────────────────

  /** Tell the OS this file was opened. See the header for each platform.
   *  Windows quietly ignores files in the temp folder (and everything, when
   *  the user has turned recent items off): no error, no shortcut. */
  static void registerRecentFileNatively(const std::string& path) {
    if (path.empty()) return;
#if defined(_WIN32)
    const std::wstring wide = pathio::widen(path.c_str());
    SHAddToRecentDocs(SHARD_PATHW, wide.c_str());
#elif defined(__APPLE__)
    detail::recent::AutoreleasePool pool;
    id controller = detail::recent::documentController();
    id url = detail::recent::fileUrl(path);
    if (controller && url) detail::recent::objcCall<void>(controller, "noteNewRecentDocumentURL:", url);
#endif
  }

  /**
   * Take one file off the OS's list.
   *
   * Neither OS has a call for one file. Windows: the shortcut in the Recent
   * folder that points at it is deleted (found by its target, not its name --
   * two "Kit.wav" in different folders share a shortcut name). macOS: the
   * list is cleared and every other entry re-added, oldest first, which is
   * what the reference does.
   */
  static void forgetRecentFileNatively(const std::string& path) {
    if (path.empty()) return;
#if defined(_WIN32)
    const std::wstring folder = detail::recent::recentFolder();
    if (folder.empty()) return;
    const std::wstring leaf = pathio::widen(detail::recent::leafName(path).c_str());
    const std::wstring link = folder + L"\\" + leaf + L".lnk";
    if (GetFileAttributesW(link.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    const std::string target = pathio::narrow(detail::recent::shortcutTarget(link).c_str());
    if (detail::recent::samePath(target, path)) DeleteFileW(link.c_str());
#elif defined(__APPLE__)
    detail::recent::AutoreleasePool pool;
    id controller = detail::recent::documentController();
    id forgotten = detail::recent::fileUrl(path);
    if (!controller || !forgotten) return;
    using detail::recent::objcCall;
    id urls = objcCall<id>(controller, "recentDocumentURLs");
    if (!urls) return;
    // Retained across the clear: the array is the controller's, and clearing
    // releases it.
    objcCall<id>(urls, "retain");
    objcCall<void>(controller, "clearRecentDocuments:", (id) 0);
    const unsigned long count = objcCall<unsigned long>(urls, "count");
    for (unsigned long i = count; i-- > 0;) {
      id url = objcCall<id>(urls, "objectAtIndex:", i);
      if (url && !objcCall<BOOL>(url, "isEqual:", forgotten))
        objcCall<void>(controller, "noteNewRecentDocumentURL:", url);
    }
    objcCall<void>(urls, "release");
#endif
  }

  /**
   * Empty the OS's list for this application. macOS only: on Windows the
   * only call that does it (SHAddToRecentDocs with a null path) empties the
   * USER's list for every application, which is not ours to do.
   */
  static void clearRecentFilesNatively() {
#if defined(__APPLE__)
    detail::recent::AutoreleasePool pool;
    if (id controller = detail::recent::documentController())
      detail::recent::objcCall<void>(controller, "clearRecentDocuments:", (id) 0);
#endif
  }

private:
  std::vector<std::string> files_;
  int maxItems_ = 8;
};

} // namespace sonore
