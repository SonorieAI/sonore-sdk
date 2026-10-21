// SPDX-License-Identifier: Apache-2.0
//
// SVG, into Paths this rasteriser already knows how to draw.
//
// ── Why an SVG and not a PNG ────────────────────────────────────────────────
//
// A plugin editor is resized by the host, and it is resized to whatever the
// host feels like. A PNG logo at 2x is soft and at 0.5x is mush; the same logo
// as paths is exact at every size, and costs a few hundred bytes rather than a
// few hundred kilobytes.
//
// ── What arrives, and so what is read ───────────────────────────────────────
//
// The SVGs a plugin draws come out of Figma, Illustrator, Inkscape and the
// icon sets, and what those write is the list this reads:
//
//   - shapes: path, rect (with rx and ry), circle, ellipse, line, polyline,
//     polygon; groups, <a> and <switch>; nested <svg> viewports;
//   - <defs>, <symbol> and <use>, which is how Figma and Illustrator write a
//     shape used twice -- and drawn nowhere else, which is the point of them;
//   - fill and stroke as colours (#rgb, #rgba, #rrggbb, #rrggbbaa, rgb(),
//     rgba(), hsl(), hsla(), the CSS colour names), currentColor, and
//     url(#...) to a linear or radial gradient: stops, objectBoundingBox and
//     userSpaceOnUse, gradientTransform, spreadMethod, a focal point, and
//     xlink:href between gradients;
//   - stroke-width, -linecap, -linejoin, -miterlimit, -dasharray,
//     -dashoffset; fill-rule; opacity, fill-opacity, stroke-opacity;
//   - clip-path (clipPath with clipPathUnits and clip-rule) and mask
//     (luminance or alpha, maskUnits, maskContentUnits), which is what Figma
//     wraps every frame in;
//   - group opacity, drawn as a layer, so overlaps inside the group do not
//     show through each other;
//   - presentation attributes, style="", and <style> blocks with type,
//     class and id selectors -- Illustrator's default export is all classes;
//   - transform with matrix, translate, scale, rotate, skewX and skewY;
//     viewBox and preserveAspectRatio; lengths in px, pt, pc, mm, cm, in, em
//     and percentages; display and visibility;
//   - <text> and <tspan> (and <a> inside them): x, y, dx and dy, singly or
//     as a list per character; text-anchor per chunk; font-family, -size,
//     -weight and -style, matched to a face by FontMatcher (font_match.h)
//     with a variable font's weight axis used where it has one; fill and
//     stroke and every paint above, opacity, transforms, letter- and
//     word-spacing, xml:space; text inside a clipPath. Each run of text is
//     also kept as a DrawableText, so what an SVG said can be read back.
//
// Not read, and named rather than left to be discovered: <textPath>, rotate=
// and textLength on text, vertical text, <image>, patterns, markers, filters
// (a Figma drop shadow is a filter: the shape draws, without its shadow),
// blend modes, and CSS selectors beyond a type, classes and an id. An
// element with a filter is drawn as if it had none, which is nearer what was
// meant than not drawing it at all.
//
// ── Text, and the machine it is drawn on ────────────────────────────────────
//
// Text is turned into glyph outlines at parse time and drawn as shapes, so a
// clip, a mask, a gradient or a layer treats a letter exactly as it treats a
// path. WHICH outlines depends on the fonts: a family list is matched against
// the faces a plugin registered and then the machine's, as a browser does
// (font_match.h says how). Two machines with different fonts draw the same
// SVG differently -- which is why an SVG whose text must not move registers
// the face it was designed with and parses with a FontMatcher that looks at
// nothing else, or has its text converted to outlines before export.
//
// ── How it is drawn ─────────────────────────────────────────────────────────
//
// Parsed ONCE: the document becomes a list of shapes, already transformed into
// the root's coordinates, with their paints resolved -- and a short list of
// operations saying where a clip, a mask or a layer begins and ends. Drawing
// is a walk over that list. An editor repaints thirty times a second, and a
// logo must not be re-parsed, re-cascaded or re-transformed on any of them.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../number_text.h"
#include "colour.h"
#include "effects2d.h"
#include "font.h"
#include "font_match.h"
#include "gradient.h"
#include "graphics.h"
#include "parallelogram.h"
#include "path.h"
#include "stroke.h"

namespace sonore {
namespace gfx {

/**
 * One filled and/or stroked shape out of an SVG.
 *
 * Everything is in the document's coordinates -- the root viewBox's -- with
 * every transform above the shape already applied: the path, the stroke width
 * and the dashes, and the gradients.
 */
struct SvgShape {
  /** The outline, in the root viewBox's units. */
  Path path;
  /** The fill colour, with fill-opacity and the element's opacity already
   *  multiplied into its alpha. Used when `hasFill` is set and
   *  `fillIsGradient` is not; for currentColor it is the SVG's own `color`. */
  Colour fill;
  /** The stroke colour, stroke-opacity and opacity in its alpha, as `fill`. */
  Colour stroke;
  /** The pen's width in root units: stroke-width scaled by the transforms
   *  above the shape (the geometric mean of an uneven scale). Drawable::draw
   *  scales it again to device pixels, so a stroke keeps its weight at any
   *  size. */
  float strokeWidth = 1.0f;
  /** False when nothing fills it: fill="none", a colour with no alpha left,
   *  or a paint server that cannot be used and has no fallback. A shape that
   *  paints nothing is still kept and counted by numShapes(). */
  bool hasFill = true;
  /** True when it is stroked: a usable stroke paint and a width above 0. */
  bool hasStroke = false;
  /** The fill-rule. */
  FillRule fillRule = FillRule::NonZero;

  /** Painted with the gradient rather than the colour. */
  bool fillIsGradient = false;
  /** The stroke is painted with `strokeGradient` rather than `stroke`. */
  bool strokeIsGradient = false;
  /** The fill's gradient, in root units, with the opacities applied to its
   *  stops. Used when `fillIsGradient` is set. */
  ColourGradient fillGradient;
  /** The stroke's gradient, as `fillGradient`. */
  ColourGradient strokeGradient;

  /** The SVG said currentColor: drawn in the colour Drawable::draw is handed,
   *  when it is handed one, at this paint's opacity. */
  bool fillIsCurrentColour = false;
  /** The same for the stroke. */
  bool strokeIsCurrentColour = false;

  /** stroke-linejoin; miter-clip is read as miter. */
  LineJoin lineJoin = LineJoin::Miter;
  /** stroke-linecap. */
  LineCap lineCap = LineCap::Butt;
  /** stroke-miterlimit, a multiple of the stroke width; a value below 1 in
   *  the SVG is ignored and the inherited one kept. */
  float miterLimit = 4.0f;
  /** stroke-dasharray in root units, scaled as `strokeWidth` is: on, off,
   *  on, off. Empty is a solid line, which is also what a list with a
   *  negative length or one that sums to 0 becomes. */
  std::vector<float> dashes;
  /** stroke-dashoffset in root units; only used with `dashes`. */
  float dashOffset = 0.0f;
};

/**
 * A line of text in a box: the text, the Font it is drawn in, its colour,
 * and a Parallelogram it sits in -- turned or sheared with the box.
 *
 * Laid out as Font::drawIn lays a line out: justified across the box's top
 * edge, centred down its left edge by the font's ascent and descent rather
 * than by the glyphs a particular string happens to use. The box's edges are
 * measured in the font's own pixels, so a box twice as wide as the text is
 * room for justification, not a stretch.
 *
 * Drawable keeps one of these for every run of text in an SVG -- each piece
 * that has one style and one position -- so a caller can ask what an SVG
 * said, in which face, and where, after the parse has turned it to shapes.
 */
class DrawableText {
public:
  /** No text, no font, opaque black, left-justified, an empty box: draws
   *  nothing until it has a valid font, some text and a box with area. */
  DrawableText() = default;

  /** The line, as UTF-8. No line breaking: it is one line. */
  void setText(const std::string& utf8) { text_ = utf8; }
  /** The line, as UTF-8. For an SVG's text, the run's characters after
   *  whitespace handling (collapsed unless xml:space kept it). */
  const std::string& getText() const { return text_; }

  /** The face and size the line is drawn in. */
  void setFont(const Font& font) { font_ = font; }
  /** For an SVG's text, its height is the font-size scaled into the root's
   *  coordinates. */
  const Font& getFont() const { return font_; }

  /** The colour draw() fills with; fully transparent draws nothing. */
  void setColour(Colour c) { colour_ = c; }
  /** For an SVG's text, the fill colour with fill-opacity applied, and fully
   *  transparent when the fill is a gradient or none. */
  Colour getColour() const { return colour_; }

  /** Where the line sits across the box's width. */
  void setJustification(Justify j) { justify_ = j; }
  /** Left unless set; an SVG's runs are always Left, since their box is
   *  their own advance. */
  Justify getJustification() const { return justify_; }

  /** The box, in the coordinates the text is drawn in; its width and height
   *  are in the font's pixels, and turning or shearing it turns the text. */
  void setBoundingBox(const Parallelogram& box) { box_ = box; }
  /** For an SVG's text, the run's advance by the font's ascent plus
   *  descent, in the root's coordinates. */
  const Parallelogram& getBoundingBox() const { return box_; }

  /** The family list the text asked for, and the face's family it got --
   *  for an SVG's text, which is where the two differ. */
  void setRequestedFamily(const std::string& families) { requested_ = families; }
  /** The font-family list as the SVG wrote it, unparsed. */
  const std::string& getRequestedFamily() const { return requested_; }
  /** The family of the face FontMatcher chose for it. */
  const std::string& getMatchedFamily() const { return matched_; }
  /** Recorded only: it does not change the font. */
  void setMatchedFamily(const std::string& family) { matched_ = family; }

  /** The outlines of the text, laid out in its box. */
  Path toPath() const {
    Path out;
    if (!font_.isValid() || text_.empty()) return out;
    const float w = box_.getWidth(), h = box_.getHeight();
    if (!(w > 0.0f) || !(h > 0.0f)) return out;
    const float width = font_.stringWidth(text_);
    const float x = justify_ == Justify::Left ? 0.0f : (justify_ == Justify::Right ? w - width : (w - width) * 0.5f);
    const float baseline = (h - (font_.ascent() + font_.descent())) * 0.5f + font_.ascent();
    Path local;
    font_.appendString(local, text_, x, baseline);
    return local.transformed(box_.transformFrom(Rect(0.0f, 0.0f, w, h)));
  }

  /** Fills toPath() with the colour, non-zero, in the Graphics' current
   *  coordinates and clip. Nothing when the colour is fully transparent. */
  void draw(Graphics& g) const {
    if (colour_.isTransparent()) return;
    g.setColour(colour_);
    g.fillPath(toPath(), FillRule::NonZero);
  }

private:
  std::string text_;
  Font font_;
  Colour colour_{0, 0, 0, 255};
  Justify justify_ = Justify::Left;
  Parallelogram box_;
  std::string requested_, matched_;
};

/**
 * An SVG, parsed into shapes.
 *
 * Nothing here holds text or a document tree after the parse: drawing is a
 * loop over shapes, with the clips, masks and layers between them, and costs
 * nothing per frame that a hand-written paint() would not.
 */
class Drawable {
public:
  /** Parses, or returns a Drawable with no shapes and a reason. Text is
   *  matched against FontMatcher::shared(): the faces the plugin registered
   *  there, then the machine's. */
  static Drawable parse(const std::string& svg) { return parse(svg, FontMatcher::shared()); }

  /** The same, its text matched by `fonts` -- a FontMatcher that knows only
   *  the faces a plugin ships draws the same text on every machine. */
  static Drawable parse(const std::string& svg, const FontMatcher& fonts) {
    Drawable out;
    Parser parser(svg, out, fonts);
    parser.run();
    return out;
  }

  /** No shapes were kept: an empty document, or one that failed (error()
   *  says why). A shape that paints nothing still counts, so a document of
   *  invisible shapes is not empty though it draws nothing. */
  bool isEmpty() const { return shapes_.empty(); }
  /** The shapes drawn outside masks and clips, in drawing order. A shape that
   *  paints nothing (fill="none", no stroke) is still counted. */
  int numShapes() const { return (int) shapes_.size(); }
  /** Shape `i`, 0-based; `i` must be below numShapes() (not checked). It
   *  belongs to the Drawable and goes when the Drawable is destroyed or
   *  assigned to. */
  const SvgShape& shapeAt(int i) const { return shapes_[(size_t) i]; }
  /** Empty when the document was read whole. `no <svg> element found` when
   *  there was neither an `<svg>` element nor any shape; `drawn in part: ...`
   *  when the parser's limits stopped it, in which case the shapes before
   *  that point are kept and drawn. An `<svg>` with nothing in it is empty
   *  with no error. */
  const std::string& error() const { return error_; }

  /** The viewBox, or the width/height attributes, or the bounds of what was
   *  parsed -- in that order of preference. */
  Rect viewBox() const { return viewBox_; }

  /** The runs of text the SVG held, in document order: each one's words,
   *  face, colour and box, in the root's coordinates. Drawn as shapes like
   *  everything else; these say what those shapes were. */
  int numTexts() const { return (int) texts_.size(); }
  /** Run `i`, 0-based; `i` must be below numTexts() (not checked). Not
   *  kept here: text inside a mask or a clipPath, and a run no face was
   *  found for. */
  const DrawableText& textAt(int i) const { return texts_[(size_t) i]; }

  /**
   * Draw into `area`, placed as the root's preserveAspectRatio says -- which
   * by default is scaled to fit, the aspect ratio kept, and centred.
   *
   * Keeping the aspect ratio is not a preference. A logo stretched to
   * whatever rectangle a host resized an editor to is the single most
   * obvious sign that nobody looked at the result.
   */
  void draw(Graphics& g, const Rect& area) const { drawImpl(g, area, nullptr); }

  /**
   * The same, drawing currentColor in `currentColour`.
   *
   * Icon sets are drawn in currentColor so that whoever shows them chooses
   * the colour -- Lucide, Heroicons, Material Symbols, Feather all are. An
   * icon button draws its icon in its text colour this way, and the icon
   * follows the theme, the hover and the disabled state with it.
   */
  void draw(Graphics& g, const Rect& area, Colour currentColour) const {
    drawImpl(g, area, &currentColour);
  }

private:
  friend class Parser;

  /** preserveAspectRatio, the part of it that matters: where in the box, and
   *  whether to fit inside it or cover it. */
  struct Placement {
    int alignX = 1, alignY = 1; // 0 min, 1 mid, 2 max
    bool none = false;          // stretch to fill, aspect ratio and all
    bool slice = false;         // cover rather than fit
  };

  /** One step of drawing. A shape is the common one; the rest bracket them. */
  struct Op {
    enum Kind : uint8_t {
      Shape,           // shapes_[index], painted
      MaskShape,       // hidden_[index], painted -- inside a mask
      ClipShape,       // hidden_[index], filled opaque -- one shape of a clip
      ClipToShape,     // clip to hidden_[index] -- a clip with one shape
      Save,
      Restore,
      BeginLayer,      // with `opacity`
      EndLayer,
      BeginMask,
      EndMaskAlpha,
      EndMaskLuminance
    };
    Kind kind = Shape;
    int index = 0;
    float opacity = 1.0f;
  };

  /** The transform that puts `box` into `area` as `p` says. */
  static Transform placementTransform(const Rect& box, const Rect& area, const Placement& p) {
    if (!(box.w > 0.0f && box.h > 0.0f)) return Transform();
    float sx = area.w / box.w, sy = area.h / box.h;
    if (!p.none) {
      const float s = p.slice ? (std::max)(sx, sy) : (std::min)(sx, sy);
      sx = sy = s;
    }
    const float spareX = area.w - box.w * sx, spareY = area.h - box.h * sy;
    const float tx = area.x - box.x * sx + (p.none ? 0.0f : spareX * 0.5f * (float) p.alignX);
    const float ty = area.y - box.y * sy + (p.none ? 0.0f : spareY * 0.5f * (float) p.alignY);
    return Transform(sx, 0.0f, 0.0f, sy, tx, ty);
  }

  void drawImpl(Graphics& g, const Rect& area, const Colour* current) const {
    if (ops_.empty() || area.isEmpty()) return;
    const Rect box = viewBox_;
    if (box.w <= 0.0f || box.h <= 0.0f) return;

    Graphics::ScopedState scope(g);
    // The viewport is the area: what lies outside the viewBox but inside the
    // area shows, as it does in a browser, and nothing lands outside it.
    g.clipTo(area);
    g.addTransform(placementTransform(box, area, placement_));
    // Device pixels per document unit. strokePath works in device pixels --
    // right for a hairline, wrong for artwork -- so a stroke authored at 1
    // unit in a 24-unit icon is drawn at this many pixels: proportionally the
    // same weight at any size, and at any scale the editor is drawn at. This
    // was the area's scale alone, and at 200% every icon's strokes were half
    // as heavy as its fills.
    const float unit = g.state().transform.scaleFactor();
    const Colour white(255, 255, 255, 255);
    for (const Op& op : ops_) {
      switch (op.kind) {
        case Op::Shape: paintShape(g, shapes_[(size_t) op.index], unit, current); break;
        case Op::MaskShape: paintShape(g, hidden_[(size_t) op.index], unit, current); break;
        case Op::ClipShape:
          g.setColour(white);
          g.fillPath(hidden_[(size_t) op.index].path, hidden_[(size_t) op.index].fillRule);
          break;
        case Op::ClipToShape:
          g.clipToPath(hidden_[(size_t) op.index].path, hidden_[(size_t) op.index].fillRule);
          break;
        case Op::Save: g.save(); break;
        case Op::Restore: g.restore(); break;
        case Op::BeginLayer: g.beginLayer(op.opacity); break;
        case Op::EndLayer: g.endLayer(); break;
        case Op::BeginMask: g.beginMask(); break;
        case Op::EndMaskAlpha: g.endMask(Graphics::MaskFrom::Alpha); break;
        case Op::EndMaskLuminance: g.endMask(Graphics::MaskFrom::Luminance); break;
      }
    }
  }

  static Colour resolved(Colour c, bool isCurrent, const Colour* current) {
    if (!isCurrent || !current) return c;
    return Colour(current->r, current->g, current->b,
                  detail::multiplyCoverage(current->a, c.a));
  }

  static void paintShape(Graphics& g, const SvgShape& s, float unit, const Colour* current) {
    if (s.hasFill) {
      if (s.fillIsGradient) {
        g.setGradient(s.fillGradient);
        g.fillPath(s.path, s.fillRule);
      } else {
        const Colour c = resolved(s.fill, s.fillIsCurrentColour, current);
        if (c.a > 0) {
          g.setColour(c);
          g.fillPath(s.path, s.fillRule);
        }
      }
    }
    if (s.hasStroke && s.strokeWidth > 0.0f) {
      StrokeStyle style;
      style.width = s.strokeWidth * unit;
      style.join = s.lineJoin;
      style.cap = s.lineCap;
      style.miterLimit = s.miterLimit;
      if (!s.dashes.empty()) {
        style.dashes = s.dashes;
        for (float& d : style.dashes) d *= unit;
        style.dashOffset = s.dashOffset * unit;
      }
      if (s.strokeIsGradient) {
        g.setGradient(s.strokeGradient);
        g.strokePath(s.path, style);
      } else {
        const Colour c = resolved(s.stroke, s.strokeIsCurrentColour, current);
        if (c.a > 0) {
          g.setColour(c);
          g.strokePath(s.path, style);
        }
      }
    }
  }

  // ── The parser ───────────────────────────────────────────────────────────

  /**
   * Reads one SVG document into the Drawable it is given. Internal to
   * Drawable::parse, which makes one per document, calls run() once and lets
   * it go; the limits below are what keep a hostile document finite.
   *
   * Three passes. The text becomes a tree of elements; the `<style>` blocks
   * become rules; then the tree is walked from the root, each element's
   * properties cascaded from its parent's, and every shape it draws emitted
   * with the transforms and paints above it applied. The tree is needed
   * because a reference can point FORWARD: Figma writes its `<defs>` -- the
   * gradients and clip paths everything above them uses -- at the end.
   */
  class Parser {
  public:
    Parser(const std::string& text, Drawable& out, const FontMatcher& fonts)
        : text_(text), out_(out), fonts_(fonts) {}

    void run() {
      tokenize();
      readStyleSheets();
      for (int i = 0; i < (int) nodes_.size(); ++i) {
        const std::string id = attribute(i, "id");
        // The first of two elements with one id is the one a reference finds.
        if (!id.empty() && ids_.find(id) == ids_.end()) ids_[id] = i;
      }

      int root = -1;
      for (int i = 0; i < (int) nodes_.size(); ++i)
        if (nodes_[(size_t) i].name == "svg") {
          root = i;
          break;
        }

      Context top;
      if (root >= 0) {
        readRootViewport(root);
        top.viewport = Rect(0.0f, 0.0f, out_.viewBox_.w > 0.0f ? out_.viewBox_.w : 100.0f,
                            out_.viewBox_.h > 0.0f ? out_.viewBox_.h : 100.0f);
        const Local local = applyElement(root, top);
        if (local.display) renderChildren(root, top, 0, false);
      } else {
        // No <svg> at all: a fragment of shapes, drawn rather than refused --
        // what was here before read those too.
        top.viewport = Rect(0.0f, 0.0f, 100.0f, 100.0f);
        for (int i = 0; i < (int) nodes_.size(); ++i)
          if (nodes_[(size_t) i].parent < 0) render(i, top, 0, false);
        if (out_.shapes_.empty()) out_.error_ = "no <svg> element found";
      }
      if (exhausted_) out_.error_ = "drawn in part: the document is larger than the limits";

      if (out_.viewBox_.w <= 0.0f || out_.viewBox_.h <= 0.0f) out_.viewBox_ = boundsOfShapes();
    }

  private:
    // ── Limits ─────────────────────────────────────────────────────────────
    //
    // A document is data from wherever a preset or a skin came from. A use
    // that refers to a group of ten uses of a group of ten uses is ten to the
    // power of the depth; elements nested a hundred thousand deep overflow any
    // recursion. Each limit is far past an icon, a logo or a whole panel.
    static constexpr size_t kMaxElements = 200000;
    static constexpr size_t kMaxNesting = 256; // elements inside elements, as read
    // As drawn: each level is a few recursive calls with a cascaded style in
    // each, and an ASan build's frames are several times a release build's.
    static constexpr int kMaxDepth = 64;
    static constexpr size_t kMaxRendered = 100000; // elements visited, <use> expansions included
    static constexpr size_t kMaxPoints = 4000000;  // path points emitted
    static constexpr size_t kMaxRules = 4000;
    static constexpr size_t kMaxStyleWork = 4000000; // declarations kept and applied: spendStyle()
    static constexpr size_t kMaxReadBytes = 64u << 20; // attributes read, once per drawing: spendReading()
    static constexpr size_t kMaxCharacters = 200000; // laid out, over the whole document

    // ── The tree ───────────────────────────────────────────────────────────

    /** One element of the document, or a "#text" node of character data
     *  inside `<text>`: its attributes as written, and its place in the tree
     *  by index into nodes_. */
    struct Node {
      std::string name; // without a namespace prefix: svg:path is a path
      std::vector<std::pair<std::string, std::string>> attributes;
      std::vector<int> children;
      std::string text; // character data: <style>'s, and a "#text" node's
      int parent = -1;
    };

    /** Character data. Inside `<text>` it is content, and its place among the
     *  `<tspan>`s around it matters, so it becomes a "#text" node of its own
     *  in the tree; elsewhere only `<style>` reads it, and it is appended. */
    void characterData(const std::vector<int>& open, const std::vector<char>& inText, std::string data) {
      if (open.empty() || data.empty()) return;
      if (!inText.back()) {
        nodes_[(size_t) open.back()].text += data;
        return;
      }
      if (nodes_.size() >= kMaxElements) {
        exhausted_ = true;
        return;
      }
      Node node;
      node.name = "#text";
      node.text = std::move(data);
      node.parent = open.back();
      nodes_[(size_t) open.back()].children.push_back((int) nodes_.size());
      nodes_.push_back(std::move(node));
    }

    void tokenize() {
      const std::string& t = text_;
      const size_t n = t.size();
      size_t at = 0;
      std::vector<int> open;
      std::vector<char> inText; // per open element: inside a <text>
      while (at < n) {
        if (t[at] != '<') {
          const size_t next = t.find('<', at);
          const size_t end = next == std::string::npos ? n : next;
          characterData(open, inText, decodeEntities(t.substr(at, end - at)));
          at = end;
          continue;
        }
        if (t.compare(at, 4, "<!--") == 0) {
          const size_t end = t.find("-->", at + 4);
          at = end == std::string::npos ? n : end + 3;
          continue;
        }
        if (t.compare(at, 9, "<![CDATA[") == 0) {
          const size_t end = t.find("]]>", at + 9);
          const size_t stop = end == std::string::npos ? n : end;
          characterData(open, inText, t.substr(at + 9, stop - at - 9));
          at = end == std::string::npos ? n : end + 3;
          continue;
        }
        if (t.compare(at, 2, "<?") == 0) {
          const size_t end = t.find("?>", at + 2);
          at = end == std::string::npos ? n : end + 2;
          continue;
        }
        if (t.compare(at, 2, "<!") == 0) {
          // A DOCTYPE, with an internal subset in brackets that has '>' in it:
          // Illustrator's entity declarations.
          int depth = 0;
          size_t i = at + 2;
          for (; i < n; ++i) {
            if (t[i] == '[') ++depth;
            else if (t[i] == ']') --depth;
            else if (t[i] == '>' && depth <= 0) break;
          }
          at = i < n ? i + 1 : n;
          continue;
        }
        if (t.compare(at, 2, "</") == 0) {
          const size_t end = t.find('>', at);
          if (end == std::string::npos) break;
          std::string name = localName(trim(t.substr(at + 2, end - at - 2)));
          at = end + 1;
          // Closes the nearest open element of that name, and everything opened
          // inside it that never closed; a stray closing tag closes nothing.
          for (size_t k = open.size(); k-- > 0;)
            if (nodes_[(size_t) open[k]].name == name) {
              open.resize(k);
              inText.resize(k);
              break;
            }
          continue;
        }

        // An opening tag. Read with the quotes in mind: a '>' inside an
        // attribute value is not the end of the tag.
        size_t i = at + 1;
        const size_t nameStart = i;
        while (i < n && !isSpace(t[i]) && t[i] != '>' && t[i] != '/') ++i;
        if (i == nameStart) {
          at = i; // "<" followed by nothing a name is made of: text
          continue;
        }
        Node node;
        node.name = localName(t.substr(nameStart, i - nameStart));
        bool selfClosing = false, complete = false;
        while (i < n) {
          while (i < n && isSpace(t[i])) ++i;
          if (i >= n) break;
          if (t[i] == '>') {
            complete = true;
            ++i;
            break;
          }
          if (t[i] == '/') {
            if (i + 1 < n && t[i + 1] == '>') {
              selfClosing = complete = true;
              i += 2;
              break;
            }
            ++i;
            continue;
          }
          const size_t keyStart = i;
          while (i < n && !isSpace(t[i]) && t[i] != '=' && t[i] != '>' && t[i] != '/') ++i;
          std::string key = t.substr(keyStart, i - keyStart);
          while (i < n && isSpace(t[i])) ++i;
          std::string value;
          if (i < n && t[i] == '=') {
            ++i;
            while (i < n && isSpace(t[i])) ++i;
            if (i >= n) break;
            if (t[i] == '"' || t[i] == '\'') {
              const char quote = t[i];
              const size_t end = t.find(quote, i + 1);
              if (end == std::string::npos) {
                i = n; // an unterminated value: the tag never ends
                break;
              }
              value = decodeEntities(t.substr(i + 1, end - i - 1));
              i = end + 1;
            } else {
              const size_t valueStart = i;
              while (i < n && !isSpace(t[i]) && t[i] != '>') ++i;
              value = decodeEntities(t.substr(valueStart, i - valueStart));
            }
          }
          if (!key.empty()) node.attributes.emplace_back(std::move(key), std::move(value));
        }
        // A tag cut off by the end of the text is not an element: SVG stops
        // at the first error, and what came before it stands.
        if (!complete) break;
        at = i;

        if (nodes_.size() >= kMaxElements || open.size() >= kMaxNesting) {
          exhausted_ = true;
          break;
        }
        node.parent = open.empty() ? -1 : open.back();
        const int index = (int) nodes_.size();
        const bool text = node.name == "text" || (!inText.empty() && inText.back());
        nodes_.push_back(std::move(node));
        if (!open.empty()) nodes_[(size_t) open.back()].children.push_back(index);
        if (!selfClosing) {
          open.push_back(index);
          inText.push_back(text ? 1 : 0);
        }
      }
    }

    static std::string localName(const std::string& name) {
      const size_t colon = name.find(':');
      return colon == std::string::npos ? name : name.substr(colon + 1);
    }

    /** &lt; &gt; &amp; &quot; &apos; and numeric references. Anything else --
     *  an entity a DOCTYPE declared -- is left as written. */
    static std::string decodeEntities(const std::string& s) {
      if (s.find('&') == std::string::npos) return s;
      std::string out;
      out.reserve(s.size());
      for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') {
          out.push_back(s[i]);
          continue;
        }
        // The ';' is looked for only as far as a reference can reach. A find
        // to the end of the text, for every '&', made a document of ampersands
        // quadratic: a megabyte of them took 15 s to read, on the thread that
        // opens the editor.
        size_t semi = std::string::npos;
        for (size_t k = i + 1; k < s.size() && k - i <= 12; ++k)
          if (s[k] == ';') {
            semi = k;
            break;
          }
        if (semi == std::string::npos) {
          out.push_back('&');
          continue;
        }
        const std::string name = s.substr(i + 1, semi - i - 1);
        uint32_t cp = 0;
        bool known = true;
        if (name == "lt") cp = '<';
        else if (name == "gt") cp = '>';
        else if (name == "amp") cp = '&';
        else if (name == "quot") cp = '"';
        else if (name == "apos") cp = '\'';
        else if (name.size() > 1 && name[0] == '#') {
          const bool hex = name[1] == 'x' || name[1] == 'X';
          const char* digits = name.c_str() + (hex ? 2 : 1);
          char* end = nullptr;
          const unsigned long v = std::strtoul(digits, &end, hex ? 16 : 10);
          known = end && *end == '\0' && end != digits && v > 0 && v <= 0x10FFFF;
          cp = (uint32_t) v;
        } else {
          known = false;
        }
        if (!known) {
          out.push_back('&');
          continue;
        }
        appendUtf8(out, cp);
        i = semi;
      }
      return out;
    }

    static void appendUtf8(std::string& out, uint32_t cp) {
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

    /** An attribute by name, or "" -- xlink:href found as href too. */
    const std::string& attribute(int node, const char* name) const {
      static const std::string kNone;
      for (const auto& a : nodes_[(size_t) node].attributes)
        if (a.first == name) return a.second;
      return kNone;
    }

    bool hasAttribute(int node, const char* name) const {
      for (const auto& a : nodes_[(size_t) node].attributes)
        if (a.first == name) return true;
      return false;
    }

    const std::string& href(int node) const {
      const std::string& plain = attribute(node, "href");
      return plain.empty() ? attribute(node, "xlink:href") : plain;
    }

    /** The element `#id` names, or -1. */
    int lookup(const std::string& reference) const {
      std::string id = trim(reference);
      if (id.empty() || id[0] != '#') return -1;
      id.erase(0, 1);
      const auto found = ids_.find(id);
      return found == ids_.end() ? -1 : found->second;
    }

    /** `url(#id)` -> the element, or -1. */
    int lookupUrl(const std::string& value) const {
      const std::string v = trim(value);
      if (v.compare(0, 4, "url(") != 0) return -1;
      const size_t close = v.find(')');
      if (close == std::string::npos) return -1;
      std::string inner = trim(v.substr(4, close - 4));
      if (inner.size() >= 2 && (inner[0] == '"' || inner[0] == '\'')) inner = inner.substr(1, inner.size() - 2);
      return lookup(inner);
    }

    // ── CSS ────────────────────────────────────────────────────────────────
    //
    // Enough of it for what exporters write: a selector that is a type, an id,
    // classes, or the universal one, and lists of those; declarations with
    // !important. A selector with a combinator or a pseudo-class matches
    // nothing rather than everything.

    /** One simple selector -- a type, an id, classes, or any -- and its CSS
     *  specificity, which orders the rules that match. */
    struct Selector {
      std::string type; // "" or "*" for any
      std::string id;
      std::vector<std::string> classes;
      int specificity = 0;
    };
    /** One `name: value` of a rule or a style attribute, and whether it was
     *  marked !important. */
    struct Declaration {
      std::string name, value;
      bool important = false;
    };
    /** A selector and its declarations; `order` is its place in the
     *  document, which breaks a tie in specificity. The declarations are a
     *  block of blocks_, which every selector of one list shares: copied
     *  into each, "a,a,...{...}" -- four thousand selectors over one block
     *  -- turned an 18 KB style sheet into 700 MB. */
    struct Rule {
      Selector selector;
      size_t block = 0;
      int order = 0;
    };

    void readStyleSheets() {
      for (const Node& node : nodes_) {
        if (node.name != "style") continue;
        parseStyleSheet(node.text);
      }
      // Each rule filed under the one thing an element must have for it to
      // match -- its id, else its first class, else its type -- or with the
      // rules for any element. rules_ is complete, so the pointers hold.
      for (const Rule& r : rules_) {
        const Selector& s = r.selector;
        if (!s.id.empty()) rulesById_[s.id].push_back(&r);
        else if (!s.classes.empty()) rulesByClass_[s.classes.front()].push_back(&r);
        else if (!s.type.empty() && s.type != "*") rulesByType_[s.type].push_back(&r);
        else rulesForAny_.push_back(&r);
      }
    }

    void parseStyleSheet(const std::string& source) {
      // Comments out first; they may sit anywhere.
      std::string css;
      for (size_t i = 0; i < source.size(); ++i) {
        if (source.compare(i, 2, "/*") == 0) {
          const size_t end = source.find("*/", i + 2);
          if (end == std::string::npos) break;
          i = end + 1;
          continue;
        }
        css.push_back(source[i]);
      }
      size_t at = 0;
      while (at < css.size() && rules_.size() < kMaxRules) {
        while (at < css.size() && isSpace(css[at])) ++at;
        if (at >= css.size()) break;
        if (css[at] == '@') {
          // An at-rule: a block (@media, @font-face) skipped whole, braces
          // nested, or a statement (@import) to its semicolon -- whichever
          // comes first, found in one look. Looking for each to the end of
          // the sheet, a sheet of statements and no block read the rest of
          // itself once per statement: a megabyte of "@a;" took 4 s.
          const size_t stop = css.find_first_of("{;", at);
          if (stop == std::string::npos) break;
          if (css[stop] == ';') {
            at = stop + 1;
            continue;
          }
          const size_t brace = stop;
          int depth = 0;
          size_t i = brace;
          for (; i < css.size(); ++i) {
            if (css[i] == '{') ++depth;
            else if (css[i] == '}' && --depth == 0) break;
          }
          at = i + 1;
          continue;
        }
        const size_t open = css.find('{', at);
        if (open == std::string::npos) break;
        const size_t close = css.find('}', open);
        if (close == std::string::npos) break;
        const std::string selectors = css.substr(at, open - at);
        const size_t block = blocks_.size();
        blocks_.push_back(parseDeclarations(css.substr(open + 1, close - open - 1)));
        at = close + 1;
        size_t from = 0;
        while (from <= selectors.size()) {
          size_t comma = selectors.find(',', from);
          if (comma == std::string::npos) comma = selectors.size();
          Selector s;
          if (parseSelector(trim(selectors.substr(from, comma - from)), s) &&
              rules_.size() < kMaxRules) {
            Rule rule;
            rule.selector = std::move(s);
            rule.block = block;
            rule.order = (int) rules_.size();
            rules_.push_back(std::move(rule));
          }
          from = comma + 1;
        }
      }
    }

    static bool isNameChar(char c) {
      return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
             c == '-' || c == '_' || (unsigned char) c >= 0x80;
    }

    static bool parseSelector(const std::string& text, Selector& out) {
      if (text.empty()) return false;
      size_t i = 0;
      if (text[0] == '*') {
        out.type = "*";
        i = 1;
      } else if (isNameChar(text[0])) {
        while (i < text.size() && isNameChar(text[i])) ++i;
        out.type = text.substr(0, i);
        out.specificity += 1;
      }
      while (i < text.size()) {
        const char kind = text[i];
        if (kind != '.' && kind != '#') return false; // a combinator, an attribute, a pseudo-class
        const size_t start = ++i;
        while (i < text.size() && isNameChar(text[i])) ++i;
        if (i == start) return false;
        if (kind == '.') {
          out.classes.push_back(text.substr(start, i - start));
          out.specificity += 100;
        } else {
          out.id = text.substr(start, i - start);
          out.specificity += 10000;
        }
      }
      return true;
    }

    static std::vector<Declaration> parseDeclarations(const std::string& text) {
      std::vector<Declaration> out;
      size_t from = 0;
      while (from < text.size()) {
        size_t semi = text.find(';', from);
        if (semi == std::string::npos) semi = text.size();
        const std::string item = text.substr(from, semi - from);
        from = semi + 1;
        const size_t colon = item.find(':');
        if (colon == std::string::npos) continue;
        Declaration d;
        d.name = lower(trim(item.substr(0, colon)));
        d.value = trim(item.substr(colon + 1));
        const size_t bang = d.value.find('!');
        if (bang != std::string::npos) {
          d.important = lower(trim(d.value.substr(bang + 1))) == "important";
          d.value = trim(d.value.substr(0, bang));
        }
        if (!d.name.empty()) out.push_back(std::move(d));
      }
      return out;
    }

    /** Whether `s` picks out an element of this name, id and classes --
     *  `classes` sorted, so each of the selector's is a binary search. */
    static bool matches(const Selector& s, const std::string& name, const std::string& id,
                        const std::vector<std::string>& classes) {
      if (!s.type.empty() && s.type != "*" && s.type != name) return false;
      if (!s.id.empty() && id != s.id) return false;
      for (const std::string& wanted : s.classes)
        if (!std::binary_search(classes.begin(), classes.end(), wanted)) return false;
      return true;
    }

    /**
     * Everything that sets a property on this element, in the order it is
     * to be applied -- each overriding what came before. Worked out once a
     * node and kept: a `<use>` renders its target again for every reference,
     * and a 900 KB style attribute used a hundred times was parsed, and
     * matched against every rule, a hundred times -- 3.6 s for a 1 MB file.
     * The nodes are all made before anything renders, so the table is sized
     * once and a reference into it stays good.
     *
     * Kept as POINTERS -- into the style sheet's blocks and into the
     * element's own declarations, own_ -- and paid for from the style
     * budget. Kept as copies, every element held every declaration of every
     * rule it matched: four thousand "*{fill:red}" over a thousand elements,
     * a 48 KB file, came to 300 MB and 5 s, and over the hundred thousand
     * elements a document may draw it was tens of gigabytes. The element's
     * id and classes are read once, not once per rule: a long class list
     * under four thousand class rules took 16 s.
     */
    const std::vector<const Declaration*>& cascade(int node) const {
      if (cascaded_.size() != nodes_.size()) {
        cascaded_.assign(nodes_.size(), std::vector<const Declaration*>());
        own_.assign(nodes_.size(), std::vector<Declaration>());
        cascadeDone_.assign(nodes_.size(), 0);
      }
      std::vector<const Declaration*>& out = cascaded_[(size_t) node];
      if (cascadeDone_[(size_t) node]) return out;
      cascadeDone_[(size_t) node] = 1;
      // The element's own: its presentation attributes, then its style
      // attribute. Complete before anything points into it.
      std::vector<Declaration>& own = own_[(size_t) node];
      for (const auto& a : nodes_[(size_t) node].attributes)
        if (isProperty(a.first)) own.push_back({a.first, trim(a.second), false});
      const size_t presentation = own.size();
      for (Declaration& d : parseDeclarations(attribute(node, "style"))) own.push_back(std::move(d));

      std::vector<const Rule*> matched;
      if (!rules_.empty()) {
        // Separated by white space only, as a class list is.
        std::vector<std::string> classes;
        const std::string& list = attribute(node, "class");
        for (size_t i = 0; i < list.size();) {
          while (i < list.size() && isSpace(list[i])) ++i;
          const size_t start = i;
          while (i < list.size() && !isSpace(list[i])) ++i;
          if (i > start) classes.push_back(list.substr(start, i - start));
        }
        std::sort(classes.begin(), classes.end());
        classes.erase(std::unique(classes.begin(), classes.end()), classes.end());
        const std::string& id = attribute(node, "id");
        const std::string& name = nodes_[(size_t) node].name;
        // Only the rules filed under something this element has: a rule is
        // in one bucket and each bucket is looked in once, so none is seen
        // twice. Testing every rule against every element was four thousand
        // tests an element, 2.8 s over a hundred thousand of them. A sheet
        // can still file them all where one element looks -- four thousand
        // "x.y" for elements of class y -- so the tests are paid for too.
        size_t tested = 0;
        auto consider = [&](const std::vector<const Rule*>& bucket) {
          tested += bucket.size();
          for (const Rule* r : bucket)
            if (matches(r->selector, name, id, classes)) matched.push_back(r);
        };
        auto lookIn = [&](const std::unordered_map<std::string, std::vector<const Rule*>>& index,
                          const std::string& key) {
          const auto found = index.find(key);
          if (found != index.end()) consider(found->second);
        };
        consider(rulesForAny_);
        lookIn(rulesByType_, name);
        if (!id.empty()) lookIn(rulesById_, id);
        for (const std::string& c : classes) lookIn(rulesByClass_, c);
        if (!spendStyle(tested)) return out;
      }
      std::stable_sort(matched.begin(), matched.end(), [](const Rule* a, const Rule* b) {
        return a->selector.specificity != b->selector.specificity
                   ? a->selector.specificity < b->selector.specificity
                   : a->order < b->order;
      });
      auto take = [&](const Declaration& d) {
        if (!spendStyle(d.value.size())) return false;
        out.push_back(&d);
        return true;
      };
      for (size_t i = 0; i < presentation; ++i)
        if (!take(own[i])) return out;
      for (const Rule* r : matched)
        for (const Declaration& d : blocks_[r->block])
          if (!d.important && !take(d)) return out;
      for (size_t i = presentation; i < own.size(); ++i)
        if (!own[i].important && !take(own[i])) return out;
      for (const Rule* r : matched)
        for (const Declaration& d : blocks_[r->block])
          if (d.important && !take(d)) return out;
      for (size_t i = presentation; i < own.size(); ++i)
        if (own[i].important && !take(own[i])) return out;
      return out;
    }

    /**
     * The style budget. A declaration kept for an element or applied to one
     * costs one, and a sixteenth for each byte of its value -- which is what
     * reading it costs; matching an element costs one and a sixteenth for
     * each rule tested. Past kMaxStyleWork the document is exhausted, as it
     * is past the element and point budgets, and drawn as far as it got.
     * A `<use>` applies its target's style again each time, and a long value
     * -- a dash list, a transform -- is read again with it.
     */
    bool spendStyle(size_t reads) const {
      styleWork_ += 1 + reads / 16;
      if (styleWork_ <= kMaxStyleWork) return true;
      exhausted_ = true;
      return false;
    }

    static bool isProperty(const std::string& name) {
      static const char* const kNames[] = {
          "fill", "fill-opacity", "fill-rule", "stroke", "stroke-opacity", "stroke-width",
          "stroke-linecap", "stroke-linejoin", "stroke-miterlimit", "stroke-dasharray",
          "stroke-dashoffset", "color", "visibility", "display", "opacity", "clip-path",
          "clip-rule", "mask", "stop-color", "stop-opacity", "mask-type", "font-family",
          "font-size", "font-weight", "font-style", "text-anchor", "letter-spacing",
          "word-spacing"};
      for (const char* k : kNames)
        if (name == k) return true;
      return false;
    }

    // ── Properties ─────────────────────────────────────────────────────────

    /** A fill or stroke as SVG gives it: none, a colour, currentColor, or a
     *  url(#...) to a gradient element (`server`), with the colour to use
     *  when that element is missing or unusable. */
    struct Paint {
      enum Kind : uint8_t { None, Plain, Current, Url } kind = Plain;
      Colour colour{0, 0, 0, 255};
      int server = -1;            // the element url(#...) names
      bool hasFallback = false;   // url(#a) red
      Paint fallback() const {
        Paint p;
        p.kind = hasFallback ? fallbackKind : None;
        p.colour = fallbackColour;
        return p;
      }
      Kind fallbackKind = None;
      Colour fallbackColour;
    };

    /** The inherited properties: everything a child takes from its parent
     *  unless it says otherwise. */
    struct Style {
      Paint fill;
      Paint stroke = noPaint();
      float fillOpacity = 1.0f, strokeOpacity = 1.0f;
      float strokeWidth = 1.0f;
      FillRule fillRule = FillRule::NonZero;
      FillRule clipRule = FillRule::NonZero;
      LineJoin join = LineJoin::Miter;
      LineCap cap = LineCap::Butt;
      float miterLimit = 4.0f;
      std::vector<float> dashes;
      float dashOffset = 0.0f;
      Colour color{0, 0, 0, 255};
      bool visible = true;
      // Text. The family is a CSS list, "" for the matcher's default; the
      // size is in user units, CSS's medium by default.
      std::string fontFamily;
      float fontSize = 16.0f;
      int fontWeight = 400;
      bool italic = false;
      int anchor = 0; // start, middle, end
      float letterSpacing = 0.0f, wordSpacing = 0.0f;

      static Paint noPaint() {
        Paint p;
        p.kind = Paint::None;
        return p;
      }
    };

    /** The ones that are not inherited, read fresh for each element. */
    struct Local {
      float opacity = 1.0f;
      int clipPath = -1;
      int mask = -1;
      bool clipPathNamed = false; // named, even if to nothing that exists
      bool display = true;
      bool maskAlpha = false;     // mask-type: alpha
      Colour stopColour{0, 0, 0, 255};
      float stopOpacity = 1.0f;
    };

    /** What an element inherits as the walk goes down: the cascaded style,
     *  the transform to the root's user space, and the viewport percentages
     *  are taken against. Copied for each child, so a child's changes stay
     *  below it. */
    struct Context {
      Style style;
      Transform transform; // this element's user space -> the root's
      Rect viewport;       // for percentages: the nearest viewport's size
    };

    /** Reads an element's own properties onto a copy of its parent's. */
    Local applyElement(int node, Context& ctx) const {
      Local local;
      for (const Declaration* d : cascade(node)) {
        if (!spendStyle(d->value.size())) break;
        applyProperty(ctx, local, d->name, d->value);
      }
      const std::string& t = attribute(node, "transform");
      if (!t.empty() && spendStyle(t.size())) ctx.transform = parseTransform(t).then(ctx.transform);
      return local;
    }

    void applyProperty(Context& ctx, Local& local, const std::string& name,
                       const std::string& value) const {
      Style& s = ctx.style;
      if (value.empty() || value == "inherit") return;
      if (name == "fill") parsePaint(value, s.color, s.fill);
      else if (name == "stroke") parsePaint(value, s.color, s.stroke);
      else if (name == "fill-opacity") s.fillOpacity = opacityOf(value, s.fillOpacity);
      else if (name == "stroke-opacity") s.strokeOpacity = opacityOf(value, s.strokeOpacity);
      else if (name == "opacity") local.opacity = opacityOf(value, 1.0f);
      else if (name == "stroke-width") {
        float w = 0.0f;
        if (length(value, Axis::Other, ctx.viewport, w) && w >= 0.0f) s.strokeWidth = w;
      } else if (name == "fill-rule") {
        if (value == "evenodd") s.fillRule = FillRule::EvenOdd;
        else if (value == "nonzero") s.fillRule = FillRule::NonZero;
      } else if (name == "clip-rule") {
        if (value == "evenodd") s.clipRule = FillRule::EvenOdd;
        else if (value == "nonzero") s.clipRule = FillRule::NonZero;
      } else if (name == "stroke-linecap") {
        if (value == "butt") s.cap = LineCap::Butt;
        else if (value == "round") s.cap = LineCap::Round;
        else if (value == "square") s.cap = LineCap::Square;
      } else if (name == "stroke-linejoin") {
        if (value == "miter" || value == "miter-clip") s.join = LineJoin::Miter;
        else if (value == "round") s.join = LineJoin::Round;
        else if (value == "bevel") s.join = LineJoin::Bevel;
      } else if (name == "stroke-miterlimit") {
        float m = 0.0f;
        if (number(value, m) && m >= 1.0f) s.miterLimit = m;
      } else if (name == "stroke-dasharray") {
        s.dashes.clear();
        if (value == "none") return;
        bool valid = true;
        float total = 0.0f;
        forEachItem(value, [&](const std::string& item) {
          float v = 0.0f;
          if (!length(item, Axis::Other, ctx.viewport, v) || v < 0.0f) valid = false;
          s.dashes.push_back(v);
          total += v;
        });
        // A negative length makes the whole list invalid, and a pattern
        // that sums to nothing is solid -- both say so in the spec.
        if (!valid || !(total > 0.0f)) s.dashes.clear();
      } else if (name == "stroke-dashoffset") {
        float v = 0.0f;
        if (length(value, Axis::Other, ctx.viewport, v)) s.dashOffset = v;
      } else if (name == "color") {
        Colour c;
        if (parseColour(value, s.color, c)) s.color = c;
      } else if (name == "visibility") {
        s.visible = !(value == "hidden" || value == "collapse");
      } else if (name == "display") {
        local.display = value != "none";
      } else if (name == "clip-path") {
        if (value != "none") {
          local.clipPathNamed = true;
          local.clipPath = lookupUrl(value);
        }
      } else if (name == "mask") {
        if (value != "none") local.mask = lookupUrl(value);
      } else if (name == "mask-type") {
        local.maskAlpha = value == "alpha";
      } else if (name == "stop-color") {
        Colour c;
        if (lower(value) == "currentcolor") local.stopColour = s.color;
        else if (parseColour(value, s.color, c)) local.stopColour = c;
      } else if (name == "stop-opacity") {
        local.stopOpacity = opacityOf(value, 1.0f);
      } else if (name == "font-family") {
        s.fontFamily = value;
      } else if (name == "font-size") {
        float size = 0.0f;
        if (fontLength(value, s.fontSize, ctx.viewport, true, size) && size >= 0.0f) s.fontSize = size;
      } else if (name == "font-weight") {
        const std::string v = lower(trim(value));
        float w = 0.0f;
        if (v == "normal") s.fontWeight = 400;
        else if (v == "bold") s.fontWeight = 700;
        // CSS's relative weights, from the inherited one.
        else if (v == "bolder") s.fontWeight = s.fontWeight < 350 ? 400 : (s.fontWeight < 550 ? 700 : 900);
        else if (v == "lighter") s.fontWeight = s.fontWeight < 550 ? 100 : (s.fontWeight < 750 ? 400 : 700);
        else if (number(v, w) && w >= 1.0f && w <= 1000.0f) s.fontWeight = (int) (w + 0.5f);
      } else if (name == "font-style") {
        const std::string v = lower(trim(value));
        if (v == "normal") s.italic = false;
        else if (v == "italic" || v.compare(0, 7, "oblique") == 0) s.italic = true;
      } else if (name == "text-anchor") {
        if (value == "start") s.anchor = 0;
        else if (value == "middle") s.anchor = 1;
        else if (value == "end") s.anchor = 2;
      } else if (name == "letter-spacing" || name == "word-spacing") {
        float v = 0.0f;
        if (value == "normal") v = 0.0f;
        else if (!fontLength(value, s.fontSize, ctx.viewport, false, v)) return;
        (name == "letter-spacing" ? s.letterSpacing : s.wordSpacing) = v;
      }
    }

    /** A length where an em is the font's size: font-size itself (whose em
     *  and percentage are of the INHERITED size, and which has CSS's size
     *  keywords) and the spacings. */
    static bool fontLength(const std::string& text, float fontSize, const Rect& viewport, bool isSize, float& out) {
      const std::string v = lower(trim(text));
      if (isSize) {
        static const struct {
          const char* name;
          float px;
        } kKeywords[] = {{"xx-small", 9.0f}, {"x-small", 10.0f}, {"small", 13.0f},  {"medium", 16.0f},
                         {"large", 18.0f},   {"x-large", 24.0f}, {"xx-large", 32.0f}, {"xxx-large", 48.0f}};
        for (const auto& k : kKeywords)
          if (v == k.name) {
            out = k.px;
            return true;
          }
        if (v == "larger") {
          out = fontSize * 1.2f;
          return true;
        }
        if (v == "smaller") {
          out = fontSize / 1.2f;
          return true;
        }
      }
      double n = 0.0;
      size_t at = 0;
      while (at < v.size() && isSpace(v[at])) ++at;
      const size_t used = scanNumber(v, at, n);
      if (used == 0) return false;
      const std::string unit = trim(v.substr(at + used));
      if (unit == "em") out = (float) n * fontSize;
      else if (unit == "ex") out = (float) n * fontSize * 0.5f;
      else if (unit == "rem") out = (float) n * 16.0f;
      else if (unit == "%" && isSize) out = (float) n * 0.01f * fontSize;
      else return length(v, Axis::Other, viewport, out);
      return std::isfinite(out);
    }

    void parsePaint(const std::string& value, Colour current, Paint& out) const {
      const std::string v = trim(value);
      const std::string l = lower(v);
      if (l == "none") {
        out = Style::noPaint();
        return;
      }
      if (l == "currentcolor") {
        out = Paint{};
        out.kind = Paint::Current;
        out.colour = current;
        return;
      }
      if (l.compare(0, 4, "url(") == 0) {
        Paint p;
        p.kind = Paint::Url;
        p.server = lookupUrl(v);
        const size_t close = v.find(')');
        const std::string rest = close == std::string::npos ? std::string() : trim(v.substr(close + 1));
        if (!rest.empty()) {
          const std::string lr = lower(rest);
          Colour c;
          if (lr == "none") {
            p.hasFallback = true;
            p.fallbackKind = Paint::None;
          } else if (lr == "currentcolor") {
            p.hasFallback = true;
            p.fallbackKind = Paint::Current;
            p.fallbackColour = current;
          } else if (parseColour(rest, current, c)) {
            p.hasFallback = true;
            p.fallbackKind = Paint::Plain;
            p.fallbackColour = c;
          }
        }
        out = p;
        return;
      }
      Colour c;
      // A colour that does not parse is ignored, as an invalid presentation
      // attribute is: the inherited paint stands.
      if (parseColour(v, current, c)) {
        out = Paint{};
        out.kind = Paint::Plain;
        out.colour = c;
      }
    }

    static float opacityOf(const std::string& value, float fallback) {
      float v = 0.0f;
      const std::string t = trim(value);
      if (!number(t, v)) return fallback;
      if (!t.empty() && t.back() == '%') v *= 0.01f;
      return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    }

    // ── Numbers and lengths ────────────────────────────────────────────────

    enum class Axis { X, Y, Other };

    /** The number at the start of `text`, finite, or false. */
    static bool number(const std::string& text, float& out) {
      size_t at = 0;
      while (at < text.size() && isSpace(text[at])) ++at;
      double v = 0.0;
      const size_t used = scanNumber(text, at, v);
      if (used == 0) return false;
      out = (float) v;
      return true;
    }

    /** One number of SVG's grammar at `at`, which strtod's is not: no "inf",
     *  no "nan", no hexadecimal. Returns how many characters it used. */
    static size_t scanNumber(const std::string& text, size_t at, double& out) {
      size_t end = at;
      if (end < text.size() && (text[end] == '+' || text[end] == '-')) ++end;
      const size_t digitsFrom = end;
      while (end < text.size() && isDigit(text[end])) ++end;
      if (end < text.size() && text[end] == '.') {
        ++end;
        while (end < text.size() && isDigit(text[end])) ++end;
      }
      if (end == digitsFrom || (end == digitsFrom + 1 && text[digitsFrom] == '.')) return 0;
      if (end < text.size() && (text[end] == 'e' || text[end] == 'E')) {
        size_t exponent = end + 1;
        if (exponent < text.size() && (text[exponent] == '+' || text[exponent] == '-')) ++exponent;
        if (exponent < text.size() && isDigit(text[exponent])) {
          end = exponent;
          while (end < text.size() && isDigit(text[end])) ++end;
        }
      }
      double v = 0.0;
      if (numtext::parse(text.data() + at, end - at, &v) == 0) return 0;
      // "1e999" is a number the grammar accepts and geometry cannot hold.
      if (!std::isfinite(v) || std::fabs(v) > 1.0e18) return 0;
      out = v;
      return end - at;
    }

    /** A length with its unit, in user units. A percentage is of the
     *  viewport's width, height, or its normalised diagonal. */
    static bool length(const std::string& text, Axis axis, const Rect& viewport, float& out) {
      size_t at = 0;
      while (at < text.size() && isSpace(text[at])) ++at;
      double v = 0.0;
      const size_t used = scanNumber(text, at, v);
      if (used == 0) return false;
      const std::string unit = lower(trim(text.substr(at + used)));
      double scale = 1.0;
      if (unit.empty() || unit == "px") scale = 1.0;
      else if (unit == "%") {
        const double w = viewport.w, h = viewport.h;
        scale = 0.01 * (axis == Axis::X   ? w
                        : axis == Axis::Y ? h
                                          : std::sqrt((w * w + h * h) * 0.5));
      } else if (unit == "pt") scale = 96.0 / 72.0;
      else if (unit == "pc") scale = 16.0;
      else if (unit == "mm") scale = 96.0 / 25.4;
      else if (unit == "cm") scale = 96.0 / 2.54;
      else if (unit == "in") scale = 96.0;
      else if (unit == "em") scale = 16.0; // the default font size: nothing here sets another
      else if (unit == "ex") scale = 8.0;
      else return false;
      out = (float) (v * scale);
      return std::isfinite(out);
    }

    float lengthAttr(int node, const char* name, Axis axis, const Rect& viewport,
                     float fallback = 0.0f) const {
      float v = fallback;
      const std::string& a = attribute(node, name);
      if (a.empty() || !length(a, axis, viewport, v)) return fallback;
      return v;
    }

    /** A coordinate in a gradient or a clip's own units: in objectBoundingBox
     *  a plain fraction, "50%" being 0.5; otherwise a length. */
    static float unitLength(const std::string& text, bool boundingBox, Axis axis,
                            const Rect& viewport, float fallback) {
      if (text.empty()) return fallback;
      if (boundingBox) {
        float v = 0.0f;
        if (!number(text, v)) return fallback;
        if (trim(text).back() == '%') v *= 0.01f;
        return v;
      }
      float v = fallback;
      return length(text, axis, viewport, v) ? v : fallback;
    }

    template <typename F>
    static void forEachItem(const std::string& text, F f) {
      size_t at = 0;
      while (at < text.size()) {
        while (at < text.size() && (isSpace(text[at]) || text[at] == ',')) ++at;
        const size_t start = at;
        while (at < text.size() && !isSpace(text[at]) && text[at] != ',') ++at;
        if (at > start) f(text.substr(start, at - start));
      }
    }

    // ── Colours ────────────────────────────────────────────────────────────

    static bool parseColour(const std::string& text, Colour current, Colour& out) {
      std::string s;
      for (char c : text)
        if (!isSpace(c)) s.push_back(c);
      if (s.empty()) return false;
      const std::string l = lower(s);

      if (l[0] == '#') {
        auto hex = [](char c) -> int {
          if (c >= '0' && c <= '9') return c - '0';
          if (c >= 'a' && c <= 'f') return c - 'a' + 10;
          return -1;
        };
        const std::string digits = l.substr(1);
        int v[8];
        for (size_t i = 0; i < digits.size() && i < 8; ++i) {
          v[i] = hex(digits[i]);
          if (v[i] < 0) return false;
        }
        // #abc means #aabbcc, not #a0b0c0 -- so #fff is white rather than
        // very light grey. And #abcd is #aabbccdd.
        if (digits.size() == 3 || digits.size() == 4) {
          out = Colour((uint8_t) (v[0] * 17), (uint8_t) (v[1] * 17), (uint8_t) (v[2] * 17),
                       (uint8_t) (digits.size() == 4 ? v[3] * 17 : 255));
          return true;
        }
        if (digits.size() == 6 || digits.size() == 8) {
          out = Colour((uint8_t) (v[0] * 16 + v[1]), (uint8_t) (v[2] * 16 + v[3]),
                       (uint8_t) (v[4] * 16 + v[5]),
                       (uint8_t) (digits.size() == 8 ? v[6] * 16 + v[7] : 255));
          return true;
        }
        return false;
      }

      const size_t paren = l.find('(');
      if (paren != std::string::npos && l.back() == ')') {
        const std::string fn = l.substr(0, paren);
        std::vector<std::string> parts;
        // Commas, or spaces with a slash before the alpha: both CSS spellings.
        std::string inner = text.substr(text.find('(') + 1);
        inner = inner.substr(0, inner.rfind(')'));
        for (char& c : inner)
          if (c == ',' || c == '/') c = ' ';
        forEachItem(inner, [&](const std::string& item) { parts.push_back(item); });
        if (parts.size() < 3) return false;
        float alpha = 1.0f;
        if (parts.size() >= 4) {
          float a = 0.0f;
          if (!number(parts[3], a)) return false;
          if (parts[3].back() == '%') a *= 0.01f;
          alpha = a < 0.0f ? 0.0f : (a > 1.0f ? 1.0f : a);
        }
        const uint8_t a8 = (uint8_t) (alpha * 255.0f + 0.5f);
        if (fn == "rgb" || fn == "rgba") {
          float c[3];
          for (int i = 0; i < 3; ++i) {
            if (!number(parts[(size_t) i], c[i])) return false;
            if (parts[(size_t) i].back() == '%') c[i] *= 2.55f;
          }
          out = Colour(clampByte(c[0]), clampByte(c[1]), clampByte(c[2]), a8);
          return true;
        }
        if (fn == "hsl" || fn == "hsla") {
          float h = 0.0f, sat = 0.0f, light = 0.0f;
          if (!number(parts[0], h) || !number(parts[1], sat) || !number(parts[2], light)) return false;
          h = std::fmod(h, 360.0f);
          if (h < 0.0f) h += 360.0f;
          sat = (std::min)(1.0f, (std::max)(0.0f, sat * 0.01f));
          light = (std::min)(1.0f, (std::max)(0.0f, light * 0.01f));
          const float chroma = (1.0f - std::fabs(2.0f * light - 1.0f)) * sat;
          const float hp = h / 60.0f;
          const float x = chroma * (1.0f - std::fabs(std::fmod(hp, 2.0f) - 1.0f));
          float r = 0, g = 0, b = 0;
          const int sector = (int) hp; // 0..5: which sixth of the wheel
          const float rgb[6][3] = {{chroma, x, 0.0f}, {x, chroma, 0.0f}, {0.0f, chroma, x},
                                   {0.0f, x, chroma}, {x, 0.0f, chroma}, {chroma, 0.0f, x}};
          const int k = sector < 0 ? 0 : (sector > 5 ? 5 : sector);
          r = rgb[k][0];
          g = rgb[k][1];
          b = rgb[k][2];
          const float m = light - chroma * 0.5f;
          out = Colour(clampByte((r + m) * 255.0f), clampByte((g + m) * 255.0f),
                       clampByte((b + m) * 255.0f), a8);
          return true;
        }
        return false;
      }

      if (l == "currentcolor") {
        out = current;
        return true;
      }
      // The shared table, not a copy: the CSS names, every one of them.
      return colours::byName(l, &out);
    }

    static uint8_t clampByte(float v) {
      return (uint8_t) (!(v > 0.0f) ? 0.0f : (v > 255.0f ? 255.0f : v + 0.5f));
    }

    // ── Viewports ──────────────────────────────────────────────────────────

    static Placement parsePlacement(const std::string& text) {
      Placement p;
      std::string t = trim(text);
      if (t.compare(0, 5, "defer") == 0) t = trim(t.substr(5));
      if (t.compare(0, 4, "none") == 0) {
        p.none = true;
        return p;
      }
      if (t.size() >= 8 && t[0] == 'x' && t[4] == 'Y') {
        const std::string ax = t.substr(1, 3), ay = t.substr(5, 3);
        p.alignX = ax == "Min" ? 0 : ax == "Max" ? 2 : 1;
        p.alignY = ay == "Min" ? 0 : ay == "Max" ? 2 : 1;
      }
      p.slice = t.find("slice") != std::string::npos;
      return p;
    }

    bool viewBoxOf(int node, Rect& out) const {
      const std::string& box = attribute(node, "viewBox");
      if (box.empty()) return false;
      std::vector<float> v = numbersIn(box);
      if (v.size() < 4 || !(v[2] > 0.0f) || !(v[3] > 0.0f)) return false;
      out = Rect(v[0], v[1], v[2], v[3]);
      return true;
    }

    void readRootViewport(int root) {
      out_.placement_ = parsePlacement(attribute(root, "preserveAspectRatio"));
      Rect box;
      if (viewBoxOf(root, box)) {
        out_.viewBox_ = box;
        return;
      }
      // No viewBox: the width and height, with their units, or nothing --
      // and then the bounds of what was drawn.
      const Rect none;
      float w = 0.0f, h = 0.0f;
      const std::string& ws = attribute(root, "width");
      const std::string& hs = attribute(root, "height");
      if (!ws.empty() && ws.find('%') == std::string::npos) length(ws, Axis::X, none, w);
      if (!hs.empty() && hs.find('%') == std::string::npos) length(hs, Axis::Y, none, h);
      if (w > 0.0f && h > 0.0f) out_.viewBox_ = Rect(0.0f, 0.0f, w, h);
    }

    // ── Walking the tree ───────────────────────────────────────────────────

    /** Element names that are never drawn where they stand: they are drawn
     *  when something refers to them, or never. */
    static bool isNeverRendered(const std::string& name) {
      static const char* const kNames[] = {
          "defs", "symbol", "clipPath", "mask", "linearGradient", "radialGradient", "pattern",
          "marker", "style", "script", "title", "desc", "metadata", "filter", "foreignObject",
          "image", "font", "font-face", "view", "cursor", "stop", "animate", "set"};
      for (const char* k : kNames)
        if (name == k) return true;
      return false;
    }

    static bool isShape(const std::string& name) {
      return name == "path" || name == "rect" || name == "circle" || name == "ellipse" ||
             name == "line" || name == "polyline" || name == "polygon";
    }

    bool spend(size_t elements) {
      rendered_ += elements;
      if (rendered_ > kMaxRendered || points_ > kMaxPoints) exhausted_ = true;
      return !exhausted_;
    }

    /**
     * An element is read again -- its path data, its points, its viewBox --
     * each time it is drawn, and a `<use>` draws it again for every
     * reference: so each time is paid for by the size of its attributes,
     * from kMaxReadBytes. Counted as elements alone, one path of 200,000
     * closes used 5,000 times, a 480 KB file, took 7.5 s and 960 MB --
     * every copy kept its own 200,000 verbs -- and a megabyte used a hundred
     * thousand times was minutes and tens of gigabytes. A document drawn
     * once pays its own size.
     */
    bool spendReading(int node) const {
      size_t bytes = 0;
      for (const auto& a : nodes_[(size_t) node].attributes) bytes += a.first.size() + a.second.size();
      readBytes_ += bytes;
      if (readBytes_ <= kMaxReadBytes) return true;
      exhausted_ = true;
      return false;
    }

    void render(int node, const Context& parent, int depth, bool inMask) {
      if (depth > kMaxDepth || !spend(1) || !spendReading(node)) return;
      const Node& n = nodes_[(size_t) node];
      if (isNeverRendered(n.name)) return;
      const bool container = n.name == "g" || n.name == "a" || n.name == "switch" ||
                             n.name == "svg" || n.name == "use";
      const bool text = n.name == "text";
      if (!container && !isShape(n.name) && !text) return;

      Context ctx = parent;
      const Local local = applyElement(node, ctx);
      if (!local.display) return;
      if (local.opacity <= 0.0f) return;
      // A clip-path naming nothing that exists clips nothing -- what browsers
      // do, though the letter of SVG 1.1 says the element is not drawn.
      const int clip = local.clipPath >= 0 && nodes_[(size_t) local.clipPath].name == "clipPath"
                           ? local.clipPath : -1;
      const int mask = local.mask >= 0 && nodes_[(size_t) local.mask].name == "mask" ? local.mask : -1;

      // Opacity: folded into the paints for a shape that has only one of
      // fill and stroke -- exact, and free. Anything else is a layer, or the
      // fill would show through its own stroke.
      bool layer = local.opacity < 1.0f;
      float folded = 1.0f;
      if (layer && isShape(n.name)) {
        const bool fills = ctx.style.fill.kind != Paint::None;
        const bool strokes = ctx.style.stroke.kind != Paint::None && ctx.style.strokeWidth > 0.0f;
        if (!(fills && strokes)) {
          layer = false;
          folded = local.opacity;
        }
      }
      const bool bracket = layer || clip >= 0 || mask >= 0;
      if (bracket) {
        push(Op::Save);
        if (clip >= 0) emitClip(clip, node, ctx, depth, inMask);
        if (mask >= 0) emitMask(mask, node, ctx, depth, inMask);
        if (layer) {
          Op op;
          op.kind = Op::BeginLayer;
          op.opacity = local.opacity;
          out_.ops_.push_back(op);
        }
      }

      if (isShape(n.name)) {
        if (ctx.style.visible) emitShape(node, ctx, folded, inMask);
      } else if (text) {
        // Never folded: a tspan may stroke what its text only fills, and a
        // fill showing through its own stroke is what the layer prevents.
        emitText(node, ctx, inMask);
      } else if (n.name == "use") {
        renderUse(node, ctx, depth, inMask);
      } else if (n.name == "svg") {
        renderViewport(node, ctx, depth, inMask, Rect(0.0f, 0.0f, -1.0f, -1.0f));
      } else if (n.name == "switch") {
        for (int child : n.children) {
          const Node& c = nodes_[(size_t) child];
          if (c.name == "foreignObject" || !attribute(child, "requiredExtensions").empty())
            continue;
          if (isNeverRendered(c.name)) continue;
          render(child, ctx, depth + 1, inMask);
          break;
        }
      } else {
        renderChildren(node, ctx, depth, inMask);
      }

      if (bracket) {
        if (layer) push(Op::EndLayer);
        push(Op::Restore);
      }
    }

    void renderChildren(int node, const Context& ctx, int depth, bool inMask) {
      for (int child : nodes_[(size_t) node].children) {
        if (exhausted_) return;
        render(child, ctx, depth + 1, inMask);
      }
    }

    void push(Op::Kind kind, int index = 0) {
      Op op;
      op.kind = kind;
      op.index = index;
      out_.ops_.push_back(op);
    }

    /** Elements being expanded by a `<use>` right now: one that refers to
     *  itself, or to something that refers back, would never end. */
    bool expanding(int node) const {
      return std::find(active_.begin(), active_.end(), node) != active_.end();
    }

    void renderUse(int node, const Context& ctx, int depth, bool inMask) {
      const int target = lookup(href(node));
      if (target < 0 || expanding(target) || expanding(node)) return;
      // Nor anything that contains the <use> itself.
      for (int up = node; up >= 0; up = nodes_[(size_t) up].parent)
        if (up == target) return;
      // A symbol or an <svg> is read here rather than through render().
      const std::string& targetName = nodes_[(size_t) target].name;
      if ((targetName == "symbol" || targetName == "svg") && !spendReading(target)) return;
      Context inner = ctx;
      const float x = lengthAttr(node, "x", Axis::X, ctx.viewport);
      const float y = lengthAttr(node, "y", Axis::Y, ctx.viewport);
      inner.transform = Transform::translation(x, y).then(ctx.transform);
      active_.push_back(node);
      active_.push_back(target);
      const std::string& name = nodes_[(size_t) target].name;
      if (name == "symbol" || name == "svg") {
        // The <use>'s width and height size the symbol's viewport.
        Rect size(0.0f, 0.0f, -1.0f, -1.0f);
        if (hasAttribute(node, "width")) size.w = lengthAttr(node, "width", Axis::X, ctx.viewport, -1.0f);
        if (hasAttribute(node, "height")) size.h = lengthAttr(node, "height", Axis::Y, ctx.viewport, -1.0f);
        Context styled = inner;
        const Local local = applyElement(target, styled);
        if (local.display && local.opacity > 0.0f) {
          const bool layer = local.opacity < 1.0f;
          if (layer) {
            push(Op::Save);
            Op op;
            op.kind = Op::BeginLayer;
            op.opacity = local.opacity;
            out_.ops_.push_back(op);
          }
          renderViewport(target, styled, depth + 1, inMask, size);
          if (layer) {
            push(Op::EndLayer);
            push(Op::Restore);
          }
        }
      } else {
        render(target, inner, depth + 1, inMask);
      }
      active_.pop_back();
      active_.pop_back();
    }

    /**
     * A nested `<svg>`, or a `<symbol>` a `<use>` draws: a viewport of its own, the
     * viewBox fitted into it, and what falls outside it clipped away. `ctx`
     * has the element's own properties applied already; `size` is a `<use>`'s
     * width and height, negative where it gave none.
     */
    void renderViewport(int node, const Context& ctx, int depth, bool inMask, Rect size) {
      const Context& outer = ctx;
      // The x/y of a nested <svg> place it; a symbol's are its <use>'s.
      const bool nested = nodes_[(size_t) node].name == "svg";
      const float x = nested ? lengthAttr(node, "x", Axis::X, outer.viewport) : 0.0f;
      const float y = nested ? lengthAttr(node, "y", Axis::Y, outer.viewport) : 0.0f;
      float w = size.w >= 0.0f ? size.w : lengthAttr(node, "width", Axis::X, outer.viewport, -1.0f);
      float h = size.h >= 0.0f ? size.h : lengthAttr(node, "height", Axis::Y, outer.viewport, -1.0f);
      if (w < 0.0f) w = outer.viewport.w; // 100%
      if (h < 0.0f) h = outer.viewport.h;
      if (!(w > 0.0f) || !(h > 0.0f)) return;

      Rect box;
      Transform inside = Transform::translation(x, y);
      Rect viewport(0.0f, 0.0f, w, h);
      if (viewBoxOf(node, box)) {
        inside = placementTransform(box, Rect(x, y, w, h),
                                    parsePlacement(attribute(node, "preserveAspectRatio")));
        viewport = Rect(0.0f, 0.0f, box.w, box.h);
      }
      // overflow: hidden, the default for every viewport but the root's --
      // which the area clips.
      const std::string& overflow = attribute(node, "overflow");
      const bool clips = !(overflow == "visible" || overflow == "auto");
      if (clips) {
        push(Op::Save);
        Path rect;
        rect.addRect(Rect(x, y, w, h));
        pushClipPath(rect.transformed(ctx.transform), FillRule::NonZero);
      }
      Context content = ctx;
      content.transform = inside.then(ctx.transform);
      content.viewport = viewport;
      for (int child : nodes_[(size_t) node].children) {
        if (exhausted_) break;
        render(child, content, depth + 1, inMask);
      }
      if (clips) push(Op::Restore);
    }

    /** A clip with one shape: Graphics clips to it directly. */
    void pushClipPath(Path path, FillRule rule) {
      SvgShape shape;
      points_ += path.points().size();
      shape.path = std::move(path);
      shape.fillRule = rule;
      out_.hidden_.push_back(std::move(shape));
      push(Op::ClipToShape, (int) out_.hidden_.size() - 1);
    }

    // ── Shapes ─────────────────────────────────────────────────────────────

    /** The shape's outline in its own user space, or empty. */
    bool buildShape(int node, const Rect& viewport, Path& path) const {
      const std::string& name = nodes_[(size_t) node].name;
      if (name == "path") {
        parsePathData(attribute(node, "d"), path);
      } else if (name == "rect") {
        const float x = lengthAttr(node, "x", Axis::X, viewport);
        const float y = lengthAttr(node, "y", Axis::Y, viewport);
        const float w = lengthAttr(node, "width", Axis::X, viewport);
        const float h = lengthAttr(node, "height", Axis::Y, viewport);
        if (!(w > 0.0f) || !(h > 0.0f)) return false;
        float rx = lengthAttr(node, "rx", Axis::X, viewport, -1.0f);
        float ry = lengthAttr(node, "ry", Axis::Y, viewport, -1.0f);
        // One radius given is both; neither is none; each at most half a side.
        if (rx < 0.0f && ry < 0.0f) rx = ry = 0.0f;
        else if (rx < 0.0f) rx = ry;
        else if (ry < 0.0f) ry = rx;
        rx = (std::min)(rx, w * 0.5f);
        ry = (std::min)(ry, h * 0.5f);
        if (rx > 0.0f && ry > 0.0f) addRoundedRect(path, Rect(x, y, w, h), rx, ry);
        else path.addRect(Rect(x, y, w, h));
      } else if (name == "circle") {
        const float cx = lengthAttr(node, "cx", Axis::X, viewport);
        const float cy = lengthAttr(node, "cy", Axis::Y, viewport);
        const float r = lengthAttr(node, "r", Axis::Other, viewport);
        if (!(r > 0.0f)) return false;
        addEllipse(path, cx, cy, r, r);
      } else if (name == "ellipse") {
        const float cx = lengthAttr(node, "cx", Axis::X, viewport);
        const float cy = lengthAttr(node, "cy", Axis::Y, viewport);
        float rx = lengthAttr(node, "rx", Axis::X, viewport, -1.0f);
        float ry = lengthAttr(node, "ry", Axis::Y, viewport, -1.0f);
        if (rx < 0.0f) rx = ry; // SVG 2: auto is the other one
        if (ry < 0.0f) ry = rx;
        if (!(rx > 0.0f) || !(ry > 0.0f)) return false;
        addEllipse(path, cx, cy, rx, ry);
      } else if (name == "line") {
        path.moveTo(lengthAttr(node, "x1", Axis::X, viewport), lengthAttr(node, "y1", Axis::Y, viewport));
        path.lineTo(lengthAttr(node, "x2", Axis::X, viewport), lengthAttr(node, "y2", Axis::Y, viewport));
      } else if (name == "polyline" || name == "polygon") {
        std::vector<float> v = numbersIn(attribute(node, "points"));
        if (v.size() < 4) return false;
        path.moveTo(v[0], v[1]);
        for (size_t i = 2; i + 1 < v.size(); i += 2) path.lineTo(v[i], v[i + 1]);
        if (name == "polygon") path.close();
      } else {
        return false;
      }
      return !path.isEmpty();
    }

    /** From the rightmost point, clockwise on screen: where SVG starts a
     *  circle, so a dash pattern on one starts where a browser's does.
     *  Path::addEllipse starts at the top, and the dashes came out a
     *  quarter turn round. */
    static void addEllipse(Path& p, float cx, float cy, float rx, float ry) {
      const float kx = rx * 0.5522847498f, ky = ry * 0.5522847498f;
      p.moveTo(cx + rx, cy);
      p.cubicTo(cx + rx, cy + ky, cx + kx, cy + ry, cx, cy + ry);
      p.cubicTo(cx - kx, cy + ry, cx - rx, cy + ky, cx - rx, cy);
      p.cubicTo(cx - rx, cy - ky, cx - kx, cy - ry, cx, cy - ry);
      p.cubicTo(cx + kx, cy - ry, cx + rx, cy - ky, cx + rx, cy);
      p.close();
    }

    /** Corners that are quarter ellipses, rx by ry: Path's own rounded
     *  rectangle has one radius. Clockwise from the top edge, as SVG draws a
     *  rect, so a dash pattern starts where a browser's does. */
    static void addRoundedRect(Path& p, const Rect& r, float rx, float ry) {
      const float k = 0.5522847498f;
      const float x0 = r.x, y0 = r.y, x1 = r.right(), y1 = r.bottom();
      p.moveTo(x0 + rx, y0);
      p.lineTo(x1 - rx, y0);
      p.cubicTo(x1 - rx + rx * k, y0, x1, y0 + ry - ry * k, x1, y0 + ry);
      p.lineTo(x1, y1 - ry);
      p.cubicTo(x1, y1 - ry + ry * k, x1 - rx + rx * k, y1, x1 - rx, y1);
      p.lineTo(x0 + rx, y1);
      p.cubicTo(x0 + rx - rx * k, y1, x0, y1 - ry + ry * k, x0, y1 - ry);
      p.lineTo(x0, y0 + ry);
      p.cubicTo(x0, y0 + ry - ry * k, x0 + rx - rx * k, y0, x0 + rx, y0);
      p.close();
    }

    /** The geometry's bounds -- the curves', not their control points' --
     *  which is the box objectBoundingBox is measured in. */
    static Rect geometryBounds(const Path& path) {
      if (path.isEmpty()) return Rect();
      float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
      for (const Point& p : path.points()) {
        x0 = (std::min)(x0, p.x);
        y0 = (std::min)(y0, p.y);
        x1 = (std::max)(x1, p.x);
        y1 = (std::max)(y1, p.y);
      }
      const float extent = (std::max)(x1 - x0, y1 - y0);
      if (!(extent > 0.0f) || !std::isfinite(extent)) return Rect(x0, y0, 0.0f, 0.0f);
      std::vector<Contour> flat;
      path.flatten(Transform(), flat, extent * 0.0005f);
      float bx0 = 1e30f, by0 = 1e30f, bx1 = -1e30f, by1 = -1e30f;
      for (const Contour& c : flat)
        for (const Point& p : c.points) {
          bx0 = (std::min)(bx0, p.x);
          by0 = (std::min)(by0, p.y);
          bx1 = (std::max)(bx1, p.x);
          by1 = (std::max)(by1, p.y);
        }
      if (bx1 < bx0) return Rect(x0, y0, 0.0f, 0.0f);
      return Rect(bx0, by0, bx1 - bx0, by1 - by0);
    }

    void emitShape(int node, const Context& ctx, float opacity, bool inMask) {
      Path local;
      if (!buildShape(node, ctx.viewport, local)) return;
      if (points_ + local.points().size() > kMaxPoints) {
        exhausted_ = true;
        return;
      }
      points_ += local.points().size();
      emitPath(local, ctx, geometryBounds(local), opacity, inMask, ctx.style.fillRule);
    }

    /** A path in `ctx`'s user space, painted with `ctx.style` -- a shape's
     *  or a run of text's. `box` is what objectBoundingBox measures in: the
     *  shape's geometry, or the whole text element's. */
    void emitPath(const Path& local, const Context& ctx, const Rect& box, float opacity, bool inMask, FillRule rule) {
      const Style& s = ctx.style;
      SvgShape shape;
      shape.path = local.transformed(ctx.transform);
      shape.fillRule = rule;
      shape.hasFill = resolvePaint(s.fill, s.fillOpacity * opacity, box, ctx, shape.fill,
                                   shape.fillIsGradient, shape.fillGradient,
                                   shape.fillIsCurrentColour);
      // The stroke's width and dashes into the root's units too: the
      // transform scales the pen as it scales the path. An uneven scale has
      // no single answer; the geometric mean of its two is the one that keeps
      // the stroke's area right.
      const float scale = ctx.transform.scaleFactor();
      shape.strokeWidth = s.strokeWidth * scale;
      shape.hasStroke = shape.strokeWidth > 0.0f &&
                        resolvePaint(s.stroke, s.strokeOpacity * opacity, box, ctx, shape.stroke,
                                     shape.strokeIsGradient, shape.strokeGradient,
                                     shape.strokeIsCurrentColour);
      shape.lineJoin = s.join;
      shape.lineCap = s.cap;
      shape.miterLimit = s.miterLimit;
      if (!s.dashes.empty()) {
        shape.dashes = s.dashes;
        for (float& d : shape.dashes) d *= scale;
        shape.dashOffset = s.dashOffset * scale;
      }
      // Kept even when it paints nothing -- fill="none" and no stroke: it is
      // still a shape of the document, and shapeAt() counts it.
      if (inMask) {
        out_.hidden_.push_back(std::move(shape));
        push(Op::MaskShape, (int) out_.hidden_.size() - 1);
      } else {
        out_.shapes_.push_back(std::move(shape));
        push(Op::Shape, (int) out_.shapes_.size() - 1);
      }
    }

    /** A paint made concrete for one shape. False: nothing is painted. */
    bool resolvePaint(const Paint& paint, float opacity, const Rect& box, const Context& ctx,
                      Colour& colour, bool& isGradient, ColourGradient& gradient,
                      bool& isCurrent) const {
      isGradient = isCurrent = false;
      Paint p = paint;
      if (p.kind == Paint::Url) {
        const int server = p.server;
        const std::string name = server >= 0 ? nodes_[(size_t) server].name : std::string();
        if (name == "linearGradient" || name == "radialGradient") {
          Colour solid;
          const int got = buildGradient(server, box, ctx, opacity, gradient, solid);
          if (got == 2) {
            isGradient = true;
            return true;
          }
          if (got == 1) {
            colour = solid;
            return solid.a > 0;
          }
          // No stops, or a box with no area to measure the gradient in.
          p = p.fallback();
        } else {
          // A pattern, or nothing at all: the fallback colour, if any.
          p = p.fallback();
        }
      }
      if (p.kind == Paint::None) return false;
      if (p.kind == Paint::Current) {
        isCurrent = true;
        colour = ctx.style.color;
      } else {
        colour = p.colour;
      }
      colour = colour.withMultipliedAlpha(opacity);
      // A transparent colour is still a paint when it is currentColor: the
      // caller may draw it in a colour that is not.
      return colour.a > 0 || (isCurrent && opacity > 0.0f);
    }

    /**
     * A gradient for a shape whose geometry spans `box` in its own user space.
     * 2: a gradient. 1: one colour (a single stop, or a ramp of no length,
     * which SVG says is drawn in the last stop's colour). 0: nothing.
     */
    int buildGradient(int server, const Rect& box, const Context& ctx, float opacity,
                      ColourGradient& out, Colour& solid) const {
      // Attributes and stops follow xlink:href to the gradient they came
      // from: Illustrator writes the stops once and points every gradient at
      // them. Nearest first; a loop ends where it repeats.
      std::vector<int> chain;
      for (int g = server; g >= 0 && chain.size() < 16; g = lookup(href(g))) {
        const std::string& name = nodes_[(size_t) g].name;
        if (name != "linearGradient" && name != "radialGradient") break;
        if (std::find(chain.begin(), chain.end(), g) != chain.end()) break;
        chain.push_back(g);
      }
      // Read again for every shape filled: paid for as an element drawn is.
      for (int g : chain)
        if (!spendReading(g)) return 0;
      auto inherited = [&](const char* name) -> const std::string& {
        static const std::string kNone;
        for (int g : chain)
          if (hasAttribute(g, name)) return attribute(g, name);
        return kNone;
      };
      int stopsFrom = -1;
      for (int g : chain) {
        for (int child : nodes_[(size_t) g].children)
          if (nodes_[(size_t) child].name == "stop") {
            stopsFrom = g;
            break;
          }
        if (stopsFrom >= 0) break;
      }
      if (stopsFrom < 0) return 0;

      // The stops, offsets made monotonic as the spec asks: each at least the
      // one before.
      std::vector<ColourStop> stops;
      float last = 0.0f;
      for (int child : nodes_[(size_t) stopsFrom].children) {
        if (nodes_[(size_t) child].name != "stop") continue;
        // Every shape filled with a gradient gets its own copy of the stops,
        // so each stop is paid for from the style budget, every time: 60,000
        // stops filling 8,000 shapes, a 760 KB file, took 78 s and 3.7 GB.
        if (!spendStyle(0) || !spendReading(child)) return 0;
        Context stopCtx = ctx;
        Local local;
        for (const Declaration* d : cascade(child)) {
          if (!spendStyle(d->value.size())) break;
          applyProperty(stopCtx, local, d->name, d->value);
        }
        float offset = 0.0f;
        const std::string& o = attribute(child, "offset");
        if (number(o, offset) && !o.empty() && trim(o).back() == '%') offset *= 0.01f;
        offset = offset < last ? last : (offset > 1.0f ? 1.0f : offset);
        last = offset;
        stops.push_back({offset, local.stopColour.withMultipliedAlpha(local.stopOpacity * opacity)});
      }
      if (stops.empty()) return 0;
      if (stops.size() == 1) {
        solid = stops[0].colour;
        return 1;
      }

      const bool boundingBox = inherited("gradientUnits") != "userSpaceOnUse";
      if (boundingBox && (!(box.w > 0.0f) || !(box.h > 0.0f))) return 0;
      const bool radial = nodes_[(size_t) server].name == "radialGradient";
      const Rect& vp = ctx.viewport;
      Point p1, p2, focal;
      bool hasFocal = false;
      if (radial) {
        const float cx = unitLength(inherited("cx"), boundingBox, Axis::X, vp, boundingBox ? 0.5f : vp.w * 0.5f);
        const float cy = unitLength(inherited("cy"), boundingBox, Axis::Y, vp, boundingBox ? 0.5f : vp.h * 0.5f);
        const float r = unitLength(inherited("r"), boundingBox, Axis::Other, vp,
                                   boundingBox ? 0.5f : 0.5f * std::sqrt((vp.w * vp.w + vp.h * vp.h) * 0.5f));
        if (!(r > 0.0f)) {
          solid = stops.back().colour;
          return 1;
        }
        p1 = Point(cx, cy);
        p2 = Point(cx + r, cy);
        const std::string& fx = inherited("fx");
        const std::string& fy = inherited("fy");
        if (!fx.empty() || !fy.empty()) {
          focal = Point(unitLength(fx, boundingBox, Axis::X, vp, cx), unitLength(fy, boundingBox, Axis::Y, vp, cy));
          hasFocal = focal.x != cx || focal.y != cy;
        }
      } else {
        p1 = Point(unitLength(inherited("x1"), boundingBox, Axis::X, vp, 0.0f),
                   unitLength(inherited("y1"), boundingBox, Axis::Y, vp, 0.0f));
        p2 = Point(unitLength(inherited("x2"), boundingBox, Axis::X, vp, boundingBox ? 1.0f : vp.w),
                   unitLength(inherited("y2"), boundingBox, Axis::Y, vp, 0.0f));
        if (p1.x == p2.x && p1.y == p2.y) {
          solid = stops.back().colour;
          return 1;
        }
      }

      ColourGradient g(stops.front().colour, p1, stops.back().colour, p2, radial);
      g.clearStops();
      for (const ColourStop& s : stops) g.addStop(s.position, s.colour);
      if (hasFocal) g.setFocalPoint(focal);
      const std::string& spread = inherited("spreadMethod");
      if (spread == "reflect") g.setSpread(GradientSpread::Reflect);
      else if (spread == "repeat") g.setSpread(GradientSpread::Repeat);

      // The gradient's own space, then the box it is measured in, then the
      // shape's user space into the root's.
      Transform map;
      const std::string& gt = inherited("gradientTransform");
      if (!gt.empty()) map = parseTransform(gt);
      if (boundingBox) map = map.then(Transform(box.w, 0.0f, 0.0f, box.h, box.x, box.y));
      out = g.transformed(map.then(ctx.transform));
      return 2;
    }

    // ── Clips and masks ────────────────────────────────────────────────────

    /** The bounds of what an element draws, in its own user space -- what
     *  objectBoundingBox means for a group. */
    Rect elementBounds(int node, const Context& ctx, int depth) const {
      float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
      boundsInto(node, Transform(), ctx.viewport, depth, x0, y0, x1, y1);
      if (x1 < x0) return Rect();
      return Rect(x0, y0, x1 - x0, y1 - y0);
    }

    void boundsInto(int node, const Transform& t, const Rect& viewport, int depth, float& x0,
                    float& y0, float& x1, float& y1) const {
      if (depth > kMaxDepth) return;
      const Node& n = nodes_[(size_t) node];
      if (isShape(n.name)) {
        Path p;
        if (!buildShape(node, viewport, p)) return;
        const Rect b = geometryBounds(p.transformed(t));
        x0 = (std::min)(x0, b.x);
        y0 = (std::min)(y0, b.y);
        x1 = (std::max)(x1, b.right());
        y1 = (std::max)(y1, b.bottom());
        return;
      }
      if (n.name == "g" || n.name == "a" || n.name == "switch")
        for (int child : n.children) {
          Transform ct = t;
          const std::string& tr = attribute(child, "transform");
          if (!tr.empty()) ct = parseTransform(tr).then(t);
          boundsInto(child, ct, viewport, depth + 1, x0, y0, x1, y1);
        }
    }

    void emitClip(int clip, int element, const Context& ctx, int depth, bool inMask) {
      if (expanding(clip)) return;
      // The clip's coordinates: the element's user space, or its bounding
      // box when clipPathUnits says so -- and the clipPath's own transform.
      Transform base = ctx.transform;
      if (attribute(clip, "clipPathUnits") == "objectBoundingBox") {
        const std::string& elementName = nodes_[(size_t) element].name;
        const Rect b = isShape(elementName) ? shapeBounds(element, ctx)
                       : elementName == "text" ? textBounds(element, ctx)
                                               : elementBounds(element, ctx, depth);
        if (!(b.w > 0.0f) || !(b.h > 0.0f)) {
          pushClipPath(Path(), FillRule::NonZero); // nothing to measure: nothing shows
          return;
        }
        base = Transform(b.w, 0.0f, 0.0f, b.h, b.x, b.y).then(base);
      }
      const std::string& ownTransform = attribute(clip, "transform");
      if (!ownTransform.empty()) base = parseTransform(ownTransform).then(base);

      // Its shapes, each with its own transform and clip-rule. A <use> in a
      // clip path may name a shape; groups are not allowed in one.
      struct Piece {
        Path path;
        FillRule rule;
      };
      std::vector<Piece> pieces;
      active_.push_back(clip);
      Context clipCtx = ctx;
      clipCtx.transform = base;
      const Local clipLocal = applyElement(clip, clipCtx);
      clipCtx.transform = base; // the clipPath's transform is applied above, once
      (void) clipLocal;
      for (int child : nodes_[(size_t) clip].children) {
        if (!spend(1) || !spendReading(child)) break;
        int shapeNode = child;
        Context c = clipCtx;
        Local l = applyElement(child, c);
        if (!l.display || !c.style.visible) continue;
        if (nodes_[(size_t) child].name == "text") {
          // Text clips as its glyphs: each run a piece, with the nonzero rule
          // glyphs are drawn for.
          for (TextRun& run : layoutText(child, c)) {
            if (points_ + run.path.points().size() > kMaxPoints) {
              exhausted_ = true;
              break;
            }
            if (run.visible) pieces.push_back({run.path.transformed(c.transform), FillRule::NonZero});
          }
          continue;
        }
        if (nodes_[(size_t) child].name == "use") {
          const int target = lookup(href(child));
          if (target < 0 || !isShape(nodes_[(size_t) target].name)) continue;
          if (!spendReading(target)) break;
          const float x = lengthAttr(child, "x", Axis::X, ctx.viewport);
          const float y = lengthAttr(child, "y", Axis::Y, ctx.viewport);
          c.transform = Transform::translation(x, y).then(c.transform);
          l = applyElement(target, c);
          if (!l.display || !c.style.visible) continue;
          shapeNode = target;
        }
        if (!isShape(nodes_[(size_t) shapeNode].name)) continue;
        Path p;
        if (!buildShape(shapeNode, ctx.viewport, p)) continue;
        if (points_ + p.points().size() > kMaxPoints) {
          exhausted_ = true;
          break;
        }
        pieces.push_back({p.transformed(c.transform), c.style.clipRule});
      }
      active_.pop_back();

      if (pieces.size() <= 1) {
        // One shape is a clip Graphics does directly; none clips everything.
        pushClipPath(pieces.empty() ? Path() : std::move(pieces[0].path),
                     pieces.empty() ? FillRule::NonZero : pieces[0].rule);
        return;
      }
      // Several are their union, each with its own rule -- drawn opaque into
      // a mask, where overlapping is simply more opaque.
      push(Op::BeginMask);
      for (Piece& piece : pieces) {
        SvgShape shape;
        points_ += piece.path.points().size();
        shape.path = std::move(piece.path);
        shape.fillRule = piece.rule;
        out_.hidden_.push_back(std::move(shape));
        push(Op::ClipShape, (int) out_.hidden_.size() - 1);
      }
      push(Op::EndMaskAlpha);
      (void) inMask;
    }

    Rect shapeBounds(int node, const Context& ctx) const {
      Path p;
      if (!buildShape(node, ctx.viewport, p)) return Rect();
      return geometryBounds(p);
    }

    void emitMask(int mask, int element, const Context& ctx, int depth, bool) {
      if (expanding(mask)) return;
      const bool elementIsShape = isShape(nodes_[(size_t) element].name);
      Rect box;
      bool haveBox = false;
      auto needBox = [&]() -> const Rect& {
        if (!haveBox) {
          box = elementIsShape                                    ? shapeBounds(element, ctx)
                : nodes_[(size_t) element].name == "text" ? textBounds(element, ctx)
                                                                  : elementBounds(element, ctx, depth);
          haveBox = true;
        }
        return box;
      };

      push(Op::BeginMask);
      push(Op::Save);
      // The mask's region: outside it the mask is empty. objectBoundingBox
      // by default, -10% to 120% of the element's box.
      const bool regionInBox = attribute(mask, "maskUnits") != "userSpaceOnUse";
      Rect region;
      bool regionValid = true;
      if (regionInBox) {
        const Rect& b = needBox();
        if (!(b.w > 0.0f) || !(b.h > 0.0f)) regionValid = false;
        const float rx = unitLength(attribute(mask, "x"), true, Axis::X, ctx.viewport, -0.1f);
        const float ry = unitLength(attribute(mask, "y"), true, Axis::Y, ctx.viewport, -0.1f);
        const float rw = unitLength(attribute(mask, "width"), true, Axis::X, ctx.viewport, 1.2f);
        const float rh = unitLength(attribute(mask, "height"), true, Axis::Y, ctx.viewport, 1.2f);
        region = Rect(b.x + rx * b.w, b.y + ry * b.h, rw * b.w, rh * b.h);
      } else {
        const Rect& vp = ctx.viewport;
        region = Rect(lengthAttr(mask, "x", Axis::X, vp, -0.1f * vp.w),
                      lengthAttr(mask, "y", Axis::Y, vp, -0.1f * vp.h),
                      lengthAttr(mask, "width", Axis::X, vp, 1.2f * vp.w),
                      lengthAttr(mask, "height", Axis::Y, vp, 1.2f * vp.h));
      }
      if (!regionValid || !(region.w > 0.0f) || !(region.h > 0.0f)) {
        pushClipPath(Path(), FillRule::NonZero);
      } else {
        Path r;
        r.addRect(region);
        pushClipPath(r.transformed(ctx.transform), FillRule::NonZero);

        Context content = ctx;
        content.style = Style{}; // a mask's content inherits from the mask, not the element
        const Local maskLocal = applyElement(mask, content);
        content.transform = ctx.transform;
        if (attribute(mask, "maskContentUnits") == "objectBoundingBox") {
          const Rect& b = needBox();
          content.transform = Transform(b.w, 0.0f, 0.0f, b.h, b.x, b.y).then(ctx.transform);
        }
        active_.push_back(mask);
        for (int child : nodes_[(size_t) mask].children) {
          if (exhausted_) break;
          render(child, content, depth + 1, true);
        }
        active_.pop_back();
        push(Op::Restore);
        push(maskLocal.maskAlpha ? Op::EndMaskAlpha : Op::EndMaskLuminance);
        return;
      }
      push(Op::Restore);
      push(Op::EndMaskLuminance);
    }

    // ── Text ───────────────────────────────────────────────────────────────
    //
    // A <text> element is a tree of spans -- itself, its <tspan>s and <a>s --
    // and between them character data, each piece styled by the span it is
    // in. It is laid out as SVG 1.1 lays out horizontal text: whitespace
    // first (xml:space), then each character's x, y, dx and dy from the
    // nearest span that gives one for it, then a pen moving along the
    // baseline. Characters that share a span and need no position of their
    // own are a RUN, shaped as one string so ligatures, kerning and the
    // complex scripts come out as Font draws them anywhere else. An absolute
    // x or y starts a new CHUNK, and text-anchor moves each chunk whole.

    /** One run: its glyphs in the text element's user space, and what drew
     *  them. */
    struct TextRun {
      Path path;
      Context ctx; // the span's style; the text element's transform
      std::string text;
      Font font;
      FontMatch match;
      float x = 0.0f, baseline = 0.0f, advance = 0.0f;
      int chunk = 0;
      bool visible = true;
    };

    /** A `<text>` or `<tspan>` the characters were collected from: the style
     *  they are drawn in, and its own xml:space and visibility. */
    struct TextSpan {
      Context ctx;
      bool preserve = false; // xml:space="preserve"
      bool visible = true;
    };
    /** One character of a text element, with the span it came from, whether
     *  whitespace handling kept it, and any absolute (x, y) or relative
     *  (dx, dy) position the spans' lists gave it. */
    struct TextChar {
      uint32_t cp = 0;
      int span = 0;
      bool kept = true;
      bool hasX = false, hasY = false;
      float x = 0.0f, y = 0.0f, dx = 0.0f, dy = 0.0f;
    };
    /** A span's x, y, dx and dy lists, for the characters it holds. */
    struct TextPositions {
      size_t from = 0, to = 0; // its characters, before whitespace is collapsed
      std::vector<float> x, y, dx, dy;
    };
    /** Everything one text element holds, flattened: its spans, its
     *  characters in document order, and the spans' position lists -- what
     *  layoutText turns into runs. */
    struct TextContent {
      std::vector<TextSpan> spans;
      std::vector<TextChar> chars;
      std::vector<TextPositions> lists; // in document order: a span before those inside it
    };

    static uint32_t nextUtf8(const std::string& s, size_t& i) {
      const uint8_t c = (uint8_t) s[i];
      int extra = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : -1;
      if (extra < 0 || i + (size_t) extra >= s.size() + (extra == 0 ? 1 : 0)) {
        ++i;
        return extra == 0 ? c : 0xFFFD;
      }
      uint32_t cp = extra == 0 ? c : (uint32_t) (c & (0x3F >> extra));
      for (int k = 1; k <= extra; ++k) {
        const uint8_t next = (uint8_t) s[i + (size_t) k];
        if ((next >> 6) != 2) {
          ++i;
          return 0xFFFD;
        }
        cp = (cp << 6) | (next & 0x3F);
      }
      i += (size_t) extra + 1;
      return cp > 0x10FFFF ? 0xFFFD : cp;
    }

    /** A list of lengths -- x="10 20 30" -- with an em as the span's font
     *  size and a percentage as the viewport's.
     *
     *  Paid for from the character budget, a unit per sixteen bytes: a list
     *  is read again at every layout of its text -- every `<use>` of it -- and
     *  a megabyte of "0 0 0 ..." on one character, used twenty thousand
     *  times, was half an hour of parsing that no limit counted. A list as
     *  long as its text costs a fraction of the characters it positions. */
    std::vector<float> lengthList(int node, const char* name, Axis axis, const Context& ctx) {
      std::vector<float> out;
      const std::string& list = attribute(node, name);
      characters_ += list.size() / 16;
      if (characters_ > kMaxCharacters) {
        exhausted_ = true;
        return out;
      }
      forEachItem(list, [&](const std::string& item) {
        float v = 0.0f;
        const std::string l = lower(item);
        const bool em = l.size() > 2 && l.compare(l.size() - 2, 2, "em") == 0;
        const bool ex = l.size() > 2 && l.compare(l.size() - 2, 2, "ex") == 0;
        if (em || ex) {
          float n = 0.0f;
          if (number(l, n)) v = n * ctx.style.fontSize * (ex ? 0.5f : 1.0f);
        } else if (!length(item, axis, ctx.viewport, v)) {
          v = 0.0f;
        }
        out.push_back(v);
      });
      return out;
    }

    void collectText(int node, const Context& ctx, bool preserve, bool visible, int depth, TextContent& out) {
      if (depth > kMaxDepth) return;
      const int span = (int) out.spans.size();
      out.spans.push_back({ctx, preserve, visible && ctx.style.visible});
      const size_t listAt = out.lists.size();
      out.lists.emplace_back();
      {
        TextPositions& p = out.lists.back();
        p.from = out.chars.size();
        p.x = lengthList(node, "x", Axis::X, ctx);
        p.y = lengthList(node, "y", Axis::Y, ctx);
        p.dx = lengthList(node, "dx", Axis::X, ctx);
        p.dy = lengthList(node, "dy", Axis::Y, ctx);
      }
      for (int child : nodes_[(size_t) node].children) {
        const Node& c = nodes_[(size_t) child];
        if (c.name == "#text") {
          for (size_t i = 0; i < c.text.size();) {
            if (characters_ >= kMaxCharacters) {
              exhausted_ = true;
              break;
            }
            ++characters_;
            TextChar ch;
            ch.cp = nextUtf8(c.text, i);
            ch.span = span;
            out.chars.push_back(ch);
          }
        } else if (c.name == "tspan" || c.name == "a") {
          if (!spend(1) || !spendReading(child)) return;
          Context inner = ctx;
          const Local local = applyElement(child, inner);
          inner.transform = ctx.transform; // a span has no transform of its own in SVG 1.1
          if (!local.display) continue;    // not drawn, and takes no room
          const std::string& space = attribute(child, "xml:space");
          collectText(child, inner, space == "preserve" ? true : (space == "default" ? false : preserve),
                      out.spans[(size_t) span].visible, depth + 1, out);
        }
        // <textPath>, <tref> and anything else: not drawn (see the header).
      }
      out.lists[listAt].to = out.chars.size();
    }

    /** The font a span draws in, matched once per parse for each family,
     *  weight, slope and size. */
    const std::pair<FontMatch, Font>& fontFor(const Style& s) {
      const std::string key = s.fontFamily + '\x1f' + std::to_string(s.fontWeight) + (s.italic ? "i" : "n") +
                              std::to_string(s.fontSize);
      auto it = fontCache_.find(key);
      if (it != fontCache_.end()) return it->second;
      const FontMatch m = fonts_.match(s.fontFamily, s.fontWeight, s.italic);
      return fontCache_.emplace(key, std::make_pair(m, m.font(s.fontSize))).first->second;
    }

    std::vector<TextRun> layoutText(int node, const Context& ctx) {
      std::vector<TextRun> runs;
      TextContent content;
      const std::string& space = attribute(node, "xml:space");
      collectText(node, ctx, space == "preserve", true, 0, content);
      std::vector<TextChar>& chars = content.chars;
      if (chars.empty()) return runs;

      // Whitespace, for xml:space="default", as the browsers and resvg treat
      // it -- which is SVG 2's reading, not SVG 1.1's letter: a line break
      // or a tab is a space (1.1 said to delete line breaks, and "two\nwords"
      // drawn as "twowords" is not what anybody exporting text meant), the
      // spaces at the start and end of the element are dropped, and runs of
      // them collapse to one -- across span boundaries, as one string.
      // "preserve" turns each line break and tab into a space and keeps them
      // all. A CR before an LF is the line break's first half, and goes.
      int lastKept = -1;
      for (size_t i = 0; i < chars.size(); ++i) {
        TextChar& c = chars[i];
        if (c.cp == '\r' && i + 1 < chars.size() && chars[i + 1].cp == '\n') {
          c.kept = false;
          continue;
        }
        if (c.cp == '\n' || c.cp == '\r' || c.cp == '\t') c.cp = ' ';
        if (content.spans[(size_t) c.span].preserve) {
          lastKept = (int) i;
          continue;
        }
        if (c.cp == ' ' && (lastKept < 0 || chars[(size_t) lastKept].cp == ' ')) {
          c.kept = false;
          continue;
        }
        lastKept = (int) i;
      }
      for (size_t i = chars.size(); i-- > 0;) {
        if (!chars[i].kept) continue;
        if (chars[i].cp != ' ' || content.spans[(size_t) chars[i].span].preserve) break;
        chars[i].kept = false;
      }

      // Positions: each span's lists over the characters it holds that
      // survived, outer spans first so an inner one's values win.
      for (const TextPositions& p : content.lists) {
        size_t k = 0;
        for (size_t i = p.from; i < p.to && i < chars.size(); ++i) {
          if (!chars[i].kept) continue;
          TextChar& c = chars[i];
          if (k < p.x.size()) c.hasX = true, c.x = p.x[k];
          if (k < p.y.size()) c.hasY = true, c.y = p.y[k];
          if (k < p.dx.size()) c.dx = p.dx[k];
          if (k < p.dy.size()) c.dy = p.dy[k];
          ++k;
        }
      }

      // The pen.
      float penX = 0.0f, penY = 0.0f;
      std::vector<float> chunkStart, chunkEnd;
      std::vector<int> chunkAnchor;
      bool spaced = false; // the open run is one character with spacing after it
      auto finish = [&]() {
        if (runs.empty()) return;
        TextRun& r = runs.back();
        r.advance = r.font.stringWidth(r.text);
        penX = r.x + r.advance;
        if (spaced) {
          const Style& st = r.ctx.style;
          penX += st.letterSpacing + (r.text == " " ? st.wordSpacing : 0.0f);
          r.advance = penX - r.x;
        }
        chunkEnd[(size_t) r.chunk] = penX;
      };
      bool open = false;
      int lastSpan = -1;
      for (const TextChar& c : chars) {
        if (!c.kept) continue;
        const TextSpan& sp = content.spans[(size_t) c.span];
        const Style& st = sp.ctx.style;
        const bool newChunk = !open || c.hasX || c.hasY;
        const bool spacing = st.letterSpacing != 0.0f || (st.wordSpacing != 0.0f && c.cp == ' ');
        const bool newRun = newChunk || c.dx != 0.0f || c.dy != 0.0f || c.span != lastSpan || spaced || spacing;
        if (newRun && open) finish();
        if (c.hasX) penX = c.x;
        if (c.hasY) penY = c.y;
        penX += c.dx;
        penY += c.dy;
        if (newChunk) {
          chunkStart.push_back(penX);
          chunkEnd.push_back(penX);
          chunkAnchor.push_back(st.anchor);
        }
        if (newRun) {
          TextRun r;
          r.ctx = sp.ctx;
          const std::pair<FontMatch, Font>& f = fontFor(st);
          r.match = f.first;
          r.font = f.second;
          r.x = penX;
          r.baseline = penY;
          r.chunk = (int) chunkStart.size() - 1;
          r.visible = sp.visible;
          runs.push_back(std::move(r));
          open = true;
        }
        appendUtf8(runs.back().text, c.cp);
        spaced = spacing;
        lastSpan = c.span;
      }
      if (open) finish();

      // Anchored, chunk by chunk, then drawn where they ended up.
      for (TextRun& r : runs) {
        const size_t k = (size_t) r.chunk;
        const float width = chunkEnd[k] - chunkStart[k];
        r.x -= chunkAnchor[k] == 1 ? width * 0.5f : (chunkAnchor[k] == 2 ? width : 0.0f);
        r.font.appendString(r.path, r.text, r.x, r.baseline);
      }
      return runs;
    }

    static Rect runBounds(const std::vector<TextRun>& runs) {
      Rect box;
      bool any = false;
      for (const TextRun& r : runs) {
        if (r.path.isEmpty()) continue;
        const Rect b = geometryBounds(r.path);
        box = any ? box.united(b) : b;
        any = true;
      }
      return box;
    }

    /** What objectBoundingBox means for a `<text>`: its glyphs' bounds. */
    Rect textBounds(int node, const Context& ctx) { return runBounds(layoutText(node, ctx)); }

    void emitText(int node, const Context& ctx, bool inMask) {
      std::vector<TextRun> runs = layoutText(node, ctx);
      // A gradient on text spans the whole element, not each run: the
      // bounding box is the element's.
      const Rect box = runBounds(runs);
      const float scale = ctx.transform.scaleFactor();
      for (TextRun& r : runs) {
        if (exhausted_) return;
        if (points_ + r.path.points().size() > kMaxPoints) {
          exhausted_ = true;
          return;
        }
        points_ += r.path.points().size();
        if (r.visible && !r.path.isEmpty()) emitPath(r.path, r.ctx, box, 1.0f, inMask, FillRule::NonZero);
        if (inMask || !r.font.isValid()) continue;
        DrawableText t;
        t.setText(r.text);
        t.setFont(r.font.withHeight(r.font.height() * scale));
        if (r.ctx.style.fill.kind == Paint::Plain)
          t.setColour(r.ctx.style.fill.colour.withMultipliedAlpha(r.ctx.style.fillOpacity));
        else if (r.ctx.style.fill.kind == Paint::Current)
          t.setColour(r.ctx.style.color.withMultipliedAlpha(r.ctx.style.fillOpacity));
        else
          t.setColour(Colour(0, 0, 0, 0));
        const float ascent = r.font.ascent(), descent = r.font.descent();
        t.setBoundingBox(Parallelogram(Rect(r.x, r.baseline - ascent, r.advance, ascent + descent))
                             .transformedBy(ctx.transform));
        t.setRequestedFamily(r.ctx.style.fontFamily);
        t.setMatchedFamily(r.match.family);
        out_.texts_.push_back(std::move(t));
      }
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

    // ── Text helpers ───────────────────────────────────────────────────────

    static bool isSpace(char c) {
      return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    }

    static std::string trim(const std::string& s) {
      size_t a = 0, b = s.size();
      while (a < b && isSpace(s[a])) ++a;
      while (b > a && isSpace(s[b - 1])) --b;
      return s.substr(a, b - a);
    }

    static std::string lower(std::string s) {
      for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = (char) (c - 'A' + 'a');
      return s;
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
        double value = 0.0;
        const size_t used = scanNumber(text, at, value);
        if (used == 0) {
          ++at;
          continue;
        }
        out.push_back((float) value);
        at += used;
      }
      return out;
    }

    static bool isDigit(char c) { return c >= '0' && c <= '9'; }
    static bool isNumberStart(char c) {
      return isDigit(c) || c == '-' || c == '+' || c == '.';
    }

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

        const float toRadians = 3.14159265358979323846f / 180.0f;
        Transform step;
        if (trimmed == "translate" && !v.empty()) {
          step = Transform::translation(v[0], v.size() > 1 ? v[1] : 0.0f);
        } else if (trimmed == "scale" && !v.empty()) {
          step = Transform::scaling(v[0], v.size() > 1 ? v[1] : v[0]);
        } else if (trimmed == "rotate" && !v.empty()) {
          const float radians = v[0] * toRadians;
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
        } else if (trimmed == "skewX" && !v.empty()) {
          step = Transform(1.0f, 0.0f, std::tan(v[0] * toRadians), 1.0f, 0.0f, 0.0f);
        } else if (trimmed == "skewY" && !v.empty()) {
          step = Transform(1.0f, std::tan(v[0] * toRadians), 0.0f, 1.0f, 0.0f, 0.0f);
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

    static void parsePathData(const std::string& d, Path& path) {
      size_t at = 0;
      char command = 0;
      Point current, start;
      Point lastControl;
      char lastCommand = 0;

      auto skip = [&]() {
        while (at < d.size() && (isSpace(d[at]) || d[at] == ',')) ++at;
      };
      // A number, or the end of the path: SVG's own rule is that rendering
      // stops at the first error, and "1e999" -- a number the grammar accepts
      // and geometry cannot hold -- is one.
      bool failed = false;
      auto number = [&]() -> float {
        skip();
        double v = 0.0;
        const size_t used = at < d.size() ? scanNumber(d, at, v) : 0;
        if (used == 0) {
          failed = true;
          at = d.size();
          return 0.0f;
        }
        at += used;
        return (float) v;
      };
      // An arc's flags are single digits, and "a1 1 0 00 1 1" packs two of
      // them together: read as a number, "00" is one flag and every
      // coordinate after it shifts by one.
      auto flag = [&]() -> float {
        skip();
        if (at < d.size() && (d[at] == '0' || d[at] == '1')) return (float) (d[at++] - '0');
        failed = true;
        at = d.size();
        return 0.0f;
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
        bool letter = false;
        if (!isNumberStart(d[at])) {
          command = d[at];
          ++at;
          letter = true;
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
        // fuzzer found it in ninety flips. A Z with its letter has consumed
        // the letter: the stop below once took that for nothing at all, and
        // every path ended at its first Z -- every hole, every ring, every
        // second shape in one d="" was dropped.
        const size_t before = at;
        const bool relative = command >= 'a' && command <= 'z';
        const char upper = relative ? (char) (command - 32) : command;

        switch (upper) {
          case 'M': {
            const float x = number(), y = number();
            if (failed) return;
            current = relative ? Point{current.x + x, current.y + y} : Point{x, y};
            start = current;
            path.moveTo(current.x, current.y);
            break;
          }
          case 'L': {
            const float x = number(), y = number();
            if (failed) return;
            current = relative ? Point{current.x + x, current.y + y} : Point{x, y};
            path.lineTo(current.x, current.y);
            break;
          }
          case 'H': {
            const float x = number();
            if (failed) return;
            current.x = relative ? current.x + x : x;
            path.lineTo(current.x, current.y);
            break;
          }
          case 'V': {
            const float y = number();
            if (failed) return;
            current.y = relative ? current.y + y : y;
            path.lineTo(current.x, current.y);
            break;
          }
          case 'C': {
            const float x1 = number(), y1 = number();
            const float x2 = number(), y2 = number();
            const float x = number(), y = number();
            if (failed) return;
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
            if (failed) return;
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
            if (failed) return;
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
            if (failed) return;
            const Point end = relative ? Point{current.x + x, current.y + y} : Point{x, y};
            path.quadTo(c1.x, c1.y, end.x, end.y);
            lastControl = c1;
            current = end;
            break;
          }
          case 'A': {
            const float rx = number(), ry = number();
            const float rotation = number();
            const float largeArc = flag(), sweep = flag();
            const float x = number(), y = number();
            if (failed) return;
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
        if (at == before && !letter) return; // nothing consumed: the data has stopped making sense
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
      // An infinite radius gets here as a NaN sweep, and a NaN converted to
      // int is undefined: such an arc is drawn as the straight line a zero
      // radius gives.
      if (!(std::fabs(sweepAngle) <= 2.0f * twoPi)) {
        path.lineTo(to.x, to.y);
        return;
      }
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
    const FontMatcher& fonts_;
    std::map<std::string, std::pair<FontMatch, Font>> fontCache_;
    size_t characters_ = 0;
    std::vector<Node> nodes_;
    std::vector<Rule> rules_;
    std::vector<std::vector<Declaration>> blocks_; // a style sheet's declaration blocks
    std::vector<const Rule*> rulesForAny_;         // rules_, filed by readStyleSheets()
    std::unordered_map<std::string, std::vector<const Rule*>> rulesByType_, rulesById_, rulesByClass_;
    mutable std::vector<std::vector<const Declaration*>> cascaded_; // cascade(), per node
    mutable std::vector<std::vector<Declaration>> own_; // an element's own declarations
    mutable std::vector<char> cascadeDone_;
    mutable size_t styleWork_ = 0;
    mutable size_t readBytes_ = 0;
    std::unordered_map<std::string, int> ids_;
    std::vector<int> active_;
    size_t rendered_ = 0, points_ = 0;
    mutable bool exhausted_ = false; // set by the style budget too, from the const style lookups
  };

  std::vector<SvgShape> shapes_;
  std::vector<SvgShape> hidden_; // drawn into clips and masks only
  std::vector<DrawableText> texts_;
  std::vector<Op> ops_;
  Rect viewBox_;
  Placement placement_;
  std::string error_;
};

} // namespace gfx
} // namespace sonore
