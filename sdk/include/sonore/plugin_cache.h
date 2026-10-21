// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: remembering what a scan found, and what killed it.
//
// Scanning a plugin folder means LOADING SOMEBODY ELSE'S CODE. Every file in
// it gets dlopened, its entry point called, its factory walked. That is slow
// (a full folder is seconds), and it is dangerous, because one plugin that
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
// That trick is the whole reason this can work in-process. The alternative
// (scanning in a child process, as some hosts do) is more robust and costs a
// second binary, an IPC protocol and a lifetime to get wrong. This costs one
// fflush.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <initializer_list>
#include <string>
#include <vector>

#include "host.h"
#include "preset_file.h"
#include "path_io.h"

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
#include <cerrno>
#include <csignal>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace sonore {
namespace host {

/** What a file looked like when it was scanned. Size and modification time
 *  together, because either alone is fooled by something ordinary: a rebuild
 *  that lands on the same size, or an installer that preserves timestamps. */
struct FileStamp {
  /** Bytes: the file's own, or for a bundle folder the sum of its files. */
  uint64_t size = 0;
  /** The last write time, the newest in a bundle: FILETIME units (100 ns
   *  since 1601) on Windows, seconds since 1970 elsewhere. Compared, never
   *  converted, so the units only have to agree with themselves. */
  uint64_t modified = 0;

  /** The same file, as far as a scan can tell: both size and time agree. */
  bool operator==(const FileStamp& other) const {
    return size == other.size && modified == other.modified;
  }
  /** False for the {0, 0} stampOf() gives a path it could not read. */
  bool valid() const { return size != 0 || modified != 0; }
};

/** This process's id, written beside a scan mark so that ANOTHER process
 *  reading the cache can tell a scan still running from one that died. */
inline unsigned long currentProcessId() {
#if defined(_WIN32)
  return (unsigned long) GetCurrentProcessId();
#else
  return (unsigned long) getpid();
#endif
}

/** Whether a process with this id is still running. A reused id answers yes,
 *  which errs the safe way: a plugin is not blacklisted on that evidence. */
inline bool processAlive(unsigned long pid) {
  if (pid == 0) return false;
#if defined(_WIN32)
  HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, (DWORD) pid);
  if (!process) return GetLastError() == ERROR_ACCESS_DENIED; // exists, not ours
  const bool running = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
  CloseHandle(process);
  return running;
#else
  return ::kill((pid_t) pid, 0) == 0 || errno == EPERM;
#endif
}

#if defined(_WIN32)
/** A folder's files, summarised: total size and the newest modification. */
inline FileStamp stampOfTree(const std::string& directory, int depth) {
  FileStamp stamp;
  if (depth < 0) return stamp;
  WIN32_FIND_DATAW fd{};
  const HANDLE found = FindFirstFileW(pathio::widenPath((directory + "\\*").c_str()).c_str(), &fd);
  if (found == INVALID_HANDLE_VALUE) return stamp;
  do {
    if (std::wcscmp(fd.cFileName, L".") == 0 || std::wcscmp(fd.cFileName, L"..") == 0) continue;
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      const FileStamp inner = stampOfTree(directory + "\\" + pathio::narrow(fd.cFileName), depth - 1);
      stamp.size += inner.size;
      if (inner.modified > stamp.modified) stamp.modified = inner.modified;
    } else {
      stamp.size += ((uint64_t) fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
      const uint64_t modified =
          ((uint64_t) fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime;
      if (modified > stamp.modified) stamp.modified = modified;
    }
  } while (FindNextFileW(found, &fd));
  FindClose(found);
  return stamp;
}
#else
/** A folder's files, summarised: total size and the newest modification. */
inline FileStamp stampOfTree(const std::string& directory, int depth) {
  FileStamp stamp;
  if (depth < 0) return stamp;
  DIR* dir = ::opendir(directory.c_str());
  if (!dir) return stamp;
  while (const dirent* entry = ::readdir(dir)) {
    const std::string name = entry->d_name;
    if (name == "." || name == "..") continue;
    const std::string child = directory + "/" + name;
    struct stat st {};
    if (::stat(child.c_str(), &st) != 0) continue;
    if (S_ISDIR(st.st_mode)) {
      const FileStamp inner = stampOfTree(child, depth - 1);
      stamp.size += inner.size;
      if (inner.modified > stamp.modified) stamp.modified = inner.modified;
    } else {
      stamp.size += (uint64_t) st.st_size;
      if ((uint64_t) st.st_mtime > stamp.modified) stamp.modified = (uint64_t) st.st_mtime;
    }
  }
  ::closedir(dir);
  return stamp;
}
#endif

/** What `path` looks like now, a UTF-8 path to a plugin file or bundle. A
 *  bundle folder is stamped by its files, a few levels deep, since its own
 *  size and time do not move when the binary inside is replaced. {0, 0}, not
 *  valid(), for a path that does not exist or cannot be read. */
inline FileStamp stampOf(const std::string& path) {
  FileStamp stamp;
#if defined(_WIN32)
  // WIDE, from UTF-8: the A call reads the path in the ANSI code page, so a
  // plugin under a user folder with an accent in its name stamped {0, 0}, never matched its cache
  // entry, and was loaded -- its code run -- on every single scan.
  WIN32_FILE_ATTRIBUTE_DATA data{};
  bool folder = false;
  if (GetFileAttributesExW(pathio::widenPath(path.c_str()).c_str(), GetFileExInfoStandard, &data)) {
    folder = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    stamp.size = ((uint64_t) data.nFileSizeHigh << 32) | data.nFileSizeLow;
    stamp.modified = ((uint64_t) data.ftLastWriteTime.dwHighDateTime << 32) |
                     data.ftLastWriteTime.dwLowDateTime;
  }
  // A .vst3 or .lv2 is a FOLDER, and a folder's own size is zero and its
  // timestamp does not move when the binary inside it is replaced. Stamping
  // the binary is what actually answers "has this changed".
#if defined(SONORE_HOST_VST3)
  if (stamp.size == 0) {
    const std::string inner = vst3BinaryPath(path);
    if (inner != path &&
        GetFileAttributesExW(pathio::widenPath(inner.c_str()).c_str(), GetFileExInfoStandard, &data)) {
      folder = false;
      stamp.size = ((uint64_t) data.nFileSizeHigh << 32) | data.nFileSizeLow;
      stamp.modified = ((uint64_t) data.ftLastWriteTime.dwHighDateTime << 32) |
                       data.ftLastWriteTime.dwLowDateTime;
    }
  }
#endif // SONORE_HOST_VST3: vst3BinaryPath exists only with the VST3 host, and a
       // CLAP-only host -- which host.h promises compiles -- failed here.
  // Any other folder -- every .lv2 -- by the files in it, as on macOS and
  // Linux. Its own time moves when an entry in it is added, removed or
  // renamed, and not when a file in it is written over, which is how an
  // update replaces a manifest and a binary: the stamp stood still, the
  // cache answered for the old bundle, and the plugins the update added
  // were never listed. The folder's own time is kept as a floor, so an
  // entry going away still counts and an empty bundle still has a stamp.
  if (folder) {
    const FileStamp files = stampOfTree(path, 4);
    stamp.size = files.size;
    if (files.modified > stamp.modified) stamp.modified = files.modified;
  }
#else
  struct stat st {};
  if (::stat(path.c_str(), &st) == 0) {
    if (S_ISDIR(st.st_mode)) {
      // A BUNDLE -- every .vst3 and .lv2 here, and a .clap on macOS. The
      // folder's own size and time do not move when the binary inside is
      // replaced, so an updated plugin matched its stale entry and was never
      // rescanned. Its files are what changed: their sizes summed and the
      // newest time among them, a few levels deep, which reaches the binary
      // in every bundle layout there is.
      stamp = stampOfTree(path, 4);
    } else {
      stamp.size = (uint64_t) st.st_size;
      stamp.modified = (uint64_t) st.st_mtime;
    }
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

  /** As written to the file, too: a path is saved with its tabs and line
   *  breaks turned into spaces (see clean), and the dead man's mark of a
   *  plugin named with one -- legal outside Windows -- came back as a path
   *  that matched nothing, so the plugin that killed the host was loaded,
   *  and killed it, on every start. */
  bool isBlacklisted(const std::string& path) const {
    const std::string written = clean(path);
    for (const std::string& entry : blacklist_)
      if (entry == path || entry == written) return true;
    return false;
  }

  /** Never load `path` again, until forgive(). Listed once however often
   *  it is called; saved with the cache. */
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

  /** Forget everything: plugins, stamps, the blacklist and the crash mark.
   *  The file on disk is untouched until the next save(). */
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

  /**
   * Take one plugin off the list, as a user does from a plugin list.
   *
   * The file's stamp goes with it. Left behind, the stamp would say the file
   * was scanned and is unchanged, so the next scan would trust the cache's
   * answer for it -- which no longer includes the plugin -- and the user
   * could never get it back short of deleting the cache. Without the stamp
   * the file is simply new again, and a scan that reaches it finds what is
   * in it, the other plugins it holds included.
   *
   * Returns whether there was such a plugin.
   */
  bool removePlugin(const std::string& path, const std::string& id) {
    bool removed = false;
    for (size_t i = plugins_.size(); i-- > 0;)
      if (plugins_[i].path == path && plugins_[i].id == id) {
        plugins_.erase(plugins_.begin() + (ptrdiff_t) i);
        removed = true;
      }
    if (!removed) return false;
    // The file's other plugins stay listed. They are still in it; only the
    // claim that the file needs no rescan has been withdrawn.
    for (size_t i = stamps_.size(); i-- > 0;)
      if (stamps_[i].first == path) stamps_.erase(stamps_.begin() + (ptrdiff_t) i);
    return true;
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

  /** Replace what this cache holds with the file at `path`, UTF-8. A scan
   *  mark left by a run that died, or by this process, becomes crashedOn();
   *  one from another process still running is a scan in progress, not a
   *  crash, and is passed over. False, and
   *  the cache left empty, for a missing file or one of another version: the
   *  next scan rebuilds it. */
  bool load(const char* path) {
    clear();
    if (!path) return false;
    std::FILE* file = ::sonore::pathio::open(path, "rb");
    if (!file) return false;

    std::string text;
    char buffer[8192];
    size_t got = 0;
    while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0) text.append(buffer, got);
    std::fclose(file);

    // A byte-order mark, which a text editor may add when somebody edits the
    // blacklist by hand -- as the format invites. Read as part of the version
    // line, it threw the whole cache away, blacklist included.
    size_t pos = text.compare(0, 3, "\xEF\xBB\xBF") == 0 ? 3 : 0;
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
        // A mark left by ANOTHER process that is still running is a scan in
        // progress, not a crash. Taken as one, a second host process started
        // mid-scan blacklisted the plugin the first was loading -- for good.
        // A mark with no process id is from before ids were written, and is
        // read as it always was.
        const unsigned long pid = fields.size() >= 3 ? std::strtoul(fields[2].c_str(), nullptr, 10) : 0;
        if (pid == 0 || pid == currentProcessId() || !processAlive(pid)) crashedOn_ = fields[1];
      } else if (fields[0] == "BLACKLIST" && fields.size() >= 2) {
        blacklist_.push_back(fields[1]);
      } else if (fields[0] == "STAMP" && fields.size() >= 4) {
        FileStamp stamp;
        stamp.size = (uint64_t) std::strtoull(fields[2].c_str(), nullptr, 10);
        stamp.modified = (uint64_t) std::strtoull(fields[3].c_str(), nullptr, 10);
        // One stamp per file, the last line read winning, as it was written
        // last. A cache written twice over or edited by hand can hold two,
        // and the lookups disagreed about which counted: isUpToDate took the
        // first and knownFor the last. With a good stamp before a stale one
        // the file was "up to date" and known to hold nothing, so the scan
        // trusted the cache and the plugin was hidden for good.
        bool replaced = false;
        for (auto& entry : stamps_)
          if (entry.first == fields[1]) {
            entry.second = stamp;
            replaced = true;
          }
        if (!replaced) stamps_.push_back({fields[1], stamp});
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

  /** Write the cache to `path`, UTF-8, with no scan mark of this process's:
   *  to a temporary file, flushed to the disk, then moved over the old one,
   *  so a crash mid-write leaves the previous cache whole. A scan mark of
   *  another process still in the file is kept. False when it could not be
   *  written. */
  bool save(const char* path) const { return writeTo(path, std::string()); }

  /**
   * Write the cache with `path` marked as in progress, and FLUSH IT.
   *
   *  This is the dead man's switch, and the flush is the whole point: a
   *  buffered write that is still in memory when the plugin faults tells the
   *  next run nothing. It has to be on the disk before the file is opened.
   *
   *  Costs one rewrite of the cache per plugin scanned -- and a flush to the
   *  disk is not cheap: about 10 ms on an SSD here, where loading and
   *  describing the plugin took 4. So scanFilesCached makes no other write
   *  per plugin than this one (see there).
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

  /**
   * The scan marks ANOTHER process has in the cache file now, as written.
   *
   * Every write replaces the whole file from this object's memory, and two
   * processes scanning one cache -- two hosts started together -- each write
   * before and after every plugin they open. So the second one's next write
   * erased the first one's mark: a plugin that killed the first process
   * while the second scanned on left no trace, and killed it again on the
   * next start. Another process's mark is carried into each write while
   * that process lives, and after it has died until the plugin it names is
   * blacklisted -- acted on, by whichever process loads the cache next.
   * What is left is the instant between reading the file and moving the new
   * one over it, where two writes can still cross.
   */
  std::vector<std::string> marksOfOthers(const std::string& text) const {
    std::vector<std::string> kept;
    for (size_t pos = 0; pos < text.size();) {
      size_t end = text.find('\n', pos);
      if (end == std::string::npos) end = text.size();
      std::string line = text.substr(pos, end - pos);
      pos = end + 1;
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (line.compare(0, 9, "SCANNING\t") != 0) continue;
      const std::vector<std::string> fields = split(line);
      if (fields.size() < 3) continue; // no process id: read as a crash, as ever
      const unsigned long pid = std::strtoul(fields[2].c_str(), nullptr, 10);
      if (pid == 0 || pid == currentProcessId()) continue; // this process's own
      if (!processAlive(pid) && isBlacklisted(fields[1])) continue;
      kept.push_back(line);
    }
    return kept;
  }

  /** The cache file's bytes as they are now; empty when there is none. */
  static std::string fileText(const char* path) {
    std::string text;
    std::FILE* file = ::sonore::pathio::open(path, "rb");
    if (!file) return text;
    char buffer[8192];
    size_t got = 0;
    while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0) text.append(buffer, got);
    std::fclose(file);
    return text;
  }

  /** Written to a temporary of this process's own and moved over the cache,
   *  as user_settings.h writes: truncating the cache in place left a reader in
   *  another process a half-written file, and a failed write -- a full disk
   *  -- an empty one, with the blacklist gone and true returned.
   *
   *  Not written at all when the file already holds exactly these bytes. The
   *  write is a flush to the disk, about 10 ms, and a scan that found every
   *  file up to date paid one at its end to put back what was there: half of
   *  what such a scan -- the one every start of a host makes -- cost. */
  bool writeTo(const char* path, const std::string& scanning) const {
    if (!path) return false;
    const std::string existing = fileText(path);
    const std::vector<std::string> others = marksOfOthers(existing);

    // The bytes the printf-per-line form wrote, each %s cut at a NUL as
    // printf cuts it, so a file this writes compares equal to one it wrote.
    std::string text;
    auto line = [&text](std::initializer_list<const char*> fields) {
      bool first = true;
      for (const char* field : fields) {
        if (!first) text += '\t';
        text += field;
        first = false;
      }
      text += '\n';
    };
    char number[2][32];
    line({kVersion});
    if (!scanning.empty()) {
      std::snprintf(number[0], sizeof(number[0]), "%lu", currentProcessId());
      line({"SCANNING", clean(scanning).c_str(), number[0]});
    }
    for (const std::string& mark : others) line({mark.c_str()});
    for (const std::string& entry : blacklist_) line({"BLACKLIST", clean(entry).c_str()});
    for (const auto& entry : stamps_) {
      std::snprintf(number[0], sizeof(number[0]), "%llu", (unsigned long long) entry.second.size);
      std::snprintf(number[1], sizeof(number[1]), "%llu", (unsigned long long) entry.second.modified);
      line({"STAMP", clean(entry.first).c_str(), number[0], number[1]});
    }
    for (const PluginDescription& d : plugins_)
      line({"PLUGIN", clean(d.path).c_str(), clean(d.format).c_str(), clean(d.id).c_str(),
            clean(d.name).c_str(), clean(d.vendor).c_str(), clean(d.version).c_str(),
            clean(d.features).c_str(), d.isInstrument ? "1" : "0"});
    if (text == existing) return true;

    static std::atomic<unsigned> sequence{0};
    char suffix[64];
    std::snprintf(suffix, sizeof(suffix), ".tmp-%lu-%u", currentProcessId(), sequence.fetch_add(1));
    const std::string temp = std::string(path) + suffix;
    std::FILE* file = ::sonore::pathio::open(temp.c_str(), "wb");
    if (!file) return false;
    std::fwrite(text.data(), 1, text.size(), file);

    bool wrote = !std::ferror(file);
    wrote = std::fflush(file) == 0 && wrote;
#if defined(_WIN32)
    // fflush hands the bytes to the OS; it does not put them on the platter.
    // For the dead man's switch that distinction is the difference between
    // working and only appearing to.
    const intptr_t handle = _get_osfhandle(_fileno(file));
    if (handle != -1) wrote = FlushFileBuffers((HANDLE) handle) != 0 && wrote;
#else
    wrote = fsync(fileno(file)) == 0 && wrote;
#endif
    wrote = std::fclose(file) == 0 && wrote;
    if (!wrote) {
      ::sonore::pathio::remove(temp.c_str());
      return false;
    }
#if defined(_WIN32)
    const std::wstring from = pathio::widenPath(temp.c_str()), to = pathio::widenPath(path);
    bool moved = false;
    for (int attempt = 0; attempt < 50 && !moved; ++attempt) {
      moved = MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
      if (!moved) {
        const DWORD error = GetLastError();
        if (error != ERROR_ACCESS_DENIED && error != ERROR_SHARING_VIOLATION) break;
        Sleep(1); // another process replacing or reading it, for an instant
      }
    }
#else
    const bool moved = std::rename(temp.c_str(), path) == 0;
#endif
    if (!moved) ::sonore::pathio::remove(temp.c_str());
    return moved;
  }

  std::vector<PluginDescription> plugins_;
  std::vector<std::string> blacklist_;
  std::vector<std::pair<std::string, FileStamp>> stamps_;
  std::string crashedOn_;
};

inline std::vector<PluginDescription> scanFilesCached(const std::vector<std::string>& files,
                                                      PluginCache& cache, const char* cachePath);

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
  // The candidate list comes from the plain scan, which only reads FILE
  // NAMES. Nothing is loaded until scanFilesCached decides to.
  return scanFilesCached(pluginFilesIn(directory), cache, cachePath);
}

/**
 * The same protection for any list of files -- the ones a format other than
 * the bundle formats finds, such as LADSPAPluginFormat::filesIn, or a file a
 * user dropped on a plugin list.
 *
 * describeFile decides what each file is, so one list may mix formats.
 */
inline std::vector<PluginDescription> scanFilesCached(const std::vector<std::string>& files,
                                                      PluginCache& cache, const char* cachePath) {
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
  for (const std::string& file : files) {
    if (cache.isBlacklisted(file)) continue;

    // isUpToDate, not "did it find anything": a file that was scanned and
    // held nothing must still be remembered as scanned, or every stray DLL in
    // the folder gets reopened on every launch for ever.
    //
    // Its plugins straight from the list, not through knownFor(), which
    // stamps the file again to answer the question just answered: a second
    // look at every file's size and time, half of what a scan of a folder
    // that has not changed -- every start of a host -- cost.
    if (cache.isUpToDate(file)) {
      for (const PluginDescription& description : cache.plugins())
        if (description.path == file) result.push_back(description);
      continue;
    }

    // ONE write per file: the mark, which must be on the disk before the
    // file's code runs. The write that cleared it again afterwards is folded
    // into the next one -- the next file's mark, or the save below -- which
    // replaces it with the same cache plus this file's result. That clearing
    // write was half the writes of a scan, and each is a flush to the disk:
    // 81 of them for 40 plugins took 700 of a cold scan's 863 ms, where
    // loading and describing all 40 took 158. Between here and the next
    // write the mark names a file whose code has finished; what runs there
    // is this loop's own stamping of files, as the clearing write's own
    // flush used to be.
    if (cachePath) cache.beginScanning(cachePath, file);
    const std::vector<PluginDescription> found = describeFile(file);

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
 *
 * An Audio Unit's state blob is its ClassInfo as an XML property list
 * (host_au.h), which is exactly what an .aupreset file holds -- so for AU the
 * raw write IS the format's own preset file, and loading one written by any
 * other AU host works the same way. The unit itself checks the
 * type/subtype/manufacturer inside and refuses another unit's preset.
 */
inline bool savePresetFile(const HostedPlugin& plugin, const PluginDescription& description,
                           const char* path) {
  if (!path) return false;
  std::vector<uint8_t> state;
  if (!plugin.saveState(state) || state.empty()) return false;

  if (description.format == "VST3" && description.id.size() == 32)
    return writeVstPreset(path, description.id, state);

  std::FILE* file = ::sonore::pathio::open(path, "wb");
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

  std::FILE* file = ::sonore::pathio::open(path, "rb");
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
