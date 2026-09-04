// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: remembering what a scan found, and what killed it.
//
// Scanning a plugin folder means LOADING SOMEBODY ELSE'S CODE. Every file in
// it gets dlopened, its entry point called, its factory walked. That is slow
//, a full folder is seconds, and it is dangerous, because one plugin that
// faults on load takes the whole host down with it, every single time, and
// the user has no way to get past it.
//
// Two problems, and the second is the interesting one.
//
// SLOW is solved by writing down what was found and checking timestamps: a
// file that has not changed since it was scanned does not need scanning
// again.
//
// CRASHING is solved by a dead man's switch. The path being scanned is
// written to the cache and FLUSHED TO DISK before the file is opened. If the
// process dies inside that load, the entry is still sitting there next time,
// and a plugin that was mid-scan when the host last died is a plugin that
// killed it. It goes on the blacklist and is not tried again.
//
// That trick is the whole reason this can work in-process. The alternative:
// scanning in a child process, as some hosts do: is more robust and costs a
// second binary, an IPC protocol and a lifetime to get wrong. This costs one
// fflush.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "host.h"
#include "preset_file.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

// _get_osfhandle, which is how a FILE* becomes something FlushFileBuffers
// will take.
#include <io.h>
#else
#include <sys/stat.h>
#endif

namespace sonore {
namespace host {

/** What a file looked like when it was scanned. Size and modification time
 *  together, because either alone is fooled by something ordinary: a rebuild
 *  that lands on the same size, or an installer that preserves timestamps. */
struct FileStamp {
  uint64_t size = 0;
  uint64_t modified = 0;

  bool operator==(const FileStamp& other) const {
    return size == other.size && modified == other.modified;
  }
  bool valid() const { return size != 0 || modified != 0; }
};

inline FileStamp stampOf(const std::string& path) {
  FileStamp stamp;
#if defined(_WIN32)
  WIN32_FILE_ATTRIBUTE_DATA data{};
  if (GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &data)) {
    stamp.size = ((uint64_t) data.nFileSizeHigh << 32) | data.nFileSizeLow;
    stamp.modified = ((uint64_t) data.ftLastWriteTime.dwHighDateTime << 32) |
                     data.ftLastWriteTime.dwLowDateTime;
  }
  // A .vst3 or .lv2 is a FOLDER, and a folder's own size is zero and its
  // timestamp does not move when the binary inside it is replaced. Stamping
  // the binary is what actually answers "has this changed".
  if (stamp.size == 0) {
    const std::string inner = vst3BinaryPath(path);
    if (inner != path && GetFileAttributesExA(inner.c_str(), GetFileExInfoStandard, &data)) {
      stamp.size = ((uint64_t) data.nFileSizeHigh << 32) | data.nFileSizeLow;
      stamp.modified = ((uint64_t) data.ftLastWriteTime.dwHighDateTime << 32) |
                       data.ftLastWriteTime.dwLowDateTime;
    }
  }
#else
  struct stat st {};
  if (::stat(path.c_str(), &st) == 0) {
    stamp.size = (uint64_t) st.st_size;
    stamp.modified = (uint64_t) st.st_mtime;
  }
#endif
  return stamp;
}

/**
 * What previous scans found, and what they must not try again.
 *
 * The file format is one record per line, tab separated, with a version on
 * the first line. Not JSON and not binary: a user whose host will not start
 * because of a cache should be able to open it in a text editor and see which
 * plugin is the problem, and delete one line rather than the lot.
 */
class PluginCache {
public:
  /** Everything a previous scan described. */
  const std::vector<PluginDescription>& plugins() const { return plugins_; }
  /** Paths that must not be loaded, because loading one killed the host. */
  const std::vector<std::string>& blacklist() const { return blacklist_; }
  /** The file that was being scanned when the last run ended. Empty unless
   *  that run died inside a plugin's own code. */
  const std::string& crashedOn() const { return crashedOn_; }

  bool isBlacklisted(const std::string& path) const {
    for (const std::string& entry : blacklist_)
      if (entry == path) return true;
    return false;
  }

  void blacklistPath(const std::string& path) {
    if (!isBlacklisted(path)) blacklist_.push_back(path);
  }

  /** Take a path off the blacklist, for a user who has updated the plugin and
   *  wants to give it another chance. Returns whether it was there. */
  bool forgive(const std::string& path) {
    for (size_t i = 0; i < blacklist_.size(); ++i) {
      if (blacklist_[i] != path) continue;
      blacklist_.erase(blacklist_.begin() + (ptrdiff_t) i);
      return true;
    }
    return false;
  }

  /** Forget the in-progress mark, once it has been acted on.
   *
   *  Without this the mark outlives the blacklisting it caused: a caller that
   *  scans, forgives a plugin and scans again gets it blacklisted straight
   *  back, because the same object still remembers what the last RUN died
   *  inside. Which makes forgive() look like it does nothing. */
  void clearCrashMark() { crashedOn_.clear(); }

  void clear() {
    plugins_.clear();
    blacklist_.clear();
    stamps_.clear();
    crashedOn_.clear();
  }

  /** What this cache already knows about a file, if it is still the same
   *  file. Empty means it has to be scanned. */
  std::vector<PluginDescription> knownFor(const std::string& path) const {
    const FileStamp now = stampOf(path);
    if (!now.valid()) return {};

    bool unchanged = false;
    for (const auto& entry : stamps_)
      if (entry.first == path) unchanged = (entry.second == now);
    if (!unchanged) return {};

    std::vector<PluginDescription> found;
    for (const PluginDescription& description : plugins_)
      if (description.path == path) found.push_back(description);
    return found;
  }

  /** Whether a file has been scanned before and has not changed since --
   *  including one that turned out to hold no plugins, which is exactly the
   *  case knownFor() cannot express because its answer is empty either way. */
  bool isUpToDate(const std::string& path) const {
    const FileStamp now = stampOf(path);
    if (!now.valid()) return false;
    for (const auto& entry : stamps_)
      if (entry.first == path) return entry.second == now;
    return false;
  }

  /** Record what a file turned out to contain. A file with no plugins in it
   *  is REMEMBERED as empty rather than forgotten: otherwise every scan
   *  reopens every stray DLL in the folder for ever. */
  void remember(const std::string& path, const std::vector<PluginDescription>& found) {
    for (size_t i = plugins_.size(); i-- > 0;)
      if (plugins_[i].path == path) plugins_.erase(plugins_.begin() + (ptrdiff_t) i);
    for (const PluginDescription& description : found) plugins_.push_back(description);

    const FileStamp stamp = stampOf(path);
    for (auto& entry : stamps_)
      if (entry.first == path) {
        entry.second = stamp;
        return;
      }
    stamps_.push_back({path, stamp});
  }

  bool load(const char* path) {
    clear();
    if (!path) return false;
    std::FILE* file = std::fopen(path, "rb");
    if (!file) return false;

    std::string text;
    char buffer[8192];
    size_t got = 0;
    while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0) text.append(buffer, got);
    std::fclose(file);

    size_t pos = 0;
    bool first = true;
    while (pos < text.size()) {
      size_t end = text.find('\n', pos);
      if (end == std::string::npos) end = text.size();
      std::string line = text.substr(pos, end - pos);
      pos = end + 1;
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (line.empty()) continue;

      if (first) {
        first = false;
        // An unrecognised version is not an error to report, it is a cache to
        // throw away: the next scan rebuilds it, and refusing to start
        // because of a stale cache would be worse than the cache.
        if (line != kVersion) {
          clear();
          return false;
        }
        continue;
      }

      std::vector<std::string> fields = split(line);
      if (fields.empty()) continue;
      if (fields[0] == "SCANNING" && fields.size() >= 2) {
        crashedOn_ = fields[1];
      } else if (fields[0] == "BLACKLIST" && fields.size() >= 2) {
        blacklist_.push_back(fields[1]);
      } else if (fields[0] == "STAMP" && fields.size() >= 4) {
        FileStamp stamp;
        stamp.size = (uint64_t) std::strtoull(fields[2].c_str(), nullptr, 10);
        stamp.modified = (uint64_t) std::strtoull(fields[3].c_str(), nullptr, 10);
        stamps_.push_back({fields[1], stamp});
      } else if (fields[0] == "PLUGIN" && fields.size() >= 9) {
        PluginDescription description;
        description.path = fields[1];
        description.format = fields[2];
        description.id = fields[3];
        description.name = fields[4];
        description.vendor = fields[5];
        description.version = fields[6];
        description.features = fields[7];
        description.isInstrument = fields[8] == "1";
        plugins_.push_back(description);
      }
    }
    return true;
  }

  bool save(const char* path) const { return writeTo(path, std::string()); }

  /**
   * Write the cache with `path` marked as in progress, and FLUSH IT.
   *
   *  This is the dead man's switch, and the flush is the whole point: a
   *  buffered write that is still in memory when the plugin faults tells the
   *  next run nothing. It has to be on the disk before the file is opened.
   *
   *  Costs one rewrite of the cache per plugin scanned, which on a folder of
   *  a hundred is a hundred small writes and still far less than the loading
   *  itself.
   */
  bool beginScanning(const char* cachePath, const std::string& pluginPath) const {
    return writeTo(cachePath, pluginPath);
  }

  /** Clear the in-progress mark: the file was survived. */
  bool endScanning(const char* cachePath) const { return writeTo(cachePath, std::string()); }

private:
  static constexpr const char* kVersion = "SONORE-PLUGIN-CACHE-1";

  static std::vector<std::string> split(const std::string& line) {
    std::vector<std::string> fields;
    size_t pos = 0;
    while (true) {
      const size_t tab = line.find('\t', pos);
      if (tab == std::string::npos) {
        fields.push_back(line.substr(pos));
        break;
      }
      fields.push_back(line.substr(pos, tab - pos));
      pos = tab + 1;
    }
    return fields;
  }

  /** Tabs and newlines would break the format, so they are turned into
   *  spaces rather than escaped. A plugin whose name contains a tab is not
   *  worth an escaping scheme in a file a human is meant to be able to read. */
  static std::string clean(const std::string& value) {
    std::string out = value;
    for (char& c : out)
      if (c == '\t' || c == '\n' || c == '\r') c = ' ';
    return out;
  }

  bool writeTo(const char* path, const std::string& scanning) const {
    if (!path) return false;
    std::FILE* file = std::fopen(path, "wb");
    if (!file) return false;

    std::fprintf(file, "%s\n", kVersion);
    if (!scanning.empty()) std::fprintf(file, "SCANNING\t%s\n", clean(scanning).c_str());
    for (const std::string& entry : blacklist_)
      std::fprintf(file, "BLACKLIST\t%s\n", clean(entry).c_str());
    for (const auto& entry : stamps_)
      std::fprintf(file, "STAMP\t%s\t%llu\t%llu\n", clean(entry.first).c_str(),
                   (unsigned long long) entry.second.size,
                   (unsigned long long) entry.second.modified);
    for (const PluginDescription& d : plugins_)
      std::fprintf(file, "PLUGIN\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%d\n", clean(d.path).c_str(),
                   clean(d.format).c_str(), clean(d.id).c_str(), clean(d.name).c_str(),
                   clean(d.vendor).c_str(), clean(d.version).c_str(), clean(d.features).c_str(),
                   d.isInstrument ? 1 : 0);

    std::fflush(file);
#if defined(_WIN32)
    // fflush hands the bytes to the OS; it does not put them on the platter.
    // For the dead man's switch that distinction is the difference between
    // working and only appearing to.
    const intptr_t handle = _get_osfhandle(_fileno(file));
    if (handle != -1) FlushFileBuffers((HANDLE) handle);
#endif
    std::fclose(file);
    return true;
  }

  std::vector<PluginDescription> plugins_;
  std::vector<std::string> blacklist_;
  std::vector<std::pair<std::string, FileStamp>> stamps_;
  std::string crashedOn_;
};

/**
 * Scan a folder, using and updating a cache.
 *
 * The scan a host actually wants: unchanged files are not reopened, files
 * that killed a previous run are not opened at all, and a file that kills
 * THIS run leaves a mark that the next one will find.
 *
 * `cachePath` may be null, in which case this is the plain scan with no
 * memory and no protection.
 */
inline std::vector<PluginDescription> scanDirectoryCached(const std::string& directory,
                                                          PluginCache& cache,
                                                          const char* cachePath) {
  // Whatever was mid-scan when the last run ended, killed it. There is no
  // other way for that mark to survive: it is cleared on the way out of every
  // successful scan.
  if (!cache.crashedOn().empty()) {
    cache.blacklistPath(cache.crashedOn());
    // Acted on, so forgotten: leaving it set would re-blacklist the same file
    // on every later scan through this object, whatever the user did about it
    // in between.
    cache.clearCrashMark();
    if (cachePath) cache.endScanning(cachePath);
  }

  std::vector<PluginDescription> result;
  // The candidate list comes from the plain scan, which only reads FILE
  // NAMES. Nothing is loaded until the loop below decides to.
  const std::vector<std::string> files = pluginFilesIn(directory);
  for (const std::string& file : files) {
    if (cache.isBlacklisted(file)) continue;

    // isUpToDate, not "did it find anything": a file that was scanned and
    // held nothing must still be remembered as scanned, or every stray DLL in
    // the folder gets reopened on every launch for ever.
    if (cache.isUpToDate(file)) {
      for (const PluginDescription& description : cache.knownFor(file)) result.push_back(description);
      continue;
    }

    if (cachePath) cache.beginScanning(cachePath, file);
    const std::vector<PluginDescription> found = describeFile(file);
    if (cachePath) cache.endScanning(cachePath);

    cache.remember(file, found);
    for (const PluginDescription& description : found) result.push_back(description);
  }
  if (cachePath) cache.save(cachePath);
  return result;
}

/**
 * Save a loaded plugin's state to a preset FILE, in the format that plugin's
 * world uses.
 *
 * VST3 gets a .vstpreset, which is what every other host writes and reads.
 * The other two formats have no such convention: CLAP's preset discovery
 * points at whatever a plugin invents, and LV2 keeps presets as Turtle in a
 * bundle rather than as a file a host writes. For those the state blob is
 * written raw, which round-trips through this SDK and makes no claim to be
 * anything else.
 */
inline bool savePresetFile(const HostedPlugin& plugin, const PluginDescription& description,
                           const char* path) {
  if (!path) return false;
  std::vector<uint8_t> state;
  if (!plugin.saveState(state) || state.empty()) return false;

  if (description.format == "VST3" && description.id.size() == 32)
    return writeVstPreset(path, description.id, state);

  std::FILE* file = std::fopen(path, "wb");
  if (!file) return false;
  const size_t written = std::fwrite(state.data(), 1, state.size(), file);
  std::fclose(file);
  return written == state.size();
}

/**
 * Load a preset file into a plugin.
 *
 * The class id is CHECKED, not assumed. A .vstpreset says which plugin it is
 * for and that is the only thing in the file that does; applying one for a
 * different plugin is how a delay ends up with a filter's numbers in it, and
 * the plugin itself cannot tell because a state blob is opaque bytes.
 */
inline bool loadPresetFile(HostedPlugin& plugin, const PluginDescription& description,
                           const char* path) {
  if (!path) return false;

  if (description.format == "VST3") {
    Vst3PresetFile preset;
    if (!readVstPreset(path, &preset)) return false;
    if (!description.id.empty() && preset.classId != description.id) return false;
    return plugin.loadState(preset.componentState.data(), preset.componentState.size());
  }

  std::FILE* file = std::fopen(path, "rb");
  if (!file) return false;
  std::vector<uint8_t> bytes;
  char buffer[8192];
  size_t got = 0;
  while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0)
    bytes.insert(bytes.end(), buffer, buffer + got);
  std::fclose(file);
  if (bytes.empty()) return false;
  return plugin.loadState(bytes.data(), bytes.size());
}

} // namespace host
} // namespace sonore
