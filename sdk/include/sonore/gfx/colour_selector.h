// SPDX-License-Identifier: Apache-2.0
//
// ColourSelector: choosing a colour.
//
// ── The layout is the reference's ───────────────────────────────────────────
//
// The colour itself across the top with its hex code beside it, a
// saturation/brightness square with a hue strip next to it, a slider each for
// red, green, blue and alpha, and a row of swatches. Flags turn the parts on
// and off, so a picker hanging off a theme swatch in a CallOutBox can be just
// the square and the strip.
//
// ── Hue is kept, not recomputed ─────────────────────────────────────────────
//
// The selector holds hue, saturation and brightness as numbers of its own and
// derives the colour from them, rather than holding the colour and deriving
// HSV every time it is asked. The difference shows at the edges: drag the
// brightness to zero and every hue is the same black, so a picker that
// recomputed hue from the colour would snap the strip to red there, and
// dragging back up would come out red instead of the blue the user started
// with. Grey loses hue the same way at zero saturation. A colour that arrives
// from OUTSIDE (setCurrentColour, a slider, a swatch, typed hex) recomputes
// what it can and keeps the rest.
//
// ── The square is two gradients ─────────────────────────────────────────────
//
// White to the pure hue across, then transparent to black down, composited.
// That is HSV exactly -- the vertical layer multiplies by brightness, the
// horizontal one mixes towards white by saturation -- so the square needs no
// per-pixel loop and no cached image, and a test can read a pixel back and
// compare it with the conversion the selector itself uses.
//
// The hue strip is one gradient with a stop every sixty degrees, which is
// also exact: between primaries and secondaries the hue ramp is linear in RGB.
#pragma once

#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "popup_overlay.h"
#include "text_editor.h"
#include "widgets.h"

namespace sonore {
namespace gfx {

namespace detail {

inline float selectorClamp01(float v) { return v > 0.0f ? (v < 1.0f ? v : 1.0f) : 0.0f; }

inline uint8_t selectorToByte(float v) { return (uint8_t) (selectorClamp01(v) * 255.0f + 0.5f); }

/** HSV to RGB, every argument 0..1 (hue wraps). */
inline Colour selectorHsvToColour(float h, float s, float v, uint8_t alpha = 255) {
  if (!(h == h)) h = 0.0f;
  h -= std::floor(h);
  s = selectorClamp01(s);
  v = selectorClamp01(v);
  const float sector = h * 6.0f;
  const int i = (int) sector % 6;
  const float f = sector - std::floor(sector);
  const float p = v * (1.0f - s), q = v * (1.0f - s * f), t = v * (1.0f - s * (1.0f - f));
  float r = v, g = t, b = p;
  switch (i) {
    case 0: r = v; g = t; b = p; break;
    case 1: r = q; g = v; b = p; break;
    case 2: r = p; g = v; b = t; break;
    case 3: r = p; g = q; b = v; break;
    case 4: r = t; g = p; b = v; break;
    default: r = v; g = p; b = q; break;
  }
  return Colour(selectorToByte(r), selectorToByte(g), selectorToByte(b), alpha);
}

/** RGB to HSV. Hue is left alone when the colour has none (grey, black), and
 *  saturation when it has no brightness -- see the header for why. */
inline void selectorColourToHsv(Colour c, float& h, float& s, float& v) {
  const float r = (float) c.r / 255.0f, g = (float) c.g / 255.0f, b = (float) c.b / 255.0f;
  const float hi = (std::max)(r, (std::max)(g, b));
  const float lo = (std::min)(r, (std::min)(g, b));
  const float delta = hi - lo;
  v = hi;
  if (hi <= 0.0f) return; // black: hue and saturation are whatever they were
  s = delta / hi;
  if (delta <= 0.0f) return; // grey: no hue to read
  float hue;
  if (hi == r) hue = (g - b) / delta;
  else if (hi == g) hue = 2.0f + (b - r) / delta;
  else hue = 4.0f + (r - g) / delta;
  hue /= 6.0f;
  if (hue < 0.0f) hue += 1.0f;
  h = hue;
}

/** "3366CC" or, with alpha, "803366CC" (AARRGGBB, the order Colour::toARGB
 *  uses). Upper case, no '#'. */
inline std::string selectorHexString(Colour c, bool withAlpha) {
  char buffer[16];
  if (withAlpha) std::snprintf(buffer, sizeof(buffer), "%08X", (unsigned) c.toARGB());
  else std::snprintf(buffer, sizeof(buffer), "%06X", (unsigned) (c.toARGB() & 0xFFFFFFu));
  return buffer;
}

/**
 * Typed hex back to a colour. Accepts an optional '#' or "0x", and three
 * digits (RGB, each doubled), six (RRGGBB, opaque) or eight (AARRGGBB).
 * False for anything else, which leaves `out` alone -- a field that half-
 * applied "12G" would set a colour nobody typed.
 */
inline bool selectorParseHex(const std::string& text, Colour& out) {
  std::string digits;
  for (char ch : text)
    if (ch != ' ' && ch != '\t') digits += ch;
  if (!digits.empty() && digits[0] == '#') digits.erase(0, 1);
  else if (digits.size() > 2 && digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X'))
    digits.erase(0, 2);
  if (digits.size() != 3 && digits.size() != 6 && digits.size() != 8) return false;
  uint32_t value = 0;
  for (char ch : digits) {
    uint32_t d;
    if (ch >= '0' && ch <= '9') d = (uint32_t) (ch - '0');
    else if (ch >= 'a' && ch <= 'f') d = (uint32_t) (ch - 'a' + 10);
    else if (ch >= 'A' && ch <= 'F') d = (uint32_t) (ch - 'A' + 10);
    else return false;
    value = (value << 4) | d;
  }
  if (digits.size() == 3) {
    const uint32_t r = (value >> 8) & 0xF, g = (value >> 4) & 0xF, b = value & 0xF;
    out = Colour((uint8_t) (r * 17), (uint8_t) (g * 17), (uint8_t) (b * 17));
  } else if (digits.size() == 6) {
    out = Colour::fromRGB(value);
  } else {
    out = Colour::fromARGB(value);
  }
  return true;
}

/** The saturation/brightness square. */
class ColourSpaceArea : public Widget {
public:
  ColourSpaceArea() {
    setWantsKeyboardFocus(true);
    setAccessibleName("Saturation and brightness");
  }

  std::function<void(float s, float v)> onPick;
  float hue = 0.0f, saturation = 0.0f, value = 1.0f;

  /** The point a saturation and brightness are drawn at. */
  Point markerFor(float s, float v) const {
    const Rect a = localBounds();
    return {a.x + selectorClamp01(s) * a.w, a.y + (1.0f - selectorClamp01(v)) * a.h};
  }

  void paint(Graphics& g) override {
    const Rect a = localBounds();
    if (a.isEmpty()) return;
    g.setGradient(ColourGradient::horizontal(Colour(255, 255, 255), a.x,
                                             selectorHsvToColour(hue, 1.0f, 1.0f), a.right()));
    g.fillRect(a);
    g.setGradient(ColourGradient::vertical(Colour(0, 0, 0, 0), a.y, Colour(0, 0, 0, 255), a.bottom()));
    g.fillRect(a);
    lookAndFeel().drawColourSpaceMarker(g, markerFor(saturation, value), hasKeyboardFocus());
  }

  void mouseDown(const MouseEvent& e) override { pickAt(e.position); }
  void mouseDrag(const MouseEvent& e) override { pickAt(e.position); }

  /** Left and Right move saturation, Up and Down brightness, by 1% -- or 10%
   *  with Shift, since the square is too coarse a thing for a finer step. */
  bool keyPressed(const KeyPress& key) override {
    if (!isEnabled()) return false;
    const float step = key.shiftDown ? 0.10f : 0.01f;
    float s = saturation, v = value;
    if (key.is(KeyPress::Left)) s -= step;
    else if (key.is(KeyPress::Right)) s += step;
    else if (key.is(KeyPress::Up)) v += step;
    else if (key.is(KeyPress::Down)) v -= step;
    else if (key.is(KeyPress::Home)) s = 0.0f;
    else if (key.is(KeyPress::End)) s = 1.0f;
    else return false;
    if (onPick) onPick(selectorClamp01(s), selectorClamp01(v));
    return true;
  }

  AccessibleInfo accessibleInfo() const override {
    AccessibleInfo info = baseInfo(AccessibleRole::Group);
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "saturation %.0f%%, brightness %.0f%%",
                  (double) saturation * 100.0, (double) value * 100.0);
    info.value = buffer;
    return info;
  }

private:
  void pickAt(Point p) {
    if (!isEnabled()) return;
    const Rect a = localBounds();
    if (a.w <= 0.0f || a.h <= 0.0f) return;
    if (onPick) onPick(selectorClamp01((p.x - a.x) / a.w), selectorClamp01(1.0f - (p.y - a.y) / a.h));
  }
};

/** The hue strip, red at the top and round again to red at the bottom. */
class HueStrip : public Widget {
public:
  HueStrip() {
    setWantsKeyboardFocus(true);
    setAccessibleName("Hue");
  }

  std::function<void(float hue)> onPick;
  float hue = 0.0f;

  float yFor(float h) const {
    const Rect a = localBounds();
    return a.y + selectorClamp01(h) * a.h;
  }

  void paint(Graphics& g) override {
    const Rect a = localBounds();
    if (a.isEmpty()) return;
    ColourGradient ramp = ColourGradient::vertical(Colour(255, 0, 0), a.y, Colour(255, 0, 0), a.bottom());
    for (int i = 1; i < 6; ++i)
      ramp.addStop((float) i / 6.0f, selectorHsvToColour((float) i / 6.0f, 1.0f, 1.0f));
    g.setGradient(ramp);
    g.fillRect(a.reduced(3.0f, 0.0f));
    lookAndFeel().drawHueStripThumb(g, a.reduced(3.0f, 0.0f), yFor(hue), hasKeyboardFocus());
  }

  void mouseDown(const MouseEvent& e) override { pickAt(e.position.y); }
  void mouseDrag(const MouseEvent& e) override { pickAt(e.position.y); }

  /** A degree a press, ten with Shift. Up is towards red at the top. */
  bool keyPressed(const KeyPress& key) override {
    if (!isEnabled()) return false;
    const float step = (key.shiftDown ? 10.0f : 1.0f) / 360.0f;
    float h = hue;
    if (key.is(KeyPress::Up) || key.is(KeyPress::Left)) h -= step;
    else if (key.is(KeyPress::Down) || key.is(KeyPress::Right)) h += step;
    else if (key.is(KeyPress::Home)) h = 0.0f;
    else if (key.is(KeyPress::End)) h = 1.0f;
    else return false;
    if (onPick) onPick(selectorClamp01(h));
    return true;
  }

  AccessibleInfo accessibleInfo() const override {
    AccessibleInfo info = baseInfo(AccessibleRole::Slider);
    info.hasRange = true;
    info.minValue = 0.0;
    info.maxValue = 360.0;
    info.currentValue = (double) hue * 360.0;
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.0f degrees", (double) hue * 360.0);
    info.value = buffer;
    return info;
  }

private:
  void pickAt(float y) {
    if (!isEnabled()) return;
    const Rect a = localBounds();
    if (a.h <= 0.0f) return;
    if (onPick) onPick(selectorClamp01((y - a.y) / a.h));
  }
};

/** The swatches: click to use one, right-click to store the current colour. */
class SwatchRow : public Widget {
public:
  static constexpr float kSize = 20.0f;
  static constexpr float kGap = 4.0f;

  SwatchRow() {
    setWantsKeyboardFocus(true);
    setAccessibleName("Swatches");
  }

  std::vector<Colour>* swatches = nullptr;
  std::function<void(int index)> onUse;
  std::function<void(int index, const MouseEvent&)> onStore;

  int perRow() const {
    const int n = (int) ((localBounds().w + kGap) / (kSize + kGap));
    return n > 0 ? n : 1;
  }

  Rect swatchBounds(int index) const {
    const int n = perRow();
    return {(float) (index % n) * (kSize + kGap), (float) (index / n) * (kSize + kGap), kSize, kSize};
  }

  float heightFor(int count) const {
    if (count <= 0) return 0.0f;
    const int rows = (count + perRow() - 1) / perRow();
    return (float) rows * (kSize + kGap) - kGap;
  }

  int swatchAt(Point p) const {
    if (!swatches) return -1;
    for (int i = 0; i < (int) swatches->size(); ++i)
      if (swatchBounds(i).contains(p)) return i;
    return -1;
  }

  int selected() const { return selected_; }

  void paint(Graphics& g) override {
    if (!swatches) return;
    for (int i = 0; i < (int) swatches->size(); ++i)
      lookAndFeel().drawColourSwatch(g, swatchBounds(i), (*swatches)[(size_t) i],
                                     i == selected_ && hasKeyboardFocus());
  }

  void mouseDown(const MouseEvent& e) override {
    const int at = swatchAt(e.position);
    if (at < 0 || !isEnabled()) return;
    selected_ = at;
    repaint();
    if (onUse) onUse(at);
  }

  void contextMenu(const MouseEvent& e) override {
    const int at = swatchAt(e.position);
    if (at < 0 || !isEnabled()) return;
    selected_ = at;
    repaint();
    if (onStore) onStore(at, e);
  }

  bool keyPressed(const KeyPress& key) override {
    if (!swatches || swatches->empty() || !isEnabled()) return false;
    const int n = (int) swatches->size();
    if (key.is(KeyPress::Left) || key.is(KeyPress::Up)) selected_ = selected_ <= 0 ? n - 1 : selected_ - 1;
    else if (key.is(KeyPress::Right) || key.is(KeyPress::Down)) selected_ = (selected_ + 1) % n;
    else if (key.is(KeyPress::Return) || key.character == ' ') {
      if (selected_ >= 0 && selected_ < n && onUse) onUse(selected_);
      return true;
    } else return false;
    repaint();
    return true;
  }

  AccessibleInfo accessibleInfo() const override {
    AccessibleInfo info = baseInfo(AccessibleRole::List);
    if (swatches && !swatches->empty()) {
      info.hasRange = true;
      info.minValue = 0.0;
      info.maxValue = (double) swatches->size() - 1.0;
      info.currentValue = (double) (selected_ < 0 ? 0 : selected_);
      if (selected_ >= 0 && selected_ < (int) swatches->size())
        info.value = selectorHexString((*swatches)[(size_t) selected_], true);
    }
    return info;
  }

private:
  int selected_ = -1;
};

} // namespace detail

/**
 * A colour picker.
 *
 * Reports through onChange with every change, from any of its parts, and
 * never for a colour set with notify false -- the rule every control here
 * follows so a host or a theme pushing a colour in is not echoed back out.
 */
class ColourSelector : public Widget {
public:
  /** Which parts are shown, as bits; the reference's names. */
  enum Flags {
    showAlphaChannel = 1 << 0, ///< an alpha slider, and alpha in the hex code; without it colours are opaque
    showColourAtTop = 1 << 1,  ///< the preview strip along the top
    editableColour = 1 << 2,   ///< with showColourAtTop, a hex field to type a colour into
    showSliders = 1 << 3,      ///< red, green and blue sliders (and alpha, with showAlphaChannel)
    showColourspace = 1 << 4,  ///< the saturation and brightness square, and the hue strip
  };

  /** Every part. */
  static constexpr int kDefaultFlags =
      showAlphaChannel | showColourAtTop | editableColour | showSliders | showColourspace;
  static constexpr float kTopHeight = 28.0f;    ///< the preview strip's height, logical pixels
  static constexpr float kSliderHeight = 22.0f; ///< one channel slider row's height
  static constexpr float kHueWidth = 22.0f;     ///< the hue strip's width

  /** A picker showing the parts in `flags`, `edgeGap` logical pixels in from
   *  its edges and `gapAroundColourSpace` around the square. Starts at opaque
   *  white. */
  explicit ColourSelector(int flags = kDefaultFlags, float edgeGap = 4.0f, float gapAroundColourSpace = 7.0f)
      : flags_(flags), edgeGap_(edgeGap), spaceGap_(gapAroundColourSpace) {
    setAccessibleName("Colour");
    space_.onPick = [this](float s, float v) { setHSV(h_, s, v, true); };
    hue_.onPick = [this](float h) { setHSV(h, s_, v_, true); };
    addChild(&space_);
    addChild(&hue_);

    const char* names[4] = {"Red", "Green", "Blue", "Alpha"};
    const char* letters[4] = {"R", "G", "B", "A"};
    for (int i = 0; i < 4; ++i) {
      channels_[i].label.setText(letters[i]);
      channels_[i].slider.setStyle(Slider::Style::LinearHorizontal);
      channels_[i].slider.setAccessibleName(names[i]);
      channels_[i].readout.setJustify(Justify::Right);
      channels_[i].slider.onValueChange = [this, i](float v) { channelMoved(i, v); };
      addChild(&channels_[i].label);
      addChild(&channels_[i].slider);
      addChild(&channels_[i].readout);
    }

    hex_.setAccessibleName("Hex");
    hex_.setMaxLength(12);
    hex_.setAllowedCharacters("0123456789abcdefABCDEF#xX");
    hex_.onReturn = [this](const std::string& text) { applyHex(text); };
    hex_.onFocusLost = [this](const std::string& text) { applyHex(text); };
    addChild(&hex_);

    swatchRow_.swatches = &swatches_;
    swatchRow_.onUse = [this](int index) {
      if (index >= 0 && index < (int) swatches_.size()) setCurrentColour(swatches_[(size_t) index]);
    };
    swatchRow_.onStore = [this](int index, const MouseEvent& e) { offerSwatchMenu(index, e); };
    addChild(&swatchRow_);

    applyFlags();
    syncChildren();
  }

  /** What onChange carries, every time the colour moves. */
  std::function<void(Colour)> onChange;
  /** A swatch was overwritten with the current colour, for a caller that
   *  keeps its swatches somewhere. */
  std::function<void(int index, Colour)> onSwatchChanged;

  /** The colour, straight alpha; opaque unless showAlphaChannel. */
  Colour currentColour() const { return colour_; }

  /** Show `c` (made opaque without showAlphaChannel) in every part. Calls
   *  onChange when it changed and `notify` is true. */
  void setCurrentColour(Colour c, bool notify = true) {
    if (!(flags_ & showAlphaChannel)) c.a = 255;
    if (c == colour_) {
      syncChildren();
      return;
    }
    colour_ = c;
    detail::selectorColourToHsv(c, h_, s_, v_);
    syncChildren();
    if (notify && onChange) onChange(colour_);
  }

  /** Hue, saturation and brightness, each 0..1. The alpha is kept. */
  void setHSV(float h, float s, float v, bool notify = true) {
    h_ = detail::selectorClamp01(h);
    s_ = detail::selectorClamp01(s);
    v_ = detail::selectorClamp01(v);
    const Colour c = detail::selectorHsvToColour(h_, s_, v_, colour_.a);
    const bool changed = c != colour_;
    colour_ = c;
    syncChildren();
    if (notify && changed && onChange) onChange(colour_);
  }

  /** 0..1 around the colour wheel, red at 0. Kept even where the colour no
   *  longer says it -- at zero brightness or saturation. */
  float hue() const { return h_; }
  /** 0..1, grey to full colour. */
  float saturation() const { return s_; }
  /** 0..1, black to full brightness. */
  float brightness() const { return v_; }

  /** The hex code as shown: RRGGBB, or AARRGGBB with the alpha channel. */
  std::string hexString() const { return detail::selectorHexString(colour_, (flags_ & showAlphaChannel) != 0); }

  /** Show the parts in `flags` instead. Dropping showAlphaChannel makes the
   *  colour opaque, which calls onChange if it was not. */
  void setFlags(int flags) {
    flags_ = flags;
    if (!(flags_ & showAlphaChannel) && colour_.a != 255) setCurrentColour(colour_.withAlpha(255));
    applyFlags();
    resized();
    repaint();
  }
  /** The Flags bits in force. */
  int flags() const { return flags_; }

  // ── Swatches ─────────────────────────────────────────────────────────────

  /** The row of stored colours under the picker: a click uses one, a right
   *  click offers to store the current colour in it. */
  void setSwatches(std::vector<Colour> swatches) {
    swatches_ = std::move(swatches);
    resized();
    repaint();
  }
  /** How many swatches there are. */
  int numSwatches() const { return (int) swatches_.size(); }
  /** Swatch `index`, or opaque black out of range. */
  Colour swatchColour(int index) const {
    return index >= 0 && index < (int) swatches_.size() ? swatches_[(size_t) index] : Colour();
  }
  /** Store `c` in swatch `index` and call onSwatchChanged; ignored out of
   *  range. */
  void setSwatchColour(int index, Colour c) {
    if (index < 0 || index >= (int) swatches_.size()) return;
    swatches_[(size_t) index] = c;
    swatchRow_.repaint();
    if (onSwatchChanged) onSwatchChanged(index, c);
  }

  // ── Where the parts are, for a test or a caller laying out around it ──────

  /** The preview strip, in this component's coordinates; empty when hidden. */
  Rect previewBounds() const { return preview_; }
  /** The saturation and brightness square, in this component's coordinates. */
  Rect colourSpaceBounds() const { return space_.bounds(); }
  /** The hue strip, in this component's coordinates. */
  Rect hueStripBounds() const { return hue_.bounds(); }
  /** Swatch `index`, in this component's coordinates. */
  Rect swatchBounds(int index) const {
    const Rect r = swatchRow_.swatchBounds(index);
    return r.translated(swatchRow_.bounds().x, swatchRow_.bounds().y);
  }
  /** The point in this component's coordinates where a saturation and a
   *  brightness sit in the square. */
  Point colourSpacePoint(float s, float v) const {
    const Point p = space_.markerFor(s, v);
    return {p.x + space_.bounds().x, p.y + space_.bounds().y};
  }
  /** The slider for channel 0 red, 1 green, 2 blue or 3 alpha; held to 0..3. */
  Slider& channelSlider(int channel) { return channels_[channel < 0 ? 0 : (channel > 3 ? 3 : channel)].slider; }
  /** The hex field. */
  TextEditor& hexEditor() { return hex_; }
  /** The menu a right click on a swatch shows. */
  PopupMenuOverlay& swatchMenu() { return swatchMenu_; }

  // ── Component ────────────────────────────────────────────────────────────

  void resized() override {
    const Rect area = localBounds().reduced(edgeGap_);
    float y = area.y;
    preview_ = Rect();
    if (flags_ & showColourAtTop) {
      const float h = (std::min)(kTopHeight, area.h);
      const bool editable = (flags_ & editableColour) != 0;
      const float hexW = editable ? (std::min)(area.w * 0.5f, 110.0f) : 0.0f;
      preview_ = Rect(area.x, y, area.w - hexW - (editable ? edgeGap_ : 0.0f), h);
      hex_.setBounds(editable ? Rect(area.right() - hexW, y, hexW, h) : Rect());
      y += h + edgeGap_;
    }

    const int channels = (flags_ & showSliders) ? ((flags_ & showAlphaChannel) ? 4 : 3) : 0;
    const float slidersH = (float) channels * kSliderHeight;
    const float swatchH = swatchRow_.isVisible() ? swatchRow_.heightFor(numSwatchesForLayout(area.w)) : 0.0f;
    const float below = slidersH + (swatchH > 0.0f ? swatchH + edgeGap_ : 0.0f);

    if (flags_ & showColourspace) {
      const float h = (std::max)(0.0f, area.bottom() - y - below - spaceGap_);
      const Rect block(area.x, y, area.w, h);
      space_.setBounds({block.x, block.y, (std::max)(0.0f, block.w - kHueWidth - spaceGap_), block.h});
      hue_.setBounds({block.right() - kHueWidth, block.y, kHueWidth, block.h});
      y += h + spaceGap_;
    }

    for (int i = 0; i < 4; ++i) {
      if (i < channels) {
        const Rect row(area.x, y + (float) i * kSliderHeight, area.w, kSliderHeight);
        channels_[i].label.setBounds({row.x, row.y, 18.0f, row.h});
        channels_[i].readout.setBounds({row.right() - 34.0f, row.y, 34.0f, row.h});
        channels_[i].slider.setBounds({row.x + 20.0f, row.y, (std::max)(0.0f, row.w - 58.0f), row.h});
      }
    }
    y += slidersH;
    if (swatchH > 0.0f) swatchRow_.setBounds({area.x, y + edgeGap_, area.w, swatchH});
    else swatchRow_.setBounds({area.x, y, area.w, 0.0f});
  }

  void paint(Graphics& g) override {
    LookAndFeel& lf = lookAndFeel();
    g.fillAll(lf.background());
    if (!preview_.isEmpty()) {
      lf.drawColourSwatch(g, preview_, colour_, false);
      if (!(flags_ & editableColour)) {
        // The code on the colour itself, in whichever of black or white reads
        // against it -- the same contrast rule a toggled button's label uses.
        g.setColour(colour_.withAlpha(255).contrasting());
        font().drawIn(g, hexString(), preview_, Justify::Centred);
      }
    }
  }

  AccessibleInfo accessibleInfo() const override {
    AccessibleInfo info = baseInfo(AccessibleRole::Group);
    info.value = hexString();
    return info;
  }

private:
  /** One row of the slider section -- red, green, blue or alpha: its letter,
   *  a 0..1 slider standing for 0..255, and the byte value as text. */
  struct Channel {
    Label label;
    Slider slider;
    Label readout;
  };

  int numSwatchesForLayout(float width) const {
    (void) width;
    return (int) swatches_.size();
  }

  void applyFlags() {
    const bool space = (flags_ & showColourspace) != 0;
    space_.setVisible(space);
    hue_.setVisible(space);
    const bool sliders = (flags_ & showSliders) != 0;
    for (int i = 0; i < 4; ++i) {
      const bool shown = sliders && (i < 3 || (flags_ & showAlphaChannel) != 0);
      channels_[i].label.setVisible(shown);
      channels_[i].slider.setVisible(shown);
      channels_[i].readout.setVisible(shown);
    }
    hex_.setVisible((flags_ & showColourAtTop) && (flags_ & editableColour));
  }

  /** Push the colour into every part, quietly: a slider told its new value
   *  must not report it back as a change, or every update would echo. */
  void syncChildren() {
    space_.hue = h_;
    space_.saturation = s_;
    space_.value = v_;
    space_.repaint();
    hue_.hue = h_;
    hue_.repaint();
    const uint8_t parts[4] = {colour_.r, colour_.g, colour_.b, colour_.a};
    for (int i = 0; i < 4; ++i) {
      channels_[i].slider.setValue((float) parts[i] / 255.0f, false);
      const std::string text = std::to_string((int) parts[i]);
      channels_[i].slider.setAccessibleValueText(text);
      channels_[i].readout.setText(text);
    }
    // Not while somebody is typing in it: replacing the text under the caret
    // would throw away half a hex code.
    if (!hex_.hasKeyboardFocus()) hex_.setText(hexString());
    repaint();
  }

  void channelMoved(int channel, float normalised) {
    Colour c = colour_;
    const uint8_t v = detail::selectorToByte(normalised);
    if (channel == 0) c.r = v;
    else if (channel == 1) c.g = v;
    else if (channel == 2) c.b = v;
    else c.a = v;
    setCurrentColour(c);
  }

  void applyHex(const std::string& text) {
    Colour parsed;
    if (detail::selectorParseHex(text, parsed)) {
      if (!(flags_ & showAlphaChannel)) parsed.a = 255;
      // onChange is the owner's code, and may destroy this selector -- a
      // picker that closes once a colour is entered.
      detail::DeletionWatch alive(this);
      setCurrentColour(parsed);
      if (alive.deleted) return;
    }
    // Shown as the selector sees it either way: what was typed, normalised,
    // or -- for something that did not parse -- the colour that is still in
    // force, so the field never shows a code the selector is not holding.
    hex_.setText(hexString());
  }

  void offerSwatchMenu(int index, const MouseEvent& e) {
    PopupMenu menu;
    menu.addItem(1, "Use this swatch");
    menu.addItem(2, "Store the current colour here");
    const Point at = swatchRow_.swatchBounds(index).centre();
    (void) e;
    swatchMenu_.show(swatchRow_, menu, Rect(at.x, at.y, 1.0f, 1.0f), [this, index](int id) {
      if (id == 1 && index < (int) swatches_.size()) setCurrentColour(swatches_[(size_t) index]);
      else if (id == 2) setSwatchColour(index, colour_);
    });
  }

  int flags_;
  float edgeGap_, spaceGap_;
  Colour colour_{255, 255, 255, 255};
  float h_ = 0.0f, s_ = 0.0f, v_ = 1.0f;
  Rect preview_;
  detail::ColourSpaceArea space_;
  detail::HueStrip hue_;
  Channel channels_[4];
  TextEditor hex_;
  detail::SwatchRow swatchRow_;
  std::vector<Colour> swatches_;
  PopupMenuOverlay swatchMenu_;
};

} // namespace gfx
} // namespace sonore
