// SPDX-License-Identifier: Apache-2.0
//
// Three ways to look at audio: a scope that scrolls, a waveform that sits
// still, and a spectrum.
//
// ── Why these are separate ──────────────────────────────────────────────────
//
// AudioVisualiser shows what is happening NOW, fed continuously from the audio
// thread. WaveformView shows a fixed thing already loaded -- a sample, a
// recorded take -- from an AudioThumbnail built once. They have almost nothing
// in common: one is a ring buffer with a real-time constraint, the other is a
// lookup with a playhead. SpectrumView is the scope's shape again -- a ring the
// audio thread fills -- with the transform run on the interface's clock.
//
// ── None of them owns its data ──────────────────────────────────────────────
//
// AudioVisualiser points at an AudioScopeBuffer, WaveformView at an
// AudioThumbnail and SpectrumView at a SpectrumFeed, and all are fed by
// something that outlives the editor. A scope that owned its buffer would start
// blank every time a window was opened instead of showing the last second of
// audio; a waveform that owned its thumbnail would copy megabytes the plugin
// already has.
//
// The real-time sides live in scope_buffer.h and spectrum_feed.h, with no UI
// anywhere near them.
#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>
#include <algorithm>
#include <functional>
#include <string>

#include "../scope_buffer.h"
#include "../spectrum_feed.h"
#include "../thumbnail.h"
#include "widgets.h"

namespace sonore {
namespace gfx {

/**
 * A scrolling oscilloscope: a view onto an AudioScopeBuffer.
 *
 * Each column is one BUCKET -- the minimum and maximum of however many samples
 * fell into it -- rather than one sample. A scope that plotted individual
 * samples at a pixel apart would alias badly: a 10 kHz tone at 48 kHz is under
 * five samples a cycle, and picking every Nth one draws a slow wobble that is
 * not in the audio. Min and max over the group draws the envelope, which is
 * what an analogue scope shows and what people expect.
 *
 * The buffer is NOT owned, and that is the whole point of the split: it is fed
 * by the audio thread and has to exist whether or not an editor is open, so a
 * scope shows the last second of audio the moment somebody opens the window
 * rather than starting blank. Same arrangement as WaveformView and its
 * thumbnail.
 */
class AudioVisualiser : public Widget {
public:
  /** Show `buffer`, which is not owned and must outlive this view or be
   *  replaced first; null draws an empty scope. The audio thread may go on
   *  writing it while this paints. */
  void setBuffer(const AudioScopeBuffer* buffer) {
    buffer_ = buffer;
    repaint();
  }

  /** The buffer shown, or null. */
  const AudioScopeBuffer* buffer() const { return buffer_; }

  void paint(Graphics& g) override {
    if (!buffer_) {
      lookAndFeel().drawAudioVisualiser(g, localBounds(), nullptr, nullptr, 0);
      return;
    }
    // Copied onto the STACK, oldest first, before anything is drawn. The audio
    // thread is still writing: reading every column once, up front, makes the
    // picture one consistent snapshot rather than a trace assembled from
    // moments a millisecond apart.
    const int n = buffer_->columns();
    float lows[AudioScopeBuffer::kMaxColumns];
    float highs[AudioScopeBuffer::kMaxColumns];
    for (int i = 0; i < n; ++i) buffer_->columnAt(n - 1 - i, &lows[i], &highs[i]);
    lookAndFeel().drawAudioVisualiser(g, localBounds(), lows, highs, n);
  }

private:
  const AudioScopeBuffer* buffer_ = nullptr;
};

/**
 * A waveform, drawn from an AudioThumbnail.
 *
 * The thumbnail has existed in this SDK since before the native UI did, and
 * nothing drew it: a sampler that shows no waveform gives a user nothing to aim
 * at when setting a start point.
 *
 * The thumbnail is NOT owned. It is usually built by the DSP from a file it
 * loaded, and a display that owned it would either copy megabytes or decide the
 * lifetime of something the plugin already holds.
 */
class WaveformView : public Widget {
public:
  /** Show `thumb`, which is not owned and must outlive this view or be
   *  replaced first; null draws no waveform. */
  void setThumbnail(const AudioThumbnail* thumb) {
    thumb_ = thumb;
    repaint();
  }

  /** The thumbnail shown, or null. */
  const AudioThumbnail* thumbnail() const { return thumb_; }

  /** 0..1 along the waveform, or negative for none. */
  void setPlayhead(float position) {
    if (position == playhead_) return;
    playhead_ = position;
    repaint();
  }

  /** 0..1 along the waveform, or negative for none. */
  float playhead() const { return playhead_; }

  /** A highlighted region, 0..1. An empty one is drawn as nothing rather than
   *  as a zero-width line, which would look like a second playhead. */
  void setSelection(float from, float to) {
    selectionFrom_ = from < to ? from : to;
    selectionTo_ = from < to ? to : from;
    repaint();
  }

  /** The selection's start, 0..1. */
  float selectionFrom() const { return selectionFrom_; }
  /** The selection's end, 0..1; equal to selectionFrom when empty. */
  float selectionTo() const { return selectionTo_; }
  /** Whether a region of more than zero width is selected. */
  bool hasSelection() const { return selectionTo_ > selectionFrom_; }

  /** Clicking scrubs, dragging selects. Fired with 0..1 positions. */
  std::function<void(float position)> onClick;
  /** The new selection, 0..1, on each move of a drag. The press itself
   *  empties the selection and calls onClick instead; setSelection calls
   *  neither. */
  std::function<void(float from, float to)> onSelectionChange;

  void mouseDown(const MouseEvent& e) override {
    if (!isEnabled()) return;
    dragFrom_ = positionOf(e.position.x);
    setSelection(dragFrom_, dragFrom_);
    if (onClick) onClick(dragFrom_);
  }

  void mouseDrag(const MouseEvent& e) override {
    if (!isEnabled()) return;
    const float at = positionOf(e.position.x);
    setSelection(dragFrom_, at);
    if (onSelectionChange) onSelectionChange(selectionFrom_, selectionTo_);
  }

  void paint(Graphics& g) override {
    lookAndFeel().drawWaveform(g, localBounds(), thumb_, playhead_, selectionFrom_, selectionTo_);
  }

private:
  float positionOf(float x) const {
    const Rect area = localBounds();
    if (area.w <= 0.0f) return 0.0f;
    const float f = (x - area.x) / area.w;
    return f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
  }

  const AudioThumbnail* thumb_ = nullptr;
  float playhead_ = -1.0f;
  float selectionFrom_ = 0.0f, selectionTo_ = 0.0f;
  float dragFrom_ = 0.0f;
};

/**
 * A spectrum analyzer: a view onto a SpectrumFeed.
 *
 * The same display a Sonorie page mounts with sonore.spectrum(), drawn
 * natively for an editor written by hand: the plugin's output on a log axis,
 * calibrated so a full-scale sine reads 0 dBFS, with the input faint behind
 * it when a second feed is given, a held peak line, fractional-octave
 * smoothing, a tilt, and an equaliser's own response drawn over the top.
 *
 * Like AudioVisualiser it does not own its data. The feeds belong to whatever
 * owns the audio (a plugin's instance has one for its output and one for its
 * input), so a window opened mid-session opens onto audio that is already
 * playing. The editor calls update() from its timer: that measures the feeds
 * on the interface thread and steps the ballistics, and repaints only when
 * something moved.
 *
 * Every look is a setter, and every colour defaults to the look and feel's.
 */
class SpectrumView : public Widget {
public:
  /** How the spectrum is drawn: a smooth line over a soft fill, the line
   *  alone, rounded bars, or segmented ladders lit green, amber and red. */
  enum class Style { Curve, Line, Bars, Led };
  /** Values per frame: the feed's grid. */
  static constexpr int kPoints = SpectrumFeed::kPoints;

  SpectrumView() {
    for (int i = 0; i < kPoints; ++i) {
      rawOut_[i] = rawIn_[i] = out_[i] = in_[i] = shownOut_[i] = shownIn_[i] = peak_[i] = SpectrumFeed::kFloorDb;
      peakAge_[i] = 0.0;
    }
  }

  /** Show `output`, and `input` behind it (or null), measured at the rate
   *  setSampleRate() gives. Neither is owned: each must outlive this view or
   *  be replaced first. Turns both feeds on. */
  void setFeeds(SpectrumFeed* output, SpectrumFeed* input = nullptr) {
    feedOut_ = output;
    feedIn_ = input;
    if (feedOut_) feedOut_->setActive(true);
    if (feedIn_) feedIn_->setActive(true);
  }
  /** The rate the feeds are measured at; 48 kHz until set. */
  void setSampleRate(double sampleRate) { sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0; }

  /** [main thread] Measure the feeds and advance the display by `dt` seconds:
   *  call from the editor's timer. Repaints only when something moved. */
  void update(double dt) {
    if (feedOut_) {
      float out[kPoints], in[kPoints];
      feedOut_->analyse(sampleRate_, out);
      if (feedIn_) feedIn_->analyse(sampleRate_, in);
      push(out, feedIn_ ? in : nullptr);
    }
    step(dt);
  }

  /** A frame of your own instead of a feed: kPoints dBFS values on the
   *  feed's grid (SpectrumFeed::pointHz), and the input's or null. */
  void push(const float* outputDb, const float* inputDb) {
    if (outputDb)
      for (int i = 0; i < kPoints; ++i) rawOut_[i] = outputDb[i] == outputDb[i] ? outputDb[i] : SpectrumFeed::kFloorDb;
    hasInput_ = inputDb != nullptr;
    if (inputDb)
      for (int i = 0; i < kPoints; ++i) rawIn_[i] = inputDb[i] == inputDb[i] ? inputDb[i] : SpectrumFeed::kFloorDb;
    shapeTargets();
  }

  /** Advance the display `dt` seconds toward the last frame: a fast rise, a
   *  fall at the speed set, the peak line held and then let down. */
  void step(double dt) {
    if (!(dt > 0.0)) dt = 0.0;
    if (dt > 0.25) dt = 0.25;
    const double rel = 1.0 - std::exp(-dt / speed_), att = 1.0 - std::exp(-dt / 0.012);
    bool moved = false;
    for (int i = 0; i < kPoints; ++i) {
      moved |= approach(shownOut_[i], out_[i], att, rel);
      if (hasInput_) moved |= approach(shownIn_[i], in_[i], att, rel);
      if (peakHold_) {
        if (shownOut_[i] >= peak_[i]) {
          peak_[i] = shownOut_[i];
          peakAge_[i] = 0.0;
        } else if ((peakAge_[i] += dt) > holdSeconds_) {
          const float fall = (float) (18.0 * dt), next = std::max(shownOut_[i], peak_[i] - fall);
          if (next != peak_[i]) {
            peak_[i] = next;
            moved = true;
          }
        }
      }
    }
    if (moved || dirty_) {
      dirty_ = false;
      repaint();
    }
  }

  /** How it is drawn. */
  void setStyle(Style s) { style_ = s; shapeTargets(); look(); }
  Style style() const { return style_; }
  /** The dBFS shown, bottom to top; -90 .. +6 until set. */
  void setRange(float lowDb, float highDb) {
    if (highDb > lowDb) {
      lowDb_ = lowDb;
      highDb_ = highDb;
    }
    look();
  }
  /** The frequencies shown, left to right; 20 Hz .. 20 kHz until set. */
  void setFrequencyRange(double lowHz, double highHz) {
    if (lowHz > 0.0 && highHz > lowHz) {
      lowHz_ = lowHz;
      highHz_ = highHz;
    }
    look();
  }
  /** Fractional-octave smoothing, in octaves (1/12, 1/6, 1/3), averaged in
   *  power as a 1/3-octave analyser sums a band; 0 is none. Negative (the
   *  default) is 1/6 for the line styles and none for the bars. */
  void setSmoothing(double octaves) { smoothing_ = octaves; shapeTargets(); }
  /** A slope added about 1 kHz, in dB per octave: 4.5 makes pink noise, and
   *  so most mixes, read flat. 0 until set. */
  void setTilt(double dbPerOctave) { tilt_ = dbPerOctave; shapeTargets(); }
  /** How fast the display falls, as a time constant in seconds: 0.08 fast,
   *  0.22 (the default) medium, 0.6 slow. */
  void setSpeed(double seconds) { speed_ = seconds > 0.005 ? seconds : 0.005; }
  /** A held maximum line (a cap on each bar), let down after `holdSeconds`. */
  void setPeakHold(bool on, double holdSeconds = 1.5) {
    peakHold_ = on;
    holdSeconds_ = holdSeconds > 0.0 ? holdSeconds : 0.0;
    look();
  }
  /** Draw the input behind the output when there is one; on until set. */
  void setShowInput(bool on) { showInput_ = on; look(); }
  /** The soft fill under the curve; on until set. */
  void setFill(bool on) { fill_ = on; look(); }
  /** Grid lines, and the Hz and dB labels (labels need the widget's font). */
  void setGrid(bool on) { grid_ = on; look(); }
  void setLabels(bool on) { labels_ = on; look(); }
  /** The trace; the look and feel's accent until set. */
  void setColour(Colour c) { colour_ = c; hasColour_ = true; look(); }
  /** The input behind it; the look and feel's text colour, faint, until set. */
  void setInputColour(Colour c) { inputColour_ = c; hasInputColour_ = true; look(); }
  /** The panel behind it; the scope's recessed panel until set, and a fully
   *  transparent colour for none (drawn on a face of your own). */
  void setBackground(Colour c) { background_ = c; hasBackground_ = true; look(); }
  /** How many bars or ladders; 0 (the default) picks by width. */
  void setBands(int count) { bands_ = count > 0 ? (count < 128 ? count : 128) : 0; look(); }

  /** An equaliser's response drawn over the analyzer, on its own +/-
   *  `rangeDb` scale about the middle: `curve` maps hertz to dB. An empty
   *  function removes it; `colour` left transparent is the accent, lighter. */
  void setCurve(std::function<double(double)> curve, float rangeDb = 18.0f, Colour colour = Colour(0, 0, 0, 0)) {
    curve_ = std::move(curve);
    curveRange_ = rangeDb > 0.0f ? rangeDb : 18.0f;
    curveColour_ = colour;
    look();
  }

  /** What is drawn now: kPoints dBFS values, the output after ballistics,
   *  smoothing and tilt. */
  const float* displayed() const { return shownOut_; }
  /** The held peaks, kPoints dBFS values, each falling once its hold ends. */
  const float* peaks() const { return peak_; }

  void paint(Graphics& g) override {
    const Rect area = localBounds();
    if (area.w <= 2.0f || area.h <= 2.0f) return;
    LookAndFeel& laf = lookAndFeel();
    const Colour trace = hasColour_ ? colour_ : laf.accent();
    if (!hasBackground_ || background_.a) {
      g.setColour(hasBackground_ ? background_ : laf.background().darkened(0.03f));
      g.fillRoundedRect(area, 3.0f);
    }
    g.save();
    g.clipTo(area);
    // The bar styles keep a strip at the bottom for the frequency labels:
    // drawn over lit LED segments, the labels vanish into them.
    const bool bars = style_ == Style::Bars || style_ == Style::Led;
    const bool showLabels = labels_ && font().isValid();
    Rect plot = area;
    if (bars && grid_ && showLabels && area.w >= 120.0f && area.h >= 80.0f) plot.h -= 14.0f;
    if (grid_) paintGrid(g, plot, laf);
    if (bars) {
      paintBars(g, plot, trace);
    } else {
      if (hasInput_ && showInput_) {
        const Colour ic = hasInputColour_ ? inputColour_ : laf.text().withMultipliedAlpha(0.55f);
        paintTrace(g, plot, shownIn_, ic, true, 1.0f, 0.45f);
      }
      if (peakHold_) paintTrace(g, plot, peak_, trace, false, 1.0f, 0.5f);
      paintTrace(g, plot, shownOut_, trace, style_ == Style::Curve && fill_, 1.5f, 1.0f);
    }
    if (curve_) paintCurve(g, plot, curveColour_.a ? curveColour_ : laf.accent().brightened(0.25f));
    if (showLabels) paintLabels(g, area, plot, laf);
    g.restore();
  }

private:
  static bool approach(float& shown, float target, double att, double rel) {
    const float before = shown;
    shown += (float) ((target - shown) * (target > shown ? att : rel));
    if (std::fabs(shown - target) < 0.01f) shown = target;
    return std::fabs(shown - before) > 0.02f;
  }

  void look() {
    dirty_ = true;
    repaint();
  }

  double smoothingNow() const {
    if (smoothing_ >= 0.0) return smoothing_;
    return style_ == Style::Bars || style_ == Style::Led ? 0.0 : 1.0 / 6.0;
  }

  // The same shaping a page's analyzer applies (spectrum.ts): smoothing in
  // power, then the tilt about 1 kHz.
  void shapeTargets() {
    shape(rawOut_, out_);
    if (hasInput_) shape(rawIn_, in_);
    dirty_ = true;
  }
  void shape(const float* src, float* dst) const {
    const double perPoint = std::log2(SpectrumFeed::kHighHz / SpectrumFeed::kLowHz) / (kPoints - 1);
    const double sm = smoothingNow();
    const int half = sm > 0.0 ? std::max(1, (int) std::lround(sm / 2.0 / perPoint)) : 0;
    for (int i = 0; i < kPoints; ++i) {
      double v = src[i];
      if (half > 0) {
        double sum = 0.0, wsum = 0.0;
        for (int d = -half; d <= half; ++d) {
          const int k = i + d;
          if (k < 0 || k >= kPoints) continue;
          const double w = 1.0 - std::abs(d) / (double) (half + 1);
          sum += w * std::pow(10.0, src[k] / 10.0);
          wsum += w;
        }
        v = sum > 0.0 ? 10.0 * std::log10(sum / wsum) : SpectrumFeed::kFloorDb;
      }
      if (tilt_ != 0.0 && v > SpectrumFeed::kFloorDb) v += tilt_ * std::log2(SpectrumFeed::pointHz(i) / 1000.0);
      dst[i] = v > SpectrumFeed::kFloorDb ? (float) v : SpectrumFeed::kFloorDb;
    }
  }

  float xOf(const Rect& a, double hz) const {
    return a.x + (float) (std::log(hz / lowHz_) / std::log(highHz_ / lowHz_)) * a.w;
  }
  float yOf(const Rect& a, double db) const {
    return a.y + a.h * (float) (1.0 - (db - lowDb_) / (highDb_ - lowDb_));
  }

  void paintGrid(Graphics& g, const Rect& a, LookAndFeel& laf) {
    const Colour minor = laf.outline().withMultipliedAlpha(0.45f), major = laf.outline();
    for (double decade = 10.0; decade <= 10000.0; decade *= 10.0)
      for (int m = 1; m < 10; ++m) {
        const double f = decade * m;
        if (f < lowHz_ || f > highHz_) continue;
        const float x = std::floor(xOf(a, f)) + 0.5f;
        g.setColour(m == 1 ? major : minor);
        g.drawLine(x, a.y, x, a.bottom(), 1.0f);
      }
    const float span = highDb_ - lowDb_, step = span > 72.0f ? 12.0f : span > 36.0f ? 6.0f : 3.0f;
    for (float db = std::ceil(lowDb_ / step) * step; db <= highDb_; db += step) {
      const float y = std::floor(yOf(a, db)) + 0.5f;
      g.setColour(db == 0.0f ? major : minor);
      g.drawLine(a.x, y, a.right(), y, 1.0f);
    }
  }

  void tracePath(const Rect& a, const float* v, Path& p, float& firstX, float& lastX) const {
    float xs[kPoints], ys[kPoints];
    int n = 0;
    for (int i = 0; i < kPoints; ++i) {
      const double f = SpectrumFeed::pointHz(i);
      const bool beforeLow = f < lowHz_ && !(i + 1 < kPoints && SpectrumFeed::pointHz(i + 1) >= lowHz_);
      const bool pastHigh = f > highHz_ && !(i > 0 && SpectrumFeed::pointHz(i - 1) <= highHz_);
      if (beforeLow || pastHigh) continue;
      xs[n] = xOf(a, f);
      ys[n] = std::max(a.y - 2.0f, std::min(a.bottom() + 2.0f, yOf(a, v[i])));
      ++n;
    }
    if (n < 2) return;
    p.moveTo(xs[0], ys[0]);
    for (int i = 1; i < n - 1; ++i) p.quadTo(xs[i], ys[i], (xs[i] + xs[i + 1]) * 0.5f, (ys[i] + ys[i + 1]) * 0.5f);
    p.lineTo(xs[n - 1], ys[n - 1]);
    firstX = xs[0];
    lastX = xs[n - 1];
  }

  void paintTrace(Graphics& g, const Rect& a, const float* v, Colour c, bool filled, float width, float alpha) {
    Path line;
    float x0 = 0.0f, x1 = 0.0f;
    tracePath(a, v, line, x0, x1);
    if (line.isEmpty()) return;
    if (filled) {
      Path below = line;
      below.lineTo(x1, a.bottom() + 2.0f);
      below.lineTo(x0, a.bottom() + 2.0f);
      below.close();
      ColourGradient fade = ColourGradient::vertical(c.withMultipliedAlpha(0.42f * alpha), a.y,
                                                     c.withMultipliedAlpha(0.02f * alpha), a.bottom());
      fade.addStop(0.65f, c.withMultipliedAlpha(0.14f * alpha));
      g.setGradient(fade);
      g.fillPath(below);
    }
    g.setColour(c.withMultipliedAlpha(alpha));
    g.strokePath(line, width);
  }

  // Bands of equal width on the log axis, each reading the loudest grid point
  // inside it (the point nearest its centre when it is narrower than a step).
  void bandValues(const float* v, int count, float* out) const {
    const double r = std::pow(highHz_ / lowHz_, 1.0 / count);
    const double ratio = std::pow(SpectrumFeed::kHighHz / SpectrumFeed::kLowHz, 1.0 / (kPoints - 1));
    for (int b = 0; b < count; ++b) {
      const double f0 = lowHz_ * std::pow(r, b), f1 = f0 * r;
      float best = SpectrumFeed::kFloorDb;
      bool any = false;
      for (int i = 0; i < kPoints; ++i) {
        const double f = SpectrumFeed::pointHz(i);
        if (f < f0 || f >= f1) continue;
        any = true;
        best = std::max(best, v[i]);
      }
      if (!any) {
        const int k = (int) std::lround(std::log(std::sqrt(f0 * f1) / SpectrumFeed::kLowHz) / std::log(ratio));
        best = v[std::max(0, std::min(kPoints - 1, k))];
      }
      out[b] = best;
    }
  }

  void paintBars(Graphics& g, const Rect& a, Colour c) {
    int count = bands_ > 0 ? bands_ : std::max(10, std::min(64, (int) (a.w / 14.0f)));
    const float gap = a.w / count > 6.0f ? 2.0f : 1.0f;
    float bw = (a.w - gap * (count - 1)) / count;
    if (bw < 1.0f) {
      count = std::max(1, (int) ((a.w + gap) / (1.0f + gap)));
      if (count > 128) count = 128;
      bw = (a.w - gap * (count - 1)) / count;
    }
    float vals[128], peaks[128];
    bandValues(shownOut_, count, vals);
    if (peakHold_) bandValues(peak_, count, peaks);
    if (style_ == Style::Bars) {
      g.setGradient(ColourGradient::vertical(c.withMultipliedAlpha(0.95f), a.y, c.withMultipliedAlpha(0.25f), a.bottom()));
      for (int b = 0; b < count; ++b) {
        const float x = a.x + b * (bw + gap), y = std::max(a.y, yOf(a, vals[b])), h = a.bottom() - y;
        if (h > 0.0f) g.fillRoundedRect(Rect(x, y, bw, h), std::min(2.0f, bw * 0.5f));
      }
      if (peakHold_) {
        g.setColour(c.withMultipliedAlpha(0.9f));
        for (int b = 0; b < count; ++b) {
          const float y = std::max(a.y, yOf(a, peaks[b]));
          if (y < a.bottom() - 2.0f) g.fillRect(Rect(a.x + b * (bw + gap), y, bw, 1.5f));
        }
      }
      return;
    }
    // LED ladders: the trace colour, amber within 18 dB of the top, red within
    // 6, and the unlit segments faintly there.
    const float seg = a.h > 160.0f ? 4.0f : 3.0f, segGap = 1.0f, stepY = seg + segGap;
    const int rows = std::max(1, (int) (a.h / stepY));
    const float warn = yOf(a, highDb_ - 18.0f), over = yOf(a, highDb_ - 6.0f);
    const Colour amber = Colour::fromRGB(0xFFB02E), red = Colour::fromRGB(0xFF4D4D);
    for (int b = 0; b < count; ++b) {
      const float x = a.x + b * (bw + gap), top = yOf(a, vals[b]);
      const float pk = peakHold_ ? yOf(a, peaks[b]) : a.bottom() + 1.0f;
      for (int r = 0; r < rows; ++r) {
        const float y = a.bottom() - (r + 1) * stepY + segGap;
        const Colour zone = y < over ? red : y < warn ? amber : c;
        const bool lit = y + seg > top, isPeak = peakHold_ && y <= pk && y + stepY > pk;
        g.setColour(zone.withMultipliedAlpha(lit || isPeak ? 0.95f : 0.08f));
        g.fillRect(Rect(x, y, bw, seg));
      }
    }
  }

  void paintCurve(Graphics& g, const Rect& a, Colour c) {
    const float mid = a.y + a.h * 0.5f, scale = a.h * 0.5f * 0.9f / curveRange_;
    Path line, fill;
    const int steps = std::max(2, (int) (a.w / 2.0f));
    for (int s = 0; s <= steps; ++s) {
      const float x = a.x + a.w * s / steps;
      const double hz = lowHz_ * std::pow(highHz_ / lowHz_, (double) s / steps);
      double db = curve_(hz);
      if (!(db == db)) db = 0.0;
      const float y = std::max(a.y - 4.0f, std::min(a.bottom() + 4.0f, mid - (float) db * scale));
      if (s == 0) {
        line.moveTo(x, y);
        fill.moveTo(x, mid);
        fill.lineTo(x, y);
      } else {
        line.lineTo(x, y);
        fill.lineTo(x, y);
      }
    }
    fill.lineTo(a.right(), mid);
    fill.close();
    g.setColour(c.withMultipliedAlpha(0.1f));
    g.fillPath(fill);
    g.setColour(c.withMultipliedAlpha(0.95f));
    g.strokePath(line, 2.0f);
  }

  void paintLabels(Graphics& g, const Rect& a, const Rect& plot, LookAndFeel& laf) {
    if (a.w < 120.0f || a.h < 48.0f) return;
    // Not `small`: <rpcndr.h> defines it as a macro for char.
    const Font labelFont = font().withHeight(std::min(font().height(), 10.0f));
    g.setColour(laf.dimText());
    static const double wide[] = {20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000};
    static const double middle[] = {50, 100, 500, 1000, 5000, 10000};
    static const double narrow[] = {100, 1000, 10000};
    const double* marks = a.w >= 520.0f ? wide : a.w >= 300.0f ? middle : narrow;
    const int count = a.w >= 520.0f ? 10 : a.w >= 300.0f ? 6 : 3;
    for (int i = 0; i < count; ++i) {
      const double f = marks[i];
      if (f < lowHz_ || f > highHz_) continue;
      const std::string text = f >= 1000.0 ? std::to_string((int) (f / 1000.0)) + "k" : std::to_string((int) f);
      const float x = std::max(a.x + 3.0f, std::min(a.right() - 30.0f, xOf(a, f) + 3.0f));
      labelFont.drawIn(g, text, Rect(x, a.bottom() - labelFont.height() - 3.0f, 30.0f, labelFont.height() + 2.0f), Justify::Left);
    }
    const float span = highDb_ - lowDb_, base = span > 72.0f ? 12.0f : span > 36.0f ? 6.0f : 3.0f;
    const float step = plot.h < 110.0f ? base * 2.0f : base;
    for (float db = std::ceil(lowDb_ / step) * step; db <= highDb_; db += step) {
      const float y = yOf(plot, db);
      if (y < plot.y + 8.0f || y > plot.bottom() - 14.0f) continue;
      labelFont.drawIn(g, std::to_string((int) db), Rect(a.right() - 34.0f, y - labelFont.height() * 0.5f, 30.0f, labelFont.height()),
                   Justify::Right);
    }
  }

  SpectrumFeed* feedOut_ = nullptr;
  SpectrumFeed* feedIn_ = nullptr;
  double sampleRate_ = 48000.0;
  float rawOut_[kPoints], rawIn_[kPoints], out_[kPoints], in_[kPoints];
  float shownOut_[kPoints], shownIn_[kPoints], peak_[kPoints];
  double peakAge_[kPoints];
  bool hasInput_ = false, dirty_ = true;

  Style style_ = Style::Curve;
  float lowDb_ = -90.0f, highDb_ = 6.0f;
  double lowHz_ = 20.0, highHz_ = 20000.0;
  double smoothing_ = -1.0, tilt_ = 0.0, speed_ = 0.22, holdSeconds_ = 1.5;
  bool peakHold_ = false, showInput_ = true, fill_ = true, grid_ = true, labels_ = true;
  Colour colour_, inputColour_, background_;
  bool hasColour_ = false, hasInputColour_ = false, hasBackground_ = false;
  int bands_ = 0;
  std::function<double(double)> curve_;
  float curveRange_ = 18.0f;
  Colour curveColour_ = Colour(0, 0, 0, 0);
};

} // namespace gfx
} // namespace sonore
