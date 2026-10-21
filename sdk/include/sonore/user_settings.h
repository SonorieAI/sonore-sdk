// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: settings that belong to the USER, not to the session.
//
// A plugin has two kinds of persistent state and hosts only understand one of
// them. The session blob is per-instance: this reverb, on this track, in this
// project. But a plugin also accumulates things that belong to the person
// using it: where their sample folder is, what scale their editor should
// open at, which one-time notice they have already dismissed, which output
// device the standalone last used.
//
// Putting any of that in the session blob gets it wrong in both directions.
// It travels to whoever the project is sent to, carrying a path that does not
// exist on their machine; and it is absent from every NEW instance, so the
// preference the user set five minutes ago does not apply to the next plugin
// they add.
//
// A properties file, and here it is the same StateBag the session
// uses, written to a per-user file: deliberately the same, because a second
// key/value format is a second parser to get wrong, and this one is already
// exercised by every state round-trip in the suite.
//
// MAIN THREAD ONLY. It opens files.
#pragma once

#include <atomic>
#include <cstdio>
#include <string>
#include <vector>

#include "state_bag.h"
#include "path_io.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <direct.h>
#include <io.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#include <cstdlib>
#endif

namespace sonore {

/**
 * Where this machine keeps per-user application data.
 *
 * Each platform has one right answer and several wrong ones that work until
 * they don't: $HOME on Windows is often unset, %APPDATA% is roaming and
 * correct, and on Linux XDG_CONFIG_HOME beats a hardcoded ~/.config for
 * exactly the users who have set it.
 *
 * Empty means there is nowhere to write (a service account, a sandbox), and
 * a caller must treat that as "no settings", not as an error worth reporting.
 */
inline std::string userDataRoot() {
#if defined(_WIN32)
  // As UTF-8: the profile is named after its user, and getenv's ANSI answer
  // turned C:\Users\Олег into question marks.
  const std::string base = pathio::environment("APPDATA");
  if (base.empty()) return std::string();
  return base + "\\Sonore";
#elif defined(__APPLE__)
  const char* home = std::getenv("HOME");
  if (!home || !home[0]) return std::string();
  return std::string(home) + "/Library/Application Support/Sonore";
#else
  if (const char* xdg = std::getenv("XDG_CONFIG_HOME"))
    if (xdg[0]) return std::string(xdg) + "/sonore";
  const char* home = std::getenv("HOME");
  if (!home || !home[0]) return std::string();
  return std::string(home) + "/.config/sonore";
#endif
}

/** Make a directory and its parents. Existing is success, which is the whole
 *  reason this is not one call: mkdir -p is not in the standard library and
 *  every caller writes a slightly different version of it. */
inline bool ensureDirectory(const std::string& path) {
  if (path.empty()) return false;
  // Almost always it is there already, and asking costs one call. The walk
  // below costs one per level of the path, each refused as existing -- on
  // Windows a fifth of a millisecond apiece, a millisecond and more for a
  // folder under the user's profile -- paid by every preset saved and every
  // CLAP host loading a plugin with factory presets (syncFactoryPresetFiles).
  // The walk ends by asking the same question, so the answer is the same.
  if (pathio::isDirectoryAt(path.c_str())) return true;
  std::string partial;
  for (size_t i = 0; i <= path.size(); ++i) {
    const bool end = i == path.size();
    const char c = end ? '\0' : path[i];
    if (!end && c != '/' && c != '\\') {
      partial.push_back(c);
      continue;
    }
    if (partial.empty()) {
      if (!end) partial.push_back(c); // a leading slash is the root, not a name
      continue;
    }
    // A bare drive letter is not a directory anyone can create.
    const bool driveRoot = partial.size() == 2 && partial[1] == ':';
    if (!driveRoot) {
      pathio::makeDirectory(partial.c_str());
    }
    if (!end) partial.push_back(c);
  }
  return pathio::isDirectoryAt(path.c_str());
}

/** One plugin's settings file. Named by the plugin id, which is the only
 *  identifier guaranteed stable across versions and renames. */
inline std::string userSettingsPath(const char* pluginId, const char* leaf = "settings") {
  const std::string root = userDataRoot();
  if (root.empty() || !pluginId || !pluginId[0]) return std::string();
#if defined(_WIN32)
  const char sep = '\\';
#else
  const char sep = '/';
#endif
  // The id is a reverse-DNS string and can contain anything a generator put
  // there. Only the characters a path can safely hold survive.
  std::string safe;
  for (const char* p = pluginId; *p; ++p) {
    const char c = *p;
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
    safe.push_back(ok ? c : '_');
  }
  return root + sep + safe + sep + (leaf && leaf[0] ? leaf : "settings") + ".dat";
}

namespace detail {

/** The most UserSettings::load reads; save writes nothing larger. A settings
 *  file is small by construction, and the cap keeps a wrong path pointing at
 *  something enormous from being read into memory. */
constexpr size_t kMaxUserSettingsBytes = (size_t) 4 * 1024 * 1024;

/**
 * Write a whole file, atomically: through a temporary and a rename, so a
 * crash or a power cut leaves the PREVIOUS contents rather than half of the
 * new ones. Shared by UserSettings and PropertiesFile (application_properties.h),
 * which is why it is here rather than inside one of them.
 */
inline bool writeFileAtomically(const std::string& path, const std::vector<uint8_t>& bytes) {
  if (path.empty()) return false;
  const size_t cut = path.find_last_of("/\\");
  if (cut != std::string::npos && !ensureDirectory(path.substr(0, cut))) return false;

  // A temporary of this writer's own. One shared name was truncated by
  // every other writer mid-save -- a second instance of the plugin, or a
  // second host process -- and the rename moved whichever mixture won, or
  // failed against a temporary someone else still had open: 157 of 180
  // saves from six concurrent writers. Unique per process and per save,
  // the renames simply land in turn and the last one is the file.
  static std::atomic<unsigned> sequence{0};
#if defined(_WIN32)
  const unsigned long pid = (unsigned long) GetCurrentProcessId();
#else
  const unsigned long pid = (unsigned long) getpid();
#endif
  char suffix[64];
  std::snprintf(suffix, sizeof(suffix), ".tmp-%lu-%u", pid, sequence.fetch_add(1));
  const std::string temp = path + suffix;
  std::FILE* f = ::sonore::pathio::open(temp.c_str(), "wb");
  if (!f) return false;
  // Every step's answer counts, not only fwrite's. A settings file is a few
  // hundred bytes, which fwrite only copies into the CRT's buffer -- so on a
  // full disk it "succeeds", the real write fails inside fflush or fclose,
  // and a zero-byte temporary was then renamed over the good settings with
  // save() reporting true. Every preference gone, silently.
  bool wrote =
      bytes.empty() || std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
  wrote = std::fflush(f) == 0 && wrote;
#if defined(_WIN32)
  // fflush hands the bytes to the OS; it does not put them on the platter,
  // and a rename over a file whose contents are still in a cache is an
  // atomic swap to nothing.
  const intptr_t handle = _get_osfhandle(_fileno(f));
  if (handle != -1) wrote = FlushFileBuffers((HANDLE) handle) != 0 && wrote;
#else
  // The same reasoning, which the POSIX side had none of: rename() is
  // atomic over the NAME, not over data still in the page cache.
  wrote = fsync(fileno(f)) == 0 && wrote;
#endif
  wrote = std::fclose(f) == 0 && wrote;
  if (!wrote) {
    pathio::remove(temp.c_str());
    return false;
  }

#if defined(_WIN32)
  // rename() refuses an existing target on Windows. MoveFileEx with
  // REPLACE_EXISTING is the atomic swap; remove-then-rename would leave a
  // window with no file at all.
  //
  // Another writer's replace, or a reader with the file open, holds the
  // target for an instant; that is a sharing violation, not a failure, and
  // it is waited out for a moment rather than reported as a lost save.
  const std::wstring from = pathio::widenPath(temp.c_str()), to = pathio::widenPath(path.c_str());
  bool moved = false;
  for (int attempt = 0; attempt < 50 && !moved; ++attempt) {
    moved = MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
    if (!moved) {
      const DWORD error = GetLastError();
      if (error != ERROR_ACCESS_DENIED && error != ERROR_SHARING_VIOLATION) break;
      Sleep(1);
    }
  }
  if (!moved) {
    pathio::remove(temp.c_str());
    return false;
  }
#else
  if (pathio::rename(temp.c_str(), path.c_str()) != 0) {
    pathio::remove(temp.c_str());
    return false;
  }
#endif
  return true;
}

} // namespace detail

/**
 * A plugin's per-user settings, loaded from and saved to one file.
 *
 * Every instance of a plugin shares the file, and nothing coordinates them:
 * two editors open at once, both saving, and the last writer wins. That is
 * the right trade for preferences (the alternative is a lock held across a
 * file write in a process that must never block), but it means this is for
 * things a user SETS, not for anything a plugin accumulates continuously.
 */
class UserSettings {
public:
  /** No file: usable() is false, and load() and save() fail. */
  UserSettings() = default;
  /** The file userSettingsPath(pluginId, leaf) names. Nothing is read until
   *  load(). */
  explicit UserSettings(const char* pluginId, const char* leaf = "settings")
      : path_(userSettingsPath(pluginId, leaf)) {}

  /** The file's full UTF-8 path; empty when there is no user data folder or
   *  no plugin id. */
  const std::string& path() const { return path_; }
  /** False when there is nowhere to keep the file (path() is empty). */
  bool usable() const { return !path_.empty(); }

  ///@{
  /** The settings: filled by load(), written by save(). A change stays in
   *  memory until save(). Not locked: one thread at a time. */
  StateBag& values() { return bag_; }
  const StateBag& values() const { return bag_; }
  ///@}

  /**
   * Read the file. False means there was nothing to read or it was unusable.
   *
   * A missing file is the NORMAL first run and not a failure; a corrupt one
   * is treated the same way on purpose, because the alternative is a plugin
   * that refuses to open because a preferences file went bad.
   */
  bool load() {
    bag_ = StateBag();
    if (path_.empty()) return false;
    std::FILE* f = ::sonore::pathio::open(path_.c_str(), "rb");
    if (!f) return false;
    std::vector<uint8_t> bytes;
    uint8_t chunk[4096];
    size_t got = 0;
    // A settings file is small by construction; the cap is here so a wrong
    // path pointing at something enormous cannot be read into memory.
    while ((got = std::fread(chunk, 1, sizeof(chunk), f)) > 0) {
      bytes.insert(bytes.end(), chunk, chunk + got);
      if (bytes.size() > detail::kMaxUserSettingsBytes) break;
    }
    std::fclose(f);
    if (bytes.empty()) return false;
    return bag_.deserialise(bytes.data(), bytes.size());
  }

  /**
   * Write the file, atomically.
   *
   * Through a temporary and a rename, so a crash or a power cut leaves the
   * PREVIOUS settings rather than half of the new ones. Writing in place is
   * the version that works every time it is tested and loses somebody's
   * preferences once.
   */
  bool save() const {
    if (path_.empty()) return false;
    std::vector<uint8_t> bytes;
    bag_.serialise(bytes);
    // Never more than load() reads back. A larger file failed to load, so
    // every preference in it read as a default, and the next save -- of any
    // one of them, writeString included -- wrote that empty bag over the
    // file. Refused, the file on disk keeps what it had.
    if (bytes.size() > detail::kMaxUserSettingsBytes) return false;
    return detail::writeFileAtomically(path_, bytes);
  }

  /** Read one value without keeping the object around, for the common case of
   *  wanting a single preference at startup. */
  static std::string readString(const char* pluginId, const char* key,
                                const std::string& fallback = std::string()) {
    UserSettings s(pluginId);
    if (!s.load()) return fallback;
    return s.values().getString(key, fallback);
  }

  /** Write one value, preserving everything else in the file. Read-modify-write
   *  rather than replace: a plugin that saved only the key it cared about
   *  would delete every other preference the user had set. */
  static bool writeString(const char* pluginId, const char* key, const std::string& value) {
    UserSettings s(pluginId);
    s.load(); // a missing file is a normal first run
    s.values().setString(key, value);
    return s.save();
  }

private:
  std::string path_;
  StateBag bag_;
};

} // namespace sonore
