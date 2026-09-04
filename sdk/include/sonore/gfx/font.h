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
#pragma once

#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include <vector>

#include "graphics.h"
#include "truetype.h"

namespace sonore {
namespace gfx {

enum class Justify { Left, Centred, Right };

class Font {
public:
  Font() = default;
  Font(std::shared_ptr<Typeface> face, float pointHeight)
      : face_(std::move(face)), height_(pointHeight) {}

  bool isValid() const { return face_ && face_->isValid() && height_ > 0.0f; }
  float height() const { return height_; }
  const std::shared_ptr<Typeface>& typeface() const { return face_; }

  Font withHeight(float newHeight) const { return Font(face_, newHeight); }

  /** Design units to pixels. */
  float scale() const {
    if (!isValid()) return 0.0f;
    return height_ / (float) face_->unitsPerEm();
  }

  float ascent() const { return isValid() ? (float) face_->ascender() * scale() : 0.0f; }
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
    float total = 0.0f;
    uint16_t previous = 0;
    size_t i = 0;
    while (i < utf8.size()) {
      const uint32_t cp = nextCodepoint(utf8, i);
      const uint16_t g = face_->glyphForChar(cp);
      if (previous) total += (float) face_->kerning(previous, g);
      total += (float) face_->advanceWidth(g);
      previous = g;
    }
    return total * scale();
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
    Path path;
    appendString(path, utf8, x, baselineY);
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
   * Breaks at spaces, and at explicit newlines, which are honoured rather than
   * treated as spaces -- a plugin's description has paragraphs in it and a
   * layout that ran them together would be wrong in a way nobody could fix
   * from the outside.
   *
   * A single WORD longer than the line is broken mid-word rather than allowed
   * to overflow. That is ugly and it is the right answer: the alternative is a
   * file path or a preset name running out of its panel and over the control
   * beside it.
   */
  std::vector<std::string> wrapText(const std::string& utf8, float maxWidth) const {
    std::vector<std::string> lines;
    if (utf8.empty()) return lines;
    if (!isValid() || maxWidth <= 0.0f) {
      lines.push_back(utf8);
      return lines;
    }

    std::string line;
    size_t at = 0;
    while (at <= utf8.size()) {
      // One word, plus whatever space preceded it.
      size_t wordStart = at;
      while (wordStart < utf8.size() && utf8[wordStart] == ' ') ++wordStart;
      size_t wordEnd = wordStart;
      while (wordEnd < utf8.size() && utf8[wordEnd] != ' ' && utf8[wordEnd] != '\n') ++wordEnd;

      const bool newlineNext = wordEnd < utf8.size() && utf8[wordEnd] == '\n';
      const std::string word = utf8.substr(wordStart, wordEnd - wordStart);

      if (!word.empty()) {
        const std::string candidate = line.empty() ? word : line + " " + word;
        if (stringWidth(candidate) <= maxWidth) {
          line = candidate;
        } else {
          if (!line.empty()) {
            lines.push_back(line);
            line.clear();
          }
          // The word alone still does not fit: break it. Ugly, and better than
          // a preset name running over the control beside it.
          if (stringWidth(word) > maxWidth) {
            std::string piece;
            size_t i = 0;
            while (i < word.size()) {
              const size_t from = i;
              nextCodepoint(word, i); // advances by a whole character, never a byte
              const std::string next = piece + word.substr(from, i - from);
              if (!piece.empty() && stringWidth(next) > maxWidth) {
                lines.push_back(piece);
                piece = word.substr(from, i - from);
              } else {
                piece = next;
              }
            }
            line = piece;
          } else {
            line = word;
          }
        }
      }

      if (newlineNext) {
        lines.push_back(line);
        line.clear();
        at = wordEnd + 1;
        continue;
      }
      if (wordEnd >= utf8.size()) break;
      at = wordEnd;
    }
    if (!line.empty()) lines.push_back(line);
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
    const float s = scale();
    float pen = x;
    uint16_t previous = 0;
    size_t i = 0;
    while (i < utf8.size()) {
      const uint32_t cp = nextCodepoint(utf8, i);
      const uint16_t glyph = face_->glyphForChar(cp);
      if (previous) pen += (float) face_->kerning(previous, glyph) * s;
      appendCachedGlyph(out, glyph, pen, baselineY, s);
      pen += (float) face_->advanceWidth(glyph) * s;
      previous = glyph;
    }
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
  /** The cache holds outlines in DESIGN units, so one entry serves every
   *  size. A cache keyed by size would be thrown away on every window
   *  resize, which is exactly when a UI can least afford it. */
  void appendCachedGlyph(Path& out, uint16_t glyph, float x, float y, float s) const {
    auto it = cache_->find(glyph);
    if (it == cache_->end()) {
      Path built;
      face_->appendGlyph(glyph, built, 0.0f, 0.0f, 1.0f);
      it = cache_->emplace(glyph, std::move(built)).first;
    }
    // Re-emitted through the transform. Appending a scaled copy per draw is
    // the price of a size-independent cache, and it is small next to
    // rasterising it.
    std::vector<Contour> contours;
    it->second.flatten(Transform::scaling(s, s).then(Transform::translation(x, y)), contours,
                       0.05f);
    for (const Contour& c : contours) {
      if (c.points.size() < 2) continue;
      out.moveTo(c.points[0].x, c.points[0].y);
      for (size_t k = 1; k < c.points.size(); ++k) out.lineTo(c.points[k].x, c.points[k].y);
      out.close();
    }
  }

  std::shared_ptr<Typeface> face_;
  float height_ = 0.0f;
  /** Shared between copies of the same Font, so passing one by value does not
   *  rebuild every glyph. */
  std::shared_ptr<std::map<uint16_t, Path>> cache_ =
      std::make_shared<std::map<uint16_t, Path>>();
};

} // namespace gfx
} // namespace sonore
