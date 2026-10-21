// SPDX-License-Identifier: Apache-2.0
//
// A TrueType and OpenType parser: glyph outlines out of a .ttf or an .otf, as
// ordinary Paths -- quadratic glyf outlines and cubic CFF ones alike.
//
// ── Why parse it here rather than use a library or the platform ─────────────
//
// Two alternatives were on the table and both cost more than they look.
//
// Platform text -- DirectWrite, CoreText, Pango -- means three backends whose
// metrics disagree, so the same interface has different line breaks on
// different machines and I could never see two of the three. That is exactly
// the property the software rasteriser exists to avoid.
//
// A third-party rasteriser (stb_truetype and friends) means a second
// rasteriser with its own antialiasing, sitting next to the one this SDK
// already has and has tested to a fifth of a pixel.
//
// Parsing the outlines directly costs about five hundred lines and gives
// glyphs as Path objects, drawn by the SAME rasteriser as everything else.
// One antialiasing quality, one set of tests, identical output everywhere,
// and no dependency or licence to review.
//
// ── What this does NOT do ───────────────────────────────────────────────────
//
// No hinting, no OpenType layout (GSUB/GPOS), no bidi, no shaping. Those are
// what you need for Arabic, Devanagari or fine ligature control, and each is
// larger than everything here put together. What is covered is the text a
// plugin's interface is made of: a glyph per character, advance widths from
// hmtx, kerning from GPOS or the old kern table -- and a variable font moved
// along its axes (fvar, avar, gvar, HVAR).
//
// A font this cannot read is REFUSED rather than half-drawn. A parser that
// guesses at a table it does not understand produces glyphs that are subtly
// the wrong shape, which nobody reports as a bug because it just looks bad.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "path.h"

namespace sonore {
namespace gfx {

/**
 * One axis a variable font moves along: weight ('wght'), width ('wdth'),
 * slant ('slnt'), italic ('ital'), optical size ('opsz'), or one the
 * designer named. Values are the font's own -- a weight of 300 to 700, a
 * width of 75 to 100 per cent.
 */
struct FontAxis {
  /** The four-character tag packed big-endian, first character in the top
   *  byte, as tagOf() makes it; name() gives it back as text. */
  uint32_t tag = 0;
  float minimum = 0.0f,      ///< The lowest value, in the axis's own units, from fvar.
      defaultValue = 0.0f,   ///< The value with no variation asked for; minimum <= defaultValue <= maximum.
      maximum = 0.0f;        ///< The highest value, in the axis's own units.
  /** The font asks for the axis not to be offered to users. */
  bool hidden = false;

  /** The tag as text: "wght". */
  std::string name() const {
    std::string t(4, ' ');
    for (int i = 0; i < 4; ++i) t[(size_t) i] = (char) ((tag >> (24 - 8 * i)) & 0xFF);
    return t;
  }
};

namespace detail {

/** A font file's bytes, read where they are and never written: a vector the
 *  typeface owns, or memory something else keeps valid -- a mapped file,
 *  data compiled into the plugin. Copies share them. */
class FontBytes {
public:
  FontBytes() = default;
  explicit FontBytes(std::vector<uint8_t> owned) {
    auto held = std::make_shared<const std::vector<uint8_t>>(std::move(owned));
    p_ = held->data();
    n_ = held->size();
    keep_ = std::move(held);
  }
  FontBytes(const uint8_t* bytes, size_t size, std::shared_ptr<const void> keepAlive)
      : keep_(std::move(keepAlive)), p_(bytes), n_(bytes ? size : 0) {}

  const uint8_t* data() const { return p_; }
  size_t size() const { return n_; }
  bool empty() const { return n_ == 0; }
  uint8_t operator[](size_t i) const { return p_[i]; }
  void clear() { *this = FontBytes(); }

private:
  std::shared_ptr<const void> keep_;
  const uint8_t* p_ = nullptr;
  size_t n_ = 0;
};

} // namespace detail

/**
 * A layer of a colour glyph, from a font's COLR and CPAL tables (version 0,
 * as Windows' Segoe UI Emoji has them): the outline of `glyph`, filled with
 * this colour -- or, where `foreground`, with the colour the text is drawn
 * in. A glyph's layers are drawn in order, the first at the bottom.
 */
struct ColourLayer {
  /** The glyph index whose outline this layer fills, as appendGlyph takes it. */
  uint16_t glyph = 0;
  /** The fill colour from the palette, 0..255 per channel, straight alpha (not
   *  premultiplied). Ignored when `foreground` is set. */
  uint8_t red = 0,
      green = 0,  ///< The fill's green, 0..255.
      blue = 0,   ///< The fill's blue, 0..255.
      alpha = 255; ///< The fill's alpha, 0..255, straight (not premultiplied).
  /** Fill with the text's own colour instead: the font's 0xFFFF entry, or a
   *  palette entry the font does not have. */
  bool foreground = false;
};

/**
 * A colour glyph as a picture, from a font's sbix table (macOS's Apple Color
 * Emoji) or its CBDT and CBLC (Linux's Noto Color Emoji): PNG bytes, read in
 * place from the font, and where the picture goes -- its top-left corner
 * from the glyph's origin on the baseline, x to the right and y DOWN, in
 * pixels of the strike it was made for, `ppem` pixels to the em.
 */
struct ImageLayer {
  /** The PNG file's bytes, pointing into the font's own data: valid while
   *  the Typeface (or a copy sharing its bytes) is alive and not reloaded. */
  const uint8_t* png = nullptr;
  /** Length of `png` in bytes. */
  size_t size = 0;
  /** The strike's size, pixels to the em: scale the picture by the text's
   *  pixels per em over this. */
  int ppem = 0;
  /** The picture's top-left corner from the glyph origin, in the strike's
   *  pixels, x right and y down (negative `top` is above the baseline). */
  int left = 0,
      top = 0; ///< The picture's top edge from the baseline, strike pixels, y down.
};

/**
 * A parsed font file.
 *
 * Holds the file bytes and reads out of them on demand. Glyph outlines are
 * built when asked for, because a font has thousands and an interface uses
 * eighty.
 */
class Typeface {
public:
  /** Takes ownership of the bytes. Returns false for anything it cannot read
   *  with confidence, leaving the object unusable rather than approximate. */
  bool load(std::vector<uint8_t> fileBytes) { return load(detail::FontBytes(std::move(fileBytes))); }

  /**
   * Reads a font where its bytes already are, without copying them: data
   * compiled into the plugin, with `keepAlive` null -- the bytes must then
   * outlast every use of the typeface -- or memory `keepAlive` keeps valid,
   * as loadTypefaceFile's mapped files are. Copies of the typeface share it.
   */
  bool load(const uint8_t* bytes, size_t size, std::shared_ptr<const void> keepAlive = nullptr) {
    return load(detail::FontBytes(bytes, size, std::move(keepAlive)));
  }

  /** The overload the two above forward to. Clears everything the last font
   *  left before reading this one; on failure the object is as a new one --
   *  the bytes released, every metric 0 -- except that error() says why. */
  bool load(detail::FontBytes bytes) {
    // Every offset, table and count the last font left, back to nothing: a
    // Typeface that held a .ttf kept its glyf and loca, and a CFF font loaded
    // into it drew from them -- empty glyphs.
    *this = Typeface();
    data_ = std::move(bytes);
    ok_ = parse();
    if (!ok_) {
      // And what THIS font got as far as reading: parse() fills the metrics
      // before it reads the CFF table, so a refused CFF font left an invalid
      // typeface answering unitsPerEm() and ascender() with its numbers.
      std::string why = std::move(error_);
      *this = Typeface();
      error_ = std::move(why);
    }
    return ok_;
  }

  /** True after a load() that succeeded. After a failed one every query
   *  answers 0, false or empty, the metrics included. */
  bool isValid() const { return ok_; }
  /** Why the last load() failed, in English ("not a TrueType or OpenType
   *  font"); empty after a successful load or before any. */
  const std::string& error() const { return error_; }

  /** Font design units per em. Every metric below is in these units; divide
   *  by this and multiply by the point size to get pixels. */
  int unitsPerEm() const { return unitsPerEm_; }
  /** How many glyphs the font has (maxp): valid glyph indices are 0 to this
   *  minus one, 0 being .notdef. */
  int numGlyphs() const { return numGlyphs_; }

  /** The hhea ascent: how far the tallest glyphs rise above the baseline, in
   *  design units, positive. */
  int ascender() const { return ascender_; }
  /** The hhea descent, in design units: NEGATIVE for a font that descends
   *  below the baseline, as fonts store it. */
  int descender() const { return descender_; }
  /** The hhea line gap: the extra space between one line's descent and the
   *  next line's ascent, in design units; often 0. */
  int lineGap() const { return lineGap_; }

  /**
   * A table's bytes, for code that reads a table this class does not: the
   * OpenType layout tables text_shaper.h reads. False when the font has no
   * such table, or one that runs past the end of the file.
   */
  bool table(uint32_t tag, const uint8_t** bytes, uint32_t* length) const {
    if (!ok_) return false;
    for (uint16_t i = 0; i < numTables_; ++i) {
      const uint32_t rec = directory_ + (uint32_t) i * 16u;
      if (readU32(rec) != tag) continue;
      const uint32_t off = readU32(rec + 8), len = readU32(rec + 12);
      if (off > data_.size() || len > data_.size() - off) return false;
      *bytes = data_.data() + off;
      *length = len;
      return true;
    }
    return false;
  }

  /** The glyph for a Unicode code point, or 0 -- which by the specification
   *  is .notdef, the empty box. Returning 0 rather than failing is right: a
   *  missing character should draw the box a user recognises. */
  uint16_t glyphForChar(uint32_t codepoint) const {
    if (!ok_ || cmapOffset_ == 0) return 0;
    if (cmapFormat_ == 4) return lookupFormat4(codepoint);
    if (cmapFormat_ == 12) return lookupFormat12(codepoint);
    return 0;
  }

  /** Whether the font has colour glyphs: layers (COLR) or pictures (sbix,
   *  CBDT). */
  bool hasColourGlyphs() const { return ok_ && (colrOffset_ || sbixOffset_ || (cblcOffset_ && cbdtOffset_)); }

  /** How many palettes the font's CPAL table offers colour layers. */
  int numPalettes() const { return ok_ && cpalOffset_ ? (int) readU16(cpalOffset_ + 4) : 0; }

  /**
   * The layers of a colour glyph, as COLR version 0 lists them, coloured
   * from palette `palette` -- the first, where there is no such one -- or
   * none for a glyph with no layers. A layer whose colour the palette lacks
   * is drawn in the text's colour, as HarfBuzz draws it. No more than
   * kMaxColourLayers: Segoe UI Emoji's busiest glyph has 120, and a font
   * claiming 65,535 of one rectangle took a tenth of a second a glyph.
   */
  std::vector<ColourLayer> colourLayers(uint16_t glyph, int palette = 0) const {
    std::vector<ColourLayer> out;
    if (!ok_ || colrOffset_ == 0) return out;
    // Base glyph records of 6 bytes, sorted by glyph: the glyph, its first
    // layer and how many it has. Layer records of 4: a glyph and a colour.
    const uint32_t bases = colrOffset_ + readU32(colrOffset_ + 4);
    const uint32_t layers = colrOffset_ + readU32(colrOffset_ + 8);
    const uint32_t layerCount = readU16(colrOffset_ + 12);
    uint32_t lo = 0, hi = readU16(colrOffset_ + 2);
    while (lo < hi) {
      const uint32_t mid = lo + (hi - lo) / 2;
      const uint32_t at = bases + mid * 6u;
      const uint16_t base = readU16(at);
      if (glyph < base) hi = mid;
      else if (glyph > base) lo = mid + 1;
      else {
        const uint32_t first = readU16(at + 2);
        const uint32_t end = (std::min)((std::min)(first + readU16(at + 4), layerCount), first + kMaxColourLayers);
        for (uint32_t k = first; k < end; ++k) {
          ColourLayer layer;
          layer.glyph = readU16(layers + k * 4u);
          layer.foreground = !paletteColour(palette, readU16(layers + k * 4u + 2), &layer);
          out.push_back(layer);
        }
        break;
      }
    }
    return out;
  }

  /**
   * A colour glyph's picture, for text drawn `pixelsPerEm` pixels to the em:
   * from the strike HarfBuzz would take -- the smallest at least that size,
   * or else the largest -- in sbix, or in CBDT where the font has no sbix.
   * False where that strike has no picture of the glyph, or none in PNG.
   */
  bool colourImage(uint16_t glyph, float pixelsPerEm, ImageLayer* out) const {
    if (!ok_ || glyph >= numGlyphs_) return false;
    // Under a pixel, the smallest strike; no size, or no number, the largest,
    // as HarfBuzz takes for a font with none.
    uint32_t wanted = 1u << 30;
    if (pixelsPerEm > 0.0f && pixelsPerEm < 65536.0f) wanted = pixelsPerEm < 1.0f ? 1u : (uint32_t) (pixelsPerEm + 0.5f);
    if (sbixOffset_) return sbixImage(glyph, wanted, out);
    if (cblcOffset_ && cbdtOffset_) return cbdtImage(glyph, wanted, out);
    return false;
  }

  /**
   * The glyph for a character followed by a variation selector, from the
   * cmap's variation sequences (format 14) -- an emoji's emoji or text
   * presentation, an ideograph's variant -- or 0 where the font lists no
   * glyph for the pair. A pair the font lists as its default takes the
   * character's own glyph.
   */
  uint16_t variationGlyph(uint32_t codepoint, uint32_t selector) const {
    if (!ok_ || uvsOffset_ == 0) return 0;
    // Records of 11 bytes: the selector, then its default and non-default
    // tables' offsets, sorted by selector.
    const uint32_t at = uvsSearch(uvsOffset_ + 6u, 11u, selector, 0);
    if (at == 0) return 0;
    const uint32_t defaults = readU32(at + 3), special = readU32(at + 7);
    // Default ranges of 4 bytes: a first character and how many follow it.
    if (defaults && uvsSearch(uvsOffset_ + defaults, 4u, codepoint, 1)) return glyphForChar(codepoint);
    // Mappings of 5 bytes: a character and its glyph.
    const uint32_t mapping = special ? uvsSearch(uvsOffset_ + special, 5u, codepoint, 0) : 0;
    return mapping ? readU16(mapping + 3) : 0;
  }

  /** Advance width in design units. */
  int advanceWidth(uint16_t glyph) const {
    if (!ok_ || hmtxOffset_ == 0 || numHMetrics_ == 0) return 0;
    // Past numberOfHMetrics the advances stop and only left side bearings
    // follow, so every remaining glyph shares the LAST advance. A parser that
    // indexes past the end here reads bearings as widths and the text comes
    // out with wild spacing.
    const uint16_t index = glyph < numHMetrics_ ? glyph : (uint16_t) (numHMetrics_ - 1);
    return readU16(hmtxOffset_ + (uint32_t) index * 4u);
  }

  /** Kerning between two glyphs, in design units, from the old `kern` table.
   *  Zero when there is none, which is the common case for modern fonts that
   *  put kerning in GPOS instead. */
  int kerning(uint16_t left, uint16_t right) const {
    if (!ok_) return 0;
    // GPOS first, where a font has it: it is where every font made this
    // century keeps its kerning, and many -- Georgia, Bahnschrift, most .otf
    // a designer ships -- have no kern table at all, so their text set with
    // no kerning whatever.
    if (!kernLookups_.empty()) {
      int total = 0;
      CoverageMemo memo;
      for (const std::vector<uint32_t>& lookup : kernLookups_)
        for (const uint32_t subtable : lookup) {
          int value = 0;
          if (pairAdjustment(subtable, left, right, &value, memo)) {
            total += value;
            break; // the first subtable that applies is the lookup's answer
          }
        }
      return total;
    }
    return kernTableKerning(left, right);
  }

  /** Whether the font has the old `kern` table, read by kernTableKerning. */
  bool hasKernTable() const { return ok_ && kernOffset_ != 0; }

  /** Kerning from the old `kern` table alone, whatever GPOS has: what a
   *  shaper applies when the script's GPOS features have no kern. */
  int kernTableKerning(uint16_t left, uint16_t right) const {
    if (!ok_ || kernOffset_ == 0) return 0;
    const uint32_t key = ((uint32_t) left << 16) | (uint32_t) right;
    // Binary search: the subtable is sorted by that packed key, and a linear
    // scan of a few thousand pairs per character pair would show up in a
    // repaint.
    uint32_t lo = 0, hi = kernPairs_;
    while (lo < hi) {
      const uint32_t mid = (lo + hi) / 2;
      const uint32_t at = kernOffset_ + mid * 6u;
      const uint32_t k = ((uint32_t) readU16(at) << 16) | (uint32_t) readU16(at + 2);
      if (k == key) return (int16_t) readU16(at + 4);
      if (k < key) lo = mid + 1;
      else hi = mid;
    }
    return 0;
  }

  /**
   * Append a glyph's outline to `out`, in design units, y DOWNWARD.
   *
   * TrueType has y increasing upward; every graphics API here has it going
   * down. Flipping at the source means no caller ever has to remember, and
   * the alternative -- a transform at each use -- is a sign error waiting in
   * whichever call site was written last.
   */
  bool appendGlyph(uint16_t glyph, Path& out, float xOffset = 0.0f, float yOffset = 0.0f,
                   float scale = 1.0f) const {
    int budget = kMaxGlyphParts;
    return appendGlyphImpl(glyph, out, Transform(scale, 0.0f, 0.0f, -scale, xOffset, yOffset),
                           nullptr, true, 0, &budget);
  }

  /**
   * A glyph's ink box in design units, y UP, as HarfBuzz reports its extents:
   * for a TrueType glyph the box its header stores, moved to begin at its
   * left side bearing, where rasterisers draw it; for CFF, or at a location
   * in a variable font, the box of its outline's points, control points
   * included. An empty glyph -- a space -- has an empty box at the origin.
   * False for a glyph the font does not have.
   */
  bool glyphBox(uint16_t glyph, int* xMin, int* yMin, int* xMax, int* yMax,
                const std::vector<int16_t>* location = nullptr) const {
    *xMin = *yMin = *xMax = *yMax = 0;
    if (!ok_ || glyph >= numGlyphs_) return false;
    const bool varies = location && !location->empty() && isVariable() && location->size() == axes_.size();
    if (!cffOffset_ && !varies) {
      uint32_t length = 0;
      const uint32_t at = glyphOffset(glyph, &length);
      if (at == 0 || length < 10) return true;
      const int x0 = (int16_t) readU16(at + 2), y0 = (int16_t) readU16(at + 4);
      const int x1 = (int16_t) readU16(at + 6), y1 = (int16_t) readU16(at + 8);
      *xMin = hmtxOffset_ ? leftSideBearing(glyph) : (std::min)(x0, x1);
      *xMax = *xMin + (std::max)(x0, x1) - (std::min)(x0, x1);
      *yMin = (std::min)(y0, y1);
      *yMax = (std::max)(y0, y1);
      return true;
    }
    Path outline;
    if (varies) appendGlyph(glyph, outline, *location);
    else appendGlyph(glyph, outline);
    const std::vector<Point>& points = outline.points();
    if (points.empty()) return true;
    float left = points[0].x, right = left, top = points[0].y, bottom = top;
    for (const Point& q : points) {
      left = (std::min)(left, q.x);
      right = (std::max)(right, q.x);
      top = (std::min)(top, q.y);
      bottom = (std::max)(bottom, q.y);
    }
    // The outline is drawn y down; the box is y up.
    *xMin = (int) std::lround(left);
    *xMax = (int) std::lround(right);
    *yMin = (int) std::lround(-bottom);
    *yMax = (int) std::lround(-top);
    return true;
  }

  // ── Variable fonts ─────────────────────────────────────────────────────────
  //
  // One file holding a continuum of designs: Bahnschrift's weight and width,
  // Segoe UI Variable's weight and optical size. The outlines are the default
  // design plus deltas (gvar) that each apply in part of the design space;
  // advances move by HVAR. CFF2 -- variable CFF outlines -- is not read: such
  // a font is refused, as it was.

  /** The font has axes and the deltas to move along them. */
  bool isVariable() const { return ok_ && !axes_.empty() && gvarOffset_ != 0; }

  /** The fvar axes in the font's own order -- the order locationFor()'s
   *  result follows. Empty for a static font, or one whose fvar this cannot
   *  read. A font with axes but no gvar lists them while isVariable() is
   *  false. */
  const std::vector<FontAxis>& variationAxes() const { return axes_; }

  /** 'wght' as the four bytes a font stores it as. */
  static uint32_t tagOf(const char* four) {
    uint32_t t = 0;
    bool ended = four == nullptr;
    for (int i = 0; i < 4; ++i) {
      if (!ended && four[i] == '\0') ended = true;
      t = (t << 8) | (uint8_t) (ended ? ' ' : four[i]); // a short tag is padded, as fonts pad it
    }
    return t;
  }

  /**
   * Where axis values put you in the design space, as the font's own tables
   * are written: per axis, -1 at its minimum, 0 at its default and 1 at its
   * maximum, in 1/16384ths, mapped through the font's avar. An axis not
   * named keeps its default, and a value past an end is held at it. Empty --
   * the default design -- when nothing moves.
   */
  std::vector<int16_t> locationFor(const std::vector<std::pair<uint32_t, float>>& settings) const {
    std::vector<int16_t> out(axes_.size(), 0);
    bool moved = false;
    for (size_t i = 0; i < axes_.size(); ++i) {
      const FontAxis& a = axes_[i];
      float v = a.defaultValue;
      for (const auto& setting : settings)
        if (setting.first == a.tag && setting.second == setting.second) v = setting.second;
      v = v < a.minimum ? a.minimum : (v > a.maximum ? a.maximum : v);
      double n = 0.0;
      if (v < a.defaultValue && a.minimum < a.defaultValue)
        n = ((double) v - a.defaultValue) / ((double) a.defaultValue - a.minimum);
      else if (v > a.defaultValue && a.maximum > a.defaultValue)
        n = ((double) v - a.defaultValue) / ((double) a.maximum - a.defaultValue);
      // Rounded to the tables' precision before avar and after it, as the
      // specification says, so every renderer lands on the same deltas.
      int q = toF2Dot14(n);
      if (i < avar_.size() && !avar_[i].empty()) q = toF2Dot14(avarMap(avar_[i], q / 16384.0));
      out[i] = (int16_t) q;
      if (q != 0) moved = true;
    }
    if (!moved) out.clear();
    return out;
  }

  /** The advance at a location, in design units: through HVAR where the font
   *  has it, from the glyph's varied phantom points where it does not. */
  float advanceWidth(uint16_t glyph, const std::vector<int16_t>& location) const {
    const int base = advanceWidth(glyph);
    if (location.empty() || !isVariable() || location.size() != axes_.size()) return (float) base;
    if (hvarOffset_) return (float) base + (float) hvarAdvanceDelta(glyph, location);
    double left = 0.0, right = 0.0;
    if (!variedPhantoms(glyph, location, &left, &right)) return (float) base;
    return (float) std::lround(right - left);
  }

  /** appendGlyph, at a location in the design space (see locationFor). */
  bool appendGlyph(uint16_t glyph, Path& out, const std::vector<int16_t>& location,
                   float xOffset = 0.0f, float yOffset = 0.0f, float scale = 1.0f) const {
    int budget = kMaxGlyphParts;
    const bool varies = !location.empty() && isVariable() && location.size() == axes_.size();
    return appendGlyphImpl(glyph, out, Transform(scale, 0.0f, 0.0f, -scale, xOffset, yOffset),
                           varies ? &location : nullptr, true, 0, &budget);
  }

private:
  static constexpr int kMaxCompositeDepth = 5;
  /** How many glyphs, composite and simple, one top-level glyph may expand
   *  to. The depth cap above does not bound this on its own: it limits how
   *  DEEP a composite goes, not how WIDE, and five levels of a thousand
   *  components each is 10^15 leaf visits -- a font file that hangs the UI
   *  thread for good. A real accented letter is two or three parts. */
  static constexpr int kMaxGlyphParts = 512;

  bool parse() {
    error_.clear();
    kernLookups_.clear();
    if (data_.size() < 12) return fail("file is too small to be a font");

    uint32_t base = 0;
    const uint32_t tag = readU32(0);
    if (tag == 0x74746366u) { // 'ttcf': a collection, take the first face
      if (data_.size() < 16) return fail("truncated font collection");
      base = readU32(12);
      if ((size_t) base + 12u > data_.size()) return fail("collection points outside the file");
    } else if (tag != 0x00010000u && tag != 0x74727565u && tag != 0x4F54544Fu) {
      // 0x4F54544F is 'OTTO': an OpenType font with CFF outlines, read by
      // parseCff below.
      return fail("not a TrueType or OpenType font");
    }

    const uint16_t numTables = readU16(base + 4);
    const uint32_t dir = base + 12;
    directory_ = dir;
    numTables_ = numTables;
    if ((size_t) dir + (size_t) numTables * 16u > data_.size())
      return fail("table directory runs past the end of the file");

    for (uint16_t i = 0; i < numTables; ++i) {
      const uint32_t rec = dir + (uint32_t) i * 16u;
      const uint32_t t = readU32(rec);
      const uint32_t off = readU32(rec + 8);
      const uint32_t len = readU32(rec + 12);
      // Every table is bounds-checked here, once, so nothing downstream has
      // to. A font file is untrusted input: it may have come from a preset
      // somebody downloaded.
      // Never off + len: size_t is 32 bits on a 32-bit build, and the sum
      // of two header fields near 4 GB wraps right past this test.
      if (off > data_.size() || len > data_.size() - off) continue;
      switch (t) {
        case 0x68656164: headOffset_ = off; break; // head
        case 0x6D617870: maxpOffset_ = off; break; // maxp
        case 0x68686561: hheaOffset_ = off; break; // hhea
        case 0x686D7478: hmtxOffset_ = off; break; // hmtx
        case 0x636D6170: cmapOffset_ = off; break; // cmap
        case 0x6C6F6361: locaOffset_ = off; locaLength_ = len; break; // loca
        case 0x676C7966: glyfOffset_ = off; glyfLength_ = len; break; // glyf
        case 0x6B65726E: parseKern(off, len); break;                  // kern
        case 0x43464620: cffOffset_ = off; cffLength_ = len; break;   // 'CFF '
        case 0x47504F53: gposOffset_ = off; gposLength_ = len; break; // GPOS
        case 0x66766172: fvarOffset_ = off; fvarLength_ = len; break; // fvar
        case 0x61766172: avarOffset_ = off; avarLength_ = len; break; // avar
        case 0x67766172: gvarOffset_ = off; gvarLength_ = len; break; // gvar
        case 0x48564152: hvarOffset_ = off; hvarLength_ = len; break; // HVAR
        case 0x434F4C52: colrOffset_ = len >= 14 && readU16(off + 2) ? off : 0; break; // COLR, version 0's layers
        case 0x4350414C: cpalOffset_ = len >= 12 ? off : 0; break;     // CPAL
        case 0x73626978: sbixOffset_ = len >= 8 ? off : 0; sbixLength_ = len; break; // sbix
        case 0x43424C43: cblcOffset_ = len >= 8 ? off : 0; cblcLength_ = len; break; // CBLC
        case 0x43424454: cbdtOffset_ = off; cbdtLength_ = len; break;  // CBDT
        default: break;
      }
    }

    if (!headOffset_ || !maxpOffset_ || !hheaOffset_)
      return fail("a required table is missing (head, maxp or hhea)");
    // Outlines: glyf and loca, or CFF. Both present, glyf is preferred: it is
    // the format this parser has read longest. A font of pictures alone --
    // sbix, or CBDT and CBLC, as colour emoji fonts are -- has none.
    const bool haveGlyf = glyfOffset_ && locaOffset_;
    const bool pictures = sbixOffset_ || (cblcOffset_ && cbdtOffset_);
    if (!haveGlyf && !cffOffset_ && !pictures)
      return fail("no outlines: neither glyf and loca nor CFF, and no pictures");
    if (haveGlyf) cffOffset_ = 0;

    unitsPerEm_ = readU16(headOffset_ + 18);
    if (unitsPerEm_ <= 0) return fail("unitsPerEm is zero");
    indexToLocFormat_ = (int16_t) readU16(headOffset_ + 50);
    numGlyphs_ = readU16(maxpOffset_ + 4);
    ascender_ = (int16_t) readU16(hheaOffset_ + 4);
    descender_ = (int16_t) readU16(hheaOffset_ + 6);
    lineGap_ = (int16_t) readU16(hheaOffset_ + 8);
    numHMetrics_ = readU16(hheaOffset_ + 34);

    if (cmapOffset_) selectCmap();
    if (cffOffset_ && !parseCff(cffOffset_, cffLength_)) return false;
    if (gposOffset_) parseGposKerning(gposOffset_, gposLength_);
    parseVariations();
    return true;
  }

  bool fail(const char* why) {
    error_ = why;
    return false;
  }

  /** Prefer a full-Unicode subtable, fall back to the Basic Multilingual
   *  Plane, and take a Windows symbol table only if there is nothing else. */
  void selectCmap() {
    const uint16_t tables = readU16(cmapOffset_ + 2);
    uint32_t best = 0;
    int bestScore = -1;
    for (uint16_t i = 0; i < tables; ++i) {
      const uint32_t rec = cmapOffset_ + 4u + (uint32_t) i * 8u;
      if ((size_t) rec + 8u > data_.size()) break;
      const uint16_t platform = readU16(rec);
      const uint16_t encoding = readU16(rec + 2);
      const uint32_t sub = cmapOffset_ + readU32(rec + 4);
      if ((size_t) sub + 4u > data_.size()) continue;
      const uint16_t format = readU16(sub);
      if (platform == 0 && encoding == 5 && format == 14 && !uvsOffset_) uvsOffset_ = sub;
      if (format != 4 && format != 12) continue;

      int score = 0;
      if (platform == 3 && encoding == 10) score = 4;      // Windows UCS-4
      else if (platform == 0) score = 3;                    // Unicode
      else if (platform == 3 && encoding == 1) score = 2;   // Windows BMP
      else if (platform == 3 && encoding == 0) score = 1;   // Windows symbol
      if (score > bestScore) {
        bestScore = score;
        best = sub;
        cmapFormat_ = format;
      }
    }
    cmapOffset_ = best;
  }

  uint16_t lookupFormat4(uint32_t cp) const {
    if (cp > 0xFFFF) return 0;
    const uint16_t segX2 = readU16(cmapOffset_ + 6);
    if (segX2 == 0) return 0;
    const uint32_t ends = cmapOffset_ + 14;
    const uint32_t starts = ends + segX2 + 2;
    const uint32_t deltas = starts + segX2;
    const uint32_t ranges = deltas + segX2;

    for (uint32_t i = 0; i < (uint32_t) segX2; i += 2) {
      if (readU16(ends + i) < cp) continue;
      if (readU16(starts + i) > cp) return 0;
      const uint16_t rangeOffset = readU16(ranges + i);
      if (rangeOffset == 0)
        return (uint16_t) ((cp + readU16(deltas + i)) & 0xFFFF);
      // The glyph id array is addressed RELATIVE to the range offset's own
      // location -- one of the strangest pieces of the format, and reading it
      // as an absolute offset gives a plausible wrong glyph rather than an
      // error.
      const uint32_t at = ranges + i + rangeOffset + (cp - readU16(starts + i)) * 2u;
      if ((size_t) at + 2u > data_.size()) return 0;
      const uint16_t g = readU16(at);
      return g == 0 ? 0 : (uint16_t) ((g + readU16(deltas + i)) & 0xFFFF);
    }
    return 0;
  }

  /** A colour of palette `palette` (or the first), from CPAL; false where
   *  the palette has no such entry -- 0xFFFF, the text's colour, among them. */
  bool paletteColour(int palette, uint16_t index, ColourLayer* layer) const {
    if (index == 0xFFFF || cpalOffset_ == 0) return false;
    const uint16_t entries = readU16(cpalOffset_ + 2), palettes = readU16(cpalOffset_ + 4);
    const uint16_t records = readU16(cpalOffset_ + 6);
    if (palettes == 0 || index >= entries) return false;
    const uint32_t which = palette > 0 && palette < (int) palettes ? (uint32_t) palette : 0u;
    const uint32_t record = (uint32_t) readU16(cpalOffset_ + 12u + which * 2u) + index;
    if (record >= records) return false;
    const uint32_t at = cpalOffset_ + readU32(cpalOffset_ + 8) + record * 4u;
    if ((size_t) at + 4u > data_.size()) return false;
    // Blue, green, red, alpha.
    layer->blue = data_[at];
    layer->green = data_[at + 1u];
    layer->red = data_[at + 2u];
    layer->alpha = data_[at + 3u];
    return true;
  }

  /** HarfBuzz's choice among strikes of these sizes: the smallest at least
   *  the size wanted, or else the largest. */
  template <typename SizeOf>
  static uint32_t chooseStrike(uint32_t count, uint32_t wanted, SizeOf sizeOf, uint32_t* ppem) {
    uint32_t best = 0, bestPpem = sizeOf(0u);
    for (uint32_t i = 1; i < count; ++i) {
      const uint32_t size = sizeOf(i);
      if ((wanted <= size && size < bestPpem) || (wanted > bestPpem && size > bestPpem)) {
        best = i;
        bestPpem = size;
      }
    }
    *ppem = bestPpem;
    return best;
  }

  /** Where a PNG's picture goes, from its header: false for bytes that are
   *  not one. */
  static bool pngSize(const uint8_t* png, size_t size, uint32_t* width, uint32_t* height) {
    static const uint8_t kSignature[12] = {137, 80, 78, 71, 13, 10, 26, 10, 0, 0, 0, 13};
    if (size < 24 || std::memcmp(png, kSignature, 12) != 0 || std::memcmp(png + 12, "IHDR", 4) != 0) return false;
    *width = ((uint32_t) png[16] << 24) | ((uint32_t) png[17] << 16) | ((uint32_t) png[18] << 8) | png[19];
    *height = ((uint32_t) png[20] << 24) | ((uint32_t) png[21] << 16) | ((uint32_t) png[22] << 8) | png[23];
    return *width > 0 && *height > 0 && *width < 65536 && *height < 65536;
  }

  /** Whether a picture's size fits its strike: an emoji is about an em
   *  square. A few kilobytes of PNG can claim 65,535 pixels a side, and
   *  decoded that is gigabytes for one glyph. */
  static bool plausibleSize(uint32_t width, uint32_t height, uint32_t ppem) {
    const uint32_t most = 4u * ppem + 64u;
    return width <= most && height <= most;
  }

  /** sbix: per strike, an offset for each glyph's data -- its origin, a
   *  four-byte type, and the picture, or for 'dupe' another glyph's id. */
  bool sbixImage(uint16_t glyph, uint32_t wanted, ImageLayer* out) const {
    const uint32_t end = sbixOffset_ + sbixLength_;
    const uint32_t count = (std::min)(readU32(sbixOffset_ + 4), sbixLength_ >= 8 ? (sbixLength_ - 8u) / 4u : 0u);
    if (count == 0) return false;
    auto strikeAt = [&](uint32_t i) { return sbixOffset_ + readU32(sbixOffset_ + 8u + i * 4u); };
    uint32_t ppem = 0;
    const uint32_t strike = strikeAt(chooseStrike(count, wanted, [&](uint32_t i) { return (uint32_t) readU16(strikeAt(i)); }, &ppem));
    if (ppem == 0) return false;
    for (int hops = 0; hops <= 8; ++hops) {
      if (glyph >= numGlyphs_) return false;
      const uint32_t from = readU32(strike + 4u + glyph * 4u), to = readU32(strike + 8u + glyph * 4u);
      if (to <= from || to - from <= 8u) return false;
      const uint32_t at = strike + from, length = to - from;
      if (at < strike || at > end || length > end - at) return false;
      if (readU32(at + 4u) == 0x64757065) { // 'dupe': the picture of the glyph it names
        if (length < 10u) return false;
        glyph = readU16(at + 8u);
        continue;
      }
      if (readU32(at + 4u) != 0x706E6720) return false; // 'png '
      uint32_t width = 0, height = 0;
      if (!pngSize(data_.data() + at + 8u, length - 8u, &width, &height)) return false;
      if (!plausibleSize(width, height, ppem)) return false;
      out->png = data_.data() + at + 8u;
      out->size = length - 8u;
      out->ppem = (int) ppem;
      out->left = (int16_t) readU16(at);
      out->top = -((int) height + (int16_t) readU16(at + 2u));
      return true;
    }
    return false;
  }

  /** CBLC's strikes, each a list of glyph ranges whose index subtables (of
   *  format 1 or 3, the ones HarfBuzz reads) give where in CBDT a glyph's
   *  picture is, in format 17 or 18 -- its metrics, then its PNG. */
  bool cbdtImage(uint16_t glyph, uint32_t wanted, ImageLayer* out) const {
    const uint32_t count = (std::min)(readU32(cblcOffset_ + 4), cblcLength_ >= 8 ? (cblcLength_ - 8u) / 48u : 0u);
    if (count == 0) return false;
    auto sizeOf = [&](uint32_t i) {
      const uint32_t at = cblcOffset_ + 8u + i * 48u;
      return (uint32_t) (std::max)(data_[at + 44u], data_[at + 45u]);
    };
    uint32_t ppem = 0;
    const uint32_t size = cblcOffset_ + 8u + chooseStrike(count, wanted, sizeOf, &ppem) * 48u;
    if (data_[size + 44u] == 0 || data_[size + 45u] == 0) return false;
    const uint32_t array = cblcOffset_ + readU32(size);
    const uint32_t tables = (std::min)(readU32(size + 8u), cblcLength_ / 8u);
    for (uint32_t k = 0; k < tables; ++k) {
      const uint32_t record = array + k * 8u;
      const uint16_t first = readU16(record), last = readU16(record + 2u);
      if (glyph < first || glyph > last) continue;
      const uint32_t sub = array + readU32(record + 4u);
      const uint16_t indexFormat = readU16(sub), imageFormat = readU16(sub + 2u);
      const uint32_t index = (uint32_t) (glyph - first);
      uint32_t from = 0, to = 0;
      if (indexFormat == 1) {
        from = readU32(sub + 8u + index * 4u);
        to = readU32(sub + 12u + index * 4u);
      } else if (indexFormat == 3) {
        from = readU16(sub + 8u + index * 2u);
        to = readU16(sub + 10u + index * 2u);
      } else {
        return false;
      }
      if (to <= from) return false;
      const uint32_t offset = readU32(sub + 4u) + from, length = to - from;
      if (offset > cbdtLength_ || length > cbdtLength_ - offset) return false;
      const uint32_t at = cbdtOffset_ + offset;
      // Small metrics are five bytes, big ones eight; both begin height,
      // width, bearing x, bearing y.
      const uint32_t metrics = imageFormat == 17 ? 5u : imageFormat == 18 ? 8u : 0u;
      if (metrics == 0 || length < metrics + 4u) return false;
      const uint32_t pngLength = readU32(at + metrics);
      if (pngLength > length - metrics - 4u) return false;
      uint32_t width = 0, height = 0;
      if (!pngSize(data_.data() + at + metrics + 4u, pngLength, &width, &height)) return false;
      if (!plausibleSize(width, height, ppem)) return false;
      out->png = data_.data() + at + metrics + 4u;
      out->size = pngLength;
      out->ppem = (int) ppem;
      out->left = (int8_t) data_[at + 2u];
      out->top = -(int) (int8_t) data_[at + 3u];
      return true;
    }
    return false;
  }

  /** A binary search of the sorted array of `size`-byte entries counted at
   *  `count`, keyed by the 24 bits each begins with -- or, with `extra`, by
   *  the range from them to them plus the byte after. The entry's offset,
   *  or 0. */
  uint32_t uvsSearch(uint32_t count, uint32_t size, uint32_t key, int extra) const {
    const size_t first = (size_t) count + 4u;
    if (first > data_.size()) return 0;
    uint32_t lo = 0, hi = (uint32_t) (std::min)((size_t) readU32(count), (data_.size() - first) / size);
    while (lo < hi) {
      const uint32_t mid = lo + (hi - lo) / 2;
      const uint32_t at = (uint32_t) first + mid * size;
      const uint32_t start = ((uint32_t) readU16(at) << 8) | data_[at + 2u];
      const uint32_t end = start + (extra ? data_[at + 3u] : 0u);
      if (key < start) hi = mid;
      else if (key > end) lo = mid + 1;
      else return at;
    }
    return 0;
  }

  uint16_t lookupFormat12(uint32_t cp) const {
    const uint32_t groups = readU32(cmapOffset_ + 12);
    uint32_t lo = 0, hi = groups;
    while (lo < hi) {
      const uint32_t mid = (lo + hi) / 2;
      const uint32_t at = cmapOffset_ + 16u + mid * 12u;
      if ((size_t) at + 12u > data_.size()) return 0;
      const uint32_t first = readU32(at), last = readU32(at + 4);
      if (cp < first) hi = mid;
      else if (cp > last) lo = mid + 1;
      else return (uint16_t) (readU32(at + 8) + (cp - first));
    }
    return 0;
  }

  // ── GPOS kerning ──────────────────────────────────────────────────────────
  //
  // The pair adjustments the 'kern' feature names: lookups of type 2, or of
  // type 9 wrapping type 2, with their subtables in format 1 (pairs listed
  // per first glyph) or format 2 (a matrix of glyph classes). Only the first
  // glyph's x advance is read, which is what kerning is; no script or
  // language is chosen -- every 'kern' feature's lookups are taken, in the
  // order the lookup list gives them, which is the order they apply in.

  bool inGpos(uint32_t at, uint32_t bytes) const {
    return at >= gposOffset_ && (size_t) at + bytes <= (size_t) gposOffset_ + gposLength_;
  }

  void parseGposKerning(uint32_t off, uint32_t len) {
    gposLength_ = len;
    if (len < 10 || readU16(off) != 1) return;
    const uint32_t features = off + readU16(off + 6);
    const uint32_t lookups = off + readU16(off + 8);
    if (!inGpos(features, 2) || !inGpos(lookups, 2)) return;
    std::vector<uint16_t> wanted;
    const uint16_t featureCount = readU16(features);
    for (uint32_t i = 0; i < featureCount; ++i) {
      const uint32_t record = features + 2u + i * 6u;
      if (!inGpos(record, 6)) return;
      if (readU32(record) != 0x6B65726Eu) continue; // 'kern'
      const uint32_t feature = features + readU16(record + 4);
      if (!inGpos(feature, 4)) continue;
      const uint16_t count = readU16(feature + 2);
      for (uint32_t k = 0; k < count && wanted.size() < 256; ++k)
        if (inGpos(feature + 4u + k * 2u, 2)) wanted.push_back(readU16(feature + 4u + k * 2u));
    }
    std::sort(wanted.begin(), wanted.end());
    wanted.erase(std::unique(wanted.begin(), wanted.end()), wanted.end());
    const uint16_t lookupCount = readU16(lookups);
    size_t subtables = 0;
    for (const uint16_t index : wanted) {
      if (index >= lookupCount || !inGpos(lookups + 2u + index * 2u, 2)) continue;
      const uint32_t lookup = lookups + readU16(lookups + 2u + index * 2u);
      if (!inGpos(lookup, 6)) continue;
      const uint16_t type = readU16(lookup);
      const uint16_t count = readU16(lookup + 4);
      std::vector<uint32_t> found;
      for (uint32_t k = 0; k < count && subtables < 1024; ++k) {
        if (!inGpos(lookup + 6u + k * 2u, 2)) break;
        uint32_t subtable = lookup + readU16(lookup + 6u + k * 2u);
        if (type == 9) { // Extension: the real subtable at a 32-bit offset
          if (!inGpos(subtable, 8) || readU16(subtable) != 1 || readU16(subtable + 2) != 2) continue;
          const uint32_t offset = readU32(subtable + 4);
          if (offset > len) continue;
          subtable += offset;
        } else if (type != 2) {
          break;
        }
        const uint16_t format = readU16(subtable);
        if (!inGpos(subtable, 16) || (format != 1 && format != 2)) continue;
        // The same subtable listed again answers the same: thousands of
        // entries pointing at one large one cost every pair thousands of
        // searches -- 216 us a pair from a 75 KB table.
        if (std::find(found.begin(), found.end(), subtable) != found.end()) continue;
        found.push_back(subtable);
        ++subtables;
      }
      if (!found.empty()) kernLookups_.push_back(std::move(found));
    }
  }

  /** The coverage index of `glyph`, or -1. */
  int coverageIndex(uint32_t table, uint16_t glyph) const {
    if (!inGpos(table, 4)) return -1;
    const uint16_t format = readU16(table), count = readU16(table + 2);
    // Every array within GPOS, not merely within the file: a record read
    // from the next table is a kerning of -30000.
    if (!inGpos(table, 4u + (uint32_t) count * (format == 1 ? 2u : 6u))) return -1;
    if (format == 1) {
      uint32_t lo = 0, hi = count;
      while (lo < hi) {
        const uint32_t mid = (lo + hi) / 2;
        const uint16_t g = readU16(table + 4u + mid * 2u);
        if (g == glyph) return (int) mid;
        if (g < glyph) lo = mid + 1;
        else hi = mid;
      }
    } else if (format == 2) {
      uint32_t lo = 0, hi = count;
      while (lo < hi) {
        const uint32_t mid = (lo + hi) / 2;
        const uint32_t range = table + 4u + mid * 6u;
        const uint16_t first = readU16(range), last = readU16(range + 2);
        if (glyph < first) hi = mid;
        else if (glyph > last) lo = mid + 1;
        else return (int) readU16(range + 4) + (glyph - first);
      }
    }
    return -1;
  }

  /** The class `glyph` is in, 0 where none says. */
  uint16_t classOf(uint32_t table, uint16_t glyph) const {
    if (!inGpos(table, 4)) return 0;
    const uint16_t format = readU16(table);
    if (format == 1) {
      const uint16_t first = readU16(table + 2), count = readU16(table + 4);
      if (!inGpos(table, 6u + (uint32_t) count * 2u)) return 0;
      if (glyph < first || glyph - first >= count) return 0;
      return readU16(table + 6u + (uint32_t) (glyph - first) * 2u);
    }
    if (format == 2) {
      uint32_t lo = 0, hi = readU16(table + 2);
      if (!inGpos(table, 4u + hi * 6u)) return 0;
      while (lo < hi) {
        const uint32_t mid = (lo + hi) / 2;
        const uint32_t range = table + 4u + mid * 6u;
        const uint16_t first = readU16(range), last = readU16(range + 2);
        if (glyph < first) hi = mid;
        else if (glyph > last) lo = mid + 1;
        else return readU16(range + 4);
      }
    }
    return 0;
  }

  static uint32_t valueSize(uint16_t format) {
    uint32_t bits = 0;
    for (int b = 0; b < 8; ++b) bits += (format >> b) & 1u;
    return bits * 2u;
  }

  /** A value record's x advance: after its x and y placement, if it has them. */
  int16_t xAdvance(uint32_t record, uint16_t format) const {
    if (!(format & 0x0004)) return 0;
    uint32_t at = record;
    if (format & 0x0001) at += 2;
    if (format & 0x0002) at += 2;
    return (int16_t) readU16(at);
  }

  /** The last Coverage table searched for the left glyph, and the answer:
   *  subtables that share one -- a class-kerned font split across many --
   *  search it once per pair rather than once per subtable. */
  struct CoverageMemo {
    uint32_t table = 0;
    int index = -1;
  };

  /** What one PairPos subtable says about (left, right), if it covers left. */
  bool pairAdjustment(uint32_t subtable, uint16_t left, uint16_t right, int* value,
                      CoverageMemo& memo) const {
    const uint32_t coverage = subtable + readU16(subtable + 2);
    if (coverage != memo.table) {
      memo.table = coverage;
      memo.index = coverageIndex(coverage, left);
    }
    const int index = memo.index;
    if (index < 0) return false;
    const uint16_t format = readU16(subtable);
    const uint16_t format1 = readU16(subtable + 4), format2 = readU16(subtable + 6);
    const uint32_t size1 = valueSize(format1), size2 = valueSize(format2);
    if (format == 1) {
      if ((uint32_t) index >= readU16(subtable + 8)) return false;
      if (!inGpos(subtable + 10u + (uint32_t) index * 2u, 2)) return false;
      const uint32_t set = subtable + readU16(subtable + 10u + (uint32_t) index * 2u);
      if (!inGpos(set, 2)) return false;
      const uint32_t stride = 2u + size1 + size2;
      uint32_t lo = 0, hi = readU16(set);
      if (!inGpos(set + 2u, hi * stride)) return false;
      while (lo < hi) {
        const uint32_t mid = (lo + hi) / 2;
        const uint32_t record = set + 2u + mid * stride;
        const uint16_t second = readU16(record);
        if (second == right) {
          *value = xAdvance(record + 2u, format1);
          return true;
        }
        if (second < right) lo = mid + 1;
        else hi = mid;
      }
      return false;
    }
    const uint16_t class1 = classOf(subtable + readU16(subtable + 8), left);
    const uint16_t class2 = classOf(subtable + readU16(subtable + 10), right);
    const uint16_t class1Count = readU16(subtable + 12), class2Count = readU16(subtable + 14);
    if (class1 >= class1Count || class2 >= class2Count) return false;
    const uint32_t record =
        subtable + 16u + ((uint32_t) class1 * class2Count + class2) * (size1 + size2);
    if (!inGpos(record, size1 > 0 ? size1 : 1)) return false;
    *value = xAdvance(record, format1);
    return true;
  }

  void parseKern(uint32_t off, uint32_t len) {
    if (len < 18) return;
    // Only the first subtable, and only format 0 horizontal. Anything else is
    // rare enough that guessing is worse than reporting no kerning.
    const uint16_t coverage = readU16(off + 8);
    if ((coverage & 0x0001) == 0) return; // not horizontal
    if (((coverage >> 8) & 0xFF) != 0) return; // not format 0
    kernPairs_ = readU16(off + 10);
    // After nPairs come searchRange, entrySelector and rangeShift: the pairs
    // start at 18. Read from 14, every pair was three fields off -- the
    // search key made of one pair's right glyph and the next pair's left,
    // the value out of the next key -- and a font that kerns only here got
    // pairs it never had: Tahoma, Microsoft YaHei, Malgun Gothic, hundreds
    // of pairs each against fontTools' reading.
    kernOffset_ = off + 18;
    if ((size_t) kernOffset_ + (size_t) kernPairs_ * 6u > data_.size()) {
      kernOffset_ = 0;
      kernPairs_ = 0;
    }
  }

  uint32_t glyphOffset(uint16_t glyph, uint32_t* lengthOut) const {
    if (glyph >= numGlyphs_) return 0;
    uint32_t start, end;
    if (indexToLocFormat_ == 0) {
      const uint32_t at = locaOffset_ + (uint32_t) glyph * 2u;
      if ((size_t) at + 4u > data_.size()) return 0;
      start = (uint32_t) readU16(at) * 2u;
      end = (uint32_t) readU16(at + 2) * 2u;
    } else {
      const uint32_t at = locaOffset_ + (uint32_t) glyph * 4u;
      if ((size_t) at + 8u > data_.size()) return 0;
      start = readU32(at);
      end = readU32(at + 4);
    }
    // start == end means an EMPTY glyph, which is how a space is stored. Not
    // an error, and treating it as one puts a .notdef box where a space
    // belongs.
    if (end <= start || end > glyfLength_) return 0;
    if (lengthOut) *lengthOut = end - start;
    return glyfOffset_ + start;
  }

  /** The outline points of a simple glyph: flags, coordinates and where its
   *  contours end. False for data that does not hold together. */
  bool readSimpleGlyph(uint32_t at, std::vector<uint8_t>& flags, std::vector<int>& xs,
                       std::vector<int>& ys, std::vector<uint32_t>& ends) const {
    const int16_t contours = (int16_t) readU16(at);
    const uint32_t endPts = at + 10;
    const uint32_t n = (uint32_t) contours;
    if (contours <= 0) return false;
    if ((size_t) endPts + n * 2u + 2u > data_.size()) return false;
    const uint32_t numPoints = (uint32_t) readU16(endPts + (n - 1) * 2u) + 1u;
    if (numPoints > 10000u) return false; // implausible; refuse rather than allocate

    const uint32_t instrLen = readU16(endPts + n * 2u);
    uint32_t p = endPts + n * 2u + 2u + instrLen;

    // Flags are run-length encoded: a REPEAT bit means the next byte says how
    // many more points share these flags.
    flags.clear();
    flags.reserve(numPoints);
    while (flags.size() < numPoints) {
      if ((size_t) p >= data_.size()) return false;
      const uint8_t f = data_[p++];
      flags.push_back(f);
      if (f & 0x08) {
        if ((size_t) p >= data_.size()) return false;
        uint8_t repeat = data_[p++];
        while (repeat-- > 0 && flags.size() < numPoints) flags.push_back(f);
      }
    }

    // Coordinates are DELTAS, and each axis is either a byte or a short
    // depending on two flag bits. Reading them as absolute values gives a
    // glyph that explodes across the page.
    xs.assign(numPoints, 0);
    ys.assign(numPoints, 0);
    int v = 0;
    for (uint32_t i = 0; i < numPoints; ++i) {
      const uint8_t f = flags[i];
      if (f & 0x02) {
        if ((size_t) p >= data_.size()) return false;
        const int d = data_[p++];
        v += (f & 0x10) ? d : -d;
      } else if (!(f & 0x10)) {
        if ((size_t) p + 2u > data_.size()) return false;
        v += (int16_t) readU16(p);
        p += 2;
      }
      xs[i] = v;
    }
    v = 0;
    for (uint32_t i = 0; i < numPoints; ++i) {
      const uint8_t f = flags[i];
      if (f & 0x04) {
        if ((size_t) p >= data_.size()) return false;
        const int d = data_[p++];
        v += (f & 0x20) ? d : -d;
      } else if (!(f & 0x20)) {
        if ((size_t) p + 2u > data_.size()) return false;
        v += (int16_t) readU16(p);
        p += 2;
      }
      ys[i] = v;
    }

    ends.clear();
    uint32_t first = 0;
    for (uint32_t c = 0; c < n; ++c) {
      const uint32_t last = readU16(endPts + c * 2u);
      if (last < first || last >= numPoints) return false;
      ends.push_back(last);
      first = last + 1;
    }
    return true;
  }

  /** `m` takes the glyph's own coordinates -- y up -- to the output. A
   *  location, when there is one, moves the points first. Only the glyph
   *  drawn at the top is placed by its varied left phantom point; a
   *  component is placed by its parent. */
  bool appendGlyphImpl(uint16_t glyph, Path& out, const Transform& m,
                       const std::vector<int16_t>* location, bool top, int depth,
                       int* budget) const {
    if (!ok_ || depth > kMaxCompositeDepth) return false;
    if (--*budget < 0) return false;
    if (cffOffset_) return appendCffGlyph(glyph, out, m.e, m.f, m.a);
    uint32_t length = 0;
    const uint32_t at = glyphOffset(glyph, &length);
    if (at == 0) return true; // empty glyph, e.g. a space

    const int16_t contours = (int16_t) readU16(at);
    if (contours < 0)
      return appendComposite(glyph, at, at + length, out, m, location, depth, budget);
    if (contours == 0) return true; // a header and no outline: nothing to draw

    std::vector<uint8_t> flags;
    std::vector<int> xs, ys;
    std::vector<uint32_t> ends;
    if (!readSimpleGlyph(at, flags, xs, ys, ends)) return false;
    // The glyph's origin is its left phantom point, xMin less the left side
    // bearing hmtx gives it: almost always 0, but a font may set a bearing
    // that is not its outline's xMin, and every rasteriser then places the
    // outline by the bearing. Times New Roman's box-drawing glyphs sat 190
    // units left of where they belong. Only the glyph drawn at the top is
    // placed so; a component is placed by its parent.
    const double left = (double) ((int16_t) readU16(at + 2) - leftSideBearing(glyph));
    const double place = top ? left : 0.0;
    std::vector<float> fx(xs.size()), fy(ys.size());
    for (size_t i = 0; i < xs.size(); ++i) {
      fx[i] = (float) (xs[i] - place);
      fy[i] = (float) ys[i];
    }

    if (location) {
      // The points, then the four phantom points that carry the metrics --
      // the left side, the advance, and two vertical ones nothing here reads.
      const size_t n = xs.size();
      std::vector<double> X(xs.begin(), xs.end()), Y(ys.begin(), ys.end());
      X.insert(X.end(), {left, left + advanceWidth(glyph), 0.0, 0.0});
      Y.insert(Y.end(), {0.0, 0.0, 0.0, 0.0});
      applyVariations(glyph, *location, X, Y, ends);
      // Placed by its VARIED left side, as the advance is measured from it.
      const double shift = top ? X[n] : 0.0;
      for (size_t i = 0; i < n; ++i) {
        fx[i] = (float) (X[i] - shift);
        fy[i] = (float) Y[i];
      }
    }

    uint32_t first = 0;
    for (const uint32_t last : ends) {
      emitContour(out, flags, fx, fy, first, last, m);
      first = last + 1;
    }
    return true;
  }

  /** hmtx's left side bearing: after the long metrics, a bearing per glyph
   *  that shares the last advance. */
  int leftSideBearing(uint16_t glyph) const {
    if (hmtxOffset_ == 0 || numHMetrics_ == 0) return 0;
    if (glyph < numHMetrics_) return (int16_t) readU16(hmtxOffset_ + (uint32_t) glyph * 4u + 2u);
    return (int16_t) readU16(hmtxOffset_ + (uint32_t) numHMetrics_ * 4u +
                             (uint32_t) (glyph - numHMetrics_) * 2u);
  }

  /**
   * One contour of a TrueType glyph.
   *
   * The awkward part of the format: points are on-curve or off-curve, and TWO
   * CONSECUTIVE OFF-CURVE POINTS IMPLY AN ON-CURVE POINT HALFWAY BETWEEN
   * THEM. A parser that misses that draws every rounded letter with corners.
   * A contour may also begin off-curve, in which case the start is that
   * implied midpoint.
   */
  void emitContour(Path& out, const std::vector<uint8_t>& flags, const std::vector<float>& xs,
                   const std::vector<float>& ys, uint32_t first, uint32_t last,
                   const Transform& m) const {
    const uint32_t count = last - first + 1;
    if (count < 2) return;

    auto onCurve = [&](uint32_t i) { return (flags[first + (i % count)] & 0x01) != 0; };
    // Written as offset plus coordinate times scale when the transform is
    // only that, which is every glyph but a skewed component: the same
    // arithmetic, so the same points, as before transforms were read. y is
    // NEGATED by the transform: TrueType grows upward, every graphics API
    // here grows downward, and doing it once at the source means no caller
    // ever has to.
    const bool plain = m.b == 0.0f && m.c == 0.0f;
    auto px = [&](uint32_t i) {
      const float x = xs[first + (i % count)], y = ys[first + (i % count)];
      return plain ? m.e + x * m.a : m.a * x + m.c * y + m.e;
    };
    auto py = [&](uint32_t i) {
      const float x = xs[first + (i % count)], y = ys[first + (i % count)];
      return plain ? m.f + y * m.d : m.b * x + m.d * y + m.f;
    };

    uint32_t startIndex = 0;
    float sx, sy;
    if (onCurve(0)) {
      sx = px(0);
      sy = py(0);
      startIndex = 1;
    } else if (onCurve(count - 1)) {
      sx = px(count - 1);
      sy = py(count - 1);
      startIndex = 0;
    } else {
      // Every point is off-curve: start at the implied midpoint.
      sx = (px(0) + px(count - 1)) * 0.5f;
      sy = (py(0) + py(count - 1)) * 0.5f;
      startIndex = 0;
    }
    out.moveTo(sx, sy);

    float cx = 0.0f, cy = 0.0f;
    bool haveControl = false;
    for (uint32_t k = 0; k < count; ++k) {
      const uint32_t i = startIndex + k;
      const float x = px(i), y = py(i);
      if (onCurve(i)) {
        if (haveControl) {
          out.quadTo(cx, cy, x, y);
          haveControl = false;
        } else {
          out.lineTo(x, y);
        }
      } else {
        if (haveControl) {
          // Two off-curve in a row: the implied on-curve point is halfway.
          out.quadTo(cx, cy, (cx + x) * 0.5f, (cy + y) * 0.5f);
        }
        cx = x;
        cy = y;
        haveControl = true;
      }
    }
    if (haveControl) out.quadTo(cx, cy, sx, sy);
    out.close();
  }

  /** One part of a composite glyph: which glyph, where, and how transformed. */
  struct Component {
    uint16_t flags = 0, index = 0;
    double dx = 0.0, dy = 0.0;
    float a = 1.0f, b = 0.0f, c = 0.0f, d = 1.0f;
  };

  static float f2dot14(uint16_t v) { return (float) (int16_t) v / 16384.0f; }

  /** The component records from `at` to `end` -- where THIS glyph's bytes
   *  stop. They were once read to the end of the FILE, so a composite with no
   *  last-component flag walked on through every glyph after it. */
  bool readComponents(uint32_t at, uint32_t end, std::vector<Component>& parts) const {
    if ((size_t) end > data_.size()) return false;
    parts.clear();
    uint32_t p = at + 10;
    for (;;) {
      if (p + 4u > end || parts.size() >= (size_t) kMaxGlyphParts) return false;
      Component part;
      part.flags = readU16(p);
      part.index = readU16(p + 2);
      p += 4;
      if (part.flags & 0x0001) { // ARG_1_AND_2_ARE_WORDS
        if (p + 4u > end) return false;
        part.dx = (int16_t) readU16(p);
        part.dy = (int16_t) readU16(p + 2);
        p += 4;
      } else {
        if (p + 2u > end) return false;
        part.dx = (int8_t) data_[p];
        part.dy = (int8_t) data_[p + 1];
        p += 2;
      }
      // A component may be scaled, scaled unevenly, or put through a 2x2:
      // small capitals made from capitals, a mirrored bracket.
      if (part.flags & 0x0008) { // WE_HAVE_A_SCALE
        if (p + 2u > end) return false;
        part.a = part.d = f2dot14(readU16(p));
        p += 2;
      } else if (part.flags & 0x0040) { // WE_HAVE_AN_X_AND_Y_SCALE
        if (p + 4u > end) return false;
        part.a = f2dot14(readU16(p));
        part.d = f2dot14(readU16(p + 2));
        p += 4;
      } else if (part.flags & 0x0080) { // WE_HAVE_A_TWO_BY_TWO
        if (p + 8u > end) return false;
        part.a = f2dot14(readU16(p));
        part.b = f2dot14(readU16(p + 2));
        part.c = f2dot14(readU16(p + 4));
        part.d = f2dot14(readU16(p + 6));
        p += 8;
      }
      parts.push_back(part);
      if (!(part.flags & 0x0020)) break; // MORE_COMPONENTS
    }
    return true;
  }

  bool appendComposite(uint16_t glyph, uint32_t at, uint32_t end, Path& out, const Transform& m,
                       const std::vector<int16_t>* location, int depth, int* budget) const {
    std::vector<Component> parts;
    if (!readComponents(at, end, parts)) return false;

    // Placed by its components alone. A composite's own bearing is not its
    // origin: an accented capital whose accent reaches left of the letter
    // has an xMin left of the bearing, and the letter's metrics -- which the
    // composite takes with USE_MY_METRICS -- place it. Moved by its own
    // left side, Arial's I-grave sat 33 units right of the I-grave anyone
    // else draws.
    if (location) {
      // A composite's points are its components' offsets, then the four
      // phantom points; gvar moves them like any other glyph's.
      const size_t n = parts.size();
      const double left = (double) ((int16_t) readU16(at + 2) - leftSideBearing(glyph));
      std::vector<double> X, Y;
      std::vector<uint32_t> ends;
      for (size_t i = 0; i < n; ++i) {
        X.push_back(parts[i].dx);
        Y.push_back(parts[i].dy);
        ends.push_back((uint32_t) i); // each its own contour, for the interpolation
      }
      X.insert(X.end(), {left, left + advanceWidth(glyph), 0.0, 0.0});
      Y.insert(Y.end(), {0.0, 0.0, 0.0, 0.0});
      applyVariations(glyph, *location, X, Y, ends);
      for (size_t i = 0; i < n; ++i)
        if (parts[i].flags & 0x0002) { // ARGS_ARE_XY_VALUES: an offset, which can vary
          parts[i].dx = X[i];
          parts[i].dy = Y[i];
        }
    }

    for (const Component& part : parts) {
      // The offset is in the composite's units, not the component's, unless
      // the font says SCALED_COMPONENT_OFFSET.
      double ox = part.dx, oy = part.dy;
      if (part.flags & 0x0800) {
        ox = part.a * part.dx + part.c * part.dy;
        oy = part.b * part.dx + part.d * part.dy;
      }
      const Transform own(part.a, part.b, part.c, part.d, (float) ox, (float) oy);
      if (!appendGlyphImpl(part.index, out, own.then(m), location, false, depth + 1, budget))
        return false;
    }
    return true;
  }

  // ── gvar, avar, fvar, HVAR ────────────────────────────────────────────────

  static int toF2Dot14(double v) {
    const double r = std::floor(v * 16384.0 + 0.5);
    return (int) (r < -16384.0 ? -16384.0 : (r > 16384.0 ? 16384.0 : r));
  }

  /** avar's piecewise-linear map for one axis, in normalised units. */
  static double avarMap(const std::vector<std::pair<int16_t, int16_t>>& map, double v) {
    const double lo = map.front().first / 16384.0, hi = map.back().first / 16384.0;
    if (v <= lo) return v + map.front().second / 16384.0 - lo;
    if (v >= hi) return v + map.back().second / 16384.0 - hi;
    for (size_t i = 1; i < map.size(); ++i) {
      const double a = map[i - 1].first / 16384.0, b = map[i].first / 16384.0;
      if (v > b) continue;
      if (v == b) return map[i].second / 16384.0;
      const double va = map[i - 1].second / 16384.0, vb = map[i].second / 16384.0;
      return b > a ? va + (vb - va) * (v - a) / (b - a) : vb;
    }
    return v;
  }

  void parseVariations() {
    axes_.clear();
    avar_.clear();
    // fvar: the axes. A table this cannot make sense of leaves the font
    // static -- its default design, which is what every reader without
    // variations has always drawn.
    auto staticFont = [&]() {
      axes_.clear();
      gvarOffset_ = 0;
      hvarOffset_ = 0;
    };
    if (!fvarOffset_ || fvarLength_ < 16 || readU16(fvarOffset_) != 1) return staticFont();
    const uint32_t axesAt = fvarOffset_ + readU16(fvarOffset_ + 4);
    const uint16_t axisCount = readU16(fvarOffset_ + 8);
    const uint16_t axisSize = readU16(fvarOffset_ + 10);
    if (axisCount == 0 || axisCount > 64 || axisSize < 20 ||
        (size_t) axesAt + (size_t) axisCount * axisSize > (size_t) fvarOffset_ + fvarLength_)
      return staticFont();
    for (uint16_t i = 0; i < axisCount; ++i) {
      const uint32_t rec = axesAt + (uint32_t) i * axisSize;
      FontAxis axis;
      axis.tag = readU32(rec);
      axis.minimum = (float) ((int32_t) readU32(rec + 4) / 65536.0);
      axis.defaultValue = (float) ((int32_t) readU32(rec + 8) / 65536.0);
      axis.maximum = (float) ((int32_t) readU32(rec + 12) / 65536.0);
      axis.hidden = (readU16(rec + 16) & 0x0001) != 0;
      if (!(axis.minimum <= axis.defaultValue && axis.defaultValue <= axis.maximum))
        return staticFont();
      axes_.push_back(axis);
    }

    // avar: per axis, a map that bends the normalised scale. Its axis count
    // must be fvar's, and each map runs -1 -> -1, 0 -> 0, 1 -> 1 at least.
    if (avarOffset_ && avarLength_ >= 8 && readU16(avarOffset_) == 1 &&
        readU16(avarOffset_ + 6) == axisCount) {
      uint32_t p = avarOffset_ + 8;
      const uint32_t limit = avarOffset_ + avarLength_;
      avar_.resize(axisCount);
      for (uint16_t i = 0; i < axisCount; ++i) {
        if (p + 2u > limit) {
          avar_.clear();
          break;
        }
        const uint16_t count = readU16(p);
        p += 2;
        if ((size_t) p + (size_t) count * 4u > limit) {
          avar_.clear();
          break;
        }
        for (uint16_t k = 0; k < count; ++k, p += 4)
          avar_[i].push_back({(int16_t) readU16(p), (int16_t) readU16(p + 2)});
        // Sorted, or it is not a map: refused for this axis.
        for (size_t k = 1; k < avar_[i].size(); ++k)
          if (avar_[i][k].first < avar_[i][k - 1].first) avar_[i].clear();
      }
    }

    // gvar: where each glyph's deltas are.
    if (gvarOffset_) {
      const uint32_t g = gvarOffset_;
      const bool valid = gvarLength_ >= 20 && readU16(g) == 1 && readU16(g + 4) == axisCount;
      gvarSharedCount_ = readU16(g + 6);
      gvarShared_ = g + readU32(g + 8);
      gvarGlyphCount_ = readU16(g + 12);
      gvarLongOffsets_ = (readU16(g + 14) & 1) != 0;
      gvarData_ = g + readU32(g + 16);
      const size_t offsets = ((size_t) gvarGlyphCount_ + 1u) * (gvarLongOffsets_ ? 4u : 2u);
      const size_t end = (size_t) g + gvarLength_;
      if (!valid || (size_t) g + 20u + offsets > end ||
          (size_t) gvarShared_ + (size_t) gvarSharedCount_ * axisCount * 2u > end || gvarData_ > end)
        gvarOffset_ = 0;
    }
    if (hvarOffset_ && (hvarLength_ < 20 || readU16(hvarOffset_) != 1)) hvarOffset_ = 0;
  }

  /** How much a delta applies at `location`: per axis, 1 at its peak, falling
   *  to 0 at the edges of the region it covers -- the default to the peak,
   *  or an explicit start and end. */
  double tupleScalar(const std::vector<int16_t>& location, uint32_t peak, uint32_t start,
                     uint32_t end) const {
    double scalar = 1.0;
    for (size_t a = 0; a < location.size(); ++a) {
      const double p = (int16_t) readU16(peak + (uint32_t) a * 2u) / 16384.0;
      const double v = location[a] / 16384.0;
      if (p == 0.0 || v == p) continue;
      double lo = p < 0.0 ? p : 0.0, hi = p > 0.0 ? p : 0.0;
      if (start) {
        lo = (int16_t) readU16(start + (uint32_t) a * 2u) / 16384.0;
        hi = (int16_t) readU16(end + (uint32_t) a * 2u) / 16384.0;
        // A region that does not hold its peak, or that straddles the
        // default, is ignored on that axis -- the spec's own reading.
        if (lo > p || p > hi) continue;
        if (lo < 0.0 && hi > 0.0) continue;
      }
      if (v <= lo || v >= hi) return 0.0;
      scalar *= v < p ? (v - lo) / (p - lo) : (hi - v) / (hi - p);
    }
    return scalar;
  }

  /** Packed point numbers. `all` for the count of zero that means every
   *  point. */
  bool readPoints(uint32_t& q, uint32_t limit, std::vector<uint16_t>& out, bool& all) const {
    out.clear();
    all = false;
    if (q >= limit) return false;
    uint32_t count = data_[q++];
    if (count & 0x80) {
      if (q >= limit) return false;
      count = ((count & 0x7F) << 8) | data_[q++];
    }
    if (count == 0) {
      all = true;
      return true;
    }
    uint32_t value = 0;
    while (out.size() < count) {
      if (q >= limit) return false;
      const uint8_t control = data_[q++];
      const uint32_t run = (control & 0x7Fu) + 1u;
      const bool words = (control & 0x80) != 0;
      for (uint32_t r = 0; r < run && out.size() < count; ++r) {
        if (q + (words ? 2u : 1u) > limit) return false;
        value += words ? readU16(q) : data_[q];
        q += words ? 2u : 1u;
        out.push_back((uint16_t) value);
      }
    }
    return true;
  }

  /** Packed deltas: runs of zeros, bytes, words or longs. */
  bool readDeltas(uint32_t& q, uint32_t limit, size_t count, std::vector<int32_t>& out) const {
    out.clear();
    while (out.size() < count) {
      if (q >= limit) return false;
      const uint8_t control = data_[q++];
      const uint32_t run = (control & 0x3Fu) + 1u;
      const uint8_t kind = control & 0xC0;
      const uint32_t width = kind == 0x80 ? 0u : kind == 0x40 ? 2u : kind == 0xC0 ? 4u : 1u;
      for (uint32_t r = 0; r < run && out.size() < count; ++r) {
        if (q + width > limit) return false;
        if (width == 0) out.push_back(0);
        else if (width == 1) out.push_back((int8_t) data_[q]);
        else if (width == 2) out.push_back((int16_t) readU16(q));
        else out.push_back((int32_t) readU32(q));
        q += width;
      }
    }
    return true;
  }

  /** The deltas a tuple gives the points it does not list, inferred from
   *  the ones it does along each contour: between two listed points a point
   *  moves in proportion to where it lies between them, and outside them it
   *  moves as the nearer does. */
  static double interpolated(double x, double x1, double x2, double d1, double d2) {
    if (x1 == x2) return d1 == d2 ? d1 : 0.0;
    if (x1 > x2) {
      std::swap(x1, x2);
      std::swap(d1, d2);
    }
    if (x <= x1) return d1;
    if (x >= x2) return d2;
    return d1 + (x - x1) * (d2 - d1) / (x2 - x1);
  }

  static void inferContour(size_t from, size_t to, const std::vector<double>& ox,
                           const std::vector<double>& oy, std::vector<double>& dx,
                           std::vector<double>& dy, const std::vector<uint8_t>& touched) {
    std::vector<size_t> refs;
    for (size_t i = from; i <= to; ++i)
      if (touched[i]) refs.push_back(i);
    if (refs.empty()) return; // nothing listed: nothing moves
    if (refs.size() == 1) {
      for (size_t i = from; i <= to; ++i) {
        dx[i] = dx[refs[0]];
        dy[i] = dy[refs[0]];
      }
      return;
    }
    const size_t length = to - from + 1;
    for (size_t r = 0; r < refs.size(); ++r) {
      const size_t a = refs[r], b = refs[(r + 1) % refs.size()];
      // The points strictly between a and b, round the contour.
      for (size_t i = (a - from + 1) % length + from; i != b; i = (i - from + 1) % length + from) {
        dx[i] = interpolated(ox[i], ox[a], ox[b], dx[a], dx[b]);
        dy[i] = interpolated(oy[i], oy[a], oy[b], dy[a], dy[b]);
      }
    }
  }

  /** The glyph's gvar deltas at `location`, added to X and Y -- its points
   *  and then its four phantom points. `ends` are where its contours end,
   *  for the points a delta set does not list. Data that does not hold
   *  together stops here, with the deltas applied so far. */
  void applyVariations(uint16_t glyph, const std::vector<int16_t>& location, std::vector<double>& X,
                       std::vector<double>& Y, const std::vector<uint32_t>& ends) const {
    if (!gvarOffset_ || glyph >= gvarGlyphCount_ || location.size() != axes_.size()) return;
    const uint32_t table = gvarOffset_ + 20u;
    const uint32_t from = gvarLongOffsets_ ? readU32(table + glyph * 4u) : readU16(table + glyph * 2u) * 2u;
    const uint32_t to = gvarLongOffsets_ ? readU32(table + glyph * 4u + 4u)
                                         : readU16(table + glyph * 2u + 2u) * 2u;
    if (to <= from) return; // this glyph does not vary
    const uint32_t base = gvarData_ + from;
    const uint32_t limit = gvarData_ + to;
    if ((size_t) limit > (size_t) gvarOffset_ + gvarLength_ || base + 4u > limit) return;

    const size_t n = X.size();
    const uint16_t countWord = readU16(base);
    const uint16_t tuples = countWord & 0x0FFF;
    uint32_t data = base + readU16(base + 2);
    uint32_t header = base + 4;
    const uint32_t axes = (uint32_t) axes_.size();

    std::vector<uint16_t> shared, own;
    bool sharedAll = false, ownAll = false;
    if ((countWord & 0x8000) && !readPoints(data, limit, shared, sharedAll)) return;

    const std::vector<double> ox = X, oy = Y;
    std::vector<double> dx, dy;
    std::vector<uint8_t> touched;
    std::vector<int32_t> xd, yd;
    for (uint16_t t = 0; t < tuples; ++t) {
      if (header + 4u > limit) return;
      const uint16_t size = readU16(header);
      const uint16_t index = readU16(header + 2);
      header += 4;
      uint32_t peak = 0, start = 0, end = 0;
      if (index & 0x8000) {
        peak = header;
        header += axes * 2u;
      } else {
        if ((index & 0x0FFF) >= gvarSharedCount_) return;
        peak = gvarShared_ + (uint32_t) (index & 0x0FFF) * axes * 2u;
      }
      if (index & 0x4000) {
        start = header;
        end = header + axes * 2u;
        header += axes * 4u;
      }
      if (header > limit) return;
      uint32_t q = data;
      data += size;
      if (data > limit) return;

      const double scalar = tupleScalar(location, peak, start, end);
      if (scalar == 0.0) continue;
      const std::vector<uint16_t>* points = &shared;
      bool all = sharedAll;
      if (index & 0x2000) {
        if (!readPoints(q, data, own, ownAll)) return;
        points = &own;
        all = ownAll;
      }
      const size_t count = all ? n : points->size();
      if (!readDeltas(q, data, count, xd) || !readDeltas(q, data, count, yd)) return;
      if (all) {
        for (size_t i = 0; i < n; ++i) {
          X[i] += xd[i] * scalar;
          Y[i] += yd[i] * scalar;
        }
        continue;
      }
      dx.assign(n, 0.0);
      dy.assign(n, 0.0);
      touched.assign(n, 0);
      for (size_t k = 0; k < count; ++k) {
        const uint16_t i = (*points)[k];
        if (i >= n) continue;
        dx[i] = xd[k];
        dy[i] = yd[k];
        touched[i] = 1;
      }
      size_t first = 0;
      for (const uint32_t last : ends) {
        if (last >= n || last < first) break;
        inferContour(first, last, ox, oy, dx, dy, touched);
        first = last + 1;
      }
      for (size_t i = 0; i < n; ++i) {
        X[i] += dx[i] * scalar;
        Y[i] += dy[i] * scalar;
      }
    }
  }

  /** A glyph's two horizontal phantom points, varied: the advance where a
   *  font has no HVAR. */
  bool variedPhantoms(uint16_t glyph, const std::vector<int16_t>& location, double* left,
                      double* right) const {
    uint32_t length = 0;
    const uint32_t at = glyphOffset(glyph, &length);
    std::vector<double> X, Y;
    std::vector<uint32_t> ends;
    double l = -leftSideBearing(glyph);
    if (at) {
      l += (int16_t) readU16(at + 2);
      const int16_t contours = (int16_t) readU16(at);
      if (contours < 0) {
        std::vector<Component> parts;
        if (!readComponents(at, at + length, parts)) return false;
        for (size_t i = 0; i < parts.size(); ++i) {
          X.push_back(parts[i].dx);
          Y.push_back(parts[i].dy);
          ends.push_back((uint32_t) i);
        }
      } else if (contours > 0) {
        std::vector<uint8_t> flags;
        std::vector<int> xs, ys;
        if (!readSimpleGlyph(at, flags, xs, ys, ends)) return false;
        X.assign(xs.begin(), xs.end());
        Y.assign(ys.begin(), ys.end());
      }
    }
    const size_t n = X.size();
    X.insert(X.end(), {l, l + advanceWidth(glyph), 0.0, 0.0});
    Y.insert(Y.end(), {0.0, 0.0, 0.0, 0.0});
    applyVariations(glyph, location, X, Y, ends);
    *left = X[n];
    *right = X[n + 1];
    return true;
  }

  /** HVAR: the advance's delta from the item variation store, through the
   *  advance map when there is one. */
  double hvarAdvanceDelta(uint16_t glyph, const std::vector<int16_t>& location) const {
    const uint32_t h = hvarOffset_;
    const size_t tableEnd = (size_t) h + hvarLength_;
    uint32_t outer = 0, inner = glyph;
    const uint32_t mapOffset = readU32(h + 8);
    if (mapOffset) {
      const uint32_t map = h + mapOffset;
      const uint8_t format = (size_t) map + 2u <= tableEnd ? data_[map] : 0xFF;
      const uint8_t entryFormat = (size_t) map + 2u <= tableEnd ? data_[map + 1] : 0;
      if (format > 1) return 0.0;
      const uint32_t mapCount = format == 0 ? readU16(map + 2) : readU32(map + 2);
      const uint32_t entries = map + (format == 0 ? 4u : 6u);
      const uint32_t entrySize = ((entryFormat >> 4) & 3u) + 1u;
      const uint32_t innerBits = (entryFormat & 0x0Fu) + 1u;
      if (mapCount == 0) return 0.0;
      const uint32_t i = glyph < mapCount ? glyph : mapCount - 1;
      const uint32_t at = entries + i * entrySize;
      if ((size_t) at + entrySize > tableEnd) return 0.0;
      uint32_t entry = 0;
      for (uint32_t k = 0; k < entrySize; ++k) entry = (entry << 8) | data_[at + k];
      outer = entry >> innerBits;
      inner = entry & ((1u << innerBits) - 1u);
    }
    return itemDelta(h + readU32(h + 4), tableEnd, outer, inner, location);
  }

  /** One delta set of an item variation store, at `location`. */
  double itemDelta(uint32_t store, size_t tableEnd, uint32_t outer, uint32_t inner,
                   const std::vector<int16_t>& location) const {
    if ((size_t) store + 8u > tableEnd || readU16(store) != 1) return 0.0;
    const uint32_t regions = store + readU32(store + 2);
    const uint16_t dataCount = readU16(store + 6);
    if (outer >= dataCount || (size_t) store + 8u + (size_t) dataCount * 4u > tableEnd) return 0.0;
    const uint32_t ivd = store + readU32(store + 8u + outer * 4u);
    if ((size_t) ivd + 6u > tableEnd) return 0.0;
    const uint16_t itemCount = readU16(ivd);
    const uint16_t wordWord = readU16(ivd + 2);
    const bool longWords = (wordWord & 0x8000) != 0;
    const uint32_t words = wordWord & 0x7FFFu;
    const uint32_t regionCount = readU16(ivd + 4);
    if (inner >= itemCount || words > regionCount) return 0.0;
    const uint32_t indexes = ivd + 6;
    const uint32_t rowSize = longWords ? 4u * words + 2u * (regionCount - words)
                                       : 2u * words + (regionCount - words);
    const uint32_t row = indexes + regionCount * 2u + inner * rowSize;
    if ((size_t) row + rowSize > tableEnd) return 0.0;

    // The region list: per region, a start, peak and end per axis.
    if ((size_t) regions + 4u > tableEnd) return 0.0;
    const uint16_t regionAxes = readU16(regions);
    const uint16_t regionTotal = readU16(regions + 2);
    if (regionAxes != location.size() ||
        (size_t) regions + 4u + (size_t) regionTotal * regionAxes * 6u > tableEnd)
      return 0.0;

    double sum = 0.0;
    uint32_t p = row;
    for (uint32_t r = 0; r < regionCount; ++r) {
      int32_t delta;
      if (r < words) {
        delta = longWords ? (int32_t) readU32(p) : (int16_t) readU16(p);
        p += longWords ? 4u : 2u;
      } else {
        delta = longWords ? (int16_t) readU16(p) : (int8_t) data_[p];
        p += longWords ? 2u : 1u;
      }
      const uint16_t region = readU16(indexes + r * 2u);
      if (region >= regionTotal || delta == 0) continue;
      double scalar = 1.0;
      const uint32_t rec = regions + 4u + (uint32_t) region * regionAxes * 6u;
      for (uint32_t a = 0; a < regionAxes && scalar != 0.0; ++a) {
        const double lo = (int16_t) readU16(rec + a * 6u) / 16384.0;
        const double pk = (int16_t) readU16(rec + a * 6u + 2u) / 16384.0;
        const double hi = (int16_t) readU16(rec + a * 6u + 4u) / 16384.0;
        const double v = location[a] / 16384.0;
        if (pk == 0.0 || v == pk || lo > pk || pk > hi || (lo < 0.0 && hi > 0.0)) continue;
        if (v <= lo || v >= hi) scalar = 0.0;
        else scalar *= v < pk ? (v - lo) / (pk - lo) : (hi - v) / (hi - pk);
      }
      sum += delta * scalar;
    }
    return sum;
  }

  // ── CFF: the outlines of an .otf ──────────────────────────────────────────
  //
  // The Compact Font Format holds outlines as small programs -- Type 2
  // charstrings -- of cubic curves, with shared pieces factored out into
  // subroutines, local to the font and global to the table. A CID-keyed font,
  // which is how CJK fonts are built, splits its glyphs among several Font
  // DICTs, each with its own local subroutines, chosen per glyph by FDSelect.
  // Everything is read from the bytes on demand and bounds-checked against
  // the CFF table, which is untrusted input like the rest of the file.

  /** An INDEX: a count, then count+1 offsets, then the data they point into. */
  struct CffIndex {
    uint32_t offsets = 0; // the first offset
    uint32_t base = 0;    // where offsets count from, less one (offsets are 1-based)
    uint16_t count = 0;
    uint8_t offSize = 0;
  };

  uint32_t readOffset(uint32_t at, uint8_t size) const {
    uint32_t v = 0;
    for (uint8_t i = 0; i < size; ++i) {
      if (at + i >= cffEnd_) return 0;
      v = (v << 8) | data_[at + i];
    }
    return v;
  }

  /** The INDEX at `at`; `next`, if given, is where the following structure
   *  starts. False when it does not fit inside the table. */
  bool readIndex(uint32_t at, CffIndex& out, uint32_t* next) const {
    out = CffIndex{};
    if (at < cffStart_ || (size_t) at + 2u > cffEnd_) return false;
    out.count = readU16(at);
    if (out.count == 0) {
      if (next) *next = at + 2;
      return true;
    }
    if ((size_t) at + 3u > cffEnd_) return false;
    out.offSize = data_[at + 2];
    if (out.offSize < 1 || out.offSize > 4) return false;
    out.offsets = at + 3;
    // In 64 bits: a 32-bit build's size_t wraps on offsets near 4 GB.
    const uint64_t table = (uint64_t) out.offsets + ((uint64_t) out.count + 1u) * out.offSize;
    if (table > cffEnd_) return false;
    out.base = (uint32_t) table - 1u;
    const uint32_t last = readOffset(out.offsets + (uint32_t) out.count * out.offSize, out.offSize);
    if (last < 1 || (uint64_t) out.base + last > cffEnd_) return false;
    if (next) *next = out.base + last;
    return true;
  }

  /** Item `i` of an INDEX as [start, end) in the file, or false. */
  bool indexItem(const CffIndex& index, uint32_t i, uint32_t* start, uint32_t* end) const {
    if (i >= index.count) return false;
    const uint32_t a = readOffset(index.offsets + i * index.offSize, index.offSize);
    const uint32_t b = readOffset(index.offsets + (i + 1) * index.offSize, index.offSize);
    if (a < 1 || b < a || (uint64_t) index.base + b > cffEnd_) return false;
    *start = index.base + a;
    *end = index.base + b;
    return true;
  }

  /** Walk a DICT, calling `op(operator, operands, count)` for each entry. A
   *  two-byte operator 12 x is reported as 1200 + x. */
  template <typename Op>
  bool walkDict(uint32_t p, uint32_t end, Op op) const {
    double operands[48];
    int n = 0;
    while (p < end) {
      const uint8_t b0 = data_[p];
      if (b0 <= 21) {
        int code = b0;
        ++p;
        if (b0 == 12) {
          if (p >= end) return false;
          code = 1200 + data_[p++];
        }
        op(code, operands, n);
        n = 0;
        continue;
      }
      double v = 0.0;
      if (b0 == 28) {
        if (p + 3u > end) return false;
        v = (int16_t) readU16(p + 1);
        p += 3;
      } else if (b0 == 29) {
        if (p + 5u > end) return false;
        v = (int32_t) readU32(p + 1);
        p += 5;
      } else if (b0 == 30) {
        // A real, as decimal nibbles: 0-9, a '.', b 'E', c 'E-', e '-', f end.
        // Read as numbers, not handed to atof: atof reads the decimal point
        // of the process's locale, hosts set the locale, and in one that
        // writes "0,001" a FontMatrix of 0.001 came out as 0.
        double mantissa = 0.0;
        int fraction = 0, exponent = 0;
        bool point = false, inExponent = false, negative = false, exponentNegative = false;
        ++p;
        bool done = false;
        while (!done && p < end) {
          const uint8_t byte = data_[p++];
          for (int half = 0; half < 2 && !done; ++half) {
            const int nib = half == 0 ? (byte >> 4) : (byte & 0x0F);
            if (nib <= 9) {
              if (inExponent) {
                if (exponent < 400) exponent = exponent * 10 + nib;
              } else if (mantissa < 1e17) {
                mantissa = mantissa * 10.0 + nib;
                if (point) ++fraction;
              } else if (!point) {
                ++exponent; // digits past what a double keeps scale it
              }
            } else if (nib == 0xA) point = true;
            else if (nib == 0xB) inExponent = true;
            else if (nib == 0xC) inExponent = exponentNegative = true;
            else if (nib == 0xE) negative = true;
            else if (nib == 0xF) done = true;
            else return false;
          }
        }
        const int power = (exponentNegative ? -exponent : exponent) - fraction;
        v = mantissa * std::pow(10.0, (double) (power < -400 ? -400 : (power > 400 ? 400 : power)));
        if (negative) v = -v;
      } else if (b0 >= 32 && b0 <= 246) {
        v = (int) b0 - 139;
        ++p;
      } else if (b0 >= 247 && b0 <= 250) {
        if (p + 2u > end) return false;
        v = ((int) b0 - 247) * 256 + data_[p + 1] + 108;
        p += 2;
      } else if (b0 >= 251 && b0 <= 254) {
        if (p + 2u > end) return false;
        v = -((int) b0 - 251) * 256 - data_[p + 1] - 108;
        p += 2;
      } else {
        return false; // 22-27, 31, 255: reserved in a DICT
      }
      if (n >= 48) return false;
      operands[n++] = v;
    }
    return true;
  }

  /** A DICT operand as an offset or a size: -1 unless it is a whole number
   *  a font table could hold. Checked BEFORE the conversion, which is
   *  undefined for a double past the integer's range. */
  static int64_t dictOffset(double v) {
    return (v >= 0.0 && v < 2147483648.0) ? (int64_t) v : -1;
  }

  /** The local subroutines a Private DICT names, or an empty INDEX. */
  bool readPrivate(uint32_t size, uint32_t offset, CffIndex& subrs) const {
    subrs = CffIndex{};
    const uint32_t start = cffStart_ + offset;
    if (offset > cffEnd_ - cffStart_ || size > cffEnd_ - start) return false;
    int64_t subrsOffset = -1;
    if (!walkDict(start, start + size, [&](int code, const double* v, int n) {
          if (code == 19 && n >= 1) subrsOffset = dictOffset(v[0]);
        }))
      return false;
    if (subrsOffset < 0) return true;
    if ((uint64_t) subrsOffset >= cffEnd_ - start) return false;
    return readIndex(start + (uint32_t) subrsOffset, subrs, nullptr);
  }

  bool parseCff(uint32_t offset, uint32_t length) {
    cffStart_ = offset;
    cffEnd_ = offset + length;
    if (length < 4) return fail("the CFF table is truncated");
    // Version 1. CFF2 -- variable fonts' outlines -- is a different format.
    if (data_[offset] != 1) return fail("only CFF version 1 is supported, not CFF2");
    uint32_t p = offset + data_[offset + 2];
    CffIndex names, tops, strings;
    if (!readIndex(p, names, &p) || !readIndex(p, tops, &p) || !readIndex(p, strings, &p) ||
        !readIndex(p, globalSubrs_, &p))
      return fail("a CFF INDEX runs past the table");
    uint32_t ts = 0, te = 0;
    if (!indexItem(tops, 0, &ts, &te)) return fail("the CFF table has no Top DICT");
    int64_t charStrings = -1, privateSize = 0, privateOffset = -1, fdArray = -1, fdSelect = -1;
    int charstringType = 2;
    bool cid = false;
    double matrix[2] = {0.0, 0.0};
    bool haveMatrix = false;
    if (!walkDict(ts, te, [&](int code, const double* v, int n) {
          if (code == 17 && n >= 1) charStrings = dictOffset(v[0]);
          else if (code == 18 && n >= 2) {
            privateSize = dictOffset(v[0]);
            privateOffset = dictOffset(v[1]);
          } else if (code == 1206 && n >= 1) charstringType = v[0] == 2.0 ? 2 : 0;
          else if (code == 1207 && n >= 4) {
            matrix[0] = v[0];
            matrix[1] = v[3];
            haveMatrix = true;
          } else if (code == 1230) cid = true;
          else if (code == 1236 && n >= 1) fdArray = dictOffset(v[0]);
          else if (code == 1237 && n >= 1) fdSelect = dictOffset(v[0]);
        }))
      return fail("the CFF Top DICT is malformed");
    if (charstringType != 2) return fail("only Type 2 charstrings are supported");
    if (charStrings <= 0 || charStrings >= (int64_t) length ||
        !readIndex(cffStart_ + (uint32_t) charStrings, charStrings_, nullptr))
      return fail("the CFF CharStrings INDEX is missing or malformed");
    // FontMatrix, as a scale onto the head table's em: 0.001 on a 1000-unit
    // em is 1, which is nearly every font. A CID-keyed font may give one per
    // Font DICT as well, which applies on top of the Top DICT's -- Adobe's
    // own CJK fonts put the identity in the Top DICT and 0.001 in each Font
    // DICT, and reading only the Top refused them as implausible.
    auto scaleOf = [&](bool fdHas, const double* fd, float* sx, float* sy) {
      double a = 0.001, d = 0.001;
      if (fdHas && haveMatrix) {
        a = fd[0] * matrix[0];
        d = fd[1] * matrix[1];
      } else if (fdHas) {
        a = fd[0];
        d = fd[1];
      } else if (haveMatrix) {
        a = matrix[0];
        d = matrix[1];
      }
      *sx = (float) (a * unitsPerEm_);
      *sy = (float) (d * unitsPerEm_);
      return *sx > 0.0f && *sx < 64.0f && *sy > 0.0f && *sy < 64.0f;
    };
    if (!cid && !scaleOf(false, nullptr, &cffScaleX_, &cffScaleY_))
      return fail("the CFF FontMatrix is implausible");
    cid_ = cid;
    if (cid) {
      CffIndex fds;
      if (fdArray <= 0 || fdArray >= (int64_t) length || fdSelect <= 0 ||
          fdSelect >= (int64_t) length || !readIndex(cffStart_ + (uint32_t) fdArray, fds, nullptr))
        return fail("a CID-keyed CFF font without a usable FDArray or FDSelect");
      // A real CJK font has a few dozen Font DICTs. Many thousands, every
      // one naming the same large Private DICT, made loading the font walk
      // that DICT thousands of times: 13 s for a 1 MB file. Capped, and a
      // Private shared between Font DICTs read once.
      if (fds.count > 256) return fail("a CID-keyed CFF font with more than 256 Font DICTs");
      fdSubrs_.assign(fds.count, CffIndex{});
      fdScaleX_.assign(fds.count, 1.0f);
      fdScaleY_.assign(fds.count, 1.0f);
      struct Read {
        int64_t at, size;
        uint32_t fd;
      };
      std::vector<Read> privates; // each Private DICT read, and the Font DICT that read it
      for (uint32_t i = 0; i < fds.count; ++i) {
        uint32_t fs = 0, fe = 0;
        if (!indexItem(fds, i, &fs, &fe)) return fail("a CFF Font DICT runs past the table");
        int64_t size = 0, at = -1;
        double fdMatrix[2] = {0.0, 0.0};
        bool fdHasMatrix = false;
        if (!walkDict(fs, fe, [&](int code, const double* v, int n) {
              if (code == 18 && n >= 2) {
                size = dictOffset(v[0]);
                at = dictOffset(v[1]);
              } else if (code == 1207 && n >= 4) {
                fdMatrix[0] = v[0];
                fdMatrix[1] = v[3];
                fdHasMatrix = true;
              }
            }))
          return fail("a CFF Font DICT is malformed");
        if (!scaleOf(fdHasMatrix, fdMatrix, &fdScaleX_[i], &fdScaleY_[i]))
          return fail("the CFF FontMatrix is implausible");
        if (at < 0) continue;
        if (size < 0) return fail("a CFF Private DICT is malformed");
        bool shared = false;
        for (const Read& r : privates)
          if (r.at == at && r.size == size) {
            fdSubrs_[i] = fdSubrs_[r.fd];
            shared = true;
            break;
          }
        if (shared) continue;
        if (!readPrivate((uint32_t) size, (uint32_t) at, fdSubrs_[i]))
          return fail("a CFF Private DICT is malformed");
        privates.push_back({at, size, i});
      }
      fdSelect_ = cffStart_ + (uint32_t) fdSelect;
      fdSelectFormat_ = data_[fdSelect_];
      if (fdSelectFormat_ != 0 && fdSelectFormat_ != 3) return fail("an unknown FDSelect format");
    } else if (privateOffset >= 0) {
      if (privateSize < 0 || !readPrivate((uint32_t) privateSize, (uint32_t) privateOffset, localSubrs_))
        return fail("the CFF Private DICT is malformed");
    }
    return true;
  }

  /** The Font DICT a CID-keyed font draws `glyph` with, or -1. */
  int fdFor(uint16_t glyph) const {
    if (fdSelectFormat_ == 0) {
      const uint32_t at = fdSelect_ + 1u + glyph;
      return at < cffEnd_ ? data_[at] : -1;
    }
    const uint16_t ranges = readU16(fdSelect_ + 1);
    for (uint32_t r = 0; r < ranges; ++r) {
      const uint32_t at = fdSelect_ + 3u + r * 3u;
      if ((size_t) at + 5u > cffEnd_) return -1;
      const uint16_t first = readU16(at), next = readU16(at + 3);
      if (glyph >= first && glyph < next) return data_[at + 2];
    }
    return -1;
  }

  static int subrBias(uint16_t count) { return count < 1240 ? 107 : (count < 33900 ? 1131 : 32768); }

  /** A Type 2 charstring being run: its operand stack, its pen, and how far
   *  it has gone, so a hostile font's subroutines cannot run without end. */
  struct Charstring {
    Path* out;
    float ox, oy, scale, sx, sy;
    float stack[48] = {}; // initialised: the aggregate is built without it
    int sp = 0;
    float x = 0.0f, y = 0.0f;
    int stems = 0;
    bool widthTaken = false;
    bool open = false;
    bool ended = false;
    int steps = 0;
  };

  void csMove(Charstring& c, float dx, float dy) const {
    if (c.open) c.out->close();
    c.x += dx;
    c.y += dy;
    // y NEGATED, as for glyf: the font grows upward, the screen downward.
    c.out->moveTo(c.ox + c.x * c.sx * c.scale, c.oy - c.y * c.sy * c.scale);
    c.open = true;
  }
  void csLine(Charstring& c, float dx, float dy) const {
    if (!c.open) csMove(c, 0.0f, 0.0f);
    c.x += dx;
    c.y += dy;
    c.out->lineTo(c.ox + c.x * c.sx * c.scale, c.oy - c.y * c.sy * c.scale);
  }
  void csCurve(Charstring& c, float dx1, float dy1, float dx2, float dy2, float dx3, float dy3) const {
    if (!c.open) csMove(c, 0.0f, 0.0f);
    const float x1 = c.x + dx1, y1 = c.y + dy1;
    const float x2 = x1 + dx2, y2 = y1 + dy2;
    c.x = x2 + dx3;
    c.y = y2 + dy3;
    const float k = c.scale;
    c.out->cubicTo(c.ox + x1 * c.sx * k, c.oy - y1 * c.sy * k, c.ox + x2 * c.sx * k,
                   c.oy - y2 * c.sy * k, c.ox + c.x * c.sx * k, c.oy - c.y * c.sy * k);
  }

  /** The advance width, which may lead the first stack-clearing operator's
   *  arguments, is dropped: hmtx says the same, and this parser reads that. */
  static void csDropWidth(Charstring& c, bool hasWidth) {
    if (!c.widthTaken && hasWidth && c.sp > 0) {
      for (int i = 1; i < c.sp; ++i) c.stack[i - 1] = c.stack[i];
      --c.sp;
    }
    c.widthTaken = true;
  }

  bool runCharstring(uint32_t p, uint32_t end, Charstring& c, const CffIndex* local, int depth) const {
    if (depth > 10) return false;
    while (p < end && !c.ended) {
      if (++c.steps > 200000) return false;
      const uint8_t b0 = data_[p];
      // Operands.
      if (b0 == 28 || b0 >= 32) {
        float v = 0.0f;
        if (b0 == 28) {
          if (p + 3u > end) return false;
          v = (float) (int16_t) readU16(p + 1);
          p += 3;
        } else if (b0 <= 246) {
          v = (float) ((int) b0 - 139);
          ++p;
        } else if (b0 <= 250) {
          if (p + 2u > end) return false;
          v = (float) (((int) b0 - 247) * 256 + data_[p + 1] + 108);
          p += 2;
        } else if (b0 <= 254) {
          if (p + 2u > end) return false;
          v = (float) (-((int) b0 - 251) * 256 - data_[p + 1] - 108);
          p += 2;
        } else {
          if (p + 5u > end) return false;
          v = (float) (int32_t) readU32(p + 1) / 65536.0f;
          p += 5;
        }
        if (c.sp >= 48) return false;
        c.stack[c.sp++] = v;
        continue;
      }
      ++p;
      float* a = c.stack;
      switch (b0) {
        case 1:  // hstem
        case 3:  // vstem
        case 18: // hstemhm
        case 23: // vstemhm
          csDropWidth(c, (c.sp & 1) != 0);
          c.stems += c.sp / 2;
          c.sp = 0;
          break;
        case 19: // hintmask
        case 20: // cntrmask
          // Arguments here are an implied vstem.
          csDropWidth(c, (c.sp & 1) != 0);
          c.stems += c.sp / 2;
          c.sp = 0;
          p += (uint32_t) ((c.stems + 7) / 8);
          break;
        case 21: // rmoveto
          csDropWidth(c, c.sp > 2);
          if (c.sp < 2) return false;
          csMove(c, a[0], a[1]);
          c.sp = 0;
          break;
        case 22: // hmoveto
          csDropWidth(c, c.sp > 1);
          if (c.sp < 1) return false;
          csMove(c, a[0], 0.0f);
          c.sp = 0;
          break;
        case 4: // vmoveto
          csDropWidth(c, c.sp > 1);
          if (c.sp < 1) return false;
          csMove(c, 0.0f, a[0]);
          c.sp = 0;
          break;
        case 5: // rlineto
          for (int i = 0; i + 1 < c.sp; i += 2) csLine(c, a[i], a[i + 1]);
          c.sp = 0;
          break;
        case 6: // hlineto
        case 7: // vlineto
          for (int i = 0; i < c.sp; ++i) {
            const bool horizontal = (i % 2 == 0) == (b0 == 6);
            csLine(c, horizontal ? a[i] : 0.0f, horizontal ? 0.0f : a[i]);
          }
          c.sp = 0;
          break;
        case 8: // rrcurveto
          for (int i = 0; i + 5 < c.sp; i += 6)
            csCurve(c, a[i], a[i + 1], a[i + 2], a[i + 3], a[i + 4], a[i + 5]);
          c.sp = 0;
          break;
        case 27: { // hhcurveto
          int i = 0;
          float dy1 = 0.0f;
          if (c.sp & 1) dy1 = a[i++];
          for (; i + 3 < c.sp; i += 4) {
            csCurve(c, a[i], dy1, a[i + 1], a[i + 2], a[i + 3], 0.0f);
            dy1 = 0.0f;
          }
          c.sp = 0;
          break;
        }
        case 26: { // vvcurveto
          int i = 0;
          float dx1 = 0.0f;
          if (c.sp & 1) dx1 = a[i++];
          for (; i + 3 < c.sp; i += 4) {
            csCurve(c, dx1, a[i], a[i + 1], a[i + 2], 0.0f, a[i + 3]);
            dx1 = 0.0f;
          }
          c.sp = 0;
          break;
        }
        case 30:   // vhcurveto
        case 31: { // hvcurveto
          bool horizontal = b0 == 31;
          int i = 0;
          while (c.sp - i >= 4) {
            const bool last = c.sp - i == 5;
            if (horizontal)
              csCurve(c, a[i], 0.0f, a[i + 1], a[i + 2], last ? a[i + 4] : 0.0f, a[i + 3]);
            else
              csCurve(c, 0.0f, a[i], a[i + 1], a[i + 2], a[i + 3], last ? a[i + 4] : 0.0f);
            i += last ? 5 : 4;
            horizontal = !horizontal;
          }
          c.sp = 0;
          break;
        }
        case 24: { // rcurveline
          int i = 0;
          for (; c.sp - i >= 8; i += 6)
            csCurve(c, a[i], a[i + 1], a[i + 2], a[i + 3], a[i + 4], a[i + 5]);
          if (c.sp - i >= 2) csLine(c, a[i], a[i + 1]);
          c.sp = 0;
          break;
        }
        case 25: { // rlinecurve
          int i = 0;
          for (; c.sp - i > 6; i += 2) csLine(c, a[i], a[i + 1]);
          if (c.sp - i >= 6) csCurve(c, a[i], a[i + 1], a[i + 2], a[i + 3], a[i + 4], a[i + 5]);
          c.sp = 0;
          break;
        }
        case 10:   // callsubr
        case 29: { // callgsubr
          const CffIndex* subrs = b0 == 10 ? local : &globalSubrs_;
          if (!subrs || c.sp < 1) return false;
          const int index = (int) a[--c.sp] + subrBias(subrs->count);
          uint32_t ss = 0, se = 0;
          if (index < 0 || !indexItem(*subrs, (uint32_t) index, &ss, &se)) return false;
          if (!runCharstring(ss, se, c, local, depth + 1)) return false;
          break;
        }
        case 11: // return
          return true;
        case 14: // endchar
          // Four more arguments would be a seac accent, which Type 2 fonts
          // no longer use; drawn as the base glyph's outline is not here.
          csDropWidth(c, c.sp == 1 || c.sp == 5);
          if (c.open) c.out->close();
          c.open = false;
          c.ended = true;
          return true;
        case 12: {
          if (p >= end) return false;
          const uint8_t b1 = data_[p++];
          if (b1 == 0) { // dotsection: deprecated, and ignored, as every reader does
            c.sp = 0;
            break;
          }
          if (b1 == 35 && c.sp >= 13) { // flex
            csCurve(c, a[0], a[1], a[2], a[3], a[4], a[5]);
            csCurve(c, a[6], a[7], a[8], a[9], a[10], a[11]);
          } else if (b1 == 34 && c.sp >= 7) { // hflex
            csCurve(c, a[0], 0.0f, a[1], a[2], a[3], 0.0f);
            csCurve(c, a[4], 0.0f, a[5], -a[2], a[6], 0.0f);
          } else if (b1 == 36 && c.sp >= 9) { // hflex1
            csCurve(c, a[0], a[1], a[2], a[3], a[4], 0.0f);
            csCurve(c, a[5], 0.0f, a[6], a[7], a[8], -(a[1] + a[3] + a[7]));
          } else if (b1 == 37 && c.sp >= 11) { // flex1
            const float dx = a[0] + a[2] + a[4] + a[6] + a[8];
            const float dy = a[1] + a[3] + a[5] + a[7] + a[9];
            const bool wide = std::fabs(dx) > std::fabs(dy);
            csCurve(c, a[0], a[1], a[2], a[3], a[4], a[5]);
            csCurve(c, a[6], a[7], a[8], a[9], wide ? a[10] : -dx, wide ? -dy : a[10]);
          } else {
            // The arithmetic and storage operators: in the specification,
            // in no font anyone ships. Refused rather than guessed at.
            return false;
          }
          c.sp = 0;
          break;
        }
        default:
          return false; // reserved
      }
    }
    return true;
  }

  bool appendCffGlyph(uint16_t glyph, Path& out, float ox, float oy, float scale) const {
    uint32_t start = 0, end = 0;
    if (!indexItem(charStrings_, glyph, &start, &end)) return true; // past the font: empty
    const CffIndex* local = &localSubrs_;
    float sx = cffScaleX_, sy = cffScaleY_;
    if (cid_) {
      const int fd = fdFor(glyph);
      const bool known = fd >= 0 && (size_t) fd < fdSubrs_.size();
      local = known ? &fdSubrs_[(size_t) fd] : nullptr;
      if (known) {
        sx = fdScaleX_[(size_t) fd];
        sy = fdScaleY_[(size_t) fd];
      }
    }
    // Built apart and added only whole: a glyph refused part-way -- an
    // operator this does not draw -- left the contours before it in `out`,
    // and a Font cached that half a letter.
    Path glyphPath;
    Charstring c{&glyphPath, ox, oy, scale, sx, sy};
    const bool ok = runCharstring(start, end, c, local, 0);
    if (!ok) return false;
    if (c.open) glyphPath.close();
    out.append(glyphPath);
    return true;
  }

  uint16_t readU16(uint32_t at) const {
    if ((size_t) at + 2u > data_.size()) return 0;
    return (uint16_t) (((uint16_t) data_[at] << 8) | data_[at + 1]);
  }

  uint32_t readU32(uint32_t at) const {
    if ((size_t) at + 4u > data_.size()) return 0;
    return ((uint32_t) data_[at] << 24) | ((uint32_t) data_[at + 1] << 16) |
           ((uint32_t) data_[at + 2] << 8) | (uint32_t) data_[at + 3];
  }

  detail::FontBytes data_;
  std::string error_;
  bool ok_ = false;

  uint32_t headOffset_ = 0, maxpOffset_ = 0, hheaOffset_ = 0, hmtxOffset_ = 0;
  uint32_t cmapOffset_ = 0, locaOffset_ = 0, locaLength_ = 0, glyfOffset_ = 0, glyfLength_ = 0;
  uint32_t kernOffset_ = 0, kernPairs_ = 0;
  uint16_t cmapFormat_ = 0;
  uint32_t uvsOffset_ = 0; // the cmap's variation sequences, format 14, or 0
  static constexpr uint32_t kMaxColourLayers = 256;
  // Colour glyphs: COLR's layers and CPAL's palettes; sbix's pictures, or
  // CBLC's index to CBDT's.
  uint32_t colrOffset_ = 0, cpalOffset_ = 0;
  uint32_t sbixOffset_ = 0, sbixLength_ = 0, cblcOffset_ = 0, cblcLength_ = 0, cbdtOffset_ = 0, cbdtLength_ = 0;
  // GPOS kerning: per lookup, its pair-adjustment subtables.
  uint32_t gposOffset_ = 0, gposLength_ = 0;
  std::vector<std::vector<uint32_t>> kernLookups_;

  // CFF outlines, when the font has them instead of glyf.
  uint32_t cffOffset_ = 0, cffLength_ = 0, cffStart_ = 0, cffEnd_ = 0;
  CffIndex charStrings_, globalSubrs_, localSubrs_;
  std::vector<CffIndex> fdSubrs_; // a CID-keyed font's, one per Font DICT
  uint32_t fdSelect_ = 0;
  uint8_t fdSelectFormat_ = 0;
  bool cid_ = false;
  float cffScaleX_ = 1.0f, cffScaleY_ = 1.0f;
  std::vector<float> fdScaleX_, fdScaleY_; // a CID-keyed font's, per Font DICT

  int unitsPerEm_ = 0, numGlyphs_ = 0;
  int ascender_ = 0, descender_ = 0, lineGap_ = 0;
  int indexToLocFormat_ = 0;
  uint16_t numHMetrics_ = 0;
  uint32_t directory_ = 0; // the table directory, for table()
  uint16_t numTables_ = 0;

  // Variations.
  uint32_t fvarOffset_ = 0, fvarLength_ = 0, avarOffset_ = 0, avarLength_ = 0;
  uint32_t gvarOffset_ = 0, gvarLength_ = 0, hvarOffset_ = 0, hvarLength_ = 0;
  uint32_t gvarShared_ = 0, gvarData_ = 0;
  uint16_t gvarSharedCount_ = 0, gvarGlyphCount_ = 0;
  bool gvarLongOffsets_ = false;
  std::vector<FontAxis> axes_;
  std::vector<std::vector<std::pair<int16_t, int16_t>>> avar_;
};

} // namespace gfx
} // namespace sonore
