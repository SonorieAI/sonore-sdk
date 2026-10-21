// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: the modulation family: an LFO, and the four effects that are
// nothing but one.
//
// A tremolo is a gain multiplied by an LFO; an auto-panner is a constant-
// power pan driven by one; a vibrato is a delay read at a moving point; a
// ring modulator is a multiply by a sine. None of that is hard, and every
// one of them has been written from scratch, slightly wrong, on demand: a
// tremolo whose depth changes its centre level, a pan that dips 3 dB in the
// middle, a vibrato read with the linear tap (a lowpass that sweeps with
// it), a ring modulator with the carrier leaking through. This is the
// family, once, on the rules the rest of the SDK already keeps.
//
// The LFO itself is the part worth having: free-running at a rate in hertz
// with the shapes people keep re-implementing -- sine, triangle, saw,
// square, sample-and-hold, and the SMOOTHED random that a sample-and-hold
// through a one-pole gives (the "drift" every analogue-modelled plugin
// offers). All shapes run -1..1, unipolar() maps them to 0..1, and a phase
// offset lets a stereo pair be a quarter turn apart. For an LFO locked to
// the host's bars, transport.h's SyncedLfo is the one to use; this one is
// for the rate knob.
//
// Included by dsp.h.
#pragma once
#ifndef SONORE_DSP_H_INCLUDED
#error "modulation.h is part of dsp.h and needs what it includes first: include <sonore/dsp.h> instead."
#endif
#include <cmath>
#include <cstdint>
#include "audio.h"
#include "random.h"

namespace sonore {

/**
 * A free-running low-frequency oscillator: sine, triangle, saw, square,
 * sample-and-hold and smoothed random, every shape -1..1 (see the file
 * comment; for one locked to the host's bars use transport.h's SyncedLfo).
 *
 * Call setSampleRate() in prepare() -- the rate in hertz is kept and worked
 * out again for the new sample rate, clamped to 0.45 of it -- and reset()
 * to start the cycle and the random sequence from the seed, so that two
 * renders match. next() is one sample. The setters are cheap enough to call
 * every block, and all of it runs on the audio thread. The random shapes
 * draw a new value each cycle from a Random seeded with setSeed().
 */
class Lfo {
public:
  /** The wave next() follows, every one -1..1. */
  enum class Shape {
    Sine,        ///< sine from 0, rising: the default
    Triangle,    ///< from -1 up to +1 at half a cycle and back
    Saw,         ///< a rising ramp, -1 to +1, then a drop
    Square,      ///< +1 for the first half of the cycle, -1 for the second
    SampleHold,  ///< a new random value at each cycle, held flat
    SmoothRandom ///< SampleHold through a one-pole at the rate: a smooth wander
  };

  /** Sample rate in Hz (1 or less means 48 kHz); the rate in hertz is kept
   *  and worked out again for it. */
  void setSampleRate(float sr) {
    sr_ = sr > 1.0f ? sr : 48000.0f;
    setRate(rateHz_);
  }
  /** Rate in Hz (1 by default), clamped to 0 .. 0.45 x the sample rate.
   *  The cycle carries on from its current phase. */
  void setRate(float hz) {
    rateHz_ = clampf(hz, 0.0f, sr_ * 0.45f);
    inc_ = rateHz_ / sr_;
    // The random shape's smoother closes at the rate itself, so the drift
    // is a wander and not a staircase with the corners rounded.
    smoothCoef_ = 1.0f - std::exp(-2.0f * kPi * (rateHz_ > 0.01f ? rateHz_ : 0.01f) / sr_);
  }
  /** The shape, from the next sample; Sine by default. */
  void setShape(Shape s) { shape_ = s; }
  /** Phase offset in turns: 0.25 puts a second LFO in quadrature. */
  void setPhaseOffset(float turns) { offset_ = turns - std::floor(turns); }
  /** Seed for the two random shapes. The sequence restarts from it now, and
   *  reset() returns to its start. */
  void setSeed(uint64_t seed) { random_.setSeed(seed); }
  /** Restart the cycle -- on a note-on, say. */
  void retrigger() { phase_ = 0.0f; }
  /** Phase 0 and the random sequence from the start of its seed, so two
   *  renders from a reset match. Unlike retrigger(), which keeps the
   *  random sequence where it is. */
  void reset() {
    phase_ = 0.0f;
    // From the start of the seed: drawing on, a sample-and-hold or random
    // LFO rendered twice from a reset differed by up to 1.33.
    random_.restart();
    held_ = random_.nextBipolar();
    smooth_ = held_;
    lastTurn_ = 0;
  }
  /** Where the cycle is, in turns 0..1, before the phase offset. */
  float phase() const { return phase_; }

  /** Turn any shape into 0..1. */
  static inline float unipolar(float bipolar) { return 0.5f * (bipolar + 1.0f); }

  /** The next value, -1..1. */
  inline float next() {
    float p = phase_ + offset_;
    p -= std::floor(p);
    // A new cycle starts when the phase wraps: that is when a held value is redrawn.
    const int turn = (int) std::floor(phase_ + offset_);
    if (turn != lastTurn_) {
      lastTurn_ = turn;
      held_ = random_.nextBipolar();
    }
    float y;
    switch (shape_) {
      case Shape::Sine: y = fastmath::sinTurns(p); break;
      case Shape::Triangle: y = p < 0.5f ? (p * 4.0f - 1.0f) : (3.0f - p * 4.0f); break;
      case Shape::Saw: y = p * 2.0f - 1.0f; break;
      case Shape::Square: y = p < 0.5f ? 1.0f : -1.0f; break;
      case Shape::SampleHold: y = held_; break;
      default:
        smooth_ += (held_ - smooth_) * smoothCoef_;
        y = flushDenormal(smooth_);
        break;
    }
    phase_ += inc_;
    if (phase_ >= 1.0f) { phase_ -= 1.0f; lastTurn_ -= 1; }
    return y;
  }

private:
  Random random_;
  Shape shape_ = Shape::Sine;
  float sr_ = 48000.0f, rateHz_ = 1.0f, inc_ = 1.0f / 48000.0f, phase_ = 0.0f, offset_ = 0.0f;
  float held_ = 0.0f, smooth_ = 0.0f, smoothCoef_ = 0.001f;
  int lastTurn_ = 0;
};

/**
 * Tremolo. The gain swings between 1 and 1 - depth, so depth 0 is silence-
 * free unity and the level never exceeds the input; a stereo phase puts the
 * right channel's swing anywhere from in step to opposite.
 */
class Tremolo {
public:
  /** Sample rate in Hz, for both channels' LFOs (see Lfo::setSampleRate()). */
  void setSampleRate(float sr) {
    lfoL_.setSampleRate(sr);
    lfoR_.setSampleRate(sr);
  }
  /** Rate in Hz (1 by default), held as Lfo::setRate() holds it. */
  void setRate(float hz) { lfoL_.setRate(hz); lfoR_.setRate(hz); }
  /** How far the gain dips, clamped to 0..1 (0.5 by default): the gain
   *  swings between 1 and 1 - depth. Applies from the next sample. */
  void setDepth(float d) { depth_ = clampf(d, 0.0f, 1.0f); }
  /** The LFO's shape for both channels; Sine by default. */
  void setShape(Lfo::Shape s) { lfoL_.setShape(s); lfoR_.setShape(s); }
  /** Right channel's offset in turns: 0.5 is opposite, the "stereo" tremolo. */
  void setStereoPhase(float turns) { lfoR_.setPhaseOffset(turns); }
  /** Both LFOs back to the start of their cycles (see Lfo::reset()). */
  void reset() { lfoL_.reset(); lfoR_.reset(); }

  /** One stereo frame, in place. No allocation. */
  inline void process(float& left, float& right) {
    left *= 1.0f - depth_ * Lfo::unipolar(lfoL_.next());
    right *= 1.0f - depth_ * Lfo::unipolar(lfoR_.next());
  }
  /** One mono sample, on the left channel's LFO alone: use one overload or
   *  the other on an instance, or the two LFOs drift apart. */
  inline float process(float x) { return x * (1.0f - depth_ * Lfo::unipolar(lfoL_.next())); }

private:
  Lfo lfoL_, lfoR_;
  float depth_ = 0.5f;
};

/** Auto-pan: a constant-power pan swept by the LFO, so the sum of the two
 *  channels' powers is the input's at every instant -- no dip in the middle. */
class AutoPan {
public:
  /** Sample rate in Hz, for the LFO (see Lfo::setSampleRate()). */
  void setSampleRate(float sr) { lfo_.setSampleRate(sr); }
  /** Rate in Hz (1 by default), held as Lfo::setRate() holds it. */
  void setRate(float hz) { lfo_.setRate(hz); }
  /** How far the sweep reaches, clamped to 0..1: 1 (the default) is hard
   *  left to hard right, 0 holds the centre, 3 dB down on each side. */
  void setDepth(float d) { depth_ = clampf(d, 0.0f, 1.0f); }
  /** The LFO's shape; Sine by default. */
  void setShape(Lfo::Shape s) { lfo_.setShape(s); }
  /** The LFO back to the start of its cycle (see Lfo::reset()). */
  void reset() { lfo_.reset(); }

  /** Mono in, stereo out. */
  inline void process(float in, float& left, float& right) {
    const float pan = depth_ * lfo_.next();                      // -1..1
    const float angle = (pan + 1.0f) * 0.25f * kPi;              // 0..pi/2
    left = in * std::cos(angle);
    right = in * std::sin(angle);
  }

private:
  Lfo lfo_;
  float depth_ = 1.0f;
};

/**
 * Vibrato: a short delay read at a point that moves with the LFO, cubic
 * tap. Depth in CENTS, converted to the delay swing that produces it: a
 * sinusoidal delay of amplitude D samples at rate f gives a peak pitch
 * deviation of 2 pi f D / fs, so D follows from the cents asked for.
 */
template <int MaxSamples = 8192>
class Vibrato {
public:
  /** 5 Hz and 20 cents once setSampleRate() has run; until then a plain
   *  8-sample delay with no swing. */
  Vibrato() { centreNow_.set(centre_); }
  /** Sample rate in Hz (1 or less means 48 kHz): re-applies the rate and
   *  works the swing and the centre out again, so latencySamples() can
   *  change. The two glide over 20 ms once audio runs. */
  void setSampleRate(float sr) {
    sr_ = sr > 1.0f ? sr : 48000.0f;
    lfo_.setSampleRate(sr_);
    // The rate too. Only setRate() pushed it, so a vibrato set up with
    // setSampleRate and setDepthCents ran its LFO at the Lfo's own 1 Hz while
    // the swing was worked out for the class's 5 Hz: asked for 50 cents, it
    // swung 10. And 8192 samples by default, not 4096: half a hertz at 100
    // cents needs a 3600-sample swing at 192 kHz, and 4096 clamped it to 57.
    lfo_.setRate(rateHz_);
    centreNow_.setSampleRate(sr_, 20.0f);
    swingNow_.setSampleRate(sr_, 20.0f);
    update();
  }
  /** Rate in Hz, clamped to 0.1 .. 20 (5 by default). The swing that gives
   *  the depth grows as the rate falls, so this moves the centre delay and
   *  latencySamples() with it. */
  void setRate(float hz) {
    rateHz_ = clampf(hz, 0.1f, 20.0f);
    lfo_.setRate(rateHz_);
    update();
  }
  /** Peak deviation in cents, either way. */
  void setDepthCents(float cents) {
    cents_ = clampf(cents, 0.0f, 200.0f);
    update();
  }
  /** Empties the delay, restarts the LFO and lands the glides. The
   *  settings are kept. */
  void reset() {
    line_.reset();
    lfo_.reset();
    centreNow_.snap();
    swingNow_.snap();
  }
  /** The centre delay the host must be told about: the mean delay, exactly. */
  int latencySamples() const { return (int) centre_; }
  /** True when the depth asked for needs more swing than MaxSamples holds at
   *  this rate and is being clamped -- a slower rate or a larger MaxSamples
   *  gives it back. */
  bool depthLimited() const { return limited_; }

  /** One mono sample in, one out, delayed by latencySamples() on average.
   *  No allocation. */
  inline float process(float x) {
    // The centre and the swing GLIDE to new settings once audio runs. Both
    // follow the rate and the depth -- the swing for a given depth is
    // inversely the rate, and the centre sits a whole sample past it -- and
    // set in one sample they moved the read point by whole samples at once:
    // a rate knob turned from 5 Hz to 4 jumped it five, a splice in the
    // signal 3.1 times the largest step a 1 kHz tone takes under the
    // vibrato. The two glide together, by the same factor, so the read point
    // stays inside the range both settings keep it in.
    const float d = centreNow_.next() + swingNow_.next() * lfo_.next();
    const float y = line_.readCubic(d);
    line_.write(x);
    return y;
  }

private:
  void update() {
    // Peak pitch ratio 2^(cents/1200) - 1 = 2 pi f D / fs.
    const float ratio = std::pow(2.0f, cents_ / 1200.0f) - 1.0f;
    swing_ = ratio * sr_ / (2.0f * kPi * rateHz_);
    const float limit = (float) (MaxSamples / 2 - 8);
    limited_ = swing_ > limit;
    if (limited_) swing_ = limit;
    // The centre is a whole number of samples: it is the latency the host is
    // told, and a host compensates in whole samples. Truncated from a
    // fractional centre, the report was up to a sample short of the delay.
    centre_ = std::ceil(swing_) + 8.0f;
    // Landing at once before audio runs (and after reset()), gliding after.
    centreNow_.set(centre_);
    swingNow_.set(swing_);
  }

  DelayLine<MaxSamples> line_;
  Lfo lfo_;
  float sr_ = 48000.0f, rateHz_ = 5.0f, cents_ = 20.0f, swing_ = 0.0f, centre_ = 8.0f;
  /** What process() reads with: centre_ and swing_, glided to. */
  Glide centreNow_, swingNow_;
  bool limited_ = false;
};

/** Ring modulator: the signal times a carrier sine, mixed against the dry.
 *  Sidebands at f +/- carrier; the carrier itself does not appear, because
 *  a multiply has no additive term. */
class RingModulator {
public:
  /** Sample rate in Hz; re-applies the carrier and sets the mix's 20 ms
   *  glide for it (it was 960 samples at any rate). Call it before the
   *  first process(): until it or setCarrier() runs, the carrier stands
   *  still at 0 and the wet signal is silence. */
  void setSampleRate(float sr) {
    osc_.setSampleRate(sr);
    osc_.setFreq(hz_);
    mixer_.setSampleRate(sr);
  }
  /** Carrier frequency in Hz (440 by default), held to 0 .. half the
   *  sample rate. Applies at once, phase continuous. */
  void setCarrier(float hz) { hz_ = hz; osc_.setFreq(hz); }
  /** 0 dry .. 1 wet, clamped, on an equal-power law; glides once audio runs
   *  (see DryWetMixer::setMix()). Never set, the mix is 0.5, as the chorus
   *  and the phaser start, and is there from construction: nothing glides
   *  up to it. It was 0, and a ring modulator never given a mix played the
   *  dry signal alone. */
  void setMix(float mix) { mixer_.setMix(mix); }
  /** The carrier back to phase 0, and the mix landed on its target. */
  void reset() {
    osc_.reset();
    mixer_.snap();
  }
  /** One mono sample. No allocation. */
  inline float process(float x) { return mixer_.process(x, x * osc_.sine()); }

private:
  static constexpr float kDefaultMix = 0.5f;
  Oscillator osc_;
  DryWetMixer mixer_{kDefaultMix};
  float hz_ = 440.0f;
};

/**
 * Flanger: the Chorus machine at a flanger's settings -- a delay under ten
 * milliseconds swept from near zero, with feedback, so the comb's notches
 * are audible and move. Positive feedback emphasises the peaks, negative the
 * notches (the "hollow" flange). One class rather than a preset, because a
 * plugin asks for it by name.
 */
class Flanger {
public:
  /** Prepares the chorus underneath (see Chorus::prepare()) and applies
   *  the rate, depth and minimum delay: 0.3 Hz, 3 ms and 0.5 ms unless set. */
  void prepare(const ProcessSpec& spec) {
    chorus_.prepare(spec);
    apply();
  }
  /** Sweep rate in Hz (0.3 by default), held to 0 .. half the sample rate. */
  void setRate(float hz) { rate_ = hz; apply(); }
  /** Sweep depth in milliseconds, 0.1 .. 10. */
  void setDepth(float ms) { depth_ = clampf(ms, 0.1f, 10.0f); apply(); }
  /** The shortest delay of the sweep, ms: a flanger lives near zero. */
  void setMinimumDelay(float ms) { minimum_ = clampf(ms, 0.05f, 5.0f); apply(); }
  /** Feedback, clamped to -0.95 .. 0.95 and 0 until set: positive for the
   *  peaks, negative for the hollow flange. */
  void setFeedback(float fb) { chorus_.setFeedback(fb); }
  /** 0 dry .. 1 wet, as Chorus::setMix(). Never set, the mix is 0.5, the
   *  chorus's own default, from construction; it was 0, dry only. The comb
   *  is deepest at 0.5. */
  void setMix(float mix) { chorus_.setMix(mix); }
  /** The sweep from its start and the line empty (see Chorus::reset()). */
  void reset() { chorus_.reset(); }
  /** One stereo frame, in place. No allocation. */
  inline void process(float& left, float& right) { chorus_.process(left, right); }

private:
  void apply() {
    chorus_.setRate(rate_);
    chorus_.setDepth(depth_);
    chorus_.setCentreDelay(minimum_ + 0.5f * depth_);
  }
  Chorus chorus_;
  float rate_ = 0.3f, depth_ = 3.0f, minimum_ = 0.5f;
};

} // namespace sonore
