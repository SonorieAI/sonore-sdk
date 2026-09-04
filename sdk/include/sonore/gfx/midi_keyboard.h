// SPDX-License-Identifier: Apache-2.0
//
// An on-screen keyboard.
//
// ── Why an instrument needs one ─────────────────────────────────────────────
//
// A synth with no keyboard cannot be auditioned until a controller is plugged
// in, mapped and enabled. That is a real barrier at exactly the wrong moment:
// the first thirty seconds someone spends with a plugin, deciding whether it is
// worth more than thirty seconds. Every soft synth ships one for this reason.
//
// It also gives the editor somewhere to SHOW what the DSP is doing. A key lit
// while a voice is sounding is the cheapest possible answer to "is it even
// receiving MIDI", which is the first question of every support thread.
//
// ── The geometry ────────────────────────────────────────────────────────────
//
// Twelve semitones per octave and seven white keys, so the black keys cannot be
// evenly spaced -- there is no black key between E and F or between B and C.
// The offsets below are the real ones from a piano, not a uniform division,
// because a keyboard with evenly spread black keys is wrong in a way anybody
// who plays notices instantly and nobody who does not can name.
#pragma once

#include <algorithm>
#include <functional>
#include <vector>

#include "widgets.h"

namespace sonore {
namespace gfx {

class MidiKeyboard : public Widget {
public:
  /** One element, not 128. A reader enumerating every key would take a minute
   *  to get past middle C, and the keys are not what somebody is navigating
   *  for -- they are for playing with a mouse, which a reader user is not
   *  doing. */
  AccessibleInfo accessibleInfo() const override {
    AccessibleInfo info = baseInfo(AccessibleRole::MusicalKeyboard);
    if (info.name.empty()) info.name = "Keyboard";
    return info;
  }

  MidiKeyboard() { setRange(48, 84); } // C3 to C6, three octaves

  /** Fired when a key is pressed or released with the mouse. Velocity is 0..1;
   *  the wrapper turns it into 1..127. */
  std::function<void(int note, float velocity)> onNoteOn;
  std::function<void(int note)> onNoteOff;

  /**
   * The range shown, as MIDI note numbers.
   *
   * Snapped OUTWARD to whole white keys at both ends: a keyboard that started
   * on a black key would draw one with no white key under it, hanging in
   * space.
   */
  void setRange(int lowestNote, int highestNote) {
    lowest_ = clampNote(lowestNote);
    highest_ = clampNote(highestNote);
    if (highest_ < lowest_) std::swap(lowest_, highest_);
    while (lowest_ > 0 && isBlack(lowest_)) --lowest_;
    while (highest_ < 127 && isBlack(highest_)) ++highest_;
    repaint();
  }

  int lowestNote() const { return lowest_; }
  int highestNote() const { return highest_; }

  /**
   * Light a key because the DSP is sounding it.
   *
   * Separate from the mouse entirely: notes arriving from the host must light
   * the same keys, and a keyboard that only lit what it was clicked on would
   * be showing the user their own input back.
   */
  void setNoteHeld(int note, bool held) {
    if (note < 0 || note > 127) return;
    if (held_[(size_t) note] == held) return;
    held_[(size_t) note] = held;
    repaint();
  }

  bool isNoteHeld(int note) const {
    return note >= 0 && note <= 127 && held_[(size_t) note];
  }

  void allNotesOff() {
    bool any = false;
    for (size_t i = 0; i < held_.size(); ++i) {
      if (held_[i]) any = true;
      held_[i] = false;
    }
    mouseNote_ = -1;
    if (any) repaint();
  }

  /** How wide one white key is. Everything else follows from it. */
  float whiteKeyWidth() const {
    const int whites = numWhiteKeys();
    return whites > 0 ? localBounds().w / (float) whites : 0.0f;
  }

  int numWhiteKeys() const {
    int n = 0;
    for (int note = lowest_; note <= highest_; ++note)
      if (!isBlack(note)) ++n;
    return n;
  }

  /** The rectangle a key occupies, in local coordinates. Empty for a note
   *  outside the range. */
  Rect boundsForNote(int note) const {
    if (note < lowest_ || note > highest_) return {};
    const Rect area = localBounds();
    const float w = whiteKeyWidth();
    if (w <= 0.0f) return {};

    if (!isBlack(note)) return {whiteIndexOf(note) * w, 0.0f, w, area.h};

    // A black key sits between the two white keys around it, but NOT centred:
    // on a real piano C# is left of the gap and D# is right of it, because
    // three black keys have to fit over two gaps in one group and two over one
    // in the other. Uniform spacing is the tell of a keyboard drawn by
    // somebody who did not look at one.
    const float blackW = w * kBlackWidth;
    const float leftWhite = (float) whiteIndexOf(note - 1) * w;
    return {leftWhite + w - blackW * kBlackOffsets[(size_t) (note % 12)], 0.0f, blackW,
            area.h * kBlackHeight};
  }

  /**
   * The note under a point, or -1.
   *
   * Black keys are tested FIRST because they are drawn on top and are physically
   * above the white ones. Testing white first gives a keyboard where the top
   * half of every black key plays the white note underneath it.
   */
  int noteAt(Point p) const {
    for (int note = lowest_; note <= highest_; ++note)
      if (isBlack(note) && boundsForNote(note).contains(p)) return note;
    for (int note = lowest_; note <= highest_; ++note)
      if (!isBlack(note) && boundsForNote(note).contains(p)) return note;
    return -1;
  }

  /** Velocity from how far DOWN the key the click landed, as every soft
   *  keyboard does: near the top is quiet, near the bottom is loud. A fixed
   *  velocity makes an instrument impossible to audition expressively. */
  float velocityAt(Point p, int note) const {
    const Rect key = boundsForNote(note);
    if (key.isEmpty() || key.h <= 0.0f) return 0.8f;
    const float f = (p.y - key.y) / key.h;
    const float clamped = f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
    return kMinVelocity + clamped * (1.0f - kMinVelocity);
  }

  static bool isBlack(int note) {
    switch (((note % 12) + 12) % 12) {
      case 1:
      case 3:
      case 6:
      case 8:
      case 10:
        return true;
      default:
        return false;
    }
  }

  // ── Mouse ────────────────────────────────────────────────────────────────

  void mouseDown(const MouseEvent& e) override {
    if (!isEnabled()) return;
    playFrom(e.position);
  }

  void mouseDrag(const MouseEvent& e) override {
    if (!isEnabled()) return;
    // Sliding across the keyboard is GLISSANDO: the old note stops and the new
    // one starts. Without it, dragging holds one note and the keyboard cannot
    // be played the way anybody actually plays one.
    const int note = noteAt(e.position);
    if (note == mouseNote_) return;
    releaseMouseNote();
    if (note >= 0) startNote(note, velocityAt(e.position, note));
  }

  void mouseUp(const MouseEvent&) override { releaseMouseNote(); }

  /** The pointer leaving with the button down ends the note. A keyboard that
   *  kept it sounding would leave a stuck note the user has no way to stop. */
  void mouseExit(const MouseEvent&) override { releaseMouseNote(); }

  void paint(Graphics& g) override {
    LookAndFeel& lf = lookAndFeel();
    const Rect area = localBounds();
    if (area.isEmpty()) return;

    // Whites first, then blacks over them -- which is the drawing order AND
    // the physical arrangement.
    for (int note = lowest_; note <= highest_; ++note)
      if (!isBlack(note))
        lf.drawMidiKeyboardKey(g, boundsForNote(note), note, false, isNoteHeld(note),
                               note == hoverNote_);
    for (int note = lowest_; note <= highest_; ++note)
      if (isBlack(note))
        lf.drawMidiKeyboardKey(g, boundsForNote(note), note, true, isNoteHeld(note),
                               note == hoverNote_);
  }

  void mouseMove(const MouseEvent& e) override {
    const int note = noteAt(e.position);
    if (note == hoverNote_) return;
    hoverNote_ = note;
    repaint();
  }

  static constexpr float kBlackWidth = 0.62f;  // of a white key
  static constexpr float kBlackHeight = 0.62f; // of the full height
  static constexpr float kMinVelocity = 0.25f;

private:
  static int clampNote(int n) { return n < 0 ? 0 : (n > 127 ? 127 : n); }

  /** How many white keys before this one, from the bottom of the range. */
  int whiteIndexOf(int note) const {
    int n = 0;
    for (int i = lowest_; i < note; ++i)
      if (!isBlack(i)) ++n;
    return n;
  }

  void playFrom(Point p) {
    const int note = noteAt(p);
    if (note < 0) return;
    releaseMouseNote();
    startNote(note, velocityAt(p, note));
  }

  void startNote(int note, float velocity) {
    mouseNote_ = note;
    setNoteHeld(note, true);
    if (onNoteOn) onNoteOn(note, velocity);
  }

  void releaseMouseNote() {
    if (mouseNote_ < 0) return;
    const int note = mouseNote_;
    mouseNote_ = -1;
    // The HOST may also be holding this note -- somebody playing along on a
    // controller. Only the mouse's own light is taken away here; a note the
    // DSP is still sounding stays lit because setNoteHeld owns that.
    setNoteHeld(note, false);
    if (onNoteOff) onNoteOff(note);
  }

  /** Where each black key sits relative to the white key boundary, as a
   *  fraction of a black key's own width. From a real piano: the three-key
   *  group and the two-key group are spaced differently, and this is what makes
   *  the difference visible. */
  static constexpr float kBlackOffsets[12] = {
      0.0f,  // C
      0.65f, // C#
      0.0f,  // D
      0.35f, // D#
      0.0f,  // E
      0.0f,  // F
      0.70f, // F#
      0.0f,  // G
      0.50f, // G#
      0.0f,  // A
      0.30f, // A#
      0.0f,  // B
  };

  int lowest_ = 48, highest_ = 84;
  int mouseNote_ = -1;
  int hoverNote_ = -1;
  std::vector<bool> held_ = std::vector<bool>(128, false);
};

} // namespace gfx
} // namespace sonore
