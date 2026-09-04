// SPDX-License-Identifier: Apache-2.0
//
// A TrueType parser: glyph outlines out of a .ttf, as ordinary Paths.
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
// larger than everything here put together. What is covered is the Latin
// text a plugin's interface is made of: a glyph per character, advance widths
// from hmtx, optional kerning from the old kern table.
//
// A font this cannot read is REFUSED rather than half-drawn. A parser that
// guesses at a table it does not understand produces glyphs that are subtly
// the wrong shape, which nobody reports as a bug because it just looks bad.
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "path.h"

namespace sonore {
namespace gfx {

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
  bool load(std::vector<uint8_t> fileBytes) {
    data_ = std::move(fileBytes);
    ok_ = parse();
    if (!ok_) data_.clear();
    return ok_;
  }

  bool isValid() const { return ok_; }
  const std::string& error() const { return error_; }

  /** Font design units per em. Every metric below is in these units; divide
   *  by this and multiply by the point size to get pixels. */
  int unitsPerEm() const { return unitsPerEm_; }
  int numGlyphs() const { return numGlyphs_; }

  int ascender() const { return ascender_; }
  int descender() const { return descender_; }
  int lineGap() const { return lineGap_; }

  /** The glyph for a Unicode code point, or 0 -- which by the specification
   *  is .notdef, the empty box. Returning 0 rather than failing is right: a
   *  missing character should draw the box a user recognises. */
  uint16_t glyphForChar(uint32_t codepoint) const {
    if (!ok_ || cmapOffset_ == 0) return 0;
    if (cmapFormat_ == 4) return lookupFormat4(codepoint);
    if (cmapFormat_ == 12) return lookupFormat12(codepoint);
    return 0;
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
    return appendGlyphImpl(glyph, out, xOffset, yOffset, scale, 0);
  }

private:
  static constexpr int kMaxCompositeDepth = 5;

  bool parse() {
    error_.clear();
    if (data_.size() < 12) return fail("file is too small to be a font");

    uint32_t base = 0;
    const uint32_t tag = readU32(0);
    if (tag == 0x74746366u) { // 'ttcf': a collection, take the first face
      if (data_.size() < 16) return fail("truncated font collection");
      base = readU32(12);
      if ((size_t) base + 12u > data_.size()) return fail("collection points outside the file");
    } else if (tag != 0x00010000u && tag != 0x74727565u) {
      // 0x4F54544F is 'OTTO': CFF outlines, a completely different format.
      // Refused by name rather than parsed as if it had glyf.
      if (tag == 0x4F54544Fu) return fail("OpenType/CFF outlines are not supported");
      return fail("not a TrueType font");
    }

    const uint16_t numTables = readU16(base + 4);
    const uint32_t dir = base + 12;
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
      if ((size_t) off + (size_t) len > data_.size()) continue;
      switch (t) {
        case 0x68656164: headOffset_ = off; break; // head
        case 0x6D617870: maxpOffset_ = off; break; // maxp
        case 0x68686561: hheaOffset_ = off; break; // hhea
        case 0x686D7478: hmtxOffset_ = off; break; // hmtx
        case 0x636D6170: cmapOffset_ = off; break; // cmap
        case 0x6C6F6361: locaOffset_ = off; locaLength_ = len; break; // loca
        case 0x676C7966: glyfOffset_ = off; glyfLength_ = len; break; // glyf
        case 0x6B65726E: parseKern(off, len); break;                  // kern
        default: break;
      }
    }

    if (!headOffset_ || !maxpOffset_ || !hheaOffset_ || !glyfOffset_ || !locaOffset_)
      return fail("a required table is missing (head, maxp, hhea, glyf or loca)");

    unitsPerEm_ = readU16(headOffset_ + 18);
    if (unitsPerEm_ <= 0) return fail("unitsPerEm is zero");
    indexToLocFormat_ = (int16_t) readU16(headOffset_ + 50);
    numGlyphs_ = readU16(maxpOffset_ + 4);
    ascender_ = (int16_t) readU16(hheaOffset_ + 4);
    descender_ = (int16_t) readU16(hheaOffset_ + 6);
    lineGap_ = (int16_t) readU16(hheaOffset_ + 8);
    numHMetrics_ = readU16(hheaOffset_ + 34);

    if (cmapOffset_) selectCmap();
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

  void parseKern(uint32_t off, uint32_t len) {
    if (len < 14) return;
    // Only the first subtable, and only format 0 horizontal. Anything else is
    // rare enough that guessing is worse than reporting no kerning.
    const uint16_t coverage = readU16(off + 8);
    if ((coverage & 0x0001) == 0) return; // not horizontal
    if (((coverage >> 8) & 0xFF) != 0) return; // not format 0
    kernPairs_ = readU16(off + 10);
    kernOffset_ = off + 14;
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

  bool appendGlyphImpl(uint16_t glyph, Path& out, float xOffset, float yOffset, float scale,
                       int depth) const {
    if (!ok_ || depth > kMaxCompositeDepth) return false;
    uint32_t length = 0;
    const uint32_t at = glyphOffset(glyph, &length);
    if (at == 0) return true; // empty glyph, e.g. a space

    const int16_t contours = (int16_t) readU16(at);
    if (contours < 0) return appendComposite(at, out, xOffset, yOffset, scale, depth);

    const uint32_t endPts = at + 10;
    const uint32_t n = (uint32_t) contours;
    if ((size_t) endPts + n * 2u + 2u > data_.size()) return false;
    const uint32_t numPoints = (uint32_t) readU16(endPts + (n - 1) * 2u) + 1u;
    if (numPoints > 10000u) return false; // implausible; refuse rather than allocate

    const uint32_t instrLen = readU16(endPts + n * 2u);
    uint32_t p = endPts + n * 2u + 2u + instrLen;

    // Flags are run-length encoded: a REPEAT bit means the next byte says how
    // many more points share these flags.
    std::vector<uint8_t> flags;
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
    std::vector<int> xs(numPoints), ys(numPoints);
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

    uint32_t first = 0;
    for (uint32_t c = 0; c < n; ++c) {
      const uint32_t last = readU16(endPts + c * 2u);
      if (last < first || last >= numPoints) return false;
      emitContour(out, flags, xs, ys, first, last, xOffset, yOffset, scale);
      first = last + 1;
    }
    return true;
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
  void emitContour(Path& out, const std::vector<uint8_t>& flags, const std::vector<int>& xs,
                   const std::vector<int>& ys, uint32_t first, uint32_t last, float ox, float oy,
                   float scale) const {
    const uint32_t count = last - first + 1;
    if (count < 2) return;

    auto onCurve = [&](uint32_t i) { return (flags[first + (i % count)] & 0x01) != 0; };
    auto px = [&](uint32_t i) { return ox + (float) xs[first + (i % count)] * scale; };
    // y NEGATED here: TrueType grows upward, every graphics API here grows
    // downward, and doing it once at the source means no caller ever has to.
    auto py = [&](uint32_t i) { return oy - (float) ys[first + (i % count)] * scale; };

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

  bool appendComposite(uint32_t at, Path& out, float ox, float oy, float scale, int depth) const {
    uint32_t p = at + 10;
    for (;;) {
      if ((size_t) p + 4u > data_.size()) return false;
      const uint16_t flags = readU16(p);
      const uint16_t index = readU16(p + 2);
      p += 4;
      int dx = 0, dy = 0;
      if (flags & 0x0001) { // ARG_1_AND_2_ARE_WORDS
        if ((size_t) p + 4u > data_.size()) return false;
        dx = (int16_t) readU16(p);
        dy = (int16_t) readU16(p + 2);
        p += 4;
      } else {
        if ((size_t) p + 2u > data_.size()) return false;
        dx = (int8_t) data_[p];
        dy = (int8_t) data_[p + 1];
        p += 2;
      }
      // Component scaling exists and is used by a few fonts for small caps.
      // Skipped over rather than applied: the offsets still land correctly,
      // which is the visible part, and a wrongly applied 2x2 is a letter of
      // the wrong shape.
      if (flags & 0x0008) p += 2;
      else if (flags & 0x0040) p += 4;
      else if (flags & 0x0080) p += 8;

      if (!appendGlyphImpl(index, out, ox + (float) dx * scale, oy - (float) dy * scale, scale,
                           depth + 1))
        return false;
      if (!(flags & 0x0020)) break; // MORE_COMPONENTS
    }
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

  std::vector<uint8_t> data_;
  std::string error_;
  bool ok_ = false;

  uint32_t headOffset_ = 0, maxpOffset_ = 0, hheaOffset_ = 0, hmtxOffset_ = 0;
  uint32_t cmapOffset_ = 0, locaOffset_ = 0, locaLength_ = 0, glyfOffset_ = 0, glyfLength_ = 0;
  uint32_t kernOffset_ = 0, kernPairs_ = 0;
  uint16_t cmapFormat_ = 0;

  int unitsPerEm_ = 0, numGlyphs_ = 0;
  int ascender_ = 0, descender_ = 0, lineGap_ = 0;
  int indexToLocFormat_ = 0;
  uint16_t numHMetrics_ = 0;
};

} // namespace gfx
} // namespace sonore
