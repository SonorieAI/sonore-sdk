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
#include <cmath>
#include <cstdint>
#include "audio.h"
#include "random.h"

namespace sonore {

class Lfo {
public:
  enum class Shape { Sine, Triangle, Saw, Square, SampleHold, SmoothRandom };

  void setSampleRate(float sr) {
    sr_ = sr > 1.0f ? sr : 48000.0f;
    setRate(rateHz_);
  }
  void setRate(float hz) {
    rateHz_ = clampf(hz, 0.0f, sr_ * 0.45f);
    inc_ = rateHz_ / sr_;
    // The random shape's smoother closes at the rate itself, so the drift
    // is a wander and not a staircase with the corners rounded.
    smoothCoef_ = 1.0f - std::exp(-2.0f * kPi * (rateHz_ > 0.01f ? rateHz_ : 0.01f) / sr_);
  }
  void setShape(Shape s) { shape_ = s; }
  /** Phase offset in turns: 0.25 puts a second LFO in quadrature. */
  void setPhaseOffset(float turns) { offset_ = turns - std::floor(turns); }
  void setSeed(uint64_t seed) { random_.setSeed(seed); }
  /** Restart the cycle -- on a note-on, say. */
  void retrigger() { phase_ = 0.0f; }
  void reset() {
    phase_ = 0.0f;
    held_ = random_.nextBipolar();
    smooth_ = held_;
    lastTurn_ = 0;
  }
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
  void setSampleRate(float sr) {
    lfoL_.setSampleRate(sr);
    lfoR_.setSampleRate(sr);
  }
  void setRate(float hz) { lfoL_.setRate(hz); lfoR_.setRate(hz); }
  void setDepth(float d) { depth_ = clampf(d, 0.0f, 1.0f); }
  void setShape(Lfo::Shape s) { lfoL_.setShape(s); lfoR_.setShape(s); }
  /** Right channel's offset in turns: 0.5 is opposite, the "stereo" tremolo. */
  void setStereoPhase(float turns) { lfoR_.setPhaseOffset(turns); }
  void reset() { lfoL_.reset(); lfoR_.reset(); }

  inline void process(float& left, float& right) {
    left *= 1.0f - depth_ * Lfo::unipolar(lfoL_.next());
    right *= 1.0f - depth_ * Lfo::unipolar(lfoR_.next());
  }
  inline float process(float x) { return x * (1.0f - depth_ * Lfo::unipolar(lfoL_.next())); }

private:
  Lfo lfoL_, lfoR_;
  float depth_ = 0.5f;
};

/** Auto-pan: a constant-power pan swept by the LFO, so the sum of the two
 *  channels' powers is the input's at every instant -- no dip in the middle. */
class AutoPan {
public:
  void setSampleRate(float sr) { lfo_.setSampleRate(sr); }
  void setRate(float hz) { lfo_.setRate(hz); }
  void setDepth(float d) { depth_ = clampf(d, 0.0f, 1.0f); }
  void setShape(Lfo::Shape s) { lfo_.setShape(s); }
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
template <int MaxSamples = 4096>
class Vibrato {
public:
  void setSampleRate(float sr) {
    sr_ = sr > 1.0f ? sr : 48000.0f;
    lfo_.setSampleRate(sr_);
    update();
  }
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
  void reset() {
    line_.reset();
    lfo_.reset();
  }
  /** The centre delay the host must be told about. */
  int latencySamples() const { return (int) centre_; }

  inline float process(float x) {
    const float d = centre_ + swing_ * lfo_.next();
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
    if (swing_ > limit) swing_ = limit;
    centre_ = swing_ + 8.0f;
  }

  DelayLine<MaxSamples> line_;
  Lfo lfo_;
  float sr_ = 48000.0f, rateHz_ = 5.0f, cents_ = 20.0f, swing_ = 0.0f, centre_ = 8.0f;
};

/** Ring modulator: the signal times a carrier sine, mixed against the dry.
 *  Sidebands at f +/- carrier; the carrier itself does not appear, because
 *  a multiply has no additive term. */
class RingModulator {
public:
  void setSampleRate(float sr) { osc_.setSampleRate(sr); osc_.setFreq(hz_); }
  void setCarrier(float hz) { hz_ = hz; osc_.setFreq(hz); }
  void setMix(float mix) { mixer_.setMix(mix); }
  void reset() { osc_.reset(); }
  inline float process(float x) { return mixer_.process(x, x * osc_.sine()); }

private:
  Oscillator osc_;
  DryWetMixer mixer_;
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
  void prepare(const ProcessSpec& spec) {
    chorus_.prepare(spec);
    apply();
  }
  void setRate(float hz) { rate_ = hz; apply(); }
  /** Sweep depth in milliseconds, 0.1 .. 10. */
  void setDepth(float ms) { depth_ = clampf(ms, 0.1f, 10.0f); apply(); }
  /** The shortest delay of the sweep, ms: a flanger lives near zero. */
  void setMinimumDelay(float ms) { minimum_ = clampf(ms, 0.05f, 5.0f); apply(); }
  void setFeedback(float fb) { chorus_.setFeedback(fb); }
  void setMix(float mix) { chorus_.setMix(mix); }
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
