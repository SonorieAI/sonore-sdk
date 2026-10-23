// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: the rest of the dynamics family.
//
// dsp.h has a Compressor, effects.h a Limiter and a NoiseGate, and for a long
// time that was all: every "make it punchier" or "tame the esses" was written
// from scratch, and a hand-written gain computer is where a knee comes out
// discontinuous or a release runs the wrong way. These are the other standard
// gain computers, each built on the same EnvFollower the compressor uses, so a
// plugin that pairs two of them gets one detector behaviour rather than two.
//
// The knee maths follows Giannoulis, Massberg & Reiss, "Digital Dynamic Range
// Compressor Design -- A Tutorial and Analysis" (JAES 2012): a quadratic that
// meets the hard curve with matching slope at both ends of the knee, so there
// is no corner for a level sitting exactly on the threshold to buzz against.
//
// Included by dsp.h.
#pragma once
#ifndef SONORE_DSP_H_INCLUDED
#error "dynamics.h is part of dsp.h and needs what it includes first: include <sonore/dsp.h> instead."
#endif
#include <cmath>
#include "audio.h"

namespace sonore {

// ── Expander ─────────────────────────────────────────────────────────────────

/**
 * Downward expander: below the threshold, every dB the input drops the output
 * drops by `ratio` dB. A gate is the limiting case (ratio -> infinity) and the
 * NoiseGate in effects.h is the better tool for that; this is for the gentle
 * 1.5:1 -- 3:1 that pushes a drum room's bleed down without ever slamming shut.
 *
 * `range` caps the attenuation. A 2:1 expander with no range would take a
 * -80 dB fade to -140 dB; with a 40 dB range it stops where the noise floor
 * would have been anyway, and a reverb tail gets quieter without vanishing.
 */
class Expander {
public:
  /** At 48 kHz, its detector's 5 ms attack and 80 ms release set. They used
   *  to wait for setSampleRate(), setAttack() or setRelease(), and until
   *  then the detector followed the input sample by sample. */
  Expander() { setSampleRate(48000.0f); }
  /** The rate in Hz. Call it in prepare(), before processing: the
   *  detector's attack and release are worked out from it. No
   *  allocation. */
  void setSampleRate(float sr) {
    sr_ = sr;
    env_.setSampleRate(sr);
    env_.setTimes(attackMs_, releaseMs_);
    threshold_.setSampleRate(sr);
    range_.setSampleRate(sr);
    ratio_.setSampleRate(sr);
  }
  /** Threshold, range and ratio glide once audio runs (see Glide): moved in
   *  one sample they stepped the gain -- range by 7.5 times the largest
   *  natural step, ratio (1.5 -> 4) by 7.3, threshold by 5.5. */
  void setThreshold(float db) { thresholdDb_ = db; threshold_.set(db); }
  /** Below the threshold the output falls `ratio` dB for every dB the
   *  input falls: 2 by default, and anything under 1 is 1, no expansion. */
  void setRatio(float ratio) { ratio_.set(ratio >= 1.0f ? ratio : 1.0f); }
  /** Knee width in dB, centred on the threshold: 6 by default, 0 a hard
   *  knee, a negative width 0. Not glided: it takes effect at once. */
  void setKnee(float db) { kneeDb_ = db < 0.0f ? 0.0f : db; }
  /** Maximum attenuation in dB (positive number). */
  void setRange(float db) { rangeDb_ = db < 0.0f ? 0.0f : db; range_.set(rangeDb_); }
  /** The detector's attack in ms, 5 by default: how fast it follows a rising
   *  level, so how fast the expander opens. */
  void setAttack(float ms) { attackMs_ = ms; env_.setTimes(attackMs_, releaseMs_); }
  /** The detector's release in ms, 80 by default: how fast it follows a
   *  falling level, so how fast the attenuation sets in. */
  void setRelease(float ms) { releaseMs_ = ms; env_.setTimes(attackMs_, releaseMs_); }
  /** Clear the detector and land every glide on its target. The settings
   *  are kept. */
  void reset() {
    env_.reset();
    grDb_ = 0.0f;
    threshold_.snap();
    range_.snap();
    ratio_.snap();
  }

  /** The gain for this sample from a (possibly external) detector input. */
  inline float computeGain(float detector) {
    const float levelDb = gainToDb(env_.process(detector));
    const float rangeDb = range_.next();
    const float ratio = ratio_.next();
    const float over = levelDb - threshold_.next();
    const float half = kneeDb_ * 0.5f;
    float grDb = 0.0f;
    if (kneeDb_ > 0.0f && over > -half && over < half) {
      // Quadratic knee: zero with zero slope at +half, meeting the straight
      // line (ratio-1)·over with its slope at -half.
      const float t = over - half;
      grDb = (1.0f - ratio) * (t * t) / (2.0f * kneeDb_);
    } else if (over < 0.0f) {
      grDb = (ratio - 1.0f) * over; // over is negative: attenuation grows
    }
    if (grDb < -rangeDb) grDb = -rangeDb;
    grDb_ = grDb;
    return dbToGain(grDb);
  }
  /** [audio-thread] One sample, expanded by its own level. No allocation. */
  inline float process(float x) { return x * computeGain(x); }
  /** Gain reduction in dB, POSITIVE (6 means 6 dB quieter), which is what a
   *  meter reads and what every other dynamics class here reports. The signed
   *  gain change is `-gainReduction()`. */
  float gainReduction() const { return grDb_ < 0.0f ? -grDb_ : 0.0f; }

private:
  float sr_ = 48000.0f;
  float thresholdDb_ = -40.0f, kneeDb_ = 6.0f, rangeDb_ = 40.0f;
  float attackMs_ = 5.0f, releaseMs_ = 80.0f, grDb_ = 0.0f;
  EnvFollower env_;
  Glide threshold_{-40.0f, -40.0f}, range_{40.0f, 40.0f}, ratio_{2.0f, 2.0f};
};

// ── Upward compressor ────────────────────────────────────────────────────────

/**
 * Upward compression: below the threshold the signal is BROUGHT UP, by
 * (1 - 1/ratio) dB per dB it sits under. Where a downward compressor makes the
 * loud parts quieter, this makes the quiet parts louder -- the move behind
 * "bring up the room" and behind most "loudness" processors, and the thing a
 * parallel compressor is approximating with a mix knob.
 *
 * `maxGain` matters more here than range does on the expander. Uncapped, a
 * 4:1 upward compressor lifts a -100 dB noise floor by 75 dB, which is how a
 * fade-out ends in a roar. The default 20 dB cap is where broadcast tools stop.
 */
class UpwardCompressor {
public:
  /** At 48 kHz, its detector's 20 ms attack and 200 ms release set. They
   *  used to wait for setSampleRate(), setAttack() or setRelease(), and
   *  until then the detector followed the input sample by sample. */
  UpwardCompressor() { setSampleRate(48000.0f); }
  /** The rate in Hz. Call it in prepare(), before processing: the
   *  detector's attack and release are worked out from it. No
   *  allocation. */
  void setSampleRate(float sr) {
    sr_ = sr;
    env_.setSampleRate(sr);
    env_.setTimes(attackMs_, releaseMs_);
    threshold_.setSampleRate(sr);
    slope_.setSampleRate(sr);
    maxGain_.setSampleRate(sr);
  }
  /** Threshold, ratio and largest boost glide once audio runs (see Glide):
   *  moved in one sample, the ratio (1.5 -> 4) stepped the gain 5 times the
   *  largest natural step and the largest boost (3 -> 20 dB) 6 times. */
  void setThreshold(float db) { thresholdDb_ = db; threshold_.set(db); }
  /** Glides as its slope, 1 - 1/R, the boost per dB under. */
  void setRatio(float ratio) { slope_.set(1.0f - 1.0f / (ratio >= 1.0f ? ratio : 1.0f)); }
  /** Knee width in dB, centred on the threshold: 6 by default, 0 a hard
   *  knee, a negative width 0. Not glided: it takes effect at once. */
  void setKnee(float db) { kneeDb_ = db < 0.0f ? 0.0f : db; }
  /** Maximum boost in dB. */
  void setMaxGain(float db) { maxGain_.set(db >= 0.0f ? db : 0.0f); }
  /** The detector's attack in ms, 20 by default: how fast it follows a
   *  rising level, so how fast the boost is taken away. */
  void setAttack(float ms) { attackMs_ = ms; env_.setTimes(attackMs_, releaseMs_); }
  /** The detector's release in ms, 200 by default: how fast it follows a
   *  falling level, so how fast the boost comes in. */
  void setRelease(float ms) { releaseMs_ = ms; env_.setTimes(attackMs_, releaseMs_); }
  /** Clear the detector and land every glide on its target. The settings
   *  are kept. */
  void reset() {
    env_.reset();
    gainDb_ = 0.0f;
    threshold_.snap();
    slope_.snap();
    maxGain_.snap();
  }

  /** [audio-thread] The linear gain for this sample, 1 or more, from
   *  `detector`: the signal itself, or a side chain. Records gainChange().
   *  No allocation. */
  inline float computeGain(float detector) {
    const float levelDb = gainToDb(env_.process(detector));
    const float over = levelDb - threshold_.next();
    const float half = kneeDb_ * 0.5f;
    const float slope = slope_.next(); // boost per dB under, positive
    const float maxGainDb = maxGain_.next();
    float gainDb = 0.0f;
    if (kneeDb_ > 0.0f && over > -half && over < half) {
      const float t = over - half;
      gainDb = slope * (t * t) / (2.0f * kneeDb_);
    } else if (over < 0.0f) {
      gainDb = -slope * over;
    }
    if (gainDb > maxGainDb) gainDb = maxGainDb;
    gainDb_ = gainDb;
    return dbToGain(gainDb);
  }
  /** [audio-thread] One sample, raised by its own level. No allocation. */
  inline float process(float x) { return x * computeGain(x); }
  /** Last gain CHANGE in dB (positive = boosting). */
  float gainChange() const { return gainDb_; }

private:
  float sr_ = 48000.0f;
  float thresholdDb_ = -30.0f, kneeDb_ = 6.0f;
  float attackMs_ = 20.0f, releaseMs_ = 200.0f, gainDb_ = 0.0f;
  EnvFollower env_;
  Glide threshold_{-30.0f, -30.0f}, slope_{0.5f, 0.5f}, maxGain_{20.0f, 20.0f}; // 2:1, +20 dB
};

// ── Transient shaper ─────────────────────────────────────────────────────────

/**
 * Attack and sustain as two independent controls, with no threshold.
 *
 * The trick, from the SPL Transient Designer: two envelope followers that
 * differ only in ATTACK time disagree exactly while a transient is arriving
 * (the fast one is already up, the slow one still climbing), and two that
 * differ only in RELEASE disagree exactly while a note is dying (the slow one
 * is still up, the fast one already down). Each difference, in dB, is a
 * detector for one thing and nothing else, and it needs no threshold because
 * it is a RATIO -- a quiet hit and a loud hit produce the same difference.
 *
 * `attack` and `sustain` run -1..+1: +1 applies the full detected difference
 * as gain (a hit gets louder by however much it out-ran the slow follower),
 * -1 applies it inverted (the hit is pulled down to the slow follower's
 * level), 0 does nothing. Both are capped at +/-24 dB, because a difference
 * detector on a signal that starts from digital silence is unbounded.
 */
class TransientShaper {
public:
  /** At 48 kHz, the four followers' times set. They used to wait for
   *  setSampleRate(), and until then the followers followed the input
   *  instantly, agreed, and the shaper did nothing at any setting. */
  TransientShaper() { setSampleRate(48000.0f); }
  /** The rate in Hz. Call it in prepare(), before processing: the four
   *  followers' times are set here. No allocation. */
  void setSampleRate(float sr) {
    sr_ = sr;
    attackFast_.setSampleRate(sr);
    attackSlow_.setSampleRate(sr);
    sustainFast_.setSampleRate(sr);
    sustainSlow_.setSampleRate(sr);
    attackFast_.setTimes(0.05f, kAttackRelease);
    attackSlow_.setTimes(kSlowAttack, kAttackRelease);
    sustainFast_.setTimes(kSustainAttack, 30.0f);
    sustainSlow_.setTimes(kSustainAttack, 300.0f);
  }
  /** -1..+1 */
  void setAttack(float amount) { attack_ = clampf(amount, -1.0f, 1.0f); }
  /** -1..+1 */
  void setSustain(float amount) { sustain_ = clampf(amount, -1.0f, 1.0f); }
  /** Clear the four followers; the amounts are kept. */
  void reset() {
    attackFast_.reset(); attackSlow_.reset();
    sustainFast_.reset(); sustainSlow_.reset();
    gainDb_ = 0.0f;
  }

  /** [audio-thread] The linear gain for this sample from `detector` (the
   *  signal itself, or a side chain): the attack and sustain differences,
   *  in dB, each weighted by its amount, capped at +/-24 dB. Records
   *  gainDb(). No allocation. */
  inline float computeGain(float detector) {
    const float af = attackFast_.process(detector), as = attackSlow_.process(detector);
    const float sf = sustainFast_.process(detector), ss = sustainSlow_.process(detector);
    // Floored, so silence reads as a level rather than as -infinity, and the
    // differences stay finite while a note starts from nothing.
    const float attackDiff = gainToDbFloor(af) - gainToDbFloor(as);   // >= 0 on a hit
    const float sustainDiff = gainToDbFloor(ss) - gainToDbFloor(sf);  // >= 0 in a decay
    float g = attack_ * (attackDiff > 0.0f ? attackDiff : 0.0f) +
              sustain_ * (sustainDiff > 0.0f ? sustainDiff : 0.0f);
    gainDb_ = clampf(g, -kMaxDb, kMaxDb);
    return dbToGain(gainDb_);
  }
  /** [audio-thread] One sample, shaped by its own envelope. No allocation. */
  inline float process(float x) { return x * computeGain(x); }
  /** Last applied gain in dB. */
  float gainDb() const { return gainDb_; }

private:
  static constexpr float kMaxDb = 24.0f;
  static constexpr float kSlowAttack = 15.0f;     // ms: how long a "transient" is
  static constexpr float kAttackRelease = 100.0f; // ms, shared, so only attack differs
  static constexpr float kSustainAttack = 1.0f;   // ms, shared, so only release differs

  float sr_ = 48000.0f, attack_ = 0.0f, sustain_ = 0.0f, gainDb_ = 0.0f;
  EnvFollower attackFast_, attackSlow_, sustainFast_, sustainSlow_;
};

// ── De-esser ─────────────────────────────────────────────────────────────────

/**
 * Sibilance control: a compressor whose detector hears only one band.
 *
 * Two ways to apply the gain, and they are different tools. WIDEBAND ducks the
 * whole signal while an "s" is loud -- transparent on speech, because an "s"
 * carries little else, and it is what the classic hardware does. SPLIT ducks
 * only the band above the crossover, leaving the body of the voice untouched,
 * which is what you want on a vocal that is being de-essed hard.
 *
 * The detector is a bandpass a third of an octave wide around `frequency`,
 * so a bright guitar strum a fifth below it does not trip the compressor.
 */
class DeEsser {
public:
  /** Where the ducking is applied: Wideband ducks the whole signal while
   *  the band is loud, Split (the default) only the band above the
   *  crossover. */
  enum class Mode { Wideband, /**< the whole signal */ Split /**< the band above the crossover */ };

  /** The rate from `spec`, the detector and the split designed for it, and
   *  reset(). Call it before processing and whenever the rate changes. No
   *  allocation. */
  void prepare(const ProcessSpec& spec) {
    sr_ = (float) spec.sampleRate;
    detector_.setSampleRate(sr_);
    split_.setSampleRate(sr_);
    comp_.setSampleRate(sr_);
    comp_.setKnee(3.0f);
    comp_.setAttack(attackMs_);
    comp_.setRelease(releaseMs_);
    setFrequency(frequency_);
    reset();
  }
  /** Everything with memory, the crossover included: left out, a reset
   *  de-esser rang on with whatever the split filters last held. */
  void reset() {
    detector_.reset();
    comp_.reset();
    split_.reset();
  }

  /** Wideband or Split, from the next sample on. */
  void setMode(Mode m) { mode_ = m; }
  /** Centre of the sibilance band. 6-8 kHz for most voices; held under
   *  0.45 of the running rate, and asked for again at the next one. */
  void setFrequency(float hz) {
    // The frequency AS ASKED is kept, and the rate's ceiling applied to what
    // is designed. The clamped value was kept, so one prepare() at a low rate
    // lowered it for good: an 8 kHz band visited 11.025 kHz, came back to
    // 48 kHz at 4.96 kHz, and an 8 kHz "s" was ducked 4.3 dB instead of 14.
    frequency_ = hz;
    const float f = clampf(hz, 1000.0f, sr_ * 0.45f);
    // Q of 4.3 is a third of an octave: wide enough to catch every "s",
    // narrow enough to ignore what is not one.
    detector_.bandpass(f, 4.3f);
    // The split sits a little under the band, so the whole "s" is on the
    // side that gets ducked.
    split_.setCrossover(f * 0.7f);
  }
  /** The level in dB, measured in the sibilance band, above which the
   *  ducking starts: -18 by default. */
  void setThreshold(float db) { comp_.setThreshold(db); }
  /** How hard the band is ducked above the threshold, as a compression
   *  ratio: 4 by default, anything under 1 taken as 1. */
  void setRatio(float ratio) { comp_.setRatio(ratio); }
  /** How fast the ducking starts, in ms: 0.5 by default. */
  void setAttack(float ms) { attackMs_ = ms; comp_.setAttack(ms); }
  /** How fast it lets go, in ms: 40 by default. */
  void setRelease(float ms) { releaseMs_ = ms; comp_.setRelease(ms); }

  /** [audio-thread] One sample, de-essed. No allocation. */
  inline float process(float x) {
    const float gain = comp_.computeGain(detector_.process(x));
    if (mode_ == Mode::Wideband) return x * gain;
    float low, high;
    split_.process(x, low, high);
    return low + high * gain;
  }
  /** The last gain reduction in dB, positive while ducking. */
  float gainReduction() const { return comp_.gainReduction(); }

private:
  Mode mode_ = Mode::Split;
  float sr_ = 48000.0f, frequency_ = 6500.0f, attackMs_ = 0.5f, releaseMs_ = 40.0f;
  Biquad detector_;
  LinkwitzRiley split_;
  Compressor comp_;
};

} // namespace sonore
