// SPDX-License-Identifier: Apache-2.0
//
// Finding a typeface on the machine the plugin is running on.
//
// ── Why the SDK ships no font ───────────────────────────────────────────────
//
// Embedding one would guarantee identical text everywhere, which is the same
// argument that produced the software rasteriser, and it is a genuinely good
// argument. It loses to two others.
//
// A font is 150 KB to 400 KB in every plugin binary, and this SDK's whole
// case against the webview is weight. And every font worth embedding carries
// a licence -- SIL OFL, Apache -- whose terms have to be honoured by a
// commercial product that generates thousands of plugins. That is a decision
// for whoever owns the business, not a default the SDK should make quietly.
//
// So: look for one that is already there. A plugin that needs guaranteed text
// -- a specific face, a specific language -- supplies its own bytes to
// Typeface::load and never calls this.
//
// ── What happens when nothing is found ──────────────────────────────────────
//
// The controls still work. Font handles an invalid typeface by drawing
// nothing, so a knob is still a knob and a slider still drags; only the labels
// are missing. That is a degraded interface rather than a broken one, and it
// says so through Font::isValid() rather than pretending.
#pragma once

#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif (defined(__unix__) || defined(__APPLE__)) && !defined(__EMSCRIPTEN__)
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#define SONORE_MAP_FONT_FILES 1
#endif

#include "truetype.h"
#include "../path_io.h"

namespace sonore {
namespace gfx {

/** Reads a whole file, or an empty vector. */
inline std::vector<uint8_t> readFileBytes(const char* path) {
  std::vector<uint8_t> bytes;
  FILE* f = ::sonore::pathio::open(path, "rb");
  if (!f) return bytes;
  std::fseek(f, 0, SEEK_END);
  const long size = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if (size > 0) {
    bytes.resize((size_t) size);
    if (std::fread(bytes.data(), 1, bytes.size(), f) != bytes.size()) bytes.clear();
  }
  std::fclose(f);
  return bytes;
}

namespace detail {

/**
 * A file's bytes mapped into memory by the system -- read from disk as its
 * pages are touched, and shared with every process mapping the same file --
 * or, where that fails or the platform has no mapping, read whole. Empty
 * when the file cannot be opened.
 *
 * What makes a large face affordable: Windows' CJK faces are 15 to 20 MB
 * each, macOS's colour emoji 190 MB, and a label touches a few pages of
 * them. A mapped file must not be truncated while mapped -- the system's
 * fonts are not, and Windows refuses to while the mapping lasts.
 */
inline FontBytes mapFileBytes(const char* path) {
#if defined(_WIN32)
  const std::wstring wide = ::sonore::pathio::widenPath(path);
  const HANDLE file = CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file != INVALID_HANDLE_VALUE) {
    LARGE_INTEGER size;
    const bool sized = GetFileSizeEx(file, &size) && size.QuadPart > 0 &&
                       (unsigned long long) size.QuadPart <= (unsigned long long) (size_t) -1;
    const HANDLE mapping = sized ? CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr) : nullptr;
    CloseHandle(file); // the mapping keeps the file open
    if (mapping) {
      const void* view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
      CloseHandle(mapping); // and the view the mapping
      if (view) {
        std::shared_ptr<const void> keep(view, [](const void* v) { UnmapViewOfFile(v); });
        return FontBytes((const uint8_t*) view, (size_t) size.QuadPart, std::move(keep));
      }
    }
  }
#elif defined(SONORE_MAP_FONT_FILES)
  const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
  if (fd >= 0) {
    struct stat info;
    void* view = MAP_FAILED;
    size_t size = 0;
    if (fstat(fd, &info) == 0 && S_ISREG(info.st_mode) && info.st_size > 0 &&
        (unsigned long long) info.st_size <= (unsigned long long) (size_t) -1) {
      size = (size_t) info.st_size;
      view = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
    }
    ::close(fd); // the mapping keeps the file
    if (view != MAP_FAILED) {
      std::shared_ptr<const void> keep(view, [size](const void* v) { munmap(const_cast<void*>(v), size); });
      return FontBytes((const uint8_t*) view, size, std::move(keep));
    }
  }
#endif
  return FontBytes(readFileBytes(path));
}

} // namespace detail

/**
 * A typeface read from a font file, mapped rather than copied into memory
 * where the system allows; null when the file cannot be read or is not a
 * font this can read.
 *
 * The file must stay as it is while the typeface lives: one truncated by
 * another program, or on a disk that goes away, faults when the typeface
 * next reads it (SIGBUS, or an in-page error on Windows) -- the system's
 * fonts do not change under a running program, and a plugin's own font
 * sits in its bundle. A file that may change, or that lives on removable
 * media, is better read into memory and given to Typeface::load.
 */
inline std::shared_ptr<Typeface> loadTypefaceFile(const char* path) {
  detail::FontBytes bytes = detail::mapFileBytes(path);
  if (bytes.empty()) return nullptr;
  auto face = std::make_shared<Typeface>();
  if (!face->load(std::move(bytes))) return nullptr;
  return face;
}

/**
 * The system's colour emoji face, or null: Segoe UI Emoji on Windows, Apple
 * Color Emoji on macOS, Noto Color Emoji on Linux -- what Font draws an
 * emoji from, in colour, wherever its text asks for an emoji's look. Read
 * the first time one does, mapped, and kept; a face whose colour this cannot
 * draw -- COLR version 1 alone -- is passed over.
 *
 * [main-thread], though locked, as fallbackTypeface.
 */
inline std::shared_ptr<Typeface> emojiTypeface();

/**
 * The first readable typeface from the usual places, or null.
 *
 * Ordered by how much a UI-shaped face is wanted rather than by what is most
 * likely present: Segoe UI before Arial on Windows, DejaVu before FreeSans on
 * Linux. A plugin's labels look better in the face the desktop uses for its
 * own.
 *
 * Cached, because a host opens and closes an editor repeatedly and parsing a
 * 4000-glyph font each time would be visible.
 */
inline std::shared_ptr<Typeface> systemTypeface() {
  // A function-local static, initialised once however many threads ask at
  // the same moment -- a host may open two editors on two threads. The flag
  // and pointer this used to be were read and written unlocked.
  static const std::shared_ptr<Typeface> cached = [] {
  static const char* const kCandidates[] = {
#if defined(_WIN32)
      "C:/Windows/Fonts/segoeui.ttf",
      "C:/Windows/Fonts/arial.ttf",
      "C:/Windows/Fonts/tahoma.ttf",
      "C:/Windows/Fonts/verdana.ttf",
#elif defined(__APPLE__)
      // .ttc is a COLLECTION and the parser takes its first face, which for
      // these is the regular weight.
      "/System/Library/Fonts/SFNS.ttf",
      "/System/Library/Fonts/Helvetica.ttc",
      "/Library/Fonts/Arial.ttf",
#else
      "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
      "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
      "/usr/share/fonts/truetype/freefont/FreeSans.ttf",
      "/usr/share/fonts/TTF/DejaVuSans.ttf",
      "/usr/share/fonts/dejavu/DejaVuSans.ttf",
#endif
  };

  for (const char* path : kCandidates)
    if (std::shared_ptr<Typeface> face = loadTypefaceFile(path)) return face;
  return std::shared_ptr<Typeface>(); // null: labels are absent, controls still work
  }();
  return cached;
}

namespace detail {

/** A run of code points a fallback face is there for. */
struct FallbackRange {
  uint32_t first, last;
};

/** One face to fall back to, and the scripts it is there for -- none listed
 *  means any character, which only small faces may claim. */
struct FallbackFace {
  const char* path;
  const FallbackRange* ranges;
  size_t count;
  bool covers(uint32_t cp) const {
    if (count == 0) return true;
    for (size_t i = 0; i < count; ++i)
      if (cp >= ranges[i].first && cp <= ranges[i].last) return true;
    return false;
  }
};

namespace fallback_ranges {
// Hangul: the jamo, the compatibility jamo, the syllables -- and the
// parenthesised, circled and half-width forms, which sit among the CJK
// blocks and loaded every CJK face before the Korean one that has them.
inline constexpr FallbackRange kHangul[] = {
    {0x1100, 0x11FF}, {0x3130, 0x318F}, {0x3200, 0x321E}, {0x3260, 0x327E},
    {0xA960, 0xA97F}, {0xAC00, 0xD7FF}, {0xFFA0, 0xFFDC}};
// Kana, Han and the rest of CJK: radicals, symbols and punctuation,
// bopomofo, strokes, enclosed and compatibility forms, the full-width forms
// and the supplementary ideographs.
inline constexpr FallbackRange kCjk[] = {
    {0x2E80, 0x2FDF}, {0x2FF0, 0x312F}, {0x3190, 0x33FF}, {0x3400, 0x4DBF},
    {0x4E00, 0x9FFF}, {0xF900, 0xFAFF}, {0xFE30, 0xFE4F}, {0xFF00, 0xFFEF},
    {0x1B000, 0x1B16F}, {0x1F200, 0x1F2FF}, {0x20000, 0x323AF}};
// The Indic scripts and Sinhala, Ol Chiki, the Vedic signs -- and Chakma,
// Meetei Mayek and Sora Sompeng, which Nirmala UI also has.
inline constexpr FallbackRange kIndic[] = {
    {0x0900, 0x0DFF}, {0x1C50, 0x1C7F}, {0x1CD0, 0x1CFF}, {0xA8E0, 0xA8FF},
    {0xAAE0, 0xAAFF}, {0xABC0, 0xABFF}, {0x110D0, 0x110FF}, {0x11100, 0x1114F}};
inline constexpr FallbackRange kThaiLaoKhmer[] = {
    {0x0E00, 0x0EFF}, {0x1780, 0x17FF}, {0x19E0, 0x19FF}, {0x1A00, 0x1A1F}};
inline constexpr FallbackRange kThai[] = {{0x0E00, 0x0E7F}};
inline constexpr FallbackRange kMyanmar[] = {{0x1000, 0x109F}, {0xA9E0, 0xA9FF}, {0xAA60, 0xAA7F}};
inline constexpr FallbackRange kTibetan[] = {{0x0F00, 0x0FFF}};
inline constexpr FallbackRange kMongolian[] = {{0x1800, 0x18AF}, {0x11660, 0x1167F}};
inline constexpr FallbackRange kYi[] = {{0xA000, 0xA4CF}};
inline constexpr FallbackRange kJavanese[] = {{0xA980, 0xA9DF}};
inline constexpr FallbackRange kTaiLe[] = {{0x1950, 0x197F}};
inline constexpr FallbackRange kNewTaiLue[] = {{0x1980, 0x19DF}};
inline constexpr FallbackRange kPhagsPa[] = {{0xA840, 0xA87F}};
// What Windows 11's Sans Serif Collection has that the faces before it do
// not: Samaritan and Mandaic, the Philippine scripts, Limbu, Buginese, Tai
// Tham, Balinese, Sundanese, Batak, Lepcha, Coptic, Syloti Nagri,
// Saurashtra, Kayah Li, Rejang, Cham, Tai Viet, and the supplementary
// plane's historic and Brahmic scripts.
inline constexpr FallbackRange kSansSerifCollection[] = {
    {0x0800, 0x085F}, {0x1700, 0x177F}, {0x1900, 0x194F}, {0x1A00, 0x1AAF}, {0x1B00, 0x1BFF},
    {0x1C00, 0x1C4F}, {0x1CC0, 0x1CCF}, {0x2C80, 0x2CFF}, {0xA800, 0xA82F}, {0xA880, 0xA8DF},
    {0xA900, 0xA95F}, {0xAA00, 0xAA5F}, {0xAA80, 0xAAFF}, {0x10000, 0x16FFF}, {0x1E000, 0x1E9FF}};
// Cherokee, the Canadian syllabics, Osage.
inline constexpr FallbackRange kGadugi[] = {
    {0x13A0, 0x13FF}, {0x1400, 0x167F}, {0x18B0, 0x18FF}, {0xAB70, 0xABBF}, {0x104B0, 0x104FF}};
// Ethiopic, N'Ko, Tifinagh, Vai, Osmanya, Adlam.
inline constexpr FallbackRange kAfrican[] = {
    {0x07C0, 0x07FF}, {0x1200, 0x139F}, {0x2D30, 0x2D7F}, {0x2D80, 0x2DDF},
    {0xA500, 0xA63F}, {0xAB00, 0xAB2F}, {0x10480, 0x104AF}, {0x1E900, 0x1E95F}};
// Ogham, runes and the historic scripts of the supplementary plane.
inline constexpr FallbackRange kHistoric[] = {{0x1680, 0x16FF}, {0x10000, 0x12FFF}};
// What Segoe UI has and a plugin's own face often lacks: Armenian,
// Georgian, Lisu.
inline constexpr FallbackRange kSegoe[] = {
    {0x0530, 0x058F}, {0x10A0, 0x10FF}, {0x1C90, 0x1CBF}, {0x2D00, 0x2D2F}, {0xA4D0, 0xA4FF}};
inline constexpr FallbackRange kGeorgian[] = {{0x10A0, 0x10FF}, {0x2D00, 0x2D2F}};
inline constexpr FallbackRange kThaana[] = {{0x0780, 0x07BF}};
inline constexpr FallbackRange kEmoji[] = {
    {0x2300, 0x23FF}, {0x2600, 0x27BF}, {0x2B00, 0x2BFF}, {0x1F000, 0x1FAFF}};
} // namespace fallback_ranges

/** What fallbackTypeface has read and answered, process-wide. */
struct FallbackState {
  std::mutex lock;
  // By PATH, so a file listed for two scripts is read once; null where it
  // was missing or did not parse.
  std::unordered_map<std::string, std::shared_ptr<Typeface>> loaded;
  std::unordered_map<uint32_t, std::shared_ptr<Typeface>> answers; // null: a miss
};
inline FallbackState& fallbackState() {
  static FallbackState state;
  return state;
}
} // namespace detail

/** How many fallback face files have been read so far, whether or not they
 *  parsed -- what a character has cost in file reads and memory. For a test,
 *  or for a plugin that wants to know. */
inline size_t fallbackFacesRead() {
  detail::FallbackState& state = detail::fallbackState();
  std::lock_guard<std::mutex> guard(state.lock);
  return state.loaded.size();
}

#define SONORE_FALLBACK(path, table) \
  FallbackFace { path, table, sizeof(table) / sizeof(table[0]) }
#define SONORE_FALLBACK_ANY(path) \
  FallbackFace { path, nullptr, 0 }

/**
 * A typeface on this machine with a glyph for `codepoint`, or null.
 *
 * The face a plugin draws with covers the scripts its designer had in mind;
 * a preset a user named in Japanese, a sample folder in Korean, a file
 * browser listing anything at all, does not ask. Without a fallback each of
 * those characters drew as the empty .notdef box. These are the faces the
 * desktop itself falls back to.
 *
 * Each is asked only about the scripts it is there for -- Hangul goes to the
 * Korean face, kana and Han to the Japanese and then the Chinese ones, Thai
 * to Leelawadee -- and read the first time a character in those scripts
 * needs it. The first version asked every face in turn and kept each one it
 * read: one Thai letter or one newer emoji in a preset name loaded every CJK
 * face on the way past, 47 to 87 MB held for the life of the process, in
 * every plugin. A face listed for no script in particular may be asked about
 * any character, and only small ones are: a character nothing covers costs
 * those few MB once, and is then remembered as a miss. A face that does not
 * parse is passed over.
 *
 * [main-thread], though locked: a host may run two editors on two threads.
 */
inline std::shared_ptr<Typeface> fallbackTypeface(uint32_t codepoint) {
  // Not a character: a surrogate half, or past U+10FFFF -- what broken UTF-8
  // decodes to. Nothing covers these, and they are not worth a lookup.
  if (codepoint > 0x10FFFF || (codepoint >= 0xD800 && codepoint <= 0xDFFF)) return nullptr;
  using namespace detail::fallback_ranges;
  using detail::FallbackFace;
  static const FallbackFace kCandidates[] = {
#if defined(_WIN32)
      SONORE_FALLBACK_ANY("C:/Windows/Fonts/seguisym.ttf"),
      SONORE_FALLBACK("C:/Windows/Fonts/malgun.ttf", kHangul),
      SONORE_FALLBACK("C:/Windows/Fonts/YuGothM.ttc", kCjk),
      SONORE_FALLBACK("C:/Windows/Fonts/msyh.ttc", kCjk),
      SONORE_FALLBACK("C:/Windows/Fonts/msjh.ttc", kCjk),
      SONORE_FALLBACK("C:/Windows/Fonts/Nirmala.ttc", kIndic),
      SONORE_FALLBACK("C:/Windows/Fonts/Nirmala.ttf", kIndic),
      SONORE_FALLBACK("C:/Windows/Fonts/LeelawUI.ttf", kThaiLaoKhmer),
      SONORE_FALLBACK("C:/Windows/Fonts/tahoma.ttf", kThai),
      SONORE_FALLBACK("C:/Windows/Fonts/mmrtext.ttf", kMyanmar),
      SONORE_FALLBACK("C:/Windows/Fonts/himalaya.ttf", kTibetan),
      SONORE_FALLBACK("C:/Windows/Fonts/monbaiti.ttf", kMongolian),
      SONORE_FALLBACK("C:/Windows/Fonts/msyi.ttf", kYi),
      SONORE_FALLBACK("C:/Windows/Fonts/javatext.ttf", kJavanese),
      SONORE_FALLBACK("C:/Windows/Fonts/taile.ttf", kTaiLe),
      SONORE_FALLBACK("C:/Windows/Fonts/ntailu.ttf", kNewTaiLue),
      SONORE_FALLBACK("C:/Windows/Fonts/phagspa.ttf", kPhagsPa),
      SONORE_FALLBACK("C:/Windows/Fonts/gadugi.ttf", kGadugi),
      SONORE_FALLBACK("C:/Windows/Fonts/ebrima.ttf", kAfrican),
      SONORE_FALLBACK("C:/Windows/Fonts/seguihis.ttf", kHistoric),
      SONORE_FALLBACK("C:/Windows/Fonts/SansSerifCollection.ttf", kSansSerifCollection),
      SONORE_FALLBACK("C:/Windows/Fonts/segoeui.ttf", kSegoe),
      SONORE_FALLBACK("C:/Windows/Fonts/sylfaen.ttf", kGeorgian),
      SONORE_FALLBACK("C:/Windows/Fonts/mvboli.ttf", kThaana),
      SONORE_FALLBACK("C:/Windows/Fonts/seguiemj.ttf", kEmoji),
      SONORE_FALLBACK_ANY("C:/Windows/Fonts/arial.ttf"),
#elif defined(__APPLE__)
      SONORE_FALLBACK_ANY("/System/Library/Fonts/Apple Symbols.ttf"),
      SONORE_FALLBACK("/System/Library/Fonts/AppleSDGothicNeo.ttc", kHangul),
      // Hiragino's file name is Japanese; CFF outlines, which Typeface reads.
      SONORE_FALLBACK("/System/Library/Fonts/\xE3\x83\x92\xE3\x83\xA9\xE3\x82\xAE\xE3\x83\x8E"
                      "\xE8\xA7\x92\xE3\x82\xB4\xE3\x82\xB7\xE3\x83\x83\xE3\x82\xAF W3.ttc", kCjk),
      SONORE_FALLBACK("/System/Library/Fonts/PingFang.ttc", kCjk),
      SONORE_FALLBACK_ANY("/System/Library/Fonts/Supplemental/Arial.ttf"),
      // Last, and for any character: the one face here with nearly every
      // script in it, and the largest.
      SONORE_FALLBACK_ANY("/System/Library/Fonts/Supplemental/Arial Unicode.ttf"),
      SONORE_FALLBACK_ANY("/Library/Fonts/Arial Unicode.ttf"),
#else
      // Not DejaVu Sans, which is the face systemTypeface picks here: a
      // character it lacks, it lacks.
      SONORE_FALLBACK_ANY("/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf"),
      // Noto Sans CJK, where most distributions put it: CFF outlines, for
      // Han, kana and Hangul alike.
      SONORE_FALLBACK("/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc", kHangul),
      SONORE_FALLBACK("/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc", kCjk),
      SONORE_FALLBACK("/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc", kHangul),
      SONORE_FALLBACK("/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc", kCjk),
      SONORE_FALLBACK("/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc", kHangul),
      SONORE_FALLBACK("/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc", kCjk),
      SONORE_FALLBACK("/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf", kCjk),
      SONORE_FALLBACK("/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf", kHangul),
      SONORE_FALLBACK("/usr/share/fonts/truetype/wqy/wqy-microhei.ttc", kCjk),
      SONORE_FALLBACK("/usr/share/fonts/truetype/wqy/wqy-microhei.ttc", kHangul),
      SONORE_FALLBACK("/usr/share/fonts/wqy-microhei/wqy-microhei.ttc", kCjk),
      SONORE_FALLBACK("/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc", kCjk),
      SONORE_FALLBACK_ANY("/usr/share/fonts/truetype/freefont/FreeSans.ttf"),
      SONORE_FALLBACK_ANY("/usr/share/fonts/truetype/freefont/FreeSerif.ttf"),
      SONORE_FALLBACK_ANY("/usr/share/fonts/truetype/unifont/unifont.ttf"),
#endif
  };
  constexpr size_t kCount = sizeof(kCandidates) / sizeof(kCandidates[0]);
  detail::FallbackState& state = detail::fallbackState();
  auto& loaded = state.loaded;
  auto& answers = state.answers;
  std::lock_guard<std::mutex> guard(state.lock);
  const auto known = answers.find(codepoint);
  if (known != answers.end()) return known->second;
  for (size_t i = 0; i < kCount; ++i) {
    const FallbackFace& candidate = kCandidates[i];
    if (!candidate.covers(codepoint)) continue;
    auto it = loaded.find(candidate.path);
    if (it == loaded.end()) {
      it = loaded.emplace(candidate.path, loadTypefaceFile(candidate.path)).first;
    }
    if (it->second && it->second->glyphForChar(codepoint) != 0) {
      answers[codepoint] = it->second;
      return it->second;
    }
  }
  answers[codepoint] = nullptr;
  return nullptr;
}

inline std::shared_ptr<Typeface> emojiTypeface() {
  static const char* const kCandidates[] = {
#if defined(_WIN32)
      "C:/Windows/Fonts/seguiemj.ttf",
#elif defined(__APPLE__)
      "/System/Library/Fonts/Apple Color Emoji.ttc",
#else
      "/usr/share/fonts/truetype/noto/NotoColorEmoji.ttf",
      "/usr/share/fonts/noto/NotoColorEmoji.ttf",
      "/usr/share/fonts/google-noto-emoji/NotoColorEmoji.ttf",
      "/usr/share/fonts/google-noto-color-emoji-fonts/NotoColorEmoji.ttf",
      "/usr/share/fonts/truetype/noto-color-emoji/NotoColorEmoji.ttf",
      "/usr/share/fonts/noto-emoji/NotoColorEmoji.ttf",
#endif
  };
  detail::FallbackState& state = detail::fallbackState();
  std::lock_guard<std::mutex> guard(state.lock);
  for (const char* path : kCandidates) {
    auto it = state.loaded.find(path);
    if (it == state.loaded.end()) it = state.loaded.emplace(path, loadTypefaceFile(path)).first;
    if (!it->second || !it->second->hasColourGlyphs()) continue;
    // Drawable, proved on a grinning face: its layers, or a PNG picture.
    // Pictures in a form this does not decode -- LZFSE-compressed 'emjc',
    // which newer Apple Color Emoji may hold -- would draw every emoji blank.
    const Typeface& face = *it->second;
    const uint16_t grin = face.glyphForChar(0x1F600);
    ImageLayer picture;
    if (grin != 0 && (!face.colourLayers(grin).empty() || face.colourImage(grin, 64.0f, &picture)))
      return it->second;
  }
  return nullptr;
}

#undef SONORE_FALLBACK
#undef SONORE_FALLBACK_ANY
#undef SONORE_MAP_FONT_FILES

} // namespace gfx
} // namespace sonore
