// SPDX-License-Identifier: Apache-2.0
//
// Font: a typeface at a size, and drawing text with it.
//
// ── Where the design units go ───────────────────────────────────────────────
//
// A typeface stores everything in its own grid -- 2048 units per em for most
// fonts, 1000 for some. A Font is that grid divided by unitsPerEm and
// multiplied by a point size, so every measurement out of this class is in
// PIXELS and no caller ever handles a design unit.
//
// That conversion happening in one place is the whole reason this class
// exists. Done at each call site it is a multiply somebody will forget, and
// the symptom is text that is right in one panel and 2048 times too big in
// another.
//
// ── Glyphs are cached as PATHS, not as bitmaps ──────────────────────────────
//
// The usual design caches a rendered bitmap per glyph per size. This caches
// the outline, and rasterises it with the same code that draws everything
// else.
//
// It is slower per draw and it is the right trade here: a plugin's editor
// redraws a few hundred short strings, not a document; the cache is
// size-independent so a resizable window does not throw it away; and there is
// no second rasteriser whose antialiasing differs from the first. One quality,
// one set of tests.
//
// ── Text that is not a glyph per character ──────────────────────────────────
//
// A string with right-to-left letters, combining marks or joining scripts
// goes through the bidirectional algorithm (bidi.h) and OpenType shaping
// (text_shaper.h): its runs ordered as they are read, each shaped with the
// face that has its glyphs. Everything else -- most of what an interface
// says -- is drawn a glyph per character as it always was, the same pixels.
#pragma once

#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "bidi.h"
#include "graphics.h"
#include "line_break.h"
#include "png.h"
#include "system_font.h"
#include "text_shaper.h"
#include "truetype.h"

namespace sonore {
namespace gfx {

/** Where a line goes across the width it is given (Font::drawIn, drawWrapped). */
enum class Justify { Left, /**< against the left edge */ Centred, /**< centred */ Right /**< against the right edge */ };

/**
 * Where each character of one line of text is drawn, for a caret and a
 * selection: Font::layoutCharacters makes one.
 *
 * A caret stands BETWEEN characters, and in shaped text a character does not
 * always have a place of its own: a ligature, a conjunct, a letter with its
 * marks are one glyph or a stack of them, and a right-to-left word reads
 * from the right. Each such cluster is shared out among its characters, left
 * to right or right to left as it reads, and only its first character is a
 * stop -- a caret inside a conjunct has nowhere to be drawn. A right-to-left
 * character has its caret on its right.
 */
struct CharacterLayout {
  std::vector<float> left, /**< each character's left edge, pixels from the line's start */ right; ///< its right edge
  std::vector<uint8_t> rtl;       ///< 1 where the character reads right to left
  std::vector<uint8_t> stop;      ///< 1 where a caret may stand before it
  float width = 0.0f;  ///< the whole line's width, as stringWidth measures it

  /** How many characters (code points) the line has. */
  size_t size() const { return left.size(); }

  /** The caret's x before character i -- after the last, for i == size(). */
  float caretX(size_t i) const {
    const size_t n = size();
    if (n == 0) return 0.0f;
    if (i >= n) return rtl[n - 1] ? left[n - 1] : right[n - 1];
    return rtl[i] ? right[i] : left[i];
  }

  /** The stop whose caret is nearest x. */
  size_t indexAt(float x) const {
    size_t best = 0;
    float bestDistance = 1e30f;
    for (size_t i = 0; i <= size(); ++i) {
      if (i < size() && !stop[i]) continue;
      const float d = std::fabs(caretX(i) - x);
      if (d < bestDistance) {
        bestDistance = d;
        best = i;
      }
    }
    return best;
  }

  /** The next stop after character i, or size(). */
  size_t nextStop(size_t i) const {
    while (++i < size() && !stop[i]) {
    }
    return i < size() ? i : size();
  }

  /** The stop before i, or 0. */
  size_t previousStop(size_t i) const {
    if (i > size()) i = size();
    while (i > 0 && !stop[--i]) {
    }
    return i;
  }
};

/**
 * A typeface at a size: what text is measured and drawn with.
 *
 * The reference's Font, with drawing and wrapping on it rather than on
 * Graphics. The height is the em in pixels (logical units, like everything in
 * the component tree), and every measurement out of it is in pixels too -- see
 * the file comment. A default-constructed Font, or one with no usable face,
 * is not valid: it measures 0 and draws nothing.
 *
 * Copying is cheap and SHARES the caches -- outlines, flattened glyphs, shaped
 * strings, small-text coverage, colour-glyph pictures -- but withHeight,
 * withFeature, withVariation and withoutFallback each make a Font with caches
 * of its own. So make the Font once (in a constructor, or when the size
 * changes) and keep it, rather than building one in paint().
 *
 * Any thread may measure and draw with a Font, and several at once with one
 * Font or its copies: a Graphics draws on whatever thread paints, a host may
 * run two editors on two threads, and an image may be rendered on a worker.
 * The caches are filled, and emptied when full, as text is drawn, so a Font
 * and its copies share one lock: taken once a call, not once a glyph, by
 * draw and appendString, and by measuring only where it shapes or reads a
 * variable font's advances -- a plain label is measured without it. One
 * thread pays an uncontended lock a label. Threads drawing with one Font at
 * the same moment take turns; with a Font each (withHeight makes one) they
 * do not.
 */
class Font {
public:
  /** Not valid: measures 0 and draws nothing. */
  Font() = default;
  /** `face` at an em of `pointHeight` logical pixels -- pixels, whatever the
   *  name says. The typeface is shared, not copied. Not valid when `face` is
   *  null or unusable or the height is not above 0. */
  Font(std::shared_ptr<Typeface> face, float pointHeight)
      : face_(std::move(face)), height_(pointHeight) {}

  /** Whether there is a usable face and a height above 0. */
  bool isValid() const { return face_ && face_->isValid() && height_ > 0.0f; }
  /** The em, in logical pixels. */
  float height() const { return height_; }
  /** The face this font draws from; may be null. */
  const std::shared_ptr<Typeface>& typeface() const { return face_; }

  /** The same face, features, variation and fallback setting at another em
   *  height, with caches of its own. */
  Font withHeight(float newHeight) const {
    Font f(face_, newHeight);
    f.fallback_ = fallback_;
    f.settings_ = settings_;
    f.location_ = location_;
    f.features_ = features_;
    return f;
  }

  /**
   * The same font with an OpenType feature on or off, for all of its text:
   * withFeature("tnum") gives tabular figures -- every digit one width, so a
   * readout does not jitter as its value changes -- withFeature("liga", 0)
   * no ligatures, "smcp" small capitals, "zero" a slashed zero, "ss01" to
   * "ss20" a stylistic set; a value above 1 picks an alternate, for "salt"
   * or "aalt". A feature keeps the value it was last given; one the
   * typeface lacks changes nothing. A font with features shapes all its
   * text, ASCII included.
   */
  Font withFeature(const char* feature, uint32_t value = 1) const {
    Font f = withHeight(height_);
    const uint32_t tag = Typeface::tagOf(feature);
    bool found = false;
    for (auto& setting : f.features_)
      if (setting.first == tag) {
        setting.second = value;
        found = true;
      }
    if (!found) f.features_.push_back({tag, value});
    return f;
  }

  /** The features given by withFeature, in the order first given. */
  const std::vector<std::pair<uint32_t, uint32_t>>& features() const { return features_; }

  /**
   * The same font elsewhere in a variable typeface's design space:
   * withVariation("wght", 650), then withVariation("wdth", 85) on that. An
   * axis keeps the value it was last given; a value past the axis's range is
   * held at its end; an axis the typeface lacks, or a typeface that is not
   * variable, changes nothing. Kerning is the default design's.
   *
   * Each variation has its own glyph cache, as each size has its own
   * flattening: make the Font once and keep it, as for a size.
   */
  Font withVariation(const char* axis, float value) const {
    Font f = withHeight(height_);
    const uint32_t tag = Typeface::tagOf(axis);
    bool found = false;
    for (auto& setting : f.settings_)
      if (setting.first == tag) {
        setting.second = value;
        found = true;
      }
    if (!found) f.settings_.push_back({tag, value});
    f.location_.reset();
    if (face_ && face_->isVariable()) {
      std::vector<int16_t> at = face_->locationFor(f.settings_);
      if (!at.empty()) f.location_ = std::make_shared<const std::vector<int16_t>>(std::move(at));
    }
    return f;
  }

  /** The axis values given by withVariation, in the order first given. */
  const std::vector<std::pair<uint32_t, float>>& variations() const { return settings_; }

  /** The same font, drawing a character its typeface lacks as the empty
   *  .notdef box rather than from the system's fallback faces -- for a
   *  plugin that ships its own face and would rather show the box than
   *  another design. Falling back is the default. */
  Font withoutFallback() const {
    Font f = withHeight(height_);
    f.fallback_ = false;
    return f;
  }
  /** False after withoutFallback; true by default. */
  bool fallsBack() const { return fallback_; }

  /** Design units to pixels. */
  float scale() const {
    if (!isValid()) return 0.0f;
    return height_ / (float) face_->unitsPerEm();
  }

  /** Baseline up to the typeface's ascender line, in pixels; 0 when not valid. */
  float ascent() const { return isValid() ? (float) face_->ascender() * scale() : 0.0f; }
  /** Baseline down to the typeface's descender line, in pixels and POSITIVE
   *  (the typeface stores it negative); 0 when not valid. */
  float descent() const { return isValid() ? -(float) face_->descender() * scale() : 0.0f; }

  /** Baseline to baseline. Includes the font's own line gap, because a
   *  designer chose it and stacking lines at ascent+descent crowds them. */
  float lineHeight() const {
    if (!isValid()) return 0.0f;
    return (float) (face_->ascender() - face_->descender() + face_->lineGap()) * scale();
  }

  /**
   * How wide a string draws, in pixels.
   *
   * Includes kerning, so measuring and drawing agree. They have to: a caller
   * that centres text by measuring it and then draws it with different
   * spacing gets text that is off-centre by the kerning, which looks like a
   * layout bug rather than a text one.
   */
  float stringWidth(const std::string& utf8) const {
    if (!isValid()) return 0.0f;
    float pen = 0.0f;
    // Plain text at the default design -- most labels -- is measured from
    // the typeface alone, by the walk forEachGlyph would take: no cache, so
    // no lock.
    if (!location_ && features_.empty() && !needsShaping(utf8)) {
      walkUnshaped(utf8, 0.0f, [](const Typeface*, uint16_t, float, float, float) {}, &pen);
      return pen;
    }
    const std::lock_guard<std::mutex> hold(*cacheLock_);
    forEachGlyph(utf8, 0.0f, [](const Typeface&, uint16_t, float, float, float) {}, &pen);
    return pen;
  }

  /**
   * Where each character of a single line is drawn, from x = 0 as draw()
   * draws it: see CharacterLayout. The same walk drawing and measuring make,
   * so a caret and a selection land on the glyphs, in shaped and
   * right-to-left text as in the rest.
   */
  CharacterLayout layoutCharacters(const std::string& utf8) const {
    CharacterLayout out;
    if (!isValid()) return out;
    if (features_.empty() && !needsShaping(utf8)) {
      auto walk = [&]() {
        walkUnshaped(
            utf8, 0.0f,
            [&](const Typeface*, uint16_t, float at, float, float after) {
              out.left.push_back(at);
              out.right.push_back(after);
            },
            &out.width);
      };
      // A variable font's advances are the only cache this walk reaches.
      if (location_) {
        const std::lock_guard<std::mutex> hold(*cacheLock_);
        walk();
      } else {
        walk();
      }
      out.rtl.assign(out.left.size(), 0);
      out.stop.assign(out.left.size(), 1);
      return out;
    }
    std::vector<uint32_t> text;
    for (size_t i = 0; i < utf8.size();) text.push_back(nextCodepoint(utf8, i));
    const size_t n = text.size();
    // Each cluster's extent, by the character it begins with. One a
    // character not drawn begins -- ZWNJ, an embedding control -- has none,
    // and goes with the cluster before it: no place for a caret of its own.
    struct Extent {
      float lo, hi;
      bool rtl, seen;
    };
    std::vector<Extent> extents(n, Extent{0.0f, 0.0f, false, false});
    float pen = 0.0f;
    // Held while it is walked: a line the cache does not keep -- a long
    // one -- or one the cache is emptied of meanwhile, by a draw on another
    // thread, is freed with the last pointer to it. So the lock is for the
    // lookup alone.
    std::shared_ptr<const ShapedLine> line;
    {
      const std::lock_guard<std::mutex> hold(*cacheLock_);
      line = shapedLine(utf8);
    }
    for (const ShapedLine::Glyph& g : line->glyphs) {
      const float advance = (float) g.xAdvance * (height_ / (float) g.face->unitsPerEm());
      if (g.cluster < n && !detail::shaping::isIgnorable(text[g.cluster])) {
        Extent& e = extents[g.cluster];
        const float lo = (std::min)(pen, pen + advance), hi = (std::max)(pen, pen + advance);
        if (!e.seen) e = Extent{lo, hi, g.rtl, true};
        else {
          e.lo = (std::min)(e.lo, lo);
          e.hi = (std::max)(e.hi, hi);
        }
      }
      pen += advance;
    }
    out.width = pen;
    out.left.assign(n, 0.0f);
    out.right.assign(n, 0.0f);
    out.rtl.assign(n, 0);
    out.stop.assign(n, 0);
    size_t first = 0;
    while (first < n && !extents[first].seen) ++first;
    if (first == n) {
      out.stop.assign(n, 1);
      return out;
    }
    // Characters before the first cluster -- controls the bidirectional
    // algorithm took out -- stand at its leading edge.
    for (size_t k = 0; k < first; ++k) {
      const Extent& e = extents[first];
      out.left[k] = out.right[k] = e.rtl ? e.hi : e.lo;
      out.rtl[k] = e.rtl;
      out.stop[k] = k == 0;
    }
    for (size_t c = first; c < n;) {
      size_t end = c + 1;
      while (end < n && !extents[end].seen) ++end;
      const Extent& e = extents[c];
      const float share = (e.hi - e.lo) / (float) (end - c);
      for (size_t k = c; k < end; ++k) {
        const float step = (float) (k - c);
        if (e.rtl) {
          out.right[k] = e.hi - share * step;
          out.left[k] = e.hi - share * (step + 1.0f);
        } else {
          out.left[k] = e.lo + share * step;
          out.right[k] = e.lo + share * (step + 1.0f);
        }
        out.rtl[k] = e.rtl;
        out.stop[k] = k == c;
      }
      c = end;
    }
    return out;
  }

  /**
   * Draw a string with its BASELINE at `y`.
   *
   * Baseline rather than top, because that is what text sits on: two runs at
   * different sizes on the same baseline line up, and the same two aligned by
   * their tops do not.
   */
  void draw(Graphics& g, const std::string& utf8, float x, float baselineY) const {
    if (!isValid() || utf8.empty()) return;
    // Held for the whole draw: an entry found in one cache is read while the
    // next is filled, and a cache emptied under it was freed memory.
    const std::lock_guard<std::mutex> hold(*cacheLock_);
    // Moved and scaled evenly, as an editor's text always is, and no more
    // than kCoverageEm to the em on the device: from each glyph's coverage,
    // rasterised once (glyphCoverage). Rotated or skewed, or big -- a zoom, a
    // giant readout, mostly off the clip -- the outline is filled, which
    // rasterises only what the clip shows.
    const Transform& t = g.state().transform;
    if (t.b == 0.0f && t.c == 0.0f && t.a > 0.0f && t.a == t.d && height_ * t.a <= kCoverageEm) {
      drawFromCoverage(g, utf8, x, baselineY, t);
      return;
    }
    Path path;
    forEachGlyph(utf8, x, [&](const Typeface& face, uint16_t glyph, float pen, float s, float dy) {
      if (face.hasColourGlyphs() && drawColourGlyph(g, path, face, glyph, pen, baselineY + dy, s)) return;
      appendCachedGlyph(path, face, glyph, pen, baselineY + dy, s);
    });
    g.fillPath(path, FillRule::NonZero);
  }

  /** Inside a rectangle, justified, vertically centred by its ascent and
   *  descent rather than by the bounding box of the glyphs actually used --
   *  or "Type" and "type" would sit at different heights. */
  void drawIn(Graphics& g, const std::string& utf8, const Rect& area,
              Justify justify = Justify::Centred) const {
    if (!isValid() || utf8.empty()) return;
    const float w = stringWidth(utf8);
    float x = area.x;
    if (justify == Justify::Centred) x = area.x + (area.w - w) * 0.5f;
    else if (justify == Justify::Right) x = area.right() - w;
    const float baseline = area.y + (area.h - (ascent() + descent())) * 0.5f + ascent();
    draw(g, utf8, x, baseline);
  }

  // ── More than one line ───────────────────────────────────────────────────

  /**
   * Break `utf8` into lines that each fit within `maxWidth`.
   *
   * Where Unicode's line breaking algorithm allows (findLineBreaks): after a
   * space or a hyphen, between two ideographs, never before a closing
   * bracket or inside "$12.50". Explicit newlines are honoured rather than
   * treated as spaces -- a plugin's description has paragraphs in it, and a
   * layout that ran them together would be wrong in a way nobody could fix
   * from the outside. Spaces at the end of a line hang past it: they are not
   * measured and not kept.
   *
   * A WORD longer than the line is broken mid-word rather than allowed to
   * overflow, at the boundaries of its clusters -- never between a letter
   * and its marks, or inside a conjunct. That is ugly and it is the right
   * answer: the alternative is a file path or a preset name running out of
   * its panel and over the control beside it.
   */
  std::vector<std::string> wrapText(const std::string& utf8, float maxWidth) const {
    std::vector<std::string> lines;
    std::vector<size_t> at; // each character's first byte, and the end
    for (size_t i = 0; i < utf8.size();) {
      at.push_back(i);
      nextCodepoint(utf8, i);
    }
    at.push_back(utf8.size());
    for (const WrappedLine& line : wrapLines(utf8, maxWidth))
      lines.push_back(utf8.substr(at[line.begin], at[line.end] - at[line.begin]));
    return lines;
  }

  /** A line of wrapped text, in characters (code points): where it begins,
   *  where what it draws ends -- before the spaces and the line break it
   *  ends with -- and where the next line begins. */
  struct WrappedLine {
    size_t begin = 0, /**< its first character */ end = 0, /**< one past its last drawn */ next = 0; ///< next line's first
  };

  /**
   * Where wrapText breaks `utf8`, as character positions rather than copies:
   * what a multi-line field (TextArea) needs to put a caret on a line. The
   * text whole on one line where the font is not valid or there is no width.
   */
  std::vector<WrappedLine> wrapLines(const std::string& utf8, float maxWidth) const {
    std::vector<WrappedLine> lines;
    if (utf8.empty()) return lines;
    std::vector<uint32_t> text;
    std::vector<size_t> at; // each character's first byte, and the end
    for (size_t i = 0; i < utf8.size();) {
      at.push_back(i);
      text.push_back(nextCodepoint(utf8, i));
    }
    at.push_back(utf8.size());
    const size_t n = text.size();
    if (!isValid() || maxWidth <= 0.0f) {
      lines.push_back({0, n, n});
      return lines;
    }
    const std::vector<LineBreak> breaks = findLineBreaks(text.data(), n);
    auto piece = [&](size_t from, size_t to) { return utf8.substr(at[from], at[to] - at[from]); };
    // A line ends before its trailing spaces and hard breaks.
    auto trimmed = [&](size_t from, size_t to) {
      while (to > from) {
        const uint32_t c = text[to - 1];
        if (c != 0x20 && c != 0x0A && c != 0x0B && c != 0x0C && c != 0x0D && c != 0x85 && c != 0x2028 &&
            c != 0x2029)
          break;
        --to;
      }
      return to;
    };
    // Measured from one layout, not each line's every prefix shaped again:
    // that was quadratic, and a long run with nowhere to break -- Thai, a
    // hash, a path -- took a second. Plain text is laid out left to right
    // with nothing shaped, so a run's width is where it ends less where it
    // begins, as drawn. Shaped text is estimated by its characters' shares
    // of the layout, and measured as drawn -- shaped alone, where a line's
    // edges may join or kern otherwise -- only where the estimate is within
    // a tenth of the line's width of it: a run or two a line.
    const CharacterLayout whole = layoutCharacters(utf8);
    const bool plain = features_.empty() && !needsShaping(utf8) && whole.size() == n;
    const float slack = (std::max)(2.0f, maxWidth * 0.1f);
    struct Measure {
      const CharacterLayout* layout;
      size_t from;
      std::vector<float> shares;
    };
    auto measureOf = [&](const CharacterLayout& layout, size_t from) {
      Measure m{&layout, from, std::vector<float>(layout.size() + 1, 0.0f)};
      for (size_t i = 0; i < layout.size(); ++i) m.shares[i + 1] = m.shares[i] + layout.right[i] - layout.left[i];
      return m;
    };
    auto estimate = [&](const Measure& m, size_t from, size_t to) {
      if (to <= from) return 0.0f;
      const size_t a = (std::min)(from - m.from, m.layout->size()), b = (std::min)(to - m.from, m.layout->size());
      if (plain && b > a) return m.layout->right[b - 1] - m.layout->left[a];
      return m.shares[b] - m.shares[a];
    };
    auto fits = [&](const Measure& m, size_t from, size_t to) {
      const float e = estimate(m, from, to);
      if (plain || e <= maxWidth - slack) return e <= maxWidth;
      if (e > maxWidth + slack) return false;
      return stringWidth(piece(from, to)) <= maxWidth;
    };
    const Measure all = measureOf(whole, 0);

    size_t start = 0;
    std::vector<size_t> fitting; // the breaks the line so far may end at, in order
    // The line ends at the last of them that fits as drawn -- whatever the
    // estimate said -- or false where none does. The text after it is read
    // again for the next line.
    auto endLine = [&]() {
      while (!fitting.empty()) {
        const size_t b = fitting.back();
        const size_t end = trimmed(start, b);
        if (plain || stringWidth(piece(start, end)) <= maxWidth) {
          lines.push_back({start, end, b});
          start = b;
          fitting.clear();
          return true;
        }
        fitting.pop_back();
      }
      return false;
    };
    // A word too long for its line, laid out alone once for every line it
    // takes: its clusters are where it may be broken.
    CharacterLayout word;
    Measure wordMeasure{&word, 0, {}};
    size_t wordFrom = 0, wordTo = 0;
    for (size_t b = 1; b <= n; ++b) {
      if (breaks[b] == LineBreak::None) continue;
      const size_t end = trimmed(start, b);
      if (fits(all, start, end)) {
        fitting.push_back(b);
        if (breaks[b] != LineBreak::Mandatory) continue;
        if (endLine()) {
          b = start;
          continue;
        }
      } else if (endLine()) {
        b = start;
        continue;
      }
      // Not even this word fits: as many of its clusters as do, at least one.
      if (!plain && (start < wordFrom || start >= wordTo || end != wordTo)) {
        word = layoutCharacters(piece(start, end));
        wordFrom = start;
        wordTo = end;
        wordMeasure = measureOf(word, wordFrom);
      }
      const Measure& m = plain ? all : wordMeasure;
      auto isStop = [&](size_t k) {
        const size_t at = k - m.from;
        return at >= m.layout->size() || m.layout->stop[at] != 0;
      };
      size_t take = 0, first = 0;
      for (size_t k = start + 1; k <= end; ++k) {
        if (k < end && !isStop(k)) continue;
        if (first == 0) first = k - start;
        if (!fits(m, start, k)) break;
        take = k - start;
      }
      if (take == 0) take = first > 0 ? first : 1;
      lines.push_back({start, start + take, start + take});
      start += take;
      fitting.clear();
      b = start;
    }
    return lines;
  }

  /** How tall a wrapped block would be. What a caller sizes a panel from. */
  float wrappedHeight(const std::string& utf8, float maxWidth) const {
    const std::vector<std::string> lines = wrapText(utf8, maxWidth);
    return (float) lines.size() * lineHeight();
  }

  // lineHeight() is defined above and uses the FONT's own line gap, which a
  // designer chose. The version written here first was ascent+descent times
  // 1.25 -- a guess at what the designer already decided, and worse for it.

  /**
   * Draw wrapped text inside a rectangle.
   *
   * Lines past the bottom are DROPPED rather than drawn outside, because a
   * paragraph that overflowed its panel would paint over whatever is under it.
   */
  void drawWrapped(Graphics& g, const std::string& utf8, const Rect& area,
                   Justify justify = Justify::Left) const {
    if (!isValid() || utf8.empty() || area.isEmpty()) return;
    const std::vector<std::string> lines = wrapText(utf8, area.w);
    const float step = lineHeight();
    float baseline = area.y + ascent();
    for (const std::string& line : lines) {
      if (baseline - ascent() >= area.bottom()) break;
      float x = area.x;
      if (justify != Justify::Left) {
        const float w = stringWidth(line);
        if (justify == Justify::Centred) x = area.x + (area.w - w) * 0.5f;
        else x = area.right() - w;
      }
      draw(g, line, x, baseline);
      baseline += step;
    }
  }

  /** The outline of a string, for a caller that wants to stroke it, clip to
   *  it, or transform it as one shape. */
  void appendString(Path& out, const std::string& utf8, float x, float baselineY) const {
    if (!isValid()) return;
    const std::lock_guard<std::mutex> hold(*cacheLock_);
    forEachGlyph(utf8, x, [&](const Typeface& face, uint16_t glyph, float pen, float s, float dy) {
      appendCachedGlyph(out, face, glyph, pen, baselineY + dy, s);
    });
  }

  /** A code point that draws nothing and takes no room: the joiners and
   *  marks that shape text (U+200B..U+200F, U+2028..U+202E, U+2060..U+206F),
   *  the variation selectors, the byte-order mark, the soft hyphen, the tag
   *  characters. No face has a visible glyph for one, so looked up they drew
   *  a .notdef box in the middle of a word -- an emoji with a skin tone, a
   *  string pasted from a web page. */
  static bool isInvisible(uint32_t cp) {
    return cp == 0xAD || (cp >= 0x200B && cp <= 0x200F) || (cp >= 0x2028 && cp <= 0x202E) ||
           (cp >= 0x2060 && cp <= 0x206F) || (cp >= 0xFE00 && cp <= 0xFE0F) || cp == 0xFEFF ||
           (cp >= 0xE0000 && cp <= 0xE0FFF);
  }

  /**
   * Decode one UTF-8 code point, advancing `i`.
   *
   * Malformed input yields U+FFFD and advances by one byte, so a broken
   * string draws replacement characters and ends. Advancing by zero on bad
   * input -- the obvious mistake -- hangs the UI thread for ever on a string
   * a user pasted.
   */
  static uint32_t nextCodepoint(const std::string& s, size_t& i) {
    const uint8_t c = (uint8_t) s[i];
    auto cont = [&](size_t k) {
      return i + k < s.size() && ((uint8_t) s[i + k] & 0xC0) == 0x80;
    };
    if (c < 0x80) {
      ++i;
      return c;
    }
    if ((c & 0xE0) == 0xC0 && cont(1)) {
      const uint32_t v = ((uint32_t) (c & 0x1F) << 6) | ((uint8_t) s[i + 1] & 0x3F);
      i += 2;
      return v;
    }
    if ((c & 0xF0) == 0xE0 && cont(1) && cont(2)) {
      const uint32_t v = ((uint32_t) (c & 0x0F) << 12) |
                         ((uint32_t) ((uint8_t) s[i + 1] & 0x3F) << 6) |
                         ((uint8_t) s[i + 2] & 0x3F);
      i += 3;
      return v;
    }
    if ((c & 0xF8) == 0xF0 && cont(1) && cont(2) && cont(3)) {
      const uint32_t v = ((uint32_t) (c & 0x07) << 18) |
                         ((uint32_t) ((uint8_t) s[i + 1] & 0x3F) << 12) |
                         ((uint32_t) ((uint8_t) s[i + 2] & 0x3F) << 6) |
                         ((uint8_t) s[i + 3] & 0x3F);
      i += 4;
      return v;
    }
    ++i;
    return 0xFFFD;
  }

private:
  /**
   * Every character's face, glyph and pen position, left to right -- the ONE
   * walk both measuring and drawing take, so the two agree to the pixel
   * whichever faces a string ends up in.
   *
   * A character the typeface has no glyph for is looked up in the system's
   * fallback faces (fallbackTypeface), at the same em size; an invisible one
   * is skipped. Kerning applies between two glyphs of the same face only:
   * a kerning table knows nothing of another font's glyph ids.
   */
  template <typename Visit>
  void forEachGlyph(const std::string& utf8, float x, Visit visit, float* end = nullptr) const {
    if (!features_.empty() || needsShaping(utf8)) {
      forEachShapedGlyph(utf8, x, visit, end);
      return;
    }
    walkUnshaped(
        utf8, x,
        [&](const Typeface* face, uint16_t glyph, float at, float s, float) {
          if (face) visit(*face, glyph, at, s, 0.0f);
        },
        end);
  }

private:
  /** The walk over text that needs no shaping, a glyph per character:
   *  `each` gets every character's face -- null for an invisible one --
   *  glyph, the pen it is drawn at, its scale, and the pen after it. */
  template <typename Each>
  void walkUnshaped(const std::string& utf8, float x, Each each, float* end) const {
    const float s = scale();
    float pen = x;
    const Typeface* previousFace = nullptr;
    uint16_t previous = 0;
    size_t i = 0;
    while (i < utf8.size()) {
      const uint32_t cp = nextCodepoint(utf8, i);
      if (isInvisible(cp)) {
        each(nullptr, (uint16_t) 0, pen, 0.0f, pen);
        continue;
      }
      const Typeface* face = face_.get();
      float faceScale = s;
      uint16_t glyph = face->glyphForChar(cp);
      if (glyph == 0 && fallback_ && cp >= 0x20 && !(cp >= 0x7F && cp < 0xA0)) {
        if (const std::shared_ptr<Typeface> other = fallbackTypeface(cp)) {
          face = other.get();
          glyph = face->glyphForChar(cp);
          faceScale = height_ / (float) face->unitsPerEm();
        }
      }
      if (previous && face == previousFace) pen += (float) face->kerning(previous, glyph) * faceScale;
      const float at = pen;
      // The typeface's own glyphs move with the variation; a fallback face's
      // are its default design.
      if (location_ && face == face_.get()) pen += variedAdvance(glyph) * faceScale;
      else pen += (float) face->advanceWidth(glyph) * faceScale;
      each(face, glyph, at, faceScale, pen);
      previousFace = face;
      previous = glyph;
    }
    if (end) *end = pen;
  }

public:
  /**
   * Whether a string needs the bidirectional algorithm and shaping: a
   * right-to-left letter or digit, a combining mark, a joiner, a bidi
   * control, Thai or Lao SARA AM, a Hangul jamo, Malayalam's dot reph, a
   * regional indicator, a skin tone or a tag -- the last three hold emoji
   * sequences together -- or an emoji, which comes from the colour face.
   * (Indic, Khmer and Myanmar syllables that reorder all hold a mark.) Bytes
   * below 0x80 are looked at as bytes, so an ASCII label --
   * most of them -- costs one pass over its bytes and nothing else.
   */
  static bool needsShaping(const std::string& utf8) {
    size_t i = 0;
    while (i < utf8.size()) {
      if ((unsigned char) utf8[i] < 0x80) {
        ++i;
        continue;
      }
      const uint32_t cp = nextCodepoint(utf8, i);
      if (cp < 0x0300) continue;
      if (cp == 0x200C || cp == 0x200D || detail::shaping::isMark(cp)) return true;
      // SARA AM splits in two, and jamo compose into syllables.
      if ((cp & ~0x80u) == 0x0E33u || (cp >= 0x1100 && cp <= 0x11FF) || (cp >= 0xA960 && cp <= 0xA97F) ||
          (cp >= 0xD7B0 && cp <= 0xD7FF) || cp == 0x0D4E)
        return true;
      if (detail::shaping::isRegionalIndicator(cp) || (cp >= 0x1F3FB && cp <= 0x1F3FF) ||
          (cp >= 0xE0020 && cp <= 0xE007F) || cp == 0xFF9E || cp == 0xFF9F)
        return true;
      // An emoji by default goes to the colour emoji face, which shaping
      // chooses.
      if (detail::shaping::isEmojiPresentation(cp)) return true;
      const detail::bidi::Class c = detail::bidi::classOf(cp);
      if (c == detail::bidi::R || c == detail::bidi::AL || c == detail::bidi::AN ||
          (c >= detail::bidi::LRE && c <= detail::bidi::PDI))
        return true;
    }
    return false;
  }

private:
  /** A string shaped: its glyphs in the order they are drawn, each with the
   *  face it is drawn from, in that face's design units. */
  struct ShapedLine {
    /** One glyph as shaping placed it: the face it comes from (a fallback
     *  face's, as often as not), its id there, and its advance and offsets in
     *  that face's design units -- scaled to pixels when it is drawn. */
    struct Glyph {
      const Typeface* face;
      uint16_t glyph;
      int32_t xAdvance, xOffset, yOffset;
      uint32_t cluster; // the first character it was shaped from
      bool rtl;         // from a right-to-left run
    };
    std::vector<Glyph> glyphs;
  };

  template <typename Visit>
  void forEachShapedGlyph(const std::string& utf8, float x, Visit visit, float* end) const {
    const std::shared_ptr<const ShapedLine> line = shapedLine(utf8);
    float pen = x;
    for (const ShapedLine::Glyph& g : line->glyphs) {
      const float s = height_ / (float) g.face->unitsPerEm();
      // y up in the font, down on the screen.
      visit(*g.face, g.glyph, pen + (float) g.xOffset * s, s, -(float) g.yOffset * s);
      pen += (float) g.xAdvance * s;
    }
    if (end) *end = pen;
  }

  /**
   * The bidirectional algorithm over the string as one paragraph, the
   * result cut into runs -- one level, one face, one script each -- every
   * run shaped, and the runs put in the order they are shown. Kept: a label
   * is drawn thirty times a second and shaped once.
   */
  std::shared_ptr<const ShapedLine> shapedLine(const std::string& utf8) const {
    auto hit = shaped_->find(utf8);
    if (hit != shaped_->end()) return hit->second;
    auto line = std::make_shared<ShapedLine>();

    std::vector<uint32_t> text;
    for (size_t i = 0; i < utf8.size();) text.push_back(nextCodepoint(utf8, i));
    const size_t n = text.size();
    const BidiParagraph paragraph(text.data(), n, BidiDirection::Auto);
    const std::vector<uint8_t> levels = paragraph.lineLevels(0, n);

    // Each character's face: the font's own where it has the glyph, and
    // anything else the font lacks the system's fallback, as unshaped text
    // has it. A character that continues a cluster -- a mark, a selector, a
    // joiner and what it joins, a skin tone, a flag's second letter -- or
    // that is not drawn stays with the face of the one before it where that
    // face has it, so one font shapes a sequence and can ligate it; and
    // where no face it could go to has it.
    std::vector<const Typeface*> faces(n, face_.get());
    std::vector<std::shared_ptr<Typeface>> keep;
    const std::vector<uint8_t> continues = detail::shaping::clusterContinuations(text.data(), n);
    for (size_t i = 0; i < n; ++i) {
      const uint32_t cp = text[i];
      const bool follows = i > 0 && (continues[i] || detail::shaping::isIgnorable(cp));
      if (follows && (faces[i - 1]->glyphForChar(cp) || detail::shaping::isIgnorable(cp))) {
        faces[i] = faces[i - 1];
        continue;
      }
      // An emoji asked for as one -- by default, or by VS16 -- comes from
      // the colour emoji face, the label's own face's glyph for it being the
      // plain text one; a browser chooses so.
      if (!follows && fallback_ && detail::shaping::wantsEmojiPresentation(text.data(), n, i))
        if (const std::shared_ptr<Typeface> emoji = emojiTypeface())
          if (emoji->glyphForChar(cp)) {
            keep.push_back(emoji);
            faces[i] = emoji.get();
            continue;
          }
      if (face_->glyphForChar(cp)) continue;
      if (follows) {
        faces[i] = faces[i - 1];
        continue;
      }
      if (fallback_ && cp >= 0x20 && !(cp >= 0x7F && cp < 0xA0))
        if (const std::shared_ptr<Typeface> other = fallbackTypeface(cp)) {
          keep.push_back(other);
          faces[i] = other.get();
        }
    }

    // Each character's script, the neutral ones taking their neighbours'.
    std::vector<int> scripts(n, 0);
    int last = 0;
    for (size_t i = 0; i < n; ++i) {
      const int sc = detail::shaping::scriptOf(text[i]);
      if (!detail::shaping::isNeutralScript(sc)) last = sc;
      scripts[i] = detail::shaping::isNeutralScript(sc) ? -1 : sc;
    }
    int nextScript = last;
    for (size_t i = n; i-- > 0;) {
      if (scripts[i] >= 0) nextScript = scripts[i];
      else scripts[i] = nextScript; // overwritten below by the one before, where there is one
    }
    last = -1;
    for (size_t i = 0; i < n; ++i) {
      const int sc = detail::shaping::scriptOf(text[i]);
      if (!detail::shaping::isNeutralScript(sc)) last = sc;
      else if (last >= 0) scripts[i] = last;
    }

    // A character the bidirectional algorithm passes over and nothing draws
    // -- a joiner, an embedding control, a soft hyphen -- is shaped with the
    // run it stands in, at the level of the character before it, or of the
    // first one where none is: ZWJ must still join what is on either side
    // of it, and ZWNJ part them. A control character it passes over is left
    // out, as it was.
    auto dropped = [&](size_t i) { return paragraph.isRemoved(i) && !detail::shaping::isIgnorable(text[i]); };
    std::vector<uint8_t> runLevels(levels);
    size_t lead = 0;
    while (lead < n && paragraph.isRemoved(lead)) ++lead;
    for (size_t i = 0; i < n && lead < n; ++i) {
      if (!paragraph.isRemoved(i) || dropped(i)) continue;
      runLevels[i] = i < lead ? levels[lead] : runLevels[i - 1];
      if (i < lead) faces[i] = faces[lead];
    }
    struct Run {
      size_t from, to;
      uint8_t level;
    };
    std::vector<Run> runs;
    for (size_t i = 0; i < n;) {
      if (dropped(i)) {
        ++i;
        continue;
      }
      size_t j = i + 1;
      while (j < n && !dropped(j) && runLevels[j] == runLevels[i] && faces[j] == faces[i] &&
             scripts[j] == scripts[i])
        ++j;
      runs.push_back({i, j, runLevels[i]});
      i = j;
    }
    // Rule L2 over the runs: from the highest level down to the lowest odd
    // one, every sequence of runs at that level or above is reversed. A
    // right-to-left run's own glyphs come back from shaping in drawn order.
    uint8_t highest = 0, lowestOdd = 255;
    for (const Run& r : runs) {
      highest = (std::max)(highest, r.level);
      if (r.level & 1) lowestOdd = (std::min)(lowestOdd, r.level);
    }
    for (int level = highest; level >= (int) lowestOdd && level > 0; --level)
      for (size_t k = 0; k < runs.size();) {
        if (runs[k].level < level) {
          ++k;
          continue;
        }
        size_t e = k;
        while (e < runs.size() && runs[e].level >= level) ++e;
        std::reverse(runs.begin() + (long) k, runs.begin() + (long) e);
        k = e;
      }

    for (const Run& r : runs) {
      const Typeface* face = faces[r.from];
      const std::vector<int16_t>* location = face == face_.get() && location_ ? location_.get() : nullptr;
      // The caller's features, for the typeface they were chosen for.
      const auto* features = face == face_.get() && !features_.empty() ? &features_ : nullptr;
      for (const ShapedGlyph& g :
           shapeRun(*face, text.data() + r.from, r.to - r.from, (r.level & 1) != 0, location, features))
        line->glyphs.push_back({face, g.glyph, g.xAdvance, g.xOffset, g.yOffset, (uint32_t) (r.from + g.cluster),
                                (r.level & 1) != 0});
    }
    // A string a size animates through, or a caller formatting a readout
    // every frame, would grow this without end; and a long text -- a
    // TextArea's, laid out whole on every edit -- is no label to keep.
    if (utf8.size() > 1024) return line;
    if (shaped_->size() >= 512) shaped_->clear();
    shaped_->emplace(utf8, line);
    return line;
  }

  /** The cache holds outlines in DESIGN units, so one entry serves every
   *  size. A cache keyed by size would be thrown away on every window
   *  resize, which is exactly when a UI can least afford it. Keyed by face
   *  as well as glyph: a fallback face's glyph 37 is another letter. Every
   *  face keyed here outlives the cache -- the font's own is held by it, and
   *  fallback faces are kept for the life of the process. */
  void appendCachedGlyph(Path& out, const Typeface& face, uint16_t glyph, float x, float y,
                         float s) const {
    const auto key = std::make_pair(static_cast<const void*>(&face), glyph);
    auto it = cache_->find(key);
    if (it == cache_->end()) {
      Path built;
      if (location_ && &face == face_.get()) face.appendGlyph(glyph, built, *location_);
      else face.appendGlyph(glyph, built, 0.0f, 0.0f, 1.0f);
      it = cache_->emplace(key, std::move(built)).first;
    }
    // Flattened at this size once, and moved to the pen on every draw. It
    // was flattened again on every draw -- curves subdivided, contours
    // allocated -- a quarter of what a label cost. The same points: the
    // subdivision depends on the scale and not on where the glyph is, and
    // a transform adds its translation after the scale, a + e as here.
    const FlatKey flatKey{key.first, glyph, s};
    auto flat = flat_->find(flatKey);
    if (flat == flat_->end()) {
      // A size that animates -- a zoom -- would make one entry per frame.
      if (flat_->size() >= 8192) flat_->clear();
      FlatGlyph made;
      std::vector<Contour> contours;
      it->second.flatten(Transform::scaling(s, s), contours, 0.05f);
      for (const Contour& c : contours) {
        if (c.points.size() < 2) continue;
        made.points.insert(made.points.end(), c.points.begin(), c.points.end());
        made.ends.push_back((uint32_t) made.points.size());
      }
      flat = flat_->emplace(flatKey, std::move(made)).first;
    }
    uint32_t start = 0;
    for (const uint32_t end : flat->second.ends) {
      const Point* p = flat->second.points.data();
      out.moveTo(p[start].x + x, p[start].y + y);
      for (uint32_t k = start + 1; k < end; ++k) out.lineTo(p[k].x + x, p[k].y + y);
      out.close();
      start = end;
    }
  }

  /**
   * A colour glyph drawn, the outlines gathered before it filled first so
   * they stay beneath it: COLR's layers, each in its palette colour -- its
   * alpha scaled by the text colour's, so a faded label fades its emoji too
   * -- or in the text's own fill; or the picture of the strike that suits
   * the size it lands at on the screen, through the transform. False for a
   * glyph with no colour, or a picture that does not decode, which is drawn
   * as its outline.
   */
  bool drawColourGlyph(Graphics& g, Path& pending, const Typeface& face, uint16_t glyph, float x, float y,
                       float s) const {
    const std::vector<ColourLayer> layers = face.colourLayers(glyph);
    if (!layers.empty()) {
      g.fillPath(pending, FillRule::NonZero);
      pending = Path();
      const uint8_t ink = g.colour().a;
      for (const ColourLayer& layer : layers) {
        Path outline;
        appendCachedGlyph(outline, face, layer.glyph, x, y, s);
        if (layer.foreground) {
          g.fillPath(outline, FillRule::NonZero);
          continue;
        }
        g.save();
        g.setColour(Colour(layer.red, layer.green, layer.blue, (uint8_t) ((layer.alpha * ink + 127) / 255)));
        g.fillPath(outline, FillRule::NonZero);
        g.restore();
      }
      return true;
    }
    const Transform& t = g.state().transform;
    const float device = std::sqrt(std::fabs(t.a * t.d - t.b * t.c));
    const float pixelsPerEm = s * (float) face.unitsPerEm();
    ImageLayer image;
    if (!face.colourImage(glyph, pixelsPerEm * (device > 0.0f ? device : 1.0f), &image)) return false;
    const ImageKey key{&face, glyph, image.ppem};
    auto it = images_->pictures.find(key);
    if (it == images_->pictures.end()) {
      // Emoji a preset list scrolls through, a strike each: kept within
      // bounds by what they hold, an emoji picker's worth.
      if (images_->bytes > kImageBytes || images_->pictures.size() >= 2048) {
        images_->pictures.clear();
        images_->bytes = 0;
      }
      PngImage decoded = PngDecoder::decode(image.png, image.size);
      std::shared_ptr<const Bitmap> picture;
      if (decoded.ok()) {
        images_->bytes += decoded.bitmap.sizeInBytes();
        picture = std::make_shared<const Bitmap>(std::move(decoded.bitmap));
      }
      it = images_->pictures.emplace(key, std::move(picture)).first;
    }
    if (!it->second) return false;
    g.fillPath(pending, FillRule::NonZero);
    pending = Path();
    const float k = pixelsPerEm / (float) image.ppem; // a strike pixel, in the caller's units
    g.drawImageTransformed(*it->second, Transform(k, 0.0f, 0.0f, k, x + (float) image.left * k, y + (float) image.top * k));
    return true;
  }

  /** A glyph's coverage at one size and one of sixteen quarter-pixel phases:
   *  rasterised once, its top-left this far from the whole pixel the pen is
   *  in. `outline` for a glyph too big to keep -- a hostile font's, whose
   *  outline reaches far past its em -- which is filled as an outline. */
  struct GlyphCoverage {
    int left = 0, top = 0, width = 0, height = 0;
    bool outline = false;
    std::vector<uint8_t> coverage;
  };
  /** The largest em, on the device, text is drawn from cached coverage at. */
  static constexpr float kCoverageEm = 256.0f;
  /** The largest glyph kept as coverage, a side; past it, filled. */
  static constexpr float kCoverageSide = 1024.0f;
  /** What the kept coverage may hold before it is let go. */
  static constexpr size_t kCoverageBytes = 8u * 1024u * 1024u;

  /**
   * A glyph's coverage at `scale` (device pixels per design unit), the pen
   * a quarter-pixel phase into its pixel: rasterised the first time, by the
   * same rasteriser and from the same flattened outline fillPath would use,
   * and kept. Null for a glyph that covers nothing.
   */
  std::shared_ptr<const GlyphCoverage> glyphCoverage(const Typeface& face, uint16_t glyph, float scale, int phaseX,
                                                     int phaseY) const {
    const CoverageKey key{&face, glyph, scale, (uint8_t) (phaseX * 4 + phaseY)};
    auto it = coverage_->glyphs.find(key);
    if (it != coverage_->glyphs.end()) return it->second;
    // Bounded by what it holds -- some thousands of glyphs at a size -- not
    // by a count a page of CJK in two sizes fills; a size that animates
    // makes a set per frame, and is let go with the rest.
    if (coverage_->bytes > kCoverageBytes || coverage_->glyphs.size() >= 65536) {
      coverage_->glyphs.clear();
      coverage_->bytes = 0;
    }
    Path outline;
    appendCachedGlyph(outline, face, glyph, (float) phaseX * 0.25f, (float) phaseY * 0.25f, scale);
    std::shared_ptr<GlyphCoverage> made;
    const std::vector<Point>& points = outline.points();
    if (!points.empty()) {
      float x0 = points[0].x, x1 = x0, y0 = points[0].y, y1 = y0;
      for (const Point& p : points) {
        x0 = (std::min)(x0, p.x);
        x1 = (std::max)(x1, p.x);
        y0 = (std::min)(y0, p.y);
        y1 = (std::max)(y1, p.y);
      }
      // A pixel of margin: coverage spills into the pixel an edge is in.
      made = std::make_shared<GlyphCoverage>();
      if (!(x1 - x0 <= kCoverageSide && y1 - y0 <= kCoverageSide && std::fabs(x0) < 1.0e6f &&
            std::fabs(y0) < 1.0e6f)) {
        made->outline = true;
      } else {
        made->left = (int) std::floor(x0) - 1;
        made->top = (int) std::floor(y0) - 1;
        made->width = (int) std::ceil(x1) + 1 - made->left;
        made->height = (int) std::ceil(y1) + 1 - made->top;
        Bitmap ink(made->width, made->height);
        Rasteriser raster;
        raster.fill(ink, outline, Transform::translation((float) -made->left, (float) -made->top),
                    Colour(255, 255, 255, 255), FillRule::NonZero);
        made->coverage.resize((size_t) made->width * (size_t) made->height);
        const uint8_t* rgba = ink.data();
        for (size_t i = 0; i < made->coverage.size(); ++i) made->coverage[i] = rgba[i * 4u + 3u];
        coverage_->bytes += made->coverage.size() + sizeof(GlyphCoverage);
      }
    }
    return coverage_->glyphs.emplace(key, std::move(made)).first->second;
  }

  /**
   * A line of text drawn from its glyphs' coverage: each glyph's, at the
   * quarter pixel nearest where it lands on the device, laid into one
   * buffer for the line -- the part of it the clip shows -- as each would
   * be drawn over the last, and the buffer painted once. A colour glyph is
   * drawn as it comes, the coverage gathered before it painted first; a
   * glyph too big to keep is filled as its outline.
   */
  void drawFromCoverage(Graphics& g, const std::string& utf8, float x, float baselineY, const Transform& t) const {
    const PixelRect clip = g.state().clip;
    struct Placed {
      std::shared_ptr<const GlyphCoverage> glyph;
      int x, y;
    };
    std::vector<Placed> placed;
    // This draw's own, not the Font's, like everything here but the caches
    // -- which draw holds the lock on.
    std::vector<uint8_t> line;
    Path outlines;
    auto flush = [&]() {
      if (!outlines.isEmpty()) {
        g.fillPath(outlines, FillRule::NonZero);
        outlines = Path();
      }
      if (placed.empty()) return;
      int x0 = placed[0].x, y0 = placed[0].y, x1 = x0, y1 = y0;
      for (const Placed& p : placed) {
        x0 = (std::min)(x0, p.x);
        y0 = (std::min)(y0, p.y);
        x1 = (std::max)(x1, p.x + p.glyph->width);
        y1 = (std::max)(y1, p.y + p.glyph->height);
      }
      x0 = (std::max)(x0, clip.x);
      y0 = (std::max)(y0, clip.y);
      x1 = (std::min)(x1, clip.right());
      y1 = (std::min)(y1, clip.bottom());
      if (x1 <= x0 || y1 <= y0) {
        placed.clear();
        return;
      }
      const int w = x1 - x0, h = y1 - y0;
      line.assign((size_t) w * (size_t) h, 0);
      for (const Placed& p : placed) {
        const GlyphCoverage& c = *p.glyph;
        const int top = (std::max)(0, y0 - p.y), bottom = (std::min)(c.height, y1 - p.y);
        const int left = (std::max)(0, x0 - p.x), right = (std::min)(c.width, x1 - p.x);
        for (int row = top; row < bottom; ++row) {
          uint8_t* to = line.data() + (size_t) (p.y - y0 + row) * (size_t) w + (size_t) (p.x - x0);
          const uint8_t* from = c.coverage.data() + (size_t) row * (size_t) c.width;
          // Over, as drawing one glyph after the other gives: where a joined
          // Arabic stroke or a mark overlaps its neighbour's edge, a sum
          // made those pixels heavy.
          for (int col = left; col < right; ++col) {
            const unsigned a = to[col], b = from[col];
            to[col] = (uint8_t) (a + b - (a * b + 127u) / 255u);
          }
        }
      }
      g.fillCoverage(line.data(), w, h, x0, y0);
      placed.clear();
    };
    forEachGlyph(utf8, x, [&](const Typeface& face, uint16_t glyph, float pen, float s, float dy) {
      if (face.hasColourGlyphs()) {
        flush();
        Path none;
        if (drawColourGlyph(g, none, face, glyph, pen, baselineY + dy, s)) return;
      }
      // Where the pen lands on the device, to the nearest quarter pixel.
      const float qx = std::floor((t.a * pen + t.e) * 4.0f + 0.5f), qy = std::floor((t.d * (baselineY + dy) + t.f) * 4.0f + 0.5f);
      if (!(std::fabs(qx) < 3.0e7f && std::fabs(qy) < 3.0e7f)) return; // off any device, or not a number
      const int px = (int) std::floor(qx / 4.0f), py = (int) std::floor(qy / 4.0f);
      const int phaseX = (int) qx - px * 4, phaseY = (int) qy - py * 4;
      std::shared_ptr<const GlyphCoverage> c = glyphCoverage(face, glyph, s * t.a, phaseX, phaseY);
      if (!c) return;
      if (c->outline) {
        appendCachedGlyph(outlines, face, glyph, pen, baselineY + dy, s);
        return;
      }
      const int left = px + c->left, top = py + c->top;
      // Wholly off the clip: none of it is drawn.
      if (left >= clip.right() || top >= clip.bottom() || left + c->width <= clip.x || top + c->height <= clip.y)
        return;
      placed.push_back({std::move(c), left, top});
    });
    flush();
  }

  /** A varied glyph's advance, worked out once: through HVAR it is a sum
   *  over the font's regions, and a label asks for every character's. */
  float variedAdvance(uint16_t glyph) const {
    auto it = advances_->find(glyph);
    if (it == advances_->end())
      it = advances_->emplace(glyph, face_->advanceWidth(glyph, *location_)).first;
    return it->second;
  }

  std::shared_ptr<Typeface> face_;
  float height_ = 0.0f;
  bool fallback_ = true;
  std::vector<std::pair<uint32_t, float>> settings_;
  /** The normalised location; null at the default design. */
  std::shared_ptr<const std::vector<int16_t>> location_;
  std::vector<std::pair<uint32_t, uint32_t>> features_;
  /** Guards every cache below, and is shared with them: one per family of
   *  copies. Taken by the public calls that reach a cache, never by anything
   *  they call -- it is not recursive, so drawIn and the wrapping calls,
   *  which measure and then draw, take it for each in turn. See the class
   *  comment.
   *
   *  Deliberately not one cache per typeface for the whole module, shared by
   *  every editor and every open: measured, it would save about 6 ms of the
   *  showcase's 44 ms open, and the caches (120 KB there) would then stay
   *  after the editor closed, where they now go with it, under a lock every
   *  Font of the face would contend for. */
  std::shared_ptr<std::mutex> cacheLock_ = std::make_shared<std::mutex>();
  std::shared_ptr<std::map<uint16_t, float>> advances_ = std::make_shared<std::map<uint16_t, float>>();
  /** Shaped strings, by their text: the bidi and shaping of a label done
   *  once, not on each of the thirty draws a second it gets. */
  std::shared_ptr<std::map<std::string, std::shared_ptr<const ShapedLine>>> shaped_ =
      std::make_shared<std::map<std::string, std::shared_ptr<const ShapedLine>>>();
  /** Shared between copies of the same Font, so passing one by value does not
   *  rebuild every glyph. */
  using GlyphKey = std::pair<const void*, uint16_t>;
  std::shared_ptr<std::map<GlyphKey, Path>> cache_ = std::make_shared<std::map<GlyphKey, Path>>();
  /** A glyph flattened at one size: its points, and where each contour ends. */
  struct FlatGlyph {
    std::vector<Point> points;
    std::vector<uint32_t> ends;
  };
  /** Which flattening: a face's glyph at one scale (pixels per design unit).
   *  The cache it keys is emptied whole at 8192 entries, so a size that
   *  animates cannot grow it without end. */
  struct FlatKey {
    const void* face;
    uint16_t glyph;
    float scale;
    bool operator<(const FlatKey& o) const {
      if (face != o.face) return face < o.face;
      if (glyph != o.glyph) return glyph < o.glyph;
      return scale < o.scale;
    }
  };
  std::shared_ptr<std::map<FlatKey, FlatGlyph>> flat_ = std::make_shared<std::map<FlatKey, FlatGlyph>>();
  /** Which coverage: a face's glyph at a device scale and one of sixteen
   *  quarter-pixel phases (x phase times 4, plus the y phase). */
  struct CoverageKey {
    const void* face;
    uint16_t glyph;
    float scale;
    uint8_t phase;
    bool operator<(const CoverageKey& o) const {
      if (face != o.face) return face < o.face;
      if (glyph != o.glyph) return glyph < o.glyph;
      if (scale != o.scale) return scale < o.scale;
      return phase < o.phase;
    }
  };
  /** Glyphs' coverage, rasterised once per size and quarter-pixel phase;
   *  null where a glyph covers nothing. Emptied whole once it holds more
   *  than kCoverageBytes, or 65536 glyphs. */
  struct CoverageCache {
    std::map<CoverageKey, std::shared_ptr<const GlyphCoverage>> glyphs;
    size_t bytes = 0; // what the kept coverage holds
  };
  std::shared_ptr<CoverageCache> coverage_ = std::make_shared<CoverageCache>();
  /** Which picture: a face's colour glyph in the strike of one pixels-per-em. */
  struct ImageKey {
    const void* face;
    uint16_t glyph;
    int ppem;
    bool operator<(const ImageKey& o) const {
      if (face != o.face) return face < o.face;
      if (glyph != o.glyph) return glyph < o.glyph;
      return ppem < o.ppem;
    }
  };
  /** Colour glyphs' pictures, decoded once per strike; null where one did
   *  not decode. Emptied whole past kImageBytes, or 2048 pictures. */
  struct ImageCache {
    std::map<ImageKey, std::shared_ptr<const Bitmap>> pictures;
    size_t bytes = 0; // what the decoded pictures hold
  };
  static constexpr size_t kImageBytes = 64u * 1024u * 1024u;
  std::shared_ptr<ImageCache> images_ = std::make_shared<ImageCache>();
};

} // namespace gfx
} // namespace sonore
