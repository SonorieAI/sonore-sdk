// SPDX-License-Identifier: Apache-2.0
//
// LookAndFeel_V1, V2, V3 and V4: the reference's four generations of default
// appearance, so a port can keep the face it had.
//
// ── Why four old looks are worth carrying ───────────────────────────────────
//
// A plugin being moved onto this SDK from the reference has users who know what
// it looks like. Its screenshots are in a manual, its knobs are in a video
// tutorial, and a port that changed every control's appearance on the same day
// it changed the framework underneath would be reported as broken by the people
// who rely on it most. Most such plugins never chose a look -- they shipped
// whichever default their framework version had -- so the faithful port is
// "the default it had", and there were four.
//
// ── What each one is ────────────────────────────────────────────────────────
//
//   V1  the original: grey, rectangular, bevelled. Light top-left edges and
//       dark bottom-right ones, pressed controls with the bevel inverted, a
//       pie-segment knob and a block thumb on a sunken groove.
//   V2  the glossy one: lavender "glass" lozenge buttons with a highlight over
//       the top half, sphere-shaped slider thumbs on a shadowed groove, a ring
//       segment knob, white popups with a deep-blue highlight.
//   V3  V2 flattened: the same palette family with the gloss taken off --
//       square-ish buttons with a faint gradient, thin translucent scroll
//       thumbs, flat tabs.
//   V4  the colour-scheme look: everything drawn from nine named colours, in
//       the four schemes the reference ships -- dark, midnight, grey and light
//       -- with arc knobs, round thumbs on line tracks and pill scroll thumbs.
//
// The colours are the reference's own where its constructors give them (the
// V4 schemes are its nine-colour tables verbatim; V1/V2's lavender 0xbbbbff,
// popup and tooltip colours likewise). The SHAPES are drawn through this SDK's
// LookAndFeel virtuals with its own rasteriser, so they are faithful in form --
// bevel, gloss, flatness, arcs -- rather than pixel-identical to a renderer
// this SDK does not have.
//
// ── How a port picks one ────────────────────────────────────────────────────
//
//   static LookAndFeel_V4 look(LookAndFeel_V4::getMidnightColourScheme());
//   editor.setLookAndFeel(&look);   // every widget inside inherits it
//
// Each is an ordinary LookAndFeel subclass, so a port that customised its
// look derives from the version it customised and overrides the same few
// methods it did before.
#pragma once

#include <cmath>
#include <string>

#include "lookandfeel.h"

namespace sonore {
namespace gfx {

namespace detail {

/** A filled pie: centre, out to the arc, round, back. Angles as Path::addArc
 *  takes them -- twelve o'clock, clockwise. */
inline Path lafPieSegment(const Rect& circle, float from, float to) {
  Path p;
  const Point c = circle.centre();
  p.moveTo(c.x, c.y);
  p.addArc(circle, from, to, /*startNewContour=*/false);
  p.close();
  return p;
}

/** A band of a ring between two radii: the outer arc one way, the inner arc
 *  back. `inner` is a fraction of the outer radius. */
inline Path lafRingSegment(const Rect& circle, float from, float to, float inner) {
  Path p;
  const Point c = circle.centre();
  const float rx = circle.w * 0.5f * inner, ry = circle.h * 0.5f * inner;
  p.addArc(circle, from, to);
  p.addArc(Rect(c.x - rx, c.y - ry, rx * 2.0f, ry * 2.0f), to, from, /*startNewContour=*/false);
  p.close();
  return p;
}

/** A triangle pointing down, centred in `r` -- the combo box arrow every
 *  generation before V4 used. */
inline Path lafDownTriangle(const Rect& r) {
  Path p;
  const Point c = r.centre();
  const float w = (std::min)(r.w, r.h) * 0.5f;
  p.moveTo(c.x - w * 0.5f, c.y - w * 0.25f);
  p.lineTo(c.x + w * 0.5f, c.y - w * 0.25f);
  p.lineTo(c.x, c.y + w * 0.35f);
  p.close();
  return p;
}

/**
 * V1's bevel: light along the top and left, dark along the bottom and right,
 * `width` pixels deep. Inverted, it is a SUNKEN surface -- a groove, a text
 * well, a pressed button -- and that one swap is the whole vocabulary of the
 * original look.
 */
inline void lafBevel(Graphics& g, const Rect& r, float width, bool sunken) {
  const Colour light(255, 255, 255, 200);
  const Colour dark(0, 0, 0, 110);
  g.setColour(sunken ? dark : light);
  g.fillRect(Rect(r.x, r.y, r.w, width));
  g.fillRect(Rect(r.x, r.y + width, width, r.h - width));
  g.setColour(sunken ? light : dark);
  g.fillRect(Rect(r.x + width, r.bottom() - width, r.w - width, width));
  g.fillRect(Rect(r.right() - width, r.y + width, width, r.h - width * 2.0f));
}

/**
 * V2's glass: a body ramp, a bright highlight over the top half that fades
 * out before the middle, and a faint glow at the bottom edge. `radius` of half
 * the height is the lozenge -- the pill every V2 button was.
 */
inline void lafGlassLozenge(Graphics& g, const Rect& r, Colour colour, float radius,
                            bool pressed) {
  if (r.isEmpty()) return;
  const Colour top = pressed ? colour.darkened(0.12f) : colour.brightened(0.10f);
  const Colour bottom = pressed ? colour.brightened(0.02f) : colour.darkened(0.14f);
  g.setGradient(ColourGradient::vertical(top, r.y, bottom, r.bottom()));
  g.fillRoundedRect(r, radius);

  const Rect gloss(r.x + radius * 0.35f, r.y + 1.0f, r.w - radius * 0.7f, r.h * 0.46f);
  if (!gloss.isEmpty()) {
    g.setGradient(ColourGradient::vertical(Colour(255, 255, 255, pressed ? 90 : 170), gloss.y,
                                           Colour(255, 255, 255, 20), gloss.bottom()));
    g.fillRoundedRect(gloss, (std::min)(radius, gloss.h * 0.5f));
  }
  g.setGradient(ColourGradient::vertical(Colour(255, 255, 255, 0), r.y + r.h * 0.62f,
                                         Colour(255, 255, 255, 70), r.bottom()));
  g.fillRoundedRect(r.reduced(1.0f), (std::max)(0.0f, radius - 1.0f));

  g.setColour(colour.darkened(0.45f).withMultipliedAlpha(0.85f));
  g.drawRoundedRect(r, radius, 1.0f);
}

/** V2's sphere: lit from the top left, a specular spot, a darker rim. */
inline void lafGlassSphere(Graphics& g, const Rect& r, Colour colour) {
  if (r.isEmpty()) return;
  const Point c = r.centre();
  g.setGradient(ColourGradient::radial(colour.brightened(0.22f),
                                       Point(c.x - r.w * 0.18f, c.y - r.h * 0.22f), r.w * 0.75f,
                                       colour.darkened(0.25f)));
  g.fillEllipse(r);
  const Rect spot(r.x + r.w * 0.22f, r.y + r.h * 0.08f, r.w * 0.56f, r.h * 0.38f);
  g.setGradient(ColourGradient::vertical(Colour(255, 255, 255, 190), spot.y,
                                         Colour(255, 255, 255, 10), spot.bottom()));
  g.fillEllipse(spot);
  g.setColour(colour.darkened(0.5f).withMultipliedAlpha(0.8f));
  g.drawEllipse(r, 1.0f);
}

} // namespace detail

// ═══════════════════════════════════════════════════════════════════════════
// V1 -- grey, square, bevelled
// ═══════════════════════════════════════════════════════════════════════════

/**
 * The reference's first default look: grey, rectangular and bevelled, with
 * pressed controls sunken and a pie-segment knob. Same name as the
 * reference's class, for a port that wants to keep this face; the file
 * comment describes all four versions and how to pick one.
 *
 * Stateless, so one static instance can serve every widget; like any
 * LookAndFeel it must outlive the widgets it is set on.
 */
class LookAndFeel_V1 : public LookAndFeel {
public:
  Colour background() const override { return Colour::fromRGB(0xD4D4D4); }
  Colour panel() const override { return Colour::fromRGB(0xE4E4E4); }
  Colour outline() const override { return Colour::fromRGB(0x404040); }
  Colour text() const override { return Colour::fromRGB(0x000000); }
  Colour dimText() const override { return Colour::fromRGB(0x808080); }
  /** The reference's classic button blue. */
  Colour accent() const override { return Colour::fromRGB(0xBBBBFF); }

  void drawFocusRing(Graphics& g, const Rect& area) const override {
    g.setColour(Colour(0, 0, 0, 160));
    g.drawRect(area.reduced(0.5f), 1.0f);
  }

  void drawButton(Graphics& g, const Rect& area, const std::string& label, const Font& font,
                  const WidgetState& st) override {
    Colour face = st.toggled ? accent() : panel();
    if (st.hovered && !st.pressed) face = face.brightened(0.04f);
    if (!st.enabled) face = face.withMultipliedAlpha(0.5f);
    g.setColour(face);
    g.fillRect(area);
    detail::lafBevel(g, area.reduced(1.0f), 2.0f, st.pressed || st.toggled);
    g.setColour(Colour(0, 0, 0, st.enabled ? 255 : 120));
    g.drawRect(area.reduced(0.5f), 1.0f);
    if (label.empty()) return;
    // A pressed V1 button's label moves down and right by a pixel, which is
    // the whole of its pressed feedback beyond the bevel.
    const float shift = st.pressed ? 1.0f : 0.0f;
    g.setColour(st.enabled ? text() : dimText());
    font.drawIn(g, label, area.translated(shift, shift), Justify::Centred);
  }

  void drawRotarySlider(Graphics& g, const Rect& area, float normalised,
                        const WidgetState& st) override {
    const Rect dial = area.withAspect(1.0f).reduced(3.0f);
    if (dial.isEmpty()) return;
    const float from = rotaryStart(), to = rotaryEnd();
    const float at = from + (to - from) * clamp01(normalised);
    // A plain disc, then the value as a pie segment over it -- V1's knob.
    g.setColour(Colour::fromRGB(0xF4F4F4));
    g.fillEllipse(dial);
    if (normalised > 0.0f) {
      g.setColour(Colour::fromARGB(st.enabled ? 0x7F0000FFu : 0x400000FFu));
      g.fillPath(detail::lafPieSegment(dial, from, at));
      g.setColour(Colour(0, 0, 0, 102));
      g.strokePath(detail::lafPieSegment(dial, from, at), StrokeStyle{1.0f});
    }
    g.setColour(Colour(0, 0, 0, 160));
    g.drawEllipse(dial, 1.0f);
    const Point c = dial.centre();
    const float r = dial.w * 0.5f;
    g.setColour(text());
    g.drawLine(c.x, c.y, c.x + std::sin(at) * r, c.y - std::cos(at) * r, 1.5f);
  }

  void drawLinearSlider(Graphics& g, const Rect& area, float normalised, bool vertical,
                        const WidgetState& st) override {
    const float v = clamp01(normalised);
    const float grooveW = 4.0f, thumbLong = 18.0f, thumbShort = 10.0f;
    const Rect groove = vertical ? Rect(area.centre().x - grooveW * 0.5f, area.y + thumbShort * 0.5f,
                                        grooveW, area.h - thumbShort)
                                 : Rect(area.x + thumbShort * 0.5f, area.centre().y - grooveW * 0.5f,
                                        area.w - thumbShort, grooveW);
    if (groove.isEmpty()) return;
    g.setColour(background().darkened(0.08f));
    g.fillRect(groove);
    detail::lafBevel(g, groove, 1.0f, /*sunken=*/true);

    const Rect thumb =
        vertical ? Rect(area.centre().x - thumbLong * 0.5f,
                        groove.bottom() - groove.h * v - thumbShort * 0.5f, thumbLong, thumbShort)
                 : Rect(groove.x + groove.w * v - thumbShort * 0.5f,
                        area.centre().y - thumbLong * 0.5f, thumbShort, thumbLong);
    g.setColour(st.enabled ? (st.pressed ? Colour::fromRGB(0xDDDDDD) : Colour::fromRGB(0xFFFFFF))
                           : Colour::fromRGB(0xE8E8E8));
    g.fillRect(thumb);
    detail::lafBevel(g, thumb.reduced(1.0f), 1.0f, st.pressed);
    g.setColour(Colour(0, 0, 0, 200));
    g.drawRect(thumb.reduced(0.5f), 1.0f);
  }

  void drawLinearBar(Graphics& g, const Rect& area, float normalised, bool vertical,
                     const WidgetState& st) override {
    const float v = clamp01(normalised);
    g.setColour(Colour::fromRGB(0xFFFFFF));
    g.fillRect(area);
    Rect fill = area.reduced(2.0f);
    if (vertical) {
      fill.h *= v;
      fill.y = area.bottom() - 2.0f - fill.h;
    } else {
      fill.w *= v;
    }
    if (!fill.isEmpty()) {
      g.setColour(st.enabled ? accent().darkened(0.15f) : dimText());
      g.fillRect(fill);
    }
    detail::lafBevel(g, area, 1.0f, /*sunken=*/true);
  }

  void drawRangeSlider(Graphics& g, const Rect& area, float low, float high,
                       const WidgetState& st) override {
    const float thumb = 10.0f;
    const Rect groove(area.x + thumb * 0.5f, area.centre().y - 2.0f, area.w - thumb, 4.0f);
    if (groove.isEmpty()) return;
    g.setColour(background().darkened(0.08f));
    g.fillRect(groove);
    detail::lafBevel(g, groove, 1.0f, true);
    const float x0 = groove.x + groove.w * clamp01(low), x1 = groove.x + groove.w * clamp01(high);
    g.setColour(accent().darkened(0.2f));
    g.fillRect(Rect(x0, groove.y, x1 - x0, groove.h));
    for (float x : {x0, x1}) {
      const Rect t(x - thumb * 0.5f, area.centre().y - 9.0f, thumb, 18.0f);
      g.setColour(st.enabled ? Colour::fromRGB(0xFFFFFF) : Colour::fromRGB(0xE8E8E8));
      g.fillRect(t);
      detail::lafBevel(g, t.reduced(1.0f), 1.0f, false);
      g.setColour(Colour(0, 0, 0, 200));
      g.drawRect(t.reduced(0.5f), 1.0f);
    }
  }

  void drawComboBox(Graphics& g, const Rect& area, const std::string& label, const Font& font,
                    const WidgetState& st) override {
    g.setColour(Colour::fromRGB(0xFFFFFF));
    g.fillRect(area);
    detail::lafBevel(g, area, 2.0f, /*sunken=*/true);
    const float b = area.h - 4.0f;
    const Rect button(area.right() - b - 2.0f, area.y + 2.0f, b, b);
    g.setColour(st.pressed ? panel().darkened(0.06f) : panel());
    g.fillRect(button);
    detail::lafBevel(g, button, 1.0f, st.pressed);
    g.setColour(st.enabled ? text() : dimText());
    g.fillPath(detail::lafDownTriangle(button));
    g.setColour(st.enabled ? text() : dimText());
    font.drawIn(g, label, Rect(area.x + 5.0f, area.y, area.w - b - 10.0f, area.h), Justify::Left);
  }

  void drawScrollBar(Graphics& g, const Rect& track, const Rect& thumb, bool vertical,
                     bool active) override {
    (void) vertical;
    g.setColour(background().darkened(0.06f));
    g.fillRect(track);
    g.setColour(active ? panel().brightened(0.04f) : panel());
    g.fillRect(thumb);
    detail::lafBevel(g, thumb, 1.0f, false);
    g.setColour(Colour(0, 0, 0, 140));
    g.drawRect(thumb.reduced(0.5f), 1.0f);
  }

  void drawTextEditorBackground(Graphics& g, const Rect& area, bool focused,
                                bool enabled) override {
    g.setColour(enabled ? Colour::fromRGB(0xFFFFFF) : Colour::fromRGB(0xEEEEEE));
    g.fillRect(area);
    detail::lafBevel(g, area, 2.0f, /*sunken=*/true);
    if (focused) {
      g.setColour(accent().darkened(0.3f));
      g.drawRect(area.reduced(2.5f), 1.0f);
    }
  }

  void drawTextEditorSelection(Graphics& g, const Rect& area) override {
    g.setColour(Colour::fromRGB(0xA4C2CE));
    g.fillRect(area);
  }

  void drawTextEditorCaret(Graphics& g, const Rect& area) override {
    g.setColour(Colour::fromRGB(0x000000));
    g.fillRect(area);
  }

  void drawPopupBackground(Graphics& g, const Rect& area) override {
    g.setColour(Colour::fromRGB(0xEEF5F8));
    g.fillRect(area);
    g.setColour(Colour(0, 0, 0, 160));
    g.drawRect(area.reduced(0.5f), 1.0f);
  }

  void drawPopupItem(Graphics& g, const Rect& row, const PopupItem& item, const Font& font,
                     bool highlighted) override {
    if (highlighted && item.enabled && !item.isHeader) {
      g.setColour(Colour::fromARGB(0xBFA4C2CEu));
      g.fillRect(Rect(1.0f, row.y, row.w - 2.0f, row.h));
    }
    LookAndFeel::drawPopupItem(g, row, item, font, false);
  }

  void drawTab(Graphics& g, const Rect& area, const std::string& name, const Font& font,
               bool current, bool hovered) override {
    const Rect tab = current ? area : Rect(area.x, area.y + 2.0f, area.w, area.h - 2.0f);
    g.setColour(current ? panel() : (hovered ? background().brightened(0.03f) : background().darkened(0.04f)));
    g.fillRect(tab);
    detail::lafBevel(g, tab, 1.0f, false);
    g.setColour(current ? text() : dimText().darkened(0.2f));
    font.drawIn(g, name, tab, Justify::Centred);
  }

  void drawListRow(Graphics& g, const Rect& row, const std::string& label, const Font& font,
                   bool selected) override {
    if (selected) {
      g.setColour(accent());
      g.fillRect(row);
    }
    g.setColour(text());
    font.drawIn(g, label, Rect(row.x + 6.0f, row.y, row.w - 12.0f, row.h), Justify::Left);
  }

  void drawProgressBar(Graphics& g, const Rect& area, double progress, const std::string& label,
                       const Font& font) override {
    g.setColour(Colour::fromRGB(0xFFFFFF).interpolatedTo(Colour::fromRGB(0x0000FF), 0.4f));
    g.fillRect(area);
    const Rect inner = area.reduced(2.0f);
    const Rect fill = progress >= 0.0
                          ? Rect(inner.x, inner.y, inner.w * (float) (progress > 1.0 ? 1.0 : progress), inner.h)
                          : inner.reduced(inner.w * 0.25f, 0.0f);
    if (!fill.isEmpty()) {
      g.setColour(Colour(0, 0, 255, 178));
      g.fillRect(fill);
    }
    detail::lafBevel(g, area, 1.0f, /*sunken=*/true);
    if (!label.empty()) {
      g.setColour(text());
      font.drawIn(g, label, area, Justify::Centred);
    }
  }

  void drawTooltip(Graphics& g, const Rect& area, const std::string& message,
                   const Font& font) override {
    g.setColour(Colour::fromRGB(0xEEEEBB));
    g.fillRect(area);
    g.setColour(Colour(0, 0, 0, 255));
    g.drawRect(area.reduced(0.5f), 1.0f);
    g.setColour(text());
    font.drawIn(g, message, area, Justify::Centred);
  }

  void drawIconButtonBackground(Graphics& g, const Rect& area, const WidgetState& st,
                                bool toggled) override {
    g.setColour(toggled ? accent() : panel());
    g.fillRect(area);
    detail::lafBevel(g, area, 1.0f, st.pressed || toggled);
  }

  void drawResizableCorner(Graphics& g, const Rect& area, bool active) override {
    (void) active;
    for (int i = 1; i <= 3; ++i) {
      const float inset = (float) i * 4.0f;
      g.setColour(Colour(255, 255, 255, 220));
      g.drawLine(area.right() - inset, area.bottom() - 1.0f, area.right() - 1.0f,
                 area.bottom() - inset, 1.0f);
      g.setColour(Colour(0, 0, 0, 140));
      g.drawLine(area.right() - inset + 1.0f, area.bottom(), area.right(),
                 area.bottom() - inset + 1.0f, 1.0f);
    }
  }
};

// ═══════════════════════════════════════════════════════════════════════════
// V2 -- glass
// ═══════════════════════════════════════════════════════════════════════════

/**
 * The reference's second default look: lavender glass lozenge buttons with a
 * highlight over their top half, sphere-shaped thumbs on a shadowed groove,
 * and white popups with a deep-blue highlight. Same name as the reference's
 * class; see the file comment for the four versions.
 *
 * Stateless, and the base of LookAndFeel_V3. Like any LookAndFeel it must
 * outlive the widgets it is set on.
 */
class LookAndFeel_V2 : public LookAndFeel {
public:
  Colour background() const override { return Colour::fromRGB(0xF2F2F2); }
  Colour panel() const override { return Colour::fromRGB(0xFFFFFF); }
  Colour outline() const override { return Colour(0, 0, 0, 0x66); }
  Colour text() const override { return Colour::fromRGB(0x000000); }
  Colour dimText() const override { return Colour(0, 0, 0, 0x80); }
  Colour accent() const override { return Colour::fromRGB(0xBBBBFF); }

  void drawFocusRing(Graphics& g, const Rect& area) const override {
    g.setColour(Colour::fromARGB(0xA06A6AFFu));
    g.drawRoundedRect(area.reduced(0.5f), 4.0f, 1.5f);
  }

  void drawButton(Graphics& g, const Rect& area, const std::string& label, const Font& font,
                  const WidgetState& st) override {
    // The lozenge: a pill, lavender off and deep blue on.
    Colour base = st.toggled ? Colour::fromRGB(0x4444FF) : accent();
    if (st.hovered && !st.pressed) base = base.brightened(0.06f);
    if (!st.enabled) base = base.withMultipliedAlpha(0.5f);
    const Rect r = area.reduced(1.0f);
    detail::lafGlassLozenge(g, r, base, (std::min)(r.w, r.h) * 0.5f, st.pressed);
    if (label.empty()) return;
    g.setColour(st.toggled ? Colour::fromRGB(0xFFFFFF) : (st.enabled ? text() : dimText()));
    font.drawIn(g, label, area, Justify::Centred);
  }

  void drawRotarySlider(Graphics& g, const Rect& area, float normalised,
                        const WidgetState& st) override {
    const Rect dial = area.withAspect(1.0f).reduced(3.0f);
    if (dial.isEmpty()) return;
    const float from = rotaryStart(), to = rotaryEnd();
    const float at = from + (to - from) * clamp01(normalised);
    // A ring band from 70 % of the radius, outlined over its whole sweep and
    // filled translucent blue up to the value: V2's knob.
    g.setColour(Colour(0, 0, 0, 0x10));
    g.fillPath(detail::lafRingSegment(dial, from, to, 0.7f));
    if (normalised > 0.0f) {
      g.setColour(Colour::fromARGB(st.enabled ? (st.hovered ? 0x9F0000FFu : 0x7F0000FFu)
                                              : 0x400000FFu));
      g.fillPath(detail::lafRingSegment(dial, from, at, 0.7f));
    }
    g.setColour(Colour(0, 0, 0, 0x66));
    g.strokePath(detail::lafRingSegment(dial, from, to, 0.7f), StrokeStyle{1.0f});
    // The centre, as a small glass sphere.
    const float inner = dial.w * 0.32f;
    const Point c = dial.centre();
    detail::lafGlassSphere(g, Rect(c.x - inner, c.y - inner, inner * 2.0f, inner * 2.0f), accent());
  }

  void drawLinearSlider(Graphics& g, const Rect& area, float normalised, bool vertical,
                        const WidgetState& st) override {
    const float v = clamp01(normalised);
    const float grooveW = 6.0f, thumb = 15.0f;
    const Rect groove = vertical ? Rect(area.centre().x - grooveW * 0.5f, area.y + thumb * 0.5f,
                                        grooveW, area.h - thumb)
                                 : Rect(area.x + thumb * 0.5f, area.centre().y - grooveW * 0.5f,
                                        area.w - thumb, grooveW);
    if (groove.isEmpty()) return;
    // A shadowed groove: darkest along the edge the light does not reach.
    g.setGradient(vertical ? ColourGradient::horizontal(Colour(0, 0, 0, 80), groove.x,
                                                        Colour(0, 0, 0, 20), groove.right())
                           : ColourGradient::vertical(Colour(0, 0, 0, 80), groove.y,
                                                      Colour(0, 0, 0, 20), groove.bottom()));
    g.fillRoundedRect(groove, grooveW * 0.5f);
    g.setColour(Colour(0, 0, 0, 0x4C));
    g.drawRoundedRect(groove, grooveW * 0.5f, 1.0f);
    const Point centre = vertical ? Point(groove.centre().x, groove.bottom() - groove.h * v)
                                  : Point(groove.x + groove.w * v, groove.centre().y);
    Colour ball = st.enabled ? accent() : accent().withMultipliedAlpha(0.5f);
    if (st.pressed) ball = ball.darkened(0.08f);
    else if (st.hovered) ball = ball.brightened(0.05f);
    detail::lafGlassSphere(g, Rect(centre.x - thumb * 0.5f, centre.y - thumb * 0.5f, thumb, thumb),
                           ball);
  }

  void drawLinearBar(Graphics& g, const Rect& area, float normalised, bool vertical,
                     const WidgetState& st) override {
    const float v = clamp01(normalised);
    g.setColour(Colour::fromRGB(0xFFFFFF));
    g.fillRoundedRect(area, 3.0f);
    Rect fill = area.reduced(1.0f);
    if (vertical) {
      fill.h *= v;
      fill.y = area.bottom() - 1.0f - fill.h;
    } else {
      fill.w *= v;
    }
    if (fill.w > 2.0f && fill.h > 2.0f)
      detail::lafGlassLozenge(g, fill, st.enabled ? Colour::fromRGB(0xAAAAEE) : dimText(), 2.5f,
                              false);
    g.setColour(outline());
    g.drawRoundedRect(area, 3.0f, 1.0f);
  }

  void drawComboBox(Graphics& g, const Rect& area, const std::string& label, const Font& font,
                    const WidgetState& st) override {
    g.setColour(Colour::fromRGB(0xFFFFFF));
    g.fillRect(area);
    g.setColour(outline());
    g.drawRect(area.reduced(0.5f), 1.0f);
    const float b = area.h - 2.0f;
    const Rect button(area.right() - b - 1.0f, area.y + 1.0f, b, b);
    detail::lafGlassLozenge(g, button, st.hovered ? accent().brightened(0.05f) : accent(), 3.0f,
                            st.pressed);
    g.setColour(Colour(0, 0, 0, st.enabled ? 0xCC : 0x55));
    g.fillPath(detail::lafDownTriangle(button));
    g.setColour(st.enabled ? text() : dimText());
    font.drawIn(g, label, Rect(area.x + 5.0f, area.y, area.w - b - 10.0f, area.h), Justify::Left);
  }

  void drawScrollBar(Graphics& g, const Rect& track, const Rect& thumb, bool vertical,
                     bool active) override {
    const float r = (vertical ? track.w : track.h) * 0.5f;
    g.setGradient(vertical ? ColourGradient::horizontal(Colour(0, 0, 0, 40), track.x,
                                                        Colour(0, 0, 0, 10), track.right())
                           : ColourGradient::vertical(Colour(0, 0, 0, 40), track.y,
                                                      Colour(0, 0, 0, 10), track.bottom()));
    g.fillRoundedRect(track, r);
    const Rect t = vertical ? thumb.reduced(1.0f, 1.0f) : thumb.reduced(1.0f, 1.0f);
    detail::lafGlassLozenge(g, t, active ? Colour::fromRGB(0xA8A8DD) : Colour::fromRGB(0xBBBBDD),
                            (std::min)(t.w, t.h) * 0.5f, false);
  }

  void drawTextEditorBackground(Graphics& g, const Rect& area, bool focused,
                                bool enabled) override {
    g.setColour(enabled ? Colour::fromRGB(0xFFFFFF) : Colour::fromRGB(0xF4F4F4));
    g.fillRect(area);
    if (focused) {
      g.setColour(Colour::fromARGB(0x6A6A6AFFu));
      g.drawRect(area.reduced(1.0f), 2.0f);
    } else {
      g.setColour(Colour(0, 0, 0, 0x1C));
      g.drawRect(area.reduced(0.5f), 1.0f);
    }
  }

  void drawTextEditorSelection(Graphics& g, const Rect& area) override {
    g.setColour(Colour::fromARGB(0xC01111EEu).withMultipliedAlpha(0.4f));
    g.fillRect(area);
  }

  void drawTextEditorCaret(Graphics& g, const Rect& area) override {
    g.setColour(Colour::fromRGB(0x000000));
    g.fillRect(area);
  }

  void drawPopupBackground(Graphics& g, const Rect& area) override {
    g.setColour(Colour::fromRGB(0xFFFFFF));
    g.fillRect(area);
    g.setColour(Colour(0, 0, 0, 0x60));
    g.drawRect(area.reduced(0.5f), 1.0f);
  }

  void drawPopupItem(Graphics& g, const Rect& row, const PopupItem& item, const Font& font,
                     bool highlighted) override {
    if (!(highlighted && item.enabled && !item.isHeader)) {
      LookAndFeel::drawPopupItem(g, row, item, font, false);
      return;
    }
    // The deep translucent blue band, with white text over it.
    g.setColour(Colour::fromARGB(0x991111AAu));
    g.fillRect(Rect(1.0f, row.y, row.w - 2.0f, row.h));
    g.setColour(Colour::fromRGB(0xFFFFFF));
    font.drawIn(g, item.text,
                Rect(kPopupPadX + kPopupTickWidth, row.y,
                     row.w - kPopupPadX * 2.0f - kPopupTickWidth, row.h),
                Justify::Left);
  }

  void drawTab(Graphics& g, const Rect& area, const std::string& name, const Font& font,
               bool current, bool hovered) override {
    const Rect tab = current ? area : Rect(area.x + 1.0f, area.y + 2.0f, area.w - 2.0f, area.h - 2.0f);
    const Colour base = current ? Colour::fromRGB(0xFFFFFF)
                                : (hovered ? Colour::fromRGB(0xE4E4F4) : Colour::fromRGB(0xD8D8E8));
    g.setGradient(ColourGradient::vertical(base.brightened(0.06f), tab.y, base.darkened(0.08f),
                                           tab.bottom()));
    g.fillRoundedRect(tab, 4.0f);
    g.setColour(Colour(0, 0, 0, 0x66));
    g.drawRoundedRect(tab, 4.0f, 1.0f);
    g.setColour(current ? text() : dimText());
    font.drawIn(g, name, tab, Justify::Centred);
  }

  void drawListRow(Graphics& g, const Rect& row, const std::string& label, const Font& font,
                   bool selected) override {
    if (selected) {
      g.setColour(Colour::fromARGB(0x401111EEu));
      g.fillRect(row);
    }
    g.setColour(text());
    font.drawIn(g, label, Rect(row.x + 6.0f, row.y, row.w - 12.0f, row.h), Justify::Left);
  }

  void drawProgressBar(Graphics& g, const Rect& area, double progress, const std::string& label,
                       const Font& font) override {
    g.setColour(Colour::fromRGB(0xEEEEEE));
    g.fillRoundedRect(area, 3.0f);
    const Rect inner = area.reduced(1.0f);
    if (progress >= 0.0) {
      const float w = inner.w * (float) (progress > 1.0 ? 1.0 : progress);
      if (w > 2.0f)
        detail::lafGlassLozenge(g, Rect(inner.x, inner.y, w, inner.h), Colour::fromRGB(0xAAAAEE),
                                2.5f, false);
    } else {
      // Indeterminate: V2's diagonal stripes, still -- an animation needs a
      // timer, which the base class explains why nobody here runs.
      Graphics::ScopedState s(g);
      g.clipTo(inner);
      g.setColour(Colour::fromRGB(0xAAAAEE));
      const float stripe = inner.h;
      // COUNTED, not stepped until past the end. The step is the bar's own
      // height, and a bar laid out two pixels tall or less has an inner
      // height of zero or below: the loop never ended and the paint never
      // returned. Under a pixel there is no stripe to see; past a million
      // pixels of width there is no bar anybody sees whole.
      const float span = inner.w + inner.h;
      const int count = stripe >= 1.0f && span > 0.0f && span < 1.0e6f ? (int) (span / (stripe * 2.0f)) + 1 : 0;
      for (int i = 0; i < count; ++i) {
        const float x = inner.x - inner.h + (float) i * stripe * 2.0f;
        Path p;
        p.moveTo(x, inner.bottom());
        p.lineTo(x + stripe, inner.bottom());
        p.lineTo(x + stripe + inner.h, inner.y);
        p.lineTo(x + inner.h, inner.y);
        p.close();
        g.fillPath(p);
      }
    }
    g.setColour(Colour(0, 0, 0, 0x55));
    g.drawRoundedRect(area, 3.0f, 1.0f);
    if (!label.empty()) {
      g.setColour(text());
      font.drawIn(g, label, area, Justify::Centred);
    }
  }

  void drawTooltip(Graphics& g, const Rect& area, const std::string& message,
                   const Font& font) override {
    g.setColour(Colour::fromRGB(0xEEEEBB));
    g.fillRect(area);
    g.setColour(Colour(0, 0, 0, 0x80));
    g.drawRect(area.reduced(0.5f), 1.0f);
    g.setColour(text());
    font.drawIn(g, message, area, Justify::Centred);
  }

  void drawIconButtonBackground(Graphics& g, const Rect& area, const WidgetState& st,
                                bool toggled) override {
    detail::lafGlassLozenge(g, area.reduced(1.0f), toggled ? Colour::fromRGB(0x4444FF) : accent(),
                            4.0f, st.pressed);
  }

  void drawRangeSlider(Graphics& g, const Rect& area, float low, float high,
                       const WidgetState& st) override {
    const float thumb = 14.0f;
    const Rect groove(area.x + thumb * 0.5f, area.centre().y - 3.0f, area.w - thumb, 6.0f);
    if (groove.isEmpty()) return;
    g.setGradient(ColourGradient::vertical(Colour(0, 0, 0, 80), groove.y, Colour(0, 0, 0, 20),
                                           groove.bottom()));
    g.fillRoundedRect(groove, 3.0f);
    const float x0 = groove.x + groove.w * clamp01(low), x1 = groove.x + groove.w * clamp01(high);
    g.setColour(Colour::fromARGB(0x7F0000FFu));
    g.fillRect(Rect(x0, groove.y, x1 - x0, groove.h));
    for (float x : {x0, x1})
      detail::lafGlassSphere(g, Rect(x - thumb * 0.5f, area.centre().y - thumb * 0.5f, thumb, thumb),
                             st.enabled ? accent() : accent().withMultipliedAlpha(0.5f));
  }
};

// ═══════════════════════════════════════════════════════════════════════════
// V3 -- V2 with the gloss taken off
// ═══════════════════════════════════════════════════════════════════════════

/**
 * The reference's third default look: V2 flattened -- square-ish buttons with
 * a faint gradient, thin translucent scroll thumbs, flat tabs and bars. Same
 * name as the reference's class, and derived from LookAndFeel_V2 as the
 * reference's is, so what V3 does not redraw it draws as V2.
 *
 * Stateless. Like any LookAndFeel it must outlive the widgets it is set on.
 */
class LookAndFeel_V3 : public LookAndFeel_V2 {
public:
  Colour background() const override { return Colour::fromRGB(0xDDDDDD); }
  Colour panel() const override { return Colour::fromRGB(0xEEEEFF); }
  Colour outline() const override { return Colour(0, 0, 0, 0x66); }
  /** The value colour of V3's flat bars and arcs. */
  Colour accent() const override { return Colour::fromRGB(0x8A8AD8); }

  void drawButton(Graphics& g, const Rect& area, const std::string& label, const Font& font,
                  const WidgetState& st) override {
    Colour base = st.toggled ? accent() : panel();
    if (st.pressed) base = base.darkened(0.10f);
    else if (st.hovered) base = base.brightened(0.04f);
    if (!st.enabled) base = base.withMultipliedAlpha(0.5f);
    // The whole of V3's lighting is this: a ramp of a few per cent.
    g.setGradient(ColourGradient::vertical(base.brightened(0.04f), area.y, base.darkened(0.06f),
                                           area.bottom()));
    g.fillRoundedRect(area.reduced(0.5f), 3.0f);
    g.setColour(outline());
    g.drawRoundedRect(area.reduced(0.5f), 3.0f, 1.0f);
    if (label.empty()) return;
    g.setColour(st.toggled ? Colour::fromRGB(0xFFFFFF) : (st.enabled ? text() : dimText()));
    font.drawIn(g, label, area, Justify::Centred);
  }

  void drawRotarySlider(Graphics& g, const Rect& area, float normalised,
                        const WidgetState& st) override {
    const Rect dial = area.withAspect(1.0f).reduced(3.0f);
    if (dial.isEmpty()) return;
    const float from = rotaryStart(), to = rotaryEnd();
    const float at = from + (to - from) * clamp01(normalised);
    g.setColour(Colour::fromRGB(0xF6F6F6));
    g.fillEllipse(dial);
    g.setColour(outline());
    g.drawEllipse(dial, 1.0f);
    const float thickness = (std::max)(2.0f, dial.w * 0.08f);
    const Rect ring = dial.reduced(thickness);
    if (normalised > 0.0f) {
      Path arc;
      arc.addArc(ring, from, at);
      g.setColour(st.enabled ? accent() : dimText());
      g.strokePath(arc, StrokeStyle{thickness, LineJoin::Round, LineCap::Butt});
    }
    const Point c = dial.centre();
    const float r = ring.w * 0.5f - thickness;
    g.setColour(Colour(0, 0, 0, 0xB0));
    g.drawLine(c.x, c.y, c.x + std::sin(at) * r, c.y - std::cos(at) * r, 2.0f);
  }

  void drawLinearSlider(Graphics& g, const Rect& area, float normalised, bool vertical,
                        const WidgetState& st) override {
    const float v = clamp01(normalised);
    const float trackW = 4.0f, thumbLong = 16.0f, thumbShort = 9.0f;
    const Rect track = vertical ? Rect(area.centre().x - trackW * 0.5f, area.y + thumbShort * 0.5f,
                                       trackW, area.h - thumbShort)
                                : Rect(area.x + thumbShort * 0.5f, area.centre().y - trackW * 0.5f,
                                       area.w - thumbShort, trackW);
    if (track.isEmpty()) return;
    g.setColour(Colour(255, 255, 255, 0xBB));
    g.fillRoundedRect(track, 2.0f);
    g.setColour(Colour(0, 0, 0, 0x40));
    g.drawRoundedRect(track, 2.0f, 1.0f);
    const Rect thumb =
        vertical ? Rect(area.centre().x - thumbLong * 0.5f,
                        track.bottom() - track.h * v - thumbShort * 0.5f, thumbLong, thumbShort)
                 : Rect(track.x + track.w * v - thumbShort * 0.5f,
                        area.centre().y - thumbLong * 0.5f, thumbShort, thumbLong);
    Colour face = Colour::fromRGB(0xDDDDFF);
    if (st.pressed) face = face.darkened(0.08f);
    else if (st.hovered) face = face.brightened(0.04f);
    if (!st.enabled) face = face.withMultipliedAlpha(0.5f);
    g.setColour(face);
    g.fillRoundedRect(thumb, 2.0f);
    g.setColour(Colour(0, 0, 0, 0x80));
    g.drawRoundedRect(thumb, 2.0f, 1.0f);
  }

  void drawLinearBar(Graphics& g, const Rect& area, float normalised, bool vertical,
                     const WidgetState& st) override {
    const float v = clamp01(normalised);
    g.setColour(Colour::fromRGB(0xFFFFFF));
    g.fillRect(area);
    Rect fill = area;
    if (vertical) {
      fill.h *= v;
      fill.y = area.bottom() - fill.h;
    } else {
      fill.w *= v;
    }
    if (!fill.isEmpty()) {
      g.setColour(st.enabled ? accent() : dimText());
      g.fillRect(fill);
    }
    g.setColour(outline());
    g.drawRect(area.reduced(0.5f), 1.0f);
  }

  void drawComboBox(Graphics& g, const Rect& area, const std::string& label, const Font& font,
                    const WidgetState& st) override {
    g.setColour(st.hovered ? Colour::fromRGB(0xF8F8FF) : Colour::fromRGB(0xFFFFFF));
    g.fillRect(area);
    g.setColour(outline());
    g.drawRect(area.reduced(0.5f), 1.0f);
    const float b = area.h;
    g.drawLine(area.right() - b, area.y + 3.0f, area.right() - b, area.bottom() - 3.0f, 1.0f);
    g.setColour(Colour(0, 0, 0, st.enabled ? 0xA0 : 0x50));
    g.fillPath(detail::lafDownTriangle(Rect(area.right() - b, area.y, b, b)));
    g.setColour(st.enabled ? text() : dimText());
    font.drawIn(g, label, Rect(area.x + 5.0f, area.y, area.w - b - 10.0f, area.h), Justify::Left);
  }

  void drawScrollBar(Graphics& g, const Rect& track, const Rect& thumb, bool vertical,
                     bool active) override {
    (void) track;
    // No track at all, and a thumb that is barely there: a thirteen per cent
    // shadow of the background, darker while it is held.
    const Rect t = vertical ? thumb.reduced(2.0f, 1.0f) : thumb.reduced(1.0f, 2.0f);
    g.setColour(Colour(0, 0, 0, active ? 0x48 : 0x22));
    g.fillRoundedRect(t, (std::min)(t.w, t.h) * 0.5f);
  }

  void drawTextEditorBackground(Graphics& g, const Rect& area, bool focused,
                                bool enabled) override {
    g.setColour(enabled ? Colour::fromRGB(0xFFFFFF) : Colour::fromRGB(0xF0F0F0));
    g.fillRect(area);
    if (focused) {
      g.setColour(accent());
      g.drawRect(area.reduced(0.75f), 1.5f);
    }
  }

  void drawPopupBackground(Graphics& g, const Rect& area) override {
    g.setColour(Colour::fromRGB(0xFAFAFA));
    g.fillRect(area);
    g.setColour(Colour(0, 0, 0, 0x40));
    g.drawRect(area.reduced(0.5f), 1.0f);
  }

  void drawPopupItem(Graphics& g, const Rect& row, const PopupItem& item, const Font& font,
                     bool highlighted) override {
    if (!(highlighted && item.enabled && !item.isHeader)) {
      LookAndFeel::drawPopupItem(g, row, item, font, false);
      return;
    }
    g.setColour(accent());
    g.fillRect(Rect(0.0f, row.y, row.w, row.h));
    g.setColour(Colour::fromRGB(0xFFFFFF));
    font.drawIn(g, item.text,
                Rect(kPopupPadX + kPopupTickWidth, row.y,
                     row.w - kPopupPadX * 2.0f - kPopupTickWidth, row.h),
                Justify::Left);
  }

  void drawTab(Graphics& g, const Rect& area, const std::string& name, const Font& font,
               bool current, bool hovered) override {
    g.setColour(current ? Colour::fromRGB(0xFFFFFF)
                        : (hovered ? Colour::fromRGB(0xE8E8E8) : Colour::fromRGB(0xD2D2D2)));
    g.fillRect(area);
    g.setColour(outline());
    g.drawRect(area.reduced(0.5f), 1.0f);
    g.setColour(current ? text() : dimText());
    font.drawIn(g, name, area, Justify::Centred);
  }

  void drawProgressBar(Graphics& g, const Rect& area, double progress, const std::string& label,
                       const Font& font) override {
    g.setColour(Colour::fromRGB(0xFFFFFF));
    g.fillRect(area);
    const Rect fill = progress >= 0.0
                          ? Rect(area.x, area.y, area.w * (float) (progress > 1.0 ? 1.0 : progress), area.h)
                          : area.reduced(area.w * 0.25f, 0.0f);
    if (!fill.isEmpty()) {
      g.setColour(accent());
      g.fillRect(fill);
    }
    g.setColour(outline());
    g.drawRect(area.reduced(0.5f), 1.0f);
    if (!label.empty()) {
      g.setColour(text());
      font.drawIn(g, label, area, Justify::Centred);
    }
  }

  void drawIconButtonBackground(Graphics& g, const Rect& area, const WidgetState& st,
                                bool toggled) override {
    Colour base = toggled ? accent() : panel();
    if (st.pressed) base = base.darkened(0.1f);
    g.setColour(base);
    g.fillRoundedRect(area, 3.0f);
    g.setColour(outline());
    g.drawRoundedRect(area, 3.0f, 1.0f);
  }

  void drawRangeSlider(Graphics& g, const Rect& area, float low, float high,
                       const WidgetState& st) override {
    LookAndFeel::drawRangeSlider(g, area, low, high, st);
  }
};

// ═══════════════════════════════════════════════════════════════════════════
// V4 -- the colour-scheme look
// ═══════════════════════════════════════════════════════════════════════════

/**
 * The reference's fourth default look: every control drawn from a nine-colour
 * ColourScheme, with arc knobs, round thumbs on line tracks and pill scroll
 * thumbs. Same name as the reference's class, and the same four schemes
 * (dark, the default, then midnight, grey and light), verbatim.
 *
 * The scheme is the one piece of state: change it with setColourScheme, or
 * through the reference held by getCurrentColourScheme, then repaint the
 * editor -- nothing is told it changed. Like any LookAndFeel it must outlive
 * the widgets it is set on, and is used on the message thread.
 */
class LookAndFeel_V4 : public LookAndFeel {
public:
  /**
   * Nine colours, from which every control is drawn. The reference's own
   * table and its own order.
   */
  class ColourScheme {
  public:
    /** The nine slots, in the reference's order. */
    enum UIColour {
      windowBackground = 0,  ///< the editor's ground: background()
      widgetBackground,  ///< a control's body: panel()
      menuBackground,  ///< a popup menu's body
      outline,  ///< edges and tracks: outline()
      defaultText,  ///< text: text(), and dimText() at half its alpha
      defaultFill,  ///< a control's value, the focus ring: accent()
      highlightedText,  ///< text on a highlighted fill
      highlightedFill,  ///< a toggled button, a highlighted menu row, a selected item
      menuText,  ///< text in a menu
      numColours  ///< how many there are; not a colour
    };

    /** Every colour opaque black, to be filled in with setUIColour. */
    ColourScheme() = default;

    /** The nine colours as 0xAARRGGBB, in UIColour order. */
    ColourScheme(uint32_t windowBg, uint32_t widgetBg, uint32_t menuBg, uint32_t outlineColour,
                 uint32_t text, uint32_t fill, uint32_t hiText, uint32_t hiFill, uint32_t menuTextColour) {
      const uint32_t all[numColours] = {windowBg, widgetBg, menuBg, outlineColour, text,
                                        fill,     hiText,   hiFill, menuTextColour};
      for (int i = 0; i < numColours; ++i) colours_[i] = Colour::fromARGB(all[i]);
    }

    /** One slot's colour; opaque black for one out of range. */
    Colour getUIColour(UIColour which) const {
      return (which >= 0 && which < numColours) ? colours_[which] : Colour();
    }
    /** Replace one slot's colour; ignored out of range. */
    void setUIColour(UIColour which, Colour c) {
      if (which >= 0 && which < numColours) colours_[which] = c;
    }

    /** Whether all nine colours are the same. */
    bool operator==(const ColourScheme& o) const {
      for (int i = 0; i < numColours; ++i)
        if (colours_[i] != o.colours_[i]) return false;
      return true;
    }
    /** Whether any of the nine differs. */
    bool operator!=(const ColourScheme& o) const { return !(*this == o); }

  private:
    Colour colours_[numColours];
  };

  /** The default: blue-grey surfaces, white text, a cyan fill. */
  static ColourScheme getDarkColourScheme() {
    return {0xff323e44, 0xff263238, 0xff323e44, 0xff8e989b, 0xffffffff,
            0xff42a2c8, 0xffffffff, 0xff181f22, 0xffffffff};
  }
  /** Deep blue-violet surfaces with pale grey fills and a light menu. */
  static ColourScheme getMidnightColourScheme() {
    return {0xff2f2f3a, 0xff191926, 0xffd0d0d0, 0xff66667c, 0xc8ffffff,
            0xffd8d8d8, 0xffffffff, 0xff606073, 0xff000000};
  }
  /** Mid-grey surfaces, white text, a green fill. */
  static ColourScheme getGreyColourScheme() {
    return {0xff505050, 0xff424242, 0xff606060, 0xffa6a6a6, 0xffffffff,
            0xff21ba90, 0xff000000, 0xffffffff, 0xffffffff};
  }
  /** Pale grey and white surfaces, black text, a blue highlight. */
  static ColourScheme getLightColourScheme() {
    return {0xffefefef, 0xffffffff, 0xffffffff, 0xffdddddd, 0xff000000,
            0xffa9a9a9, 0xffffffff, 0xff42a2c8, 0xff000000};
  }

  /** Dark, as the reference defaults to. */
  LookAndFeel_V4() : scheme_(getDarkColourScheme()) {}
  /** Drawn from `scheme`. */
  explicit LookAndFeel_V4(ColourScheme scheme) : scheme_(scheme) {}

  /** Replace the scheme. Nothing repaints by itself: repaint the editor after. */
  void setColourScheme(const ColourScheme& scheme) { scheme_ = scheme; }
  /** The scheme in use, to change in place -- then repaint the editor. */
  ColourScheme& getCurrentColourScheme() { return scheme_; }
  /** The scheme in use. */
  const ColourScheme& getCurrentColourScheme() const { return scheme_; }

  /** One colour of the current scheme. */
  Colour ui(ColourScheme::UIColour c) const { return scheme_.getUIColour(c); }

  // The palette, mapped the way the reference maps its colour ids onto the
  // scheme: windows on windowBackground, controls on widgetBackground, the
  // value of a control in defaultFill or highlightedFill.
  Colour background() const override { return ui(ColourScheme::windowBackground); }
  Colour panel() const override { return ui(ColourScheme::widgetBackground); }
  Colour outline() const override { return ui(ColourScheme::outline); }
  Colour text() const override { return ui(ColourScheme::defaultText); }
  Colour dimText() const override { return ui(ColourScheme::defaultText).withMultipliedAlpha(0.5f); }
  Colour accent() const override { return ui(ColourScheme::defaultFill); }

  /** V4's knobs sweep further than this SDK's default: 0.8 of a half-turn
   *  either side of twelve o'clock. */
  float rotaryStart() const override { return -2.5133f; }
  float rotaryEnd() const override { return 2.5133f; }

  void drawFocusRing(Graphics& g, const Rect& area) const override {
    g.setColour(ui(ColourScheme::defaultFill));
    g.drawRoundedRect(area.reduced(0.5f), 4.0f, 1.5f);
  }

  void drawRotarySlider(Graphics& g, const Rect& area, float normalised,
                        const WidgetState& st) override {
    const Rect bounds = area.withAspect(1.0f).reduced(10.0f);
    if (bounds.isEmpty()) return;
    const float radius = bounds.w * 0.5f;
    const float from = rotaryStart(), to = rotaryEnd();
    const float at = from + (to - from) * clamp01(normalised);
    const float lineW = (std::min)(8.0f, radius * 0.5f);
    const float arcRadius = radius - lineW * 0.5f;
    const Point c = bounds.centre();
    const Rect arcBox(c.x - arcRadius, c.y - arcRadius, arcRadius * 2.0f, arcRadius * 2.0f);

    Path track;
    track.addArc(arcBox, from, to);
    g.setColour(ui(ColourScheme::widgetBackground).brightened(0.06f));
    g.strokePath(track, StrokeStyle{lineW, LineJoin::Round, LineCap::Round});
    if (st.enabled && normalised > 0.0f) {
      Path value;
      value.addArc(arcBox, from, at);
      g.setColour(ui(ColourScheme::highlightedFill).interpolatedTo(ui(ColourScheme::defaultFill),
                                                                  st.hovered ? 0.6f : 0.45f));
      g.strokePath(value, StrokeStyle{lineW, LineJoin::Round, LineCap::Round});
    }
    // The thumb: a dot at the end of the value arc, twice the line's width.
    const float thumb = lineW * 2.0f;
    const Point p(c.x + arcRadius * std::sin(at), c.y - arcRadius * std::cos(at));
    g.setColour(st.enabled ? ui(ColourScheme::defaultFill) : ui(ColourScheme::outline));
    g.fillEllipse(Rect(p.x - thumb * 0.5f, p.y - thumb * 0.5f, thumb, thumb));
  }

  void drawLinearSlider(Graphics& g, const Rect& area, float normalised, bool vertical,
                        const WidgetState& st) override {
    const float v = clamp01(normalised);
    const float trackW = (std::min)(6.0f, (vertical ? area.w : area.h) * 0.25f);
    const float thumb = (std::min)(16.0f, (vertical ? area.w : area.h) * 0.5f);
    const Point start = vertical ? Point(area.centre().x, area.bottom() - thumb * 0.5f)
                                 : Point(area.x + thumb * 0.5f, area.centre().y);
    const Point end = vertical ? Point(area.centre().x, area.y + thumb * 0.5f)
                               : Point(area.right() - thumb * 0.5f, area.centre().y);
    const Point at(start.x + (end.x - start.x) * v, start.y + (end.y - start.y) * v);

    Path track;
    track.moveTo(start.x, start.y);
    track.lineTo(end.x, end.y);
    g.setColour(ui(ColourScheme::widgetBackground).brightened(0.06f));
    g.strokePath(track, StrokeStyle{trackW, LineJoin::Round, LineCap::Round});
    if (v > 0.0f) {
      Path value;
      value.moveTo(start.x, start.y);
      value.lineTo(at.x, at.y);
      g.setColour(st.enabled ? ui(ColourScheme::defaultFill).darkened(0.12f) : ui(ColourScheme::outline));
      g.strokePath(value, StrokeStyle{trackW, LineJoin::Round, LineCap::Round});
    }
    Colour t = ui(ColourScheme::defaultFill);
    if (st.pressed) t = t.brightened(0.1f);
    else if (st.hovered) t = t.brightened(0.05f);
    g.setColour(st.enabled ? t : ui(ColourScheme::outline));
    g.fillEllipse(Rect(at.x - thumb * 0.5f, at.y - thumb * 0.5f, thumb, thumb));
  }

  void drawLinearBar(Graphics& g, const Rect& area, float normalised, bool vertical,
                     const WidgetState& st) override {
    const float v = clamp01(normalised);
    g.setColour(ui(ColourScheme::widgetBackground));
    g.fillRect(area);
    Rect fill = area;
    if (vertical) {
      fill.h *= v;
      fill.y = area.bottom() - fill.h;
    } else {
      fill.w *= v;
    }
    if (!fill.isEmpty()) {
      g.setColour(st.enabled ? ui(ColourScheme::defaultFill) : ui(ColourScheme::outline));
      g.fillRect(fill);
    }
    g.setColour(ui(ColourScheme::outline));
    g.drawRect(area.reduced(0.5f), 1.0f);
  }

  void drawButton(Graphics& g, const Rect& area, const std::string& label, const Font& font,
                  const WidgetState& st) override {
    Colour base = st.toggled ? ui(ColourScheme::highlightedFill) : ui(ColourScheme::widgetBackground);
    // contrasting(): towards white on a dark scheme, towards black on a light
    // one -- the reference's hover and down shading, which therefore works in
    // all four schemes without a second set of colours.
    const bool dark = base.brightness() < 0.5f;
    if (st.pressed) base = dark ? base.brightened(0.20f) : base.darkened(0.20f);
    else if (st.hovered) base = dark ? base.brightened(0.05f) : base.darkened(0.05f);
    if (!st.enabled) base = base.withMultipliedAlpha(0.5f);
    const Rect r = area.reduced(0.5f);
    g.setColour(base);
    g.fillRoundedRect(r, 6.0f);
    g.setColour(ui(ColourScheme::outline));
    g.drawRoundedRect(r, 6.0f, 1.0f);
    if (label.empty()) return;
    Colour ink = st.toggled ? ui(ColourScheme::highlightedText) : ui(ColourScheme::defaultText);
    if (!st.enabled) ink = ink.withMultipliedAlpha(0.5f);
    g.setColour(ink);
    font.drawIn(g, label, area, Justify::Centred);
  }

  void drawComboBox(Graphics& g, const Rect& area, const std::string& label, const Font& font,
                    const WidgetState& st) override {
    const Rect r = area.reduced(0.5f);
    g.setColour(st.hovered ? ui(ColourScheme::widgetBackground).brightened(0.04f)
                           : ui(ColourScheme::widgetBackground));
    g.fillRoundedRect(r, 3.0f);
    g.setColour(ui(ColourScheme::outline));
    g.drawRoundedRect(r, 3.0f, 1.0f);
    // The arrow: an open chevron in the text colour, V4's replacement for the
    // filled triangle every earlier look drew.
    const float cx = area.right() - 15.0f, cy = area.centre().y;
    Path arrow;
    arrow.moveTo(cx - 5.0f, cy - 2.5f);
    arrow.lineTo(cx, cy + 2.5f);
    arrow.lineTo(cx + 5.0f, cy - 2.5f);
    g.setColour(ui(ColourScheme::defaultText).withMultipliedAlpha(st.enabled ? 0.9f : 0.3f));
    g.strokePath(arrow, StrokeStyle{2.0f, LineJoin::Round, LineCap::Round});
    g.setColour(st.enabled ? text() : dimText());
    font.drawIn(g, label, Rect(area.x + 6.0f, area.y, area.w - 32.0f, area.h), Justify::Left);
  }

  void drawScrollBar(Graphics& g, const Rect& track, const Rect& thumb, bool vertical,
                     bool active) override {
    (void) track;
    const Rect t = vertical ? thumb.reduced(2.0f, 1.0f) : thumb.reduced(1.0f, 2.0f);
    Colour c = ui(ColourScheme::defaultFill);
    if (active) c = c.brightened(0.08f);
    g.setColour(c);
    g.fillRoundedRect(t, (std::min)(t.w, t.h) * 0.5f);
  }

  void drawTextEditorBackground(Graphics& g, const Rect& area, bool focused,
                                bool enabled) override {
    g.setColour(enabled ? ui(ColourScheme::widgetBackground)
                        : ui(ColourScheme::widgetBackground).withMultipliedAlpha(0.6f));
    g.fillRect(area);
    if (focused) {
      g.setColour(ui(ColourScheme::defaultFill));
      g.drawRect(area.reduced(1.0f), 2.0f);
    } else {
      g.setColour(ui(ColourScheme::outline));
      g.drawRect(area.reduced(0.5f), 1.0f);
    }
  }

  void drawTextEditorSelection(Graphics& g, const Rect& area) override {
    g.setColour(ui(ColourScheme::defaultFill).withMultipliedAlpha(0.45f));
    g.fillRect(area);
  }

  void drawTextEditorCaret(Graphics& g, const Rect& area) override {
    g.setColour(ui(ColourScheme::defaultText));
    g.fillRect(area);
  }

  void drawPopupBackground(Graphics& g, const Rect& area) override {
    g.setColour(ui(ColourScheme::menuBackground));
    g.fillRect(area);
    g.setColour(ui(ColourScheme::outline).withMultipliedAlpha(0.6f));
    g.drawRect(area.reduced(0.5f), 1.0f);
  }

  void drawPopupItem(Graphics& g, const Rect& row, const PopupItem& item, const Font& font,
                     bool highlighted) override {
    const Rect textArea(kPopupPadX + kPopupTickWidth, row.y,
                        row.w - kPopupPadX * 2.0f - kPopupTickWidth, row.h);
    if (item.isHeader) {
      g.setColour(ui(ColourScheme::menuText).withMultipliedAlpha(0.6f));
      font.drawIn(g, item.text, row.reduced(kPopupPadX, 0.0f), Justify::Left);
      return;
    }
    const bool lit = highlighted && item.enabled;
    if (lit) {
      g.setColour(ui(ColourScheme::highlightedFill));
      g.fillRect(row.reduced(1.0f, 0.0f));
    }
    const Colour ink = lit ? ui(ColourScheme::highlightedText)
                           : ui(ColourScheme::menuText).withMultipliedAlpha(item.enabled ? 1.0f : 0.5f);
    if (item.ticked) {
      const float cy = row.centre().y, cx = kPopupPadX + 5.0f;
      Path tick;
      tick.moveTo(cx - 4.0f, cy);
      tick.lineTo(cx - 1.0f, cy + 3.5f);
      tick.lineTo(cx + 5.0f, cy - 4.0f);
      g.setColour(ink);
      g.strokePath(tick, StrokeStyle{1.8f, LineJoin::Round, LineCap::Round});
    }
    g.setColour(ink);
    font.drawIn(g, item.text, textArea, Justify::Left);
  }

  void drawPopupSeparator(Graphics& g, const Rect& row) override {
    g.setColour(ui(ColourScheme::menuText).withMultipliedAlpha(0.3f));
    const float y = row.centre().y;
    g.drawLine(kPopupPadX, y, row.w - kPopupPadX, y, 1.0f);
  }

  void drawTab(Graphics& g, const Rect& area, const std::string& name, const Font& font,
               bool current, bool hovered) override {
    Colour fill = current ? ui(ColourScheme::widgetBackground)
                          : ui(ColourScheme::windowBackground);
    if (hovered && !current) fill = fill.brightened(fill.brightness() < 0.5f ? 0.05f : -0.05f);
    g.setColour(fill);
    g.fillRect(area);
    g.setColour(ui(ColourScheme::outline));
    g.drawRect(area.reduced(0.5f), 1.0f);
    g.setColour(current ? ui(ColourScheme::defaultText)
                        : ui(ColourScheme::defaultText).withMultipliedAlpha(0.6f));
    font.drawIn(g, name, area, Justify::Centred);
  }

  void drawListRow(Graphics& g, const Rect& row, const std::string& label, const Font& font,
                   bool selected) override {
    if (selected) {
      g.setColour(ui(ColourScheme::highlightedFill));
      g.fillRect(row);
    }
    g.setColour(selected ? ui(ColourScheme::highlightedText) : ui(ColourScheme::defaultText));
    font.drawIn(g, label, Rect(row.x + 6.0f, row.y, row.w - 12.0f, row.h), Justify::Left);
  }

  void drawProgressBar(Graphics& g, const Rect& area, double progress, const std::string& label,
                       const Font& font) override {
    const float radius = area.h * 0.5f;
    g.setColour(ui(ColourScheme::windowBackground).darkened(0.08f));
    g.fillRoundedRect(area, radius);
    const Rect inner = area.reduced(2.0f);
    if (progress >= 0.0) {
      const float w = inner.w * (float) (progress > 1.0 ? 1.0 : progress);
      if (w > 1.0f) {
        g.setColour(ui(ColourScheme::defaultFill));
        g.fillRoundedRect(Rect(inner.x, inner.y, w, inner.h), inner.h * 0.5f);
      }
    } else {
      g.setColour(ui(ColourScheme::defaultFill).withMultipliedAlpha(0.5f));
      g.fillRoundedRect(inner.reduced(inner.w * 0.25f, 0.0f), inner.h * 0.5f);
    }
    if (!label.empty()) {
      g.setColour(ui(ColourScheme::defaultText));
      font.drawIn(g, label, area, Justify::Centred);
    }
  }

  void drawTooltip(Graphics& g, const Rect& area, const std::string& message,
                   const Font& font) override {
    g.setColour(ui(ColourScheme::widgetBackground));
    g.fillRoundedRect(area, 3.0f);
    g.setColour(ui(ColourScheme::outline));
    g.drawRoundedRect(area, 3.0f, 1.0f);
    g.setColour(ui(ColourScheme::defaultText));
    font.drawIn(g, message, area, Justify::Centred);
  }

  void drawIconButtonBackground(Graphics& g, const Rect& area, const WidgetState& st,
                                bool toggled) override {
    Colour base = toggled ? ui(ColourScheme::highlightedFill) : ui(ColourScheme::widgetBackground);
    if (st.pressed) base = base.brightened(base.brightness() < 0.5f ? 0.2f : -0.2f);
    g.setColour(base);
    g.fillRoundedRect(area, 6.0f);
    g.setColour(ui(ColourScheme::outline));
    g.drawRoundedRect(area, 6.0f, 1.0f);
  }

  Colour iconButtonColour(const WidgetState& st, bool toggled) override {
    if (!st.enabled) return dimText();
    return toggled ? ui(ColourScheme::highlightedText) : ui(ColourScheme::defaultText);
  }

  void drawRangeSlider(Graphics& g, const Rect& area, float low, float high,
                       const WidgetState& st) override {
    const float thumb = (std::min)(16.0f, area.h * 0.5f);
    const float trackW = (std::min)(6.0f, area.h * 0.25f);
    const float y = area.centre().y;
    const float x0 = area.x + thumb * 0.5f, x1 = area.right() - thumb * 0.5f;
    Path track;
    track.moveTo(x0, y);
    track.lineTo(x1, y);
    g.setColour(ui(ColourScheme::widgetBackground).brightened(0.06f));
    g.strokePath(track, StrokeStyle{trackW, LineJoin::Round, LineCap::Round});
    const float a = x0 + (x1 - x0) * clamp01(low), b = x0 + (x1 - x0) * clamp01(high);
    Path span;
    span.moveTo(a, y);
    span.lineTo(b, y);
    g.setColour(ui(ColourScheme::defaultFill).darkened(0.12f));
    g.strokePath(span, StrokeStyle{trackW, LineJoin::Round, LineCap::Round});
    g.setColour(st.enabled ? ui(ColourScheme::defaultFill) : ui(ColourScheme::outline));
    for (float x : {a, b}) g.fillEllipse(Rect(x - thumb * 0.5f, y - thumb * 0.5f, thumb, thumb));
  }

private:
  ColourScheme scheme_;
};

} // namespace gfx
} // namespace sonore
