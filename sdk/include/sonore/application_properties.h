// SPDX-License-Identifier: Apache-2.0
//
// An application's settings files: one for the user, one for the machine.
//
// ── What it is for ──────────────────────────────────────────────────────────
//
// user_settings.h already keeps a plugin's per-user preferences, keyed by
// plugin id and stored as a StateBag. An APPLICATION -- the standalone, a
// tool, an editor that owns its process -- wants the reference's arrangement
// instead: a file named after the application, in the place each OS expects
// one, a second file shared by every user of the machine, and reads that fall
// through from the user's file to the shared one. A licence path set by the
// installer for everyone, overridden per user, is the ordinary case.
//
// ── Where the files go ──────────────────────────────────────────────────────
//
//   Windows  %APPDATA%\<folder>\<app><suffix>        (common: %ProgramData%)
//   macOS    ~/Library/<sub>/<folder>/<app><suffix>  (common: /Library/...)
//            <sub> is "Application Support" or "Preferences", as Apple asks
//   Linux    $XDG_CONFIG_HOME or ~/.config/<folder>/<app><suffix>
//            (common: the first of $XDG_CONFIG_DIRS, or /etc/xdg)
//
// <folder> is the folder name if one is given, else the application name --
// except on macOS, where no folder name means directly in <sub>, as the
// reference does it. The machine-wide file is usually read-only for an
// ordinary user; getCommonSettings(true) notices and hands back the user's
// file instead, so a caller never has to.
//
// ── Two formats ─────────────────────────────────────────────────────────────
//
// XML in the reference's own shape -- <PROPERTIES> holding <VALUE name val>
// elements -- so a file an older application wrote is read, and a user can
// fix one in a text editor. Or binary: the same StateBag encoding every
// session blob here uses, behind a magic so the reader can tell. Either is
// read whatever the options say; the option decides what is WRITTEN.
//
// ── Saving later, and the clock ────────────────────────────────────────────
//
// A change marks the file dirty and saves it millisecondsBeforeSaving after
// the LAST change -- a slider dragged for two seconds writes once, not sixty
// times. The wait is driven by advance(elapsedMs), the same rule every timed
// thing in this SDK follows: no timer thread inside a settings object, and a
// test of "saves after 3 s" that runs in no time at all. Zero saves on every
// change; negative only on save()/saveIfNeeded(), and the destructor saves
// whatever is still dirty.
//
// ── Two processes, one file ────────────────────────────────────────────────
//
// The machine-wide file is written by more than one process -- two users,
// two copies of the application. With the process lock on (always, for the
// common file) every read and write holds an OS file lock on "<file>.lock"
// beside it: LockFileEx on Windows, flock elsewhere. The lock dies with its
// process, so a crash never leaves the file locked. The write itself goes
// through a temporary and a rename, like UserSettings.
//
// MAIN THREAD for saving and loading -- they open files. Reading and setting
// values is safe from any thread (a lock guards the values).
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "broadcaster.h"
#include "number_text.h"
#include "path_io.h"
#include "state_bag.h"
#include "user_settings.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace sonore {
namespace detail {
namespace props {

using Values = std::vector<std::pair<std::string, std::string>>;

/** The binary format's first bytes. StateBag's own encoding starts with a
 *  count, and 60 entries begin with '<' -- so without a magic a binary file
 *  could be mistaken for XML. */
constexpr char kBinaryMagic[8] = {'S', 'N', 'P', 'R', 'O', 'P', 'S', '1'};

/** Big enough for any settings file, small enough that a wrong path pointing
 *  at something enormous is not read into memory. */
constexpr size_t kMaxFileBytes = (size_t) 16 * 1024 * 1024;

/** How long a read or a write waits for another process's lock. */
constexpr int kLockTimeoutMs = 1000;

inline char asciiLower(char c) { return (c >= 'A' && c <= 'Z') ? (char) (c - 'A' + 'a') : c; }

inline bool keysEqual(const std::string& a, const std::string& b, bool ignoreCase) {
  if (a.size() != b.size()) return false;
  if (!ignoreCase) return a == b;
  for (size_t i = 0; i < a.size(); ++i)
    if (asciiLower(a[i]) != asciiLower(b[i])) return false;
  return true;
}

/** A name both formats can carry: not empty, and no NUL (the binary form's
 *  keys are C strings, and XML has no way to write one). */
inline bool isStorableName(const std::string& name) {
  return !name.empty() && name.find('\0') == std::string::npos;
}

/**
 * What either reader hands back: unstorable names dropped, and a name that
 * appears twice kept once, with the LATER value, where it first appeared --
 * so whatever was read is exactly what a save writes and a reload reads.
 */
inline void normalise(Values& values) {
  Values out;
  out.reserve(values.size());
  for (auto& entry : values) {
    if (!isStorableName(entry.first)) continue;
    bool replaced = false;
    for (auto& kept : out)
      if (kept.first == entry.first) {
        kept.second = std::move(entry.second);
        replaced = true;
        break;
      }
    if (!replaced) out.push_back(std::move(entry));
  }
  values.swap(out);
}

inline void appendUtf8(std::string& out, uint32_t cp) {
  if (cp < 0x80) {
    out.push_back((char) cp);
  } else if (cp < 0x800) {
    out.push_back((char) (0xC0 | (cp >> 6)));
    out.push_back((char) (0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out.push_back((char) (0xE0 | (cp >> 12)));
    out.push_back((char) (0x80 | ((cp >> 6) & 0x3F)));
    out.push_back((char) (0x80 | (cp & 0x3F)));
  } else {
    out.push_back((char) (0xF0 | (cp >> 18)));
    out.push_back((char) (0x80 | ((cp >> 12) & 0x3F)));
    out.push_back((char) (0x80 | ((cp >> 6) & 0x3F)));
    out.push_back((char) (0x80 | (cp & 0x3F)));
  }
}

/** An attribute value as XML wants it. Newlines and tabs as character
 *  references: written literally, any XML reader turns them into spaces,
 *  and a multi-line value would come back as one line. */
inline void appendXmlEscaped(std::string& out, const std::string& text) {
  for (char ch : text) {
    const unsigned char c = (unsigned char) ch;
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&apos;"; break;
      default:
        if (c < 0x20) {
          char ref[8];
          std::snprintf(ref, sizeof(ref), "&#%u;", (unsigned) c);
          out += ref;
        } else {
          out.push_back(ch);
        }
    }
  }
}

/** Entity and character references back to text. Anything that is not a
 *  well-formed reference is kept as it was rather than dropped. */
inline std::string decodeXmlText(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] != '&') {
      out.push_back(text[i]);
      continue;
    }
    const size_t semi = text.find(';', i + 1);
    if (semi == std::string::npos || semi - i > 12) {
      out.push_back('&');
      continue;
    }
    const std::string name = text.substr(i + 1, semi - i - 1);
    if (name == "amp") out.push_back('&');
    else if (name == "lt") out.push_back('<');
    else if (name == "gt") out.push_back('>');
    else if (name == "quot") out.push_back('"');
    else if (name == "apos") out.push_back('\'');
    else if (name.size() >= 2 && name[0] == '#') {
      const bool hex = name[1] == 'x' || name[1] == 'X';
      uint32_t cp = 0;
      bool ok = name.size() > (hex ? 2u : 1u);
      for (size_t k = hex ? 2 : 1; k < name.size() && ok; ++k) {
        const char d = name[k];
        uint32_t digit = 0;
        if (d >= '0' && d <= '9') digit = (uint32_t) (d - '0');
        else if (hex && d >= 'a' && d <= 'f') digit = (uint32_t) (d - 'a' + 10);
        else if (hex && d >= 'A' && d <= 'F') digit = (uint32_t) (d - 'A' + 10);
        else ok = false;
        cp = cp * (hex ? 16u : 10u) + digit;
        if (cp > 0x10FFFF) ok = false;
      }
      if (!ok || cp == 0 || (cp >= 0xD800 && cp <= 0xDFFF)) {
        out.push_back('&');
        continue;
      }
      appendUtf8(out, cp);
    } else {
      out.push_back('&');
      continue;
    }
    i = semi;
  }
  return out;
}

inline std::string writeXml(const Values& values) {
  std::string out = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n\n<PROPERTIES>\n";
  for (const auto& entry : values) {
    out += "  <VALUE name=\"";
    appendXmlEscaped(out, entry.first);
    out += "\" val=\"";
    appendXmlEscaped(out, entry.second);
    out += "\"/>\n";
  }
  out += "</PROPERTIES>\n";
  return out;
}

inline bool isXmlSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

/**
 * Read the reference's properties XML: a <PROPERTIES> element holding
 * <VALUE name="..." val="..."/> elements.
 *
 * Not a general XML parser and not trying to be: it finds that one shape,
 * skips comments and anything else, and is strict about the frame -- a file
 * cut off before </PROPERTIES> is reported as broken rather than loaded half
 * way. A VALUE with element content instead of a val attribute (the
 * reference stores nested XML that way) keeps the content as raw text, so a
 * save writes back what was read. Untrusted input: see fuzz_app_infra.cpp.
 */
inline bool parseXml(const std::string& text, Values& out) {
  out.clear();
  size_t pos = 0;
  // Find the root, past a BOM, the prolog and any comments.
  for (;;) {
    pos = text.find('<', pos);
    if (pos == std::string::npos) return false;
    if (text.compare(pos, 4, "<!--") == 0) {
      const size_t end = text.find("-->", pos + 4);
      if (end == std::string::npos) return false;
      pos = end + 3;
      continue;
    }
    if (text.compare(pos, 11, "<PROPERTIES") == 0 &&
        (pos + 11 >= text.size() || isXmlSpace(text[pos + 11]) || text[pos + 11] == '>' ||
         text[pos + 11] == '/'))
      break;
    ++pos;
  }
  const size_t rootClose = text.find('>', pos);
  if (rootClose == std::string::npos) return false;
  if (text[rootClose - 1] == '/') return true; // <PROPERTIES/>: empty, and complete
  pos = rootClose + 1;

  for (;;) {
    pos = text.find('<', pos);
    if (pos == std::string::npos) return false; // no </PROPERTIES>: truncated
    if (text.compare(pos, 13, "</PROPERTIES>") == 0) {
      normalise(out);
      return true;
    }
    if (text.compare(pos, 4, "<!--") == 0) {
      const size_t end = text.find("-->", pos + 4);
      if (end == std::string::npos) return false;
      pos = end + 3;
      continue;
    }
    const bool isValue = text.compare(pos, 6, "<VALUE") == 0 && pos + 6 < text.size() &&
                         (isXmlSpace(text[pos + 6]) || text[pos + 6] == '/' || text[pos + 6] == '>');
    if (!isValue) {
      const size_t skip = text.find('>', pos + 1);
      if (skip == std::string::npos) return false;
      pos = skip + 1;
      continue;
    }

    // Attributes, up to "/>" or ">".
    size_t p = pos + 6;
    std::string name, val;
    bool hasName = false, hasVal = false, selfClosing = false, closed = false;
    while (p < text.size()) {
      while (p < text.size() && isXmlSpace(text[p])) ++p;
      if (p >= text.size()) break;
      if (text[p] == '/' && p + 1 < text.size() && text[p + 1] == '>') {
        selfClosing = closed = true;
        p += 2;
        break;
      }
      if (text[p] == '>') {
        closed = true;
        ++p;
        break;
      }
      const size_t nameStart = p;
      while (p < text.size() && !isXmlSpace(text[p]) && text[p] != '=' && text[p] != '>' &&
             text[p] != '/')
        ++p;
      const std::string attribute = text.substr(nameStart, p - nameStart);
      while (p < text.size() && isXmlSpace(text[p])) ++p;
      if (p >= text.size() || text[p] != '=') {
        if (attribute.empty()) return false; // a stray character: not XML
        continue;                            // an attribute without a value
      }
      ++p;
      while (p < text.size() && isXmlSpace(text[p])) ++p;
      if (p >= text.size() || (text[p] != '"' && text[p] != '\'')) return false;
      const char quote = text[p++];
      const size_t valueEnd = text.find(quote, p);
      if (valueEnd == std::string::npos) return false;
      const std::string raw = text.substr(p, valueEnd - p);
      p = valueEnd + 1;
      if (attribute == "name") {
        name = decodeXmlText(raw);
        hasName = true;
      } else if (attribute == "val") {
        val = decodeXmlText(raw);
        hasVal = true;
      }
    }
    if (!closed) return false;
    if (!selfClosing) {
      const size_t end = text.find("</VALUE>", p);
      if (end == std::string::npos) return false;
      if (!hasVal) {
        // Element content: kept raw, trimmed of the indentation around it.
        size_t a = p, b = end;
        while (a < b && isXmlSpace(text[a])) ++a;
        while (b > a && isXmlSpace(text[b - 1])) --b;
        val = text.substr(a, b - a);
        hasVal = true;
      }
      p = end + 8;
    }
    // A repeated name: the later one wins (normalise, at the end), as a
    // reload of a hand-edited file would expect.
    if (hasName) out.emplace_back(std::move(name), std::move(val));
    pos = p;
  }
}

inline std::vector<uint8_t> writeBinary(const Values& values) {
  StateBag bag;
  for (const auto& entry : values)
    if (isStorableName(entry.first)) bag.setString(entry.first.c_str(), entry.second);
  std::vector<uint8_t> out(kBinaryMagic, kBinaryMagic + sizeof(kBinaryMagic));
  bag.serialise(out);
  return out;
}

inline bool isBinary(const std::vector<uint8_t>& bytes) {
  return bytes.size() >= sizeof(kBinaryMagic) &&
         std::equal(kBinaryMagic, kBinaryMagic + sizeof(kBinaryMagic), (const char*) bytes.data());
}

inline bool parseBinary(const std::vector<uint8_t>& bytes, Values& out) {
  out.clear();
  if (!isBinary(bytes)) return false;
  StateBag bag;
  size_t consumed = 0;
  if (!bag.deserialise(bytes.data() + sizeof(kBinaryMagic), bytes.size() - sizeof(kBinaryMagic),
                       &consumed))
    return false;
  for (size_t i = 0; i < bag.size(); ++i) out.emplace_back(bag.keyAt(i), bag.valueAt(i));
  normalise(out);
  return true;
}

/** Either format, decided by the bytes rather than by the options. */
inline bool parseAny(const std::vector<uint8_t>& bytes, Values& out) {
  if (isBinary(bytes)) return parseBinary(bytes, out);
  return parseXml(std::string(bytes.begin(), bytes.end()), out);
}

inline bool readWholeFile(const std::string& path, std::vector<uint8_t>& out) {
  out.clear();
  std::FILE* f = pathio::open(path.c_str(), "rb");
  if (!f) return false;
  uint8_t chunk[8192];
  size_t got = 0;
  bool ok = true;
  while ((got = std::fread(chunk, 1, sizeof(chunk), f)) > 0) {
    out.insert(out.end(), chunk, chunk + got);
    if (out.size() > kMaxFileBytes) {
      ok = false;
      break;
    }
  }
  if (std::ferror(f)) ok = false;
  std::fclose(f);
  return ok;
}

inline bool fileIsThere(const std::string& path) {
#if defined(_WIN32)
  return GetFileAttributesW(pathio::widenPath(path.c_str()).c_str()) != INVALID_FILE_ATTRIBUTES;
#else
  struct stat st {};
  return ::stat(path.c_str(), &st) == 0;
#endif
}

/** A name a filesystem will take: separators and the characters Windows
 *  reserves become '_', and "." / ".." are not names at all. */
inline std::string safeComponent(const std::string& name) {
  std::string out;
  for (char c : name) {
    const unsigned char u = (unsigned char) c;
    const bool bad = u < 0x20 || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
                     c == '"' || c == '<' || c == '>' || c == '|';
    out.push_back(bad ? '_' : c);
  }
  if (out == "." || out == "..") out = "_";
  return out;
}

inline char separator() {
#if defined(_WIN32)
  return '\\';
#else
  return '/';
#endif
}

/** A folder name may be nested ("Company/App"): each part is cleaned on its
 *  own, and a part that would climb out ("..") is dropped. */
inline std::string safeFolder(const std::string& folder) {
  std::string out, part;
  auto flush = [&]() {
    if (!part.empty() && part != "." && part != "..") {
      if (!out.empty()) out.push_back(separator());
      out += safeComponent(part);
    }
    part.clear();
  };
  for (char c : folder) {
    if (c == '/' || c == '\\') flush();
    else part.push_back(c);
  }
  flush();
  return out;
}

/**
 * An OS file lock, held for as long as this object lives.
 *
 * On "<file>.lock", not on the settings file itself: the settings file is
 * REPLACED by every save (a rename over it), and a lock on the old one would
 * guard nothing. The lock file is never deleted -- deleting a lock file is a
 * race of its own -- and an OS lock vanishes with its process.
 */
class FileLock {
public:
  FileLock(const std::string& path, int timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
#if defined(_WIN32)
    const std::wstring wide = pathio::widenPath(path.c_str());
    const DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    handle_ = CreateFileW(wide.c_str(), GENERIC_READ | GENERIC_WRITE, share, nullptr, OPEN_ALWAYS,
                          FILE_ATTRIBUTE_NORMAL, nullptr);
    // A read-only folder (the machine-wide one, for an ordinary user) cannot
    // have a lock file made in it, but may have one the installer made.
    if (handle_ == INVALID_HANDLE_VALUE)
      handle_ = CreateFileW(wide.c_str(), GENERIC_READ, share, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle_ == INVALID_HANDLE_VALUE) {
      handle_ = nullptr;
      return;
    }
    for (;;) {
      OVERLAPPED overlapped{};
      if (LockFileEx(handle_, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0,
                     &overlapped)) {
        locked_ = true;
        return;
      }
      if (GetLastError() != ERROR_LOCK_VIOLATION) return;
      if (std::chrono::steady_clock::now() >= deadline) return;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
#else
    fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0666);
    if (fd_ < 0) fd_ = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd_ < 0) return;
    for (;;) {
      if (flock(fd_, LOCK_EX | LOCK_NB) == 0) {
        locked_ = true;
        return;
      }
      if (errno != EWOULDBLOCK && errno != EINTR) return;
      if (std::chrono::steady_clock::now() >= deadline) return;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
#endif
  }

  ~FileLock() {
#if defined(_WIN32)
    if (locked_) {
      OVERLAPPED overlapped{};
      UnlockFileEx(handle_, 0, 1, 0, &overlapped);
    }
    if (handle_) CloseHandle(handle_);
#else
    if (locked_) flock(fd_, LOCK_UN);
    if (fd_ >= 0) ::close(fd_);
#endif
  }

  /** Whether the lock FILE could be opened at all. Not being able to is a
   *  read-only folder with no lock file in it, which a reader may proceed
   *  without; failing to LOCK an open file means somebody else holds it. */
  bool opened() const {
#if defined(_WIN32)
    return handle_ != nullptr;
#else
    return fd_ >= 0;
#endif
  }

  bool isLocked() const { return locked_; }

  FileLock(const FileLock&) = delete;
  FileLock& operator=(const FileLock&) = delete;

private:
#if defined(_WIN32)
  HANDLE handle_ = nullptr;
#else
  int fd_ = -1;
#endif
  bool locked_ = false;
};

} // namespace props
} // namespace detail

/**
 * One settings file: string values by key, loaded on construction, saved
 * after a delay, on demand, and on destruction.
 *
 * Values are text, as in the reference, with typed getters and setters on
 * top. A getter for a key this file lacks asks the fallback file, if one is
 * set -- that is how ApplicationProperties makes the user's file read through
 * to the machine's. Every change is also a ChangeBroadcaster change, so a
 * preferences page can redraw on the next tick.
 */
class PropertiesFile : public ChangeBroadcaster {
public:
  /** How save() writes the file. Loading reads either, whichever is set. */
  enum class StorageFormat {
    storeAsBinary, ///< compact, behind a magic header; not for editing by hand
    storeAsXML     ///< an XML file a person can read and edit
  };

  /** Where the file lives and how it behaves. */
  struct Options {
    /** Names the file. Required: an empty one gives no file at all. */
    std::string applicationName;
    /** Appended to the application name, with a dot if it has none. */
    std::string filenameSuffix = ".settings";
    /** The folder the file goes in. Empty means the application name
     *  (directly in the library folder, on macOS). May be nested: "Co/App". */
    std::string folderName;
    /** macOS only: "Application Support" or "Preferences" under ~/Library.
     *  Anything else gives no file, as the reference refuses it. */
    std::string osxLibrarySubFolder = "Application Support";
    /** The machine-wide location rather than the user's. */
    bool commonToAllUsers = false;
    /** "Volume" and "volume" are the same key. */
    bool ignoreCaseOfKeyNames = false;
    /** Never write: a read-only view of somebody else's settings. */
    bool doNotSave = false;
    /** See the header: >0 waits that long after the LAST change (driven by
     *  advance()), 0 saves on every change, <0 only when asked. */
    int millisecondsBeforeSaving = 3000;
    /** How save() writes the file; XML unless changed. */
    StorageFormat storageFormat = StorageFormat::storeAsXML;
    /** Hold an OS file lock around every read and write, for a file more
     *  than one process writes. ApplicationProperties turns it on for the
     *  machine-wide file. */
    bool useProcessLock = false;

    /** The full path these options name, or empty when there is nowhere --
     *  no application name, an unset home, a macOS sub-folder Apple would
     *  not accept. */
    std::string getDefaultFile() const;
  };

  /** The file the options name, loaded now. */
  explicit PropertiesFile(const Options& options) : PropertiesFile(options.getDefaultFile(), options) {}

  /** A file at an explicit path, with these options' behaviour. */
  PropertiesFile(std::string file, const Options& options)
      : file_(std::move(file)), options_(options) {
    reload();
  }

  /** Saves whatever is still dirty. */
  ~PropertiesFile() override { saveIfNeeded(); }

  ///@{
  /** Not copyable: two copies would save over each other. */
  PropertiesFile(const PropertiesFile&) = delete;
  PropertiesFile& operator=(const PropertiesFile&) = delete;
  ///@}

  /** The file's path; empty when the options named nowhere, and then
   *  nothing is loaded or saved. */
  const std::string& getFile() const { return file_; }
  /** The options it was made with. */
  const Options& getOptions() const { return options_; }

  /** True when the file loaded, or did not exist yet (a first run). False
   *  when it was there and could not be read or parsed. */
  bool isValidFile() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return loadedOk_;
  }

  // ── Values ────────────────────────────────────────────────────────────────

  /** The value for `key` in this file, else in the fallback file, else
   *  `fallback`. Reading and setting values is safe from any thread; saving
   *  and loading open files and belong on the main thread. */
  std::string getValue(const std::string& key, const std::string& fallback = std::string()) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (const std::string* found = findLocked(key)) return *found;
    return fallbackSet_ ? fallbackSet_->getValue(key, fallback) : fallback;
  }

  /** The value as a whole number, or `fallback` if it is missing or is not
   *  one. */
  int getIntValue(const std::string& key, int fallback = 0) const {
    const std::string text = getValue(key);
    if (text.empty()) return fallback;
    char* end = nullptr;
    const long value = std::strtol(text.c_str(), &end, 10);
    if (end == text.c_str()) return fallback;
    return (int) value;
  }

  /** The value as a number written with '.', whatever the host's locale;
   *  `fallback` when it is missing or does not start with a number. Text
   *  after the number is ignored: "2.5 dB" gives 2.5. */
  double getDoubleValue(const std::string& key, double fallback = 0.0) const {
    const std::string text = getValue(key);
    if (text.empty()) return fallback;
    return numtext::parseOr(text, fallback);
  }

  /** "true", or any non-zero number, as the reference reads it. */
  bool getBoolValue(const std::string& key, bool fallback = false) const {
    std::string text = getValue(key);
    if (text.empty()) return fallback;
    while (!text.empty() && detail::props::isXmlSpace(text.back())) text.pop_back();
    size_t start = 0;
    while (start < text.size() && detail::props::isXmlSpace(text[start])) ++start;
    text = text.substr(start);
    std::string lower;
    for (char c : text) lower.push_back(detail::props::asciiLower(c));
    if (lower == "true") return true;
    char* end = nullptr;
    const long value = std::strtol(text.c_str(), &end, 10);
    return end != text.c_str() && value != 0;
  }

  /** An empty key, or one containing a NUL, is ignored: neither format can
   *  write it back. */
  void setValue(const std::string& key, const std::string& value) {
    if (!detail::props::isStorableName(key)) return;
    {
      std::lock_guard<std::recursive_mutex> lock(mutex_);
      if (std::string* found = findLocked(key)) {
        if (*found == value) return; // unchanged: not dirty, nobody told
        *found = value;
      } else {
        values_.emplace_back(key, value);
      }
    }
    propertyChanged();
  }
  // Overloads, because a string literal would otherwise pick bool.
  /** The same for a C string; a null pointer stores "". */
  void setValue(const std::string& key, const char* value) {
    setValue(key, std::string(value ? value : ""));
  }
  /** Stored in decimal, read back by getIntValue(). */
  void setValue(const std::string& key, int value) { setValue(key, std::to_string(value)); }
  /** Stored as "1" or "0", read back by getBoolValue(). */
  void setValue(const std::string& key, bool value) {
    setValue(key, std::string(value ? "1" : "0"));
  }
  /** Stored with 17 significant digits and '.' as the point, so
   *  getDoubleValue() reads back the same double. */
  void setValue(const std::string& key, double value) {
    char text[64];
    numtext::format(text, sizeof(text), value);
    setValue(key, std::string(text));
  }

  /** Removes `key` from this file (never from the fallback). A change, and
   *  a save, only if it was there. */
  void removeValue(const std::string& key) {
    bool removed = false;
    {
      std::lock_guard<std::recursive_mutex> lock(mutex_);
      for (size_t i = 0; i < values_.size(); ++i)
        if (detail::props::keysEqual(values_[i].first, key, options_.ignoreCaseOfKeyNames)) {
          values_.erase(values_.begin() + (std::ptrdiff_t) i);
          removed = true;
          break;
        }
    }
    if (removed) propertyChanged();
  }

  /** In THIS file; the fallback is not asked. */
  bool containsKey(const std::string& key) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return findLocked(key) != nullptr;
  }

  /** This file's keys, in the order they were first set or read; the
   *  fallback's are not included. */
  std::vector<std::string> getAllKeys() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::vector<std::string> keys;
    keys.reserve(values_.size());
    for (const auto& entry : values_) keys.push_back(entry.first);
    return keys;
  }

  /** Removes every value from this file; the fallback is untouched. Saved
   *  like any other change. */
  void clear() {
    bool had = false;
    {
      std::lock_guard<std::recursive_mutex> lock(mutex_);
      had = !values_.empty();
      values_.clear();
    }
    if (had) propertyChanged();
  }

  /** Where getters look for a key this file does not have. Not owned; must
   *  outlive this or be reset first. */
  void setFallbackPropertySet(PropertiesFile* fallback) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    fallbackSet_ = fallback == this ? nullptr : fallback;
  }
  /** The file set by setFallbackPropertySet(), or null. */
  PropertiesFile* getFallbackPropertySet() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return fallbackSet_;
  }

  // ── Saving and loading ────────────────────────────────────────────────────

  /** True while there are changes not yet written. */
  bool needsToBeSaved() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return needsWriting_;
  }

  /** Marks the file changed or unchanged by hand. It does not start the
   *  delayed save: saveIfNeeded(), save() or the destructor writes it. */
  void setNeedsToBeSaved(bool needs) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    needsWriting_ = needs;
  }

  /** Save if anything changed. True when there was nothing to save, too. */
  bool saveIfNeeded() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return !needsWriting_ || save();
  }

  /**
   * Write the file now, in the configured format, atomically, under the
   * process lock if there is one. False if it could not be written -- a
   * read-only folder, doNotSave, a lock another process held for a second.
   */
  bool save() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    pendingSave_ = false;
    if (options_.doNotSave || file_.empty()) return false;
    const size_t cut = file_.find_last_of("/\\");
    if (cut != std::string::npos && !ensureDirectory(file_.substr(0, cut))) return false;
    std::unique_ptr<detail::props::FileLock> processLock;
    if (options_.useProcessLock) {
      processLock.reset(new detail::props::FileLock(file_ + ".lock", detail::props::kLockTimeoutMs));
      if (!processLock->isLocked()) return false;
    }
    std::vector<uint8_t> bytes;
    if (options_.storageFormat == StorageFormat::storeAsBinary) {
      bytes = detail::props::writeBinary(values_);
    } else {
      const std::string xml = detail::props::writeXml(values_);
      bytes.assign(xml.begin(), xml.end());
    }
    // Never more than reload() reads back. A file past kMaxFileBytes is
    // refused whole, so one oversized value lost EVERY value at the next
    // start, and the next save then wrote the empty set over the file. The
    // file on disk is left as it was, and the change stays unsaved.
    if (bytes.size() > detail::props::kMaxFileBytes) return false;
    if (!detail::writeFileAtomically(file_, bytes)) return false;
    needsWriting_ = false;
    return true;
  }

  /**
   * Throw away the values in memory and read the file again. A missing file
   * is an empty, valid one. Returns isValidFile().
   */
  bool reload() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    values_.clear();
    needsWriting_ = false;
    pendingSave_ = false;
    loadedOk_ = false;
    if (file_.empty()) return false;
    if (!detail::props::fileIsThere(file_)) {
      loadedOk_ = true;
      return true;
    }
    std::unique_ptr<detail::props::FileLock> processLock;
    if (options_.useProcessLock) {
      processLock.reset(new detail::props::FileLock(file_ + ".lock", detail::props::kLockTimeoutMs));
      // Held by another process for longer than we wait: not loaded. A lock
      // file that cannot be opened at all is a read-only folder nobody can
      // be writing to, and reading without the lock is safe there.
      if (processLock->opened() && !processLock->isLocked()) return false;
    }
    std::vector<uint8_t> bytes;
    if (!detail::props::readWholeFile(file_, bytes)) return false;
    detail::props::Values parsed;
    if (!detail::props::parseAny(bytes, parsed)) return false;
    if (options_.ignoreCaseOfKeyNames) {
      // Collapse keys that differ only in case: the later one wins.
      for (auto& entry : parsed)
        if (std::string* found = findLocked(entry.first)) *found = entry.second;
        else values_.push_back(entry);
    } else {
      values_ = std::move(parsed);
    }
    loadedOk_ = true;
    return true;
  }

  /**
   * Move the save clock on. A file with a change waiting saves once
   * millisecondsBeforeSaving have passed since the last change. Returns
   * whether it saved. Call it from whatever clock is already running -- the
   * editor tick, a MultiTimer.
   */
  bool advance(double elapsedMs) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!pendingSave_ || elapsedMs <= 0.0) return false;
    sinceChangeMs_ += elapsedMs;
    if (sinceChangeMs_ < (double) options_.millisecondsBeforeSaving) return false;
    pendingSave_ = false;
    return needsWriting_ && save();
  }

  /** Whether a delayed save is waiting for the clock. */
  bool isSavePending() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return pendingSave_;
  }

private:
  const std::string* findLocked(const std::string& key) const {
    for (const auto& entry : values_)
      if (detail::props::keysEqual(entry.first, key, options_.ignoreCaseOfKeyNames))
        return &entry.second;
    return nullptr;
  }
  std::string* findLocked(const std::string& key) {
    for (auto& entry : values_)
      if (detail::props::keysEqual(entry.first, key, options_.ignoreCaseOfKeyNames))
        return &entry.second;
    return nullptr;
  }

  void propertyChanged() {
    sendChangeMessage();
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    needsWriting_ = true;
    if (options_.millisecondsBeforeSaving > 0) {
      // Restarted on every change: the wait is from the LAST one.
      pendingSave_ = true;
      sinceChangeMs_ = 0.0;
    } else if (options_.millisecondsBeforeSaving == 0) {
      saveIfNeeded();
    }
  }

  std::string file_;
  Options options_;
  mutable std::recursive_mutex mutex_;
  detail::props::Values values_;
  PropertiesFile* fallbackSet_ = nullptr;
  bool loadedOk_ = false;
  bool needsWriting_ = false;
  bool pendingSave_ = false;
  double sinceChangeMs_ = 0.0;
};

inline std::string PropertiesFile::Options::getDefaultFile() const {
  if (applicationName.empty()) return std::string();
  const char sep = detail::props::separator();
  std::string dir;
#if defined(_WIN32)
  std::string root = pathio::environment(commonToAllUsers ? "ProgramData" : "APPDATA");
  if (root.empty() && commonToAllUsers) root = pathio::environment("ALLUSERSPROFILE");
  if (root.empty()) return std::string();
  dir = root + sep + detail::props::safeFolder(folderName.empty() ? applicationName : folderName);
#elif defined(__APPLE__)
  // Apple's two blessed places, as the reference insists: a sandboxed
  // application can write nowhere else in ~/Library.
  const bool allowed = osxLibrarySubFolder == "Preferences" ||
                       osxLibrarySubFolder.compare(0, 19, "Application Support") == 0 ||
                       osxLibrarySubFolder.compare(0, 10, "Containers") == 0;
  if (!allowed) return std::string();
  std::string root;
  if (commonToAllUsers) {
    root = "/Library";
  } else {
    const std::string home = pathio::environment("HOME");
    if (home.empty()) return std::string();
    root = home + "/Library";
  }
  dir = root + sep + osxLibrarySubFolder;
  if (!folderName.empty()) dir += sep + detail::props::safeFolder(folderName);
#else
  std::string root;
  if (commonToAllUsers) {
    // The first of XDG_CONFIG_DIRS: where a desktop expects machine-wide
    // configuration, writable by an administrator and read by everyone.
    const std::string dirs = pathio::environment("XDG_CONFIG_DIRS");
    root = dirs.substr(0, dirs.find(':'));
    if (root.empty()) root = "/etc/xdg";
  } else {
    root = pathio::environment("XDG_CONFIG_HOME");
    if (root.empty()) {
      const std::string home = pathio::environment("HOME");
      if (home.empty()) return std::string();
      root = home + "/.config";
    }
  }
  dir = root + sep + detail::props::safeFolder(folderName.empty() ? applicationName : folderName);
#endif
  std::string leaf = detail::props::safeComponent(applicationName);
  if (!filenameSuffix.empty())
    leaf += filenameSuffix[0] == '.' ? filenameSuffix : "." + filenameSuffix;
  return dir + sep + leaf;
}

/**
 * The pair: a user file and a machine-wide one, opened on first use, the
 * user's reading through to the machine's.
 *
 * setStorageParameters() first; then getUserSettings() for anything a user
 * chooses and getCommonSettings(true) for anything an installer or an
 * administrator sets -- which comes back as the user's file when the shared
 * one cannot be written.
 */
class ApplicationProperties {
public:
  /** No files yet: call setStorageParameters() first. */
  ApplicationProperties() = default;
  /** Saves what is dirty and closes both files. */
  ~ApplicationProperties() { closeFiles(); }

  ///@{
  /** Not copyable: it owns the two open files. */
  ApplicationProperties(const ApplicationProperties&) = delete;
  ApplicationProperties& operator=(const ApplicationProperties&) = delete;
  ///@}

  /** Closes (and saves) any files already open, so the next get opens the
   *  files these options name. */
  void setStorageParameters(const PropertiesFile::Options& options) {
    closeFiles();
    options_ = options;
  }

  /** The options last given to setStorageParameters(). Both files are
   *  opened from them, the machine-wide one with commonToAllUsers and
   *  useProcessLock set. */
  const PropertiesFile::Options& getStorageParameters() const { return options_; }

  /** The user's file; null until setStorageParameters names an application. */
  PropertiesFile* getUserSettings() {
    if (!user_) openFiles();
    return user_.get();
  }

  /**
   * The machine-wide file. With `returnUserPropsIfReadOnly`, it is tried
   * once -- by saving it -- and if that fails, the user's file is returned
   * instead, then and on every later call. A file that is there but could
   * not be read counts as read-only and is not tried.
   */
  PropertiesFile* getCommonSettings(bool returnUserPropsIfReadOnly) {
    if (!common_) openFiles();
    if (!common_) return nullptr;
    if (returnUserPropsIfReadOnly) {
      // Not tried by saving when it did not load -- another process held its
      // lock past the wait, or it does not parse: the save wrote this empty
      // copy over it, and a question about the file wiped every value an
      // installer or administrator had put there.
      if (commonReadOnly_ == 0)
        commonReadOnly_ = common_->isValidFile() && common_->save() ? -1 : 1;
      if (commonReadOnly_ > 0) return user_.get();
    }
    return common_.get();
  }

  /** Save both if dirty. True if nothing failed. */
  bool saveIfNeeded() {
    bool ok = true;
    if (user_) ok = user_->saveIfNeeded() && ok;
    if (common_) ok = common_->saveIfNeeded() && ok;
    return ok;
  }

  /** Save and close both. The next get opens them again. */
  void closeFiles() {
    // The user's first: it points at the common one as its fallback.
    user_.reset();
    common_.reset();
    commonReadOnly_ = 0;
  }

  /** Drive both files' delayed saves; see PropertiesFile::advance. */
  bool advance(double elapsedMs) {
    bool saved = false;
    if (user_) saved = user_->advance(elapsedMs) || saved;
    if (common_) saved = common_->advance(elapsedMs) || saved;
    return saved;
  }

private:
  void openFiles() {
    if (options_.applicationName.empty()) return;
    PropertiesFile::Options o = options_;
    if (!user_) {
      o.commonToAllUsers = false;
      user_.reset(new PropertiesFile(o));
    }
    if (!common_) {
      o.commonToAllUsers = true;
      // More than one process -- more than one USER -- writes this file.
      o.useProcessLock = true;
      common_.reset(new PropertiesFile(o));
    }
    user_->setFallbackPropertySet(common_.get());
  }

  PropertiesFile::Options options_;
  std::unique_ptr<PropertiesFile> common_;
  std::unique_ptr<PropertiesFile> user_;
  /** 0 not yet known, 1 read-only, -1 writable. */
  int commonReadOnly_ = 0;
};

} // namespace sonore
