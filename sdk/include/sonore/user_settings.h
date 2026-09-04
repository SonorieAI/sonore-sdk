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

#include <cstdio>
#include <string>
#include <vector>

#include "state_bag.h"

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
 * Empty means there is nowhere to write, a service account, a sandbox, and
 * a caller must treat that as "no settings", not as an error worth reporting.
 */
inline std::string userDataRoot() {
#if defined(_WIN32)
  const char* base = std::getenv("APPDATA");
  if (!base || !base[0]) return std::string();
  return std::string(base) + "\\Sonore";
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
#if defined(_WIN32)
      _mkdir(partial.c_str());
#else
      ::mkdir(partial.c_str(), 0755);
#endif
    }
    if (!end) partial.push_back(c);
  }
#if defined(_WIN32)
  const DWORD attrs = GetFileAttributesA(path.c_str());
  return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
  struct stat st{};
  return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
#endif
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

/**
 * A plugin's per-user settings, loaded from and saved to one file.
 *
 * Every instance of a plugin shares the file, and nothing coordinates them:
 * two editors open at once, both saving, and the last writer wins. That is
 * the right trade for preferences: the alternative is a lock held across a
 * file write in a process that must never block, but it means this is for
 * things a user SETS, not for anything a plugin accumulates continuously.
 */
class UserSettings {
public:
  UserSettings() = default;
  explicit UserSettings(const char* pluginId, const char* leaf = "settings")
      : path_(userSettingsPath(pluginId, leaf)) {}

  const std::string& path() const { return path_; }
  bool usable() const { return !path_.empty(); }

  StateBag& values() { return bag_; }
  const StateBag& values() const { return bag_; }

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
    std::FILE* f = std::fopen(path_.c_str(), "rb");
    if (!f) return false;
    std::vector<uint8_t> bytes;
    uint8_t chunk[4096];
    size_t got = 0;
    // A settings file is small by construction; the cap is here so a wrong
    // path pointing at something enormous cannot be read into memory.
    while ((got = std::fread(chunk, 1, sizeof(chunk), f)) > 0) {
      bytes.insert(bytes.end(), chunk, chunk + got);
      if (bytes.size() > (size_t) 4 * 1024 * 1024) break;
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
    const size_t cut = path_.find_last_of("/\\");
    if (cut != std::string::npos && !ensureDirectory(path_.substr(0, cut))) return false;

    const std::string temp = path_ + ".tmp";
    std::FILE* f = std::fopen(temp.c_str(), "wb");
    if (!f) return false;
    std::vector<uint8_t> bytes;
    bag_.serialise(bytes);
    const bool wrote =
        bytes.empty() || std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    std::fflush(f);
#if defined(_WIN32)
    // fflush hands the bytes to the OS; it does not put them on the platter,
    // and a rename over a file whose contents are still in a cache is an
    // atomic swap to nothing.
    const intptr_t handle = _get_osfhandle(_fileno(f));
    if (handle != -1) FlushFileBuffers((HANDLE) handle);
#endif
    std::fclose(f);
    if (!wrote) {
      std::remove(temp.c_str());
      return false;
    }

#if defined(_WIN32)
    // rename() refuses an existing target on Windows. MoveFileEx with
    // REPLACE_EXISTING is the atomic swap; remove-then-rename would leave a
    // window with no settings file at all.
    if (!MoveFileExA(temp.c_str(), path_.c_str(), MOVEFILE_REPLACE_EXISTING)) {
      std::remove(temp.c_str());
      return false;
    }
#else
    if (std::rename(temp.c_str(), path_.c_str()) != 0) {
      std::remove(temp.c_str());
      return false;
    }
#endif
    return true;
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
