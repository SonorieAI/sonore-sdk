// SPDX-License-Identifier: Apache-2.0
//
// LookAndFeel: where a widget's appearance lives, separately from what it does.
//
// ── Why the split is worth a virtual call ───────────────────────────────────
//
// A Slider knows how a drag becomes a value. It should not know what a knob
// looks like, because those two change for completely different reasons: the
// drag behaviour is decided once and is the same in every plugin ever made,
// while the appearance is the thing a generated plugin most wants to be its
// own.
//
// Keeping them apart means a plugin can supply a LookAndFeel and get its own
// face without touching -- or being able to break -- the interaction. Which
// matters here more than in a hand-written framework: the appearance is the
// part a model writes, and the part most likely to be wrong.
//
// ── The defaults are meant to be shipped, not replaced ──────────────────────
//
// A generated plugin whose author says nothing about appearance gets this,
// and it has to look deliberate. A default that looked like a placeholder
// would mean every plugin needed a designer before it could be released.
#pragma once

#include <string>

#include "font.h"
#include "gradient.h"
#include "graphics.h"
#include "../thumbnail.h"
#include "popup_item.h"

namespace sonore {
namespace gfx {

/** Every state a widget can be in when it is drawn. Passed as one struct so
 *  adding a state later does not change six signatures. */
struct WidgetState {
  bool hovered = false;
  bool pressed = false;
  bool toggled = false;
  bool enabled = true;
};

class LookAndFeel {
public:
  virtual ~LookAndFeel() = default;

  // ── The palette a plugin overrides first ──────────────────────────────────
  virtual Colour background() const { return palette::background(); }
  virtual Colour panel() const { return palette::panel(); }
  virtual Colour outline() const { return palette::outline(); }
  virtual Colour text() const { return palette::text(); }
  virtual Colour dimText() const { return palette::dimText(); }
  virtual Colour accent() const { return palette::accent(); }

  /**
   * The ring around whatever has the keyboard.
   *
   * Drawn by the WIDGET rather than by drawSlider or drawButton, so a
   * LookAndFeel that replaces a control's appearance does not silently lose
   * its focus indicator -- which is the failure mode that makes keyboard
   * support present and useless. Override this to restyle the ring; override
   * drawLinearSlider and you still get one.
   *
   * Outside the control's bounds by a pixel, not inside: a ring drawn within a
   * slider's own rectangle sits on top of the track and reads as part of the
   * value.
   */
  virtual void drawFocusRing(Graphics& g, const Rect& area) const {
    g.setColour(accent());
    g.drawRoundedRect(Rect(area.x + 0.5f, area.y + 0.5f, area.w - 1.0f, area.h - 1.0f), 3.0f,
                      1.0f);
  }

  /** The arc a rotary control sweeps: seven o'clock to five o'clock. Ten
   *  o'clock to two would be a knob nobody can set precisely, and a full
   *  circle has no visible start. */
  virtual float rotaryStart() const { return -2.356f; }
  virtual float rotaryEnd() const { return 2.356f; }

  virtual void drawRotarySlider(Graphics& g, const Rect& area, float normalised,
                                const WidgetState& st) {
    const Rect dial = area.withAspect(1.0f).reduced(2.0f);
    if (dial.isEmpty()) return;
    const float from = rotaryStart(), to = rotaryEnd();
    const float at = from + (to - from) * clamp01(normalised);
    // The arc is inset by its own half-width, or a thick stroke spills past
    // the bounds a caller gave and overlaps the control next to it.
    const float thickness = std::max(3.0f, dial.w * 0.09f);
    const Rect ring = dial.reduced(thickness * 0.75f);

    Path track;
    track.addArc(ring, from, to);
    g.setColour(st.enabled ? outline() : outline().withMultipliedAlpha(0.5f));
    g.strokePath(track, StrokeStyle{thickness, LineJoin::Round, LineCap::Round});

    if (normalised > 0.0f) {
      Path filled;
      filled.addArc(ring, from, at);
      Colour c = accent();
      if (!st.enabled) c = c.withMultipliedAlpha(0.4f);
      else if (st.pressed) c = c.brightened(0.15f);
      else if (st.hovered) c = c.brightened(0.07f);
      g.setColour(c);
      g.strokePath(filled, StrokeStyle{thickness, LineJoin::Round, LineCap::Round});
    }

    // The body under the ring, domed. A rotary control is the one thing in a
    // plugin people look at closely, and a flat disc under a coloured arc is
    // the difference between "drawn" and "designed". Off-centre because light
    // comes from above and to the left, which is where every physical knob
    // anyone has ever photographed is lit from.
    const Rect body = ring.reduced(thickness * 0.6f);
    if (!body.isEmpty()) {
      const Point bc = body.centre();
      g.setGradient(ColourGradient::radial(panel().brightened(st.enabled ? 0.10f : 0.04f),
                                           Point(bc.x - body.w * 0.22f, bc.y - body.h * 0.28f),
                                           body.w * 0.95f, panel().darkened(0.12f)));
      g.fillEllipse(body);
    }

    const Point c = ring.centre();
    const float r = ring.w * 0.5f - thickness * 0.5f;
    Path pointer;
    pointer.moveTo(c.x, c.y);
    pointer.lineTo(c.x + std::sin(at) * r, c.y - std::cos(at) * r);
    g.setColour(st.enabled ? text() : dimText());
    g.strokePath(pointer, StrokeStyle{std::max(1.5f, thickness * 0.4f), LineJoin::Round,
                                      LineCap::Round});
  }

  /**
   * A slider drawn as a FILLED PROPORTION rather than a thumb on a track.
   *
   * A linear bar. It is the shape a level or a mix control takes in
   * most modern plugins, and it reads at a glance across a column of them in a
   * way a thumb does not -- the eye compares fill heights without having to
   * find each thumb first.
   *
   * No thumb means no thumb radius to shorten the track by, so the fill really
   * does span the whole rectangle: at 0 it is empty and at 1 it is full, which
   * is the property that makes a row of them comparable.
   */
  virtual void drawLinearBar(Graphics& g, const Rect& area, float normalised, bool vertical,
                             const WidgetState& st) {
    const float v = clamp01(normalised);
    g.setColour(background());
    g.fillRoundedRect(area, 3.0f);

    Rect fill = area;
    if (vertical) {
      fill.h = area.h * v;
      fill.y = area.y + area.h - fill.h;
    } else {
      fill.w = area.w * v;
    }
    if (!fill.isEmpty()) {
      g.setColour(st.enabled ? accent() : dimText());
      g.fillRoundedRect(fill, 3.0f);
    }
    g.setColour(outline());
    g.drawRoundedRect(area, 3.0f, 1.0f);
  }

  virtual void drawLinearSlider(Graphics& g, const Rect& area, float normalised, bool vertical,
                                const WidgetState& st) {
    const float v = clamp01(normalised);
    const float thickness = 6.0f;
    const float knob = 14.0f;
    // The track is shortened by the thumb's radius at each end, so the thumb
    // centre reaches exactly 0 and 1 without half of it hanging off.
    const Rect track = vertical
                           ? Rect(area.centre().x - thickness * 0.5f, area.y + knob * 0.5f,
                                  thickness, area.h - knob)
                           : Rect(area.x + knob * 0.5f, area.centre().y - thickness * 0.5f,
                                  area.w - knob, thickness);
    if (track.isEmpty()) return;

    g.setColour(st.enabled ? outline() : outline().withMultipliedAlpha(0.5f));
    g.fillRoundedRect(track, thickness * 0.5f);

    Colour fill = accent();
    if (!st.enabled) fill = fill.withMultipliedAlpha(0.4f);
    else if (st.pressed) fill = fill.brightened(0.15f);
    else if (st.hovered) fill = fill.brightened(0.07f);
    g.setColour(fill);
    if (vertical) {
      // Upward: a vertical fader at 1.0 is full at the TOP, and filling
      // downward from the top is the mistake that makes one read backwards.
      const float h = track.h * v;
      g.fillRoundedRect(Rect(track.x, track.bottom() - h, track.w, h), thickness * 0.5f);
    } else {
      g.fillRoundedRect(Rect(track.x, track.y, track.w * v, track.h), thickness * 0.5f);
    }

    const Point centre = vertical ? Point{track.centre().x, track.bottom() - track.h * v}
                                  : Point{track.x + track.w * v, track.centre().y};
    g.setColour(st.enabled ? text() : dimText());
    g.fillEllipse(Rect(centre.x - knob * 0.5f, centre.y - knob * 0.5f, knob, knob));
  }

  virtual void drawButton(Graphics& g, const Rect& area, const std::string& label, const Font& font,
                          const WidgetState& st) {
    Colour body = st.toggled ? accent() : panel();
    if (!st.enabled) body = body.withMultipliedAlpha(0.4f);
    else if (st.pressed) body = body.brightened(0.12f);
    else if (st.hovered) body = body.brightened(0.06f);
    // Lit from above, faintly. Flat fills are what makes a generated interface
    // look generated, and the whole difference here is six per cent -- enough
    // for a surface to read as a surface, not enough to become a style a
    // plugin has to fight.
    //
    // Pressed inverts it, because a control that lights the same way whether
    // it is being held or not gives no feedback at all on a machine where the
    // hover colour is too subtle to see.
    g.setGradient(st.pressed ? ColourGradient::vertical(body.darkened(0.06f), area.y,
                                                        body.brightened(0.06f), area.bottom())
                             : ColourGradient::vertical(body.brightened(0.06f), area.y,
                                                        body.darkened(0.06f), area.bottom()));
    g.fillRoundedRect(area, 4.0f);
    g.setColour(st.enabled ? outline() : outline().withMultipliedAlpha(0.5f));
    g.drawRoundedRect(area, 4.0f, 1.0f);

    if (label.empty()) return;
    // Text colour chosen from the body's BRIGHTNESS rather than from whether
    // it is toggled: a plugin with a pale accent colour would otherwise get
    // white text on near-white.
    Colour ink = st.toggled ? body.contrasting() : text();
    if (!st.enabled) ink = ink.withMultipliedAlpha(0.5f);
    g.setColour(ink);
    font.drawIn(g, label, area, Justify::Centred);
  }

  // ── Popup menus ──────────────────────────────────────────────────────────

  /** Shared with PopupMenu, which lays out to the same numbers. Here rather
   *  than in popup.h because a LookAndFeel subclass that redraws items needs
   *  them and must not have to reach into the menu to get them. */
  static constexpr float kPopupPadX = 10.0f;
  static constexpr float kPopupTickWidth = 20.0f;

  /** The panel behind the whole menu, edge included. A popup covers something
   *  else, so unlike every other surface here it has to look detached from
   *  what is behind it rather than part of it. */
  virtual void drawPopupBackground(Graphics& g, const Rect& area) {
    g.setColour(panel().darkened(0.03f));
    g.fillRoundedRect(area, 5.0f);
    // A brighter edge than a button gets, for the same reason.
    g.setColour(outline().brightened(0.10f));
    g.drawRoundedRect(area, 5.0f, 1.0f);
  }

  /** One row, spanning the menu's full width, in the menu's coordinates. */
  virtual void drawPopupItem(Graphics& g, const Rect& row, const PopupItem& item, const Font& font,
                             bool highlighted) {
    if (item.isHeader) {
      g.setColour(dimText());
      font.drawIn(g, item.text, row.reduced(kPopupPadX, 0.0f), Justify::Left);
      return;
    }

    if (highlighted && item.enabled) {
      // Inset by a pixel so the highlight does not paint over the menu's own
      // rounded edge and square off its corners.
      g.setColour(accent().withMultipliedAlpha(0.28f));
      g.fillRoundedRect(Rect(2.0f, row.y, row.w - 4.0f, row.h), 3.0f);
    }

    if (item.ticked) {
      // A check, drawn rather than written: a glyph would need a font that has
      // one, and the SDK ships no font at all.
      const float cy = row.centre().y;
      const float cx = kPopupPadX + 5.0f;
      Path tick;
      tick.moveTo(cx - 4.0f, cy);
      tick.lineTo(cx - 1.0f, cy + 3.5f);
      tick.lineTo(cx + 5.0f, cy - 4.0f);
      g.setColour(item.enabled ? accent() : dimText());
      g.strokePath(tick, StrokeStyle{1.8f, LineJoin::Round, LineCap::Round});
    }

    g.setColour(item.enabled ? text() : dimText());
    font.drawIn(g, item.text, Rect(kPopupPadX + kPopupTickWidth, row.y,
                                   row.w - kPopupPadX * 2.0f - kPopupTickWidth, row.h),
                Justify::Left);
  }

  virtual void drawPopupSeparator(Graphics& g, const Rect& row) {
    const float y = row.centre().y;
    g.setColour(outline());
    g.drawLine(kPopupPadX, y, row.w - kPopupPadX, y, 1.0f);
  }

  /**
   * A scroll bar: the track it runs in, and the thumb.
   *
   * The thumb brightens on hover and while dragged, which is the only feedback
   * a bar can give -- it has no label and no value to show.
   */
  virtual void drawScrollBar(Graphics& g, const Rect& track, const Rect& thumb, bool vertical,
                             bool active) {
    (void) vertical;
    g.setColour(background().darkened(0.02f));
    g.fillRect(track);
    Colour c = outline().brightened(active ? 0.16f : 0.06f);
    g.setColour(c);
    // Inset by a pixel on the long sides so the thumb reads as sitting IN the
    // track rather than covering it.
    const float radius = (vertical ? thumb.w : thumb.h) * 0.5f;
    g.fillRoundedRect(vertical ? thumb.reduced(1.0f, 2.0f) : thumb.reduced(2.0f, 1.0f), radius);
  }

  /**
   * One key of an on-screen keyboard.
   *
   * `held` is what the DSP is sounding, which is not the same as what the mouse
   * is on -- a note arriving from the host lights a key nobody touched, and
   * that is the point of showing it.
   */
  virtual void drawMidiKeyboardKey(Graphics& g, const Rect& key, int note, bool isBlack, bool held,
                                   bool hovered) {
    if (key.isEmpty()) return;
    if (isBlack) {
      Colour body = held ? accent().darkened(0.20f) : Colour::fromRGB(0x1A2028);
      if (!held && hovered) body = body.brightened(0.10f);
      g.setColour(body);
      g.fillRoundedRect(key, 2.0f);
      g.setColour(Colour::fromRGB(0x05070A));
      g.drawRoundedRect(key, 2.0f, 1.0f);
      return;
    }

    Colour body = held ? accent().brightened(0.10f) : Colour::fromRGB(0xE8EDF3);
    if (!held && hovered) body = body.darkened(0.05f);
    // A faint vertical ramp, so a row of white keys does not read as one flat
    // bar with lines on it.
    g.setGradient(ColourGradient::vertical(body, key.y, body.darkened(0.07f), key.bottom()));
    g.fillRect(key);
    g.setColour(Colour::fromRGB(0x333B45));
    g.drawRect(key, 1.0f);

    // C is marked, because a keyboard with no landmark is one nobody can find
    // middle C on.
    if (note % 12 == 0) {
      g.setColour(Colour::fromRGB(0x8A94A0));
      const float w = key.w * 0.34f;
      g.fillRect(Rect(key.centre().x - w * 0.5f, key.bottom() - 5.0f, w, 2.0f));
    }
  }

  /** A tooltip. Deliberately unlike every other surface here: it floats over
   *  the editor rather than being part of it, so it is lighter and has a harder
   *  edge than a panel would. */
  virtual void drawTooltip(Graphics& g, const Rect& area, const std::string& message,
                           const Font& font) {
    // The parameter is `message`, not `text`, because text() is the palette
    // accessor on this very class and a parameter of that name shadows it --
    // which compiles, calls nothing, and leaves the tooltip drawn in whatever
    // colour was last set.
    g.setColour(panel().brightened(0.10f));
    g.fillRoundedRect(area, 3.0f);
    g.setColour(outline().brightened(0.20f));
    g.drawRoundedRect(area, 3.0f, 1.0f);
    g.setColour(text());
    font.drawIn(g, message, area, Justify::Centred);
  }

  /** The grip. Diagonal lines, which is what every resize corner on every
   *  desktop is, because it is the one shape that reads as "drag me" without a
   *  label. */
  virtual void drawResizableCorner(Graphics& g, const Rect& area, bool active) {
    g.setColour(outline().brightened(active ? 0.30f : 0.12f));
    for (int i = 1; i <= 3; ++i) {
      const float inset = (float) i * 4.0f;
      g.drawLine(area.right() - inset, area.bottom(), area.right(), area.bottom() - inset, 1.5f);
    }
  }

  /**
   * A two-ended slider.
   *
   * The SELECTED span is what gets the accent, not the whole track: the point
   * of the control is which part of the range is in, and colouring the outside
   * would say the opposite.
   */
  virtual void drawRangeSlider(Graphics& g, const Rect& area, float low, float high,
                               const WidgetState& st) {
    const float thumb = 12.0f;
    const float track = 6.0f;
    const Rect line(area.x + thumb * 0.5f, area.centre().y - track * 0.5f, area.w - thumb, track);
    if (line.isEmpty()) return;

    g.setColour(st.enabled ? outline() : outline().withMultipliedAlpha(0.5f));
    g.fillRoundedRect(line, track * 0.5f);

    const float lowX = line.x + low * line.w;
    const float highX = line.x + high * line.w;
    Colour fill = accent();
    if (!st.enabled) fill = fill.withMultipliedAlpha(0.4f);
    else if (st.pressed) fill = fill.brightened(0.15f);
    else if (st.hovered) fill = fill.brightened(0.07f);
    g.setColour(fill);
    g.fillRoundedRect(Rect(lowX, line.y, highX - lowX, line.h), track * 0.5f);

    for (float x : {lowX, highX}) {
      g.setColour(st.enabled ? text() : dimText());
      g.fillEllipse(Rect(x - thumb * 0.5f, area.centre().y - thumb * 0.5f, thumb, thumb));
    }
  }

  /** The plate under an icon. The icon itself is drawn by the button, because
   *  it came from an SVG and this class knows nothing about that. */
  virtual void drawIconButtonBackground(Graphics& g, const Rect& area, const WidgetState& st,
                                        bool toggled) {
    Colour body = toggled ? accent().withMultipliedAlpha(0.30f) : panel();
    if (!st.enabled) body = body.withMultipliedAlpha(0.4f);
    else if (st.pressed) body = body.brightened(0.12f);
    else if (st.hovered) body = body.brightened(0.06f);
    g.setColour(body);
    g.fillRoundedRect(area, 4.0f);
    g.setColour(toggled ? accent() : outline());
    g.drawRoundedRect(area, 4.0f, 1.0f);
  }

  // ── Lists, tabs and progress ─────────────────────────────────────────────

  /** One row of a list. Selection is drawn as a filled band rather than as an
   *  outline: a list is scanned rather than read, and a band is visible at a
   *  glance where an outline has to be looked for. */
  virtual void drawListRow(Graphics& g, const Rect& row, const std::string& label,
                           const Font& font, bool selected) {
    if (selected) {
      g.setColour(accent().withMultipliedAlpha(0.30f));
      g.fillRect(row);
    }
    g.setColour(selected ? text() : text().withMultipliedAlpha(0.85f));
    font.drawIn(g, label, Rect(row.x + 8.0f, row.y, row.w - 16.0f, row.h), Justify::Left);
  }

  /**
   * One tab.
   *
   * The current one is marked with a bar along its BOTTOM edge, against the
   * page it belongs to. A tab that only changed colour is one people have to
   * compare against its neighbours to read.
   */
  virtual void drawTab(Graphics& g, const Rect& area, const std::string& name, const Font& font,
                       bool current, bool hovered) {
    if (current) {
      g.setColour(panel());
      g.fillRect(area);
    } else if (hovered) {
      g.setColour(panel().withMultipliedAlpha(0.5f));
      g.fillRect(area);
    }
    g.setColour(current ? text() : dimText());
    font.drawIn(g, name, area, Justify::Centred);
    if (current) {
      g.setColour(accent());
      g.fillRect(Rect(area.x, area.bottom() - 2.0f, area.w, 2.0f));
    }
  }

  /**
   * A progress bar. A negative value means INDETERMINATE.
   *
   * Indeterminate is drawn as a half-filled bar with no motion rather than as
   * an animated sweep: an animation would need a repaint several times a second
   * for as long as it is up, and that is a timer running while somebody
   * records.
   */
  virtual void drawProgressBar(Graphics& g, const Rect& area, double progress,
                               const std::string& label, const Font& font) {
    g.setColour(background().darkened(0.03f));
    g.fillRoundedRect(area, area.h * 0.5f);

    if (progress >= 0.0) {
      const float w = area.w * (float) progress;
      if (w > 1.0f) {
        g.setColour(accent());
        g.fillRoundedRect(Rect(area.x, area.y, w, area.h), area.h * 0.5f);
      }
    } else {
      g.setColour(accent().withMultipliedAlpha(0.45f));
      g.fillRoundedRect(area.reduced(area.w * 0.25f, 0.0f), area.h * 0.5f);
    }

    g.setColour(outline());
    g.drawRoundedRect(area, area.h * 0.5f, 1.0f);
    if (!label.empty()) {
      // Contrasting against the BAR rather than the track, because the label
      // sits over the filled part for most of its life.
      g.setColour(accent().contrasting());
      font.drawIn(g, label, area, Justify::Centred);
    }
  }

  // ── Audio displays ───────────────────────────────────────────────────────

  /**
   * A scrolling scope, drawn from the column DATA rather than from the widget.
   *
   * Taking `const AudioVisualiser&` would need a forward declaration, and a
   * declared-but-undefined virtual in a header-only SDK is a link error about
   * a vtable in whichever translation unit happens not to include the widget --
   * the same trap popup_item.h exists to avoid. Two arrays and a count need no
   * declaration at all, and they are a simpler thing to override besides.
   *
   * `lows` and `highs` run OLDEST FIRST, so index 0 is drawn at the left and
   * the newest column is at the right. That is the direction every scope and
   * every DAW timeline runs; the other way reads as running backwards and is
   * the kind of thing nobody reports because they assume they are holding it
   * wrong.
   */
  virtual void drawAudioVisualiser(Graphics& g, const Rect& area, const float* lows,
                                   const float* highs, int count) {
    g.setColour(background().darkened(0.03f));
    g.fillRoundedRect(area, 3.0f);
    const float mid = area.centre().y;

    g.setColour(outline());
    g.drawLine(area.x, mid, area.right(), mid, 1.0f);
    if (!lows || !highs || count <= 0 || area.w <= 0.0f) return;

    const float half = area.h * 0.5f - 1.0f;
    const float step = area.w / (float) count;
    g.setColour(accent());
    for (int i = 0; i < count; ++i) {
      const float top = mid - clampUnit(highs[i]) * half;
      const float bottom = mid - clampUnit(lows[i]) * half;
      const float h = bottom - top;
      // At least a pixel: silence between two bursts must still draw the
      // centre line's worth of ink, or the trace looks like it has gaps in it.
      g.fillRect(Rect(area.x + (float) i * step, top, step < 1.0f ? 1.0f : step,
                      h < 1.0f ? 1.0f : h));
    }
  }

  /**
   * A waveform, with an optional playhead and selection.
   *
   * A null or empty thumbnail draws the panel and a centre line -- an empty
   * BOX would look like a broken control, where an empty waveform looks like
   * silence, which is what it is.
   */
  virtual void drawWaveform(Graphics& g, const Rect& area, const AudioThumbnail* thumb,
                            float playhead, float selectionFrom, float selectionTo) {
    g.setColour(background().darkened(0.03f));
    g.fillRoundedRect(area, 3.0f);

    if (selectionTo > selectionFrom) {
      const float x0 = area.x + selectionFrom * area.w;
      const float x1 = area.x + selectionTo * area.w;
      g.setColour(accent().withMultipliedAlpha(0.18f));
      g.fillRect(Rect(x0, area.y, x1 - x0, area.h));
    }

    const float mid = area.centre().y;
    if (!thumb || thumb->empty() || thumb->numBuckets() == 0) {
      g.setColour(outline());
      g.drawLine(area.x, mid, area.right(), mid, 1.0f);
      return;
    }

    // Normalised to the loudest sample, so a quiet recording still fills the
    // box. Drawn at absolute scale, anything recorded conservatively -- which
    // is most things -- is a flat line.
    const float peak = thumb->peak();
    const float scale = (peak > 1e-6f ? 1.0f / peak : 1.0f) * (area.h * 0.5f - 2.0f);

    g.setColour(accent().withMultipliedAlpha(0.85f));
    const uint32_t buckets = thumb->numBuckets();
    for (int px = 0; px < (int) area.w; ++px) {
      // One bucket per PIXEL, chosen by position rather than by stepping: the
      // bucket count and the width are unrelated, and stepping would run off
      // the end or stop short.
      const uint32_t b = (uint32_t) ((float) px / area.w * (float) buckets);
      const ThumbnailBucket bucket = thumb->at(0, b < buckets ? b : buckets - 1);
      const float top = mid - bucket.high * scale;
      const float bottom = mid - bucket.low * scale;
      const float h = bottom - top;
      g.fillRect(Rect(area.x + (float) px, top, 1.0f, h < 1.0f ? 1.0f : h));
    }

    if (playhead >= 0.0f && playhead <= 1.0f) {
      g.setColour(text());
      g.fillRect(Rect(area.x + playhead * area.w, area.y, 1.0f, area.h));
    }
  }

  // ── Text fields ──────────────────────────────────────────────────────────

  /** The well a field's text sits in. FOCUS is what this has to make obvious:
   *  a user typing needs to see which field is receiving it without hunting
   *  for a caret one pixel wide. */
  virtual void drawTextEditorBackground(Graphics& g, const Rect& area, bool focused,
                                        bool enabled) {
    g.setColour(enabled ? background().darkened(0.03f) : background());
    g.fillRoundedRect(area, 4.0f);
    g.setColour(focused ? accent() : outline());
    g.drawRoundedRect(area, 4.0f, focused ? 1.6f : 1.0f);
  }

  virtual void drawTextEditorSelection(Graphics& g, const Rect& area) {
    g.setColour(accent().withMultipliedAlpha(0.35f));
    g.fillRect(area);
  }

  virtual void drawTextEditorCaret(Graphics& g, const Rect& area) {
    // Not blinking. A blink needs a repaint two or three times a second for as
    // long as a field has focus, and in a plugin that is a timer running while
    // somebody records. A steady caret says the same thing for nothing.
    g.setColour(accent());
    g.fillRect(area);
  }

  virtual void drawLabel(Graphics& g, const Rect& area, const std::string& label, const Font& font,
                         Justify justify, const WidgetState& st) {
    if (label.empty()) return;
    g.setColour(st.enabled ? text() : dimText());
    font.drawIn(g, label, area, justify);
  }

  virtual void drawComboBox(Graphics& g, const Rect& area, const std::string& label,
                            const Font& font, const WidgetState& st) {
    Colour body = panel();
    if (st.pressed) body = body.brightened(0.12f);
    else if (st.hovered) body = body.brightened(0.06f);
    // The same lighting as a button, because a combo box IS a button with a
    // value in it, and two controls next to each other lit differently is the
    // thing people notice without being able to say why.
    g.setGradient(ColourGradient::vertical(body.brightened(0.06f), area.y, body.darkened(0.06f),
                                           area.bottom()));
    g.fillRoundedRect(area, 4.0f);
    g.setColour(outline());
    g.drawRoundedRect(area, 4.0f, 1.0f);

    const Rect textArea(area.x + 8.0f, area.y, area.w - 28.0f, area.h);
    g.setColour(st.enabled ? text() : dimText());
    font.drawIn(g, label, textArea, Justify::Left);

    // The chevron. Drawn rather than typed, so it does not depend on the font
    // having a glyph for it -- which a font subset very well may not.
    const float cx = area.right() - 14.0f, cy = area.centre().y;
    Path chevron;
    chevron.moveTo(cx - 4.0f, cy - 2.0f);
    chevron.lineTo(cx, cy + 2.5f);
    chevron.lineTo(cx + 4.0f, cy - 2.0f);
    g.setColour(dimText());
    g.strokePath(chevron, StrokeStyle{1.6f, LineJoin::Round, LineCap::Round});
  }

  /** The one instance a component uses when nobody gave it another. */
  static LookAndFeel& defaultLookAndFeel() {
    static LookAndFeel instance;
    return instance;
  }

protected:
  static float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
  /** -1..1, for a signal. A sample past full scale would otherwise draw past
   *  the top of the box and over whatever is above it. */
  static float clampUnit(float v) { return v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v); }
};

} // namespace gfx
} // namespace sonore
