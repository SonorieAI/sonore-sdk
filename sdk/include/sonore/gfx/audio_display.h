// SPDX-License-Identifier: Apache-2.0
//
// Two ways to look at audio: a scope that scrolls, and a waveform that sits
// still.
//
// ── Why these are separate ──────────────────────────────────────────────────
//
// AudioVisualiser shows what is happening NOW, fed continuously from the audio
// thread. WaveformView shows a fixed thing already loaded -- a sample, a
// recorded take -- from an AudioThumbnail built once. They have almost nothing
// in common: one is a ring buffer with a real-time constraint, the other is a
// lookup with a playhead.
//
// ── Neither of them owns its data ───────────────────────────────────────────
//
// AudioVisualiser points at an AudioScopeBuffer and WaveformView points at an
// AudioThumbnail, and both are fed by something that outlives the editor. A
// scope that owned its buffer would start blank every time a window was opened
// instead of showing the last second of audio; a waveform that owned its
// thumbnail would copy megabytes the plugin already has.
//
// The real-time side lives in scope_buffer.h, with no UI anywhere near it.
#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>
#include <functional>

#include "../scope_buffer.h"
#include "../thumbnail.h"
#include "widgets.h"

namespace sonore {
namespace gfx {

/**
 * A scrolling oscilloscope.
 *
 * Each column is one BUCKET -- the minimum and maximum of however many samples
 * fell into it -- rather than one sample. A scope that plotted individual
 * samples at a pixel apart would alias badly: a 10 kHz tone at 48 kHz is under
 * five samples a cycle, and picking every Nth one draws a slow wobble that is
 * not in the audio. Min and max over the group draws the envelope, which is
 * what an analogue scope shows and what people expect.
 */
/**
 * A view onto an AudioScopeBuffer.
 *
 * The buffer is NOT owned, and that is the whole point of the split: it is fed
 * by the audio thread and has to exist whether or not an editor is open, so a
 * scope shows the last second of audio the moment somebody opens the window
 * rather than starting blank. Same arrangement as WaveformView and its
 * thumbnail.
 */
class AudioVisualiser : public Widget {
public:
  void setBuffer(const AudioScopeBuffer* buffer) {
    buffer_ = buffer;
    repaint();
  }

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
  void setThumbnail(const AudioThumbnail* thumb) {
    thumb_ = thumb;
    repaint();
  }

  const AudioThumbnail* thumbnail() const { return thumb_; }

  /** 0..1 along the waveform, or negative for none. */
  void setPlayhead(float position) {
    if (position == playhead_) return;
    playhead_ = position;
    repaint();
  }

  float playhead() const { return playhead_; }

  /** A highlighted region, 0..1. An empty one is drawn as nothing rather than
   *  as a zero-width line, which would look like a second playhead. */
  void setSelection(float from, float to) {
    selectionFrom_ = from < to ? from : to;
    selectionTo_ = from < to ? to : from;
    repaint();
  }

  float selectionFrom() const { return selectionFrom_; }
  float selectionTo() const { return selectionTo_; }
  bool hasSelection() const { return selectionTo_ > selectionFrom_; }

  /** Clicking scrubs, dragging selects. Fired with 0..1 positions. */
  std::function<void(float position)> onClick;
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

} // namespace gfx
} // namespace sonore
