// SPDX-License-Identifier: Apache-2.0
//
// SVG, into Paths this rasteriser already knows how to draw.
//
// ── Why this is mostly a parser ─────────────────────────────────────────────
//
// Path can already do everything an SVG path describes: lines, quadratics,
// cubics, and closing. So none of this is rendering work -- it is reading a
// text format and turning it into calls that already exist. The one genuine
// piece of geometry is the arc command, which SVG states in ENDPOINT form (go
// to here, with these radii, the large way round) and every renderer needs in
// CENTRE form (a centre, a start angle, a sweep). That conversion is the only
// part of this file that is mathematics rather than bookkeeping.
//
// ── Why an SVG and not a PNG ────────────────────────────────────────────────
//
// A plugin editor is resized by the host, and it is resized to whatever the
// host feels like. A PNG logo at 2x is soft and at 0.5x is mush; the same logo
// as paths is exact at every size, and costs a few hundred bytes rather than a
// few hundred kilobytes.
//
// ── What is supported, and what is not ──────────────────────────────────────
//
// Supported: path, rect, circle, ellipse, line, polyline, polygon; groups with
// inherited presentation attributes; transform with translate, scale, rotate
// and matrix; viewBox; fill, stroke, stroke-width, fill-rule, opacity, and
// colours as #rgb, #rrggbb, a name from the small set below, or none.
//
// Not supported, and named rather than left to be discovered: text (it would
// need font matching against whatever the machine has, and an SVG whose text
// silently vanished would be worse than one that says it cannot), gradients,
// clip paths, masks, filters, patterns, and CSS in <style> blocks. A plugin's
// artwork is a logo and a set of icons; everything on that list belongs to
// illustration software.
#pragma once

#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

#include "colour.h"
#include "effects2d.h"
#include "graphics.h"
#include "path.h"
#include "stroke.h"

namespace sonore {
namespace gfx {

/** One filled and/or stroked shape out of an SVG. */
struct SvgShape {
  Path path;
  Colour fill;
  Colour stroke;
  float strokeWidth = 1.0f;
  bool hasFill = true;
  bool hasStroke = false;
  FillRule fillRule = FillRule::NonZero;
};

/**
 * An SVG, parsed into shapes.
 *
 * Nothing here holds text or a document tree: the parse flattens groups and
 * their transforms into absolute paths at load, so drawing is a loop over
 * shapes and costs nothing per frame. An editor redraws thirty times a second
 * and a logo must not be re-parsed on any of them.
 */
class Drawable {
public:
  /** Parses, or returns a Drawable with no shapes and a reason. */
  static Drawable parse(const std::string& svg) {
    Drawable out;
    Parser parser(svg, out);
    parser.run();
    return out;
  }

  bool isEmpty() const { return shapes_.empty(); }
  int numShapes() const { return (int) shapes_.size(); }
  const SvgShape& shapeAt(int i) const { return shapes_[(size_t) i]; }
  const std::string& error() const { return error_; }

  /** The viewBox, or the width/height attributes, or the bounds of what was
   *  parsed -- in that order of preference. */
  Rect viewBox() const { return viewBox_; }

  /**
   * Draw scaled to fit `area`, preserving the aspect ratio and centred.
   *
   * Preserving it is not a preference. A logo stretched to whatever rectangle a
   * host resized an editor to is the single most obvious sign that nobody
   * looked at the result.
   */
  void draw(Graphics& g, const Rect& area) const {
    if (shapes_.empty() || area.isEmpty()) return;
    const Rect box = viewBox_;
    if (box.w <= 0.0f || box.h <= 0.0f) return;

    // Contain, centred -- which is what RectanglePlacement means by its
    // defaults. Hand-rolled here before that existed, and WaveformView and
    // drawImage want the same rules.
    const RectanglePlacement placement;
    const Rect placed = placement.apply(Rect(0.0f, 0.0f, box.w, box.h), area);
    const float scale = placed.w / box.w;
    const float offsetX = placed.x - box.x * scale;
    const float offsetY = placed.y - box.y * scale;

    Graphics::ScopedState scope(g);
    g.addTransform(Transform::scaling(scale, scale).then(Transform::translation(offsetX,
                                                                                offsetY)));
    for (const SvgShape& shape : shapes_) {
      if (shape.hasFill && shape.fill.a > 0) {
        g.setColour(shape.fill);
        g.fillPath(shape.path, shape.fillRule);
      }
      if (shape.hasStroke && shape.stroke.a > 0 && shape.strokeWidth > 0.0f) {
        g.setColour(shape.stroke);
        // Multiplied by the scale, so a stroke authored at 1 unit in a 24-unit
        // viewBox stays proportionally the same weight when the logo is drawn
        // large. strokePath works in device pixels, which is right for a
        // hairline and wrong for artwork.
        g.strokePath(shape.path, StrokeStyle{shape.strokeWidth * scale, LineJoin::Round,
                                             LineCap::Round});
      }
    }
  }

private:
  friend class Parser;

  // ── The parser ───────────────────────────────────────────────────────────
  class Parser {
  public:
    Parser(const std::string& text, Drawable& out) : text_(text), out_(out) {}

    void run() {
      // A stack of inherited presentation state, one entry per open element.
      // SVG's attributes cascade, so a <g fill="red"> colours everything inside
      // it that does not say otherwise.
      state_.push_back(State{});

      size_t at = 0;
      bool sawSvg = false;
      while (at < text_.size()) {
        const size_t open = text_.find('<', at);
        if (open == std::string::npos) break;
        if (text_.compare(open, 4, "<!--") == 0) {
          const size_t end = text_.find("-->", open);
          at = end == std::string::npos ? text_.size() : end + 3;
          continue;
        }
        if (open + 1 < text_.size() && (text_[open + 1] == '?' || text_[open + 1] == '!')) {
          const size_t end = text_.find('>', open);
          at = end == std::string::npos ? text_.size() : end + 1;
          continue;
        }

        const size_t close = text_.find('>', open);
        if (close == std::string::npos) break;
        std::string tag = text_.substr(open + 1, close - open - 1);
        at = close + 1;

        const bool closing = !tag.empty() && tag[0] == '/';
        const bool selfClosing = !tag.empty() && tag.back() == '/';
        if (selfClosing) tag.pop_back();
        if (closing) {
          if (state_.size() > 1) state_.pop_back();
          continue;
        }

        const size_t nameEnd = tag.find_first_of(" \t\r\n");
        const std::string name = tag.substr(0, nameEnd);
        const std::string attributes =
            nameEnd == std::string::npos ? std::string() : tag.substr(nameEnd);

        State inherited = state_.back();
        applyAttributes(inherited, attributes);

        if (name == "svg") {
          sawSvg = true;
          readViewBox(attributes);
        } else {
          emit(name, attributes, inherited);
        }

        // A self-closing element inherits but does not open a scope. Pushing
        // for one and never popping is how a parser ends up applying a fill to
        // everything after it.
        if (!selfClosing) state_.push_back(inherited);
      }

      if (!sawSvg && out_.shapes_.empty()) out_.error_ = "no <svg> element found";
      if (out_.viewBox_.w <= 0.0f || out_.viewBox_.h <= 0.0f) out_.viewBox_ = boundsOfShapes();
    }

  private:
    struct State {
      Colour fill{0, 0, 0, 255};
      Colour stroke;
      float strokeWidth = 1.0f;
      bool hasFill = true;
      bool hasStroke = false;
      FillRule fillRule = FillRule::NonZero;
      Transform transform;
    };

    void readViewBox(const std::string& attributes) {
      const std::string box = attributeOf(attributes, "viewBox");
      if (!box.empty()) {
        std::vector<float> v = numbersIn(box);
        if (v.size() >= 4) {
          out_.viewBox_ = Rect(v[0], v[1], v[2], v[3]);
          return;
        }
      }
      const float w = toFloat(attributeOf(attributes, "width"));
      const float h = toFloat(attributeOf(attributes, "height"));
      if (w > 0.0f && h > 0.0f) out_.viewBox_ = Rect(0.0f, 0.0f, w, h);
    }

    void applyAttributes(State& s, const std::string& attributes) {
      const std::string fill = attributeOf(attributes, "fill");
      if (!fill.empty()) {
        if (fill == "none") s.hasFill = false;
        else {
          s.hasFill = true;
          s.fill = parseColour(fill, s.fill);
        }
      }
      const std::string stroke = attributeOf(attributes, "stroke");
      if (!stroke.empty()) {
        if (stroke == "none") s.hasStroke = false;
        else {
          s.hasStroke = true;
          s.stroke = parseColour(stroke, s.stroke);
        }
      }
      const std::string width = attributeOf(attributes, "stroke-width");
      if (!width.empty()) s.strokeWidth = toFloat(width);

      const std::string rule = attributeOf(attributes, "fill-rule");
      if (rule == "evenodd") s.fillRule = FillRule::EvenOdd;
      else if (rule == "nonzero") s.fillRule = FillRule::NonZero;

      // Opacity multiplies into the colours rather than being carried
      // separately, because the rasteriser composites one colour at a time and
      // an alpha kept beside it would have to be applied at every call site.
      const std::string opacity = attributeOf(attributes, "opacity");
      if (!opacity.empty()) {
        const float o = toFloat(opacity);
        s.fill = s.fill.withMultipliedAlpha(o);
        s.stroke = s.stroke.withMultipliedAlpha(o);
      }
      const std::string fillOpacity = attributeOf(attributes, "fill-opacity");
      if (!fillOpacity.empty()) s.fill = s.fill.withMultipliedAlpha(toFloat(fillOpacity));
      const std::string strokeOpacity = attributeOf(attributes, "stroke-opacity");
      if (!strokeOpacity.empty()) s.stroke = s.stroke.withMultipliedAlpha(toFloat(strokeOpacity));

      const std::string transform = attributeOf(attributes, "transform");
      // Applied BEFORE what is already there, because a child's transform is
      // relative to its parent's -- the same composition order the component
      // tree uses.
      if (!transform.empty()) s.transform = parseTransform(transform).then(s.transform);
    }

    void emit(const std::string& name, const std::string& attributes, const State& s) {
      Path path;
      if (name == "path") {
        parsePathData(attributeOf(attributes, "d"), path);
      } else if (name == "rect") {
        const float x = toFloat(attributeOf(attributes, "x"));
        const float y = toFloat(attributeOf(attributes, "y"));
        const float w = toFloat(attributeOf(attributes, "width"));
        const float h = toFloat(attributeOf(attributes, "height"));
        const float rx = toFloat(attributeOf(attributes, "rx"));
        if (w <= 0.0f || h <= 0.0f) return;
        if (rx > 0.0f) path.addRoundedRect(Rect(x, y, w, h), rx);
        else path.addRect(Rect(x, y, w, h));
      } else if (name == "circle") {
        const float cx = toFloat(attributeOf(attributes, "cx"));
        const float cy = toFloat(attributeOf(attributes, "cy"));
        const float r = toFloat(attributeOf(attributes, "r"));
        if (r <= 0.0f) return;
        path.addEllipse(Rect(cx - r, cy - r, r * 2.0f, r * 2.0f));
      } else if (name == "ellipse") {
        const float cx = toFloat(attributeOf(attributes, "cx"));
        const float cy = toFloat(attributeOf(attributes, "cy"));
        const float rx = toFloat(attributeOf(attributes, "rx"));
        const float ry = toFloat(attributeOf(attributes, "ry"));
        if (rx <= 0.0f || ry <= 0.0f) return;
        path.addEllipse(Rect(cx - rx, cy - ry, rx * 2.0f, ry * 2.0f));
      } else if (name == "line") {
        path.moveTo(toFloat(attributeOf(attributes, "x1")),
                    toFloat(attributeOf(attributes, "y1")));
        path.lineTo(toFloat(attributeOf(attributes, "x2")),
                    toFloat(attributeOf(attributes, "y2")));
      } else if (name == "polyline" || name == "polygon") {
        std::vector<float> v = numbersIn(attributeOf(attributes, "points"));
        if (v.size() < 4) return;
        path.moveTo(v[0], v[1]);
        for (size_t i = 2; i + 1 < v.size(); i += 2) path.lineTo(v[i], v[i + 1]);
        if (name == "polygon") path.close();
      } else {
        return; // not a shape: <g>, <defs>, <title>, anything else
      }

      if (path.isEmpty()) return;

      SvgShape shape;
      // Baked in at parse time rather than carried to draw time, so a redraw is
      // a loop over shapes and nothing else. An editor repaints thirty times a
      // second and a logo must not be re-transformed on every one.
      shape.path = path.transformed(s.transform);
      shape.fill = s.fill;
      shape.stroke = s.stroke;
      shape.strokeWidth = s.strokeWidth;
      shape.hasFill = s.hasFill;
      // A polyline with no explicit stroke is invisible, which is the one case
      // where SVG's default of "fill black, stroke none" produces nothing a
      // person expects to see.
      shape.hasStroke = s.hasStroke;
      shape.fillRule = s.fillRule;
      out_.shapes_.push_back(std::move(shape));
    }

    Rect boundsOfShapes() const {
      if (out_.shapes_.empty()) return Rect(0.0f, 0.0f, 1.0f, 1.0f);
      float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
      // The control points, not a flattened outline. A Bezier stays inside the
      // hull of its control points, so this box always contains the shape --
      // it can be slightly generous around a tight curve, which for choosing a
      // viewBox nobody supplied is the right way to be wrong.
      for (const SvgShape& shape : out_.shapes_)
        for (const Point& p : shape.path.points()) {
          if (p.x < x0) x0 = p.x;
          if (p.y < y0) y0 = p.y;
          if (p.x > x1) x1 = p.x;
          if (p.y > y1) y1 = p.y;
        }
      if (x1 <= x0 || y1 <= y0) return Rect(0.0f, 0.0f, 1.0f, 1.0f);
      return Rect(x0, y0, x1 - x0, y1 - y0);
    }

    // ── Attribute text ─────────────────────────────────────────────────────

    static std::string attributeOf(const std::string& attributes, const char* name) {
      const size_t nameLength = std::strlen(name);
      size_t at = 0;
      while (at < attributes.size()) {
        const size_t found = attributes.find(name, at);
        if (found == std::string::npos) return {};
        // A whole word, or `stroke` matches inside `stroke-width` and a shape
        // ends up with its line weight as its colour.
        const bool startOk = found == 0 || isSpace(attributes[found - 1]);
        size_t after = found + nameLength;
        while (after < attributes.size() && isSpace(attributes[after])) ++after;
        if (!startOk || after >= attributes.size() || attributes[after] != '=') {
          at = found + 1;
          continue;
        }
        ++after;
        while (after < attributes.size() && isSpace(attributes[after])) ++after;
        if (after >= attributes.size()) return {};
        const char quote = attributes[after];
        if (quote != '"' && quote != '\'') return {};
        const size_t end = attributes.find(quote, after + 1);
        if (end == std::string::npos) return {};
        return attributes.substr(after + 1, end - after - 1);
      }
      return {};
    }

    static bool isSpace(char c) {
      return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    }

    static float toFloat(const std::string& text) {
      if (text.empty()) return 0.0f;
      return (float) std::atof(text.c_str());
    }

    /** Numbers separated by anything that is not part of one. SVG allows
     *  commas, spaces, and nothing at all between a number and a following
     *  minus sign -- "10-5" is two numbers. */
    static std::vector<float> numbersIn(const std::string& text) {
      std::vector<float> out;
      size_t at = 0;
      while (at < text.size()) {
        while (at < text.size() && !isNumberStart(text[at])) ++at;
        if (at >= text.size()) break;
        size_t end = at;
        if (text[end] == '+' || text[end] == '-') ++end;
        while (end < text.size() && (isDigit(text[end]) || text[end] == '.')) ++end;
        if (end < text.size() && (text[end] == 'e' || text[end] == 'E')) {
          size_t exponent = end + 1;
          if (exponent < text.size() && (text[exponent] == '+' || text[exponent] == '-'))
            ++exponent;
          if (exponent < text.size() && isDigit(text[exponent])) {
            end = exponent;
            while (end < text.size() && isDigit(text[end])) ++end;
          }
        }
        if (end == at) {
          ++at;
          continue;
        }
        out.push_back((float) std::atof(text.substr(at, end - at).c_str()));
        at = end;
      }
      return out;
    }

    static bool isDigit(char c) { return c >= '0' && c <= '9'; }
    static bool isNumberStart(char c) {
      return isDigit(c) || c == '-' || c == '+' || c == '.';
    }

    // ── Colours ────────────────────────────────────────────────────────────

    static Colour parseColour(const std::string& text, Colour fallback) {
      std::string s;
      for (char c : text)
        if (!isSpace(c)) s.push_back(c);
      if (s.empty()) return fallback;

      if (s[0] == '#') {
        const std::string digits = s.substr(1);
        auto hex = [](char c) -> int {
          if (c >= '0' && c <= '9') return c - '0';
          if (c >= 'a' && c <= 'f') return c - 'a' + 10;
          if (c >= 'A' && c <= 'F') return c - 'A' + 10;
          return -1;
        };
        if (digits.size() == 3) {
          const int r = hex(digits[0]), g = hex(digits[1]), b = hex(digits[2]);
          if (r < 0 || g < 0 || b < 0) return fallback;
          // #abc means #aabbcc, not #a0b0c0 -- so #fff is white rather than
          // very light grey.
          return Colour((uint8_t) (r * 17), (uint8_t) (g * 17), (uint8_t) (b * 17), 255);
        }
        if (digits.size() >= 6) {
          int v[6];
          for (int i = 0; i < 6; ++i) {
            v[i] = hex(digits[(size_t) i]);
            if (v[i] < 0) return fallback;
          }
          return Colour((uint8_t) (v[0] * 16 + v[1]), (uint8_t) (v[2] * 16 + v[3]),
                        (uint8_t) (v[4] * 16 + v[5]), 255);
        }
        return fallback;
      }

      if (s.compare(0, 4, "rgb(") == 0) {
        std::vector<float> v = numbersIn(s.substr(4));
        if (v.size() >= 3)
          return Colour((uint8_t) clampByte(v[0]), (uint8_t) clampByte(v[1]),
                        (uint8_t) clampByte(v[2]), 255);
        return fallback;
      }

      // The shared table, not a copy. Thirteen names were hand-rolled here
      // before effects2d.h existed, which is always the sign that something
      // belongs somewhere else.
      Colour named;
      if (colours::byName(s, &named)) return named;
      return fallback;
    }

    static float clampByte(float v) { return v < 0.0f ? 0.0f : (v > 255.0f ? 255.0f : v); }

    // ── transform="" ───────────────────────────────────────────────────────

    static Transform parseTransform(const std::string& text) {
      Transform result;
      size_t at = 0;
      while (at < text.size()) {
        const size_t open = text.find('(', at);
        if (open == std::string::npos) break;
        std::string name = text.substr(at, open - at);
        std::string trimmed;
        for (char c : name)
          if (!isSpace(c) && c != ',') trimmed.push_back(c);
        const size_t close = text.find(')', open);
        if (close == std::string::npos) break;
        std::vector<float> v = numbersIn(text.substr(open + 1, close - open - 1));
        at = close + 1;

        Transform step;
        if (trimmed == "translate" && !v.empty()) {
          step = Transform::translation(v[0], v.size() > 1 ? v[1] : 0.0f);
        } else if (trimmed == "scale" && !v.empty()) {
          step = Transform::scaling(v[0], v.size() > 1 ? v[1] : v[0]);
        } else if (trimmed == "rotate" && !v.empty()) {
          const float radians = v[0] * 3.14159265358979323846f / 180.0f;
          if (v.size() >= 3) {
            // rotate(angle cx cy) is a rotation ABOUT a point, which is three
            // transforms. Treating it as a plain rotation puts the shape
            // somewhere else entirely.
            step = Transform::translation(-v[1], -v[2])
                       .then(Transform::rotation(radians))
                       .then(Transform::translation(v[1], v[2]));
          } else {
            step = Transform::rotation(radians);
          }
        } else if (trimmed == "matrix" && v.size() >= 6) {
          step = Transform{v[0], v[1], v[2], v[3], v[4], v[5]};
        } else {
          continue;
        }
        // Left to right, each applied before the ones already accumulated --
        // which is what SVG means by a list of transforms.
        result = step.then(result);
      }
      return result;
    }

    // ── The `d` attribute ──────────────────────────────────────────────────

    void parsePathData(const std::string& d, Path& path) {
      size_t at = 0;
      char command = 0;
      Point current, start;
      Point lastControl;
      char lastCommand = 0;

      auto skip = [&]() {
        while (at < d.size() && (isSpace(d[at]) || d[at] == ',')) ++at;
      };
      auto number = [&]() -> float {
        skip();
        const size_t from = at;
        if (at < d.size() && (d[at] == '+' || d[at] == '-')) ++at;
        while (at < d.size() && (isDigit(d[at]) || d[at] == '.')) ++at;
        if (at < d.size() && (d[at] == 'e' || d[at] == 'E')) {
          size_t exponent = at + 1;
          if (exponent < d.size() && (d[exponent] == '+' || d[exponent] == '-')) ++exponent;
          if (exponent < d.size() && isDigit(d[exponent])) {
            at = exponent;
            while (at < d.size() && isDigit(d[at])) ++at;
          }
        }
        if (at == from) return 0.0f;
        const double v = std::atof(d.substr(from, at - from).c_str());
        // "1e999" is a number the grammar accepts and geometry cannot hold:
        // it becomes infinity, and the first subtraction makes it NaN. The
        // rasteriser now refuses such coordinates too, but the parse is where
        // the file stops making sense, so it stops here.
        if (!std::isfinite(v) || std::fabs(v) > 1.0e18) {
          at = d.size();
          return 0.0f;
        }
        return (float) v;
      };
      auto hasNumber = [&]() {
        size_t probe = at;
        while (probe < d.size() && (isSpace(d[probe]) || d[probe] == ',')) ++probe;
        return probe < d.size() && isNumberStart(d[probe]);
      };
      /** The reflection of the last control point, which is what S and T mean
       *  by "smooth". With no previous curve of the right kind the reflection
       *  is the current point, so a stray S behaves like a plain C. */
      auto reflected = [&](char forCommand) {
        const bool cubic = forCommand == 'S';
        const bool previousMatched =
            cubic ? (lastCommand == 'C' || lastCommand == 'S')
                  : (lastCommand == 'Q' || lastCommand == 'T');
        if (!previousMatched) return current;
        return Point{2.0f * current.x - lastControl.x, 2.0f * current.y - lastControl.y};
      };

      while (at < d.size()) {
        skip();
        if (at >= d.size()) break;
        if (!isNumberStart(d[at])) {
          command = d[at];
          ++at;
        } else if (command == 0) {
          break; // numbers before any command: malformed
        } else if (command == 'M') {
          // A repeated M is an implicit L, which is in the spec and which a
          // parser that ignores it turns into a shape made of disconnected
          // dots.
          command = 'L';
        } else if (command == 'm') {
          command = 'l';
        }

        // Where this command started reading. Every command below consumes
        // its numbers, except Z -- and a Z followed by a digit (corrupt data,
        // or a hand-typed path) consumed nothing, so the loop closed the same
        // subpath forever while the verb list grew until memory ran out. The
        // fuzzer found it in ninety flips. SVG's own rule is that rendering
        // stops at the first error, which is also the only rule that ends.
        const size_t before = at;
        const bool relative = command >= 'a' && command <= 'z';
        const char upper = relative ? (char) (command - 32) : command;

        switch (upper) {
          case 'M': {
            const float x = number(), y = number();
            current = relative ? Point{current.x + x, current.y + y} : Point{x, y};
            start = current;
            path.moveTo(current.x, current.y);
            break;
          }
          case 'L': {
            const float x = number(), y = number();
            current = relative ? Point{current.x + x, current.y + y} : Point{x, y};
            path.lineTo(current.x, current.y);
            break;
          }
          case 'H': {
            const float x = number();
            current.x = relative ? current.x + x : x;
            path.lineTo(current.x, current.y);
            break;
          }
          case 'V': {
            const float y = number();
            current.y = relative ? current.y + y : y;
            path.lineTo(current.x, current.y);
            break;
          }
          case 'C': {
            const float x1 = number(), y1 = number();
            const float x2 = number(), y2 = number();
            const float x = number(), y = number();
            const Point c1 = relative ? Point{current.x + x1, current.y + y1} : Point{x1, y1};
            const Point c2 = relative ? Point{current.x + x2, current.y + y2} : Point{x2, y2};
            const Point end = relative ? Point{current.x + x, current.y + y} : Point{x, y};
            path.cubicTo(c1.x, c1.y, c2.x, c2.y, end.x, end.y);
            lastControl = c2;
            current = end;
            break;
          }
          case 'S': {
            const Point c1 = reflected('S');
            const float x2 = number(), y2 = number();
            const float x = number(), y = number();
            const Point c2 = relative ? Point{current.x + x2, current.y + y2} : Point{x2, y2};
            const Point end = relative ? Point{current.x + x, current.y + y} : Point{x, y};
            path.cubicTo(c1.x, c1.y, c2.x, c2.y, end.x, end.y);
            lastControl = c2;
            current = end;
            break;
          }
          case 'Q': {
            const float x1 = number(), y1 = number();
            const float x = number(), y = number();
            const Point c1 = relative ? Point{current.x + x1, current.y + y1} : Point{x1, y1};
            const Point end = relative ? Point{current.x + x, current.y + y} : Point{x, y};
            path.quadTo(c1.x, c1.y, end.x, end.y);
            lastControl = c1;
            current = end;
            break;
          }
          case 'T': {
            const Point c1 = reflected('T');
            const float x = number(), y = number();
            const Point end = relative ? Point{current.x + x, current.y + y} : Point{x, y};
            path.quadTo(c1.x, c1.y, end.x, end.y);
            lastControl = c1;
            current = end;
            break;
          }
          case 'A': {
            const float rx = number(), ry = number();
            const float rotation = number();
            const float largeArc = number(), sweep = number();
            const float x = number(), y = number();
            const Point end = relative ? Point{current.x + x, current.y + y} : Point{x, y};
            appendArc(path, current, end, rx, ry, rotation, largeArc != 0.0f, sweep != 0.0f);
            current = end;
            break;
          }
          case 'Z':
            path.close();
            current = start;
            break;
          default:
            // An unknown command with no way to know how many numbers follow.
            // Stopping is the only safe move: guessing consumes the wrong count
            // and every subsequent coordinate is off by one.
            return;
        }
        lastCommand = upper;
        if (at == before) return; // nothing consumed: the data has stopped making sense
      }
    }

    /**
     * The one piece of real geometry here.
     *
     * SVG states an arc in ENDPOINT form -- go to this point, with these radii,
     * the large way round, clockwise -- and a renderer needs it in CENTRE form:
     * a centre, a start angle and a sweep. This is the conversion from the SVG
     * specification's appendix F.6.5, including its two corrections: radii too
     * small to reach the endpoint are scaled UP until they can (rather than
     * producing a NaN), and a zero radius degenerates to a straight line.
     */
    static void appendArc(Path& path, Point from, Point to, float rx, float ry,
                          float degrees, bool largeArc, bool sweep) {
      if (rx == 0.0f || ry == 0.0f) {
        path.lineTo(to.x, to.y);
        return;
      }
      rx = std::fabs(rx);
      ry = std::fabs(ry);

      const float radians = degrees * 3.14159265358979323846f / 180.0f;
      const float cosR = std::cos(radians), sinR = std::sin(radians);

      const float dx = (from.x - to.x) * 0.5f, dy = (from.y - to.y) * 0.5f;
      const float x1 = cosR * dx + sinR * dy;
      const float y1 = -sinR * dx + cosR * dy;

      // Radii that cannot span the two points are scaled up until they exactly
      // can. The spec requires this; without it the square root below is of a
      // negative number and the whole path becomes NaN.
      const float lambda = (x1 * x1) / (rx * rx) + (y1 * y1) / (ry * ry);
      if (lambda > 1.0f) {
        const float scale = std::sqrt(lambda);
        rx *= scale;
        ry *= scale;
      }

      const float rxSq = rx * rx, rySq = ry * ry;
      const float x1Sq = x1 * x1, y1Sq = y1 * y1;
      float numerator = rxSq * rySq - rxSq * y1Sq - rySq * x1Sq;
      if (numerator < 0.0f) numerator = 0.0f;
      const float denominator = rxSq * y1Sq + rySq * x1Sq;
      float factor = denominator > 0.0f ? std::sqrt(numerator / denominator) : 0.0f;
      if (largeArc == sweep) factor = -factor;

      const float cx1 = factor * rx * y1 / ry;
      const float cy1 = -factor * ry * x1 / rx;
      const float cx = cosR * cx1 - sinR * cy1 + (from.x + to.x) * 0.5f;
      const float cy = sinR * cx1 + cosR * cy1 + (from.y + to.y) * 0.5f;

      auto angleOf = [](float ux, float uy, float vx, float vy) {
        const float dot = ux * vx + uy * vy;
        const float lengths = std::sqrt((ux * ux + uy * uy) * (vx * vx + vy * vy));
        if (lengths <= 0.0f) return 0.0f;
        float value = dot / lengths;
        if (value < -1.0f) value = -1.0f;
        if (value > 1.0f) value = 1.0f;
        const float angle = std::acos(value);
        return (ux * vy - uy * vx) < 0.0f ? -angle : angle;
      };

      const float startAngle = angleOf(1.0f, 0.0f, (x1 - cx1) / rx, (y1 - cy1) / ry);
      float sweepAngle = angleOf((x1 - cx1) / rx, (y1 - cy1) / ry, (-x1 - cx1) / rx,
                                 (-y1 - cy1) / ry);
      const float twoPi = 6.28318530717958647692f;
      if (!sweep && sweepAngle > 0.0f) sweepAngle -= twoPi;
      else if (sweep && sweepAngle < 0.0f) sweepAngle += twoPi;

      // Flattened into cubics, a quarter turn at a time. A single cubic cannot
      // approximate more than about that without visible error, and an arc of
      // three quarters drawn as one curve bulges where it should not.
      const int segments = (int) std::ceil(std::fabs(sweepAngle) / (twoPi / 4.0f));
      const int count = segments < 1 ? 1 : segments;
      const float step = sweepAngle / (float) count;
      const float alpha = 4.0f / 3.0f * std::tan(step * 0.25f);

      float angle = startAngle;
      for (int i = 0; i < count; ++i) {
        const float nextAngle = angle + step;
        const float cosA = std::cos(angle), sinA = std::sin(angle);
        const float cosB = std::cos(nextAngle), sinB = std::sin(nextAngle);

        auto place = [&](float ex, float ey) {
          return Point{cosR * rx * ex - sinR * ry * ey + cx,
                       sinR * rx * ex + cosR * ry * ey + cy};
        };
        const Point p1 = place(cosA - alpha * sinA, sinA + alpha * cosA);
        const Point p2 = place(cosB + alpha * sinB, sinB - alpha * cosB);
        const Point end = place(cosB, sinB);
        path.cubicTo(p1.x, p1.y, p2.x, p2.y, end.x, end.y);
        angle = nextAngle;
      }
    }

    const std::string& text_;
    Drawable& out_;
    std::vector<State> state_;
  };

  std::vector<SvgShape> shapes_;
  Rect viewBox_;
  std::string error_;
};

} // namespace gfx
} // namespace sonore
