// SPDX-License-Identifier: Apache-2.0
//
// An on-screen keyboard for MPE: every sounding note drawn as a circle that
// slides sideways with its pitch bend, rises with its timbre and swells with
// its pressure -- and a mouse that plays it the same way.
//
// ── Why a second keyboard ───────────────────────────────────────────────────
//
// midi_keyboard.h answers "is it receiving notes". For an MPE instrument
// that is a quarter of the question. The sound of a note on a Seaboard or a
// LinnStrument is in what each finger does AFTER it lands -- a slide of a
// whole tone, a press that opens the filter -- and a lit key cannot show any
// of it. A user checking that their controller's slides are reaching the
// plugin, or that the bend range agrees on both sides (the most common MPE
// setup fault there is), needs to SEE the slide.
//
// So this is MidiKeyboard's geometry and look, with the notes drawn over it:
//
//   x        the note's pitch: its key plus its own bend plus its zone's
//            master bend, placed between key centres so a slide moves
//            smoothly across white and black keys alike
//   y        its timbre (CC 74): bright at the top, dark at the bottom
//   radius   its pressure
//
// ── Driven by the SDK's own MPE decoding ────────────────────────────────────
//
// processMidi() reads the same channel-per-note MIDI the DSP gets, through
// MpeDecoder -- so zones from the MPE Configuration Message, bend ranges from
// RPN 0, and a master channel's zone-wide bend are understood exactly as the
// DSP understands them, by the same code. A host speaking per-note
// expression natively (CLAP, VST3) hands over a NoteExpressionBuffer instead,
// through applyExpression().
//
// Per channel it also keeps the LAST bend, pressure and timbre seen, because
// MPE sends a note's starting expression BEFORE its note-on: a note begins
// where its channel already is, and a decoder that only tracks sounding
// notes would draw every note starting from the centre.
//
// ── The mouse ───────────────────────────────────────────────────────────────
//
// A mouse cannot slide and press at once the way a finger does, but it has
// two axes. Pressing plays a note on a free member channel of the zone;
// dragging sideways bends it, measured from where the button went down (a
// click off a key's centre must not start bent); the vertical position sets
// pressure -- or timbre, setVerticalGesture() -- with the bottom of the key
// strongest, the way MidiKeyboard's velocity already works. What comes out is
// real MPE on onMpeMessage: the bend, CC 74 and pressure first, then the
// note-on, as the MPE specification orders them, so a synth hears the note
// begin already shaped.
//
// UI thread only, like every widget: the plugin forwards what its DSP
// received to it on the UI's own clock.
#pragma once

#include <cmath>
#include <functional>

#include "../audio.h"
#include "midi_keyboard.h"

namespace sonore {
namespace gfx {

/**
 * A keyboard that draws each sounding MPE note as a circle moved by its bend,
 * timbre and pressure, and plays MPE from the mouse -- the reference's
 * MPEKeyboardComponent. The file comment explains the drawing, the decoding
 * and the mouse gesture.
 *
 * Feed it what the DSP received with processMidi() (channel-per-note MIDI) or
 * applyExpression() (a host's per-note expression), on the UI thread: the
 * owner carries the audio thread's events across, as for MidiKeyboard. Mouse
 * notes come out on onMpeMessage when it is set, and on MidiKeyboard's
 * onNoteOn / onNoteOff otherwise. It holds at most kMaxNotes notes; past
 * that the oldest is dropped.
 */
class MPEKeyboardComponent : public MidiKeyboard {
public:
  /** One sounding note as drawn: its channel and key, and its expression
   *  now. Read through getNote() / findNote(). */
  struct Note {
    int channel = 0; ///< 0-based
    int key = 60;    ///< the MIDI note number
    float velocity = 0.0f; ///< 0..1
    float bend = 0.0f;     ///< semitones: this note's own, not its zone's
    float pressure = 0.0f; ///< 0..1
    float timbre = 0.5f;   ///< 0..1, CC 74
    bool fromMouse = false; ///< played here with the mouse, not received
  };

  /** More notes than fingers, with room for a host's chords on top. */
  static constexpr int kMaxNotes = 32;

  /** What the mouse's vertical position controls. */
  enum class VerticalGesture { Pressure, /**< lower down a key presses harder */ Timbre /**< higher up a key is brighter: a higher CC 74 */ };

  /** No notes; mouse notes go to a lower zone, master on channel 1 and the
   *  other fifteen for notes, until setZone. */
  MPEKeyboardComponent() {
    // MPE's usual layout: a lower zone, master on channel 1, the other
    // fifteen channels for notes. Mouse notes go there until setZone().
    MpeZone lower;
    lower.active = true;
    lower.isLowerZone = true;
    lower.masterChannel = 0;
    lower.numMemberChannels = 15;
    zone_ = lower;
    clearChannelState();
  }

  AccessibleInfo accessibleInfo() const override {
    AccessibleInfo info = MidiKeyboard::accessibleInfo();
    if (accessibleName().empty()) info.name = "MPE keyboard";
    return info;
  }

  // ── Configuration ──────────────────────────────────────────────────────

  /** The zone mouse notes are played in. An inactive zone falls back to
   *  every channel but the first. */
  void setZone(const MpeZone& zone) { zone_ = zone; }
  /** The zone mouse notes are played in. */
  const MpeZone& zone() const { return zone_; }

  /** The per-note bend range, in semitones, for both directions: what the
   *  mouse encodes with, and what incoming bends are read with until the
   *  controller announces its own through RPN 0. */
  void setPitchBendRange(float semitones) {
    bendRange_ = semitones > 0.0f ? semitones : 0.0f;
    decoder_.setPitchBendRange(bendRange_);
  }
  /** The member channels' bend range in semitones: the last setPitchBendRange,
   *  or what a controller announced through RPN 0 since. */
  float pitchBendRange() const { return decoder_.pitchBendRange(); }

  /** What dragging up and down a key controls for a mouse note. */
  void setVerticalGesture(VerticalGesture g) { gesture_ = g; }
  /** Pressure unless setVerticalGesture said otherwise. */
  VerticalGesture verticalGesture() const { return gesture_; }

  /** The colour the notes' circles are drawn in. */
  void setNoteColour(Colour c) {
    noteColour_ = c;
    repaint();
  }

  // ── Driving it ─────────────────────────────────────────────────────────

  /** One block's worth of MIDI, in order. */
  void processMidi(const MidiBuffer& midi) {
    for (const auto& e : midi) processMidiMessage(e.getMessage());
  }

  /** One message: a channel message updates the notes drawn and the
   *  channels' expression; anything else is ignored. Only the drawing
   *  changes -- nothing is sent anywhere. */
  void processMidiMessage(const MidiMessage& m) {
    const int status = m.getRawStatus();
    if (!isChannelMessage(status)) return;
    const int ch = status & 0x0F;

    // The decoder sees everything first: it follows RPN 0 and the MPE
    // Configuration Message, which may arrive as ordinary controllers.
    scratch_.clear();
    scratch_.addEvent(m, 0);
    expressions_.clear();
    decoder_.process(scratch_, expressions_);
    const bool master = isMasterChannel(ch);

    if (m.isPitchWheel()) {
      const float range = master ? decoder_.masterBendRange() : decoder_.pitchBendRange();
      channelBend_[ch] = ((float) m.getPitchWheelValue() - 8192.0f) / 8192.0f * range;
    } else if (m.isChannelPressure()) {
      channelPressure_[ch] = (float) m.getChannelPressureValue() / 127.0f;
    } else if (m.isController() && m.getControllerNumber() == 74) {
      channelTimbre_[ch] = (float) m.getControllerValue() / 127.0f;
    }

    if (m.isNoteOn()) {
      noteOn(ch, m.getNoteNumber(), m.getFloatVelocity());
      return;
    }
    if (m.isNoteOff()) {
      noteOff(ch, m.getNoteNumber());
      return;
    }
    if (m.isController() && (m.getControllerNumber() == 120 || m.getControllerNumber() == 123)) {
      // All notes off: on a master channel it means the whole zone.
      for (int i = numNotes_ - 1; i >= 0; --i)
        if (notes_[i].channel == ch || (master && zoneOf(notes_[i].channel) == zoneOf(ch)))
          removeAt(i);
      repaint();
      return;
    }
    if (master) {
      // Zone-wide: drawn through masterBendFor(), not copied into each note,
      // so a note's own bend and its zone's stay separate and both show.
      repaint();
      return;
    }
    // A note's bend here is its OWN, with the zone's added when it is drawn;
    // the decoder's tuning already holds both, as a DSP needs it.
    for (const auto& e : expressions_)
      setNoteExpression(e.channel, e.key, e.expression,
                        e.expression == kExprTuning ? channelBend_[e.channel] : e.value);
  }

  /** Expression a host delivered per note (CLAP, VST3). Tuning, pressure and
   *  brightness are drawn; the others are not something this shows. */
  void applyExpression(const NoteExpressionBuffer& expressions) {
    for (const auto& e : expressions) {
      if (e.key < 0) {
        // "Every note": the CLAP wildcard.
        for (int i = 0; i < numNotes_; ++i)
          if (e.channel < 0 || notes_[i].channel == e.channel)
            setNoteExpression(notes_[i].channel, notes_[i].key, e.expression, e.value);
        continue;
      }
      if (e.channel < 0) {
        for (int i = 0; i < numNotes_; ++i)
          if (notes_[i].key == e.key)
            setNoteExpression(notes_[i].channel, notes_[i].key, e.expression, e.value);
        continue;
      }
      setNoteExpression(e.channel, e.key, e.expression, e.value);
    }
  }

  /** A note starts, with its channel's current expression. A second note-on
   *  for the same channel and key restarts that note rather than adding a
   *  copy -- which is also what keeps a mouse note the host echoes back from
   *  being drawn twice. */
  void noteOn(int channel, int key, float velocity) {
    if (channel < 0 || channel > 15 || key < 0 || key > 127) return;
    Note* n = find(channel, key);
    if (!n) {
      if (numNotes_ >= kMaxNotes) removeAt(0); // the oldest gives way
      n = &notes_[numNotes_++];
      n->fromMouse = false;
    }
    n->channel = channel;
    n->key = key;
    n->velocity = clamp01(velocity);
    n->bend = channelBend_[channel];
    n->pressure = channelPressure_[channel];
    n->timbre = channelTimbre_[channel];
    channelUsed_[channel] = ++useClock_;
    repaint();
  }

  /** Stop drawing the note on 0-based `channel` with `key`; nothing if there
   *  is none. Drawing only, like noteOn. */
  void noteOff(int channel, int key) {
    for (int i = 0; i < numNotes_; ++i)
      if (notes_[i].channel == channel && notes_[i].key == key) {
        removeAt(i);
        repaint();
        return;
      }
  }

  /** One expression value for one note, in NoteExpression's units: tuning in
   *  semitones, pressure and brightness 0..1. */
  void setNoteExpression(int channel, int key, int expression, float value) {
    Note* n = find(channel, key);
    if (!n) return;
    switch (expression) {
      case kExprTuning: n->bend = value; break;
      case kExprPressure: n->pressure = clamp01(value); break;
      case kExprBrightness: n->timbre = clamp01(value); break;
      default: return;
    }
    repaint();
  }

  /** Forget every note and every channel's expression. The zones and bend
   *  ranges a controller announced are configuration, not notes, and stay. */
  void clearNotes() {
    releaseMouse(); // a note the mouse is holding is ended, not orphaned in the synth
    numNotes_ = 0;
    clearChannelState();
    repaint();
  }

  /** How many notes are drawn now, at most kMaxNotes. */
  int numNotes() const { return numNotes_; }
  /** Note `index`, oldest first, for 0 <= index < numNotes(). */
  const Note& getNote(int index) const {
    return notes_[index < 0 ? 0 : (index >= kMaxNotes ? kMaxNotes - 1 : index)];
  }
  /** The note on 0-based `channel` with `key`, or null. Valid until the notes
   *  next change. */
  const Note* findNote(int channel, int key) const {
    for (int i = 0; i < numNotes_; ++i)
      if (notes_[i].channel == channel && notes_[i].key == key) return &notes_[i];
    return nullptr;
  }

  /** The zone-wide bend, in semitones, that applies on top of this channel's
   *  notes: its zone's master channel's. Zero outside a zone. */
  float masterBendFor(int channel) const {
    const MpeZone& lower = decoder_.lowerZone();
    const MpeZone& upper = decoder_.upperZone();
    if (lower.hasMember(channel)) return channelBend_[lower.masterChannel];
    if (upper.hasMember(channel)) return channelBend_[upper.masterChannel];
    return 0.0f;
  }

  /** Where a note sounds, in semitones: key, own bend, zone bend. */
  float pitchOf(const Note& n) const { return (float) n.key + n.bend + masterBendFor(n.channel); }

  // ── Geometry ───────────────────────────────────────────────────────────

  /** The x of a pitch, in local coordinates. Whole pitches are key centres;
   *  between them it is a straight line from one centre to the next, so a
   *  slide moves at an even pace across each semitone whatever colour the
   *  keys are. Beyond the range it carries on at the average semitone width,
   *  so a note bent off the edge leaves the way it was going. */
  float xForPitch(float pitch) const {
    const int lo = lowestNote(), hi = highestNote();
    const float semitone = whiteKeyWidth() * 7.0f / 12.0f;
    if (!(pitch == pitch)) return keyCentreX(lo);
    if (pitch <= (float) lo) return keyCentreX(lo) - ((float) lo - pitch) * semitone;
    if (pitch >= (float) hi) return keyCentreX(hi) + (pitch - (float) hi) * semitone;
    const int k = (int) std::floor(pitch);
    const float f = pitch - (float) k;
    return keyCentreX(k) + (keyCentreX(k + 1) - keyCentreX(k)) * f;
  }

  /** The inverse: the pitch at an x. */
  float pitchForX(float x) const {
    const int lo = lowestNote(), hi = highestNote();
    const float semitone = whiteKeyWidth() * 7.0f / 12.0f;
    if (semitone <= 0.0f) return (float) lo;
    if (x <= keyCentreX(lo)) return (float) lo - (keyCentreX(lo) - x) / semitone;
    if (x >= keyCentreX(hi)) return (float) hi + (x - keyCentreX(hi)) / semitone;
    for (int k = lo; k < hi; ++k) {
      const float a = keyCentreX(k), b = keyCentreX(k + 1);
      if (x < b) return (float) k + (b > a ? (x - a) / (b - a) : 0.0f);
    }
    return (float) hi;
  }

  /** The circle a note is drawn as. */
  Rect noteCircle(const Note& n) const {
    const Rect area = localBounds();
    const float maxR = maxRadius();
    const float r = maxR * (0.3f + 0.7f * clamp01(n.pressure));
    const float inset = maxR < area.h * 0.5f ? maxR : area.h * 0.5f;
    const float cy = inset + (1.0f - clamp01(n.timbre)) * (area.h - 2.0f * inset);
    const float cx = xForPitch(pitchOf(n));
    return {cx - r, cy - r, 2.0f * r, 2.0f * r};
  }

  // ── The mouse ──────────────────────────────────────────────────────────

  /**
   * Everything the mouse plays, as MPE: pitch bend, CC 74, channel pressure
   * and note-on/off on a member channel. When this is set it carries
   * everything and MidiKeyboard's onNoteOn/onNoteOff stay silent -- a host
   * wired to both would otherwise hear each note twice. Left empty, those
   * two fire instead, so a plain instrument still plays.
   */
  std::function<void(const MidiMessage&)> onMpeMessage;

  void mouseDown(const MouseEvent& e) override {
    if (!isEnabled()) return;
    const int key = MidiKeyboard::noteAt(e.position);
    if (key < 0) return;
    releaseMouse();
    const int channel = chooseChannel();
    const float velocity = velocityAt(e.position, key);
    const float vertical = verticalAt(e.position);
    const float pressure = gesture_ == VerticalGesture::Pressure ? vertical : velocity;
    const float timbre = gesture_ == VerticalGesture::Timbre ? 1.0f - vertical : 0.5f;

    downX_ = e.position.x;
    sentBend_ = 8192;
    sentPressure_ = toSeven(pressure);
    sentTimbre_ = toSeven(timbre);
    // Expression first, then the note: the note must begin already shaped.
    emit(MidiMessage::pitchBend(channel, sentBend_));
    emit(MidiMessage::controlChange(channel, 74, sentTimbre_));
    emit(MidiMessage::channelPressure(channel, sentPressure_));
    channelBend_[channel] = 0.0f;
    channelPressure_[channel] = pressure;
    channelTimbre_[channel] = timbre;
    noteOn(channel, key, velocity);
    if (Note* n = find(channel, key)) n->fromMouse = true;
    mouseChannel_ = channel;
    mouseKey_ = key;
    mouseIndexValid_ = true;
    const int v = (int) std::lround(velocity * 127.0f);
    if (onMpeMessage) emit(MidiMessage::noteOn(channel, key, v < 1 ? 1 : v));
    else if (onNoteOn) onNoteOn(key, velocity);
  }

  void mouseDrag(const MouseEvent& e) override {
    if (!mouseIndexValid_) return;
    // The note may have left the list -- a host's all-notes-off, or evicted
    // by a flood of host notes -- while the synth still sounds it. The
    // gesture carries on regardless and the release still sends its
    // note-off; only the drawing has nothing to update.
    Note* n = find(mouseChannel_, mouseKey_);
    const float range = decoder_.pitchBendRange();
    float bend = pitchForX(e.position.x) - pitchForX(downX_);
    if (bend > range) bend = range;
    if (bend < -range) bend = -range;
    const float vertical = verticalAt(e.position);

    if (n) n->bend = bend;
    channelBend_[mouseChannel_] = bend;
    const int bend14 = range > 0.0f ? clamp14((int) std::lround(8192.0f + bend / range * 8192.0f)) : 8192;
    if (bend14 != sentBend_) {
      sentBend_ = bend14;
      emit(MidiMessage::pitchBend(mouseChannel_, bend14));
    }
    if (gesture_ == VerticalGesture::Pressure) {
      if (n) n->pressure = vertical;
      channelPressure_[mouseChannel_] = vertical;
      const int p = toSeven(vertical);
      if (p != sentPressure_) {
        sentPressure_ = p;
        emit(MidiMessage::channelPressure(mouseChannel_, p));
      }
    } else {
      if (n) n->timbre = 1.0f - vertical;
      channelTimbre_[mouseChannel_] = 1.0f - vertical;
      const int t = toSeven(1.0f - vertical);
      if (t != sentTimbre_) {
        sentTimbre_ = t;
        emit(MidiMessage::controlChange(mouseChannel_, 74, t));
      }
    }
    repaint();
  }

  void mouseUp(const MouseEvent&) override { releaseMouse(); }

  /** As MidiKeyboard: leaving with the button down ends the note, or it
   *  would sound until the editor closed. */
  void mouseExit(const MouseEvent& e) override {
    releaseMouse();
    MidiKeyboard::mouseExit(e);
  }

  // ── Drawing ────────────────────────────────────────────────────────────

  void paint(Graphics& g) override {
    LookAndFeel& lf = lookAndFeel();
    const Rect area = localBounds();
    if (area.isEmpty()) return;
    const int lo = lowestNote(), hi = highestNote();
    for (int pass = 0; pass < 2; ++pass)
      for (int note = lo; note <= hi; ++note)
        if (isBlack(note) == (pass == 1))
          lf.drawMidiKeyboardKey(g, boundsForNote(note), note, pass == 1, keyLit(note),
                                 note == hoveredNote());
    // Notes over the keys, oldest first so the newest is on top.
    for (int i = 0; i < numNotes_; ++i) drawNote(g, notes_[i], noteCircle(notes_[i]));
  }

protected:
  /** One note. Override to draw notes differently; the circle is already
   *  placed and sized from the note's pitch, timbre and pressure. */
  virtual void drawNote(Graphics& g, const Note& n, const Rect& circle) {
    g.setColour(noteColour_.withMultipliedAlpha(0.55f + 0.45f * clamp01(n.velocity)));
    g.fillEllipse(circle);
    g.setColour(noteColour_.darkened(0.35f));
    g.drawEllipse(circle, 1.5f);
  }

private:
  static float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : (v == v ? v : 0.0f)); }
  static int clamp14(int v) { return v < 0 ? 0 : (v > 16383 ? 16383 : v); }
  static int toSeven(float v) { return (int) std::lround(clamp01(v) * 127.0f); }

  float keyCentreX(int key) const { return boundsForNote(key).centre().x; }

  /** As large as fits a white key, and no taller than a third of the
   *  keyboard, so a fully pressed note does not hide its neighbours. */
  float maxRadius() const {
    const float byWidth = whiteKeyWidth() * 0.45f;
    const float byHeight = localBounds().h * 0.3f;
    return byWidth < byHeight ? byWidth : byHeight;
  }

  float verticalAt(Point p) const {
    const float h = localBounds().h;
    return h > 0.0f ? clamp01(p.y / h) : 0.5f;
  }

  bool keyLit(int key) const {
    if (isNoteHeld(key)) return true;
    for (int i = 0; i < numNotes_; ++i)
      if (notes_[i].key == key) return true;
    return false;
  }

  bool isMasterChannel(int ch) const {
    return decoder_.lowerZone().isMaster(ch) || decoder_.upperZone().isMaster(ch);
  }

  /** 1 for the lower zone, 2 for the upper, 0 for neither. */
  int zoneOf(int ch) const {
    const MpeZone& lower = decoder_.lowerZone();
    const MpeZone& upper = decoder_.upperZone();
    if (lower.isMaster(ch) || lower.hasMember(ch)) return 1;
    if (upper.isMaster(ch) || upper.hasMember(ch)) return 2;
    return 0;
  }

  Note* find(int channel, int key) {
    for (int i = 0; i < numNotes_; ++i)
      if (notes_[i].channel == channel && notes_[i].key == key) return &notes_[i];
    return nullptr;
  }

  void removeAt(int index) {
    for (int i = index; i + 1 < numNotes_; ++i) notes_[i] = notes_[i + 1];
    --numNotes_;
  }

  /**
   * A member channel for a new mouse note: one with nothing sounding on it,
   * and of those the one used longest ago -- MPE's own advice, because a
   * note's release tail is still using its channel's expression for a while
   * after the note-off. With every channel busy, the least recently used is
   * shared.
   */
  int chooseChannel() const {
    int best = -1, busyBest = -1;
    for (int ch = 0; ch < 16; ++ch) {
      const bool member = zone_.active ? zone_.hasMember(ch) : ch != 0;
      if (!member) continue;
      bool busy = false;
      for (int i = 0; i < numNotes_ && !busy; ++i) busy = notes_[i].channel == ch;
      int& pick = busy ? busyBest : best;
      if (pick < 0 || channelUsed_[ch] < channelUsed_[pick]) pick = ch;
    }
    return best >= 0 ? best : (busyBest >= 0 ? busyBest : 1);
  }

  void releaseMouse() {
    if (!mouseIndexValid_) return;
    mouseIndexValid_ = false;
    const int channel = mouseChannel_, key = mouseKey_;
    noteOff(channel, key);
    if (onMpeMessage) emit(MidiMessage::noteOff(channel, key));
    else if (onNoteOff) onNoteOff(key);
  }

  void emit(const MidiMessage& m) {
    if (onMpeMessage) onMpeMessage(m);
  }

  void clearChannelState() {
    for (int ch = 0; ch < 16; ++ch) {
      channelBend_[ch] = 0.0f;
      channelPressure_[ch] = 0.0f;
      channelTimbre_[ch] = 0.5f; // CC 74's neutral 64
      channelUsed_[ch] = 0;
    }
  }

  Note notes_[kMaxNotes];
  int numNotes_ = 0;

  MpeDecoder decoder_;
  MidiBuffer scratch_;
  NoteExpressionBuffer expressions_;
  float channelBend_[16], channelPressure_[16], channelTimbre_[16];
  uint32_t channelUsed_[16];
  uint32_t useClock_ = 0;

  MpeZone zone_;
  float bendRange_ = 48.0f;
  VerticalGesture gesture_ = VerticalGesture::Pressure;
  Colour noteColour_ = Colour::fromRGB(0xF2994A);

  bool mouseIndexValid_ = false;
  int mouseChannel_ = 0, mouseKey_ = 0;
  float downX_ = 0.0f;
  int sentBend_ = 8192, sentPressure_ = 0, sentTimbre_ = 64;
};

} // namespace gfx
} // namespace sonore
