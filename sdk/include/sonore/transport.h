// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: musical time.
//
// The host tells us the tempo, the position and whether it is playing on every
// block. A tempo-synced delay, an LFO locked to eighth notes, a gate that
// restarts on the bar: all of it needs this, and a plugin that ignores it is
// limited to knob-in-milliseconds designs.
//
// `TransportInfo` is filled by the format wrapper before process() and handed
// to the DSP; `NoteLength` converts a musical division into seconds at the
// current tempo, which is the calculation every synced effect gets slightly
// wrong when it writes its own.
#pragma once
#include <cmath>
#include <cstdint>
#include "audio.h"
#include "special.h"

namespace sonore {

/** What the host knows about musical time at the start of this block. */
struct TransportInfo {
  bool isPlaying = false;
  bool isRecording = false;
  bool isLooping = false;
  /** Beats per minute. Valid only when `hasTempo`. */
  double tempo = 120.0;
  bool hasTempo = false;
  /** Position in quarter notes from the song start. Valid when `hasBeats`. */
  double positionBeats = 0.0;
  bool hasBeats = false;
  /** Position in seconds from the song start. Valid when `hasSeconds`. */
  double positionSeconds = 0.0;
  bool hasSeconds = false;
  /** Quarter-note position where the current bar began. */
  double barStartBeats = 0.0;
  int32_t barNumber = 0;
  int timeSigNumerator = 4;
  int timeSigDenominator = 4;

  /** Seconds per quarter note. Falls back to 120 BPM when the host said
   *  nothing, so a synced effect still produces a musical result rather than a
   *  division by zero. */
  double secondsPerBeat() const {
    const double bpm = (hasTempo && tempo > 1.0) ? tempo : 120.0;
    return 60.0 / bpm;
  }

  /** How far into the current bar we are, 0..1. Useful for bar-synced shapes. */
  double barPhase() const {
    if (!hasBeats) return 0.0;
    const double beatsPerBar =
        (double) timeSigNumerator * (4.0 / (double) (timeSigDenominator > 0 ? timeSigDenominator : 4));
    if (beatsPerBar <= 0.0) return 0.0;
    const double into = positionBeats - barStartBeats;
    const double phase = into / beatsPerBar;
    return phase - std::floor(phase);
  }
};

/** A musical division. `Straight` is the plain note, `Dotted` is 1.5x,
 *  `Triplet` is 2/3: the three every synced control offers. */
enum class NoteFlavour { Straight, Dotted, Triplet };

/**
 * Musical note lengths in beats. A quarter note is 1 beat by definition, so
 * everything else scales from there. Kept as free functions rather than a table
 * so a generated plugin can ask for "1/16 dotted" without a lookup it has to
 * get right.
 */
inline double noteLengthInBeats(int denominator, NoteFlavour flavour = NoteFlavour::Straight) {
  // 1/4 == 1 beat; 1/8 == 0.5; 1/1 == 4.
  const double beats = 4.0 / (double) (denominator > 0 ? denominator : 4);
  switch (flavour) {
    case NoteFlavour::Dotted: return beats * 1.5;
    case NoteFlavour::Triplet: return beats * (2.0 / 3.0);
    case NoteFlavour::Straight:
    default: return beats;
  }
}

/** Seconds for a musical division at the transport's tempo. */
inline double noteLengthInSeconds(const TransportInfo& t, int denominator,
                                  NoteFlavour flavour = NoteFlavour::Straight) {
  return noteLengthInBeats(denominator, flavour) * t.secondsPerBeat();
}

/** Samples for a musical division: what a delay line actually needs. */
inline double noteLengthInSamples(const TransportInfo& t, double sampleRate, int denominator,
                                  NoteFlavour flavour = NoteFlavour::Straight) {
  return noteLengthInSeconds(t, denominator, flavour) * sampleRate;
}

/** Hertz for a division: what an LFO actually needs. */
inline double noteRateInHz(const TransportInfo& t, int denominator,
                           NoteFlavour flavour = NoteFlavour::Straight) {
  const double secs = noteLengthInSeconds(t, denominator, flavour);
  return secs > 1e-9 ? 1.0 / secs : 1.0;
}

/**
 * An LFO that stays locked to the host's timeline.
 *
 * Free-running phase drifts: stop and restart playback, or loop a bar, and a
 * plain oscillator is wherever it happened to be. This derives phase from the
 * transport's beat position instead, so the same bar always sounds the same,
 * which is the entire reason a producer syncs a modulation in the first place.
 * When the host isn't playing (or reports no beats) it free-runs, so the plugin
 * still moves while auditioning.
 */
class SyncedLfo {
public:
  void setSampleRate(double sr) { sampleRate_ = sr > 1.0 ? sr : 48000.0; }

  /** Call once per block. `beatsPerCycle` comes from noteLengthInBeats(). */
  void update(const TransportInfo& t, double beatsPerCycle) {
    beatsPerCycle_ = beatsPerCycle > 1e-6 ? beatsPerCycle : 1.0;
    locked_ = t.hasBeats && t.isPlaying;
    if (locked_) {
      const double cycles = t.positionBeats / beatsPerCycle_;
      phase_ = cycles - std::floor(cycles);
    }
    const double hz = 1.0 / (beatsPerCycle_ * t.secondsPerBeat());
    increment_ = hz / sampleRate_;
  }

  /** Advance one sample and return the phase, 0..1. */
  inline float nextPhase() {
    const float p = (float) phase_;
    phase_ += increment_;
    if (phase_ >= 1.0) phase_ -= 1.0;
    return p;
  }

  // ── Shapes ──────────────────────────────────────────────────
  //
  // ALL of them run -1..1. That is worth stating because it did not use to be
  // true: sine was bipolar and triangle ran 0..1, so a plugin offering a
  // shape switch changed the modulation's depth AND its centre when the user
  // moved it -- a vibrato that jumped up an octave on the way past triangle.
  // Nothing had called either of them, which is the only reason it was safe
  // to make them agree.
  //
  // Depth and offset belong to the caller: `centre + depth * lfo.nextSine()`
  // says what it does, where a shape that quietly arrives pre-offset does
  // not. For the 0..1 form a modulation target usually wants, unipolar().

  /** Turn any shape into 0..1, for a target that has no negative half. */
  static inline float unipolar(float bipolar) { return 0.5f * (bipolar + 1.0f); }

  /** The phase is already in turns, so the polynomial sine takes it as is:
   *  one call per sample, no multiply by 2*pi, no library sine. */
  inline float nextSine() { return fastmath::sinTurns(nextPhase()); }

  inline float nextTriangle() {
    const float p = nextPhase();
    // Up from -1 to +1 over the first half, back down over the second.
    return p < 0.5f ? (p * 4.0f - 1.0f) : (3.0f - p * 4.0f);
  }

  /** Rising sawtooth. The falling one is its negative, which is cheaper for a
   *  caller to write than for this to offer twice. */
  inline float nextSaw() { return nextPhase() * 2.0f - 1.0f; }

  /** Square, and the one shape here with a real caveat: it steps. Modulating
   *  an amplitude or a filter cutoff with it clicks unless the target is
   *  smoothed, which is the caller's decision and not something an LFO can
   *  make on their behalf. */
  inline float nextSquare() { return nextPhase() < 0.5f ? 1.0f : -1.0f; }

  bool isLocked() const { return locked_; }
  void reset() { phase_ = 0.0; }

private:
  double sampleRate_ = 48000.0;
  double phase_ = 0.0;
  double increment_ = 0.0;
  double beatsPerCycle_ = 1.0;
  bool locked_ = false;
};

} // namespace sonore
