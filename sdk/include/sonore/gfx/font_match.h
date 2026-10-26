// SPDX-License-Identifier: Apache-2.0
//
// FontMatcher: a CSS font-family list, a weight and a style, to a typeface.
//
// ── Why this exists ─────────────────────────────────────────────────────────
//
// system_font.h answers "a face for the interface" and "a face that has this
// character". Text in an SVG asks a third question, the one a browser
// answers: font-family="Inter, 'Helvetica Neue', Arial, sans-serif",
// font-weight 600, italic -- which face on this machine is that? Without an
// answer every <text> in a logo was dropped, and the row said text "looks
// different per machine". It does, and that is now said precisely below,
// along with what to do about it.
//
// ── How a family list is matched ────────────────────────────────────────────
//
// As CSS Fonts says, in its simplest complete form:
//
//   1. The families in order. Each is looked for first among the faces a
//      plugin REGISTERED -- its own font, compiled in -- and then among the
//      machine's fonts. The first family that has any face wins; later ones
//      are fallbacks for a family that is absent, not for a glyph.
//   2. A generic family -- serif, sans-serif, monospace, cursive, fantasy,
//      system-ui -- is a list of concrete families per platform (Arial for
//      sans-serif on Windows, as the browsers there use), and can be pointed
//      at registered faces with setGenericFamily.
//   3. Within the family, the style first (italic wanted: an italic face if
//      the family has one), then the weight by CSS's rule: for 400 to 500,
//      the nearest at or above up to 500, then below, then above 500; under
//      400 the nearest at or below first; over 500 the nearest at or above
//      first. A VARIABLE face with a weight axis covering the request is an
//      exact match, drawn at that weight on the axis.
//   4. Nothing named exists: the default family (serif, as browsers and
//      resvg default), and failing that the interface face.
//
// No bold or italic is synthesised -- a family with no bold face draws its
// regular one, as resvg does -- and a character the matched face lacks is
// drawn from the system's fallback faces, as everywhere else in Font.
//
// The machine's faces are found by reading the name, OS/2 and fvar tables of
// every font file in the system's font folders, once, the first time a
// family is asked for that was not registered: the files are mapped, so this
// touches a few pages of each rather than reading them -- on Windows only
// the bytes of those tables are read, on several threads, which a file
// system that is slow to open a file allows to overlap (buildIndexLocked).
//
// ── Text that is the same everywhere ────────────────────────────────────────
//
// A family list matched against the machine's fonts gives the machine's
// answer, and two machines answer differently -- the reason this was left
// out. A plugin whose art must look the same everywhere REGISTERS the face
// the art was designed with (registerTypeface, from bytes compiled into the
// plugin) and parses with a matcher that does not look at the system at all
// (FontMatcher(FontMatcher::EmbeddedOnly)). Then every family resolves to a
// registered face or to the first one registered, and no character falls
// back to a system face: the same file draws the same pixels on every
// machine. Converting the text to outlines before export remains the other
// way, and the right one for a logo nobody will edit.
#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../files.h"
#include "../path_io.h"
#include "font.h"
#include "system_font.h"
#include "truetype.h"

namespace sonore {
namespace gfx {

/** A face a FontMatcher chose, and how to draw it. */
struct FontMatch {
  /** The face, shared with the matcher, which keeps registered and loaded
   *  system faces for reuse. Null when nothing was found. */
  std::shared_ptr<Typeface> typeface;
  /** The family it was found under, as the font names it. */
  std::string family;
  /** The weight it draws at: the face's own, or the request when the face
   *  has a weight axis that reaches it. */
  int weight = 400;
  /** The face is sloped: italic or oblique by its OS/2 table (or its head
   *  table when it has no OS/2), or as given to registerTypeface. It can
   *  differ from the request when the family has no face of that slope. */
  bool italic = false;
  /** The weight is set on the face's 'wght' axis. */
  bool weightOnAxis = false;
  /** Characters the face lacks may come from the system's fallback faces --
   *  off for a matcher that looks only at registered faces. */
  bool fallback = true;
  /** The face is one of the machine's fonts, not one the plugin registered:
   *  set too for the last resort, the interface face. */
  bool fromSystem = false;

  /** A face was found and loaded. */
  bool isValid() const { return typeface && typeface->isValid(); }

  /** A Font of this face at `size` pixels to the em, the weight applied
   *  where an axis can apply it. */
  Font font(float size) const {
    if (!isValid() || !(size > 0.0f)) return Font();
    Font f(typeface, size);
    if (weightOnAxis) f = f.withVariation("wght", (float) weight);
    if (!fallback) f = f.withoutFallback();
    return f;
  }
};

namespace detail {
namespace fontmatch {

/** What the matcher needs to know of a face, from its own tables. */
struct FaceInfo {
  std::vector<std::string> families; // typographic family first, then legacy
  int weight = 400;
  bool italic = false;
  bool variable = false;
  float weightMin = 0.0f, weightMax = 0.0f;
};

inline uint32_t be16(const uint8_t* p) { return (uint32_t) ((p[0] << 8) | p[1]); }
inline uint32_t be32(const uint8_t* p) {
  return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) | ((uint32_t) p[2] << 8) | (uint32_t) p[3];
}

inline std::string lowerAscii(std::string s) {
  for (char& c : s)
    if (c >= 'A' && c <= 'Z') c = (char) (c - 'A' + 'a');
  return s;
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

/** A name record's string: UTF-16BE for the Unicode and Windows platforms,
 *  bytes taken as Latin-1 for the Macintosh one (its family names are ASCII
 *  in every font that has a Windows name as well). */
inline std::string nameString(const uint8_t* p, uint32_t n, bool utf16) {
  std::string out;
  if (!utf16) {
    for (uint32_t i = 0; i < n; ++i) appendUtf8(out, p[i]);
    return out;
  }
  for (uint32_t i = 0; i + 1 < n; i += 2) {
    uint32_t cp = be16(p + i);
    if (cp >= 0xD800 && cp < 0xDC00 && i + 3 < n) {
      const uint32_t low = be16(p + i + 2);
      if (low >= 0xDC00 && low < 0xE000) {
        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
        i += 2;
      }
    }
    appendUtf8(out, cp);
  }
  return out;
}

/** The names a face answers to, from its 'name' table: the typographic
 *  family (ID 16), the legacy family (ID 1), the full name (ID 4) and the
 *  PostScript name (ID 6) -- Illustrator writes font-family="Arial-BoldMT"
 *  and means exactly that face. Each in the best language there is: US
 *  English on the Windows platform, then any Windows or Unicode record, then
 *  Mac. The first is the family a FontMatch reports. */
inline void readNames(const uint8_t* t, uint32_t len, FaceInfo& info) {
  if (len < 6) return;
  const uint32_t count = be16(t + 2), storage = be16(t + 4);
  std::string best[4];
  int score[4] = {0, 0, 0, 0};
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t rec = 6 + i * 12;
    if (rec + 12 > len) break;
    const uint32_t platform = be16(t + rec), encoding = be16(t + rec + 2), language = be16(t + rec + 4);
    const uint32_t id = be16(t + rec + 6), n = be16(t + rec + 8), off = be16(t + rec + 10);
    const int slot = id == 16 ? 0 : (id == 1 ? 1 : (id == 4 ? 2 : (id == 6 ? 3 : -1)));
    if (slot < 0 || (uint64_t) storage + off + n > len) continue;
    int s = 0;
    if (platform == 3 && (encoding == 1 || encoding == 10)) s = language == 0x409 ? 4 : 3;
    else if (platform == 0) s = 3;
    else if (platform == 1 && encoding == 0) s = 1;
    if (s <= score[slot]) continue;
    const std::string name = nameString(t + storage + off, n, platform != 1);
    if (name.empty()) continue;
    best[slot] = name;
    score[slot] = s;
  }
  for (const std::string& name : best)
    if (!name.empty() && std::find(info.families.begin(), info.families.end(), name) == info.families.end())
      info.families.push_back(name);
}

/** Weight and slope from OS/2, the slope from head's macStyle when there is
 *  no OS/2, and a weight axis's range from fvar. */
template <typename GetTable>
inline FaceInfo readInfo(GetTable table) {
  FaceInfo info;
  const uint8_t* p = nullptr;
  uint32_t n = 0;
  if (table(0x6E616D65u, &p, &n)) readNames(p, n, info); // name
  bool sloped = false, haveOs2 = false;
  if (table(0x4F532F32u, &p, &n) && n >= 64) {           // OS/2
    haveOs2 = true;
    int w = (int) be16(p + 4);
    if (w >= 1 && w <= 9) w *= 100; // a few old fonts wrote the 1-9 scale
    if (w >= 1 && w <= 1000) info.weight = w;
    const uint32_t selection = be16(p + 62);
    sloped = (selection & 0x0001) != 0 || (selection & 0x0200) != 0; // ITALIC, OBLIQUE
  }
  if (!haveOs2 && table(0x68656164u, &p, &n) && n >= 46) sloped = (be16(p + 44) & 0x0002) != 0; // head
  info.italic = sloped;
  if (table(0x66766172u, &p, &n) && n >= 16) { // fvar
    const uint32_t axesAt = be16(p + 4), axisCount = be16(p + 8), axisSize = be16(p + 10);
    for (uint32_t i = 0; i < axisCount && axisSize >= 20; ++i) {
      const uint64_t rec = (uint64_t) axesAt + (uint64_t) i * axisSize;
      if (rec + 20 > n) break;
      if (be32(p + rec) != 0x77676874u) continue; // 'wght'
      info.variable = true;
      info.weightMin = (float) (int32_t) be32(p + rec + 4) / 65536.0f;
      info.weightMax = (float) (int32_t) be32(p + rec + 12) / 65536.0f;
    }
  }
  return info;
}

/** A table in a face whose table directory is at `base` of `data`. */
inline bool findTable(const uint8_t* data, size_t size, uint32_t base, uint32_t tag, const uint8_t** out,
                      uint32_t* len) {
  if ((uint64_t) base + 12 > size) return false;
  const uint32_t tables = be16(data + base + 4);
  for (uint32_t i = 0; i < tables; ++i) {
    const uint64_t rec = (uint64_t) base + 12 + (uint64_t) i * 16;
    if (rec + 16 > size) return false;
    if (be32(data + rec) != tag) continue;
    const uint32_t off = be32(data + rec + 8), n = be32(data + rec + 12);
    if (off > size || n > size - off) return false;
    *out = data + off;
    *len = n;
    return true;
  }
  return false;
}

/** Where each face's table directory is: one for a font, several for a
 *  collection. */
inline std::vector<uint32_t> faceOffsets(const uint8_t* data, size_t size) {
  std::vector<uint32_t> out;
  if (size < 12) return out;
  if (be32(data) == 0x74746366u) { // 'ttcf'
    const uint32_t count = be32(data + 8);
    for (uint32_t i = 0; i < count && i < 64; ++i) {
      if ((uint64_t) 12 + (uint64_t) i * 4 + 4 > size) break;
      out.push_back(be32(data + 12 + i * 4));
    }
  } else {
    out.push_back(0);
  }
  return out;
}

/** The two questions the index asks of a font file -- where its faces are,
 *  and where a face's table is -- over the whole file in memory. */
struct WholeFontFile {
  const uint8_t* data;
  size_t size;
  std::vector<uint32_t> faces() const { return faceOffsets(data, size); }
  bool table(uint32_t base, uint32_t tag, const uint8_t** out, uint32_t* len) {
    return findTable(data, size, base, tag, out, len);
  }
};

#if defined(_WIN32)
/**
 * The same two questions answered by reading only what they need: the
 * header, a face's table directory, the tables asked for. Each answer is the
 * one faceOffsets and findTable give over the whole file, check for check --
 * the same bounds, against the file's size, and the first matching record
 * decides.
 *
 * Mapping a file costs three more calls than opening it, and unmapping it is
 * the dearest of them (a sampling profile of the index put 16% of all its
 * time in the unmap alone); a few small reads cost less. A file this cannot
 * open is left to the mapped path, which falls back further.
 */
class FontFileReads {
public:
  explicit FontFileReads(const std::string& path) {
    const std::wstring wide = ::sonore::pathio::widenPath(path.c_str());
    file_ = CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                        FILE_ATTRIBUTE_NORMAL, nullptr);
    LARGE_INTEGER size;
    if (file_ != INVALID_HANDLE_VALUE && GetFileSizeEx(file_, &size) && size.QuadPart > 0)
      size_ = (uint64_t) size.QuadPart;
  }
  ~FontFileReads() {
    if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
  }
  FontFileReads(const FontFileReads&) = delete;
  FontFileReads& operator=(const FontFileReads&) = delete;

  bool ok() const { return file_ != INVALID_HANDLE_VALUE && size_ > 0; }

  /** faceOffsets over the first bytes: it reads no further than 12 + 64 * 4. */
  std::vector<uint32_t> faces() {
    std::vector<uint8_t> head;
    if (!read(0, (uint32_t) (std::min)(size_, (uint64_t) (12 + 64 * 4)), head)) return {};
    return faceOffsets(head.data(), head.size());
  }

  bool table(uint32_t base, uint32_t tag, const uint8_t** out, uint32_t* len) {
    if ((uint64_t) base + 12 > size_) return false;
    if (!haveDirectory_ || directoryBase_ != base) {
      // The records that fit in the file: findTable stops at the first that
      // does not, so a tag beyond them is not found either way.
      haveDirectory_ = false;
      std::vector<uint8_t> head;
      if (!read(base, 12, head)) return false;
      const uint64_t tables = be16(head.data() + 4);
      const uint64_t fit = (size_ - base - 12) / 16;
      const uint64_t records = tables < fit ? tables : fit;
      if (!read((uint64_t) base + 12, (uint32_t) (records * 16), directory_)) return false;
      directoryBase_ = base;
      haveDirectory_ = true;
    }
    for (size_t rec = 0; rec + 16 <= directory_.size(); rec += 16) {
      if (be32(directory_.data() + rec) != tag) continue;
      const uint32_t off = be32(directory_.data() + rec + 8), n = be32(directory_.data() + rec + 12);
      if (off > size_ || n > size_ - off) return false;
      held_.emplace_back();
      if (!read(off, n, held_.back())) return false;
      static const uint8_t kNothing = 0;
      *out = n > 0 ? held_.back().data() : &kNothing;
      *len = n;
      return true;
    }
    return false;
  }

private:
  bool read(uint64_t at, uint32_t n, std::vector<uint8_t>& into) {
    into.resize(n);
    if (n == 0) return true;
    OVERLAPPED where{};
    where.Offset = (DWORD) at;
    where.OffsetHigh = (DWORD) (at >> 32);
    DWORD got = 0;
    return ReadFile(file_, into.data(), n, &got, &where) && got == n;
  }

  HANDLE file_ = INVALID_HANDLE_VALUE;
  uint64_t size_ = 0;
  bool haveDirectory_ = false;
  uint32_t directoryBase_ = 0;
  std::vector<uint8_t> directory_;
  std::vector<std::vector<uint8_t>> held_; // what `table` pointed at, for the file's life
};
#endif

/** CSS's font-weight matching, as an ordering: the smaller key is preferred. */
inline std::pair<int, float> weightKey(float desired, float lo, float hi) {
  if (desired >= lo && desired <= hi) return {0, 0.0f}; // an axis that reaches it, or the exact weight
  const float w = desired < lo ? lo : hi;                // the nearest the face reaches
  if (desired >= 400.0f && desired <= 500.0f) {
    if (w > desired && w <= 500.0f) return {1, w - desired};
    if (w < desired) return {2, desired - w};
    return {3, w - 500.0f};
  }
  if (desired < 400.0f) return w < desired ? std::make_pair(1, desired - w) : std::make_pair(2, w - desired);
  return w > desired ? std::make_pair(1, w - desired) : std::make_pair(2, desired - w);
}

/** The jobs runShared hands out, and what has become of them. Shared with
 *  every thread it started, and freed by the last of them to let go. */
struct SharedRun {
  std::function<void(size_t)> job;
  size_t count = 0;
  std::atomic<size_t> next{0}, finished{0};
  std::vector<unsigned char> failed; // one writer per slot: the thread that claimed it

  void work() {
    for (size_t i = next.fetch_add(1); i < count; i = next.fetch_add(1)) {
      try {
        job(i);
      } catch (...) {
        failed[i] = 1; // done again on the calling thread, where a throw belongs
      }
      finished.fetch_add(1, std::memory_order_release);
    }
  }
};

#if defined(_WIN32)
struct SharedRunStart {
  std::shared_ptr<SharedRun> run;
  HMODULE module = nullptr;
};

inline DWORD WINAPI sharedRunThread(void* arg) {
  HMODULE module = nullptr;
  {
    std::unique_ptr<SharedRunStart> start(static_cast<SharedRunStart*>(arg));
    module = start->module;
    start->run->work();
  }
  // Out through kernel32 with the reference dropped: this thread's code is
  // in a module that may be unloaded the moment it is.
  FreeLibraryAndExitThread(module, 0);
}
#endif

/**
 * job(0) .. job(count - 1), each exactly once, on this thread and on up to
 * threads - 1 more, returning when all have run.
 *
 * The calling thread works through the jobs too and never waits for a
 * thread that has not started -- only for jobs a running thread has taken.
 * That is what makes this safe where a join is not: called while the
 * loader lock is held (a plugin's static initialiser parsing an SVG, inside
 * DllMain), a new thread cannot start until the lock is released, and
 * joining it would hang the host. Here the caller simply does every job
 * itself, and the helpers start later, find nothing left and exit. Each
 * holds a reference to this module until then, so an unload cannot take
 * their code away first.
 */
inline void runShared(size_t count, unsigned threads, std::function<void(size_t)> job) {
  auto run = std::make_shared<SharedRun>();
  run->job = std::move(job);
  run->count = count;
  run->failed.assign(count, 0);
#if defined(_WIN32)
  for (unsigned t = 1; t < threads; ++t) {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCWSTR) &sharedRunThread, &module))
      break;
    SharedRunStart* start = new SharedRunStart();
    start->run = run;
    start->module = module;
    const HANDLE thread = CreateThread(nullptr, 0, sharedRunThread, start, 0, nullptr);
    if (!thread) {
      delete start;
      FreeLibrary(module);
      break;
    }
    CloseHandle(thread);
  }
#else
  (void) threads;
#endif
  run->work();
  while (run->finished.load(std::memory_order_acquire) < count) std::this_thread::yield();
  for (size_t i = 0; i < count; ++i)
    if (run->failed[i]) run->job(i);
}

} // namespace fontmatch
} // namespace detail

/**
 * Resolves a CSS font-family list, a weight and a slope to a typeface: the
 * question an SVG's text asks. How a list is matched, and how to get text that
 * is the same on every machine, is in the file comment.
 *
 * Faces a plugin registers (registerTypeface, registerFile) are found before
 * the machine's; an EmbeddedOnly matcher never looks at the machine at all.
 * The system's font folders are indexed the first time a family is asked for
 * that was not registered, which takes some tens of milliseconds and reads
 * files: never from the audio thread. Every public call takes the matcher's
 * lock, so one matcher may be shared between threads; shared() is the
 * process-wide one Drawable::parse uses when it is given none. Not copyable.
 */
class FontMatcher {
public:
  /** Where a matcher looks for faces. */
  enum Source {
    SystemFonts, ///< Registered faces first, then the machine's.
    EmbeddedOnly ///< Registered faces only: the same everywhere.
  };

  /** A matcher with no faces registered and the platform's generic families
   *  (Arial for sans-serif on Windows, Helvetica on macOS, DejaVu Sans on
   *  Linux). Reads no files: the system index is built on first use. */
  explicit FontMatcher(Source source = SystemFonts) : system_(source == SystemFonts) {
#if defined(_WIN32)
    generic_["serif"] = "Times New Roman, Georgia, Cambria";
    generic_["sans-serif"] = "Arial, Segoe UI, Tahoma, Verdana";
    generic_["monospace"] = "Courier New, Consolas, Lucida Console";
    generic_["cursive"] = "Comic Sans MS, Segoe Script";
    generic_["fantasy"] = "Impact, Gabriola";
    generic_["system-ui"] = "Segoe UI, Arial";
#elif defined(__APPLE__)
    generic_["serif"] = "Times, Times New Roman, Georgia";
    generic_["sans-serif"] = "Helvetica, Arial, Helvetica Neue";
    generic_["monospace"] = "Courier, Menlo, Courier New";
    generic_["cursive"] = "Apple Chancery, Snell Roundhand";
    generic_["fantasy"] = "Papyrus, Impact";
    generic_["system-ui"] = "SF Pro Text, .AppleSystemUIFont, Helvetica Neue, Helvetica";
#else
    generic_["serif"] = "DejaVu Serif, Liberation Serif, Noto Serif, FreeSerif";
    generic_["sans-serif"] = "DejaVu Sans, Liberation Sans, Noto Sans, FreeSans";
    generic_["monospace"] = "DejaVu Sans Mono, Liberation Mono, Noto Sans Mono, FreeMono";
    generic_["cursive"] = "URW Chancery L, Z003";
    generic_["fantasy"] = "Impact";
    generic_["system-ui"] = "Cantarell, Ubuntu, DejaVu Sans, Noto Sans";
#endif
    generic_["ui-serif"] = generic_["serif"];
    generic_["ui-sans-serif"] = generic_["sans-serif"];
    generic_["ui-monospace"] = generic_["monospace"];
    generic_["ui-rounded"] = generic_["sans-serif"];
  }

  FontMatcher(const FontMatcher&) = delete;
  FontMatcher& operator=(const FontMatcher&) = delete;

  /** The process-wide matcher: what Drawable::parse uses when it is given
   *  none. A plugin registers its own faces here once, at startup. */
  static FontMatcher& shared() {
    static FontMatcher matcher;
    return matcher;
  }

  /** True for a SystemFonts matcher, false for EmbeddedOnly. */
  bool usesSystemFonts() const { return system_; }

  /**
   * A face the plugin ships, found under `family` -- or, given "", under the
   * family names its own name table carries. Its weight and slope are read
   * from its OS/2 table unless given (weight above 0, italic 0 or 1). Found
   * before any face of the machine's.
   */
  bool registerTypeface(std::shared_ptr<Typeface> face, const std::string& family = std::string(), int weight = 0,
                        int italic = -1) {
    if (!face || !face->isValid()) return false;
    detail::fontmatch::FaceInfo info = detail::fontmatch::readInfo(
        [&face](uint32_t tag, const uint8_t** p, uint32_t* n) { return face->table(tag, p, n); });
    if (!family.empty()) info.families.insert(info.families.begin(), family);
    if (info.families.empty()) return false;
    if (weight > 0) info.weight = weight;
    if (italic >= 0) info.italic = italic != 0;
    std::lock_guard<std::mutex> lock(mutex_);
    Record r;
    r.info = std::move(info);
    r.face = std::move(face);
    registered_.push_back(std::move(r));
    return true;
  }

  /** The same, from a font file's bytes -- data compiled into the plugin. */
  bool registerTypeface(const uint8_t* data, size_t size, const std::string& family = std::string()) {
    if (data == nullptr || size == 0) return false;
    auto face = std::make_shared<Typeface>();
    if (!face->load(std::vector<uint8_t>(data, data + size))) return false;
    return registerTypeface(std::move(face), family);
  }

  /** The same, from a font file on disk. */
  bool registerFile(const std::string& path, const std::string& family = std::string()) {
    return registerTypeface(loadTypefaceFile(path.c_str()), family);
  }

  /** What a generic family means here: a CSS family list, which may name
   *  registered faces. setGenericFamily("sans-serif", "Inter") makes every
   *  sans-serif in an SVG the plugin's own face. */
  void setGenericFamily(const std::string& generic, const std::string& families) {
    std::lock_guard<std::mutex> lock(mutex_);
    generic_[detail::fontmatch::lowerAscii(generic)] = families;
  }

  /** The family list used where text names none, or names nothing found.
   *  "serif" unless set, as in a browser. */
  void setDefaultFamily(const std::string& families) {
    std::lock_guard<std::mutex> lock(mutex_);
    defaultFamily_ = families;
  }

  /** A CSS font-family value, matched as the header describes. Invalid only
   *  when no face at all can be found. */
  FontMatch match(const std::string& familyList, int weight = 400, bool italic = false) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const int w = weight < 1 ? 1 : (weight > 1000 ? 1000 : weight);
    FontMatch found;
    if (matchList(familyList, w, italic, 0, found)) return found;
    if (matchList(defaultFamily_, w, italic, 0, found)) return found;
    if (!registered_.empty() && !system_) {
      // Nothing named is here, and the system is not to be asked: the
      // first face registered, at the nearest weight of its family.
      if (matchFamily(registered_.front().info.families.front(), w, italic, found)) return found;
    }
    if (system_) {
      found.typeface = systemTypeface();
      found.family = "system";
      found.fromSystem = true;
    }
    return found;
  }

  /** Every family a face can be found under, registered and (for a matcher
   *  that looks) the machine's, sorted. */
  std::vector<std::string> familyNames() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::map<std::string, std::string> names; // by lower case, so "Arial" and "ARIAL" are one
    for (const Record& r : registered_)
      for (const std::string& f : r.info.families) names.emplace(detail::fontmatch::lowerAscii(f), f);
    if (system_) {
      buildIndexLocked();
      for (const Record& r : system_records_)
        for (const std::string& f : r.info.families) names.emplace(detail::fontmatch::lowerAscii(f), f);
    }
    std::vector<std::string> out;
    for (const auto& n : names) out.push_back(n.second);
    return out;
  }

  /** How many faces the machine's font folders hold -- read the first time
   *  it is needed. 0 for a matcher that does not look. */
  size_t systemFaceCount() const {
    if (!system_) return 0;
    std::lock_guard<std::mutex> lock(mutex_);
    buildIndexLocked();
    return system_records_.size();
  }

private:
  /** One face the matcher can choose: what its tables say about it, and
   *  either the face itself (registered, or a system face once loaded) or
   *  where on disk to load it from. */
  struct Record {
    detail::fontmatch::FaceInfo info;
    std::shared_ptr<Typeface> face; // registered; or a system face once loaded
    std::string path;               // a system face's file
    uint32_t index = 0;             // and its place in a collection
  };

  static std::vector<std::string> parseList(const std::string& list) {
    std::vector<std::string> out;
    size_t at = 0;
    while (at <= list.size()) {
      size_t comma = at;
      char quote = 0;
      while (comma < list.size() && (quote || list[comma] != ',')) {
        if (quote && list[comma] == quote) quote = 0;
        else if (!quote && (list[comma] == '"' || list[comma] == '\'')) quote = list[comma];
        ++comma;
      }
      std::string item = list.substr(at, comma - at);
      size_t a = 0, b = item.size();
      while (a < b && (item[a] == ' ' || item[a] == '\t' || item[a] == '\n' || item[a] == '\r')) ++a;
      while (b > a && (item[b - 1] == ' ' || item[b - 1] == '\t' || item[b - 1] == '\n' || item[b - 1] == '\r')) --b;
      item = item.substr(a, b - a);
      if (item.size() >= 2 && (item[0] == '"' || item[0] == '\'') && item.back() == item[0])
        item = item.substr(1, item.size() - 2);
      if (!item.empty()) out.push_back(item);
      at = comma + 1;
    }
    return out;
  }

  bool matchList(const std::string& list, int weight, bool italic, int depth, FontMatch& found) const {
    if (depth > 3) return false; // a generic family defined as itself
    for (const std::string& family : parseList(list)) {
      const std::string key = detail::fontmatch::lowerAscii(family);
      const auto generic = generic_.find(key);
      if (generic != generic_.end()) {
        if (matchList(generic->second, weight, italic, depth + 1, found)) return true;
        continue;
      }
      if (matchFamily(family, weight, italic, found)) return true;
    }
    return false;
  }

  /** The best face of one family: registered faces, then the machine's. */
  bool matchFamily(const std::string& family, int weight, bool italic, FontMatch& found) const {
    const std::string key = detail::fontmatch::lowerAscii(family);
    std::vector<const Record*> candidates;
    for (const Record& r : registered_)
      for (const std::string& f : r.info.families)
        if (detail::fontmatch::lowerAscii(f) == key) {
          candidates.push_back(&r);
          break;
        }
    bool system = false;
    if (candidates.empty() && system_) {
      buildIndexLocked();
      const auto it = systemFamilies_.find(key);
      if (it != systemFamilies_.end())
        for (size_t i : it->second) candidates.push_back(&system_records_[i]);
      system = true;
    }
    if (candidates.empty()) return false;

    // The style first: if the family has the slope asked for, only those.
    bool anyStyle = false;
    for (const Record* r : candidates) anyStyle = anyStyle || r->info.italic == italic;
    const Record* best = nullptr;
    std::pair<int, float> bestKey(99, 0.0f);
    for (const Record* r : candidates) {
      if (anyStyle && r->info.italic != italic) continue;
      const float lo = r->info.variable ? r->info.weightMin : (float) r->info.weight;
      const float hi = r->info.variable ? r->info.weightMax : (float) r->info.weight;
      const std::pair<int, float> k = detail::fontmatch::weightKey((float) weight, lo, hi);
      if (!best || k < bestKey) {
        best = r;
        bestKey = k;
      }
    }
    if (!best) return false;
    std::shared_ptr<Typeface> face = best->face;
    if (!face) face = loadSystemFaceLocked(*best);
    if (!face) return false;
    found = FontMatch();
    found.typeface = std::move(face);
    found.family = best->info.families.front();
    found.italic = best->info.italic;
    found.fromSystem = system;
    found.fallback = system_;
    if (best->info.variable && found.typeface->isVariable()) {
      const float lo = best->info.weightMin, hi = best->info.weightMax;
      const float w = (float) weight < lo ? lo : ((float) weight > hi ? hi : (float) weight);
      found.weight = (int) (w + 0.5f);
      found.weightOnAxis = true;
    } else {
      found.weight = best->info.weight;
    }
    return true;
  }

  /** A system face, read the first time it is chosen: mapped for a single
   *  font or the first face of a collection; for a later face of a
   *  collection, read whole and pointed at that face, because Typeface
   *  reads a collection's first. */
  std::shared_ptr<Typeface> loadSystemFaceLocked(const Record& record) const {
    Record& r = const_cast<Record&>(record);
    if (r.face) return r.face;
    if (r.index == 0) {
      r.face = loadTypefaceFile(r.path.c_str());
      return r.face;
    }
    std::vector<uint8_t> bytes = readFileBytes(r.path.c_str());
    const uint64_t slot = 12ull + 4ull * r.index;
    if (bytes.size() < slot + 4 || bytes.size() < 16) return nullptr;
    std::memcpy(&bytes[12], &bytes[slot], 4); // the first entry of the offset table, now face `index`
    auto face = std::make_shared<Typeface>();
    if (!face->load(std::move(bytes))) return nullptr;
    r.face = std::move(face);
    return r.face;
  }

  /** The machine's font folders, every face in them, read once. */
  void buildIndexLocked() const {
    if (indexed_ || !system_) return;
    indexed_ = true;
    std::vector<std::string> folders;
#if defined(_WIN32)
    const std::string windows = ::sonore::pathio::environment("WINDIR");
    folders.push_back((windows.empty() ? std::string("C:\\Windows") : windows) + "\\Fonts");
    const std::string local = ::sonore::pathio::environment("LOCALAPPDATA");
    if (!local.empty()) folders.push_back(local + "\\Microsoft\\Windows\\Fonts");
#elif defined(__APPLE__)
    folders = {"/System/Library/Fonts", "/Library/Fonts"};
    const std::string home = ::sonore::pathio::environment("HOME");
    if (!home.empty()) folders.push_back(home + "/Library/Fonts");
#elif defined(__ANDROID__)
    // The system's faces, and the ones a device maker adds beside them.
    folders = {"/system/fonts", "/product/fonts", "/system_ext/fonts"};
#else
    folders = {"/usr/share/fonts", "/usr/local/share/fonts"};
    const std::string home = ::sonore::pathio::environment("HOME");
    if (!home.empty()) {
      folders.push_back(home + "/.fonts");
      folders.push_back(home + "/.local/share/fonts");
    }
#endif
    // ── Walked in order, read in parallel, kept in order ──
    //
    // Almost all of the time is the file system's, not the parsing: opening,
    // mapping and closing each file (a sampling profile put ~80% in
    // NtCreateFile, the section calls and NtClose, ~7% in the tables), and
    // the first SVG with text in an editor waited for all of it -- ~20 ms
    // of the showcase's ~50 ms first open on a Windows desktop with a few
    // hundred font files, and in proportion on a designer's machine with
    // thousands. Those calls overlap across threads where one after another
    // they cannot, so the folders are walked first, exactly as before, the
    // files are read on several threads (readFontFiles), and the faces are
    // added in the walk's order -- the order a family's faces are compared
    // in -- with the walk's limits applied as it applied them. The index is
    // the same, face for face.
    std::vector<Step> walk;
    std::vector<std::string> files;
    for (const std::string& folder : folders) planFolder(folder, 0, walk, files);
    std::vector<std::vector<Record>> faces(files.size());
    readFontFiles(files, faces);
    // A folder is entered only while under the cap, as the walk entered it,
    // and skipped with everything below it otherwise.
    for (size_t s = 0; s < walk.size(); ++s) {
      const Step& step = walk[s];
      if (step.folderEnd > 0) {
        if (system_records_.size() > 20000) s = step.folderEnd - 1;
        continue;
      }
      for (Record& r : faces[step.file]) {
        const size_t at = system_records_.size();
        for (const std::string& f : r.info.families) systemFamilies_[detail::fontmatch::lowerAscii(f)].push_back(at);
        system_records_.push_back(std::move(r));
      }
    }
  }

  /** One step of the walk: entering a folder (folderEnd is the step past
   *  everything inside it), or a font file (an index into the file list). */
  struct Step {
    size_t folderEnd = 0;
    size_t file = 0;
  };

  /** The walk the index is built in: each folder's entries in listing
   *  order, a subfolder where the listing has it, six levels deep. */
  static void planFolder(const std::string& folder, int depth, std::vector<Step>& walk,
                         std::vector<std::string>& files) {
    if (depth > 6) return;
    const size_t enter = walk.size();
    walk.push_back(Step());
    for (const FileEntry& e : listDirectory(folder, {}, true, true)) {
      if (e.isDirectory) {
        planFolder(e.path, depth + 1, walk, files);
        continue;
      }
      if (!hasExtension(e.name, {".ttf", ".otf", ".ttc", ".otc"})) continue;
      Step file;
      file.file = files.size();
      files.push_back(e.path);
      walk.push_back(file);
    }
    walk[enter].folderEnd = walk.size();
  }

  /** Every face of one font file that has a family name, in the file's order. */
  static std::vector<Record> readFontFile(const std::string& path) {
#if defined(_WIN32)
    detail::fontmatch::FontFileReads reads(path);
    if (reads.ok()) return facesOf(path, reads);
#endif
    const detail::FontBytes bytes = detail::mapFileBytes(path.c_str());
    if (bytes.empty()) return {};
    detail::fontmatch::WholeFontFile whole{bytes.data(), bytes.size()};
    return facesOf(path, whole);
  }

  template <typename FontFile>
  static std::vector<Record> facesOf(const std::string& path, FontFile& file) {
    std::vector<Record> out;
    const std::vector<uint32_t> faces = file.faces();
    for (uint32_t i = 0; i < (uint32_t) faces.size(); ++i) {
      const uint32_t base = faces[i];
      Record r;
      r.info = detail::fontmatch::readInfo(
          [&](uint32_t tag, const uint8_t** p, uint32_t* n) { return file.table(base, tag, p, n); });
      if (r.info.families.empty()) continue;
      r.path = path;
      r.index = i;
      out.push_back(std::move(r));
    }
    return out;
  }

  /** readFontFile for each file, into its own slot: on Windows on up to
   *  eight threads, this one among them (see detail::fontmatch::runShared);
   *  elsewhere, where opening a file is not what costs, on this one. */
  static void readFontFiles(const std::vector<std::string>& files, std::vector<std::vector<Record>>& faces) {
    unsigned threads = 1;
#if defined(_WIN32)
    const unsigned cores = std::thread::hardware_concurrency();
    threads = (std::min)((std::min)(cores > 1 ? cores : 1u, 8u), (unsigned) (files.size() / 16 + 1));
#endif
    detail::fontmatch::runShared(files.size(), threads,
                                 [&files, &faces](size_t i) { faces[i] = readFontFile(files[i]); });
  }

  const bool system_;
  mutable std::mutex mutex_;
  std::vector<Record> registered_;
  std::unordered_map<std::string, std::string> generic_;
  std::string defaultFamily_ = "serif";
  mutable bool indexed_ = false;
  mutable std::vector<Record> system_records_;
  mutable std::unordered_map<std::string, std::vector<size_t>> systemFamilies_;
};

} // namespace gfx
} // namespace sonore
