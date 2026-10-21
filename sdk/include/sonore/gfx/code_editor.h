// SPDX-License-Identifier: Apache-2.0
//
// A source code editor: a document kept as lines, tokenisers that colour
// it, and the component that shows and edits it.
//
// ── Why a plugin has one ────────────────────────────────────────────────────
//
// A plugin that runs a user's script -- a Lua patch, a formula for a
// waveshaper, a live-coded sequencer -- or that lets somebody edit the XML of
// a preset or a mapping by hand needs a place to type that is not a text
// field. Code is long, indented, line-based and read by its colours; a
// TextArea that wraps it, eats its tabs and redraws all of it per keystroke
// fails at every one of those.
//
// ── Lines, not a buffer ─────────────────────────────────────────────────────
//
// TextArea keeps one vector of characters and wraps it again on change,
// which is right for a paragraph. A source file is a hundred thousand
// lines: inserting a character in one flat buffer moves megabytes, and every
// question a caret asks becomes a search for line starts. CodeDocument keeps
// a string per line, so an edit touches its own line and a line number is
// an index. Absolute offsets -- what find and a listener speak -- come from
// line starts computed lazily and invalidated only from the edited line
// down: an edit near the end of a long file does not recount its beginning.
//
// ── Line endings ────────────────────────────────────────────────────────────
//
// Each line remembers how it ended -- LF, CR LF or a lone CR -- so a file
// with mixed endings saves byte for byte as it loaded, and a diff of an edit
// shows the line edited rather than every line. A line break is ONE
// character in every offset whatever bytes it is saved as, so a caret never
// stands between a CR and its LF. New breaks -- typed, pasted, inserted --
// take the document's newline, which loading sets to the file's commonest
// ending; normaliseLineEndings() brings the rest into line, undoably.
//
// ── Undo is commands here, not snapshots ────────────────────────────────────
//
// undo.h keeps whole states, and says why that is right for a plugin's
// state. For a document of megabytes a snapshot per keystroke is the wrong
// trade, so every edit is recorded as the text it inserted or removed,
// grouped into transactions (newTransaction), adjacent typing merged into
// one record, and the history bounded by a budget of characters.
//
// ── Colouring a line at a time ──────────────────────────────────────────────
//
// A tokeniser reads ONE line, given the state the line before it ended in --
// inside a block comment, a raw string, a string continued with a backslash
// -- and returns the state this line ends in. The editor keeps those states,
// one per line, valid down to the first line edited since: painting the
// bottom of a file scans the lines above it once, without building tokens,
// and an edit re-scans from its own line. Tokens are only built for the lines
// on screen.
//
// ── Monospaced, on a grid ───────────────────────────────────────────────────
//
// Every character stands in a column -- a tab reaches the next stop, an East
// Asian wide character takes two, a combining mark none -- and is drawn at
// its column's x. A caret on a grid is arithmetic, and a font that is not
// monospaced still lines up, drawn a character at a time. The price is that
// no bidirectional reordering or Arabic joining happens: a grid of cells
// cannot show those, and a caret moving over the cells could not follow them.
#pragma once

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <exception>
#include <functional>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "../path_io.h"
#include "clipboard.h"
#include "system_font.h"
#include "viewport.h"
#include "widgets.h"

namespace sonore {
namespace gfx {

namespace detail {
namespace code {

/** One code point from UTF-8, strictly: an overlong form, a surrogate, a
 *  value past U+10FFFF or a broken sequence is U+FFFD for its first byte,
 *  and the walk goes on at the next -- a file with one bad byte loads with
 *  one replacement character rather than not at all. */
inline char32_t decodeNext(const unsigned char* s, size_t n, size_t& i) {
  const unsigned c = s[i];
  if (c < 0x80) {
    ++i;
    return (char32_t) c;
  }
  size_t extra = 0;
  char32_t cp = 0, least = 0;
  if (c >= 0xC2 && c <= 0xDF) {
    extra = 1;
    cp = c & 0x1Fu;
    least = 0x80;
  } else if (c >= 0xE0 && c <= 0xEF) {
    extra = 2;
    cp = c & 0x0Fu;
    least = 0x800;
  } else if (c >= 0xF0 && c <= 0xF4) {
    extra = 3;
    cp = c & 0x07u;
    least = 0x10000;
  } else {
    ++i;
    return 0xFFFD;
  }
  for (size_t k = 1; k <= extra; ++k) {
    if (i + k >= n || (s[i + k] & 0xC0u) != 0x80u) {
      ++i;
      return 0xFFFD;
    }
    cp = (cp << 6) | (char32_t) (s[i + k] & 0x3Fu);
  }
  if (cp < least || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
    ++i;
    return 0xFFFD;
  }
  i += extra + 1;
  return cp;
}

inline void appendUtf8(std::string& out, char32_t cp) {
  if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
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

/** A line break inside the document's own record of an edit: one code point
 *  per break, past U+10FFFF where no character can be, so a CR break
 *  followed by an LF break is never read back as one CR LF. */
constexpr char32_t kBreakLF = 0x110000;
constexpr char32_t kBreakCRLF = 0x110001;
constexpr char32_t kBreakCR = 0x110002;
inline bool isBreak(char32_t c) { return c >= kBreakLF && c <= kBreakCR; }

/** UTF-8 out: a break marker as LF. */
inline std::string encode(const char32_t* s, size_t n) {
  std::string out;
  out.reserve(n);
  for (size_t i = 0; i < n; ++i) appendUtf8(out, isBreak(s[i]) ? (char32_t) '\n' : s[i]);
  return out;
}

/** UTF-8 in, every line break -- CR LF, CR, LF -- as one LF. */
inline std::u32string decodeText(const std::string& utf8) {
  std::u32string out;
  out.reserve(utf8.size());
  const unsigned char* s = (const unsigned char*) utf8.data();
  const size_t n = utf8.size();
  for (size_t i = 0; i < n;) {
    char32_t c = decodeNext(s, n, i);
    if (c == '\r') {
      if (i < n && s[i] == '\n') ++i;
      c = '\n';
    }
    out.push_back(c);
  }
  return out;
}

/** A character drawn in no column of its own: a combining mark, a joiner, a
 *  variation selector -- what belongs to the character before it. */
inline bool isZeroWidth(char32_t c) {
  return c >= 0x300 && c <= 0x10FFFF &&
         (c == 0x200D || shaping::isNonspacingMark((uint32_t) c) || shaping::isIgnorable((uint32_t) c));
}

/** East Asian wide and fullwidth characters, which take two columns on a
 *  grid -- the ranges every terminal agrees on. */
inline bool isWide(char32_t c) {
  if (c < 0x1100) return false;
  return c <= 0x115F || (c >= 0x2E80 && c <= 0x303E) || (c >= 0x3041 && c <= 0x33FF) ||
         (c >= 0x3400 && c <= 0x4DBF) || (c >= 0x4E00 && c <= 0x9FFF) || (c >= 0xA000 && c <= 0xA4CF) ||
         (c >= 0xAC00 && c <= 0xD7A3) || (c >= 0xF900 && c <= 0xFAFF) || (c >= 0xFE30 && c <= 0xFE4F) ||
         (c >= 0xFF00 && c <= 0xFF60) || (c >= 0xFFE0 && c <= 0xFFE6) || (c >= 0x1F300 && c <= 0x1F64F) ||
         (c >= 0x1F900 && c <= 0x1F9FF) || (c >= 0x20000 && c <= 0x2FFFD) || (c >= 0x30000 && c <= 0x3FFFD);
}

inline bool isControl(char32_t c) { return c < 0x20 || (c >= 0x7F && c < 0xA0); }

/** Whether a caret may stand before character i: not before a mark or a
 *  joiner, nor after a zero-width joiner -- inside a cluster a caret has no
 *  cell to be drawn in. */
inline bool isStop(const std::u32string& s, size_t i) {
  if (i == 0 || i >= s.size()) return true;
  return !isZeroWidth(s[i]) && s[i - 1] != 0x200D;
}

/** The columns character i takes, at column `col`. */
inline int columnsOf(const std::u32string& s, size_t i, int col, int tab) {
  const char32_t c = s[i];
  if (c == '\t') return tab - col % tab;
  if (!isStop(s, i)) return 0;
  return isWide(c) ? 2 : 1;
}

/** 0 for a space, 1 for a word character, 2 for anything else: what a word
 *  move and a double click divide a line by. */
inline int charClass(char32_t c) {
  if (c == ' ' || c == '\t' || c == 0xA0 || c == 0x3000 || (c >= 0x2000 && c <= 0x200A)) return 0;
  if (c < 0x80)
    return ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_') ? 1 : 2;
  if (c > 0x10FFFF) return 2;
  return (shaping::continuesWord((uint32_t) c) || shaping::isDecimal((uint32_t) c)) ? 1 : 2;
}

/** Case folding for a search that ignores case: ASCII, Latin-1, Greek and
 *  Cyrillic -- the alphabets with simple one-to-one cases. */
inline char32_t fold(char32_t c) {
  if (c >= 'A' && c <= 'Z') return c + 32;
  if (c < 0xC0) return c;
  if (c <= 0xDE && c != 0xD7) return c + 32;
  if (c >= 0x391 && c <= 0x3A9 && c != 0x3A2) return c + 32;
  if (c >= 0x410 && c <= 0x42F) return c + 32;
  if (c >= 0x400 && c <= 0x40F) return c + 80;
  return c;
}

/** What an ASCII character is to a scanner, as bits -- 1 space, 2 an
 *  identifier's first character, 4 any of an identifier's, 8 a decimal
 *  digit: one load instead of a row of comparisons, in the loops that read
 *  every character above the screen. */
inline uint8_t asciiKind(char32_t c) {
  static const uint8_t kKinds[128] = {
      0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 0, 0, 0, 0, 0, 0,
      0, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 0, 0, 0, 0, 6,
      0, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 0, 0, 0, 0, 0};
  return c < 0x80 ? kKinds[c] : (uint8_t) 0;
}

/** Space a tokeniser skips between tokens. */
inline bool isSpace(char32_t c) {
  if (c < 0x80) return (asciiKind(c) & 1) != 0;
  return c == 0xA0 || c == 0xFEFF || c == 0x3000 || (c >= 0x2000 && c <= 0x200B);
}
inline bool isDigit(char32_t c) { return c >= '0' && c <= '9'; }
inline bool isHexDigit(char32_t c) {
  return isDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
inline bool isAsciiLetter(char32_t c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
/** Letters, '_' and a word character past ASCII: an identifier's first. */
inline bool isIdentifierStart(char32_t c) {
  if (c < 0x80) return (asciiKind(c) & 2) != 0;
  return charClass(c) == 1;
}
inline bool isIdentifierBody(char32_t c) {
  if (c < 0x80) return (asciiKind(c) & 4) != 0;
  return charClass(c) == 1 || isZeroWidth(c);
}

/** `s[0..n)` in a sorted table of ASCII words, n at most 16. */
inline bool isWordIn(const char32_t* s, int n, const char* const* table, size_t count) {
  if (n <= 0 || n > 16) return false;
  char word[17];
  for (int i = 0; i < n; ++i) {
    if (s[i] >= 0x80) return false;
    word[i] = (char) s[i];
  }
  word[n] = 0;
  size_t lo = 0, hi = count;
  while (lo < hi) {
    const size_t mid = (lo + hi) / 2;
    const int c = std::strcmp(table[mid], word);
    if (c == 0) return true;
    if (c < 0) lo = mid + 1;
    else hi = mid;
  }
  return false;
}

/** A C++ raw string's delimiter, as the 28 bits a line's state has room for.
 *  The delimiter itself does not fit, and a state that held a pointer would
 *  not be a value: two delimiters sharing a hash would need to share it
 *  among the 16 characters a delimiter may have AND be closed by a `)` that
 *  happens to spell the other one -- a wrong colour, once, by chance. */
inline uint32_t delimiterHash(const char32_t* s, int n) {
  uint32_t h = 2166136261u;
  for (int i = 0; i < n; ++i) {
    h ^= (uint32_t) s[i];
    h *= 16777619u;
  }
  h ^= (uint32_t) n;
  h *= 16777619u;
  return h >> 4;
}

} // namespace code
} // namespace detail

/**
 * A monospaced typeface from the usual places, or null: Consolas, Courier
 * New or Lucida Console on Windows; Menlo, SF Mono or Monaco on macOS; DejaVu
 * Sans Mono, Liberation Mono, Noto Sans Mono or FreeMono on Linux. Read once
 * and kept, as systemTypeface() is. The editor below uses it when nobody has
 * given it a font.
 */
inline std::shared_ptr<Typeface> monospacedTypeface() {
  static const std::shared_ptr<Typeface> cached = [] {
    static const char* const kCandidates[] = {
#if defined(_WIN32)
        "C:/Windows/Fonts/consola.ttf",
        "C:/Windows/Fonts/cour.ttf",
        "C:/Windows/Fonts/lucon.ttf",
#elif defined(__APPLE__)
        "/System/Library/Fonts/Menlo.ttc",
        "/System/Library/Fonts/SFNSMono.ttf",
        "/System/Library/Fonts/Monaco.ttf",
        "/System/Library/Fonts/Supplemental/Courier New.ttf",
#else
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
        "/usr/share/fonts/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
        "/usr/share/fonts/truetype/noto/NotoSansMono-Regular.ttf",
        "/usr/share/fonts/truetype/freefont/FreeMono.ttf",
#endif
    };
    for (const char* path : kCandidates)
      if (std::shared_ptr<Typeface> face = loadTypefaceFile(path)) return face;
    return std::shared_ptr<Typeface>();
  }();
  return cached;
}

// ═════════════════════════════════════════════════════════════════════════════
// CodeDocument
// ═════════════════════════════════════════════════════════════════════════════

/**
 * Text kept as lines, with positions that move with edits, transactions of
 * undo, line endings kept per line, find, and listeners.
 *
 * Text goes in and out as UTF-8 and is kept as code points. Offsets count
 * code points with every line break as one, so they agree with the text
 * getTextBetween returns, whose breaks are always LF.
 */
class CodeDocument {
public:
  /** How one line ends, kept per line; also the document's newline, what new
   *  line breaks are made of. */
  enum class LineEnding : uint8_t {
    None, ///< No break: the document's last line.
    LF,   ///< A line feed.
    CRLF, ///< A carriage return then a line feed.
    CR    ///< A carriage return alone.
  };
  /** What a file was read as, and is written back as. */
  enum class Encoding : uint8_t {
    Utf8,        ///< UTF-8 with no byte order mark: what bytes without a mark are read as.
    Utf8WithBom, ///< UTF-8 after the EF BB BF mark, which a save writes back.
    Utf16LE,     ///< UTF-16, little-endian, after the FF FE mark.
    Utf16BE      ///< UTF-16, big-endian, after the FE FF mark.
  };

  /** Characters the document holds at most unless setMaximumSize says
   *  otherwise: 32 Mi code points, 128 MB of text. */
  static constexpr int kDefaultMaximumSize = 32 * 1024 * 1024;

  // ── Positions ────────────────────────────────────────────────────────────

  /**
   * A place in the document: a line, and an index in it.
   *
   * An ordinary Position is a value and goes stale when text before it
   * changes. A MAINTAINED one is kept up to date by the document through
   * every insert and delete -- which is what a caret, a selection's anchor or
   * a bookmark needs to be. A copy is not maintained; assigning to a
   * maintained Position keeps it maintained. Text inserted exactly at a
   * maintained position pushes it along, as typing pushes a caret.
   */
  class Position {
  public:
    /** In no document: line 0, index 0, offset 0, until a Position in a
     *  document is assigned to it. */
    Position() noexcept = default;
    /** At a 0-based line and a 0-based index in it, in code points, held
     *  inside the document: a line past the end is the end, an index past
     *  the line's length is its end. Not maintained. */
    Position(const CodeDocument& document, int line, int indexInLine) noexcept
        : owner_(const_cast<CodeDocument*>(&document)), line_(line), index_(indexInLine) {
      owner_->clampPosition(line_, index_);
    }
    /** At an offset from the start, in code points with a line break as
     *  one, held to [0, getNumCharacters()]. Not maintained. */
    Position(const CodeDocument& document, int offset) noexcept : owner_(const_cast<CodeDocument*>(&document)) {
      setPosition(offset);
    }
    /** The same place in the same document, NOT maintained even when
     *  `other` is. */
    Position(const Position& other) noexcept : owner_(other.owner_), line_(other.line_), index_(other.index_) {}
    /** Takes the other's document and place. This one stays maintained if
     *  it was -- now by the other's document, so not at all when the other
     *  is in none. */
    Position& operator=(const Position& other) {
      if (this != &other) {
        const bool keep = maintained_;
        if (owner_ != other.owner_) setPositionMaintained(false);
        owner_ = other.owner_;
        line_ = other.line_;
        index_ = other.index_;
        setPositionMaintained(keep);
      }
      return *this;
    }
    /** A maintained Position stops being maintained: the document forgets
     *  it. */
    ~Position() { setPositionMaintained(false); }

    /** The same document, line and index. */
    bool operator==(const Position& o) const { return owner_ == o.owner_ && line_ == o.line_ && index_ == o.index_; }
    /** A different document, line or index. */
    bool operator!=(const Position& o) const { return !(*this == o); }
    /** Earlier: by line, then by index. The documents are not compared, so
     *  the order means something only within one document. */
    bool operator<(const Position& o) const { return line_ < o.line_ || (line_ == o.line_ && index_ < o.index_); }
    /** Later, by line then index; the documents are not compared. */
    bool operator>(const Position& o) const { return o < *this; }
    /** Not later, by line then index; the documents are not compared. */
    bool operator<=(const Position& o) const { return !(o < *this); }
    /** Not earlier, by line then index; the documents are not compared. */
    bool operator>=(const Position& o) const { return !(*this < o); }

    /** The line, 0-based. */
    int getLineNumber() const { return line_; }
    /** The index in the line, 0-based, in code points: from 0 to the line's
     *  length, which is the place after its last character. */
    int getIndexInLine() const { return index_; }
    /** The offset from the start of the document. */
    int getPosition() const { return owner_ ? owner_->offsetOf(line_, index_) : 0; }
    /** The document, or null: a default-made Position, or one whose document
     *  was destroyed while it was maintained. */
    const CodeDocument* getOwner() const { return owner_; }

    /** Moves to a 0-based line and index, held inside the document as the
     *  constructor holds them; in no document, kept as given. */
    void setLineAndIndex(int line, int indexInLine) {
      line_ = line;
      index_ = indexInLine;
      if (owner_) owner_->clampPosition(line_, index_);
    }
    /** Moves to an offset, held to [0, getNumCharacters()]. Nothing happens
     *  in no document. */
    void setPosition(int offset) {
      if (owner_) owner_->lineAndIndexOf(offset, &line_, &index_);
    }

    /** Forwards or back by characters, a line break counting as one. Walks
     *  line by line rather than through offsets, so moving by one near the
     *  end of a long document does not count its beginning. */
    void moveBy(int characterDelta) {
      if (!owner_) return;
      owner_->clampPosition(line_, index_);
      long long remaining = characterDelta;
      while (remaining > 0) {
        const int length = owner_->getLineLength(line_);
        const long long room = length - index_;
        if (remaining <= room) {
          index_ += (int) remaining;
          return;
        }
        if (line_ + 1 >= owner_->getNumLines()) {
          index_ = length;
          return;
        }
        remaining -= room + 1;
        ++line_;
        index_ = 0;
      }
      while (remaining < 0) {
        if (-remaining <= index_) {
          index_ += (int) remaining;
          return;
        }
        if (line_ == 0) {
          index_ = 0;
          return;
        }
        remaining += index_ + 1;
        --line_;
        index_ = owner_->getLineLength(line_);
      }
    }
    /** A copy moved as moveBy moves; the copy is not maintained. */
    Position movedBy(int characterDelta) const {
      Position p(*this);
      p.moveBy(characterDelta);
      return p;
    }
    /** Up or down by lines, the index held to the new line's length. */
    Position movedByLines(int deltaLines) const {
      Position p(*this);
      p.setLineAndIndex(line_ + deltaLines, index_);
      return p;
    }

    /** The character here: '\n' at the end of a line that has a break, 0 at
     *  the end of the document. */
    char32_t getCharacter() const {
      if (!owner_) return 0;
      int line = line_, index = index_;
      owner_->clampPosition(line, index);
      const std::u32string& text = owner_->getLineCodePoints(line);
      if ((size_t) index < text.size()) return text[(size_t) index];
      return line + 1 < owner_->getNumLines() ? (char32_t) '\n' : (char32_t) 0;
    }
    /** The UTF-8 text of this position's line, without its break; empty in
     *  no document. */
    std::string getLineText() const { return owner_ ? owner_->getLine(line_) : std::string(); }

    /** Starts or stops following edits. A maintained Position is held by
     *  address in the document's list until it stops, is destroyed, or the
     *  document is; in no document it cannot be maintained. */
    void setPositionMaintained(bool maintain) {
      if (maintain == maintained_) return;
      if (!owner_) {
        maintained_ = false;
        return;
      }
      maintained_ = maintain;
      std::vector<Position*>& all = owner_->positions_;
      if (maintain) {
        owner_->clampPosition(line_, index_);
        all.push_back(this);
      } else {
        all.erase(std::remove(all.begin(), all.end(), this), all.end());
      }
    }
    /** Whether edits move it: false for a copy and after its document has
     *  gone. */
    bool isPositionMaintained() const { return maintained_; }

  private:
    friend class CodeDocument;
    CodeDocument* owner_ = nullptr;
    int line_ = 0, index_ = 0;
    bool maintained_ = false;
  };

  // ── Listeners ────────────────────────────────────────────────────────────

  /**
   * Told about every change, after it is made and after maintained positions
   * have moved.
   *
   * Offsets rather than the text itself: loading a hundred thousand lines
   * would otherwise build a second copy of them for every listener, and one
   * that wants the text asks getTextBetween. Undo and redo are told as the
   * inserts and deletes they are.
   */
  class Listener {
  public:
    /** A listener does not take itself out of the document: removeListener
     *  it before it is destroyed. */
    virtual ~Listener() = default;
    /** Text now occupies [startIndex, endIndex). */
    virtual void codeDocumentTextInserted(int startIndex, int endIndex) {
      (void) startIndex;
      (void) endIndex;
    }
    /** The text that was at [startIndex, endIndex) is gone. */
    virtual void codeDocumentTextDeleted(int startIndex, int endIndex) {
      (void) startIndex;
      (void) endIndex;
    }
    /** The same changes counted in lines, for a cache kept per line: lines
     *  from `firstLine` on may read differently, and `lineDelta` lines were
     *  added after it -- or, negative, removed. A line ending changed alone
     *  is a change with a delta of zero. Called before the two above. */
    virtual void codeDocumentLinesChanged(int firstLine, int lineDelta) {
      (void) firstLine;
      (void) lineDelta;
    }
  };

  /** Not owned. Null, or one already added, is ignored; listeners are told
   *  in the order they were added. */
  void addListener(Listener* l) {
    if (l && std::find(listeners_.begin(), listeners_.end(), l) == listeners_.end()) listeners_.push_back(l);
  }
  /** May be called from inside a listener's callback: a listener removed
   *  there is not told the rest of the change. */
  void removeListener(Listener* l) {
    listeners_.erase(std::remove(listeners_.begin(), listeners_.end(), l), listeners_.end());
  }

  // ── Construction ─────────────────────────────────────────────────────────

  /** Empty: one empty line, LF for new breaks, UTF-8 for saving. */
  CodeDocument() : lines_(1), starts_(1, 0) {}
  /** Positions still maintained are let go -- a caret outliving its document
   *  is a mistake, but it should not be one that writes into freed memory. */
  ~CodeDocument() {
    for (Position* p : positions_) {
      p->owner_ = nullptr;
      p->maintained_ = false;
    }
  }
  /** Not copyable: maintained positions and listeners point at it. */
  CodeDocument(const CodeDocument&) = delete;
  /** Not assignable, for the same reason; loadFromText or
   *  replaceAllContent take another's text. */
  CodeDocument& operator=(const CodeDocument&) = delete;

  // ── Reading ──────────────────────────────────────────────────────────────

  /** At least 1: an empty document is one empty line, and text that ends in
   *  a line break has an empty line after it. */
  int getNumLines() const { return (int) lines_.size(); }
  /** In code points, each line break counted as one, whatever it is saved
   *  as. */
  int getNumCharacters() const { return numChars_; }
  /** A 0-based line's length in code points, without its break; 0 for a
   *  line out of range. */
  int getLineLength(int line) const {
    return line >= 0 && line < getNumLines() ? (int) lines_[(size_t) line].text.size() : 0;
  }
  /** The line's code points, without its break; empty past the end. */
  const std::u32string& getLineCodePoints(int line) const {
    static const std::u32string kEmpty;
    return line >= 0 && line < getNumLines() ? lines_[(size_t) line].text : kEmpty;
  }
  /** The line as UTF-8, without its break. */
  std::string getLine(int line) const {
    const std::u32string& t = getLineCodePoints(line);
    return detail::code::encode(t.data(), t.size());
  }
  /** How the line ends; None for the last. */
  LineEnding getLineEnding(int line) const {
    return line >= 0 && line < getNumLines() ? lines_[(size_t) line].ending : LineEnding::None;
  }
  /** The offset the line starts at. */
  int getLineStart(int line) const { return offsetOf(line, 0); }

  /** Everything, each line with its own ending, as UTF-8. */
  std::string getAllContent() const {
    std::string out;
    out.reserve((size_t) numChars_ + lines_.size());
    for (const Line& l : lines_) {
      for (char32_t c : l.text) detail::code::appendUtf8(out, c);
      out += endingChars(l.ending);
    }
    return out;
  }

  /** The text between two positions, in either order, breaks as LF. */
  std::string getTextBetween(const Position& a, const Position& b) const {
    int l1 = a.line_, i1 = a.index_, l2 = b.line_, i2 = b.index_;
    clampPosition(l1, i1);
    clampPosition(l2, i2);
    if (l2 < l1 || (l2 == l1 && i2 < i1)) {
      std::swap(l1, l2);
      std::swap(i1, i2);
    }
    std::string out;
    for (int k = l1; k <= l2; ++k) {
      const std::u32string& t = lines_[(size_t) k].text;
      const size_t from = k == l1 ? (size_t) i1 : 0, to = k == l2 ? (size_t) i2 : t.size();
      for (size_t i = from; i < to; ++i) detail::code::appendUtf8(out, t[i]);
      if (k < l2) out.push_back('\n');
    }
    return out;
  }
  /** The same between two offsets, each held inside the document. */
  std::string getTextBetween(int startOffset, int endOffset) const {
    return getTextBetween(Position(*this, startOffset), Position(*this, endOffset));
  }

  /** The longest line, in code points. Counted again after an edit, the
   *  first time somebody asks. */
  int getMaximumLineLength() const {
    if (maxLineLength_ < 0) {
      size_t most = 0;
      for (const Line& l : lines_) most = (std::max)(most, l.text.size());
      maxLineLength_ = (int) most;
    }
    return maxLineLength_;
  }

  /** Counts every change, undo and redo included: what a cache compares. */
  uint64_t getVersion() const { return version_; }

  // ── Editing ──────────────────────────────────────────────────────────────

  /**
   * Insert UTF-8 text. Its line breaks -- LF, CR LF or CR -- become the
   * document's newline. Refused whole, returning false, if it would take the
   * document past its maximum size.
   */
  bool insertText(const Position& at, const std::string& utf8) {
    int line = at.line_, index = at.index_;
    clampPosition(line, index);
    return insertRawRecorded(line, index, toRaw(utf8));
  }
  /** The same at an offset, held inside the document. */
  bool insertText(int offset, const std::string& utf8) { return insertText(Position(*this, offset), utf8); }

  /** Delete between two positions, in either order. */
  void deleteSection(const Position& a, const Position& b) {
    int l1 = a.line_, i1 = a.index_, l2 = b.line_, i2 = b.index_;
    clampPosition(l1, i1);
    clampPosition(l2, i2);
    if (l2 < l1 || (l2 == l1 && i2 < i1)) {
      std::swap(l1, l2);
      std::swap(i1, i2);
    }
    if (l1 == l2 && i1 == i2) return;
    std::u32string removed = removeRaw(l1, i1, l2, i2);
    record(Edit::Remove, l1, i1, l2, i2, std::move(removed));
  }
  /** The same between two offsets, in either order. */
  void deleteSection(int startOffset, int endOffset) {
    deleteSection(Position(*this, startOffset), Position(*this, endOffset));
  }

  /** Delete then insert, as one change: refused whole if the result would
   *  be past the maximum size, so a refused replace leaves the text it would
   *  have replaced. */
  bool replaceSection(const Position& a, const Position& b, const std::string& utf8) {
    int l1 = a.line_, i1 = a.index_, l2 = b.line_, i2 = b.index_;
    clampPosition(l1, i1);
    clampPosition(l2, i2);
    if (l2 < l1 || (l2 == l1 && i2 < i1)) {
      std::swap(l1, l2);
      std::swap(i1, i2);
    }
    const std::u32string raw = toRaw(utf8);
    const long long removing = (long long) offsetOf(l2, i2) - offsetOf(l1, i1);
    if ((long long) numChars_ - removing + (long long) raw.size() > (long long) maxSize_) return false;
    if (l1 != l2 || i1 != i2) {
      std::u32string removed = removeRaw(l1, i1, l2, i2);
      record(Edit::Remove, l1, i1, l2, i2, std::move(removed));
    }
    return insertRawRecorded(l1, i1, raw);
  }
  /** The same between two offsets, in either order. */
  bool replaceSection(int startOffset, int endOffset, const std::string& utf8) {
    return replaceSection(Position(*this, startOffset), Position(*this, endOffset), utf8);
  }

  /** Everything replaced, undoably, as one transaction of its own. */
  bool replaceAllContent(const std::string& utf8) {
    newTransaction();
    const bool ok = replaceSection(Position(*this, 0, 0), Position(*this, getNumLines() - 1, INT_MAX), utf8);
    newTransaction();
    return ok;
  }

  // ── Loading and saving ───────────────────────────────────────────────────

  /**
   * Replace everything with the contents of a file's bytes -- not undoably:
   * the history is cleared and the save point set, as opening a file does.
   *
   * A UTF-8 byte order mark, or a UTF-16 one of either order, chooses the
   * encoding and is remembered for saving; without one the bytes are UTF-8.
   * Each line keeps the ending it had, and the document's newline becomes the
   * commonest of them. Every position goes to the start. False, with the
   * document unchanged, past the maximum size.
   */
  bool loadFromText(const std::string& bytes) {
    const unsigned char* b = (const unsigned char*) bytes.data();
    const size_t n = bytes.size();
    Encoding encoding = Encoding::Utf8;
    size_t at = 0;
    if (n >= 3 && b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF) {
      encoding = Encoding::Utf8WithBom;
      at = 3;
    } else if (n >= 2 && b[0] == 0xFF && b[1] == 0xFE) {
      encoding = Encoding::Utf16LE;
      at = 2;
    } else if (n >= 2 && b[0] == 0xFE && b[1] == 0xFF) {
      encoding = Encoding::Utf16BE;
      at = 2;
    }
    std::vector<Line> lines(1);
    long long chars = 0;
    bool afterCR = false, tooBig = false;
    size_t counts[4] = {0, 0, 0, 0};
    auto take = [&](char32_t c) {
      if (c == '\n' && afterCR) {
        lines[lines.size() - 2].ending = LineEnding::CRLF;
        --counts[(int) LineEnding::CR];
        ++counts[(int) LineEnding::CRLF];
        afterCR = false;
        return;
      }
      afterCR = false;
      if (++chars > maxSize_) tooBig = true;
      if (c == '\r' || c == '\n') {
        const LineEnding e = c == '\r' ? LineEnding::CR : LineEnding::LF;
        lines.back().ending = e;
        ++counts[(int) e];
        lines.emplace_back();
        afterCR = c == '\r';
        return;
      }
      lines.back().text.push_back(c);
    };
    if (encoding == Encoding::Utf16LE || encoding == Encoding::Utf16BE) {
      const bool le = encoding == Encoding::Utf16LE;
      auto unit = [&](size_t i) -> uint32_t {
        return le ? (uint32_t) b[i] | ((uint32_t) b[i + 1] << 8) : ((uint32_t) b[i] << 8) | (uint32_t) b[i + 1];
      };
      size_t i = at;
      for (; i + 1 < n && !tooBig; i += 2) {
        uint32_t u = unit(i);
        if (u >= 0xD800 && u <= 0xDBFF) {
          const uint32_t low = i + 3 < n ? unit(i + 2) : 0u;
          if (low >= 0xDC00 && low <= 0xDFFF) {
            u = 0x10000u + ((u - 0xD800u) << 10) + (low - 0xDC00u);
            i += 2;
          } else {
            u = 0xFFFD;
          }
        } else if (u >= 0xDC00 && u <= 0xDFFF) {
          u = 0xFFFD;
        }
        take((char32_t) u);
      }
      if (i < n && !tooBig) take(0xFFFD); // an odd byte left over
    } else {
      // A line at a time, each string made at its length: grown a character
      // at a time, a forty-character line was reallocated six times on the
      // way, and that was most of what opening a large file cost.
      lines.clear();
      lines.reserve((size_t) std::count(b + at, b + n, (unsigned char) '\n') + 1u);
      for (size_t i = at;;) {
        size_t end = i;
        unsigned high = 0;
        while (end < n && b[end] != '\n' && b[end] != '\r') high |= b[end++];
        Line line;
        if (high < 0x80) {
          line.text.assign(b + i, b + end); // ASCII: each byte is its code point
        } else {
          line.text.reserve(end - i);
          for (size_t k = i; k < end;) line.text.push_back(detail::code::decodeNext(b, end, k));
        }
        chars += (long long) line.text.size();
        if (end >= n) {
          lines.push_back(std::move(line));
          break;
        }
        if (b[end] == '\r' && end + 1 < n && b[end + 1] == '\n') {
          line.ending = LineEnding::CRLF;
          i = end + 2;
        } else {
          line.ending = b[end] == '\r' ? LineEnding::CR : LineEnding::LF;
          i = end + 1;
        }
        ++counts[(int) line.ending];
        ++chars;
        lines.push_back(std::move(line));
        if (chars > maxSize_) break;
      }
      tooBig = chars > maxSize_;
    }
    if (tooBig) return false;

    const int oldChars = numChars_, oldLines = getNumLines();
    lines_.swap(lines);
    numChars_ = (int) chars;
    encoding_ = encoding;
    starts_.assign(lines_.size(), 0);
    startsValid_ = 1;
    maxLineLength_ = -1;
    ++version_;
    // The newline is the file's commonest ending, so what is typed into a
    // CR LF file is CR LF.
    if (counts[1] + counts[2] + counts[3] > 0) {
      LineEnding most = LineEnding::LF;
      if (counts[2] > counts[(int) most]) most = LineEnding::CRLF;
      if (counts[3] > counts[(int) most]) most = LineEnding::CR;
      newLine_ = most;
    }
    for (Position* p : positions_) p->line_ = p->index_ = 0;
    undo_.clear();
    redo_.clear();
    undoCost_ = 0;
    open_ = false;
    baseId_ = ++lastId_;
    savedId_ = baseId_;
    notify(0, 0, oldChars, -(oldLines - 1), false);
    notify(0, 0, numChars_, getNumLines() - 1, true);
    return true;
  }

  /** loadFromText, from a file at a UTF-8 path. False if it cannot be read
   *  or is past the maximum size. */
  bool loadFromFile(const std::string& path) {
    std::FILE* f = ::sonore::pathio::open(path.c_str(), "rb");
    if (!f) return false;
    std::string bytes;
    char chunk[65536];
    size_t got = 0;
    const size_t limit = (size_t) maxSize_ * 4u + 4u;
    while ((got = std::fread(chunk, 1, sizeof(chunk), f)) > 0) {
      bytes.append(chunk, got);
      if (bytes.size() > limit) {
        std::fclose(f);
        return false;
      }
    }
    const bool failed = std::ferror(f) != 0;
    std::fclose(f);
    return !failed && loadFromText(bytes);
  }

  /** The bytes a save writes: each line's own ending, in the encoding the
   *  document was loaded as, with its byte order mark if it had one. */
  std::string getEncodedContent() const {
    if (encoding_ == Encoding::Utf8) return getAllContent();
    if (encoding_ == Encoding::Utf8WithBom) return std::string("\xEF\xBB\xBF") + getAllContent();
    const bool le = encoding_ == Encoding::Utf16LE;
    std::string out;
    out.reserve((size_t) numChars_ * 2u + 4u);
    auto unit = [&](uint32_t u) {
      const char hi = (char) (u >> 8), lo = (char) (u & 0xFF);
      out.push_back(le ? lo : hi);
      out.push_back(le ? hi : lo);
    };
    unit(0xFEFF);
    for (const Line& l : lines_) {
      for (char32_t c : l.text) {
        uint32_t cp = (uint32_t) c;
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
        if (cp >= 0x10000) {
          cp -= 0x10000;
          unit(0xD800u + (cp >> 10));
          unit(0xDC00u + (cp & 0x3FFu));
        } else {
          unit(cp);
        }
      }
      for (char c : endingChars(l.ending)) unit((uint32_t) (unsigned char) c);
    }
    return out;
  }

  /** Writes getEncodedContent to a UTF-8 path and sets the save point. */
  bool saveToFile(const std::string& path) {
    const std::string bytes = getEncodedContent();
    std::FILE* f = ::sonore::pathio::open(path.c_str(), "wb");
    if (!f) return false;
    const bool wrote = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    const bool closed = std::fclose(f) == 0;
    if (!wrote || !closed) return false;
    setSavePoint();
    return true;
  }

  /** What getEncodedContent and saveToFile write: the encoding the last
   *  load found, Utf8 for a document never loaded. */
  Encoding getEncoding() const { return encoding_; }
  /** Changes what a save writes, a UTF-16 file saved as UTF-8 say. The text
   *  is not touched, nothing is recorded for undo, and the document does
   *  not count as changed. */
  void setEncoding(Encoding e) { encoding_ = e; }

  // ── Line endings ─────────────────────────────────────────────────────────

  /** "\n", "\r\n" or "\r": what new line breaks are made of. Anything else
   *  is ignored. */
  void setNewLineCharacters(const std::string& chars) {
    if (chars == "\n") newLine_ = LineEnding::LF;
    else if (chars == "\r\n") newLine_ = LineEnding::CRLF;
    else if (chars == "\r") newLine_ = LineEnding::CR;
  }
  /** The newline as the characters a save writes for it: LF, CR LF or CR. */
  std::string getNewLineCharacters() const { return endingChars(newLine_); }
  /** The newline: LF until a load or setNewLineCharacters changes it. */
  LineEnding getNewLineEnding() const { return newLine_; }

  /** Whether the lines end in more than one way. */
  bool hasMixedLineEndings() const {
    LineEnding seen = LineEnding::None;
    for (const Line& l : lines_) {
      if (l.ending == LineEnding::None) continue;
      if (seen != LineEnding::None && l.ending != seen) return true;
      seen = l.ending;
    }
    return false;
  }

  /** Every line break made the document's newline, as one transaction. */
  void normaliseLineEndings() {
    newTransaction();
    for (int k = 0; k + 1 < getNumLines(); ++k) {
      const LineEnding before = lines_[(size_t) k].ending;
      if (before == newLine_) continue;
      setEndingRaw(k, newLine_);
      Edit e;
      e.kind = Edit::Ending;
      e.line = k;
      e.before = before;
      e.after = newLine_;
      recordEdit(std::move(e));
    }
    newTransaction();
  }

  // ── Undo ─────────────────────────────────────────────────────────────────

  /** Ends the transaction being recorded: what follows is a new undo step. */
  void newTransaction() { open_ = false; }

  /** Whether undo() has a transaction to take back. */
  bool canUndo() const { return !undo_.empty(); }
  /** Whether redo() has one to play again. Any new edit empties the redo
   *  list. */
  bool canRedo() const { return !redo_.empty(); }
  /** The transactions undo() can take back, at most what setUndoLimits
   *  allows. */
  int getNumUndoTransactions() const { return (int) undo_.size(); }

  /** Undoes the last transaction. Maintained positions move with what it
   *  puts back and takes away, as they did with the edits. */
  bool undo() {
    open_ = false;
    if (undo_.empty()) return false;
    Transaction t = std::move(undo_.back());
    undo_.pop_back();
    undoCost_ -= t.cost;
    replaying_ = true;
    for (auto it = t.edits.rbegin(); it != t.edits.rend(); ++it) {
      if (it->kind == Edit::Insert) {
        removeRaw(it->line, it->index, it->endLine, it->endIndex);
      } else if (it->kind == Edit::Remove) {
        int el = 0, ei = 0;
        insertRaw(it->line, it->index, it->text, &el, &ei);
      } else {
        setEndingRaw(it->line, it->before);
      }
    }
    replaying_ = false;
    redo_.push_back(std::move(t));
    return true;
  }

  /** Plays the last undone transaction again; false with none. Maintained
   *  positions move as they did with the edits. */
  bool redo() {
    open_ = false;
    if (redo_.empty()) return false;
    Transaction t = std::move(redo_.back());
    redo_.pop_back();
    replaying_ = true;
    for (const Edit& e : t.edits) {
      if (e.kind == Edit::Insert) {
        int el = 0, ei = 0;
        insertRaw(e.line, e.index, e.text, &el, &ei);
      } else if (e.kind == Edit::Remove) {
        int el = 0, ei = 0;
        endOf(e.line, e.index, e.text, &el, &ei);
        removeRaw(e.line, e.index, el, ei);
      } else {
        setEndingRaw(e.line, e.after);
      }
    }
    replaying_ = false;
    undoCost_ += t.cost;
    undo_.push_back(std::move(t));
    return true;
  }

  /** Forgets every step. The text stays as it is, and so does whether it
   *  counts as saved. */
  void clearUndoHistory() {
    const bool saved = !hasChangedSinceSavePoint();
    undo_.clear();
    redo_.clear();
    undoCost_ = 0;
    open_ = false;
    baseId_ = ++lastId_;
    if (saved) savedId_ = baseId_;
  }

  /** How many transactions are kept, and how many characters of text they
   *  may hold between them; past either, the oldest go. The newest is kept
   *  whatever its size. */
  void setUndoLimits(int maxTransactions, int maxCharacters) {
    maxTransactions_ = (size_t) (std::max)(1, maxTransactions);
    undoBudget_ = (size_t) (std::max)(1, maxCharacters);
    trimHistory();
  }

  /** Marks the text as saved. hasChangedSinceSavePoint is then false until
   *  an edit, and false again if undo comes back here. */
  void setSavePoint() {
    open_ = false;
    savedId_ = currentStateId();
  }
  /** True once the text differs from the state at the last setSavePoint,
   *  saveToFile or loadFromText; false again when undo or redo comes back
   *  to that state. */
  bool hasChangedSinceSavePoint() const { return currentStateId() != savedId_; }

  // ── Size ─────────────────────────────────────────────────────────────────

  /** The most code points the document may hold: an insert, a replace or a
   *  load that would pass it is refused whole. Held to [1, INT_MAX / 4];
   *  text already longer is not cut. */
  void setMaximumSize(int characters) { maxSize_ = (std::max)(1, (std::min)(characters, INT_MAX / 4)); }
  /** In code points; kDefaultMaximumSize unless set. */
  int getMaximumSize() const { return maxSize_; }

  // ── Find ─────────────────────────────────────────────────────────────────

  /**
   * The next match of `needle` -- forwards from `from`, including a match
   * starting there, or backwards for one starting before it -- going round
   * the end if asked. A needle with line breaks matches across lines.
   * Ignoring case folds ASCII, Latin-1, Greek and Cyrillic.
   */
  bool find(const std::string& needle, const Position& from, bool forwards, bool caseSensitive, bool wrapAround,
            Position* matchStart, Position* matchEnd) const {
    std::u32string n = detail::code::decodeText(needle);
    if (n.empty()) return false;
    if (!caseSensitive)
      for (char32_t& c : n) c = detail::code::fold(c);
    std::vector<std::u32string> parts(1);
    for (char32_t c : n) {
      if (c == '\n') parts.emplace_back();
      else parts.back().push_back(c);
    }
    int line = from.line_, index = from.index_;
    clampPosition(line, index);
    const int count = getNumLines();
    int foundLine = -1, foundIndex = -1;
    auto tryLine = [&](int k, int lo, int hi) -> bool { // a start in [lo, hi] on line k
      const int at = matchOnLine(parts, caseSensitive, k, lo, hi, forwards);
      if (at < 0) return false;
      foundLine = k;
      foundIndex = at;
      return true;
    };
    bool found = false;
    if (forwards) {
      for (int k = line; k < count && !found; ++k) found = tryLine(k, k == line ? index : 0, INT_MAX);
      for (int k = 0; wrapAround && k <= line && !found; ++k) found = tryLine(k, 0, k == line ? index - 1 : INT_MAX);
    } else {
      for (int k = line; k >= 0 && !found; --k) found = tryLine(k, 0, k == line ? index - 1 : INT_MAX);
      for (int k = count - 1; wrapAround && k >= line && !found; --k) found = tryLine(k, k == line ? index : 0, INT_MAX);
    }
    if (!found) return false;
    int endLine = foundLine + (int) parts.size() - 1;
    int endIndex = parts.size() == 1 ? foundIndex + (int) parts[0].size() : (int) parts.back().size();
    if (matchStart) *matchStart = Position(*this, foundLine, foundIndex);
    if (matchEnd) *matchEnd = Position(*this, endLine, endIndex);
    return true;
  }

  // ── Words ────────────────────────────────────────────────────────────────

  /** Where a word move to the right lands: past the run of word characters
   *  or of punctuation here and the spaces after it -- or, from spaces, past
   *  them; from a line's end, onto the next line past its indent. */
  Position findWordBreakAfter(const Position& pos) const {
    int line = pos.line_, index = pos.index_;
    clampPosition(line, index);
    const std::u32string* t = &lines_[(size_t) line].text;
    auto skipSpaces = [&] {
      while ((size_t) index < t->size() && detail::code::charClass((*t)[(size_t) index]) == 0) ++index;
    };
    if ((size_t) index >= t->size()) {
      if (line + 1 >= getNumLines()) return Position(*this, line, index);
      ++line;
      index = 0;
      t = &lines_[(size_t) line].text;
      skipSpaces();
      return Position(*this, line, index);
    }
    const int cls = detail::code::charClass((*t)[(size_t) index]);
    if (cls == 0) {
      skipSpaces();
      return Position(*this, line, index);
    }
    while ((size_t) index < t->size() && sameClass((*t)[(size_t) index], cls)) ++index;
    skipSpaces();
    return Position(*this, line, index);
  }

  /** Where a word move to the left lands: back over spaces -- and at most
   *  one line break, which stops it -- then over the run before them. */
  Position findWordBreakBefore(const Position& pos) const {
    int line = pos.line_, index = pos.index_;
    clampPosition(line, index);
    if (index == 0) {
      if (line == 0) return Position(*this, 0, 0);
      return Position(*this, line - 1, getLineLength(line - 1));
    }
    const std::u32string& t = lines_[(size_t) line].text;
    while (index > 0 && detail::code::charClass(t[(size_t) index - 1]) == 0) --index;
    if (index > 0) {
      const int cls = detail::code::charClass(t[(size_t) index - 1]);
      while (index > 0 && sameClass(t[(size_t) index - 1], cls)) --index;
    }
    return Position(*this, line, index);
  }

  /** The run a double click selects: the word, or the punctuation or the
   *  spaces, at a position -- or the one just before it at a line's end. */
  void findTokenContaining(const Position& pos, Position& start, Position& end) const {
    int line = pos.line_, index = pos.index_;
    clampPosition(line, index);
    const std::u32string& t = lines_[(size_t) line].text;
    if (t.empty()) {
      start = end = Position(*this, line, 0);
      return;
    }
    size_t at = (size_t) index;
    if (at >= t.size() || (detail::code::charClass(t[at]) != 1 && at > 0 && detail::code::charClass(t[at - 1]) == 1))
      at = at > 0 ? at - 1 : 0;
    const int cls = detail::code::charClass(t[at]);
    size_t a = at, b = at + 1;
    while (a > 0 && sameClass(t[a - 1], cls)) --a;
    while (b < t.size() && sameClass(t[b], cls)) ++b;
    start = Position(*this, line, (int) a);
    end = Position(*this, line, (int) b);
  }

  /** The line a position is on, its break included where it has one. */
  void findLineContaining(const Position& pos, Position& start, Position& end) const {
    int line = pos.line_, index = pos.index_;
    clampPosition(line, index);
    start = Position(*this, line, 0);
    end = line + 1 < getNumLines() ? Position(*this, line + 1, 0) : Position(*this, line, getLineLength(line));
  }

  // ── Offsets ──────────────────────────────────────────────────────────────

  /** A line and an index, held inside the document. */
  void clampPosition(int& line, int& index) const {
    const int count = getNumLines();
    if (line < 0) {
      line = 0;
      index = 0;
    } else if (line >= count) {
      line = count - 1;
      index = INT_MAX;
    }
    const int length = (int) lines_[(size_t) line].text.size();
    if (index < 0) index = 0;
    if (index > length) index = length;
  }

  /** The offset of a line and an index, both held inside the document. Line
   *  starts are counted lazily, from the first line an edit invalidated
   *  down to `line` only. */
  int offsetOf(int line, int index) const {
    clampPosition(line, index);
    validateStarts(line);
    return starts_[(size_t) line] + index;
  }

  /** The line and index of an offset, held inside the document. */
  void lineAndIndexOf(int offset, int* line, int* index) const {
    if (offset <= 0) {
      *line = 0;
      *index = 0;
      return;
    }
    if (offset >= numChars_) {
      *line = getNumLines() - 1;
      *index = getLineLength(*line);
      return;
    }
    // Valid starts reach past the offset first -- only as far as needed, so
    // a caret near an edit far down a long file does not recount the rest.
    const int count = getNumLines();
    while (startsValid_ < count &&
           starts_[(size_t) startsValid_ - 1] + (int) lines_[(size_t) startsValid_ - 1].text.size() < offset)
      validateStarts(startsValid_);
    size_t lo = 0, hi = (size_t) startsValid_;
    while (hi - lo > 1) {
      const size_t mid = (lo + hi) / 2;
      if (starts_[mid] <= offset) lo = mid;
      else hi = mid;
    }
    *line = (int) lo;
    *index = (std::min)(offset - starts_[lo], (int) lines_[lo].text.size());
  }

private:
  /** One line's characters, without its break, and the break that ends it
   *  (None on the last line). Each line keeps its own, so a file with mixed
   *  endings is written back exactly as it was read. */
  struct Line {
    std::u32string text;
    LineEnding ending = LineEnding::None;
  };
  /** One recorded change: text inserted or removed between (line, index) and
   *  (endLine, endIndex), with line breaks held in `text` as break markers;
   *  or, for Ending, one line's break changed from `before` to `after`. Undo
   *  plays a transaction's edits backwards, redo forwards. */
  struct Edit {
    enum Kind : uint8_t { Insert, Remove, Ending };
    Kind kind = Insert;
    int line = 0, index = 0, endLine = 0, endIndex = 0;
    std::u32string text; // break markers for line breaks
    LineEnding before = LineEnding::None, after = LineEnding::None;
  };
  /** One undo step: the edits recorded since newTransaction, with adjacent
   *  typing and deleting merged. `id` names the document state it leaves
   *  behind (what the saved-point check compares); `cost` is its share of the
   *  undo budget, in characters plus a fixed charge per edit. */
  struct Transaction {
    std::vector<Edit> edits;
    uint64_t id = 0;
    size_t cost = 0;
  };

  static std::string endingChars(LineEnding e) {
    switch (e) {
      case LineEnding::LF: return "\n";
      case LineEnding::CRLF: return "\r\n";
      case LineEnding::CR: return "\r";
      case LineEnding::None: break;
    }
    return std::string();
  }
  static char32_t markerOf(LineEnding e) {
    return e == LineEnding::CRLF ? detail::code::kBreakCRLF
                                 : (e == LineEnding::CR ? detail::code::kBreakCR : detail::code::kBreakLF);
  }
  static LineEnding endingOf(char32_t marker) {
    return marker == detail::code::kBreakCRLF ? LineEnding::CRLF
                                              : (marker == detail::code::kBreakCR ? LineEnding::CR : LineEnding::LF);
  }
  static bool sameClass(char32_t c, int cls) {
    return detail::code::charClass(c) == cls || (cls != 0 && detail::code::isZeroWidth(c));
  }

  /** UTF-8 to the record's form: breaks as markers of the newline. */
  std::u32string toRaw(const std::string& utf8) const {
    std::u32string raw = detail::code::decodeText(utf8);
    const char32_t marker = markerOf(newLine_);
    for (char32_t& c : raw)
      if (c == '\n') c = marker;
    return raw;
  }

  /** Line starts valid through `line`. */
  void validateStarts(int line) const {
    if (line < startsValid_) return;
    for (int k = startsValid_; k <= line; ++k)
      starts_[(size_t) k] = starts_[(size_t) k - 1] + (int) lines_[(size_t) k - 1].text.size() + 1;
    startsValid_ = line + 1;
  }

  /** Where `raw` ends when inserted at (line, index). */
  static void endOf(int line, int index, const std::u32string& raw, int* endLine, int* endIndex) {
    int breaks = 0;
    size_t after = 0;
    for (size_t i = 0; i < raw.size(); ++i)
      if (detail::code::isBreak(raw[i])) {
        ++breaks;
        after = i + 1;
      }
    *endLine = line + breaks;
    *endIndex = breaks == 0 ? index + (int) raw.size() : (int) (raw.size() - after);
  }

  bool insertRawRecorded(int line, int index, const std::u32string& raw) {
    if (raw.empty()) return true;
    if ((long long) numChars_ + (long long) raw.size() > (long long) maxSize_) return false;
    int endLine = 0, endIndex = 0;
    insertRaw(line, index, raw, &endLine, &endIndex);
    Edit e;
    e.kind = Edit::Insert;
    e.line = line;
    e.index = index;
    e.endLine = endLine;
    e.endIndex = endIndex;
    e.text = raw;
    recordEdit(std::move(e));
    return true;
  }

  void record(Edit::Kind kind, int line, int index, int endLine, int endIndex, std::u32string&& text) {
    Edit e;
    e.kind = kind;
    e.line = line;
    e.index = index;
    e.endLine = endLine;
    e.endIndex = endIndex;
    e.text = std::move(text);
    recordEdit(std::move(e));
  }

  /** Into the open transaction, merged with the edit before it where the
   *  two are one run of typing or of deleting. */
  void recordEdit(Edit&& e) {
    if (replaying_) return;
    redo_.clear();
    if (!open_ || undo_.empty()) {
      undo_.emplace_back();
      undo_.back().id = ++lastId_;
      open_ = true;
    }
    Transaction& t = undo_.back();
    const size_t cost = e.text.size() + 8u;
    t.cost += cost;
    undoCost_ += cost;
    if (!t.edits.empty()) {
      Edit& prev = t.edits.back();
      if (e.kind == Edit::Insert && prev.kind == Edit::Insert && prev.endLine == e.line && prev.endIndex == e.index) {
        prev.text += e.text;
        prev.endLine = e.endLine;
        prev.endIndex = e.endIndex;
        trimHistory();
        return;
      }
      if (e.kind == Edit::Remove && prev.kind == Edit::Remove) {
        if (e.endLine == prev.line && e.endIndex == prev.index) { // backspacing
          prev.text.insert(0, e.text);
          prev.line = e.line;
          prev.index = e.index;
          trimHistory();
          return;
        }
        if (e.line == prev.line && e.index == prev.index) { // deleting forwards
          prev.text += e.text;
          trimHistory();
          return;
        }
      }
    }
    t.edits.push_back(std::move(e));
    trimHistory();
  }

  void trimHistory() {
    while (undo_.size() > 1 && (undo_.size() > maxTransactions_ || undoCost_ > undoBudget_)) {
      undoCost_ -= undo_.front().cost;
      baseId_ = undo_.front().id; // what undoing everything now comes back to
      undo_.pop_front();
    }
  }

  uint64_t currentStateId() const { return undo_.empty() ? baseId_ : undo_.back().id; }

  /** A match of `parts` starting on line k at an index in [lo, hi]: the
   *  first such start searching forwards, the last searching back; or -1. */
  int matchOnLine(const std::vector<std::u32string>& parts, bool caseSensitive, int k, int lo, int hi,
                  bool forwards) const {
    if (lo < 0) lo = 0;
    if (lo > hi) return -1;
    const std::u32string& t = lines_[(size_t) k].text;
    auto same = [&](char32_t a, char32_t b) { return (caseSensitive ? a : detail::code::fold(a)) == b; };
    auto equalAt = [&](const std::u32string& line, size_t at, const std::u32string& part) {
      if (at + part.size() > line.size()) return false;
      for (size_t i = 0; i < part.size(); ++i)
        if (!same(line[at + i], part[i])) return false;
      return true;
    };
    if (parts.size() == 1) {
      const std::u32string& p = parts[0];
      if (p.size() > t.size()) return -1;
      const long long last = (long long) (t.size() - p.size());
      const long long from = lo, to = (std::min)((long long) hi, last);
      if (from > to) return -1;
      if (forwards) {
        for (long long i = from; i <= to; ++i)
          if (equalAt(t, (size_t) i, p)) return (int) i;
      } else {
        for (long long i = to; i >= from; --i)
          if (equalAt(t, (size_t) i, p)) return (int) i;
      }
      return -1;
    }
    // Several lines: the first part ends line k, the middle ones are whole
    // lines, the last begins the line after them.
    if (k + (int) parts.size() - 1 >= getNumLines()) return -1;
    if (parts[0].size() > t.size()) return -1;
    const int at = (int) (t.size() - parts[0].size());
    if (at < lo || at > hi) return -1;
    if (!equalAt(t, (size_t) at, parts[0])) return -1;
    for (size_t j = 1; j + 1 < parts.size(); ++j) {
      const std::u32string& whole = lines_[(size_t) k + j].text;
      if (whole.size() != parts[j].size() || !equalAt(whole, 0, parts[j])) return -1;
    }
    if (!equalAt(lines_[(size_t) k + parts.size() - 1].text, 0, parts.back())) return -1;
    return at;
  }

  // ── The edits themselves ─────────────────────────────────────────────────
  //
  // Three primitives, used alike by an edit and by undo and redo: so a
  // maintained position, a line start and a listener cannot be told one
  // thing by typing and another by undoing it.

  void insertRaw(int line, int index, const std::u32string& raw, int* endLine, int* endIndex) {
    size_t breaks = 0;
    for (char32_t c : raw)
      if (detail::code::isBreak(c)) ++breaks;
    Line& first = lines_[(size_t) line];
    if (breaks == 0) {
      first.text.insert((size_t) index, raw);
      *endLine = line;
      *endIndex = index + (int) raw.size();
    } else {
      std::u32string tail = first.text.substr((size_t) index);
      const LineEnding tailEnding = first.ending;
      first.text.resize((size_t) index);
      std::vector<Line> added;
      added.reserve(breaks);
      size_t segment = 0;
      bool isFirst = true;
      for (size_t i = 0; i <= raw.size(); ++i) {
        if (i < raw.size() && !detail::code::isBreak(raw[i])) continue;
        if (isFirst) {
          first.text.append(raw, segment, i - segment);
          first.ending = endingOf(raw[i]);
          isFirst = false;
        } else {
          Line l;
          l.text.assign(raw, segment, i - segment);
          if (i < raw.size()) l.ending = endingOf(raw[i]);
          added.push_back(std::move(l));
        }
        segment = i + 1;
      }
      Line& last = added.back();
      *endIndex = (int) last.text.size();
      last.text += tail;
      last.ending = tailEnding;
      lines_.insert(lines_.begin() + line + 1, std::make_move_iterator(added.begin()),
                    std::make_move_iterator(added.end()));
      starts_.insert(starts_.begin() + line + 1, breaks, 0);
      *endLine = line + (int) breaks;
    }
    numChars_ += (int) raw.size();
    if (startsValid_ > line + 1) startsValid_ = line + 1;
    maxLineLength_ = -1;
    ++version_;
    const int b = (int) breaks;
    for (Position* p : positions_) {
      if (p->line_ > line) {
        p->line_ += b;
      } else if (p->line_ == line && p->index_ >= index) {
        if (b == 0) {
          p->index_ += (int) raw.size();
        } else {
          p->index_ = p->index_ - index + *endIndex;
          p->line_ += b;
        }
      }
    }
    notify(line, index, (int) raw.size(), b, true);
  }

  std::u32string removeRaw(int l1, int i1, int l2, int i2) {
    std::u32string removed;
    if (l1 == l2) {
      std::u32string& t = lines_[(size_t) l1].text;
      removed.assign(t, (size_t) i1, (size_t) (i2 - i1));
      t.erase((size_t) i1, (size_t) (i2 - i1));
    } else {
      Line& a = lines_[(size_t) l1];
      removed.assign(a.text, (size_t) i1, std::u32string::npos);
      removed.push_back(markerOf(a.ending));
      for (int k = l1 + 1; k < l2; ++k) {
        removed += lines_[(size_t) k].text;
        removed.push_back(markerOf(lines_[(size_t) k].ending));
      }
      const Line& b = lines_[(size_t) l2];
      removed.append(b.text, 0, (size_t) i2);
      a.text.resize((size_t) i1);
      a.text.append(b.text, (size_t) i2, std::u32string::npos);
      a.ending = b.ending;
      lines_.erase(lines_.begin() + l1 + 1, lines_.begin() + l2 + 1);
      starts_.erase(starts_.begin() + l1 + 1, starts_.begin() + l2 + 1);
    }
    numChars_ -= (int) removed.size();
    if (startsValid_ > l1 + 1) startsValid_ = l1 + 1;
    maxLineLength_ = -1;
    ++version_;
    for (Position* p : positions_) {
      const bool afterStart = p->line_ > l1 || (p->line_ == l1 && p->index_ > i1);
      if (!afterStart) continue;
      const bool afterEnd = p->line_ > l2 || (p->line_ == l2 && p->index_ > i2);
      if (!afterEnd) {
        p->line_ = l1;
        p->index_ = i1;
      } else if (p->line_ == l2) {
        p->line_ = l1;
        p->index_ = i1 + p->index_ - i2;
      } else {
        p->line_ -= l2 - l1;
      }
    }
    notify(l1, i1, (int) removed.size(), -(l2 - l1), false);
    return removed;
  }

  void setEndingRaw(int line, LineEnding e) {
    lines_[(size_t) line].ending = e;
    ++version_;
    forEachListener([&](Listener& l) { l.codeDocumentLinesChanged(line, 0); });
  }

  void notify(int line, int index, int count, int lineDelta, bool inserted) {
    if (listeners_.empty()) return;
    const int start = offsetOf(line, index);
    forEachListener([&](Listener& l) {
      l.codeDocumentLinesChanged(line, lineDelta);
      // Asked again: a listener that stopped listening in that call -- or was
      // destroyed in it, as an editor deleted from its onTextChange is -- is
      // not told the rest of the change.
      if (std::find(listeners_.begin(), listeners_.end(), &l) == listeners_.end()) return;
      if (inserted) l.codeDocumentTextInserted(start, start + count);
      else l.codeDocumentTextDeleted(start, start + count);
    });
  }

  /** Over a copy, asking before each whether it is still listening: one
   *  removed by another's callback is not called. */
  template <typename Fn>
  void forEachListener(Fn fn) {
    if (listeners_.empty()) return;
    const std::vector<Listener*> copy = listeners_;
    for (Listener* l : copy)
      if (std::find(listeners_.begin(), listeners_.end(), l) != listeners_.end()) fn(*l);
  }

  std::vector<Line> lines_;
  mutable std::vector<int> starts_; // each line's offset, valid below startsValid_
  mutable int startsValid_ = 1;
  mutable int maxLineLength_ = 0;
  int numChars_ = 0;
  int maxSize_ = kDefaultMaximumSize;
  uint64_t version_ = 0;
  LineEnding newLine_ = LineEnding::LF;
  Encoding encoding_ = Encoding::Utf8;
  std::vector<Position*> positions_;
  std::vector<Listener*> listeners_;

  std::deque<Transaction> undo_;
  std::vector<Transaction> redo_;
  size_t undoCost_ = 0;
  size_t maxTransactions_ = 10000;
  size_t undoBudget_ = 16u * 1024u * 1024u;
  bool open_ = false;      // edits go into undo_.back()
  bool replaying_ = false; // inside undo or redo: nothing is recorded
  uint64_t lastId_ = 0, baseId_ = 0, savedId_ = 0;
};

// ═════════════════════════════════════════════════════════════════════════════
// Tokenisers
// ═════════════════════════════════════════════════════════════════════════════

/**
 * Splits a line of code into coloured tokens.
 *
 * A line at a time, with a state carried from the end of one line to the
 * start of the next -- 0 at the top of a document -- so a comment or a
 * string that spans lines is known on every line it spans, and a line can be
 * coloured without the lines above it being coloured again: only their
 * states are needed.
 */
class CodeTokeniser {
public:
  /** A token: `length` characters from `start`, of a tokeniser's type. The
   *  gaps between tokens are spaces. */
  struct Token {
    /** The index of its first character in the line, 0-based, in code
     *  points. */
    int start = 0;
    /** How many code points it covers. */
    int length = 0;
    /** The tokeniser's type number: an index into the colour scheme's
     *  `types`. */
    int type = 0;
  };

  /** A colour for each token type, in the order of the type numbers, by
   *  name -- the reference's arrangement, so a scheme can be edited by name. */
  struct ColourScheme {
    /** One token type: its name, and the colour it is drawn in. Its type
     *  number is its index in `types`. */
    struct TokenType {
      /** What set() and indexOf() find it by: "Keyword", "String" and the
       *  rest of the tokeniser's names. */
      std::string name;
      /** What its tokens are drawn in. */
      Colour colour;
    };
    /** The types, indexed by type number. */
    std::vector<TokenType> types;

    /** Changes the colour of a type by name, or adds one. */
    void set(const std::string& name, Colour colour) {
      for (TokenType& t : types)
        if (t.name == name) {
          t.colour = colour;
          return;
        }
      types.push_back({name, colour});
    }
    /** The type a name is, or -1. */
    int indexOf(const std::string& name) const {
      for (size_t i = 0; i < types.size(); ++i)
        if (types[i].name == name) return (int) i;
      return -1;
    }
  };

  /** An editor does not own its tokeniser: it must outlive every editor
   *  given it, or be taken away with setTokeniser first. */
  virtual ~CodeTokeniser() = default;

  /**
   * Tokenise `text[0..length)`, a line without its break, starting in
   * `state`; return the state it ends in. Tokens are APPENDED to `tokens`,
   * in order and not overlapping; with null, only the state is wanted --
   * which is how the lines above a screen are scanned, and must cost no more
   * than reading them.
   */
  virtual uint32_t tokeniseLine(const char32_t* text, int length, uint32_t state,
                                std::vector<Token>* tokens) const = 0;

  /** The colours this tokeniser's types are drawn in unless an editor is
   *  told otherwise: the reference's, made for a white background. */
  virtual ColourScheme getDefaultColourScheme() const = 0;

protected:
  /** For a tokeniseLine: appends `[start, end)` as a token of `type`, unless
   *  `out` is null (only the state is wanted) or the range is empty. */
  static void emit(std::vector<Token>* out, int start, int end, int type) {
    if (out && end > start) out->push_back({start, end - start, type});
  }
  /** Past this character and the zero-width ones that belong to it, so an
   *  error token never splits a letter from its accent. */
  static int clusterEnd(const char32_t* s, int n, int i) {
    ++i;
    while (i < n && detail::code::isZeroWidth(s[i])) ++i;
    return i;
  }
};

/**
 * C and C++: the reference's token types and colours. Keywords to C++20;
 * integer and floating literals in decimal, hex, octal and binary, with digit
 * separators, hex floats and suffixes; string and character literals with
 * their prefixes, raw strings with delimiters, and a backslash at a line's
 * end continuing a string, a line comment or a directive onto the next line,
 * as the language's line splicing does.
 */
class CPlusPlusCodeTokeniser : public CodeTokeniser {
public:
  /** The type numbers, in the order of the default colour scheme. */
  enum TokenType {
    tokenType_error = 0,   ///< What the language has no use for: a stray character, a malformed number, a backslash not ending the line.
    tokenType_comment,     ///< A line or block comment, and the lines a spliced line comment continues onto.
    tokenType_keyword,     ///< A reserved word, as isReservedKeyword.
    tokenType_operator,    ///< An operator, the longest that matches: `<=>`, `->*`, `<<=` and the rest.
    tokenType_identifier,  ///< Any other name; `$` and word characters past ASCII included.
    tokenType_integer,     ///< An integer literal: decimal, hex, octal or binary, with separators and suffix.
    tokenType_float,       ///< A floating literal, hex floats included, with its suffix.
    tokenType_string,      ///< A string or character literal with its prefix, raw strings included.
    tokenType_bracket,     ///< One of the six brackets: round, square and curly.
    tokenType_punctuation, ///< A comma, a semicolon, `:` or `::`, `.` or `...`, and a backslash ending the line.
    tokenType_preprocessor ///< A directive, to the line's end or a comment, and the lines it continues onto.
  };

  /** Whether `s[0..n)` is a C++20 keyword, alternative tokens such as `and`
   *  included: case-sensitive, ASCII. */
  static bool isReservedKeyword(const char32_t* s, int n) {
    static const char* const kKeywords[] = {
        "alignas", "alignof", "and", "and_eq", "asm", "auto", "bitand", "bitor", "bool", "break", "case",
        "catch", "char", "char16_t", "char32_t", "char8_t", "class", "co_await", "co_return", "co_yield",
        "compl", "concept", "const", "const_cast", "consteval", "constexpr", "constinit", "continue",
        "decltype", "default", "delete", "do", "double", "dynamic_cast", "else", "enum", "explicit",
        "export", "extern", "false", "final", "float", "for", "friend", "goto", "if", "inline", "int",
        "long", "mutable", "namespace", "new", "noexcept", "not", "not_eq", "nullptr", "operator", "or",
        "or_eq", "override", "private", "protected", "public", "register", "reinterpret_cast", "requires",
        "return", "short", "signed", "sizeof", "static", "static_assert", "static_cast", "struct", "switch",
        "template", "this", "thread_local", "throw", "true", "try", "typedef", "typeid", "typename", "union",
        "unsigned", "using", "virtual", "void", "volatile", "wchar_t", "while", "xor", "xor_eq"};
    return detail::code::isWordIn(s, n, kKeywords, sizeof(kKeywords) / sizeof(kKeywords[0]));
  }

  uint32_t tokeniseLine(const char32_t* s, int n, uint32_t state, std::vector<Token>* out) const override {
    const bool continued = n > 0 && s[n - 1] == '\\';
    int i = 0;
    bool directive = false; // this line is a directive's, or continues one
    switch (state & 15u) {
      case kBlockComment: {
        const int end = commentEnd(s, n, 0);
        if (end < 0) {
          emit(out, 0, n, tokenType_comment);
          return kBlockComment;
        }
        emit(out, 0, end, tokenType_comment);
        i = end;
        break;
      }
      case kLineComment:
        emit(out, 0, n, tokenType_comment);
        return continued ? kLineComment : kNormal;
      case kString: {
        const char32_t quote = (state & kSingleQuote) ? '\'' : '"';
        const int from = (state & kEscapePending) ? (std::min)(1, n) : 0;
        bool closed = false;
        const int end = quoted(s, n, from, quote, &closed);
        emit(out, 0, (std::min)(end, n), tokenType_string);
        if (!closed) return stringState(quote, continued, end == n);
        i = end;
        break;
      }
      case kRawString: {
        const int end = rawEnd(s, n, 0, state >> 4);
        if (end < 0) {
          emit(out, 0, n, tokenType_string);
          return state;
        }
        emit(out, 0, end, tokenType_string);
        i = end;
        break;
      }
      case kDirective: {
        directive = true;
        const int end = directiveEnd(s, n, 0);
        emit(out, 0, end, tokenType_preprocessor);
        if (end >= n) return continued ? kDirective : kNormal;
        i = end;
        break;
      }
      default:
        break;
    }

    while (i < n) {
      const char32_t c = s[i];
      // Spaces and plain identifiers first, from one table load: they are
      // most of any line, and a raw string's or a literal's prefix is the
      // only identifier that can change what follows it.
      const uint8_t kind = detail::code::asciiKind(c);
      if (kind & 1) {
        ++i;
        continue;
      }
      if ((kind & 2) && c != 'R' && c != 'L' && c != 'u' && c != 'U') {
        int end = i + 1;
        while (end < n && ((detail::code::asciiKind(s[end]) & 4) || (s[end] >= 0x80 && detail::code::isIdentifierBody(s[end])) ||
                           s[end] == '$'))
          ++end;
        if (out) emit(out, i, end, isReservedKeyword(s + i, end - i) ? tokenType_keyword : tokenType_identifier);
        i = end;
        continue;
      }
      if (c >= 0x80 && detail::code::isSpace(c)) {
        ++i;
        continue;
      }
      const char32_t next = i + 1 < n ? s[i + 1] : 0;
      if (c == '/' && next == '/') {
        emit(out, i, n, tokenType_comment);
        return continued ? kLineComment : kNormal;
      }
      if (c == '/' && next == '*') {
        const int end = commentEnd(s, n, i + 2);
        if (end < 0) {
          emit(out, i, n, tokenType_comment);
          return kBlockComment;
        }
        emit(out, i, end, tokenType_comment);
        i = end;
        continue;
      }
      if (c == '#') {
        directive = true;
        const int end = directiveEnd(s, n, i);
        emit(out, i, end, tokenType_preprocessor);
        if (end >= n) return continued ? kDirective : kNormal;
        i = end;
        continue;
      }
      if (c == '"' || c == '\'') {
        bool closed = false;
        const int end = quoted(s, n, i + 1, c, &closed);
        emit(out, i, (std::min)(end, n), tokenType_string);
        if (!closed) return stringState(c, continued, end == n);
        i = end;
        continue;
      }
      if (detail::code::isDigit(c) || (c == '.' && detail::code::isDigit(next))) {
        int type = tokenType_integer;
        const int end = number(s, n, i, &type);
        emit(out, i, end, type);
        i = end;
        continue;
      }
      if (detail::code::isIdentifierStart(c) || c == '$') {
        int end = i + 1;
        while (end < n && (detail::code::isIdentifierBody(s[end]) || s[end] == '$')) ++end;
        // A literal's prefix: L, u, U, u8 and their raw forms with R.
        if (end < n && (s[end] == '"' || s[end] == '\'') && isLiteralPrefix(s + i, end - i)) {
          const bool raw = s[end - 1] == 'R';
          if (raw && s[end] == '"') {
            int open = end + 1;
            while (open < n && open - (end + 1) <= 16 && isDelimiterChar(s[open])) ++open;
            if (open < n && s[open] == '(' && open - (end + 1) <= 16) {
              const uint32_t hash = detail::code::delimiterHash(s + end + 1, open - (end + 1));
              const int close = rawEnd(s, n, open + 1, hash);
              if (close < 0) {
                emit(out, i, n, tokenType_string);
                return kRawString | (hash << 4);
              }
              emit(out, i, close, tokenType_string);
              i = close;
              continue;
            }
            // Not a raw string after all -- a delimiter too long or broken:
            // the quote starts an ordinary one.
          }
          if (!raw || s[end] == '"') {
            bool closed = false;
            const int stop = quoted(s, n, end + 1, s[end], &closed);
            emit(out, i, (std::min)(stop, n), tokenType_string);
            if (!closed) return stringState(s[end], continued, stop == n);
            i = stop;
            continue;
          }
        }
        if (out) emit(out, i, end, isReservedKeyword(s + i, end - i) ? tokenType_keyword : tokenType_identifier);
        i = end;
        continue;
      }
      switch (c) {
        case '(': case ')': case '[': case ']': case '{': case '}':
          emit(out, i, i + 1, tokenType_bracket);
          ++i;
          continue;
        case ',': case ';':
          emit(out, i, i + 1, tokenType_punctuation);
          ++i;
          continue;
        case ':': {
          const int len = next == ':' ? 2 : 1;
          emit(out, i, i + len, tokenType_punctuation);
          i += len;
          continue;
        }
        case '.': {
          if (next == '.' && i + 2 < n && s[i + 2] == '.') {
            emit(out, i, i + 3, tokenType_punctuation);
            i += 3;
          } else if (next == '*') {
            emit(out, i, i + 2, tokenType_operator);
            i += 2;
          } else {
            emit(out, i, i + 1, tokenType_punctuation);
            ++i;
          }
          continue;
        }
        case '\\':
          // A line splice, at the end of a line; anywhere else, nothing the
          // language has.
          emit(out, i, i + 1, i == n - 1 ? tokenType_punctuation : tokenType_error);
          ++i;
          continue;
        default:
          break;
      }
      const int op = operatorLength(s, n, i);
      if (op > 0) {
        emit(out, i, i + op, tokenType_operator);
        i += op;
        continue;
      }
      const int end = clusterEnd(s, n, i);
      emit(out, i, end, tokenType_error);
      i = end;
    }
    return (directive && continued) ? kDirective : kNormal;
  }

  ColourScheme getDefaultColourScheme() const override {
    static const struct {
      const char* name;
      uint32_t argb;
    } kTypes[] = {{"Error", 0xffcc0000},    {"Comment", 0xff00aa00}, {"Keyword", 0xff0000cc},
                  {"Operator", 0xff225500}, {"Identifier", 0xff000000}, {"Integer", 0xff880000},
                  {"Float", 0xff885500},    {"String", 0xff990099},  {"Bracket", 0xff000055},
                  {"Punctuation", 0xff004400}, {"Preprocessor Text", 0xff660000}};
    ColourScheme cs;
    for (const auto& t : kTypes) cs.set(t.name, Colour::fromARGB(t.argb));
    return cs;
  }

private:
  // The kind of state in the low four bits; a raw string's delimiter hash,
  // or a string's quote and pending escape, above them.
  static constexpr uint32_t kNormal = 0, kBlockComment = 1, kLineComment = 2, kString = 3, kRawString = 4,
                            kDirective = 5;
  static constexpr uint32_t kSingleQuote = 16, kEscapePending = 32;

  /** A string the line ended inside: carried on if a backslash ends the
   *  line, as a splice -- with the escape the backslashes before the splice
   *  leave pending when they are odd -- and otherwise unterminated, which
   *  ends at the line as the language says it does. */
  static uint32_t stringState(char32_t quote, bool continued, bool escapePending) {
    if (!continued) return kNormal;
    return kString | (quote == '\'' ? kSingleQuote : 0u) | (escapePending ? kEscapePending : 0u);
  }

  /** From `from`, inside a literal: the index after its closing quote --
   *  `closed` true -- or n or n + 1 when the line ends first: n + 1 when the
   *  last backslash stood alone (a splice), n when it was escaped by the one
   *  before it (and so leaves that one pending). */
  static int quoted(const char32_t* s, int n, int from, char32_t quote, bool* closed) {
    int j = from;
    while (j < n) {
      if (s[j] == '\\') {
        j += 2;
        continue;
      }
      if (s[j] == quote) {
        *closed = true;
        return j + 1;
      }
      ++j;
    }
    *closed = false;
    return j;
  }

  static int commentEnd(const char32_t* s, int n, int from) {
    for (int j = from; j + 1 < n; ++j)
      if (s[j] == '*' && s[j + 1] == '/') return j + 2;
    return -1;
  }

  /** A directive runs to the line's end or to a comment, a string inside it
   *  skipped so that a "//" in an include path is not one. */
  static int directiveEnd(const char32_t* s, int n, int from) {
    int j = from;
    while (j < n) {
      if (s[j] == '"') {
        bool closed = false;
        j = (std::min)(quoted(s, n, j + 1, '"', &closed), n);
        continue;
      }
      if (s[j] == '/' && j + 1 < n && (s[j + 1] == '/' || s[j + 1] == '*')) return j;
      ++j;
    }
    return n;
  }

  static bool isDelimiterChar(char32_t c) {
    return c > 0x20 && c < 0x7F && c != '(' && c != ')' && c != '\\' && c != '"';
  }

  /** The index after `)delimiter"`, or -1. */
  static int rawEnd(const char32_t* s, int n, int from, uint32_t hash) {
    for (int p = from; p < n; ++p) {
      if (s[p] != ')') continue;
      for (int q = p + 1; q < n && q - p - 1 <= 16; ++q) {
        if (s[q] == '"') {
          if (detail::code::delimiterHash(s + p + 1, q - p - 1) == hash) return q + 1;
          break;
        }
        if (!isDelimiterChar(s[q])) break;
      }
    }
    return -1;
  }

  static bool isLiteralPrefix(const char32_t* s, int n) {
    static const char* const kPrefixes[] = {"L", "LR", "R", "U", "UR", "u", "u8", "u8R", "uR"};
    return detail::code::isWordIn(s, n, kPrefixes, sizeof(kPrefixes) / sizeof(kPrefixes[0]));
  }

  template <typename Pred>
  static int digits(const char32_t* s, int n, int j, Pred pred) {
    while (j < n && (pred(s[j]) || (s[j] == '\'' && j > 0 && pred(s[j - 1]) && j + 1 < n && pred(s[j + 1])))) ++j;
    return j;
  }

  /** A number from i: its end, and whether it was an integer, a float or
   *  malformed -- "0x" with no digits, an exponent with none. Its suffix
   *  goes with it, a user-defined literal's included. */
  static int number(const char32_t* s, int n, int i, int* type) {
    using detail::code::isDigit;
    using detail::code::isHexDigit;
    auto isBin = [](char32_t c) { return c == '0' || c == '1'; };
    int j = i;
    bool isFloat = false, ok = true;
    auto exponent = [&]() {
      ++j;
      if (j < n && (s[j] == '+' || s[j] == '-')) ++j;
      const int at = j;
      j = digits(s, n, j, isDigit);
      if (j == at) ok = false;
      isFloat = true;
    };
    if (s[j] == '0' && j + 1 < n && (s[j + 1] == 'x' || s[j + 1] == 'X')) {
      j += 2;
      const int at = j;
      j = digits(s, n, j, isHexDigit);
      bool any = j > at;
      if (j < n && s[j] == '.') {
        ++j;
        const int frac = j;
        j = digits(s, n, j, isHexDigit);
        any = any || j > frac;
        isFloat = true;
      }
      if (!any) ok = false;
      if (j < n && (s[j] == 'p' || s[j] == 'P')) exponent();
    } else if (s[j] == '0' && j + 1 < n && (s[j + 1] == 'b' || s[j + 1] == 'B')) {
      j += 2;
      const int at = j;
      j = digits(s, n, j, isBin);
      if (j == at) ok = false;
    } else {
      j = digits(s, n, j, isDigit);
      if (j < n && s[j] == '.') {
        ++j;
        j = digits(s, n, j, isDigit);
        isFloat = true;
      }
      if (j < n && (s[j] == 'e' || s[j] == 'E')) exponent();
    }
    while (j < n && detail::code::isIdentifierBody(s[j])) ++j;
    *type = !ok ? tokenType_error : (isFloat ? tokenType_float : tokenType_integer);
    return j;
  }

  /** The longest operator at i, or 0: C++'s, maximal munch -- <=> <<= >>=
   *  ->* -> ++ -- << >> <= >= == != && || and the compound assignments.
   *  A switch on the first character rather than a table of strings: this
   *  runs for every operator of every line above the screen. */
  static int operatorLength(const char32_t* s, int n, int i) {
    const char32_t a = i + 1 < n ? s[i + 1] : 0, b = i + 2 < n ? s[i + 2] : 0;
    switch (s[i]) {
      case '<':
        if (a == '=' && b == '>') return 3;
        if (a == '<') return b == '=' ? 3 : 2;
        return a == '=' ? 2 : 1;
      case '>':
        if (a == '>') return b == '=' ? 3 : 2;
        return a == '=' ? 2 : 1;
      case '-':
        if (a == '>') return b == '*' ? 3 : 2;
        return (a == '-' || a == '=') ? 2 : 1;
      case '+': return (a == '+' || a == '=') ? 2 : 1;
      case '&': return (a == '&' || a == '=') ? 2 : 1;
      case '|': return (a == '|' || a == '=') ? 2 : 1;
      case '*': case '/': case '%': case '^': case '=': case '!': return a == '=' ? 2 : 1;
      case '~': case '?': return 1;
      default: return 0;
    }
  }
};

/**
 * Lua 5.4: the reference's token types and colours. Long brackets of any
 * level for strings and comments, which span lines; short strings with their
 * escapes, a backslash before the line's end and `\z` carrying them onto the
 * next; decimal and hex numbers, hex floats among them.
 */
class LuaTokeniser : public CodeTokeniser {
public:
  /** The type numbers, in the order of the default colour scheme. */
  enum TokenType {
    tokenType_error = 0,  ///< What the language has no use for: a stray character, or a malformed number (a letter straight after one among them).
    tokenType_comment,    ///< `--` to the line's end, or a long bracket comment of any level, which spans lines.
    tokenType_keyword,    ///< One of Lua's 22 reserved words, as isReservedKeyword.
    tokenType_operator,   ///< An operator, the longest that matches, `..` and `...` included.
    tokenType_identifier, ///< Any other name.
    tokenType_integer,    ///< A decimal or hex integer.
    tokenType_float,      ///< A number with a fraction or an exponent, hex floats included.
    tokenType_string,     ///< A short string with its escapes, or a long bracket string, which spans lines.
    tokenType_bracket,    ///< One of the six brackets: round, square and curly.
    tokenType_punctuation ///< A comma, a semicolon, `:` or `::`, or a single `.`.
  };

  /** Whether `s[0..n)` is one of Lua 5.4's reserved words: case-sensitive,
   *  ASCII. */
  static bool isReservedKeyword(const char32_t* s, int n) {
    static const char* const kKeywords[] = {"and",   "break", "do",     "else",   "elseif", "end",
                                            "false", "for",   "function", "goto", "if",     "in",
                                            "local", "nil",   "not",    "or",     "repeat", "return",
                                            "then",  "true",  "until",  "while"};
    return detail::code::isWordIn(s, n, kKeywords, sizeof(kKeywords) / sizeof(kKeywords[0]));
  }

  uint32_t tokeniseLine(const char32_t* s, int n, uint32_t state, std::vector<Token>* out) const override {
    int i = 0;
    switch (state & 15u) {
      case kLongComment:
      case kLongString: {
        const int type = (state & 15u) == kLongComment ? tokenType_comment : tokenType_string;
        const int end = longClose(s, n, 0, state >> 4);
        if (end < 0) {
          emit(out, 0, n, type);
          return state;
        }
        emit(out, 0, end, type);
        i = end;
        break;
      }
      case kString: {
        const char32_t quote = (state & kSingleQuote) ? '\'' : '"';
        int from = 0;
        if (state & kSkipping)
          while (from < n && detail::code::isSpace(s[from])) ++from;
        if ((state & kSkipping) && from == n) {
          emit(out, 0, n, tokenType_string);
          return state; // \z goes on skipping across blank lines
        }
        uint32_t carried = kNormal;
        const int end = shortString(s, n, from, quote, &carried);
        emit(out, 0, end, tokenType_string);
        if (carried != kNormal) return carried;
        i = end; // closed -- or unfinished, and ended with the line
        break;
      }
      default:
        break;
    }

    while (i < n) {
      const char32_t c = s[i];
      if (detail::code::isSpace(c)) {
        ++i;
        continue;
      }
      const char32_t next = i + 1 < n ? s[i + 1] : 0;
      if (c == '-' && next == '-') {
        const int level = longOpen(s, n, i + 2);
        if (level >= 0) {
          const int end = longClose(s, n, i + 4 + level, (uint32_t) level);
          if (end < 0) {
            emit(out, i, n, tokenType_comment);
            return kLongComment | ((uint32_t) level << 4);
          }
          emit(out, i, end, tokenType_comment);
          i = end;
          continue;
        }
        emit(out, i, n, tokenType_comment);
        return kNormal;
      }
      if (c == '[') {
        const int level = longOpen(s, n, i);
        if (level >= 0) {
          const int end = longClose(s, n, i + 2 + level, (uint32_t) level);
          if (end < 0) {
            emit(out, i, n, tokenType_string);
            return kLongString | ((uint32_t) level << 4);
          }
          emit(out, i, end, tokenType_string);
          i = end;
          continue;
        }
      }
      if (c == '"' || c == '\'') {
        uint32_t carried = kNormal;
        const int end = shortString(s, n, i + 1, c, &carried);
        emit(out, i, end, tokenType_string);
        if (carried != kNormal) return carried;
        i = end;
        continue;
      }
      if (detail::code::isDigit(c) || (c == '.' && detail::code::isDigit(next))) {
        int type = tokenType_integer;
        const int end = number(s, n, i, &type);
        emit(out, i, end, type);
        i = end;
        continue;
      }
      if (detail::code::isIdentifierStart(c)) {
        int end = i + 1;
        while (end < n && detail::code::isIdentifierBody(s[end])) ++end;
        if (out) emit(out, i, end, isReservedKeyword(s + i, end - i) ? tokenType_keyword : tokenType_identifier);
        i = end;
        continue;
      }
      switch (c) {
        case '(': case ')': case '[': case ']': case '{': case '}':
          emit(out, i, i + 1, tokenType_bracket);
          ++i;
          continue;
        case ',': case ';':
          emit(out, i, i + 1, tokenType_punctuation);
          ++i;
          continue;
        case ':': {
          const int len = next == ':' ? 2 : 1;
          emit(out, i, i + len, tokenType_punctuation);
          i += len;
          continue;
        }
        case '.': {
          int len = 1;
          while (len < 3 && i + len < n && s[i + len] == '.') ++len;
          emit(out, i, i + len, len == 1 ? tokenType_punctuation : tokenType_operator);
          i += len;
          continue;
        }
        default:
          break;
      }
      const int op = operatorLength(s, n, i);
      if (op > 0) {
        emit(out, i, i + op, tokenType_operator);
        i += op;
        continue;
      }
      const int end = clusterEnd(s, n, i);
      emit(out, i, end, tokenType_error);
      i = end;
    }
    return kNormal;
  }

  ColourScheme getDefaultColourScheme() const override {
    static const struct {
      const char* name;
      uint32_t argb;
    } kTypes[] = {{"Error", 0xffcc0000},      {"Comment", 0xff3c3c3c},    {"Keyword", 0xff0000cc},
                  {"Operator", 0xff225500},   {"Identifier", 0xff000000}, {"Integer", 0xff880000},
                  {"Float", 0xff885500},      {"String", 0xff990099},     {"Bracket", 0xff000055},
                  {"Punctuation", 0xff004400}};
    ColourScheme cs;
    for (const auto& t : kTypes) cs.set(t.name, Colour::fromARGB(t.argb));
    return cs;
  }

private:
  static constexpr uint32_t kNormal = 0, kLongComment = 1, kLongString = 2, kString = 3;
  static constexpr uint32_t kSingleQuote = 16, kSkipping = 32;

  /** The level of a long bracket opening at i -- "[[" is 0, "[==[" is 2 --
   *  or -1 where there is none. */
  static int longOpen(const char32_t* s, int n, int i) {
    if (i >= n || s[i] != '[') return -1;
    int j = i + 1;
    while (j < n && s[j] == '=') ++j;
    return (j < n && s[j] == '[') ? j - i - 1 : -1;
  }

  /** The index after the long bracket of `level` that closes from `from`, or
   *  -1. */
  static int longClose(const char32_t* s, int n, int from, uint32_t level) {
    for (int p = (std::max)(from, 0); p < n; ++p) {
      if (s[p] != ']') continue;
      int j = p + 1;
      while (j < n && s[j] == '=') ++j;
      if (j < n && s[j] == ']' && (uint32_t) (j - p - 1) == level) return j + 1;
    }
    return -1;
  }

  /** A short string from `from`: the index after its closing quote, or the
   *  line's end -- with `carried` the state for the next line when a
   *  backslash ends the line (an escaped newline) or `\z` is still skipping. */
  static int shortString(const char32_t* s, int n, int from, char32_t quote, uint32_t* carried) {
    const uint32_t quoteBit = quote == '\'' ? kSingleQuote : 0u;
    int j = from;
    while (j < n) {
      const char32_t c = s[j];
      if (c == quote) return j + 1;
      if (c == '\\') {
        if (j + 1 >= n) {
          *carried = kString | quoteBit;
          return n;
        }
        if (s[j + 1] == 'z') {
          j += 2;
          while (j < n && detail::code::isSpace(s[j])) ++j;
          if (j >= n) {
            *carried = kString | quoteBit | kSkipping;
            return n;
          }
          continue;
        }
        j += 2;
        continue;
      }
      ++j;
    }
    return n; // unfinished: it ends with the line
  }

  static int number(const char32_t* s, int n, int i, int* type) {
    using detail::code::isDigit;
    using detail::code::isHexDigit;
    int j = i;
    bool isFloat = false, ok = true;
    const bool hex = s[j] == '0' && j + 1 < n && (s[j + 1] == 'x' || s[j + 1] == 'X');
    auto run = [&](bool hexDigits) {
      const int at = j;
      while (j < n && (hexDigits ? isHexDigit(s[j]) : isDigit(s[j]))) ++j;
      return j > at;
    };
    if (hex) j += 2;
    bool any = run(hex);
    if (j < n && s[j] == '.' && !(j + 1 < n && s[j + 1] == '.')) {
      ++j;
      any = run(hex) || any;
      isFloat = true;
    }
    if (!any) ok = false;
    const char32_t e1 = hex ? 'p' : 'e', e2 = hex ? 'P' : 'E';
    if (j < n && (s[j] == e1 || s[j] == e2)) {
      ++j;
      if (j < n && (s[j] == '+' || s[j] == '-')) ++j;
      if (!run(false)) ok = false;
      isFloat = true;
    }
    // A letter straight after a number is malformed in Lua: it goes with
    // the number, as an error.
    const int body = j;
    while (j < n && detail::code::isIdentifierBody(s[j])) ++j;
    if (j > body) ok = false;
    *type = !ok ? tokenType_error : (isFloat ? tokenType_float : tokenType_integer);
    return j;
  }

  /** Lua's operators, the longest at i: // << >> == ~= <= >= and the single
   *  ones; 0 for none. */
  static int operatorLength(const char32_t* s, int n, int i) {
    const char32_t a = i + 1 < n ? s[i + 1] : 0;
    switch (s[i]) {
      case '/': return a == '/' ? 2 : 1;
      case '<': return (a == '<' || a == '=') ? 2 : 1;
      case '>': return (a == '>' || a == '=') ? 2 : 1;
      case '=': case '~': return a == '=' ? 2 : 1;
      case '+': case '-': case '*': case '%': case '^': case '#': case '&': case '|': return 1;
      default: return 0;
    }
  }
};

/**
 * XML: the reference's token types and colours. A tag's opening -- "<name"
 * or "</name" -- and its closing ">" or "/>" are keywords, attribute
 * names and text are identifiers, "=" an operator, attribute values strings;
 * comments, CDATA sections, processing instructions and declarations span
 * lines, and so do a tag's attributes and their values. Unlike the
 * reference, text between tags is text: a quote in it starts nothing.
 */
class XmlTokeniser : public CodeTokeniser {
public:
  /** The type numbers, in the order of the default colour scheme. */
  enum TokenType {
    tokenType_error = 0,   ///< What is not XML: a bare `<` in text, a stray character inside a tag.
    tokenType_comment,     ///< A comment, which spans lines.
    tokenType_keyword,     ///< A tag's opening, with its name, and its closing `>` or `/>`.
    tokenType_operator,    ///< The `=` between an attribute's name and its value.
    tokenType_identifier,  ///< An attribute's name, or a word of the text between tags.
    tokenType_string,      ///< An attribute's value with its quotes, or a CDATA section; both span lines.
    tokenType_bracket,     ///< Not produced here: kept so the numbers and the scheme are the reference's.
    tokenType_punctuation, ///< Not produced here either, for the same reason.
    tokenType_preprocessor ///< A processing instruction or a declaration such as the DOCTYPE; both span lines.
  };

  uint32_t tokeniseLine(const char32_t* s, int n, uint32_t state, std::vector<Token>* out) const override {
    int i = 0;
    uint32_t kind = state & 15u;
    switch (kind) {
      case kComment:
      case kCData:
      case kInstruction: {
        const char* close = kind == kComment ? "-->" : (kind == kCData ? "]]>" : "?>");
        const int type = kind == kComment ? tokenType_comment : (kind == kCData ? tokenType_string : tokenType_preprocessor);
        const int end = findText(s, n, 0, close);
        if (end < 0) {
          emit(out, 0, n, type);
          return kind;
        }
        emit(out, 0, end, type);
        i = end;
        kind = kContent;
        break;
      }
      case kDeclaration: {
        uint32_t depth = state >> 4;
        const int end = declarationEnd(s, n, 0, &depth);
        if (end < 0) {
          emit(out, 0, n, tokenType_preprocessor);
          return kDeclaration | (depth << 4);
        }
        emit(out, 0, end, tokenType_preprocessor);
        i = end;
        kind = kContent;
        break;
      }
      case kValueDouble:
      case kValueSingle: {
        const char32_t quote = kind == kValueDouble ? '"' : '\'';
        int end = 0;
        while (end < n && s[end] != quote) ++end;
        if (end >= n) {
          emit(out, 0, n, tokenType_string);
          return kind;
        }
        emit(out, 0, end + 1, tokenType_string);
        i = end + 1;
        kind = kTag;
        break;
      }
      case kTag:
        break;
      default:
        kind = kContent;
        break;
    }

    while (i < n) {
      const char32_t c = s[i];
      if (detail::code::isSpace(c)) {
        ++i;
        continue;
      }
      if (kind == kTag) {
        if (c == '>') {
          emit(out, i, i + 1, tokenType_keyword);
          ++i;
          kind = kContent;
        } else if (c == '/' && i + 1 < n && s[i + 1] == '>') {
          emit(out, i, i + 2, tokenType_keyword);
          i += 2;
          kind = kContent;
        } else if (c == '=') {
          emit(out, i, i + 1, tokenType_operator);
          ++i;
        } else if (c == '"' || c == '\'') {
          int end = i + 1;
          while (end < n && s[end] != c) ++end;
          if (end >= n) {
            emit(out, i, n, tokenType_string);
            return c == '"' ? kValueDouble : kValueSingle;
          }
          emit(out, i, end + 1, tokenType_string);
          i = end + 1;
        } else if (isNameStart(c)) {
          int end = i + 1;
          while (end < n && isNameChar(s[end])) ++end;
          emit(out, i, end, tokenType_identifier);
          i = end;
        } else {
          const int end = clusterEnd(s, n, i);
          emit(out, i, end, tokenType_error);
          i = end;
        }
        continue;
      }
      // Content.
      if (c == '<') {
        if (startsWith(s, n, i, "<!--")) {
          const int end = findText(s, n, i + 4, "-->");
          if (end < 0) {
            emit(out, i, n, tokenType_comment);
            return kComment;
          }
          emit(out, i, end, tokenType_comment);
          i = end;
        } else if (startsWith(s, n, i, "<![CDATA[")) {
          const int end = findText(s, n, i + 9, "]]>");
          if (end < 0) {
            emit(out, i, n, tokenType_string);
            return kCData;
          }
          emit(out, i, end, tokenType_string);
          i = end;
        } else if (startsWith(s, n, i, "<?")) {
          const int end = findText(s, n, i + 2, "?>");
          if (end < 0) {
            emit(out, i, n, tokenType_preprocessor);
            return kInstruction;
          }
          emit(out, i, end, tokenType_preprocessor);
          i = end;
        } else if (startsWith(s, n, i, "<!")) {
          uint32_t depth = 0;
          const int end = declarationEnd(s, n, i + 2, &depth);
          if (end < 0) {
            emit(out, i, n, tokenType_preprocessor);
            return kDeclaration | (depth << 4);
          }
          emit(out, i, end, tokenType_preprocessor);
          i = end;
        } else {
          int j = i + 1;
          if (j < n && s[j] == '/') ++j;
          if (j < n && isNameStart(s[j])) {
            while (j < n && isNameChar(s[j])) ++j;
            emit(out, i, j, tokenType_keyword);
            i = j;
            kind = kTag;
          } else {
            emit(out, i, i + 1, tokenType_error); // a bare '<' is not XML
            ++i;
          }
        }
        continue;
      }
      // Text, a word at a time.
      int end = i + 1;
      while (end < n && s[end] != '<' && !detail::code::isSpace(s[end])) ++end;
      emit(out, i, end, tokenType_identifier);
      i = end;
    }
    return kind;
  }

  ColourScheme getDefaultColourScheme() const override {
    static const struct {
      const char* name;
      uint32_t argb;
    } kTypes[] = {{"Error", 0xffcc0000},       {"Comment", 0xff00aa00},  {"Keyword", 0xff0000cc},
                  {"Operator", 0xff225500},    {"Identifier", 0xff000000}, {"String", 0xff990099},
                  {"Bracket", 0xff000055},     {"Punctuation", 0xff004400}, {"Preprocessor Text", 0xff660000}};
    ColourScheme cs;
    for (const auto& t : kTypes) cs.set(t.name, Colour::fromARGB(t.argb));
    return cs;
  }

private:
  static constexpr uint32_t kContent = 0, kTag = 1, kComment = 2, kCData = 3, kInstruction = 4, kValueDouble = 5,
                            kValueSingle = 6, kDeclaration = 7;

  static bool isNameStart(char32_t c) { return detail::code::isIdentifierStart(c) || c == ':'; }
  static bool isNameChar(char32_t c) {
    return detail::code::isIdentifierBody(c) || c == ':' || c == '-' || c == '.';
  }
  static bool startsWith(const char32_t* s, int n, int i, const char* text) {
    for (int k = 0; text[k]; ++k)
      if (i + k >= n || s[i + k] != (char32_t) (unsigned char) text[k]) return false;
    return true;
  }
  /** The index after `text` found from `from`, or -1. */
  static int findText(const char32_t* s, int n, int from, const char* text) {
    const int len = (int) std::strlen(text);
    for (int i = (std::max)(from, 0); i + len <= n; ++i)
      if (startsWith(s, n, i, text)) return i + len;
    return -1;
  }
  /** A declaration -- <!DOCTYPE ...> -- ends at a '>' outside its internal
   *  subset's brackets and outside quotes; `depth` carries the brackets
   *  across lines. */
  static int declarationEnd(const char32_t* s, int n, int from, uint32_t* depth) {
    for (int i = from; i < n; ++i) {
      const char32_t c = s[i];
      if (c == '"' || c == '\'') {
        int j = i + 1;
        while (j < n && s[j] != c) ++j;
        i = j;
      } else if (c == '[') {
        if (*depth < 0xFFFFFFu) ++*depth;
      } else if (c == ']') {
        if (*depth > 0) --*depth;
      } else if (c == '>' && *depth == 0) {
        return i + 1;
      }
    }
    return -1;
  }
};

// ═════════════════════════════════════════════════════════════════════════════
// CodeEditorComponent
// ═════════════════════════════════════════════════════════════════════════════

/**
 * A monospaced editor over a CodeDocument, coloured by a CodeTokeniser.
 *
 * Neither is owned: the document, as in the reference, outlives the editor
 * and may be shown by several; the tokeniser may be null for plain text.
 */
class CodeEditorComponent : public Widget, private CodeDocument::Listener {
public:
  /** The token colours, by type number. */
  using ColourScheme = CodeTokeniser::ColourScheme;

  /** What is not a token's colour. The reference's defaults: a white page,
   *  black text, a translucent blue selection and a grey gutter -- what the
   *  tokenisers' default schemes are made for. */
  struct Colours {
    /** Fills the whole editor, under the gutter as well. */
    Colour background = Colour::fromARGB(0xffffffff);
    /** Text outside every token, all of it with no tokeniser, and a token
     *  whose type the scheme has no colour for. */
    Colour defaultText = Colour::fromARGB(0xff000000);
    /** The selection, drawn under the text. */
    Colour highlight = Colour::fromARGB(0x401111ee);
    /** The line-number gutter, over the background. */
    Colour lineNumberBackground = Colour::fromARGB(0x44999999);
    /** The line numbers. */
    Colour lineNumberText = Colour::fromARGB(0x44000000);
    /** The caret, a bar two pixels wide. */
    Colour caret = Colour::fromARGB(0xff000000);
  };

  /** Pixels between the gutter, or the left edge, and the first column. */
  static constexpr float kTextPadding = 4.0f;
  /** Past this many characters, an accessible value is the caret's line
   *  rather than the whole document. */
  static constexpr int kAccessibleTextLimit = 65536;

  /** Shows and edits `document`, coloured by `tokeniser` (null for plain
   *  text). Neither is owned, and both must outlive the editor. It starts
   *  with the caret at the top, the monospaced typeface at 14 points when
   *  one is found, and the tokeniser's default colour scheme. */
  CodeEditorComponent(CodeDocument& document, CodeTokeniser* tokeniser)
      : document_(document), tokeniser_(tokeniser), caret_(document, 0, 0), anchor_(document, 0, 0) {
    caret_.setPositionMaintained(true);
    anchor_.setPositionMaintained(true);
    setWantsKeyboardFocus(true);
    setCursor(MouseCursor::Text);
    if (std::shared_ptr<Typeface> face = monospacedTypeface()) setFont(Font(face, 14.0f));
    if (tokeniser_) scheme_ = tokeniser_->getDefaultColourScheme();
    states_.assign((size_t) document_.getNumLines(), 0u);
    vertical_.onScroll = [this](float y) {
      if (!syncingBars_) scrollToPixels(scrollX_, y);
    };
    horizontal_.onScroll = [this](float x) {
      if (!syncingBars_) scrollToPixels(x, scrollY_);
    };
    addChild(&vertical_);
    addChild(&horizontal_);
    document_.addListener(this);
  }

  /** Stops listening to the document, which must still be there. */
  ~CodeEditorComponent() override { document_.removeListener(this); }

  /** The document given to the constructor. */
  CodeDocument& getDocument() const { return document_; }

  /** Fired after every change to the document: once at the end of each of
   *  this editor's own edits (a replaceAll is one call, not one per match),
   *  and at once for a change made elsewhere. The handler may delete the
   *  editor. */
  std::function<void()> onTextChange;
  /** Fired when the caret or the selection moves -- for one of the editor's
   *  own edits, after onTextChange, as that edit's last act. */
  std::function<void()> onCaretMoved;
  /** Ctrl+F and Ctrl+H: a plugin shows its own find or replace bar and calls
   *  findNext, replaceSelection and replaceAll from it. */
  std::function<void()> onFindRequested;
  /** Ctrl+H, as onFindRequested is Ctrl+F. Left empty, the key is not taken
   *  and goes on to the parent. */
  std::function<void()> onReplaceRequested;

  /** Replaces the document's text as opening a file does -- no undo back to
   *  what was there -- and puts the caret at the top. */
  bool loadContent(const std::string& text) {
    EditScope scope(*this);
    if (!document_.loadFromText(text)) return false;
    anchor_ = caret_ = CodeDocument::Position(document_, 0, 0);
    scrollX_ = scrollY_ = 0.0f;
    lastAction_ = Action::Other;
    syncBars();
    repaint();
    return true;
  }

  // ── Caret and selection ──────────────────────────────────────────────────

  /** A copy of the caret, not maintained: the next edit can leave it
   *  stale. */
  CodeDocument::Position getCaretPos() const { return caret_; }
  /** Whether the caret and the selection's anchor are apart. */
  bool hasSelection() const { return caret_ != anchor_; }
  /** The earlier of the caret and the anchor, as an unmaintained copy. */
  CodeDocument::Position getSelectionStart() const { return caret_ < anchor_ ? caret_ : anchor_; }
  /** The later of the caret and the anchor, as an unmaintained copy. */
  CodeDocument::Position getSelectionEnd() const { return caret_ < anchor_ ? anchor_ : caret_; }
  /** As UTF-8 with LF breaks; copyToClipboard gives them the document's
   *  newline. */
  std::string getSelectedText() const { return document_.getTextBetween(getSelectionStart(), getSelectionEnd()); }
  /** The document's text between two offsets, in either order, LF breaks. */
  std::string getTextInRange(int startOffset, int endOffset) const {
    return document_.getTextBetween(startOffset, endOffset);
  }

  /** Moves the caret, keeping the selection's anchor where it was when
   *  `selecting`, and brings it into view. */
  void moveCaretTo(const CodeDocument::Position& to, bool selecting) {
    endRun();
    caret_ = CodeDocument::Position(document_, to.getLineNumber(), to.getIndexInLine());
    if (!selecting) anchor_ = caret_;
    desiredColumn_ = -1;
    caretMoved();
  }

  /** Selects from `start` to `end`: the caret at the end. */
  void selectRegion(const CodeDocument::Position& start, const CodeDocument::Position& end) {
    endRun();
    anchor_ = CodeDocument::Position(document_, start.getLineNumber(), start.getIndexInLine());
    caret_ = CodeDocument::Position(document_, end.getLineNumber(), end.getIndexInLine());
    desiredColumn_ = -1;
    caretMoved();
  }

  /** Selects everything, the caret at the end, without scrolling to it. */
  void selectAll() {
    endRun();
    anchor_ = CodeDocument::Position(document_, 0, 0);
    caret_ = CodeDocument::Position(document_, document_.getNumLines() - 1, INT_MAX);
    desiredColumn_ = -1;
    caretMoved(false);
  }
  /** Collapses the selection onto the caret. onCaretMoved is not fired: the
   *  caret has not moved. */
  void deselectAll() {
    anchor_ = caret_;
    repaint();
  }

  // ── Moving ───────────────────────────────────────────────────────────────

  /** Left by one character -- never into a cluster, and from a line's start
   *  to the end of the line above -- or by a word (Ctrl+Left), extending the
   *  selection when `selecting`. A plain move with a selection goes to the
   *  selection's start instead. */
  void cursorLeft(bool moveInWholeWordSteps, bool selecting) {
    if (hasSelection() && !selecting && !moveInWholeWordSteps) {
      moveCaretTo(getSelectionStart(), false);
      return;
    }
    moveCaretTo(moveInWholeWordSteps ? document_.findWordBreakBefore(caret_) : previousStop(caret_), selecting);
  }
  /** Right by one character or a word, as cursorLeft mirrored; a plain move
   *  with a selection goes to the selection's end. */
  void cursorRight(bool moveInWholeWordSteps, bool selecting) {
    if (hasSelection() && !selecting && !moveInWholeWordSteps) {
      moveCaretTo(getSelectionEnd(), false);
      return;
    }
    moveCaretTo(moveInWholeWordSteps ? document_.findWordBreakAfter(caret_) : nextStop(caret_), selecting);
  }
  /** Up a line, at the column vertical movement began at so a short line on
   *  the way does not pull it left; from the first line, to the start. */
  void cursorUp(bool selecting) { moveVertically(caret_.getLineNumber() - 1, selecting); }
  /** Down a line, as cursorUp; from the last line, to the end. */
  void cursorDown(bool selecting) { moveVertically(caret_.getLineNumber() + 1, selecting); }
  /** Scrolls up a screenful of lines and moves the caret up as many. */
  void pageUp(bool selecting) {
    const int page = getNumLinesOnScreen();
    scrollBy(-page);
    moveVertically(caret_.getLineNumber() - page, selecting);
  }
  /** Scrolls down a screenful of lines and moves the caret down as many. */
  void pageDown(bool selecting) {
    const int page = getNumLinesOnScreen();
    scrollBy(page);
    moveVertically(caret_.getLineNumber() + page, selecting);
  }
  /** To the start of the document (Ctrl+Home). */
  void moveCaretToTop(bool selecting) { moveCaretTo(CodeDocument::Position(document_, 0, 0), selecting); }
  /** To the end of the document (Ctrl+End). */
  void moveCaretToEnd(bool selecting) {
    moveCaretTo(CodeDocument::Position(document_, document_.getNumLines() - 1, INT_MAX), selecting);
  }
  /** To the line's first non-space character, or to its very start from
   *  there -- Home twice goes back and forth between them. */
  void moveCaretToStartOfLine(bool selecting) {
    const int line = caret_.getLineNumber();
    const std::u32string& t = document_.getLineCodePoints(line);
    int first = 0;
    while (first < (int) t.size() && (t[(size_t) first] == ' ' || t[(size_t) first] == '\t')) ++first;
    moveCaretTo(CodeDocument::Position(document_, line, caret_.getIndexInLine() == first ? 0 : first), selecting);
  }
  /** To after the last character of the caret's line (End). */
  void moveCaretToEndOfLine(bool selecting) {
    moveCaretTo(CodeDocument::Position(document_, caret_.getLineNumber(), INT_MAX), selecting);
  }

  // ── Editing ──────────────────────────────────────────────────────────────

  /** Replaces the selection, or inserts at the caret, as one undo step. */
  bool insertTextAtCaret(const std::string& text) {
    EditScope scope(*this);
    if (readOnly_) return false;
    endRun();
    const bool ok = replaceSelectionWith(text);
    endRun();
    return ok;
  }

  /** A tab at the caret: spaces to the next tab stop, or a tab character. */
  void insertTabAtCaret() {
    if (readOnly_) return;
    const int column = indexToColumn(caret_.getLineNumber(), getSelectionStart().getIndexInLine());
    insertTextAtCaret(spacesForTabs_ ? std::string((size_t) (tabSize_ - column % tabSize_), ' ') : std::string("\t"));
  }

  /** Every line the selection touches moved in by one indent -- a line the
   *  selection only reaches the start of is not touched, nor is an empty
   *  line. */
  void indentSelection() {
    EditScope scope(*this);
    if (readOnly_) return;
    int first = 0, last = 0;
    selectedLines(&first, &last);
    const bool wasSelection = hasSelection();
    endRun();
    const std::string unit = indentUnit();
    for (int k = first; k <= last; ++k)
      if (document_.getLineLength(k) > 0) document_.insertText(CodeDocument::Position(document_, k, 0), unit);
    endRun();
    if (wasSelection) selectWholeLines(first, last);
  }

  /** Every line the selection touches moved out by one indent, or by what
   *  indent it has if that is less. */
  void unindentSelection() {
    EditScope scope(*this);
    if (readOnly_) return;
    int first = 0, last = 0;
    selectedLines(&first, &last);
    const bool wasSelection = hasSelection();
    endRun();
    for (int k = first; k <= last; ++k) {
      const std::u32string& t = document_.getLineCodePoints(k);
      int remove = 0, column = 0;
      while (remove < (int) t.size() && column < tabSize_) {
        if (t[(size_t) remove] == ' ') {
          ++remove;
          ++column;
        } else if (t[(size_t) remove] == '\t') {
          ++remove;
          break;
        } else {
          break;
        }
      }
      if (remove > 0)
        document_.deleteSection(CodeDocument::Position(document_, k, 0), CodeDocument::Position(document_, k, remove));
    }
    endRun();
    if (wasSelection) selectWholeLines(first, last);
  }

  /** Backspace: the selection, or what is before the caret -- a cluster, a
   *  word, or, in an indent made of spaces, back to the previous tab stop. */
  void deleteBackwards(bool moveInWholeWordSteps) {
    EditScope scope(*this);
    if (readOnly_) return;
    if (hasSelection()) {
      endRun();
      document_.deleteSection(getSelectionStart(), getSelectionEnd());
      endRun();
      caretMoved();
      return;
    }
    CodeDocument::Position from = moveInWholeWordSteps ? document_.findWordBreakBefore(caret_) : previousStop(caret_);
    if (!moveInWholeWordSteps && spacesForTabs_) {
      const std::u32string& t = document_.getLineCodePoints(caret_.getLineNumber());
      const int index = caret_.getIndexInLine();
      bool allSpaces = index > 0;
      for (int i = 0; i < index && allSpaces; ++i) allSpaces = t[(size_t) i] == ' ';
      if (allSpaces) {
        const int target = ((index - 1) / tabSize_) * tabSize_;
        from = CodeDocument::Position(document_, caret_.getLineNumber(), target);
      }
    }
    if (from == caret_) return;
    beginRun(Action::Deleting);
    document_.deleteSection(from, caret_);
    desiredColumn_ = -1;
    caretMoved();
  }

  /** Delete: the selection, or the cluster, word or line break after the
   *  caret. */
  void deleteForwards(bool moveInWholeWordSteps) {
    EditScope scope(*this);
    if (readOnly_) return;
    if (hasSelection()) {
      endRun();
      document_.deleteSection(getSelectionStart(), getSelectionEnd());
      endRun();
      caretMoved();
      return;
    }
    const CodeDocument::Position to =
        moveInWholeWordSteps ? document_.findWordBreakAfter(caret_) : nextStop(caret_);
    if (to == caret_) return;
    beginRun(Action::Deleting);
    document_.deleteSection(caret_, to);
    desiredColumn_ = -1;
    caretMoved();
  }

  /** Return: a line break, then the indent of the line it broke; one level
   *  more after an opening bracket, and if the closing one follows the caret,
   *  that goes onto a line of its own at the first indent. */
  void insertNewLine() {
    EditScope scope(*this);
    if (readOnly_) return;
    endRun();
    if (hasSelection()) document_.deleteSection(getSelectionStart(), getSelectionEnd());
    const std::string nl = "\n";
    if (!autoIndent_) {
      document_.insertText(caret_, nl);
      endRun();
      caretMoved();
      return;
    }
    const std::u32string t = document_.getLineCodePoints(caret_.getLineNumber());
    const int index = caret_.getIndexInLine();
    std::string indent;
    for (int i = 0; i < index && (t[(size_t) i] == ' ' || t[(size_t) i] == '\t'); ++i)
      indent.push_back((char) t[(size_t) i]);
    int before = index - 1;
    while (before >= 0 && (t[(size_t) before] == ' ' || t[(size_t) before] == '\t')) --before;
    int after = index;
    while (after < (int) t.size() && (t[(size_t) after] == ' ' || t[(size_t) after] == '\t')) ++after;
    const char32_t opener = before >= 0 ? t[(size_t) before] : 0;
    const char32_t closer = after < (int) t.size() ? t[(size_t) after] : 0;
    const bool opens = opener == '{' || opener == '(' || opener == '[';
    const bool closes = opens && ((opener == '{' && closer == '}') || (opener == '(' && closer == ')') ||
                                  (opener == '[' && closer == ']'));
    std::string text = nl + indent + (opens ? indentUnit() : std::string());
    if (closes) {
      const std::string tail = nl + indent;
      if (document_.insertText(caret_, text + tail)) {
        // Back from the closing bracket's line to the end of the new one.
        caret_.moveBy(-(int) detail::code::decodeText(tail).size());
        anchor_ = caret_;
      }
    } else {
      document_.insertText(caret_, text);
    }
    endRun();
    desiredColumn_ = -1;
    caretMoved();
  }

  /** A character typed, over the selection. A run of typing is one undo
   *  step; a '}' typed into an indent takes one level of it away. */
  bool insertTypedCharacter(char32_t c) {
    EditScope scope(*this);
    if (readOnly_) return false;
    beginRun(Action::Typing);
    if (hasSelection()) document_.deleteSection(getSelectionStart(), getSelectionEnd());
    if (autoIndent_ && c == '}') {
      const std::u32string& t = document_.getLineCodePoints(caret_.getLineNumber());
      const int index = caret_.getIndexInLine();
      bool blank = index > 0;
      for (int i = 0; i < index && blank; ++i) blank = t[(size_t) i] == ' ' || t[(size_t) i] == '\t';
      if (blank) {
        int remove = 0;
        if (t[(size_t) index - 1] == '\t') {
          remove = 1;
        } else {
          const int column = indexToColumn(caret_.getLineNumber(), index);
          const int target = ((column - 1) / tabSize_) * tabSize_;
          while (remove < index && t[(size_t) (index - 1 - remove)] == ' ' && column - remove > target) ++remove;
        }
        if (remove > 0) document_.deleteSection(caret_.movedBy(-remove), caret_);
      }
    }
    std::string utf8;
    detail::code::appendUtf8(utf8, c);
    const bool ok = document_.insertText(caret_, utf8);
    anchor_ = caret_;
    desiredColumn_ = -1;
    caretMoved();
    return ok;
  }

  // ── Clipboard ────────────────────────────────────────────────────────────

  /** The selection, with the document's newline for its line breaks. */
  bool copyToClipboard() const {
    if (!hasSelection()) return false;
    return Clipboard::setText(withNewLines(getSelectedText()));
  }
  /** Copies the selection, then deletes it as one undo step. False with no
   *  selection, in a read-only editor, or when the clipboard refused it --
   *  and then nothing is deleted. */
  bool cutToClipboard() {
    EditScope scope(*this);
    if (readOnly_ || !copyToClipboard()) return false;
    endRun();
    document_.deleteSection(getSelectionStart(), getSelectionEnd());
    endRun();
    caretMoved();
    return true;
  }
  /** Over the selection. Line breaks become the document's newline; a tab
   *  stays a tab; other control characters are dropped. */
  bool pasteFromClipboard() {
    if (readOnly_) return false;
    std::string incoming;
    if (!Clipboard::getText(&incoming) || incoming.empty()) return false;
    std::string clean;
    clean.reserve(incoming.size());
    for (char32_t c : detail::code::decodeText(incoming))
      if (c == '\t' || c == '\n' || !detail::code::isControl(c)) detail::code::appendUtf8(clean, c);
    if (clean.empty()) return false;
    return insertTextAtCaret(clean);
  }

  // ── Undo ─────────────────────────────────────────────────────────────────

  /** Undoes the last step and puts the caret where it happened. */
  bool undo() {
    EditScope scope(*this);
    if (readOnly_) return false;
    endRun();
    lastChange_ = -1;
    if (!document_.undo()) return false;
    afterHistory();
    return true;
  }
  /** Redoes the last undone step and puts the caret where it happened.
   *  False with nothing to redo, or in a read-only editor. */
  bool redo() {
    EditScope scope(*this);
    if (readOnly_) return false;
    endRun();
    lastChange_ = -1;
    if (!document_.redo()) return false;
    afterHistory();
    return true;
  }

  // ── Find and replace ─────────────────────────────────────────────────────

  /** Selects the next match after the selection, going round the end if
   *  asked; remembered for Ctrl+G. */
  bool findNext(const std::string& needle, bool caseSensitive = true, bool wrapAround = true) {
    lastSearch_ = needle;
    lastSearchCaseSensitive_ = caseSensitive;
    CodeDocument::Position s, e;
    if (!document_.find(needle, getSelectionEnd(), true, caseSensitive, wrapAround, &s, &e)) return false;
    selectRegion(s, e);
    return true;
  }
  /** Selects the match before the selection, going round the start if
   *  asked; remembered for Ctrl+Shift+G. */
  bool findPrevious(const std::string& needle, bool caseSensitive = true, bool wrapAround = true) {
    lastSearch_ = needle;
    lastSearchCaseSensitive_ = caseSensitive;
    CodeDocument::Position s, e;
    if (!document_.find(needle, getSelectionStart(), false, caseSensitive, wrapAround, &s, &e)) return false;
    selectRegion(s, e);
    return true;
  }
  /** Replaces the selection, as one undo step; false with none. */
  bool replaceSelection(const std::string& replacement) {
    if (readOnly_ || !hasSelection()) return false;
    return insertTextAtCaret(replacement);
  }
  /** Every match replaced, as one undo step: the number replaced. */
  int replaceAll(const std::string& needle, const std::string& replacement, bool caseSensitive = true) {
    EditScope scope(*this);
    if (readOnly_ || needle.empty()) return 0;
    endRun();
    const int step = (int) detail::code::decodeText(replacement).size();
    CodeDocument::Position from(document_, 0, 0), s, e;
    int count = 0;
    while (document_.find(needle, from, true, caseSensitive, false, &s, &e)) {
      if (!document_.replaceSection(s, e, replacement)) break;
      ++count;
      from = s.movedBy(step);
    }
    endRun();
    caretMoved();
    return count;
  }
  /** What the last findNext or findPrevious looked for, and what Ctrl+G
   *  looks for again; empty before either. */
  const std::string& lastSearch() const { return lastSearch_; }

  // ── Scrolling ────────────────────────────────────────────────────────────

  /** The 0-based line at the top of the text area, which may be partly
   *  scrolled out of it. */
  int getFirstLineOnScreen() const { return (int) std::floor(getScrollY() / lineHeight()); }
  /** The whole lines the text area has room for, at least 1. */
  int getNumLinesOnScreen() const { return (std::max)(1, (int) (geometry().text.h / lineHeight())); }
  /** The whole columns the text area has room for, at least 1. */
  int getNumColumnsOnScreen() const { return (std::max)(1, (int) (geometry().text.w / charWidth())); }
  /** The scroll, in pixels, held inside the text as it is now: a font or an
   *  edit since the last layout cannot leave it past the end. */
  float getScrollX() const { return scrollXIn(geometry()); }
  /** The vertical scroll, in pixels, held inside the text as getScrollX's
   *  is. */
  float getScrollY() const { return scrollYIn(geometry()); }
  /** Pixels from one line's top to the next: the font's line height,
   *  rounded up (16 with no usable font). */
  float getLineHeight() const { return lineHeight(); }
  /** Pixels per column: the width of a "0" in the font (8 with no usable
   *  font). */
  float getCharWidth() const { return charWidth(); }

  /** Scrolls so `firstLine` (0-based) is at the top, as far as the text
   *  allows. The caret does not move. */
  void scrollToLine(int firstLine) { scrollToPixels(getScrollX(), (float) firstLine * lineHeight()); }
  /** Scrolls by whole lines, negative up (Ctrl+Up and Ctrl+Down). The
   *  caret does not move. */
  void scrollBy(int deltaLines) { scrollToLine(getFirstLineOnScreen() + deltaLines); }
  /** Scrolls sideways so `column` is at the left edge, as far as the text
   *  allows. */
  void scrollToColumn(int column) { scrollToPixels((float) column * charWidth(), getScrollY()); }

  /** The least scroll that shows the caret. */
  void scrollToKeepCaretOnScreen() {
    const Geometry gm = geometry();
    const float lh = lineHeight(), cw = charWidth();
    const float top = (float) caret_.getLineNumber() * lh;
    float y = scrollYIn(gm), x = scrollXIn(gm);
    if (top < y) y = top;
    else if (top + lh > y + gm.text.h) y = top + lh - gm.text.h;
    const float cx = (float) indexToColumn(caret_.getLineNumber(), caret_.getIndexInLine()) * cw;
    const float room = (std::max)(cw, gm.text.w - kTextPadding - cw);
    if (cx < x) x = cx;
    else if (cx > x + room) x = cx - room;
    scrollToPixels(x, y);
  }

  // ── Geometry ─────────────────────────────────────────────────────────────

  /** The column an index in a line is drawn at: tabs reach their stops, wide
   *  characters take two, marks none. */
  int indexToColumn(int line, int index) const {
    const std::u32string& s = document_.getLineCodePoints(line);
    const size_t n = (std::min)((size_t) (std::max)(index, 0), s.size());
    int column = 0;
    for (size_t i = 0; i < n; ++i) column += detail::code::columnsOf(s, i, column, tabSize_);
    return column;
  }
  /** The first caret index at or past a column. */
  int columnToIndex(int line, int column) const {
    const std::u32string& s = document_.getLineCodePoints(line);
    int col = 0;
    for (size_t i = 0; i < s.size(); ++i) {
      if (col >= column && detail::code::isStop(s, i)) return (int) i;
      col += detail::code::columnsOf(s, i, col, tabSize_);
    }
    return (int) s.size();
  }

  /** The caret position nearest a point in this component's coordinates. */
  CodeDocument::Position getPositionAt(float x, float y) const {
    const Geometry gm = geometry();
    const double row = std::floor(((double) y - gm.text.y + scrollYIn(gm)) / lineHeight());
    const int count = document_.getNumLines();
    const int line = row < 0.0 ? 0 : (row >= (double) count ? count - 1 : (int) row);
    const float column = (x - (gm.text.x + kTextPadding - scrollXIn(gm))) / charWidth();
    return CodeDocument::Position(document_, line, indexNearestColumn(line, column));
  }

  /** Where the character at a position is drawn. */
  Rect getCharacterBounds(const CodeDocument::Position& pos) const {
    const Geometry gm = geometry();
    const float x = gm.text.x + kTextPadding - scrollXIn(gm) +
                    (float) indexToColumn(pos.getLineNumber(), pos.getIndexInLine()) * charWidth();
    return Rect(x, lineTop(gm, pos.getLineNumber()), charWidth(), lineHeight());
  }

  bool caretBounds(Rect* out) const override {
    if (!out) return false;
    const Rect r = getCharacterBounds(caret_);
    *out = Rect(r.x, r.y, 2.0f, r.h);
    return true;
  }

  // ── Settings ─────────────────────────────────────────────────────────────

  /** Tab stops every `numSpaces` columns, held to 1..64, and whether Tab
   *  and an indent insert spaces to the next stop rather than a tab
   *  character. 4 and spaces by default. */
  void setTabSize(int numSpaces, bool insertSpacesInsteadOfTabs) {
    tabSize_ = (std::max)(1, (std::min)(numSpaces, 64));
    spacesForTabs_ = insertSpacesInsteadOfTabs;
    syncBars();
    repaint();
  }
  /** Columns between tab stops, 1..64. */
  int getTabSize() const { return tabSize_; }
  /** True by default. */
  bool areSpacesInsertedForTabs() const { return spacesForTabs_; }

  /** Shows or hides the line-number gutter at the left, shown by default.
   *  It is as wide as the number of lines needs, three digits at least; a
   *  click in it selects whole lines. */
  void setLineNumbersShown(bool shown) {
    showLineNumbers_ = shown;
    syncBars();
    repaint();
  }
  /** True by default. */
  bool areLineNumbersShown() const { return showLineNumbers_; }

  /** Navigation, selection and copy still work; nothing edits. */
  void setReadOnly(bool readOnly) {
    readOnly_ = readOnly;
    repaint();
  }
  /** False by default. */
  bool isReadOnly() const { return readOnly_; }

  /** On by default: Return keeps the line's indent and adds a level after an
   *  opening bracket, and a `}` typed into an indent takes a level away. */
  void setAutoIndent(bool on) { autoIndent_ = on; }
  /** True by default. */
  bool isAutoIndent() const { return autoIndent_; }

  /** The same face at another size; the monospaced one if none was set. */
  void setFontSize(float points) {
    const Font& f = font();
    if (f.isValid()) setFont(f.withHeight(points));
    else if (std::shared_ptr<Typeface> face = monospacedTypeface()) setFont(Font(face, points));
    syncBars();
    scrollToKeepCaretOnScreen();
  }

  /** The token colours, by type number, in place of the tokeniser's
   *  default; setTokeniser puts its default back. */
  void setColourScheme(const ColourScheme& scheme) {
    scheme_ = scheme;
    repaint();
  }
  /** The token colours in use: the tokeniser's default unless set; empty
   *  with no tokeniser. */
  const ColourScheme& getColourScheme() const { return scheme_; }
  /** The scheme's colour for a type number, or Colours::defaultText for one
   *  it has no entry for. */
  Colour getColourForTokenType(int type) const {
    return type >= 0 && type < (int) scheme_.types.size() ? scheme_.types[(size_t) type].colour : colours_.defaultText;
  }
  /** The colours that are not a token's: the background, plain text, the
   *  selection, the gutter and the caret. */
  void setColours(const Colours& colours) {
    colours_ = colours;
    repaint();
  }
  /** The reference's light defaults unless set. */
  const Colours& getColours() const { return colours_; }

  /** Another tokeniser, or none; the colour scheme becomes its default. */
  void setTokeniser(CodeTokeniser* tokeniser) {
    tokeniser_ = tokeniser;
    scheme_ = tokeniser_ ? tokeniser_->getDefaultColourScheme() : ColourScheme();
    statesValid_ = 1;
    reuseFrom_ = reuseTo_ = -1;
    repaint();
  }

  /** The tokens of a line as they are drawn, from the state the lines above
   *  leave. */
  std::vector<CodeTokeniser::Token> getTokensForLine(int line) {
    std::vector<CodeTokeniser::Token> tokens;
    if (!tokeniser_ || line < 0 || line >= document_.getNumLines()) return tokens;
    ensureStates(line);
    const std::u32string& t = document_.getLineCodePoints(line);
    tokeniser_->tokeniseLine(t.data(), (int) t.size(), states_[(size_t) line], &tokens);
    return tokens;
  }
  /** How many lines from the top have a known tokeniser state: what painting
   *  the screen, or an edit, has cost in scanning. */
  int getNumLinesWithKnownState() const { return statesValid_; }

  // ── Keyboard ─────────────────────────────────────────────────────────────

  bool keyPressed(const KeyPress& key) override {
    if (!isEnabled()) return false;
    const bool shift = key.shiftDown, ctrl = key.ctrlDown;
    switch (key.keyCode) {
      case KeyPress::Left:
        cursorLeft(ctrl, shift);
        return true;
      case KeyPress::Right:
        cursorRight(ctrl, shift);
        return true;
      case KeyPress::Up:
        if (ctrl && !shift) scrollBy(-1);
        else cursorUp(shift);
        return true;
      case KeyPress::Down:
        if (ctrl && !shift) scrollBy(1);
        else cursorDown(shift);
        return true;
      case KeyPress::PageUp:
        pageUp(shift);
        return true;
      case KeyPress::PageDown:
        pageDown(shift);
        return true;
      case KeyPress::Home:
        if (ctrl) moveCaretToTop(shift);
        else moveCaretToStartOfLine(shift);
        return true;
      case KeyPress::End:
        if (ctrl) moveCaretToEnd(shift);
        else moveCaretToEndOfLine(shift);
        return true;
      case KeyPress::Backspace:
        deleteBackwards(ctrl);
        return true;
      case KeyPress::Delete:
        deleteForwards(ctrl);
        return true;
      case KeyPress::Return:
        insertNewLine();
        return true;
      case KeyPress::Tab:
        // Ctrl+Tab and a read-only editor leave Tab to focus traversal; in
        // an editor that edits, Tab is indentation, as in every code editor.
        if (ctrl || key.altDown || readOnly_) return false;
        if (shift) unindentSelection();
        else if (hasSelection() && getSelectionStart().getLineNumber() != getSelectionEnd().getLineNumber())
          indentSelection();
        else insertTabAtCaret();
        return true;
      case KeyPress::Escape:
        return false; // whatever contains the editor closes, as with a field
      default:
        break;
    }
    if (ctrl && !key.altDown) {
      const char32_t c = key.character >= 'A' && key.character <= 'Z' ? key.character + 32 : key.character;
      switch (c) {
        case 'a': selectAll(); return true;
        case 'c': copyToClipboard(); return true;
        case 'x': cutToClipboard(); return true;
        case 'v': pasteFromClipboard(); return true;
        case 'z':
          if (shift) redo();
          else undo();
          return true;
        case 'y': redo(); return true;
        case ']': indentSelection(); return true;
        case '[': unindentSelection(); return true;
        case 'f':
          if (!onFindRequested) return false;
          onFindRequested();
          return true;
        case 'h':
          if (!onReplaceRequested) return false;
          onReplaceRequested();
          return true;
        case 'g':
          if (lastSearch_.empty()) return false;
          if (shift) findPrevious(lastSearch_, lastSearchCaseSensitive_);
          else findNext(lastSearch_, lastSearchCaseSensitive_);
          return true;
        default:
          return false;
      }
    }
    if (ctrl || key.altDown) return false; // somebody else's shortcut
    if (!key.isCharacter() || readOnly_) return false;
    // Control characters are not typed text -- the tab and the CR a peer
    // sends after Tab and Return among them, which the keys above handled.
    // Nor is what no character is.
    const char32_t c = key.character;
    if (detail::code::isControl(c) || (c >= 0xD800 && c <= 0xDFFF) || c > 0x10FFFF) return false;
    insertTypedCharacter(c);
    return true;
  }

  /** Repaints: the caret is drawn only while the editor has keyboard focus
   *  and is not read-only. */
  void focusGained() override { repaint(); }
  /** Ends the run of typing, so what is typed after focus comes back is an
   *  undo step of its own. */
  void focusLost() override {
    endRun();
    repaint();
  }

  // ── Mouse ────────────────────────────────────────────────────────────────

  /** A click puts the caret there, Shift extending the selection; a double
   *  click selects the word, punctuation or spaces under it; a triple click,
   *  or a click in the line-number gutter, selects whole lines -- a Shift
   *  click in the gutter from the line the last such selection began at.
   *  Nothing happens while the editor is disabled. */
  void mouseDown(const MouseEvent& e) override {
    if (!isEnabled()) return;
    endRun();
    const Geometry gm = geometry();
    const CodeDocument::Position p = getPositionAt(e.position.x, e.position.y);
    const bool inGutter = showLineNumbers_ && e.position.x < gm.gutter.right();
    if (inGutter || e.clickCount >= 3) {
      drag_ = DragMode::Lines;
      if (!(inGutter && e.shiftDown)) dragLine_ = p.getLineNumber();
      selectLineRange(dragLine_, p.getLineNumber());
      return;
    }
    if (e.clickCount == 2) {
      drag_ = DragMode::Words;
      CodeDocument::Position s, en;
      document_.findTokenContaining(p, s, en);
      dragFromLine_ = s.getLineNumber();
      dragFromIndex_ = s.getIndexInLine();
      dragToLine_ = en.getLineNumber();
      dragToIndex_ = en.getIndexInLine();
      selectRegion(s, en);
      return;
    }
    drag_ = DragMode::Characters;
    moveCaretTo(p, e.shiftDown);
  }

  /** Extends the selection by characters, words or lines, as the press that
   *  began the drag chose. */
  void mouseDrag(const MouseEvent& e) override {
    if (!isEnabled() || drag_ == DragMode::None) return;
    const CodeDocument::Position p = getPositionAt(e.position.x, e.position.y);
    if (drag_ == DragMode::Lines) {
      selectLineRange(dragLine_, p.getLineNumber());
    } else if (drag_ == DragMode::Words) {
      CodeDocument::Position s, en;
      document_.findTokenContaining(p, s, en);
      const CodeDocument::Position from(document_, dragFromLine_, dragFromIndex_), to(document_, dragToLine_, dragToIndex_);
      if (s < from) selectRegion(to, s);
      else selectRegion(from, en > to ? en : to);
    } else {
      moveCaretTo(p, true);
    }
  }

  void mouseUp(const MouseEvent&) override { drag_ = DragMode::None; }

  bool mouseWheel(const MouseEvent& e, float delta) override {
    if (delta == 0.0f) return false;
    const float x = getScrollX(), y = getScrollY();
    if (e.shiftDown) scrollToPixels(x - delta * 3.0f * charWidth(), y);
    else scrollToPixels(x, y - delta * 3.0f * lineHeight());
    return x != getScrollX() || y != getScrollY(); // nothing moved: the parent's turn
  }

  bool wouldScroll() const override {
    const Geometry gm = geometry();
    return gm.contentH > gm.text.h || gm.contentW > gm.text.w;
  }

  // ── Accessibility ────────────────────────────────────────────────────────

  AccessibleInfo accessibleInfo() const override {
    AccessibleInfo info = baseInfo(AccessibleRole::TextField);
    // The whole text while it is small enough for a reader to want, and the
    // caret's line past that: a value of four megabytes helps nobody.
    if (document_.getNumCharacters() <= kAccessibleTextLimit) info.value = document_.getAllContent();
    else info.value = document_.getLine(caret_.getLineNumber());
    if (info.description.empty())
      info.description = "Line " + std::to_string(caret_.getLineNumber() + 1) + " of " +
                         std::to_string(document_.getNumLines()) + ", column " +
                         std::to_string(indexToColumn(caret_.getLineNumber(), caret_.getIndexInLine()) + 1);
    return info;
  }

  // ── Painting ─────────────────────────────────────────────────────────────

  /** Places the scroll bars, each shown only while the text overflows that
   *  way, and holds the scroll inside the text. */
  void resized() override { syncBars(); }

  /** The background, the gutter and its numbers, the selection, the lines on
   *  screen in their tokens' colours -- a control character as a small box --
   *  and, with keyboard focus and not read-only, the caret. Only the lines
   *  on screen are tokenised. */
  void paint(Graphics& g) override {
    syncBars();
    const Geometry gm = geometry();
    const Font& f = font();
    const float lh = lineHeight(), cw = charWidth();
    g.setColour(colours_.background);
    g.fillRect(localBounds());

    const int count = document_.getNumLines();
    const int first = (std::max)(0, (std::min)(count - 1, (int) std::floor(scrollY_ / lh)));
    const int last = (std::min)(count, (int) std::ceil((scrollY_ + gm.text.h) / lh) + 1);
    const float baseline = (lh - (ascent_ + descent_)) * 0.5f + ascent_;

    if (showLineNumbers_ && gm.gutter.w > 0.0f) {
      g.setColour(colours_.lineNumberBackground);
      g.fillRect(gm.gutter);
      Graphics::ScopedState clip(g);
      g.clipTo(gm.gutter);
      g.setColour(colours_.lineNumberText);
      for (int k = first; k < last && f.isValid(); ++k) {
        const std::string number = std::to_string(k + 1);
        const float w = monospaced_ ? (float) number.size() * cw : f.stringWidth(number);
        f.draw(g, number, gm.gutter.right() - cw * 0.75f - w, lineTop(gm, k) + baseline);
      }
    }

    Graphics::ScopedState clip(g);
    g.clipTo(gm.text);
    if (g.isClippedOut()) return;
    const float originX = gm.text.x + kTextPadding - scrollX_;

    if (hasSelection()) {
      const CodeDocument::Position s = getSelectionStart(), e = getSelectionEnd();
      g.setColour(colours_.highlight);
      for (int k = (std::max)(first, s.getLineNumber()); k <= (std::min)(last - 1, e.getLineNumber()); ++k) {
        const int from = k == s.getLineNumber() ? s.getIndexInLine() : 0;
        const int to = k == e.getLineNumber() ? e.getIndexInLine() : document_.getLineLength(k);
        const float x0 = originX + (float) indexToColumn(k, from) * cw;
        float x1 = originX + (float) indexToColumn(k, to) * cw;
        if (k < e.getLineNumber()) x1 += cw * 0.5f; // the line break, selected
        if (x1 > x0) g.fillRect(Rect(x0, lineTop(gm, k), x1 - x0, lh));
      }
    }

    if (f.isValid()) {
      if (tokeniser_) ensureStates(last - 1);
      for (int k = first; k < last; ++k) drawLine(g, f, gm, k, originX, lineTop(gm, k) + baseline);
    }

    if (hasKeyboardFocus() && !readOnly_) {
      g.setColour(colours_.caret);
      const float cx = originX + (float) indexToColumn(caret_.getLineNumber(), caret_.getIndexInLine()) * cw;
      g.fillRect(Rect(cx, lineTop(gm, caret_.getLineNumber()), 2.0f, lh));
    }
  }

private:
  enum class Action { None, Typing, Deleting, Other };
  enum class DragMode { None, Characters, Words, Lines };

  /** Where things are this frame, worked out from the bounds and the
   *  document by geometry(): the line-number gutter, the text area, which
   *  scroll bars are showing, and the size of all the text in pixels. Made
   *  afresh wherever it is needed -- painting, hit-testing, scrolling --
   *  rather than cached, so it cannot go stale after an edit or a resize. */
  struct Geometry {
    Rect gutter, text;
    bool vbar = false, hbar = false;
    float contentW = 0.0f, contentH = 0.0f;
  };

  // ── Document changes ─────────────────────────────────────────────────────

  /**
   * The states below an edit are kept, moved with their lines, as a range
   * that may be reused: the state at the start of a line depends only on
   * the lines above it, so once a re-scan from the edit arrives at a line
   * below it in the state that line had before, every state after that is
   * what it was. Typing at the top of a long file and then going to its end
   * scans the lines between the edit and the first line that agrees -- one,
   * usually -- rather than the file.
   */
  void codeDocumentLinesChanged(int firstLine, int lineDelta) override {
    const size_t at = (size_t) firstLine + 1;
    if (lineDelta > 0) {
      states_.insert(states_.begin() + (long) (std::min)(at, states_.size()), (size_t) lineDelta, 0u);
    } else if (lineDelta < 0) {
      const size_t from = (std::min)(at, states_.size());
      const size_t to = (std::min)(from + (size_t) -lineDelta, states_.size());
      states_.erase(states_.begin() + (long) from, states_.begin() + (long) to);
    }
    const int changedEnd = firstLine + (std::max)(lineDelta, 0); // the last line whose text changed
    if (statesValid_ > firstLine + 1) {
      // Known down past the edit: those below it are the range to reuse.
      reuseFrom_ = changedEnd + 1;
      reuseTo_ = statesValid_ + lineDelta;
    } else if (reuseFrom_ >= 0 && firstLine + 1 < reuseTo_) {
      // An edit at or above an earlier range moves it, and cuts off what it
      // reaches; an edit below it leaves it alone.
      reuseFrom_ = (std::max)(reuseFrom_ + (firstLine < reuseFrom_ ? lineDelta : 0), changedEnd + 1);
      reuseTo_ += lineDelta;
    }
    if (reuseFrom_ >= reuseTo_ || reuseFrom_ < 1) reuseFrom_ = reuseTo_ = -1;
    if (statesValid_ > firstLine + 1) statesValid_ = firstLine + 1;
    if (statesValid_ < 1) statesValid_ = 1;
    repaint();
    if (editDepth_ > 0) textChangePending_ = true; // told at the end of the editor's own edit
    else if (onTextChange) onTextChange();
  }
  void codeDocumentTextInserted(int, int endIndex) override { lastChange_ = endIndex; }
  void codeDocumentTextDeleted(int startIndex, int) override { lastChange_ = startIndex; }

  void afterHistory() {
    if (lastChange_ >= 0) anchor_ = caret_ = CodeDocument::Position(document_, lastChange_);
    else anchor_ = caret_;
    desiredColumn_ = -1;
    caretMoved();
  }

  // ── Runs of typing ───────────────────────────────────────────────────────
  //
  // A run of typed characters is one undo step, and so is a run of
  // backspaces; anything else -- a move, a click, a paste, a Return -- ends
  // the run, so undo takes back a word rather than a letter and never two
  // different kinds of thing at once.

  void beginRun(Action kind) {
    if (lastAction_ != kind) document_.newTransaction();
    lastAction_ = kind;
  }
  void endRun() {
    document_.newTransaction();
    lastAction_ = Action::Other;
  }

  bool replaceSelectionWith(const std::string& text) {
    bool ok = true;
    if (hasSelection()) ok = document_.replaceSection(getSelectionStart(), getSelectionEnd(), text);
    else ok = document_.insertText(caret_, text);
    anchor_ = caret_;
    desiredColumn_ = -1;
    caretMoved();
    return ok;
  }

  void caretMoved(bool scroll = true) {
    if (scroll) scrollToKeepCaretOnScreen();
    repaint();
    if (editDepth_ > 0) caretMovePending_ = true;
    else if (onCaretMoved) onCaretMoved();
  }

  /**
   * One editing operation of the editor's own. The document reports each
   * change while the operation is still running, and the handlers it would
   * reach are user code that may delete the editor -- a panel closed on a
   * keyword typed. So onTextChange and onCaretMoved wait here and fire as the
   * operation's very last act, at most once each, text first; nested scopes
   * fire at the outermost. An edit made to the document from elsewhere is
   * reported at once, as before.
   */
  struct EditScope {
    explicit EditScope(CodeEditorComponent& e)
        : editor(e), alive(&e), exceptions(std::uncaught_exceptions()) {
      ++editor.editDepth_;
    }
    ~EditScope() noexcept(false) {
      if (alive.deleted) return;
      if (--editor.editDepth_ > 0) return;
      const bool text = editor.textChangePending_, caret = editor.caretMovePending_;
      editor.textChangePending_ = editor.caretMovePending_ = false;
      if (std::uncaught_exceptions() > exceptions) return; // unwinding: no user code
      if (text && editor.onTextChange) editor.onTextChange();
      if (alive.deleted) return;
      if (caret && editor.onCaretMoved) editor.onCaretMoved();
    }
    EditScope(const EditScope&) = delete;
    EditScope& operator=(const EditScope&) = delete;

    CodeEditorComponent& editor;
    detail::DeletionWatch alive;
    const int exceptions;
  };

  std::string indentUnit() const { return spacesForTabs_ ? std::string((size_t) tabSize_, ' ') : std::string("\t"); }

  std::string withNewLines(const std::string& text) const {
    const std::string nl = document_.getNewLineCharacters();
    if (nl == "\n") return text;
    std::string out;
    out.reserve(text.size() + text.size() / 16);
    for (char c : text) {
      if (c == '\n') out += nl;
      else out.push_back(c);
    }
    return out;
  }

  /** The lines the selection touches, a last line it only reaches the start
   *  of left out. */
  void selectedLines(int* first, int* last) const {
    const CodeDocument::Position s = getSelectionStart(), e = getSelectionEnd();
    *first = s.getLineNumber();
    *last = e.getLineNumber();
    if (*last > *first && e.getIndexInLine() == 0) --*last;
  }

  void selectWholeLines(int first, int last) {
    anchor_ = CodeDocument::Position(document_, first, 0);
    caret_ = CodeDocument::Position(document_, last, INT_MAX);
    desiredColumn_ = -1;
    caretMoved();
  }

  /** From line a to line b, in the direction of the drag, breaks included. */
  void selectLineRange(int a, int b) {
    const int count = document_.getNumLines();
    auto lineEnd = [&](int k) {
      return k + 1 < count ? CodeDocument::Position(document_, k + 1, 0) : CodeDocument::Position(document_, k, INT_MAX);
    };
    if (b >= a) selectRegion(CodeDocument::Position(document_, a, 0), lineEnd(b));
    else selectRegion(lineEnd(a), CodeDocument::Position(document_, b, 0));
  }

  CodeDocument::Position previousStop(const CodeDocument::Position& p) const {
    const int line = p.getLineNumber(), index = p.getIndexInLine();
    if (index > 0) {
      const std::u32string& s = document_.getLineCodePoints(line);
      size_t i = (size_t) index - 1;
      while (i > 0 && !detail::code::isStop(s, i)) --i;
      return CodeDocument::Position(document_, line, (int) i);
    }
    if (line == 0) return CodeDocument::Position(document_, 0, 0);
    return CodeDocument::Position(document_, line - 1, INT_MAX);
  }
  CodeDocument::Position nextStop(const CodeDocument::Position& p) const {
    const int line = p.getLineNumber(), index = p.getIndexInLine();
    const std::u32string& s = document_.getLineCodePoints(line);
    if ((size_t) index < s.size()) {
      size_t i = (size_t) index + 1;
      while (i < s.size() && !detail::code::isStop(s, i)) ++i;
      return CodeDocument::Position(document_, line, (int) i);
    }
    if (line + 1 >= document_.getNumLines()) return p;
    return CodeDocument::Position(document_, line + 1, 0);
  }

  /** Up or down to a line at the column vertical movement began at, so
   *  passing through a short line does not drift left; past the first line
   *  is the start, past the last the end. */
  void moveVertically(int targetLine, bool selecting) {
    endRun();
    if (desiredColumn_ < 0) desiredColumn_ = indexToColumn(caret_.getLineNumber(), caret_.getIndexInLine());
    const int column = desiredColumn_;
    const int count = document_.getNumLines();
    if (targetLine < 0) caret_ = CodeDocument::Position(document_, 0, 0);
    else if (targetLine >= count) caret_ = CodeDocument::Position(document_, count - 1, INT_MAX);
    else caret_ = CodeDocument::Position(document_, targetLine, indexNearestColumn(targetLine, (float) column));
    if (!selecting) anchor_ = caret_;
    desiredColumn_ = column;
    caretMoved();
  }

  /** The caret index nearest a fractional column, never inside a cluster. */
  int indexNearestColumn(int line, float column) const {
    const std::u32string& s = document_.getLineCodePoints(line);
    int col = 0;
    for (size_t i = 0; i < s.size();) {
      int width = detail::code::columnsOf(s, i, col, tabSize_);
      size_t end = i + 1;
      while (end < s.size() && !detail::code::isStop(s, end)) ++end;
      if (column < (float) col + (float) width * 0.5f) return (int) i;
      col += width;
      i = end;
    }
    return (int) s.size();
  }

  // ── Metrics and layout ───────────────────────────────────────────────────

  void refreshMetrics() const {
    const Font& f = font();
    const Typeface* face = f.typeface().get();
    if (metricsValid_ && face == metricsFace_ && f.height() == metricsHeight_) return;
    metricsValid_ = true;
    metricsFace_ = face;
    metricsHeight_ = f.height();
    if (f.isValid()) {
      charWidth_ = f.stringWidth("0");
      lineHeight_ = std::ceil(f.lineHeight());
      ascent_ = f.ascent();
      descent_ = f.descent();
      // Monospaced if the narrowest and the widest letters agree with the
      // digit: then a run of ASCII can be drawn as one string and still land
      // on its columns.
      auto same = [&](const char* t) { return std::fabs(f.stringWidth(t) - charWidth_) < 0.01f; };
      monospaced_ = same("i") && same("M") && same("W") && same(" ") && same("l");
    } else {
      charWidth_ = 8.0f;
      lineHeight_ = 16.0f;
      ascent_ = 12.0f;
      descent_ = 4.0f;
      monospaced_ = false;
    }
    if (!(charWidth_ > 0.0f)) charWidth_ = (std::max)(1.0f, f.height() * 0.6f);
    if (!(lineHeight_ > 0.0f)) lineHeight_ = 16.0f;
  }
  float charWidth() const {
    refreshMetrics();
    return charWidth_;
  }
  float lineHeight() const {
    refreshMetrics();
    return lineHeight_;
  }

  float gutterWidth() const {
    int digits = 1;
    for (int n = document_.getNumLines(); n >= 10; n /= 10) ++digits;
    return ((float) (std::max)(digits, 3) + 1.5f) * charWidth();
  }

  /** Where the gutter, the text and the bars go -- the bars decided
   *  together, as Viewport decides them, since each takes room from the
   *  other. */
  Geometry geometry() const {
    Geometry gm;
    const Rect area = localBounds();
    const float cw = charWidth(), lh = lineHeight();
    const float t = ScrollBar::kThickness;
    const float gutter = showLineNumbers_ ? gutterWidth() : 0.0f;
    const int caretColumn = indexToColumn(caret_.getLineNumber(), caret_.getIndexInLine());
    gm.contentH = (float) document_.getNumLines() * lh;
    gm.contentW = (float) ((std::max)(document_.getMaximumLineLength(), caretColumn) + 2) * cw + kTextPadding;
    const float availW = (std::max)(0.0f, area.w - gutter), availH = area.h;
    bool v = gm.contentH > availH;
    const bool h = gm.contentW > availW - (v ? t : 0.0f);
    if (h && !v) v = gm.contentH > availH - t;
    gm.vbar = v;
    gm.hbar = h;
    const float textH = (std::max)(0.0f, availH - (h ? t : 0.0f));
    gm.gutter = Rect(0.0f, 0.0f, gutter, textH);
    gm.text = Rect(gutter, 0.0f, (std::max)(0.0f, availW - (v ? t : 0.0f)), textH);
    return gm;
  }

  float lineTop(const Geometry& gm, int line) const {
    return gm.text.y + (float) ((double) line * (double) lineHeight() - (double) scrollYIn(gm));
  }

  float scrollXIn(const Geometry& gm) const {
    return (std::max)(0.0f, (std::min)(scrollX_, (std::max)(0.0f, gm.contentW - gm.text.w)));
  }
  float scrollYIn(const Geometry& gm) const {
    return (std::max)(0.0f, (std::min)(scrollY_, (std::max)(0.0f, gm.contentH - gm.text.h)));
  }

  void scrollToPixels(float x, float y) {
    const Geometry gm = geometry();
    const float maxX = (std::max)(0.0f, gm.contentW - gm.text.w), maxY = (std::max)(0.0f, gm.contentH - gm.text.h);
    x = (std::max)(0.0f, (std::min)(x, maxX));
    y = (std::max)(0.0f, (std::min)(y, maxY));
    if (x == scrollX_ && y == scrollY_) return;
    scrollX_ = x;
    scrollY_ = y;
    syncBars();
    repaint();
  }

  /** The bars put where geometry() says, showing the scroll -- and the
   *  scroll held inside the text, which an edit may have shortened. */
  void syncBars() {
    const Geometry gm = geometry();
    const Rect area = localBounds();
    const float t = ScrollBar::kThickness;
    scrollX_ = (std::max)(0.0f, (std::min)(scrollX_, (std::max)(0.0f, gm.contentW - gm.text.w)));
    scrollY_ = (std::max)(0.0f, (std::min)(scrollY_, (std::max)(0.0f, gm.contentH - gm.text.h)));
    syncingBars_ = true;
    vertical_.setVisible(gm.vbar);
    horizontal_.setVisible(gm.hbar);
    vertical_.setBounds({area.w - t, 0.0f, t, gm.text.h});
    horizontal_.setBounds({gm.text.x, area.h - t, gm.text.w, t});
    vertical_.setRange(gm.contentH, gm.text.h, scrollY_);
    horizontal_.setRange(gm.contentW, gm.text.w, scrollX_);
    syncingBars_ = false;
  }

  // ── Tokens ───────────────────────────────────────────────────────────────

  /** The state at the start of every line through `line` known: scanned
   *  from the first line not known, without building tokens. */
  void ensureStates(int line) {
    if (!tokeniser_) return;
    const int count = document_.getNumLines();
    if ((int) states_.size() != count) {
      states_.resize((size_t) count, 0u);
      statesValid_ = (std::max)(1, (std::min)(statesValid_, count));
    }
    states_[0] = 0;
    if (reuseTo_ > count) reuseFrom_ = reuseTo_ = -1;
    line = (std::min)(line, count - 1);
    while (statesValid_ <= line) {
      const int k = statesValid_ - 1;
      const std::u32string& t = document_.getLineCodePoints(k);
      const uint32_t next = tokeniser_->tokeniseLine(t.data(), (int) t.size(), states_[(size_t) k], nullptr);
      const int below = k + 1;
      if (reuseFrom_ >= 0 && below >= reuseFrom_) {
        if (below < reuseTo_ && states_[(size_t) below] == next) {
          // Arrived where the range was scanned from before, in the same
          // state: the rest of it stands.
          statesValid_ = reuseTo_;
          reuseFrom_ = reuseTo_ = -1;
          continue;
        }
        if (below >= reuseTo_ - 1) reuseFrom_ = reuseTo_ = -1;
      }
      states_[(size_t) below] = next;
      ++statesValid_;
    }
  }

  static bool sameColour(Colour a, Colour b) { return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a; }

  /**
   * One line, a column at a time. Printable ASCII in a monospaced font goes
   * out in runs of one colour -- one draw for a token, which is what keeps a
   * screenful cheap; anything else is drawn a cluster at a time at its own
   * column, so a fallback glyph of another width cannot push the rest of the
   * line off its grid. A control character is drawn as a box: invisible, it
   * would still take a column, and a caret stepping over nothing confuses.
   */
  void drawLine(Graphics& g, const Font& f, const Geometry& gm, int line, float originX, float baseline) {
    const std::u32string& s = document_.getLineCodePoints(line);
    if (s.empty()) return;
    tokens_.clear();
    if (tokeniser_)
      tokeniser_->tokeniseLine(s.data(), (int) s.size(), states_[(size_t) line], &tokens_);
    const float cw = charWidth();
    const int firstColumn = (int) std::floor(scrollX_ / cw) - 2;
    const int lastColumn = (int) std::ceil((scrollX_ + gm.text.w) / cw) + 2;
    size_t tk = 0;
    int col = 0;
    std::string run;
    int runColumn = 0;
    Colour runColour = colours_.defaultText;
    auto flush = [&] {
      if (run.empty()) return;
      g.setColour(runColour);
      f.draw(g, run, originX + (float) runColumn * cw, baseline);
      run.clear();
    };
    for (size_t i = 0; i < s.size();) {
      if (col > lastColumn) break;
      const char32_t c = s[i];
      while (tk < tokens_.size() && (size_t) (tokens_[tk].start + tokens_[tk].length) <= i) ++tk;
      const Colour colour = (tk < tokens_.size() && (size_t) tokens_[tk].start <= i) ? getColourForTokenType(tokens_[tk].type)
                                                                                     : colours_.defaultText;
      if (c == '\t') {
        flush();
        col += tabSize_ - col % tabSize_;
        ++i;
        continue;
      }
      if (c == ' ') {
        flush();
        ++col;
        ++i;
        continue;
      }
      if (monospaced_ && c > 0x20 && c < 0x7F) {
        if (col + 1 < firstColumn) {
          ++col;
          ++i;
          continue;
        }
        if (!run.empty() && !sameColour(colour, runColour)) flush();
        if (run.empty()) {
          runColumn = col;
          runColour = colour;
        }
        run.push_back((char) c);
        ++col;
        ++i;
        continue;
      }
      flush();
      size_t end = i + 1;
      while (end < s.size() && !detail::code::isStop(s, end)) ++end;
      const int width = detail::code::isWide(c) ? 2 : 1;
      if (col + width >= firstColumn) {
        const float x = originX + (float) col * cw;
        g.setColour(colour);
        if (detail::code::isControl(c)) {
          g.drawRect(Rect(x + cw * 0.15f, baseline - ascent_ * 0.7f, cw * 0.7f, ascent_ * 0.7f), 1.0f);
        } else if (c != 0xFEFF && c <= 0x10FFFF) {
          f.draw(g, detail::code::encode(s.data() + i, end - i), x, baseline);
        }
      }
      col += width;
      i = end;
    }
    flush();
  }

  CodeDocument& document_;
  CodeTokeniser* tokeniser_ = nullptr;
  CodeDocument::Position caret_, anchor_;
  ColourScheme scheme_;
  Colours colours_;
  ScrollBar vertical_{true};
  ScrollBar horizontal_{false};
  float scrollX_ = 0.0f, scrollY_ = 0.0f;
  int tabSize_ = 4;
  bool spacesForTabs_ = true;
  bool showLineNumbers_ = true;
  bool readOnly_ = false;
  bool autoIndent_ = true;
  bool syncingBars_ = false;
  int desiredColumn_ = -1;
  Action lastAction_ = Action::None;
  int lastChange_ = -1;
  std::string lastSearch_;
  bool lastSearchCaseSensitive_ = true;
  DragMode drag_ = DragMode::None;
  int dragLine_ = 0, dragFromLine_ = 0, dragFromIndex_ = 0, dragToLine_ = 0, dragToIndex_ = 0;
  int editDepth_ = 0;                                         // EditScopes open
  bool textChangePending_ = false, caretMovePending_ = false; // what they will fire

  std::vector<uint32_t> states_; // the tokeniser's state at each line's start
  int statesValid_ = 1;          // lines from the top whose state is known
  int reuseFrom_ = -1, reuseTo_ = -1; // states kept from before an edit, if a re-scan agrees with them
  std::vector<CodeTokeniser::Token> tokens_;

  mutable bool metricsValid_ = false;
  mutable const Typeface* metricsFace_ = nullptr;
  mutable float metricsHeight_ = 0.0f;
  mutable float charWidth_ = 8.0f, lineHeight_ = 16.0f, ascent_ = 12.0f, descent_ = 4.0f;
  mutable bool monospaced_ = false;
};

} // namespace gfx
} // namespace sonore
