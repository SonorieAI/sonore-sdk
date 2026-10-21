// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: the user's own presets: saved, named and browsed, in a folder.
//
// ── The gap ─────────────────────────────────────────────────────────────────
//
// A plugin had factory presets compiled in (presets.h) and a session state
// every host saves (the SNRS blob, clap_wrapper.h). What it did not have was
// the thing a user means by "preset": a sound THEY made, saved under a name,
// there again tomorrow in a different project -- and in a different DAW,
// because the host's own preset system is the host's. A preset saved in
// Live's browser does not exist in REAPER, and a preset saved in a session
// exists only in that session.
//
// So a user preset is a FILE, one per preset, in one folder per plugin, where
// every host this plugin is ever loaded into looks for it -- because it is the
// plugin looking, not the host.
//
// ── What is in the file ─────────────────────────────────────────────────────
//
// The plugin's state, as the bytes the host's save path produces. Not a list
// of parameter values: a preset of a sampler that did not carry the sample it
// was made with would be a preset of a different sound. The SAME bytes,
// because that makes a preset exactly a session's plugin state, restored by
// exactly the code that restores a session -- a parameter added in a later
// version reads as its default from an old preset, as it does from an old
// session (loadStateBody's rule), and nothing about presets has a second
// loader to get that wrong in.
//
// Around it, a small versioned container ("SNRP"), little-endian:
//
//   "SNRP" | u16 version | u16 oldest reader | u32 metadata bytes | u64 state bytes
//   metadata: a StateBag -- name, author, comment, category, tags, plugin id,
//             plugin version, SDK version, created, modified (Unix seconds)
//   state:    the plugin's state blob ("SNRS...")
//   u32 CRC-32 of everything before it
//
// The metadata is a StateBag for the reason state_bag.h gives: a key nobody
// knows is skipped, so a later version can add a field without the file
// becoming unreadable here. A change older readers genuinely cannot follow
// raises the "oldest reader" number, and this reader refuses such a file by
// name rather than misreading it. The checksum is what tells a file damaged in
// a sync or a copy from one that merely parses.
//
// ── Where the folder is ─────────────────────────────────────────────────────
//
//   Windows  Documents\<Vendor>\<Plugin>\Presets, from SHGetKnownFolderPath:
//            the Documents a user has moved to another drive or to OneDrive
//            is the one they look in.
//   macOS    ~/Library/Audio/Presets/<Vendor>/<Plugin> -- where Apple keeps a
//            unit's .aupreset files and Steinberg a plugin's .vstpreset files
//            on a Mac, so the plugin's three kinds of preset share a folder.
//            ~/Music/Audio Music Apps is Logic's own (patches, channel
//            strips) and not a plugin's. ~/Library is hidden in the Finder,
//            which is what "Show folder" in the preset menu is for.
//   Linux    $XDG_DATA_HOME/<vendor>/<plugin>/presets, ~/.local/share when
//            unset: user data that is not configuration.
//   SONORE_PRESET_ROOT, when set, replaces the OS's location on every
//   platform (<root>/<Vendor>/<Plugin>/Presets) -- for tests, validators and
//   portable installs -- and a plugin can name any folder it likes.
//
// ── Names ───────────────────────────────────────────────────────────────────
//
// What the user typed is the preset's NAME and lives in the file; the file is
// called something every filesystem accepts. "Bass: sub / 808?" is a fine
// name and no file can be called it on Windows, so the file name is the name
// with < > : " / \ | ? * and control characters replaced, leading dots and
// trailing dots and spaces taken off (Windows drops the trailing ones itself,
// and a file whose name changed on the way in cannot be found again), the
// reserved device names (CON, NUL, COM1, LPT9, COM¹...) made ordinary, and the
// length capped so the full path stays inside every system's limit. Two names
// that land on one file name get " (2)", " (3)" -- and the check is the
// filesystem's own, so "Bass" and "bass" are one file on Windows and macOS and
// two on Linux, as those systems say. Everything is UTF-8 (path_io.h).
// Unicode normalisation is NOT done: macOS stores names decomposed and hands
// them back that way, and the name shown is the one in the file anyway.
//
// ── Writing ─────────────────────────────────────────────────────────────────
//
// Through a temporary and a rename (user_settings.h's writeFileAtomically):
// a crash mid-save leaves the old preset, never half of the new one. Deleting
// goes to the Recycle Bin / Trash where the platform has one for that drive,
// and says so when it cannot -- a preset deleted for good by a misclick is
// the version of this feature people write angry forum posts about.
//
// ── Threads ─────────────────────────────────────────────────────────────────
//
// MAIN THREAD, all of it: it reads and writes files. A preset reaches the
// audio thread the way a host's restore does -- through the wrapper's state
// path, which hands the parameters over atomically (clap_wrapper.h).
//
// ── What a host sees ────────────────────────────────────────────────────────
//
// The folder is declared to CLAP hosts through preset discovery
// (clap_wrapper.h), with the factory presets written out as files beside it:
// see syncFactoryPresetFiles for why they are files there and not only in the
// binary.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "files.h"
#include "path_io.h"
#include "preset_file.h"
#include "state_bag.h"
#include "user_settings.h"
#include "version.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
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
#include <unistd.h>
#else
#include <fcntl.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <thread>
extern char** environ;
#endif

namespace sonore {

/** Everything a preset file says about itself besides the state. Text is
 *  UTF-8; times are Unix seconds, 0 for unknown. */
struct PresetMetadata {
  /** The preset's name as the user typed it. Stored cleaned: one line, no
   *  control characters, no spaces at the ends, at most 200 bytes
   *  (cleanPresetName). The file name is derived from it, not the other way
   *  round. */
  std::string name;
  std::string author;   ///< Who made it; one line, at most 512 bytes stored.
  /** Free text; line breaks are kept, at most 16384 bytes stored. */
  std::string comment;
  /** One word a browser can group by: "Bass", "Pad", "Drums". */
  std::string category;
  /** Free-form, for searching: "warm", "analog", "lofi". */
  std::vector<std::string> tags;
  /** Whose preset it is -- the descriptor's reverse-URI id. A preset for
   *  another plugin is not an error to read and IS an error to load. */
  std::string pluginId;
  /** The version of the plugin that wrote the file; empty when unknown.
   *  UserPresetStore writes its own (setPluginVersion), when it has one,
   *  into every preset it saves. */
  std::string pluginVersion;
  /** The SDK version that wrote the file. encodePresetFile writes this
   *  build's (SONORE_SDK_VERSION_STRING) when it is empty. */
  std::string sdkVersion;
  int64_t created = 0;   ///< When the preset was first saved, Unix seconds; 0 unknown.
  int64_t modified = 0;  ///< When it was last written, Unix seconds; 0 unknown.
  /** The factory preset a file was written from, or -1 for the user's own.
   *  Only syncFactoryPresetFiles writes one. */
  int factoryIndex = -1;
};

/** One preset file, decoded: what it says, and the state it carries. */
struct PresetFileContents {
  PresetMetadata metadata;  ///< What the file says about the preset.
  /** The plugin's state blob ("SNRS..."), byte for byte as the host's save
   *  path wrote it: hand it to the same restore path to load the preset. */
  std::vector<uint8_t> state;
  /** The container version that wrote it. */
  uint32_t formatVersion = 0;
};

namespace detail {
namespace presets {

constexpr uint16_t kFormatVersion = 1;
/** "SNRP", versions, metadata size, state size. */
constexpr size_t kHeaderBytes = 20;
constexpr size_t kTrailerBytes = 4;
/** No preset is this big. A sampler's state names its sample rather than
 *  carrying it, and a cap is what stops a wrong path to a disk image being
 *  read into memory. */
constexpr uint64_t kMaxFileBytes = (uint64_t) 256 << 20;
constexpr uint32_t kMaxMetadataBytes = 1u << 20;
constexpr size_t kMaxNameBytes = 200;
constexpr size_t kMaxShortTextBytes = 512;
constexpr size_t kMaxCommentBytes = 16384;
constexpr size_t kMaxTags = 64;
/** A file name's stem, in bytes. With the extension and a " (999)" it stays
 *  under the 255 every common filesystem allows a single name. */
constexpr size_t kMaxStemBytes = 120;

inline void putU16(std::vector<uint8_t>& out, uint16_t v) {
  out.push_back((uint8_t) (v & 0xff));
  out.push_back((uint8_t) (v >> 8));
}
inline void putU32(std::vector<uint8_t>& out, uint32_t v) {
  for (int i = 0; i < 4; ++i) out.push_back((uint8_t) ((v >> (8 * i)) & 0xff));
}
inline void putU64(std::vector<uint8_t>& out, uint64_t v) {
  for (int i = 0; i < 8; ++i) out.push_back((uint8_t) ((v >> (8 * i)) & 0xff));
}
inline uint16_t getU16(const uint8_t* p) { return (uint16_t) (p[0] | (p[1] << 8)); }
inline uint32_t getU32(const uint8_t* p) {
  return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}
inline uint64_t getU64(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v |= (uint64_t) p[i] << (8 * i);
  return v;
}

/** CRC-32 (IEEE 802.3, the zip and PNG one), table built once. */
inline uint32_t crc32(const uint8_t* data, size_t size) {
  struct Table {
    uint32_t v[256];
    Table() {
      for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k) c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        v[i] = c;
      }
    }
  };
  static const Table table;
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < size; ++i) crc = table.v[(crc ^ data[i]) & 0xffu] ^ (crc >> 8);
  return crc ^ 0xFFFFFFFFu;
}

/** Decode one UTF-8 sequence at `s[i]`. Returns its length, or 0 for a byte
 *  that starts nothing valid -- overlong forms, surrogates and anything past
 *  U+10FFFF included, by state_bag.h's rule. */
inline size_t utf8Length(const std::string& s, size_t i, uint32_t* codePoint) {
  const uint8_t c = (uint8_t) s[i];
  if (c < 0x80) {
    *codePoint = c;
    return 1;
  }
  size_t len = 0;
  uint32_t cp = 0, least = 0;
  if (c >= 0xC2 && c <= 0xDF) { len = 2; cp = c & 0x1Fu; least = 0x80; }
  else if (c >= 0xE0 && c <= 0xEF) { len = 3; cp = c & 0x0Fu; least = 0x800; }
  else if (c >= 0xF0 && c <= 0xF4) { len = 4; cp = c & 0x07u; least = 0x10000; }
  else return 0;
  if (i + len > s.size()) return 0;
  for (size_t k = 1; k < len; ++k) {
    const uint8_t b = (uint8_t) s[i + k];
    if ((b & 0xC0u) != 0x80u) return 0;
    cp = (cp << 6) | (b & 0x3Fu);
  }
  if (cp < least || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0;
  *codePoint = cp;
  return len;
}

/** Well-formed UTF-8 with no control characters but tab -- and line breaks,
 *  where `allowLineBreaks` says so (a comment has paragraphs; a name does
 *  not). */
inline bool isCleanText(const std::string& s, bool allowLineBreaks) {
  for (size_t i = 0; i < s.size();) {
    uint32_t cp = 0;
    const size_t len = utf8Length(s, i, &cp);
    if (len == 0) return false;
    if (cp < 0x20 && cp != '\t' && !(allowLineBreaks && (cp == '\n' || cp == '\r'))) return false;
    if (cp == 0x7F) return false;
    i += len;
  }
  return true;
}

/**
 * Text as it may be stored: invalid sequences become U+FFFD, control
 * characters go (a tab becomes a space, and a line break one too unless they
 * are allowed), spaces at either end go, and the length is capped at a
 * character boundary -- never in the middle of one, which would leave half a
 * character for the next reader to choke on.
 */
inline std::string cleanText(const std::string& in, size_t maxBytes, bool allowLineBreaks) {
  std::string out;
  out.reserve(in.size() < maxBytes ? in.size() : maxBytes);
  for (size_t i = 0; i < in.size();) {
    uint32_t cp = 0;
    size_t len = utf8Length(in, i, &cp);
    std::string piece;
    if (len == 0) {
      piece = "\xEF\xBF\xBD";
      len = 1;
    } else if (cp == '\t' || ((cp == '\n' || cp == '\r') && !allowLineBreaks)) {
      piece = " ";
    } else if ((cp < 0x20 && cp != '\n' && cp != '\r') || cp == 0x7F) {
      i += len;
      continue;
    } else {
      piece = in.substr(i, len);
    }
    if (out.size() + piece.size() > maxBytes) break;
    out += piece;
    i += len;
  }
  size_t a = 0, b = out.size();
  while (a < b && out[a] == ' ') ++a;
  while (b > a && out[b - 1] == ' ') --b;
  return out.substr(a, b - a);
}

inline char lowerAscii(char c) { return (c >= 'A' && c <= 'Z') ? (char) (c - 'A' + 'a') : c; }

inline bool equalsIgnoringAsciiCase(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (lowerAscii(a[i]) != lowerAscii(b[i])) return false;
  return true;
}

/** Whether two paths name one file as this platform compares names. A
 *  comparison of bytes would let "Bass" and "bass" be two presets on Windows,
 *  where they are one file. */
inline bool samePath(const std::string& a, const std::string& b) {
#if defined(_WIN32) || defined(__APPLE__)
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    char x = a[i], y = b[i];
    if (x == '\\') x = '/';
    if (y == '\\') y = '/';
    if (lowerAscii(x) != lowerAscii(y)) return false;
  }
  return true;
#else
  return a == b;
#endif
}

/**
 * Whether `base` -- a name up to its first dot -- is one Windows reserves for
 * a device. Opening "NUL.sonorepreset" opens the null device, whatever the
 * extension says, and writing a preset there succeeds and keeps nothing. The
 * superscript digits are on the list because Windows treats COM¹ as COM1.
 */
inline bool isReservedDeviceName(std::string base) {
  while (!base.empty() && (base.back() == ' ' || base.back() == '.')) base.pop_back();
  std::string lower;
  for (char c : base) lower.push_back(lowerAscii(c));
  static const char* const kFixed[] = {"con", "prn", "aux", "nul", "conin$", "conout$", "clock$"};
  for (const char* name : kFixed)
    if (lower == name) return true;
  if (lower.size() < 4) return false;
  const std::string head = lower.substr(0, 3);
  if (head != "com" && head != "lpt") return false;
  const std::string tail = lower.substr(3);
  if (tail.size() == 1 && tail[0] >= '0' && tail[0] <= '9') return true;
  // ¹ ² ³, as UTF-8.
  return tail == "\xC2\xB9" || tail == "\xC2\xB2" || tail == "\xC2\xB3";
}

inline int64_t nowSeconds() { return (int64_t) std::time(nullptr); }

/** Join tags with line breaks, which no tag may contain. */
inline std::string joinTags(const std::vector<std::string>& tags) {
  std::string out;
  for (const std::string& t : tags) {
    if (t.empty()) continue;
    if (!out.empty()) out.push_back('\n');
    out += t;
  }
  return out;
}

inline std::vector<std::string> splitTags(const std::string& text) {
  std::vector<std::string> out;
  size_t start = 0;
  while (start <= text.size() && out.size() < kMaxTags) {
    size_t end = text.find('\n', start);
    if (end == std::string::npos) end = text.size();
    const std::string tag = cleanText(text.substr(start, end - start), kMaxShortTextBytes, false);
    if (!tag.empty()) out.push_back(tag);
    start = end + 1;
  }
  return out;
}

/** A file's size from the filesystem, by UTF-8 path; false when it is not a
 *  file. Asked rather than counted, so listing a folder reads the headers
 *  of its presets and not the states behind them. */
inline bool fileSizeOf(const std::string& path, uint64_t* size) {
  // A regular file only, not merely "not a folder": a FIFO or a device named
  // like a preset passed, and the open that followed blocked.
#if defined(_WIN32)
  struct _stat64 st {};
  if (_wstat64(pathio::widenPath(path.c_str()).c_str(), &st) != 0 || (st.st_mode & _S_IFREG) == 0) return false;
  *size = (uint64_t) st.st_size;
#else
  struct stat st {};
  if (::stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return false;
  *size = (uint64_t) st.st_size;
#endif
  return true;
}

/** Whether `path` names a regular file -- not a folder, a FIFO or a device. */
inline bool isRegularFile(const std::string& path) {
  uint64_t size = 0;
  return fileSizeOf(path, &size);
}

/**
 * `path` open for reading when it is a regular file, and its size; null when
 * it is not one or will not open.
 *
 * Only a regular file: opening a FIFO that happens to be named like a preset
 * blocks until something writes to it, and a rescan would wait for ever on
 * the user's own folder. Outside Windows the path is asked first, as it
 * always was, because the open is what would block. Windows has no FIFO in a
 * folder to block on, and there the file is asked once it is open: a stat by
 * path is itself an open of the file, and with an on-access scanner each
 * open of a preset costs tens of microseconds -- a folder listing opened
 * every preset four times (readPresetMetadata), and spent most of its time
 * doing it. Both ask the same question of the same file.
 */
inline std::FILE* openRegularFile(const std::string& path, uint64_t* size) {
#if defined(_WIN32)
  std::FILE* f = ::sonore::pathio::open(path.c_str(), "rb");
  if (!f) return nullptr;
  struct _stat64 st {};
  if (_fstat64(_fileno(f), &st) != 0 || (st.st_mode & _S_IFREG) == 0) {
    std::fclose(f);
    return nullptr;
  }
  if (size) *size = (uint64_t) st.st_size;
  return f;
#else
  uint64_t bytes = 0;
  if (!fileSizeOf(path, &bytes)) return nullptr;
  std::FILE* f = ::sonore::pathio::open(path.c_str(), "rb");
  if (f && size) *size = bytes;
  return f;
#endif
}

/**
 * A whole file, refused past `cap`. `why` says what went wrong in words a
 * user can act on: a missing file and a file too big to be a preset are
 * different problems.
 */
inline bool readWholeFile(const std::string& path, std::vector<uint8_t>& out, uint64_t cap,
                          std::string* why) {
  out.clear();
  std::FILE* f = openRegularFile(path, nullptr);
  if (!f) {
    // Which of the two it was, asked only now that something went wrong.
    if (why) *why = isRegularFile(path) ? "the file could not be opened" : "it is not a file";
    return false;
  }
  uint8_t chunk[16384];
  size_t got = 0;
  bool tooBig = false;
  while ((got = std::fread(chunk, 1, sizeof(chunk), f)) > 0) {
    if ((uint64_t) out.size() + got > cap) {
      tooBig = true;
      break;
    }
    out.insert(out.end(), chunk, chunk + got);
  }
  std::fclose(f);
  if (tooBig) {
    if (why) *why = "the file is larger than any preset";
    out.clear();
    return false;
  }
  return true;
}

/**
 * The 16 bytes vst3_wrapper.h's deriveUid(id, "component") makes, so a CLAP
 * or AU build of a plugin can recognise a .vstpreset its VST3 build wrote.
 * vst3_wrapper.h stays the one that PRESENTS the class id; this copy is only
 * ever compared, in both byte orders (see vstPresetMatchesPlugin), and
 * tests/user_presets_test.cpp checks it against vst3::componentUid().
 */
inline void vst3ComponentUid(const char* pluginId, uint8_t out[16]) {
  uint64_t h1 = 0xcbf29ce484222325ULL;
  uint64_t h2 = 0x9e3779b97f4a7c15ULL;
  for (const char* p = pluginId; p && *p; ++p) {
    h1 = (h1 ^ (uint64_t) (uint8_t) *p) * 0x100000001b3ULL;
    h2 = (h2 ^ (uint64_t) (uint8_t) *p) * 0x100000001b3ULL;
    h2 = (h2 << 7) | (h2 >> 57);
  }
  for (const char* p = "component"; *p; ++p) h2 = (h2 ^ (uint64_t) (uint8_t) *p) * 0x100000001b3ULL;
  for (int i = 0; i < 8; ++i) {
    out[i] = (uint8_t) ((h1 >> (56 - i * 8)) & 0xff);
    out[i + 8] = (uint8_t) ((h2 >> (56 - i * 8)) & 0xff);
  }
}

/** 32 uppercase hex characters, in memory order or in the order COM prints a
 *  GUID (the first three fields byte-swapped). */
inline std::string uidHex(const uint8_t uid[16], bool comOrder) {
  static const int kCom[16] = {3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15};
  static const char kDigits[] = "0123456789ABCDEF";
  std::string out;
  for (int i = 0; i < 16; ++i) {
    const uint8_t b = uid[comOrder ? kCom[i] : i];
    out.push_back(kDigits[b >> 4]);
    out.push_back(kDigits[b & 15]);
  }
  return out;
}

// ── Where things go, per platform ────────────────────────────────────────────

#if defined(_WIN32)
inline std::string knownFolder(const KNOWNFOLDERID& id) {
  PWSTR folder = nullptr;
  std::string out;
  if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &folder)) && folder) out = pathio::narrow(folder);
  if (folder) CoTaskMemFree(folder);
  return out;
}
#endif

/** Documents (Windows), the presets root (macOS), the data home (Linux): the
 *  base the per-plugin user folder goes under. Empty when there is none. */
inline std::string userPresetBase() {
#if defined(_WIN32)
  std::string documents = knownFolder(FOLDERID_Documents);
  if (documents.empty()) {
    const std::string profile = pathio::environment("USERPROFILE");
    if (!profile.empty()) documents = profile + "\\Documents";
  }
  return documents;
#elif defined(__APPLE__)
  const std::string home = pathio::environment("HOME");
  return home.empty() ? std::string() : home + "/Library/Audio/Presets";
#else
  const std::string xdg = pathio::environment("XDG_DATA_HOME");
  if (!xdg.empty() && xdg[0] == '/') return xdg;
  const std::string home = pathio::environment("HOME");
  return home.empty() ? std::string() : home + "/.local/share";
#endif
}

/** Where a cache that the plugin can always rebuild belongs. */
inline std::string cacheBase() {
#if defined(_WIN32)
  std::string local = knownFolder(FOLDERID_LocalAppData);
  if (local.empty()) local = pathio::environment("LOCALAPPDATA");
  return local;
#elif defined(__APPLE__)
  const std::string home = pathio::environment("HOME");
  return home.empty() ? std::string() : home + "/Library/Caches";
#else
  const std::string xdg = pathio::environment("XDG_CACHE_HOME");
  if (!xdg.empty() && xdg[0] == '/') return xdg;
  const std::string home = pathio::environment("HOME");
  return home.empty() ? std::string() : home + "/.cache";
#endif
}

#if defined(__APPLE__)
template <typename Ret, typename... Args>
Ret objcSend(id target, const char* selector, Args... args) {
  using Fn = Ret (*)(id, SEL, Args...);
  return reinterpret_cast<Fn>(objc_msgSend)(target, sel_registerName(selector), args...);
}

inline id nsFileUrl(const std::string& path) {
  Class stringClass = objc_getClass("NSString");
  Class urlClass = objc_getClass("NSURL");
  if (!stringClass || !urlClass) return (id) 0;
  id text = objcSend<id>((id) stringClass, "stringWithUTF8String:", path.c_str());
  if (!text) return (id) 0;
  return objcSend<id>((id) urlClass, "fileURLWithPath:", text);
}

/** Every Cocoa call here returns autoreleased objects, and a plugin's editor
 *  callback is not necessarily inside a pool that drains. */
struct ScopedPool {
  id pool = (id) 0;
  ScopedPool() {
    if (Class cls = objc_getClass("NSAutoreleasePool"))
      pool = objcSend<id>(objcSend<id>((id) cls, "alloc"), "init");
  }
  ~ScopedPool() {
    if (pool) objcSend<void>(pool, "drain");
  }
  ScopedPool(const ScopedPool&) = delete;
  ScopedPool& operator=(const ScopedPool&) = delete;
};
#endif

#if !defined(_WIN32) && !defined(__APPLE__)
/** Percent-encoding for the trash spec's Path= line: everything but the
 *  unreserved characters and '/'. */
inline std::string percentEncode(const std::string& path) {
  static const char kDigits[] = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : path) {
    const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                       c == '-' || c == '_' || c == '.' || c == '~' || c == '/';
    if (plain) {
      out.push_back((char) c);
    } else {
      out.push_back('%');
      out.push_back(kDigits[c >> 4]);
      out.push_back(kDigits[c & 15]);
    }
  }
  return out;
}

/**
 * The freedesktop.org trash, by its specification: the file goes to
 * <trash>/files/<name> with <trash>/info/<name>.trashinfo saying where it came
 * from and when, and the .trashinfo is created FIRST, exclusively, because
 * that is what reserves the name against another program trashing a file of
 * the same name at the same moment. Only the home trash: a file on another
 * filesystem would need that volume's .Trash-$uid, and a rename cannot cross
 * filesystems anyway -- which is reported, not worked around by deleting.
 */
inline bool trashFreedesktop(const std::string& path, std::string* why) {
  std::string data = pathio::environment("XDG_DATA_HOME");
  if (data.empty() || data[0] != '/') {
    const std::string home = pathio::environment("HOME");
    if (home.empty()) {
      if (why) *why = "there is no home folder to keep a Trash in";
      return false;
    }
    data = home + "/.local/share";
  }
  const std::string trash = data + "/Trash";
  if (!ensureDirectory(trash + "/files") || !ensureDirectory(trash + "/info")) {
    if (why) *why = "the Trash folder could not be made";
    return false;
  }
  char absolute[4096];
  const std::string full = realpath(path.c_str(), absolute) ? std::string(absolute) : path;
  const std::string leaf = fileName(full);
  for (int n = 1; n < 10000; ++n) {
    const std::string name = n == 1 ? leaf : leaf + "." + std::to_string(n);
    const std::string info = trash + "/info/" + name + ".trashinfo";
    const int fd = ::open(info.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) continue; // taken: try the next name
    char date[32] = "";
    const std::time_t now = std::time(nullptr);
    struct tm local {};
    if (localtime_r(&now, &local)) std::strftime(date, sizeof(date), "%Y-%m-%dT%H:%M:%S", &local);
    const std::string text =
        "[Trash Info]\nPath=" + percentEncode(full) + "\nDeletionDate=" + date + "\n";
    const bool wrote = ::write(fd, text.data(), text.size()) == (ssize_t) text.size();
    ::close(fd);
    if (wrote && pathio::rename(full.c_str(), (trash + "/files/" + name).c_str()) == 0) return true;
    pathio::remove(info.c_str());
    if (why) *why = "the file could not be moved to the Trash (it may be on another drive)";
    return false;
  }
  if (why) *why = "the Trash has no free name for it";
  return false;
}
#endif

} // namespace presets
} // namespace detail

/** The extension every user preset file carries, with its dot. */
inline const char* userPresetExtension() { return ".sonorepreset"; }

/** A preset name as it may be stored and shown: valid UTF-8, one line, no
 *  control characters, no spaces at the ends, at most 200 bytes. Empty
 *  means there was no name in it. */
inline std::string cleanPresetName(const std::string& name) {
  return detail::presets::cleanText(name, detail::presets::kMaxNameBytes, false);
}

/**
 * The file name (without extension) a preset called `name` is saved under:
 * accepted by Windows, macOS and Linux alike. See the header for every rule;
 * the result is never empty and never a device name.
 */
inline std::string presetFileStem(const std::string& name) {
  using namespace detail::presets;
  const std::string clean = cleanPresetName(name);
  std::string out;
  for (size_t i = 0; i < clean.size();) {
    uint32_t cp = 0;
    const size_t len = utf8Length(clean, i, &cp);
    if (len == 0) { // cleanText never leaves one, but a stem must not depend on that
      ++i;
      continue;
    }
    const bool forbidden = cp < 0x20 || cp == 0x7F || cp == '<' || cp == '>' || cp == ':' ||
                           cp == '"' || cp == '/' || cp == '\\' || cp == '|' || cp == '?' ||
                           cp == '*';
    const std::string piece = forbidden ? std::string("_") : clean.substr(i, len);
    if (out.size() + piece.size() > kMaxStemBytes) break;
    out += piece;
    i += len;
  }
  // A leading dot hides the file on macOS and Linux; trailing dots and spaces
  // are dropped by Windows on the way in, so the file would not be found by
  // the name it was written under.
  while (!out.empty() && (out[0] == '.' || out[0] == ' ')) out.erase(out.begin());
  while (!out.empty() && (out.back() == '.' || out.back() == ' ')) out.pop_back();
  if (out.empty()) out = "Preset";
  const size_t dot = out.find('.');
  const std::string base = dot == std::string::npos ? out : out.substr(0, dot);
  if (isReservedDeviceName(base)) {
    out.insert(base.size(), "_");
    // The '_' can take a stem at the cap one byte past it (fuzz_user_presets
    // found "COM¹.…" at 121): whole characters come off the END, never the
    // base that was just made safe, so the stem of a stem is the stem.
    while (out.size() > kMaxStemBytes) {
      size_t cut = out.size() - 1;
      while (cut > 0 && ((uint8_t) out[cut] & 0xC0u) == 0x80u) --cut;
      out.resize(cut);
    }
    while (!out.empty() && (out.back() == '.' || out.back() == ' ')) out.pop_back();
  }
  return out;
}

/** The container, with its checksum. The state is stored as given. */
inline std::vector<uint8_t> encodePresetFile(const PresetMetadata& m, const std::vector<uint8_t>& state) {
  using namespace detail::presets;
  StateBag bag;
  bag.setString("name", cleanPresetName(m.name));
  if (!m.author.empty()) bag.setString("author", cleanText(m.author, kMaxShortTextBytes, false));
  if (!m.comment.empty()) bag.setString("comment", cleanText(m.comment, kMaxCommentBytes, true));
  if (!m.category.empty()) bag.setString("category", cleanText(m.category, kMaxShortTextBytes, false));
  std::vector<std::string> tags;
  for (const std::string& t : m.tags) {
    const std::string clean = cleanText(t, kMaxShortTextBytes, false);
    if (!clean.empty() && tags.size() < kMaxTags) tags.push_back(clean);
  }
  if (!tags.empty()) bag.setString("tags", joinTags(tags));
  bag.setString("pluginId", cleanText(m.pluginId, kMaxShortTextBytes, false));
  if (!m.pluginVersion.empty())
    bag.setString("pluginVersion", cleanText(m.pluginVersion, kMaxShortTextBytes, false));
  bag.setString("sdkVersion", m.sdkVersion.empty() ? std::string(SONORE_SDK_VERSION_STRING)
                                                   : cleanText(m.sdkVersion, kMaxShortTextBytes, false));
  bag.setInt("created", m.created);
  bag.setInt("modified", m.modified);
  if (m.factoryIndex >= 0) bag.setInt("factoryIndex", m.factoryIndex);
  std::vector<uint8_t> metadata;
  bag.serialise(metadata);

  std::vector<uint8_t> out;
  out.reserve(kHeaderBytes + metadata.size() + state.size() + kTrailerBytes);
  out.insert(out.end(), {'S', 'N', 'R', 'P'});
  putU16(out, kFormatVersion);
  putU16(out, 1); // the oldest reader that can read this
  putU32(out, (uint32_t) metadata.size());
  putU64(out, (uint64_t) state.size());
  out.insert(out.end(), metadata.begin(), metadata.end());
  out.insert(out.end(), state.begin(), state.end());
  putU32(out, crc32(out.data(), out.size()));
  return out;
}

namespace detail {
namespace presets {

/** The header and metadata, refused with a reason. `fileSize` is the size of
 *  the whole file, which `data` may be only the start of. */
inline bool decodeHead(const uint8_t* data, size_t available, uint64_t fileSize, PresetMetadata* meta,
                       uint32_t* version, uint32_t* metadataBytes, uint64_t* stateBytes, std::string* why) {
  auto fail = [why](const char* reason) {
    if (why) *why = reason;
    return false;
  };
  if (!data || fileSize < kHeaderBytes + kTrailerBytes || available < kHeaderBytes)
    return fail("too short to be a preset file");
  if (std::memcmp(data, "SNRP", 4) != 0) return fail("not a Sonore preset file");
  const uint16_t written = getU16(data + 4), oldestReader = getU16(data + 6);
  if (written == 0 || oldestReader == 0) return fail("damaged: its version is zero");
  if (oldestReader > kFormatVersion)
    return fail("made by a newer version of the plugin, in a preset format this version cannot read");
  if (fileSize > kMaxFileBytes) return fail("larger than any preset");
  const uint32_t metaSize = getU32(data + 8);
  const uint64_t stateSize = getU64(data + 12);
  const uint64_t body = fileSize - kHeaderBytes - kTrailerBytes;
  // As what is LEFT, never as sums: both sizes come from the file, and a
  // state size near 2^64 wraps an addition into something that fits.
  if (metaSize > kMaxMetadataBytes || metaSize > body) return fail("damaged: its metadata runs past its end");
  if (stateSize != body - metaSize) return fail("damaged: truncated, or with something added to it");
  if (available < kHeaderBytes + (size_t) metaSize) return fail("damaged: truncated");
  StateBag bag;
  size_t consumed = 0;
  if (!bag.deserialise(data + kHeaderBytes, metaSize, &consumed) || consumed != metaSize)
    return fail("damaged: its metadata does not parse");

  auto text = [&](const char* key, size_t cap, bool lines, std::string* out) {
    const std::string value = bag.getString(key);
    if (value.size() > cap || !isCleanText(value, lines)) return false;
    *out = value;
    return true;
  };
  PresetMetadata m;
  std::string tags;
  if (!text("name", kMaxNameBytes, false, &m.name) || !text("author", kMaxShortTextBytes, false, &m.author) ||
      !text("comment", kMaxCommentBytes, true, &m.comment) ||
      !text("category", kMaxShortTextBytes, false, &m.category) ||
      !text("tags", kMaxShortTextBytes * kMaxTags, true, &tags) ||
      !text("pluginId", kMaxShortTextBytes, false, &m.pluginId) ||
      !text("pluginVersion", kMaxShortTextBytes, false, &m.pluginVersion) ||
      !text("sdkVersion", kMaxShortTextBytes, false, &m.sdkVersion))
    return fail("damaged: its metadata is not text");
  // Read back in the form encodePresetFile writes, so what is decoded and
  // written again is the same file (fuzz_user_presets found an author with a
  // space in front, kept by the reader and trimmed by the writer).
  m.name = cleanPresetName(m.name);
  m.author = cleanText(m.author, kMaxShortTextBytes, false);
  m.comment = cleanText(m.comment, kMaxCommentBytes, true);
  m.category = cleanText(m.category, kMaxShortTextBytes, false);
  m.pluginId = cleanText(m.pluginId, kMaxShortTextBytes, false);
  m.pluginVersion = cleanText(m.pluginVersion, kMaxShortTextBytes, false);
  m.sdkVersion = cleanText(m.sdkVersion, kMaxShortTextBytes, false);
  m.tags = splitTags(tags);
  m.created = bag.getInt("created", 0);
  m.modified = bag.getInt("modified", 0);
  const int64_t factory = bag.getInt("factoryIndex", -1);
  m.factoryIndex = factory >= 0 && factory < 100000 ? (int) factory : -1;
  if (meta) *meta = std::move(m);
  if (version) *version = written;
  if (metadataBytes) *metadataBytes = metaSize;
  if (stateBytes) *stateBytes = stateSize;
  return true;
}

} // namespace presets
} // namespace detail

/**
 * Read a preset file's bytes back. Refuses -- leaving `out` empty and saying
 * why -- anything that is not a whole, undamaged preset holding a Sonore
 * plugin state: a preset applied half-way is worse than one not applied.
 * Never trusts a size from the file: every length is checked against what is
 * actually there before it is used.
 */
inline bool decodePresetFile(const uint8_t* data, size_t size, PresetFileContents* out,
                             std::string* why = nullptr) {
  using namespace detail::presets;
  if (out) *out = PresetFileContents();
  PresetMetadata meta;
  uint32_t version = 0, metaSize = 0;
  uint64_t stateSize = 0;
  if (!decodeHead(data, size, (uint64_t) size, &meta, &version, &metaSize, &stateSize, why)) return false;
  if (crc32(data, size - kTrailerBytes) != getU32(data + size - kTrailerBytes)) {
    if (why) *why = "damaged: its checksum does not match its contents";
    return false;
  }
  const uint8_t* state = data + kHeaderBytes + metaSize;
  if (stateSize < 12 || std::memcmp(state, "SNRS", 4) != 0) {
    if (why) *why = "it holds no Sonore plugin state";
    return false;
  }
  if (out) {
    out->metadata = std::move(meta);
    out->state.assign(state, state + (size_t) stateSize);
    out->formatVersion = version;
  }
  return true;
}

/** Read the preset file at `path` (UTF-8) and decode it with
 *  decodePresetFile's checks, checksum included. A file larger than any
 *  preset (256 MB) is refused before it is read in full. False with the
 *  reason in `why` when the file cannot be read or is not a whole preset;
 *  `out` is meaningful only on true. Main thread: it reads the file. */
inline bool readPresetFile(const std::string& path, PresetFileContents* out, std::string* why = nullptr) {
  std::vector<uint8_t> bytes;
  if (!detail::presets::readWholeFile(path, bytes, detail::presets::kMaxFileBytes, why)) return false;
  return decodePresetFile(bytes.data(), bytes.size(), out, why);
}

/**
 * The metadata alone, for listing a folder: the header, the metadata and the
 * first bytes of the state, never the rest. Everything a list shows is
 * checked; the checksum, which needs the whole file, is checked when the
 * preset is loaded.
 */
inline bool readPresetMetadata(const std::string& path, PresetMetadata* out, std::string* why = nullptr) {
  using namespace detail::presets;
  // One open for all of it: the header says how much metadata follows, and
  // the rest is read on from there. It was two, the second reading the file
  // again from its start, each after a stat of its own.
  uint64_t total = 0;
  std::FILE* f = openRegularFile(path, &total);
  if (!f) {
    if (why) *why = "the file could not be opened";
    return false;
  }
  std::vector<uint8_t> head(kHeaderBytes);
  head.resize(std::fread(head.data(), 1, kHeaderBytes, f));
  if (head.size() < kHeaderBytes || total < kHeaderBytes + kTrailerBytes) {
    std::fclose(f);
    if (why) *why = "too short to be a preset file";
    return false;
  }
  const uint32_t metaSize = getU32(head.data() + 8);
  if (metaSize <= kMaxMetadataBytes && metaSize <= total) {
    // The state's own magic too, which costs four bytes more.
    const size_t more = (size_t) metaSize + 4;
    head.resize(kHeaderBytes + more);
    head.resize(kHeaderBytes + std::fread(head.data() + kHeaderBytes, 1, more, f));
  }
  std::fclose(f);
  uint64_t stateSize = 0;
  if (!decodeHead(head.data(), head.size(), total, out, nullptr, nullptr, &stateSize, why)) return false;
  if (stateSize < 12 || head.size() < kHeaderBytes + (size_t) metaSize + 4 ||
      std::memcmp(head.data() + kHeaderBytes + metaSize, "SNRS", 4) != 0) {
    if (why) *why = "it holds no Sonore plugin state";
    return false;
  }
  return true;
}

/** Write a preset file through a temporary and a rename. */
inline bool writePresetFile(const std::string& path, const PresetMetadata& meta,
                            const std::vector<uint8_t>& state, std::string* why = nullptr) {
  if (detail::writeFileAtomically(path, encodePresetFile(meta, state))) return true;
  if (why) *why = "the file could not be written (is the folder read-only, or the disk full?)";
  return false;
}

/** `<root>/<Vendor>/<Plugin>/Presets` where SONORE_PRESET_ROOT is set, else the
 *  OS's place for them (see the header). Empty when this machine has none. */
inline std::string defaultUserPresetFolder(const std::string& vendor, const std::string& plugin) {
  const std::string v = presetFileStem(vendor.empty() ? std::string("Sonore") : vendor);
  const std::string p = presetFileStem(plugin);
  const std::string root = pathio::environment("SONORE_PRESET_ROOT");
  if (!root.empty()) return joinPath(joinPath(joinPath(root, v), p), "Presets");
  const std::string base = detail::presets::userPresetBase();
  if (base.empty()) return std::string();
#if defined(_WIN32)
  return joinPath(joinPath(joinPath(base, v), p), "Presets");
#elif defined(__APPLE__)
  return joinPath(joinPath(base, v), p);
#else
  return joinPath(joinPath(joinPath(base, v), p), "presets");
#endif
}

/** Where syncFactoryPresetFiles keeps the factory presets as files: the OS's
 *  cache location, because the binary can always write them again. */
inline std::string defaultFactoryPresetFolder(const std::string& vendor, const std::string& plugin) {
  const std::string v = presetFileStem(vendor.empty() ? std::string("Sonore") : vendor);
  const std::string p = presetFileStem(plugin);
  const std::string root = pathio::environment("SONORE_PRESET_ROOT");
  if (!root.empty()) return joinPath(joinPath(joinPath(root, v), p), "Factory Presets");
  const std::string base = detail::presets::cacheBase();
  if (base.empty()) return std::string();
#if defined(_WIN32) || defined(__APPLE__)
  return joinPath(joinPath(joinPath(base, v), p), "Factory Presets");
#else
  return joinPath(joinPath(joinPath(base, v), p), "factory-presets");
#endif
}

/**
 * Move a file to the Recycle Bin (Windows), the Trash (macOS) or the
 * freedesktop.org trash (Linux). False, with the reason, when it could not --
 * and then the file is still there: this never falls back to deleting for
 * good, which is a decision for the person who asked.
 */
inline bool moveFileToTrash(const std::string& path, std::string* why = nullptr) {
  if (path.empty() || !fileExists(path)) {
    if (why) *why = "the file is not there";
    return false;
  }
#if defined(_WIN32)
  std::wstring wide = pathio::widen(path.c_str());
  for (wchar_t& c : wide)
    if (c == L'/') c = L'\\';
  // SHFileOperation takes no long-path prefix and stops at MAX_PATH.
  if (wide.size() >= MAX_PATH) {
    if (why) *why = "the path is too long for the Recycle Bin";
    return false;
  }
  // A drive with no Recycle Bin -- a network share, some removable media --
  // makes FOF_ALLOWUNDO delete for good, without a word. So it is asked
  // first, and "no Recycle Bin" is reported for the caller to decide.
  wchar_t volume[MAX_PATH + 1] = {};
  if (GetVolumePathNameW(wide.c_str(), volume, MAX_PATH)) {
    SHQUERYRBINFO info{};
    info.cbSize = sizeof(info);
    if (FAILED(SHQueryRecycleBinW(volume, &info))) {
      if (why) *why = "this drive has no Recycle Bin";
      return false;
    }
  }
  wide.push_back(L'\0'); // the list SHFileOperation reads ends with two
  SHFILEOPSTRUCTW op{};
  op.wFunc = FO_DELETE;
  op.pFrom = wide.c_str();
  op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
  const int rc = SHFileOperationW(&op);
  if (rc != 0 || op.fAnyOperationsAborted || fileExists(path)) {
    if (why) *why = "the Recycle Bin refused it (is the file open somewhere?)";
    return false;
  }
  return true;
#elif defined(__APPLE__)
  detail::presets::ScopedPool pool;
  Class managerClass = objc_getClass("NSFileManager");
  id url = detail::presets::nsFileUrl(path);
  if (!managerClass || !url) {
    if (why) *why = "the Trash is not reachable from this process";
    return false;
  }
  id manager = detail::presets::objcSend<id>((id) managerClass, "defaultManager");
  const bool moved = manager && detail::presets::objcSend<BOOL>(manager, "trashItemAtURL:resultingItemURL:error:",
                                                                url, (id*) nullptr, (id*) nullptr);
  if (!moved || fileExists(path)) {
    if (why) *why = "the Trash refused it";
    return false;
  }
  return true;
#else
  return detail::presets::trashFreedesktop(path, why);
#endif
}

/**
 * Open a folder in Explorer, the Finder or the desktop's file manager. Never
 * waits for it: the call returns as soon as the request is made, because a
 * file manager that takes a second to start is a second the host's interface
 * would otherwise be frozen for.
 */
inline bool showFolderInFileManager(const std::string& folder) {
  if (folder.empty() || !isDirectory(folder)) return false;
#if defined(_WIN32)
  const std::wstring wide = pathio::widen(folder.c_str());
  const INT_PTR rc = (INT_PTR) ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  return rc > 32; // ShellExecute's documented "greater than 32 is success"
#elif defined(__APPLE__)
  detail::presets::ScopedPool pool;
  Class workspaceClass = objc_getClass("NSWorkspace");
  id url = detail::presets::nsFileUrl(folder);
  if (!workspaceClass || !url) return false;
  id workspace = detail::presets::objcSend<id>((id) workspaceClass, "sharedWorkspace");
  return workspace && detail::presets::objcSend<BOOL>(workspace, "openURL:", url);
#else
  // xdg-open, found on the PATH, with nothing of ours inherited but the
  // standard streams -- pointed at /dev/null -- and reaped by a thread of its
  // own so it is never left a zombie.
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_addopen(&actions, 1, "/dev/null", O_WRONLY, 0);
  posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0);
  std::string program = "xdg-open", target = folder;
  char* argv[] = {&program[0], &target[0], nullptr};
  pid_t pid = 0;
  const int rc = posix_spawnp(&pid, "xdg-open", &actions, nullptr, argv, environ);
  posix_spawn_file_actions_destroy(&actions);
  if (rc != 0) return false;
  std::thread([pid]() {
    int status = 0;
    waitpid(pid, &status, 0);
  }).detach();
  return true;
#endif
}

/** The class id a .vstpreset for this plugin carries, as 32 hex characters:
 *  the id's bytes in memory order, which is how a Mac or Linux host prints
 *  it and how Steinberg's convention prints it everywhere. */
inline std::string vstPresetClassIdFor(const char* pluginId) {
  uint8_t uid[16];
  detail::presets::vst3ComponentUid(pluginId, uid);
  return detail::presets::uidHex(uid, false);
}

/**
 * Whether a .vstpreset's class id is this plugin's.
 *
 * The comparison plugin_cache.h makes -- the file's 32 hex characters against
 * the plugin's -- done in BOTH byte orders, case-blind. A Windows host that
 * prints a GUID the COM way spells the same 16 bytes with its first three
 * fields reversed, and the VST3 class-id convention is moving from one
 * spelling to the other; a preset saved either side of that change is still
 * this plugin's preset.
 */
inline bool vstPresetMatchesPlugin(const std::string& classIdText, const char* pluginId) {
  if (classIdText.size() != 32 || !pluginId || !pluginId[0]) return false;
  uint8_t uid[16];
  detail::presets::vst3ComponentUid(pluginId, uid);
  return detail::presets::equalsIgnoringAsciiCase(classIdText, detail::presets::uidHex(uid, false)) ||
         detail::presets::equalsIgnoringAsciiCase(classIdText, detail::presets::uidHex(uid, true));
}

/**
 * The plugin's factory presets, written out as preset files in `folder` --
 * one per preset, named "01 Warm Glue.sonorepreset" and so on -- and any
 * other preset file there removed. A file already holding exactly the right
 * bytes is left alone, so doing this at every host scan writes nothing after
 * the first.
 *
 * Why they are files at all, when the binary already carries them: CLAP's
 * preset discovery offers two ways to list presets, files in a folder and a
 * "container" inside the plugin, and a host's browser handles both -- but the
 * container is one call reporting every preset, and the reference validator
 * (clap-validator 0.4.1 and its master as of this writing) deadlocks on the
 * second preset of any container: its begin_preset() holds a lock that its
 * flush of the previous preset takes again. Files are one preset per call.
 * The binary stays the source of truth; these are its cache, rebuilt when
 * missing, and the compiled-in presets keep working when this folder cannot
 * be written at all.
 *
 * `stateFor(i, bytes)` makes preset i's state blob. Returns false only when
 * nothing usable was written; `paths` receives the files, in preset order.
 */
inline bool syncFactoryPresetFiles(const std::string& folder, const std::vector<std::string>& names,
                                   const PresetMetadata& common,
                                   const std::function<bool(int, std::vector<uint8_t>&)>& stateFor,
                                   std::vector<std::string>* paths = nullptr, std::string* why = nullptr) {
  if (paths) paths->clear();
  if (folder.empty() || !stateFor) {
    if (why) *why = "nowhere to write the factory presets";
    return false;
  }
  if (!ensureDirectory(folder)) {
    if (why) *why = "the factory preset folder could not be made";
    return false;
  }
  std::vector<std::string> written;
  for (size_t i = 0; i < names.size(); ++i) {
    std::vector<uint8_t> state;
    if (!stateFor((int) i, state) || state.empty()) {
      if (why) *why = "a factory preset has no state";
      return false;
    }
    PresetMetadata meta = common;
    meta.name = names[i];
    meta.factoryIndex = (int) i;
    // No times: the bytes must come out the same at every scan, or "already
    // there" would never be true and every scan would write.
    meta.created = meta.modified = 0;
    char prefix[16];
    std::snprintf(prefix, sizeof(prefix), "%02d ", (int) i + 1);
    const std::string path =
        joinPath(folder, std::string(prefix) + presetFileStem(names[i]) + userPresetExtension());
    const std::vector<uint8_t> bytes = encodePresetFile(meta, state);
    std::vector<uint8_t> existing;
    std::string ignored;
    const bool same = detail::presets::readWholeFile(path, existing, detail::presets::kMaxFileBytes, &ignored) &&
                      existing == bytes;
    if (!same && !detail::writeFileAtomically(path, bytes)) {
      if (why) *why = "a factory preset could not be written";
      return false;
    }
    written.push_back(path);
  }
  // A preset renamed or removed in a later version must not linger here.
  for (const FileEntry& e : listDirectory(folder, {userPresetExtension()}, false, true)) {
    bool keep = false;
    for (const std::string& w : written) keep = keep || detail::presets::samePath(w, e.path);
    if (!keep) pathio::remove(e.path.c_str());
  }
  if (paths) *paths = written;
  return true;
}

/**
 * One plugin's folder of user presets: listing it, and every operation on it.
 *
 * Holds the list as last read; rescan() reads the folder again and says
 * whether anything changed, which is how a preset copied in from outside --
 * by a file manager, a sync client, a friend's zip -- appears without the
 * plugin being reopened. Unchanged files are not read twice: a file is known
 * by its path, size and time, and only a new or changed one is opened.
 *
 * Every operation that changes the folder rescans before returning, so the
 * list is never older than the last thing done to it. MAIN THREAD.
 */
class UserPresetStore {
public:
  /** One preset in the folder as last scanned: its metadata, where it is,
   *  and the size and time it was read at. A preset with no name of its own
   *  is listed under its file name, extension dropped. A copy taken at the
   *  scan: the file itself can change or vanish afterwards, which the next
   *  rescan() shows. */
  struct Entry {
    /** What the file says, read by readPresetMetadata: everything a list
     *  shows is checked, the checksum only when the preset is loaded. */
    PresetMetadata metadata;
    std::string path;  ///< The whole path, UTF-8: what to open.
    /** The file's name with its extension: what a selection is kept by. */
    std::string fileName;
    uint64_t fileSize = 0;      ///< In bytes, at the scan.
    int64_t fileModified = 0;   ///< Last write, Unix seconds; 0 where the platform did not say.
  };
  /** A file in the folder that is not a preset of this plugin, and why. */
  struct Refusal {
    std::string path;    ///< The whole path, UTF-8.
    std::string reason;  ///< Why it is not listed, in words a user can act on.
  };
  /** What save() does when a preset of that name (ignoring ASCII case)
   *  already exists. */
  enum class IfExists {
    Fail,     ///< Refuse, with the reason; nothing is written.
    Replace,  ///< Write over that preset's file, keeping its creation time.
    KeepBoth  ///< Save this one as well, under "name (2)", "name (3)"...
  };
  /** How remove() deletes a preset. */
  enum class Removal {
    ToTrash,     ///< Through trashFile, or to the Recycle Bin or Trash when it is empty.
    Permanently  ///< Deleted from the disk, with no way back.
  };

  /** No folder yet: every operation fails with a reason until setFolder()
   *  names one. */
  UserPresetStore() = default;

  /** The folder to list and write in, UTF-8. A different folder drops the
   *  list and the cache, and nothing is read until rescan() (save() runs one
   *  first when none has been); the same folder again changes nothing. A
   *  folder that does not exist yet lists as empty and is made by the first
   *  save. */
  void setFolder(const std::string& folder) {
    if (folder == folder_) return;
    folder_ = folder;
    entries_.clear();
    refused_.clear();
    cache_.clear();
    scanned_ = false;
  }
  /** The folder as setFolder() gave it; empty when none is set. */
  const std::string& folder() const { return folder_; }

  /** Whose presets these are. A file for another plugin is listed under
   *  refused(); empty accepts every plugin's (a browser of everything). */
  void setPluginId(const std::string& id) { pluginId_ = id; }
  /** The id setPluginId() gave, also stamped into every preset this store
   *  writes; empty when every plugin's presets are accepted, and then a
   *  preset written keeps the id it already carries. */
  const std::string& pluginId() const { return pluginId_; }
  /** The plugin's version, written into every preset this store saves.
   *  Empty leaves the version a preset already carries; an export takes it
   *  only when the metadata it is given has none. */
  void setPluginVersion(const std::string& version) { pluginVersion_ = version; }
  /** Written into the presets this store saves, when the caller gives none. */
  void setAuthor(const std::string& author) { author_ = author; }

  /** Where a deleted preset goes, when it goes to the trash. Empty means the
   *  system's (moveFileToTrash); a plugin with its own "recently deleted"
   *  folder, or a test that must not fill the user's Recycle Bin, sets it. */
  std::function<bool(const std::string& path, std::string* why)> trashFile;

  /** Whether rescan() has run since the store was made or pointed at
   *  another folder. */
  bool hasScanned() const { return scanned_; }
  /** The presets as of the last rescan(), by name ignoring ASCII case (ties
   *  by file name); empty before the first. Indices into it are what the
   *  operations below take. Every operation that changes the folder rescans,
   *  so an index or an element reference is stale after one. */
  const std::vector<Entry>& entries() const { return entries_; }
  /** The files with the preset extension the last rescan() did not list,
   *  each with its reason: damaged, not a preset, another plugin's. */
  const std::vector<Refusal>& refused() const { return refused_; }

  /**
   * Read the folder again. True when the list changed.
   *
   * Sorted by name, case-blind, the way every preset browser sorts: the order
   * a user can predict, and the same on every filesystem.
   */
  bool rescan() {
    scanned_ = true;
    std::vector<Entry> fresh;
    std::vector<Refusal> refused;
    std::vector<Cached> cache;
    if (!folder_.empty()) {
      // By path, through a map: a folder of a thousand presets read again
      // every couple of seconds must not cost a million string comparisons.
      std::map<std::string, size_t> byPath;
      for (size_t i = 0; i < cache_.size(); ++i) byPath[cache_[i].path] = i;
      for (const FileEntry& f : listDirectory(folder_, {userPresetExtension()}, false, false)) {
        const Cached* known = nullptr;
        const auto hit = byPath.find(f.path);
        if (hit != byPath.end()) {
          const Cached& c = cache_[hit->second];
          if (c.size == f.sizeInBytes && c.modified == f.modifiedTime) known = &c;
        }
        Cached c;
        if (known) {
          c = *known;
        } else {
          c.path = f.path;
          c.size = f.sizeInBytes;
          c.modified = f.modifiedTime;
          c.ok = readPresetMetadata(f.path, &c.metadata, &c.reason);
          if (c.ok && !pluginId_.empty() && c.metadata.pluginId != pluginId_) {
            c.ok = false;
            c.reason = "it is a preset for another plugin (" + c.metadata.pluginId + ")";
          }
        }
        cache.push_back(c);
        if (!c.ok) {
          refused.push_back({f.path, c.reason});
          continue;
        }
        Entry e;
        e.metadata = c.metadata;
        e.path = f.path;
        e.fileName = f.name;
        e.fileSize = f.sizeInBytes;
        e.fileModified = f.modifiedTime;
        if (e.metadata.name.empty()) {
          const std::string ext = userPresetExtension();
          e.metadata.name = f.name.substr(0, f.name.size() - ext.size());
        }
        fresh.push_back(std::move(e));
      }
    }
    std::stable_sort(fresh.begin(), fresh.end(), [](const Entry& a, const Entry& b) {
      if (!detail::presets::equalsIgnoringAsciiCase(a.metadata.name, b.metadata.name))
        return asciiLess(a.metadata.name, b.metadata.name);
      return asciiLess(a.fileName, b.fileName);
    });
    bool changed = fresh.size() != entries_.size() || refused.size() != refused_.size();
    for (size_t i = 0; !changed && i < fresh.size(); ++i)
      changed = fresh[i].path != entries_[i].path || fresh[i].fileModified != entries_[i].fileModified ||
                fresh[i].fileSize != entries_[i].fileSize || fresh[i].metadata.name != entries_[i].metadata.name;
    entries_.swap(fresh);
    refused_.swap(refused);
    cache_.swap(cache);
    return changed;
  }

  /** The index in entries() of the preset at `path`, or -1. Paths compare
   *  as the platform compares them: ignoring ASCII case and slash direction
   *  on Windows and macOS, byte for byte on Linux. Reads no files. */
  int indexOfPath(const std::string& path) const {
    for (size_t i = 0; i < entries_.size(); ++i)
      if (detail::presets::samePath(entries_[i].path, path)) return (int) i;
    return -1;
  }

  /** The index in entries() of the preset whose file is called `name`,
   *  extension included ("Bass.sonorepreset"), or -1. Compared as
   *  indexOfPath() compares. */
  int indexOfFileName(const std::string& name) const {
    for (size_t i = 0; i < entries_.size(); ++i)
      if (detail::presets::samePath(entries_[i].fileName, name)) return (int) i;
    return -1;
  }

  /** By name: exactly first, then ignoring ASCII case. */
  int indexOfName(const std::string& name) const {
    for (size_t i = 0; i < entries_.size(); ++i)
      if (entries_[i].metadata.name == name) return (int) i;
    for (size_t i = 0; i < entries_.size(); ++i)
      if (detail::presets::equalsIgnoringAsciiCase(entries_[i].metadata.name, name)) return (int) i;
    return -1;
  }

  /** Whether a preset other than `except` already has this name, ignoring
   *  ASCII case: two presets the user cannot tell apart in a list are one
   *  mistake waiting to be made. */
  bool nameExists(const std::string& name, int except = -1) const {
    const std::string clean = cleanPresetName(name);
    for (size_t i = 0; i < entries_.size(); ++i)
      if ((int) i != except && detail::presets::equalsIgnoringAsciiCase(entries_[i].metadata.name, clean))
        return true;
    return false;
  }

  /** "Bass", or "Bass (2)", "Bass (3)"... -- the first no preset has.
   *
   *  The number has to fit inside the cap on a name, so a name already at
   *  the cap gives up characters from its end to make room. Appended to the
   *  whole name it was cut off again by the cap, every candidate came out as
   *  the name itself, and after a hundred thousand tries the name was used
   *  as it was: a second preset of that name, by Keep Both, Duplicate or
   *  Import. */
  std::string uniqueName(const std::string& name) const {
    std::string clean = cleanPresetName(name);
    if (clean.empty()) clean = "Preset";
    if (!nameExists(clean)) return clean;
    for (int n = 2; n < 100000; ++n) {
      const std::string suffix = " (" + std::to_string(n) + ")";
      std::string base = clean;
      while (!base.empty() && base.size() + suffix.size() > detail::presets::kMaxNameBytes) {
        size_t cut = base.size() - 1; // back to the first byte of the last character
        while (cut > 0 && ((uint8_t) base[cut] & 0xC0u) == 0x80u) --cut;
        base.resize(cut);
      }
      const std::string candidate = cleanPresetName(base + suffix);
      if (!nameExists(candidate)) return candidate;
    }
    return clean;
  }

  /**
   * Save `state` as a preset called `name`.
   *
   * A name already in use is the caller's decision: Fail refuses, Replace
   * writes over that preset (keeping when it was created), KeepBoth saves
   * this one as "name (2)". `details` carries author, comment, category and
   * tags; its name is ignored, and so is its plugin id when the store has
   * one (setPluginId). A store with none takes the id from `details`, and
   * refuses with no id from either: no plugin would list the preset.
   */
  bool save(const std::string& name, const std::vector<uint8_t>& state, IfExists ifExists,
            std::string* savedPath = nullptr, std::string* why = nullptr, const PresetMetadata* details = nullptr) {
    if (!ready(why)) return false;
    std::string clean = cleanPresetName(name);
    if (clean.empty()) return fail(why, "a preset needs a name");
    if (pluginId_.empty() && (!details || details->pluginId.empty()))
      return fail(why, "a preset needs the id of the plugin it is for");
    if (!scanned_) rescan();
    PresetMetadata meta;
    if (details) meta = *details;
    meta.factoryIndex = -1;
    if (meta.author.empty()) meta.author = author_;
    std::string path;
    const int existing = indexOfName(clean);
    if (existing >= 0 && ifExists == IfExists::Fail)
      return fail(why, "a preset called \"" + entries_[(size_t) existing].metadata.name + "\" already exists");
    if (existing >= 0 && ifExists == IfExists::Replace) {
      path = entries_[(size_t) existing].path;
      meta.created = entries_[(size_t) existing].metadata.created;
    } else {
      if (existing >= 0) clean = uniqueName(clean);
      path = allocatePath(clean, std::string());
      meta.created = detail::presets::nowSeconds();
    }
    meta.name = clean;
    return writeEntry(path, meta, state, savedPath, why);
  }

  /** Write `state` over preset `index`, keeping its name and metadata. */
  bool overwrite(int index, const std::vector<uint8_t>& state, std::string* why = nullptr) {
    if (!ready(why) || !valid(index, why)) return false;
    const Entry e = entries_[(size_t) index];
    return writeEntry(e.path, e.metadata, state, nullptr, why);
  }

  /** Rename preset `index`: the name inside it, and its file to match. */
  bool rename(int index, const std::string& newName, std::string* newPath = nullptr, std::string* why = nullptr) {
    if (!ready(why) || !valid(index, why)) return false;
    const std::string clean = cleanPresetName(newName);
    if (clean.empty()) return fail(why, "a preset needs a name");
    if (nameExists(clean, index)) return fail(why, "a preset called \"" + clean + "\" already exists");
    const Entry e = entries_[(size_t) index];
    PresetFileContents contents;
    if (!readPresetFile(e.path, &contents, why)) return false;
    PresetMetadata meta = contents.metadata;
    meta.name = clean;
    const std::string path = allocatePath(clean, e.path);
    // The new file first, then the old one away: a failure part-way leaves
    // the preset under one name or the other, never under neither.
    if (!writeEntry(path, meta, contents.state, newPath, why)) return false;
    if (!detail::presets::samePath(path, e.path) && pathio::remove(e.path.c_str()) != 0) {
      pathio::remove(path.c_str());
      rescan();
      return fail(why, "the old file could not be removed (is it open somewhere?)");
    }
    rescan();
    return true;
  }

  /** Delete preset `index`: to the trash, or for good. */
  bool remove(int index, Removal how, std::string* why = nullptr) {
    if (!ready(why) || !valid(index, why)) return false;
    const std::string path = entries_[(size_t) index].path;
    bool ok = false;
    if (how == Removal::ToTrash) {
      ok = trashFile ? trashFile(path, why) : moveFileToTrash(path, why);
    } else {
      ok = pathio::remove(path.c_str()) == 0;
      if (!ok && why) *why = "the file could not be deleted (is it open somewhere?)";
    }
    rescan();
    return ok;
  }

  /** A copy of preset `index`, called "<name> copy" (or "copy (2)"...). */
  bool duplicate(int index, std::string* newPath = nullptr, std::string* why = nullptr) {
    if (!ready(why) || !valid(index, why)) return false;
    PresetFileContents contents;
    if (!readPresetFile(entries_[(size_t) index].path, &contents, why)) return false;
    PresetMetadata meta = contents.metadata;
    meta.name = uniqueName(contents.metadata.name + " copy");
    meta.created = detail::presets::nowSeconds();
    return writeEntry(allocatePath(meta.name, std::string()), meta, contents.state, newPath, why);
  }

  /**
   * Bring a preset file from anywhere into the folder: one of these, or a
   * .vstpreset this plugin's VST3 build wrote. Refused, with the reason, when
   * it is not this plugin's. The copy is named after the preset (or the
   * file), made unique; the original is left where it was. A store with no
   * plugin id refuses a file that does not say whose it is (a .vstpreset
   * names its plugin only by a hash), as save() does.
   */
  bool importFile(const std::string& source, std::string* importedPath = nullptr, std::string* why = nullptr) {
    if (!ready(why)) return false;
    PresetFileContents contents;
    if (!readForImport(source, &contents, why)) return false;
    if (pluginId_.empty() && contents.metadata.pluginId.empty())
      return fail(why, "a preset needs the id of the plugin it is for");
    PresetMetadata meta = contents.metadata;
    if (meta.name.empty()) meta.name = stemOf(source);
    meta.name = uniqueName(meta.name);
    if (meta.created == 0) meta.created = detail::presets::nowSeconds();
    meta.factoryIndex = -1;
    return writeEntry(allocatePath(meta.name, std::string()), meta, contents.state, importedPath, why);
  }

  /** Read an outside file the way importFile does, without copying it: a
   *  preset of this plugin, or a .vstpreset of it, or a refusal. */
  bool readForImport(const std::string& source, PresetFileContents* out, std::string* why = nullptr) const {
    if (out) *out = PresetFileContents();
    if (fileExtension(source) == ".vstpreset") {
      Vst3PresetFile preset;
      if (!readVstPreset(source.c_str(), &preset)) return fail(why, "it is not a readable .vstpreset");
      if (!pluginId_.empty() && !vstPresetMatchesPlugin(preset.classId, pluginId_.c_str()))
        return fail(why, "it is a .vstpreset for another plugin");
      if (preset.componentState.size() < 12 || std::memcmp(preset.componentState.data(), "SNRS", 4) != 0)
        return fail(why, "it holds no Sonore plugin state");
      if (out) {
        out->metadata.name = cleanPresetName(stemOf(source));
        out->metadata.pluginId = pluginId_;
        out->state = std::move(preset.componentState);
      }
      return true;
    }
    PresetFileContents contents;
    if (!readPresetFile(source, &contents, why)) return false;
    if (!pluginId_.empty() && contents.metadata.pluginId != pluginId_)
      return fail(why, "it is a preset for another plugin (" + contents.metadata.pluginId + ")");
    if (out) *out = std::move(contents);
    return true;
  }

  /** Write a preset to `destination`, outside the folder: one of these, or a
   *  .vstpreset when that is the extension asked for. */
  bool exportTo(const PresetMetadata& meta, const std::vector<uint8_t>& state, const std::string& destination,
                std::string* why = nullptr) const {
    if (destination.empty()) return fail(why, "nowhere to export to");
    if (fileExtension(destination) == ".vstpreset") {
      if (pluginId_.empty()) return fail(why, "a .vstpreset needs the plugin's id");
      if (writeVstPreset(destination.c_str(), vstPresetClassIdFor(pluginId_.c_str()), state)) return true;
      return fail(why, "the file could not be written");
    }
    PresetMetadata m = meta;
    m.pluginId = pluginId_.empty() ? meta.pluginId : pluginId_;
    if (m.pluginVersion.empty()) m.pluginVersion = pluginVersion_;
    return writePresetFile(destination, m, state, why);
  }

  /** Preset `index` in full, checksum and all. */
  bool read(int index, PresetFileContents* out, std::string* why = nullptr) const {
    if (!valid(index, why)) return false;
    if (!readPresetFile(entries_[(size_t) index].path, out, why)) return false;
    if (out && !pluginId_.empty() && out->metadata.pluginId != pluginId_)
      return fail(why, "it is a preset for another plugin (" + out->metadata.pluginId + ")");
    return true;
  }

private:
  /** What the last scan learned about one file, preset or not: rescan()
   *  reuses it while the path, size and time still match, which is what keeps
   *  a folder of a thousand presets from being opened a thousand times every
   *  couple of seconds. A refused file is cached with its reason too. */
  struct Cached {
    std::string path;         ///< The whole path, the cache's key.
    uint64_t size = 0;        ///< In bytes, when read; all ones forces a re-read.
    int64_t modified = 0;     ///< Last write when read, Unix seconds.
    bool ok = false;          ///< Whether it is a listable preset of this plugin.
    PresetMetadata metadata;  ///< Its metadata, when `ok`.
    std::string reason;       ///< Why it was refused, when not `ok`.
  };

  static bool fail(std::string* why, const std::string& reason) {
    if (why) *why = reason;
    return false;
  }

  bool ready(std::string* why) const {
    if (folder_.empty()) return fail(why, "this plugin has no preset folder on this machine");
    return true;
  }

  bool valid(int index, std::string* why) const {
    if (index < 0 || index >= (int) entries_.size()) return fail(why, "there is no such preset");
    return true;
  }

  static std::string stemOf(const std::string& path) {
    std::string name = fileName(path);
    const std::string ext = fileExtension(name);
    if (!ext.empty()) name.resize(name.size() - ext.size());
    return name;
  }

  /**
   * A path for a preset called `name` that no other file has. `keep` is a
   * file that may be reused -- the one being renamed -- so renaming "Bass"
   * to "bass" does not become "bass (2)" on a filesystem that calls them one
   * file. The check is the filesystem's own (does it exist?), so every
   * system's idea of "the same name" is the one applied.
   */
  std::string allocatePath(const std::string& name, const std::string& keep) const {
    const std::string stem = presetFileStem(name);
    for (int n = 1; n < 100000; ++n) {
      const std::string candidate =
          joinPath(folder_, (n == 1 ? stem : stem + " (" + std::to_string(n) + ")") + userPresetExtension());
      if (!keep.empty() && detail::presets::samePath(candidate, keep)) return candidate;
      if (!fileExists(candidate)) return candidate;
    }
    return joinPath(folder_, stem + userPresetExtension());
  }

  bool writeEntry(const std::string& path, PresetMetadata meta, const std::vector<uint8_t>& state,
                  std::string* savedPath, std::string* why) {
    // The store's id when it has one; a store with none (a browser of every
    // plugin's presets) keeps the preset's own. It stamped its empty id over
    // every preset it renamed, wrote over or duplicated, and the plugin the
    // preset belonged to refused it from then on.
    if (!pluginId_.empty()) meta.pluginId = pluginId_;
    if (!pluginVersion_.empty()) meta.pluginVersion = pluginVersion_;
    meta.sdkVersion = SONORE_SDK_VERSION_STRING;
    meta.modified = detail::presets::nowSeconds();
    if (meta.created == 0) meta.created = meta.modified;
    if (!writePresetFile(path, meta, state, why)) return false;
    // This file is known to have changed whatever its size and time say: a
    // save within the same second at the same size would otherwise look
    // unchanged to the cache.
    for (size_t i = 0; i < cache_.size(); ++i)
      if (detail::presets::samePath(cache_[i].path, path)) cache_[i].size = ~(uint64_t) 0;
    if (savedPath) *savedPath = path;
    rescan();
    return true;
  }

  std::string folder_, pluginId_, pluginVersion_, author_;
  std::vector<Entry> entries_;
  std::vector<Refusal> refused_;
  std::vector<Cached> cache_;
  bool scanned_ = false;
};

/**
 * A plugin instance's presets: the factory list and the user folder as one
 * list, the preset the user is on, whether they have changed it since, and
 * every operation a preset menu offers.
 *
 * The reference has no single class for this (plugins written against it
 * each build their own "preset manager" over its program calls and its
 * state); this is that class, done once.
 *
 * It does not know what a plugin's state IS. The wrapper hands it four
 * functions (Hooks): the state as the host's save path writes it, a state to
 * restore through the host's own restore path, a factory preset as a state,
 * and a hash of the current sound for the "modified" marker. Which is what
 * makes a preset load land exactly where a host's session restore lands, in
 * every format -- and what lets a test drive this with four lambdas.
 *
 * MAIN THREAD.
 */
class PresetManager {
public:
  /** Where a preset comes from. */
  enum class Origin {
    Unset,    ///< No preset: nothing is selected, or no such row.
    Factory,  ///< Compiled into the plugin, from the descriptor's preset list.
    User      ///< A file in the user folder.
  };
  /** How a state is restored. Preset: a preset load -- the session's bypass,
   *  editor size and selection are kept, and the manager sets the selection
   *  after. Restore: an earlier state of this session coming back (an undo)
   *  -- the selection comes back with it. Neither touches the host's own
   *  session-wide settings. */
  enum class LoadScope {
    Preset,  ///< A preset being loaded; the manager sets the selection after.
    Restore  ///< An earlier state of this session, selection included.
  };

  /** Which preset the user is on, and what it sounded like when they got
   *  there. Saved with the session, so a project reopens on the preset it
   *  was saved on -- with the "modified" marker if it was. */
  struct Selection {
    Origin origin = Origin::Unset;  ///< Factory or User; Unset when nothing is selected.
    /** The factory preset's 0-based index in the descriptor's list when
     *  `origin` is Factory, else -1. */
    int factoryIndex = -1;
    /** The name a preset bar shows. Kept for a user preset that has since
     *  left the folder. */
    std::string name;
    /** A user preset's file name, which survives a rename of the folder. */
    std::string fileName;
    /** The sound's hash when loaded or saved; compared for "modified". */
    uint64_t baseline = 0;
  };

  /** One row of the combined list: factory presets first, in the
   *  descriptor's order, then the user's, by name. */
  struct Item {
    Origin origin = Origin::Unset;  ///< Unset for an index outside the list.
    int factoryIndex = -1;  ///< 0-based index in the factory list when Factory, else -1.
    int userIndex = -1;     ///< Index into store().entries() when User, else -1.
    std::string name;       ///< The preset's name, as a list shows it.
  };

  /** The four functions (and one notice) that tie the manager to a plugin's
   *  state, supplied by the wrapper through setHooks(). Called on the main
   *  thread, from inside the manager's own calls. A hook left empty is not a
   *  crash: the operation that needs it fails with a reason, and with no
   *  soundHash nothing is ever marked modified. */
  struct Hooks {
    /** The state, exactly as the host's save path writes it. */
    std::function<bool(std::vector<uint8_t>& out)> saveState;
    /** Restore a state through the host's restore path, and tell the host. */
    std::function<bool(const std::vector<uint8_t>& state, LoadScope scope, std::string* why)> loadState;
    /** Factory preset `index` as a state blob. */
    std::function<bool(int index, std::vector<uint8_t>& out)> factoryState;
    /** A hash of the current sound -- parameters and the plugin's own state,
     *  not the window size or the bypass. */
    std::function<uint64_t()> soundHash;
    /** Told after the plugin itself loaded a preset, for a format that keeps
     *  the host's browser in step (CLAP's preset-load "loaded"). */
    std::function<void(const Selection&)> loaded;
  };

  /** Who the plugin is, as configure() takes it. `pluginId` is what a preset
   *  file must carry to be listed (another plugin's are refused), vendor and
   *  pluginName pick the default folder, version is stamped into saved
   *  presets, and factoryNames are the factory presets in the descriptor's
   *  order -- the indices factoryState is asked for. */
  struct Identity {
    std::string pluginId,  ///< The descriptor's id: carried by every preset saved, required of every one listed.
        pluginName,        ///< Names the default folder, and an export when no preset is selected.
        vendor,            ///< Names the default folder's parent; empty is "Sonore".
        version;           ///< The plugin version written into saved presets.
    /** The factory presets' names, index 0 first: row i of the combined
     *  list for i below numFactory(). */
    std::vector<std::string> factoryNames;
    /** Empty for the OS's place (defaultUserPresetFolder). */
    std::string userFolder;
  };

  /** Not configured and with no hooks: configure() and setHooks() first.
   *  The wrapper does both the first time an editor or the host asks for an
   *  instance's presets. */
  PresetManager() = default;
  /** Not copyable: a manager is one instance's, and the hooks a wrapper
   *  gives it call into that instance. */
  PresetManager(const PresetManager&) = delete;
  /** Not assignable, for the same reason. */
  PresetManager& operator=(const PresetManager&) = delete;

  /** Who the plugin is and where its folder is: points store() at
   *  Identity::userFolder, or at defaultUserPresetFolder() when that is
   *  empty, and gives it the plugin's id and version. Reads nothing from the
   *  disk -- the folder is read the first time the list is asked for -- and
   *  keeps the selection. Calling it again with another folder drops the
   *  list. */
  void configure(const Identity& identity) {
    identity_ = identity;
    store_.setFolder(identity.userFolder.empty() ? defaultUserPresetFolder(identity.vendor, identity.pluginName)
                                                 : identity.userFolder);
    store_.setPluginId(identity.pluginId);
    store_.setPluginVersion(identity.version);
    configured_ = true;
    ++revision_;
  }
  /** Whether configure() has been called. */
  bool isConfigured() const { return configured_; }
  /** The identity configure() was given; empty before. */
  const Identity& identity() const { return identity_; }

  /** Replace all five hooks at once. A hook left empty makes the operation
   *  that needs it fail with a reason (see Hooks). */
  void setHooks(Hooks hooks) { hooks_ = std::move(hooks); }
  /** The hooks as setHooks() gave them. */
  const Hooks& hooks() const { return hooks_; }

  /** The user folder underneath, for what the combined list does not offer:
   *  its path, its refusals. Changes made through it directly neither bump
   *  revision() nor move the selection. */
  UserPresetStore& store() { return store_; }
  /** As above, read-only. */
  const UserPresetStore& store() const { return store_; }

  /** Bumped by everything that can change what a preset bar shows: a load,
   *  a save, a rescan that found something. Cheap to poll. */
  uint64_t revision() const { return revision_; }

  /** Read the folder again; true when something was added, removed or
   *  changed there. */
  bool rescan() {
    const bool changed = store_.rescan();
    if (changed) ++revision_;
    return changed;
  }

  /** How many factory presets there are: rows 0 to numFactory() - 1. */
  int numFactory() const { return (int) identity_.factoryNames.size(); }
  /** How many user presets the folder holds as last read. The first call
   *  reads the folder; later ones do not (rescan() does). */
  int numUser() const {
    ensureScanned();
    return (int) store_.entries().size();
  }
  /** The length of the combined list: numFactory() + numUser(). */
  int count() const { return numFactory() + numUser(); }

  /** Row `index` (0-based) of the combined list; an Item whose origin is
   *  Unset when there is no such row. */
  Item itemAt(int index) const {
    Item item;
    if (index < 0) return item;
    if (index < numFactory()) {
      item.origin = Origin::Factory;
      item.factoryIndex = index;
      item.name = identity_.factoryNames[(size_t) index];
      return item;
    }
    const int u = index - numFactory();
    if (u < numUser()) {
      item.origin = Origin::User;
      item.userIndex = u;
      item.name = store_.entries()[(size_t) u].metadata.name;
    }
    return item;
  }

  /** The preset the user is on, as last loaded, saved or restored. */
  const Selection& selection() const { return selection_; }
  /** Whether a preset is selected -- also when it is a user preset that has
   *  since left the folder (currentIndex() is then -1). */
  bool hasSelection() const { return selection_.origin != Origin::Unset; }

  /** A session's selection coming back (the wrapper's state restore). The
   *  name is cleaned: it came from a file. */
  void restoreSelection(const Selection& s) {
    selection_ = s;
    selection_.name = cleanPresetName(s.name);
    if (selection_.origin == Origin::Unset) selection_ = Selection();
    ++revision_;
  }

  /** Select nothing, and so mark nothing modified. The sound is not
   *  touched. */
  void clearSelection() {
    selection_ = Selection();
    ++revision_;
  }

  /** A factory preset the HOST put in place by its own means -- a VST3
   *  program change, an AU preset -- with its values already applied: the
   *  selection follows it, and nothing is loaded twice. */
  void noteFactoryPresetApplied(int factoryIndex) {
    if (factoryIndex < 0 || factoryIndex >= numFactory()) return;
    selection_ = Selection();
    selection_.origin = Origin::Factory;
    selection_.factoryIndex = factoryIndex;
    selection_.name = identity_.factoryNames[(size_t) factoryIndex];
    selection_.baseline = hash();
    ++revision_;
  }

  /** Where the selection is in the combined list, or -1 (none, or a user
   *  preset no longer in the folder). */
  int currentIndex() const {
    if (selection_.origin == Origin::Factory)
      return selection_.factoryIndex >= 0 && selection_.factoryIndex < numFactory() ? selection_.factoryIndex : -1;
    if (selection_.origin != Origin::User) return -1;
    ensureScanned();
    int u = selection_.fileName.empty() ? -1 : store_.indexOfFileName(selection_.fileName);
    if (u < 0) u = store_.indexOfName(selection_.name);
    return u < 0 ? -1 : numFactory() + u;
  }

  /** The selected preset's name; empty when none is selected. */
  std::string currentName() const { return selection_.name; }
  /** Whether the selection is a user preset still in the folder: the case
   *  saveCurrent() can write over. */
  bool currentIsUser() const { return selection_.origin == Origin::User && currentIndex() >= 0; }

  /** Whether the sound has moved since the preset was loaded or saved. */
  bool isDirty() const {
    if (selection_.origin == Origin::Unset || !hooks_.soundHash) return false;
    return hooks_.soundHash() != selection_.baseline;
  }

  /** Load item `index` of the combined list. `announce` false when the HOST
   *  asked for it (a preset-load from its browser): it already knows. */
  bool load(int index, std::string* why = nullptr, bool announce = true) {
    const Item item = itemAt(index);
    std::vector<uint8_t> state;
    Selection next;
    if (item.origin == Origin::Factory) {
      if (!hooks_.factoryState || !hooks_.factoryState(item.factoryIndex, state))
        return fail(why, "the factory preset could not be made");
      next.origin = Origin::Factory;
      next.factoryIndex = item.factoryIndex;
      next.name = item.name;
    } else if (item.origin == Origin::User) {
      PresetFileContents contents;
      if (!store_.read(item.userIndex, &contents, why)) return false;
      state = std::move(contents.state);
      next.origin = Origin::User;
      next.name = store_.entries()[(size_t) item.userIndex].metadata.name;
      next.fileName = store_.entries()[(size_t) item.userIndex].fileName;
    } else {
      return fail(why, "there is no such preset");
    }
    return apply(state, next, announce, why);
  }

  /** The next preset in the combined list, wrapping round; the first when
   *  none is selected. The folder is read again first, so a preset that
   *  arrived from outside is in the order. */
  bool loadNext(std::string* why = nullptr) { return step(+1, why); }
  /** As loadNext(), backwards: the last preset when none is selected. */
  bool loadPrevious(std::string* why = nullptr) { return step(-1, why); }

  /**
   * Load a preset file from anywhere -- a host's preset browser handing over
   * a path, a file dropped on the editor. One in the user folder becomes the
   * selection by its file; a factory preset file (syncFactoryPresetFiles)
   * becomes that factory preset.
   */
  bool loadFile(const std::string& path, bool announce, std::string* why = nullptr) {
    PresetFileContents contents;
    if (!store_.readForImport(path, &contents, why)) return false;
    Selection next;
    const int factory = contents.metadata.factoryIndex;
    if (factory >= 0 && factory < numFactory() && contents.metadata.name == identity_.factoryNames[(size_t) factory]) {
      next.origin = Origin::Factory;
      next.factoryIndex = factory;
      next.name = identity_.factoryNames[(size_t) factory];
    } else {
      next.origin = Origin::User;
      // A preset with no name of its own is called what the list calls it
      // (and what a host's browser does): its file name without the
      // extension. The whole file name was kept, ".sonorepreset" and all.
      next.name = contents.metadata.name;
      if (next.name.empty()) {
        std::string stem = fileName(path);
        const std::string ext = fileExtension(stem);
        if (!ext.empty()) stem.resize(stem.size() - ext.size());
        next.name = cleanPresetName(stem);
      }
      ensureScanned();
      if (store_.indexOfPath(path) >= 0) next.fileName = fileName(path);
    }
    return apply(contents.state, next, announce, why);
  }

  /** Save the current sound over the current user preset. */
  bool saveCurrent(std::string* why = nullptr) {
    const int index = currentIndex();
    if (selection_.origin != Origin::User || index < 0)
      return fail(why, "the current preset is not one of yours: use Save As");
    std::vector<uint8_t> state;
    if (!capture(state, why)) return false;
    if (!store_.overwrite(index - numFactory(), state, why)) return false;
    selection_.baseline = hash();
    ++revision_;
    return true;
  }

  /** Save the current sound as a new preset, and make it the selection. */
  bool saveAs(const std::string& name, UserPresetStore::IfExists ifExists, std::string* why = nullptr,
              const PresetMetadata* details = nullptr) {
    std::vector<uint8_t> state;
    if (!capture(state, why)) return false;
    std::string path;
    if (!store_.save(name, state, ifExists, &path, why, details)) return false;
    const int u = store_.indexOfPath(path);
    selection_ = Selection();
    selection_.origin = Origin::User;
    selection_.name = u >= 0 ? store_.entries()[(size_t) u].metadata.name : cleanPresetName(name);
    selection_.fileName = fileName(path);
    selection_.baseline = hash();
    ++revision_;
    return true;
  }

  /** Rename item `index` (a user preset); the selection follows it. */
  bool rename(int index, const std::string& name, std::string* why = nullptr) {
    const Item item = itemAt(index);
    if (item.origin != Origin::User) return fail(why, "only your own presets can be renamed");
    const bool wasCurrent = index == currentIndex();
    std::string path;
    if (!store_.rename(item.userIndex, name, &path, why)) return false;
    if (wasCurrent) {
      selection_.name = cleanPresetName(name);
      selection_.fileName = fileName(path);
    }
    ++revision_;
    return true;
  }

  /** Delete item `index` (a user preset). Deleting the current one leaves
   *  the sound as it is and nothing selected. */
  bool remove(int index, UserPresetStore::Removal how, std::string* why = nullptr) {
    const Item item = itemAt(index);
    if (item.origin != Origin::User) return fail(why, "factory presets cannot be deleted");
    const bool wasCurrent = index == currentIndex();
    if (!store_.remove(item.userIndex, how, why)) return false;
    if (wasCurrent) selection_ = Selection();
    ++revision_;
    return true;
  }

  /** Copy item `index` (a user preset) in the folder as "<name> copy",
   *  made unique. The selection stays where it was; a factory preset is
   *  refused. */
  bool duplicate(int index, std::string* why = nullptr) {
    const Item item = itemAt(index);
    if (item.origin != Origin::User) return fail(why, "only your own presets can be duplicated");
    if (!store_.duplicate(item.userIndex, nullptr, why)) return false;
    ++revision_;
    return true;
  }

  /** Copy a file into the user folder (see UserPresetStore::importFile) and,
   *  when `andLoad`, load the copy. */
  bool importFile(const std::string& path, bool andLoad, std::string* why = nullptr) {
    std::string copy;
    if (!store_.importFile(path, &copy, why)) return false;
    ++revision_;
    if (!andLoad) return true;
    const int u = store_.indexOfPath(copy);
    return u >= 0 ? load(numFactory() + u, why) : fail(why, "the imported preset is not in the folder");
  }

  /** The current sound, under the current name, as a file outside the
   *  folder -- or a .vstpreset, by the extension asked for. */
  bool exportCurrent(const std::string& destination, std::string* why = nullptr) const {
    std::vector<uint8_t> state;
    if (!hooks_.saveState || !hooks_.saveState(state)) return fail(why, "the plugin's state could not be read");
    PresetMetadata meta;
    meta.name = selection_.name.empty() ? identity_.pluginName : selection_.name;
    meta.created = meta.modified = detail::presets::nowSeconds();
    return store_.exportTo(meta, state, destination, why);
  }

  /** The whole state now, for an undo history (see value_tree_state.h). */
  bool captureState(std::vector<uint8_t>& out) const { return hooks_.saveState && hooks_.saveState(out); }

  /** An earlier state of this session, back through the restore path. */
  bool restoreState(const std::vector<uint8_t>& state, std::string* why = nullptr) {
    if (!hooks_.loadState) return fail(why, "this plugin cannot restore its state from here");
    const bool ok = hooks_.loadState(state, LoadScope::Restore, why);
    ++revision_;
    return ok;
  }

private:
  static bool fail(std::string* why, const std::string& reason) {
    if (why) *why = reason;
    return false;
  }

  /** The list is read the first time anything asks for it, not when the
   *  plugin is made: an instance whose user never opens the editor never
   *  touches the disk. */
  void ensureScanned() const {
    if (!store_.hasScanned()) store_.rescan();
  }

  uint64_t hash() const { return hooks_.soundHash ? hooks_.soundHash() : 0; }

  bool capture(std::vector<uint8_t>& state, std::string* why) const {
    if (!hooks_.saveState || !hooks_.saveState(state) || state.empty())
      return fail(why, "the plugin's state could not be read");
    return true;
  }

  bool apply(const std::vector<uint8_t>& state, Selection next, bool announce, std::string* why) {
    if (!hooks_.loadState) return fail(why, "this plugin cannot load a preset from here");
    std::string reason;
    if (!hooks_.loadState(state, LoadScope::Preset, &reason))
      return fail(why, reason.empty() ? "the plugin refused the preset's state" : reason);
    next.baseline = hash();
    selection_ = std::move(next);
    ++revision_;
    if (announce && hooks_.loaded) hooks_.loaded(selection_);
    return true;
  }

  /** A preset that will not load is stepped over, not stopped at: one
   *  damaged file must not be a wall next and previous cannot pass. */
  bool step(int direction, std::string* why) {
    rescan();
    const int n = count();
    if (n == 0) return fail(why, "there are no presets yet");
    const int current = currentIndex();
    std::string first;
    for (int k = 1; k <= n; ++k) {
      const int next = current < 0 ? (direction > 0 ? k - 1 : n - k) : ((current + direction * k) % n + n) % n;
      std::string reason;
      if (load(next, &reason)) return true;
      if (first.empty()) first = itemAt(next).name + ": " + reason;
    }
    return fail(why, "no preset could be loaded (" + first + ")");
  }

  Identity identity_;
  Hooks hooks_;
  mutable UserPresetStore store_;
  Selection selection_;
  uint64_t revision_ = 0;
  bool configured_ = false;
};

} // namespace sonore
