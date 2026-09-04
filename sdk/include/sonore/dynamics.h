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
  void setSampleRate(float sr) {
    sr_ = sr;
    env_.setSampleRate(sr);
    env_.setTimes(attackMs_, releaseMs_);
  }
  void setThreshold(float db) { thresholdDb_ = db; }
  void setRatio(float ratio) { ratio_ = ratio < 1.0f ? 1.0f : ratio; }
  void setKnee(float db) { kneeDb_ = db < 0.0f ? 0.0f : db; }
  /** Maximum attenuation in dB (positive number). */
  void setRange(float db) { rangeDb_ = db < 0.0f ? 0.0f : db; }
  void setAttack(float ms) { attackMs_ = ms; env_.setTimes(attackMs_, releaseMs_); }
  void setRelease(float ms) { releaseMs_ = ms; env_.setTimes(attackMs_, releaseMs_); }
  void reset() { env_.reset(); grDb_ = 0.0f; }

  /** The gain for this sample from a (possibly external) detector input. */
  inline float computeGain(float detector) {
    const float levelDb = gainToDb(env_.process(detector));
    const float over = levelDb - thresholdDb_;
    const float half = kneeDb_ * 0.5f;
    float grDb = 0.0f;
    if (kneeDb_ > 0.0f && over > -half && over < half) {
      // Quadratic knee: zero with zero slope at +half, meeting the straight
      // line (ratio-1)·over with its slope at -half.
      const float t = over - half;
      grDb = (1.0f - ratio_) * (t * t) / (2.0f * kneeDb_);
    } else if (over < 0.0f) {
      grDb = (ratio_ - 1.0f) * over; // over is negative: attenuation grows
    }
    if (grDb < -rangeDb_) grDb = -rangeDb_;
    grDb_ = grDb;
    return dbToGain(grDb);
  }
  inline float process(float x) { return x * computeGain(x); }
  /** Last gain reduction in dB (negative = attenuating). */
  float gainReduction() const { return grDb_; }

private:
  float sr_ = 48000.0f;
  float thresholdDb_ = -40.0f, ratio_ = 2.0f, kneeDb_ = 6.0f, rangeDb_ = 40.0f;
  float attackMs_ = 5.0f, releaseMs_ = 80.0f, grDb_ = 0.0f;
  EnvFollower env_;
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
  void setSampleRate(float sr) {
    sr_ = sr;
    env_.setSampleRate(sr);
    env_.setTimes(attackMs_, releaseMs_);
  }
  void setThreshold(float db) { thresholdDb_ = db; }
  void setRatio(float ratio) { ratio_ = ratio < 1.0f ? 1.0f : ratio; }
  void setKnee(float db) { kneeDb_ = db < 0.0f ? 0.0f : db; }
  /** Maximum boost in dB. */
  void setMaxGain(float db) { maxGainDb_ = db < 0.0f ? 0.0f : db; }
  void setAttack(float ms) { attackMs_ = ms; env_.setTimes(attackMs_, releaseMs_); }
  void setRelease(float ms) { releaseMs_ = ms; env_.setTimes(attackMs_, releaseMs_); }
  void reset() { env_.reset(); gainDb_ = 0.0f; }

  inline float computeGain(float detector) {
    const float levelDb = gainToDb(env_.process(detector));
    const float over = levelDb - thresholdDb_;
    const float half = kneeDb_ * 0.5f;
    const float slope = 1.0f - 1.0f / ratio_; // boost per dB under, positive
    float gainDb = 0.0f;
    if (kneeDb_ > 0.0f && over > -half && over < half) {
      const float t = over - half;
      gainDb = slope * (t * t) / (2.0f * kneeDb_);
    } else if (over < 0.0f) {
      gainDb = -slope * over;
    }
    if (gainDb > maxGainDb_) gainDb = maxGainDb_;
    gainDb_ = gainDb;
    return dbToGain(gainDb);
  }
  inline float process(float x) { return x * computeGain(x); }
  /** Last gain CHANGE in dB (positive = boosting). */
  float gainChange() const { return gainDb_; }

private:
  float sr_ = 48000.0f;
  float thresholdDb_ = -30.0f, ratio_ = 2.0f, kneeDb_ = 6.0f, maxGainDb_ = 20.0f;
  float attackMs_ = 20.0f, releaseMs_ = 200.0f, gainDb_ = 0.0f;
  EnvFollower env_;
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
  void reset() {
    attackFast_.reset(); attackSlow_.reset();
    sustainFast_.reset(); sustainSlow_.reset();
    gainDb_ = 0.0f;
  }

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
  enum class Mode { Wideband, Split };

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
  void reset() { detector_.reset(); comp_.reset(); }

  void setMode(Mode m) { mode_ = m; }
  /** Centre of the sibilance band. 6-8 kHz for most voices. */
  void setFrequency(float hz) {
    frequency_ = clampf(hz, 1000.0f, sr_ * 0.45f);
    // Q of 4.3 is a third of an octave: wide enough to catch every "s",
    // narrow enough to ignore what is not one.
    detector_.bandpass(frequency_, 4.3f);
    // The split sits a little under the band, so the whole "s" is on the
    // side that gets ducked.
    split_.setCrossover(frequency_ * 0.7f);
  }
  void setThreshold(float db) { comp_.setThreshold(db); }
  void setRatio(float ratio) { comp_.setRatio(ratio); }
  void setAttack(float ms) { attackMs_ = ms; comp_.setAttack(ms); }
  void setRelease(float ms) { releaseMs_ = ms; comp_.setRelease(ms); }

  inline float process(float x) {
    const float gain = comp_.computeGain(detector_.process(x));
    if (mode_ == Mode::Wideband) return x * gain;
    float low, high;
    split_.process(x, low, high);
    return low + high * gain;
  }
  float gainReduction() const { return comp_.gainReduction(); }

private:
  Mode mode_ = Mode::Split;
  float sr_ = 48000.0f, frequency_ = 6500.0f, attackMs_ = 0.5f, releaseMs_ = 40.0f;
  Biquad detector_;
  LinkwitzRiley split_;
  Compressor comp_;
};

} // namespace sonore
