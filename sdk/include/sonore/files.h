// SPDX-License-Identifier: Apache-2.0
//
// Looking at a directory.
//
// ── The gap ─────────────────────────────────────────────────────────────────
//
// There was one directory scan in this SDK -- pluginFilesIn, in host.h -- and
// it is hard-coded to look for .clap, .vst3 and .lv2. Anything else that needed
// to see what was in a folder had nothing: a sampler cannot show its samples, a
// preset browser cannot list its presets, and an editor that wants to say
// "there are four impulse responses here" has no way to find out.
//
// Directory walking and recursive search, in the shape the rest of this SDK
// uses: plain functions, UTF-8
// strings, no class hierarchy.
//
// ── Two things that are easy to get wrong ──────────────────────────────────
//
// ORDER. readdir and FindNextFile both return entries in whatever order the
// filesystem feels like -- creation order on ext4, near-alphabetical on NTFS,
// hash order on some network mounts. A browser built on that reshuffles itself
// between machines and sometimes between runs, and every list of presets is in
// a different order from the last time the user looked. So this sorts, always,
// with directories first and names compared case-insensitively.
//
// ENCODING. The Windows narrow API is not UTF-8: FindFirstFileA on a folder
// called "Percussión" returns mojibake in most locales, and opening the result
// fails. Sample libraries have accented names constantly. So the Windows path
// here is entirely wide, converted at the edges, and the existing narrow scan
// in host.h has the same bug waiting for the first user outside an English
// locale.
#pragma once

#include <algorithm>
#include <cstdint>
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
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace sonore {

/** One entry in a directory. */
struct FileEntry {
  /** The last component -- "kick.wav". */
  std::string name;
  /** The whole path, which is what to open. */
  std::string path;
  bool isDirectory = false;
  uint64_t sizeInBytes = 0;
  /** Seconds since the Unix epoch, or 0 where the platform did not say.
   *  Enough to sort by age, which is what a preset browser wants. */
  int64_t modifiedTime = 0;
};

/** ASCII case folding, deliberately not tolower(): that one is locale-dependent
 *  and in a Turkish locale maps 'I' to a dotless character, which would reorder
 *  a file list for a user who did nothing but set their language. */
inline char asciiLowerChar(char c) {
  return (c >= 'A' && c <= 'Z') ? (char) (c - 'A' + 'a') : c;
}

inline bool asciiLess(const std::string& a, const std::string& b) {
  const size_t n = a.size() < b.size() ? a.size() : b.size();
  for (size_t i = 0; i < n; ++i) {
    const char x = asciiLowerChar(a[i]), y = asciiLowerChar(b[i]);
    if (x != y) return x < y;
  }
  return a.size() < b.size();
}

/** The separator this platform writes. Both are accepted when reading -- a
 *  path that arrived from a config file or a drag from another machine may use
 *  either, and refusing one is a file the user can see and the plugin cannot
 *  open. */
inline char pathSeparator() {
#if defined(_WIN32)
  return '\\';
#else
  return '/';
#endif
}

inline bool isSeparator(char c) { return c == '/' || c == '\\'; }

inline std::string joinPath(const std::string& directory, const std::string& name) {
  if (directory.empty()) return name;
  if (name.empty()) return directory;
  if (isSeparator(directory.back())) return directory + name;
  return directory + pathSeparator() + name;
}

/** The last component. "C:\\a\\b.wav" -> "b.wav", "/a/b/" -> "b". */
inline std::string fileName(const std::string& path) {
  size_t end = path.size();
  while (end > 0 && isSeparator(path[end - 1])) --end;
  size_t start = end;
  while (start > 0 && !isSeparator(path[start - 1])) --start;
  return path.substr(start, end - start);
}

/** Everything before the last component, without the trailing separator. Empty
 *  when there is nothing above -- a root, or a bare name. */
inline std::string parentPath(const std::string& path) {
  size_t end = path.size();
  while (end > 0 && isSeparator(path[end - 1])) --end;
  while (end > 0 && !isSeparator(path[end - 1])) --end;
  while (end > 1 && isSeparator(path[end - 1])) --end;
  if (end == 0) return {};
  // "C:\" and "/" keep their separator: without it "C:" is a drive-relative
  // path meaning "wherever that drive last was", which is not the same place.
  if (end == 1 && isSeparator(path[0])) return path.substr(0, 1);
#if defined(_WIN32)
  if (end == 2 && path[1] == ':') return path.substr(0, 3);
#endif
  return path.substr(0, end);
}

/** The extension INCLUDING the dot, lowercased -- ".wav". Empty when there is
 *  none, and empty for a name that is nothing but a dot prefix: ".gitignore" is
 *  a hidden file, not a file with a nine-letter extension. */
inline std::string fileExtension(const std::string& path) {
  const std::string name = fileName(path);
  const size_t dot = name.rfind('.');
  if (dot == std::string::npos || dot == 0) return {};
  std::string out = name.substr(dot);
  for (char& c : out) c = asciiLowerChar(c);
  return out;
}

/** Whether `path` ends with one of `extensions` (each with its dot, any case).
 *  An empty list accepts everything. */
inline bool hasExtension(const std::string& path, const std::vector<std::string>& extensions) {
  if (extensions.empty()) return true;
  const std::string actual = fileExtension(path);
  for (const std::string& wanted : extensions) {
    std::string lowered = wanted;
    for (char& c : lowered) c = asciiLowerChar(c);
    if (!lowered.empty() && lowered[0] != '.') lowered.insert(lowered.begin(), '.');
    if (actual == lowered) return true;
  }
  return false;
}

#if defined(_WIN32)
/** Its own namespace, not the shared `detail`.
 *
 * file_dialog.h already defines sonore::detail::widen with the same signature.
 * Two headers putting the same inline function into one namespace is an ODR
 * violation, and one the compiler happens to catch here only because both end
 * up in the same translation unit -- in another arrangement it is a linker
 * picking one at random.
 *
 * A header-only SDK where every file adds to a single `detail` namespace is a
 * collision waiting for whichever two headers are first included together,
 * which is exactly how this one surfaced: files.h compiled alone for an hour
 * and broke the moment sdk_tests included it beside the file dialog.
 */
namespace pathdetail {

inline std::wstring widen(const std::string& utf8) {
  if (utf8.empty()) return {};
  const int chars =
      MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int) utf8.size(), nullptr, 0);
  if (chars <= 0) return {};
  std::wstring out((size_t) chars, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int) utf8.size(), &out[0], chars);
  return out;
}

inline std::string narrow(const std::wstring& wide) {
  if (wide.empty()) return {};
  const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), (int) wide.size(), nullptr, 0,
                                        nullptr, nullptr);
  if (bytes <= 0) return {};
  std::string out((size_t) bytes, '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), (int) wide.size(), &out[0], bytes, nullptr,
                      nullptr);
  return out;
}

/** FILETIME is 100-nanosecond ticks since 1601. The constant is the number of
 *  seconds between then and 1970, which is the epoch everything else here
 *  uses. */
inline int64_t toUnixSeconds(const FILETIME& ft) {
  const uint64_t ticks = ((uint64_t) ft.dwHighDateTime << 32) | ft.dwLowDateTime;
  if (ticks == 0) return 0;
  return (int64_t) (ticks / 10000000ULL) - 11644473600LL;
}

} // namespace pathdetail
#endif

inline bool fileExists(const std::string& path) {
#if defined(_WIN32)
  return GetFileAttributesW(pathdetail::widen(path).c_str()) != INVALID_FILE_ATTRIBUTES;
#else
  struct stat info{};
  return stat(path.c_str(), &info) == 0;
#endif
}

inline bool isDirectory(const std::string& path) {
#if defined(_WIN32)
  const DWORD attributes = GetFileAttributesW(pathdetail::widen(path).c_str());
  return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
  struct stat info{};
  return stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
#endif
}

/**
 * What is in a directory.
 *
 * `extensions` filters FILES only -- a folder is never excluded for not being
 * a .wav, because excluding it would hide everything underneath it. An empty
 * list accepts every file.
 *
 * "." and ".." are never returned. A browser that showed them would let a user
 * walk into a directory that is the one they are in.
 *
 * Sorted: directories first, then by name, case-insensitively. See the header
 * for why that is not a nicety.
 */
inline std::vector<FileEntry> listDirectory(const std::string& directory,
                                            const std::vector<std::string>& extensions = {},
                                            bool includeDirectories = true,
                                            bool includeHidden = false) {
  std::vector<FileEntry> out;
  if (directory.empty()) return out;

#if defined(_WIN32)
  std::wstring pattern = pathdetail::widen(directory);
  if (!pattern.empty() && pattern.back() != L'\\' && pattern.back() != L'/') pattern += L'\\';
  pattern += L'*';

  WIN32_FIND_DATAW found{};
  HANDLE handle = FindFirstFileW(pattern.c_str(), &found);
  if (handle == INVALID_HANDLE_VALUE) return out;
  do {
    const std::string name = pathdetail::narrow(found.cFileName);
    if (name == "." || name == "..") continue;
    const bool hidden = (found.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) != 0 ||
                        (found.dwFileAttributes & FILE_ATTRIBUTE_SYSTEM) != 0;
    if (hidden && !includeHidden) continue;

    FileEntry entry;
    entry.isDirectory = (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if (entry.isDirectory && !includeDirectories) continue;
    if (!entry.isDirectory && !hasExtension(name, extensions)) continue;
    entry.name = name;
    entry.path = joinPath(directory, name);
    entry.sizeInBytes = ((uint64_t) found.nFileSizeHigh << 32) | found.nFileSizeLow;
    entry.modifiedTime = pathdetail::toUnixSeconds(found.ftLastWriteTime);
    out.push_back(std::move(entry));
  } while (FindNextFileW(handle, &found));
  FindClose(handle);
#else
  DIR* dir = opendir(directory.c_str());
  if (!dir) return out;
  while (struct dirent* e = readdir(dir)) {
    const std::string name = e->d_name;
    if (name == "." || name == "..") continue;
    if (!includeHidden && !name.empty() && name[0] == '.') continue;

    FileEntry entry;
    entry.name = name;
    entry.path = joinPath(directory, name);
    // stat rather than d_type: d_type is DT_UNKNOWN on several filesystems --
    // XFS and some network mounts among them -- and a browser that trusted it
    // shows every folder as a file exactly on the machines where a sample
    // library is most likely to live.
    struct stat info{};
    if (stat(entry.path.c_str(), &info) == 0) {
      entry.isDirectory = S_ISDIR(info.st_mode);
      entry.sizeInBytes = (uint64_t) info.st_size;
      entry.modifiedTime = (int64_t) info.st_mtime;
    }
    if (entry.isDirectory && !includeDirectories) continue;
    if (!entry.isDirectory && !hasExtension(name, extensions)) continue;
    out.push_back(std::move(entry));
  }
  closedir(dir);
#endif

  std::sort(out.begin(), out.end(), [](const FileEntry& a, const FileEntry& b) {
    if (a.isDirectory != b.isDirectory) return a.isDirectory;
    return asciiLess(a.name, b.name);
  });
  return out;
}

/** The user's home directory, or empty. Where a browser should start when
 *  nobody has said otherwise -- the process's current directory is wherever
 *  the HOST was launched from, which is a system folder nobody keeps samples
 *  in. */
inline std::string homeDirectory() {
#if defined(_WIN32)
  wchar_t buffer[MAX_PATH * 2];
  DWORD length = GetEnvironmentVariableW(L"USERPROFILE", buffer, (DWORD) (MAX_PATH * 2));
  if (length > 0 && length < MAX_PATH * 2) return pathdetail::narrow(std::wstring(buffer, length));
  return {};
#else
  if (const char* home = getenv("HOME")) return home;
  return {};
#endif
}

/**
 * The tops of the tree: every drive on Windows, "/" everywhere else.
 *
 * A file browser needs somewhere to go when the user walks up past the last
 * parent, and on Windows that is not a single root -- a sample library on D:
 * cannot be reached from C: by going up.
 */
inline std::vector<std::string> rootPaths() {
  std::vector<std::string> out;
#if defined(_WIN32)
  const DWORD mask = GetLogicalDrives();
  for (int i = 0; i < 26; ++i)
    if (mask & (1u << i)) {
      char letter = (char) ('A' + i);
      out.push_back(std::string(1, letter) + ":\\");
    }
#else
  out.push_back("/");
#endif
  return out;
}

} // namespace sonore
