// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: composite effects.
//
// The primitives in dsp.h are the parts; these are the finished machines a
// generated plugin reaches for by name: a reverb, a chorus, a limiter, a gate,
// a crossover. Each one is the standard published topology, implemented here,
// and each is allocation-free with its buffers sized at compile time.
//
// Included by dsp.h, so `#include <sonore/dsp.h>` stays the single entry point.
#pragma once
#ifndef SONORE_DSP_H_INCLUDED
#error "effects.h is part of dsp.h and needs what it includes first: include <sonore/dsp.h> instead."
#endif
#include <cmath>
#include <cstdint>
#include "audio.h"

namespace sonore {

// ── Reverb ───────────────────────────────────────────────────────────────────

/**
 * Schroeder/Moorer reverb: four damped feedback combs in parallel into two
 * allpasses in series, per channel, with the right channel's delays offset so
 * the two decorrelate into a stereo field.
 *
 * The delay lengths are mutually prime primes at 44.1 kHz, the classic choice,
 * because common factors make the comb resonances line up and the tail rings on
 * a pitch instead of decaying evenly.
 */
class Reverb {
public:
  /** The four controls, all 0..1 and set together with setParameters(),
   *  which clamps them: room size maps to comb feedback 0.70..0.98, damping
   *  to the high-frequency loss per pass, width from a mono tail (0) to
   *  fully decorrelated (1), mix from dry to wet. The reference's
   *  Reverb::Parameters, smaller: one mix instead of wet and dry levels, and
   *  no freeze. */
  struct Parameters {
    float roomSize = 0.6f;  ///< 0..1, maps to comb feedback 0.70 (short) .. 0.98 (long tail)
    float damping = 0.4f;   ///< 0..1, HF loss per pass: 0 bright, 1 darkest
    float width = 1.0f;     ///< 0..1 stereo spread: 0 a mono tail, 1 fully decorrelated
    float mix = 0.3f;       ///< 0..1 dry/wet, on an equal-power law
  };

  /** Sizes the delays for `spec.sampleRate`, empties the tail and applies
   *  the last setParameters() again. Only the rate is read: the reverb is
   *  always stereo. The tunings scale with the rate up to what the lines
   *  hold, about 199 kHz; above that every delay shrinks by the same factor.
   *  Allocation-free: the lines are fixed arrays. */
  void prepare(const ProcessSpec& spec) {
    sampleRate_ = (float) spec.sampleRate;
    feedbackGlide_.setSampleRate(sampleRate_, 20.0f);
    widthGlide_.setSampleRate(sampleRate_, 20.0f);
    for (DryWetMixer& m : mixer_) m.setSampleRate(sampleRate_);
    // The tunings are quoted at 44.1 kHz; scale so the room keeps its size at
    // any rate rather than shrinking as the host's rate rises -- and ROUND, to
    // whole samples. A scaled length is fractional, and a linear read of a
    // fractional delay is a lowpass that depends on the fraction. Inside a
    // feedback loop it compounds on every pass, so the tail's high end was a
    // function of the host's rate: at 48 kHz the 8 kHz T60 came out two thirds
    // of what 44.1 kHz gave, with every setting the same.
    //
    // Up to what the lines hold, which is 199 kHz; past it EVERY delay
    // shrinks by the same factor, as the FDN's and the spring's do, and the
    // room is smaller but still a room. Each line used to clamp its own read:
    // at 384 kHz all four combs and both allpasses, on both sides, sat at the
    // last sample of their lines -- one comb ringing at 62.5 Hz, and left and
    // right bit-identical whatever the width.
    float scale = sampleRate_ / 44100.0f;
    const float combFit = (float) (kCombLine - 2 - kSpread) / kCombTuning[kNumCombs - 1];
    const float allpassFit = (float) (kAllpassLine - 2 - kSpread) / kAllpassTuning[0];
    if (scale > combFit) scale = combFit;
    if (scale > allpassFit) scale = allpassFit;
    for (int c = 0; c < 2; ++c) {
      // ~23 samples of offset is what decorrelates the two sides.
      const float spread = c == 0 ? 0.0f : (float) kSpread;
      for (int i = 0; i < kNumCombs; ++i)
        combDelay_[c][i] = std::round(kCombTuning[i] * scale) + spread;
      for (int i = 0; i < kNumAllpasses; ++i) {
        allpassDelay_[c][i] = std::round(kAllpassTuning[i] * scale) + spread;
        // Found by audit: this array was computed and never APPLIED; the
        // allpasses ran at their 100-sample defaults at every rate, so the
        // diffusion was wrong and did not scale. Everything still sounded
        // like a reverb, which is exactly why nobody heard it.
        allpass_[c][i].set(allpassDelay_[c][i], 0.5f);
      }
    }
    reset();
    setParameters(params_);
  }

  /** Empties the combs and allpasses and lands every glide on its target.
   *  The parameters are kept. */
  void reset() {
    for (DryWetMixer& m : mixer_) m.snap();
    feedbackGlide_.snap();
    widthGlide_.snap();
    for (int c = 0; c < 2; ++c) {
      for (int i = 0; i < kNumCombs; ++i) {
        comb_[c][i].reset();
        combStore_[c][i] = 0.0f;
      }
      for (int i = 0; i < kNumAllpasses; ++i) allpass_[c][i].reset();
    }
  }

  /** All four controls at once, each clamped to 0..1. Room size, width and
   *  (on each side) the mix glide over 20 ms once audio runs; before the
   *  first process(), or after reset(), they land at once.
   *  Damping applies at once. No allocation: safe to call every block. */
  void setParameters(const Parameters& p) {
    params_ = p;
    feedback_ = 0.7f + clampf(p.roomSize, 0.0f, 1.0f) * 0.28f; // 0.70 .. 0.98
    // Room size and width glide once audio runs (see Glide): moved in one
    // sample they stepped the tail 3.6 and 14.5 times its largest natural
    // step.
    feedbackGlide_.set(feedback_);
    widthGlide_.set(clampf(p.width, 0.0f, 1.0f));
    // The damping is a one-pole coefficient defined at 44.1 kHz. Used as is,
    // its cutoff in hertz moved with the rate -- twice as high at 96 kHz, so a
    // damped room rang 60% longer there. Raising the pole to 44100/rate keeps
    // the cutoff where it was (the pole is e^(-wc/fs)).
    const float damp44k = clampf(p.damping, 0.0f, 1.0f) * 0.4f;
    damp_ = damp44k > 0.0f ? std::pow(damp44k, 44100.0f / sampleRate_) : 0.0f;
    for (DryWetMixer& m : mixer_) m.setMix(clampf(p.mix, 0.0f, 1.0f));
  }

  /** One stereo frame, in place. */
  inline void process(float& left, float& right) {
    const float dryL = left, dryR = right;
    const float input = (dryL + dryR) * 0.015f; // the classic input gain
    const float feedback = feedbackGlide_.next();

    float wet[2] = {0.0f, 0.0f};
    for (int c = 0; c < 2; ++c) {
      for (int i = 0; i < kNumCombs; ++i) {
        const float delayed = comb_[c][i].read(combDelay_[c][i]);
        // One-pole damping inside the loop: each pass loses more highs, which
        // is what makes a tail sound like a room rather than a metal pipe.
        combStore_[c][i] = flushDenormal(delayed * (1.0f - damp_) + combStore_[c][i] * damp_);
        comb_[c][i].write(flushDenormal(input + combStore_[c][i] * feedback));
        wet[c] += delayed;
      }
      for (int i = 0; i < kNumAllpasses; ++i) wet[c] = allpass_[c][i].process(wet[c]);
    }

    // Width: 1 = fully decorrelated, 0 = mono tail. Each side keeps
    // (1 + w) / 2 of its own tail and takes (1 - w) / 2 of the other's. It
    // took w and 1 - w, which is mono at 0.5 and at 0 the two tails SWAPPED:
    // as wide as at 1, the "mono" end of the control.
    const float w = widthGlide_.next();
    const float own = 0.5f * (1.0f + w), other = 0.5f * (1.0f - w);
    const float wetL = wet[0] * own + wet[1] * other;
    const float wetR = wet[1] * own + wet[0] * other;

    left = mixer_[0].process(dryL, wetL);
    right = mixer_[1].process(dryR, wetR);
  }

private:
  static constexpr int kNumCombs = 4;
  static constexpr int kNumAllpasses = 2;
  // Mutually prime lengths, in samples at 44.1 kHz.
  static constexpr float kCombTuning[kNumCombs] = {1116.0f, 1188.0f, 1277.0f, 1356.0f};
  static constexpr float kAllpassTuning[kNumAllpasses] = {556.0f, 441.0f};

  // Sized for 192 kHz: the tunings scale with the rate, and the longest comb
  // is 1356 samples at 44.1k -> ~5927 at 192k. The old 3072-sample lines
  // CLAMPED there, which silently shrank the room at high rates, the worst
  // kind of wrong, because everything still sounds like a reverb.
  static constexpr int kCombLine = 6144, kAllpassLine = 2560;
  static constexpr int kSpread = 23; // the right side's offset, in samples
  DelayLine<kCombLine> comb_[2][kNumCombs];
  Allpass<kAllpassLine> allpass_[2][kNumAllpasses];
  float combDelay_[2][kNumCombs]{};
  float allpassDelay_[2][kNumAllpasses]{};
  float combStore_[2][kNumCombs]{};
  float sampleRate_ = 48000.0f;
  float feedback_ = 0.84f, damp_ = 0.16f;
  Glide feedbackGlide_{0.84f, 0.84f}, widthGlide_{1.0f, 1.0f};
  Parameters params_{};
  /** One mix glide per side, set to the rate in prepare(). One shared by
   *  both stepped twice a frame, gliding in half its time, and was never
   *  given a rate. */
  DryWetMixer mixer_[2];
};

// ── Modulation ───────────────────────────────────────────────────────────────

/**
 * Chorus / flanger. The difference is only depth and delay range: a flanger
 * modulates a very short delay (under ~10 ms) with feedback to make the comb
 * notches audible; a chorus uses a longer one and no feedback so it thickens
 * rather than whooshes. One implementation covers both.
 */
template <typename T>
class ChorusT {
public:
  /** Sets the LFO and the glides for `spec.sampleRate` and resets (see
   *  reset()). Only the rate is read: the chorus is always stereo.
   *  Allocation-free. The line holds 8192 samples, so the 45 ms longest
   *  delay fits up to about 180 kHz and is about 42 ms at 192 kHz. */
  void prepare(const ProcessSpec& spec) {
    sampleRate_ = (T) spec.sampleRate;
    lfo_.setSampleRate(sampleRate_);
    lfo_.setFreq(rate_);
    // The delay times glide. A delay that jumps is a click -- a 6 -> 12 ms
    // depth change stepped the output 33 times its largest natural step, and
    // the Flanger, which is this, did the same on every depth change. One
    // that glides is a brief pitch bend, which is what turning a chorus knob
    // sounds like.
    depthSmooth_.setup(sampleRate_, kGlideMs);
    centreSmooth_.setup(sampleRate_, kGlideMs);
    for (DryWetMixerT<T>& m : mixer_) m.setSampleRate(sampleRate_);
    reset();
  }
  /** The sweep from its start, the lines empty, the glides landed. There was
   *  no reset(), and prepare() kept the LFO's phase. */
  void reset() {
    lfo_.reset();
    depthSmooth_.snap(depthMs_);
    centreSmooth_.snap(centreMs_);
    for (int c = 0; c < 2; ++c) line_[c].reset();
    for (DryWetMixerT<T>& m : mixer_) m.snap();
    running_ = false;
  }

  /** LFO rate in Hz (0.6 by default), held to 0 .. half the sample rate.
   *  The sweep carries on from its current phase. */
  void setRate(T hz) { rate_ = hz; lfo_.setFreq(hz); }
  /** How far the delay swings, in ms peak to peak (6 by default): it moves
   *  between centre - depth/2 and centre + depth/2. Glides with a 50 ms
   *  time constant while running.
   *
   *  Until the first process() a setting LANDS: the plugin flow is prepare,
   *  then the parameters, and prepare() had snapped the glides to the values
   *  before them -- the first 400 ms of a fresh chorus bent its pitch by 124
   *  cents (a flanger's by 254) on its way from the defaults. Running, a
   *  change glides, as before. */
  void setDepth(T ms) {
    depthMs_ = ms;
    if (!running_) depthSmooth_.snap(ms);
  }
  /** The delay the sweep is centred on, in ms (14 by default). Lands before
   *  the first process() and glides after it, as setDepth() does. The swept
   *  delay is held to 0.1 .. 45 ms (less above about 180 kHz). */
  void setCentreDelay(T ms) {
    centreMs_ = ms;
    if (!running_) centreSmooth_.snap(ms);
  }
  /** Wet signal fed back into each side's line, clamped to -0.95 .. 0.95.
   *  0, the default, is a chorus; feedback on a short centre delay is a
   *  flanger. Applies at once. */
  void setFeedback(T fb) { feedback_ = clampT<T>(fb, -T(0.95), T(0.95)); }
  /** 0 dry .. 1 wet, clamped, on an equal-power law; glides once audio runs
   *  (see DryWetMixer::setMix()), each side over 20 ms at the rate prepare()
   *  set. Never set, the mix is 0.5, as the reference's chorus starts, and
   *  is there from construction: nothing glides up to it. It was 0, and a
   *  chorus that was never given a mix played the dry signal alone. */
  void setMix(T mix) {
    for (DryWetMixerT<T>& m : mixer_) m.setMix(mix);
  }

  /** One stereo frame, in place, the two sides' delays swept in quadrature.
   *  No allocation. */
  inline void process(T& left, T& right) {
    running_ = true;
    // Quadrature between the sides -- sine on the left, cosine on the right --
    // which is what makes a chorus wide: the two delays are never both at
    // rest at once, so the sides' pitch wobbles never line up and the comb
    // notches of a flanger never coincide. (The first version used -sine for
    // the right side and called it quadrature: that is antiphase, and both
    // sides sit still at the same instant twice a cycle.)
    const T turns = lfo_.phase();
    const T phase = lfo_.sine();
    const T cosine = sinTurnsT(turns + T(0.25));
    const T dryL = left, dryR = right;
    const T depthMs = depthSmooth_.next(depthMs_), centreMs = centreSmooth_.next(centreMs_);
    for (int c = 0; c < 2; ++c) {
      const T mod = c == 0 ? phase : cosine;
      const T ms = centreMs + mod * depthMs * T(0.5);
      // The ceiling comes from the BUFFER, not from a constant: 45 ms is the
      // musical maximum, but at a rate where that no longer fits, the honest
      // behaviour is a stated shorter maximum rather than a silent clamp
      // deep inside the delay line.
      const T maxMs = (T) (kChorusBuffer - 8) * T(1000.0) / sampleRate_;
      const T samples =
          clampT<T>(ms, T(0.1), maxMs < T(45.0) ? maxMs : T(45.0)) * T(0.001) * sampleRate_;
      // CUBIC, not linear, and this is the one place in the SDK where that
      // matters. Linear interpolation's magnitude response depends on the
      // fractional part of the delay, so a delay that SWEEPS sweeps a lowpass
      // with it -- measured here at 0.66 dB down at 8 kHz, 1.58 dB at 16 kHz
      // against the third-order read. A static delay loses nothing, which is
      // why the reverb combs and the limiter lookahead still use read().
      //
      // readCubic() had documented exactly this and had no callers at all.
      const T wet = line_[c].readCubic(samples);
      const T in = (c == 0 ? dryL : dryR) + wet * feedback_;
      line_[c].write(flushDenormalT(in));
      if (c == 0) left = mixer_[0].process(dryL, wet);
      else right = mixer_[1].process(dryR, wet);
    }
  }

private:
  static constexpr int kChorusBuffer = 8192; // 45 ms up to 96k; ~42 ms at 192k
  static constexpr T kGlideMs = T(50.0);
  static constexpr T kDefaultMix = T(0.5);
  DelayLine<kChorusBuffer, T> line_[2];
  OscillatorT<T> lfo_;
  /** One mix glide per side: one shared by both stepped twice a frame. */
  DryWetMixerT<T> mixer_[2]{DryWetMixerT<T>(kDefaultMix), DryWetMixerT<T>(kDefaultMix)};
  SmoothT<T> depthSmooth_, centreSmooth_;
  T sampleRate_ = T(48000.0);
  T rate_ = T(0.6), depthMs_ = T(6.0), centreMs_ = T(14.0), feedback_ = T(0.0);
  bool running_ = false; // process() has run since prepare()
};

/** Chorus in float, the form every plugin written before the templates
 *  uses; ChorusT<double> is the same processor in double. */
using Chorus = ChorusT<float>;

/**
 * Phaser: a chain of modulated allpass stages mixed with the dry signal. The
 * notches come from cancellation between the two paths, so unlike a flanger the
 * spacing is not harmonic, which is what makes it sound like sweeping rather
 * than like a jet.
 */
template <typename T>
class PhaserT {
public:
  /** Sets the LFO, the stages and the depth glide for `spec.sampleRate` and
   *  resets (see reset()). Only the rate is read: the phaser is always
   *  stereo. Allocation-free. */
  void prepare(const ProcessSpec& spec) {
    sampleRate_ = (T) spec.sampleRate;
    lfo_.setSampleRate(sampleRate_);
    lfo_.setFreq(rate_);
    for (int c = 0; c < 2; ++c)
      for (int s = 0; s < kStages; ++s) stage_[c][s].setSampleRate(sampleRate_);
    depthGlide_.setSampleRate(sampleRate_, T(20.0));
    for (DryWetMixerT<T>& m : mixer_) m.setSampleRate(sampleRate_);
    reset();
  }
  /** The sweep from its start, the stages and the feedback empty. There was
   *  no reset(), and prepare() kept the LFO's phase and the feedback: the
   *  same audio rendered twice differed by 1.7 dB of null. */
  void reset() {
    lfo_.reset();
    for (int c = 0; c < 2; ++c) {
      for (int s = 0; s < kStages; ++s) stage_[c][s].reset();
      fbStore_[c] = T(0.0);
    }
    for (DryWetMixerT<T>& m : mixer_) m.snap();
    depthGlide_.snap();
  }

  /** LFO rate in Hz (0.5 by default), held to 0 .. half the sample rate.
   *  The sweep carries on from its current phase. */
  void setRate(T hz) { rate_ = hz; lfo_.setFreq(hz); }
  /** Glides over 20 ms once audio runs (see Glide). The depth sets where the
   *  sweep is, so in one sample it moved every stage's frequency at once --
   *  0 -> 1 near the top of the sweep stepped the output 3.4 times its
   *  largest natural step. */
  void setDepth(T d) { depthGlide_.set(clampT<T>(d, T(0.0), T(1.0))); }
  /** The last stage's output fed back into the first, clamped to
   *  -0.95 .. 0.95 (0.3 by default). Applies at once. */
  void setFeedback(T fb) { feedback_ = clampT<T>(fb, -T(0.95), T(0.95)); }
  /** 0 dry .. 1 wet, clamped, on an equal-power law; glides once audio runs
   *  (see DryWetMixer::setMix()), each side over 20 ms at the rate prepare()
   *  set. The notches come from the mix, deepest at 0.5. Never set, the mix
   *  is 0.5, as the reference's phaser starts, and is there from
   *  construction: nothing glides up to it. It was 0, and a phaser that was
   *  never given a mix played the dry signal alone, with no notch at all. */
  void setMix(T mix) {
    for (DryWetMixerT<T>& m : mixer_) m.setMix(mix);
  }

  /** One stereo frame, in place. Both sides follow one sine sweep, from
   *  200 Hz up to 200 x 40^depth Hz (8 kHz at depth 1), through four
   *  allpass stages spaced 10% apart. No allocation. */
  inline void process(T& left, T& right) {
    const T lfo = lfo_.sine() * T(0.5) + T(0.5);   // 0..1
    // Sweep logarithmically: linear frequency movement sounds like it slows
    // down at the top, because pitch is exponential.
    const T freq = T(200.0) * std::pow(T(40.0), lfo * depthGlide_.next());
    // One design per stage, shared by both channels: their sweep is the same,
    // and designing each section twice was eight sin/cos pairs a sample where
    // four give the identical filter.
    for (int s = 0; s < kStages; ++s) {
      stage_[0][s].allpass(freq * (T(1.0) + T(0.1) * s), T(0.7));
      stage_[1][s].copyCoefficientsFrom(stage_[0][s]);
    }
    T* io[2] = {&left, &right};
    for (int c = 0; c < 2; ++c) {
      T x = *io[c] + fbStore_[c] * feedback_;
      for (int s = 0; s < kStages; ++s) x = stage_[c][s].process(x);
      // Finite or nothing. An Inf fed back here re-entered every sample and
      // the phaser never recovered from one bad input sample; flushDenormal
      // now clears Inf too, and this says so where it matters most.
      fbStore_[c] = std::isfinite(x) ? flushDenormalT(x) : T(0.0);
      *io[c] = mixer_[c].process(*io[c], x);
    }
  }

private:
  static constexpr int kStages = 4;
  static constexpr T kDefaultMix = T(0.5);
  BiquadT<T> stage_[2][kStages];
  OscillatorT<T> lfo_;
  /** One mix glide per side: one shared by both stepped twice a frame. */
  DryWetMixerT<T> mixer_[2]{DryWetMixerT<T>(kDefaultMix), DryWetMixerT<T>(kDefaultMix)};
  T sampleRate_ = T(48000.0);
  T rate_ = T(0.5), feedback_ = T(0.3);
  T fbStore_[2] = {T(0.0), T(0.0)};
  GlideT<T> depthGlide_{T(0.7), T(0.7)};
};

/** Phaser in float, the form every plugin written before the templates
 *  uses; PhaserT<double> is the same processor in double. */
using Phaser = PhaserT<float>;

// ── Dynamics ─────────────────────────────────────────────────────────────────

/**
 * Look-ahead brickwall limiter.
 *
 * A compressor with a huge ratio still overshoots, because its detector reacts
 * after the peak has already passed. This delays the audio by the look-ahead
 * window so the gain is already down when the transient arrives, which is the
 * only way to actually guarantee a ceiling.
 *
 * HOW the gain gets there is the whole quality of a limiter. The first version
 * dropped the gain to its target the instant a peak ENTERED the window: a step,
 * three milliseconds before the peak, held until it left -- an attack that is
 * a click on sustained material and a pumping edge on drums. The textbook
 * look-ahead envelope is used instead (the structure in every transparent
 * mastering limiter): the required gain is put through a SLIDING MINIMUM over
 * the window, then a BOXCAR average of the same length, which turns every
 * reduction into a linear ramp that lands on the target exactly as the peak
 * arrives -- and provably never later, because every value the boxcar
 * averages is a minimum over a span that contains the sample now leaving the
 * delay. The release is exponential from there. The ceiling therefore holds
 * WITHOUT the output clamp, and the test measures the product before the
 * clamp to say so; the clamp stays for the last ulp.
 *
 * O(1) per sample: the sliding minimum is a monotonic deque over a ring of
 * indices, the boxcar a running sum. Look-ahead is a whole number of samples,
 * which is what a host can be told and what makes the delay read exact.
 */
// The default look-ahead buffer holds 3 ms up to 340 kHz; the old 512 ran out
// just past 170 kHz and silently shortened the look-ahead.
template <int MaxLookaheadSamples = 1024, typename T = float>
class Limiter {
public:
  /** Recomputes the release and the look-ahead (3 ms by default) for
   *  `spec.sampleRate` and resets. Only the rate is read: the limiter is
   *  two channels, linked. latencySamples() is what to report afterwards. */
  void prepare(const ProcessSpec& spec) {
    sampleRate_ = (T) spec.sampleRate;
    setRelease(releaseMs_);
    setLookahead(lookaheadMs_);
    reset();
  }

  /** The output ceiling in dBFS; the default is a gain of 0.98, about
   *  -0.18 dB. Not clamped. Applies at once: samples already inside the
   *  look-ahead were planned for the old ceiling, and the output clamp
   *  holds them to the new one. A NaN is ignored and the ceiling stays where
   *  it was: taken as given, the output clamp turned every sample into NaN. */
  void setCeiling(T db) {
    if (db != db) return;
    ceiling_ = dbToGainT(db);
  }
  /** Release time constant in ms (50 by default, held to at least 0.1): how
   *  fast the gain recovers, exponentially, once the peaks stop. */
  void setRelease(T ms) {
    releaseMs_ = ms;
    release_ = std::exp(-T(1.0) / (T(0.001) * (ms > T(0.1) ? ms : T(0.1)) * sampleRate_));
  }
  /** Rounded to whole samples and clamped to the buffer. Clears the state:
   *  a window that changes length mid-stream would leave the sliding minimum
   *  spanning the wrong samples.
   *
   *  The clamp is what the DELAY LINE can read -- MaxLookaheadSamples - 2 --
   *  which is also inside what the sliding minimum's ring can hold (one less
   *  than its size, or full reads as empty). It was kMaxWindow - 1, three
   *  past the delay: at 384 kHz the default 3 ms reported 1025 samples of
   *  latency and delayed 1022. */
  void setLookahead(T ms) {
    lookaheadMs_ = ms;
    int n = (int) (ms * T(0.001) * sampleRate_ + T(0.5));
    if (n < 1) n = 1;
    if (n > MaxLookaheadSamples - 2) n = MaxLookaheadSamples - 2;
    // The same window: nothing to do. This cleared the state on EVERY call,
    // and a plugin applies its parameters every block: called per block at
    // the window it already had, the delay line was emptied each time and
    // 22% of a 512-sample block (95% of a 64-sample one) came out silent.
    if (n == lookahead_ && primed_) return;
    lookahead_ = n;
    reset();
  }
  /** Empties the look-ahead delay and returns the gain to unity: the next
   *  latencySamples() samples out are silence. The settings are kept. */
  void reset() {
    for (int c = 0; c < 2; ++c) delay_[c].reset();
    for (int i = 0; i < kMaxWindow; ++i) {
      targets_[i] = T(1.0);
      box_[i] = T(1.0);
    }
    head_ = tail_ = 0;
    counter_ = 0;
    boxPos_ = 0;
    boxSum_ = (double) (lookahead_ + 1);
    gain_ = T(1.0);
    primed_ = true;
  }
  /** Latency the host must be told about: the look-ahead IS a delay. */
  int latencySamples() const { return lookahead_; }
  /** lastGain() as decibels of reduction, for a meter: 0 when not
   *  limiting, POSITIVE while limiting (6 is 6 dB of reduction), like every
   *  other dynamics class here. A plain member: an editor reads it through
   *  something the DSP publishes, such as an atomic. */
  T gainReduction() const { const T d = gainToDbT(gain_); return d < T(0.0) ? -d : T(0.0); }
  /** The gain applied to the sample that just left, before the safety clamp. */
  T lastGain() const { return gain_; }

  /** One stereo frame, in place, delayed by latencySamples(). Both sides
   *  get the same gain, set by the louder one, so the image holds still.
   *  No allocation; O(1) per sample. */
  inline void process(T& left, T& right) {
    const T peak = std::fabs(left) > std::fabs(right) ? std::fabs(left) : std::fabs(right);
    const T target = peak > ceiling_ ? ceiling_ / peak : T(1.0);
    const int window = lookahead_ + 1; // this sample and the N before it

    // ── Sliding minimum over the last `window` targets ──
    // The deque holds counters of candidate minima in increasing order of
    // value; a new target evicts every older candidate it undercuts, and the
    // front expires once it is older than the window.
    // A 64-bit clock, and an age test that holds across a wrap. The clock
    // was 32 bits and the test `then + window <= now`: in the last `window`
    // samples before 2^32 -- every 24.9 hours at 48 kHz, 6.2 at 192 -- that
    // sum wrapped, the deque emptied, the minimum came from a stale slot,
    // and a +12 dBFS click went through a -1 dB ceiling at +12.
    const uint64_t now = counter_++;
    targets_[now % (uint64_t) kMaxWindow] = target;
    while (head_ != tail_) {
      const uint64_t back = deque_[(tail_ + kMaxWindow - 1) % kMaxWindow];
      if (targets_[back % (uint64_t) kMaxWindow] < target) break;
      tail_ = (tail_ + kMaxWindow - 1) % kMaxWindow;
    }
    deque_[tail_] = now;
    tail_ = (tail_ + 1) % kMaxWindow;
    while (head_ != tail_ && now - deque_[head_] >= (uint64_t) window) head_ = (head_ + 1) % kMaxWindow;
    const T minimum = targets_[deque_[head_] % (uint64_t) kMaxWindow];

    // ── Boxcar of the same length: the attack ramp ──
    boxSum_ += (double) minimum - (double) box_[boxPos_];
    box_[boxPos_] = minimum;
    if (++boxPos_ >= window) boxPos_ = 0;
    const T smooth = (T) (boxSum_ / (double) window);

    // ── Release: down with the ramp, up exponentially ──
    gain_ = smooth < gain_ ? smooth : smooth + release_ * (gain_ - smooth);
    if (gain_ > T(1.0)) gain_ = T(1.0);

    const T dl = delay_[0].tap((T) lookahead_, left);
    const T dr = delay_[1].tap((T) lookahead_, right);
    left = dl * gain_;
    right = dr * gain_;
    // The construction above guarantees the ceiling; the clamp is for the
    // last ulp of float rounding, and the test asserts it never has to act.
    left = clampT<T>(left, -ceiling_, ceiling_);
    right = clampT<T>(right, -ceiling_, ceiling_);
  }

private:
  static constexpr int kMaxWindow = MaxLookaheadSamples + 2;

  DelayLine<MaxLookaheadSamples, T> delay_[2];
  T targets_[kMaxWindow]{};
  uint64_t deque_[kMaxWindow]{};
  T box_[kMaxWindow]{};
  double boxSum_ = 1.0;
  uint64_t counter_ = 0;
  int head_ = 0, tail_ = 0, boxPos_ = 0;
  T sampleRate_ = T(48000.0);
  T ceiling_ = T(0.98), gain_ = T(1.0), release_ = T(0.999);
  T releaseMs_ = T(50.0), lookaheadMs_ = T(3.0);
  int lookahead_ = 144;
  bool primed_ = false; // reset() has run, so the state is one
};

/**
 * Noise gate with hysteresis and a hold time.
 *
 * One threshold chatters: a signal sitting exactly at it opens and closes every
 * few samples. A separate (lower) close threshold plus a minimum hold is what
 * makes a gate usable on a real drum track.
 */
template <typename T>
class NoiseGateT {
public:
  /** Sets the level detector (1 ms attack, 50 ms release) and the gate's
   *  times for `spec.sampleRate`, empties the detector and closes the gate.
   *  Only the rate is read. */
  void prepare(const ProcessSpec& spec) {
    sampleRate_ = (T) spec.sampleRate;
    env_.setSampleRate(sampleRate_);
    env_.setTimes(T(1.0), T(50.0));
    // The detector too. Kept, the level of the audio before a re-prepare held
    // the gate open: a -50 dBFS tone under a -30 dB threshold leaked through
    // at full level for 240 ms, where a fresh gate passed nothing.
    env_.reset();
    setTimes(attackMs_, holdMs_, releaseMs_);
    gain_ = T(0.0);
    open_ = false;
    holdLeft_ = 0;
  }

  /** Open threshold in dBFS (-40 by default): the gate opens when the
   *  detected peak level rises above it and closes below it minus the
   *  hysteresis. */
  void setThreshold(T db) {
    thresholdDb_ = db;
    openThresh_ = dbToGainT(db);
    closeThresh_ = dbToGainT(db - hysteresisDb_);
  }
  /** How far below the open threshold the gate closes, in dB (6 by default;
   *  a negative value counts as 0).
   *
   *  Recomputes closeThresh_ from the STORED threshold. Before this it only set
   *  hysteresisDb_, so turning the hysteresis knob did nothing until the
   *  threshold knob was next touched -- a dead control, because the threshold
   *  in dB was never kept to recompute from. */
  void setHysteresis(T db) {
    hysteresisDb_ = db > T(0.0) ? db : T(0.0);
    closeThresh_ = dbToGainT(thresholdDb_ - hysteresisDb_);
  }
  /** Times in ms (defaults 1, 50 and 100). Attack and release are the time
   *  constants of the gain's fade in and out, each held to at least
   *  0.05 ms; hold is how long the level must stay below the close
   *  threshold before the gate closes. */
  void setTimes(T attackMs, T holdMs, T releaseMs) {
    attackMs_ = attackMs; holdMs_ = holdMs; releaseMs_ = releaseMs;
    attack_ = T(1.0) - std::exp(-T(1.0) / (T(0.001) * (attackMs > T(0.05) ? attackMs : T(0.05)) * sampleRate_));
    release_ = std::exp(-T(1.0) / (T(0.001) * (releaseMs > T(0.05) ? releaseMs : T(0.05)) * sampleRate_));
    holdSamples_ = (int) (T(0.001) * holdMs * sampleRate_);
  }
  /** Whether the gate is open now. The gain lags it: once closed, the
   *  output fades out over the release time. */
  bool isOpen() const { return open_; }

  /** One sample through the gate, returned. Mono: a gate per channel opens
   *  on that channel's own level. No allocation. */
  inline T process(T x) {
    const T level = env_.process(x);
    if (!open_ && level > openThresh_) {
      open_ = true;
      holdLeft_ = holdSamples_;
    } else if (open_ && level < closeThresh_) {
      if (holdLeft_ > 0) --holdLeft_;
      else open_ = false;
    } else if (open_) {
      holdLeft_ = holdSamples_;
    }
    const T target = open_ ? T(1.0) : T(0.0);
    gain_ = open_ ? gain_ + (target - gain_) * attack_ : gain_ * release_;
    gain_ = flushDenormalT(gain_);
    return x * gain_;
  }

private:
  EnvFollowerT<T> env_;
  T sampleRate_ = T(48000.0);
  T openThresh_ = T(0.01), closeThresh_ = T(0.005), hysteresisDb_ = T(6.0);
  T thresholdDb_ = -T(40.0); // kept so setHysteresis can recompute closeThresh_
  T attackMs_ = T(1.0), holdMs_ = T(50.0), releaseMs_ = T(100.0);
  T attack_ = T(0.5), release_ = T(0.999), gain_ = T(0.0);
  int holdSamples_ = 2400, holdLeft_ = 0;
  bool open_ = false;
};

/** NoiseGate in float, the form every plugin written before the templates
 *  uses; NoiseGateT<double> is the same processor in double. */
using NoiseGate = NoiseGateT<float>;

// ── Stereo + routing ─────────────────────────────────────────────────────────

/**
 * Linkwitz-Riley 4th-order crossover: two cascaded Butterworth sections per
 * band. The defining property is that the low and high outputs sum FLAT, which
 * is what makes it the right tool for multiband processing: a plain pair of
 * filters leaves a peak or a dip at the crossover point.
 */
template <typename T>
class LinkwitzRileyT {
public:
  /** Sample rate in Hz; redesigns the sections at the current crossover.
   *  Keeps the filter state. */
  void setSampleRate(T sr) {
    sr_ = sr;
    for (int i = 0; i < 2; ++i) {
      lp_[i].setSampleRate(sr);
      hp_[i].setSampleRate(sr);
    }
    setCrossover(freq_);
  }
  /** Crossover frequency in Hz (1 kHz by default), held to 10 Hz .. 0.49 x
   *  the sample rate. Safe to move every sample: the state-variable
   *  sections stay stable while swept. */
  void setCrossover(T hz) {
    freq_ = hz;
    for (int i = 0; i < 2; ++i) {
      lp_[i].lowpass(hz, 0.70710678);
      hp_[i].highpass(hz, 0.70710678);
    }
  }

  /** How many samples a full-scale transient takes to decay below `floorDb`.
   *
   *  A crossover RINGS, and the lower its corner the longer: these are
   *  Butterworth sections at Q = 1/sqrt(2), whose impulse response decays as
   *  exp(-w0 t / 2Q) = exp(-2 pi f t * sqrt(2) / 2). A 40 Hz corner is still
   *  audible fifty milliseconds after the input stops.
   *
   *  A plugin that ends in a crossover therefore HAS a tail whether or not it
   *  meant to. The splitter example declared none, and the host test measured
   *  399 samples coming out of it after the input went silent: small, real,
   *  and exactly the kind of thing that used to pass unnoticed. Pass the
   *  LOWEST corner the plugin's controls allow, not the current one: the tail
   *  a host is told about has to still be true after the user turns the knob
   *  down. */
  static int tailSamples(T sr, T crossoverHz, T floorDb = -T(80.0)) {
    const T fc = crossoverHz > T(1.0) ? crossoverHz : T(1.0);
    const T sigma = T(2.0) * piT<T> * fc * T(0.70710678); // w0 / 2Q, the pole's real part

    // The envelope is (1 + x) e^-x, not e^-x, and the difference is not small.
    //
    // This function first used the plain exponential, and the settling test in
    // sdk_tests measured it 30% short at 40 Hz, at 200 Hz and at 2 kHz alike:
    // the same ratio in all three, which is what a missing polynomial factor
    // looks like rather than a mistuned constant. A Linkwitz-Riley is two
    // identical Butterworth sections in cascade, so its poles are DOUBLE, and
    // a double pole decays as t·e^-(sigma·t): the exponential eventually wins,
    // but the linear term holds the tail up for several time constants first.
    //
    // Solve (1 + x) e^-x = floor for x = sigma·t by Newton, starting from the
    // single-pole answer. It converges in a handful of steps and this is
    // called once per prepare(), never per sample.
    const T target = -std::log(std::pow(T(10.0), floorDb * T(0.05))); // -ln(floor)
    T x = target;
    for (int i = 0; i < 24; ++i) {
      const T f = std::log(T(1.0) + x) - x + target;
      const T df = T(1.0) / (T(1.0) + x) - T(1.0);
      if (std::fabs(df) < T(1e-9)) break;
      const T step = f / df;
      x -= step;
      if (std::fabs(step) < T(1e-6)) break;
    }

    // The model lands within 2% of the measured decay across the whole range
    // the control covers; the margin is for that 2% and for the assumption
    // that the state was at full scale when the input stopped. Erring long
    // costs a host a few milliseconds of extra render, erring short costs the
    // user the end of their sound.
    return (int) std::ceil(T(1.1) * x / sigma * sr);
  }
  /** Clears both bands' filter state. */
  void reset() {
    for (int i = 0; i < 2; ++i) { lp_[i].reset(); hp_[i].reset(); }
  }
  /** Split one sample into a low and a high band that sum back to it. */
  inline void process(T x, T& low, T& high) {
    low = lp_[1].process(lp_[0].process(x));
    high = hp_[1].process(hp_[0].process(x));
  }

private:
  // Double precision: in float sections a 40 Hz crossover at 192 kHz no
  // longer summed flat -- -1.13 dB at 20 Hz, where the header promises
  // +-0.15 dB and the same design in double holds 0.0002.
  //
  // And state-variable sections, not direct-form ones: a crossover is a
  // control a user turns and a host automates, and swept every sample the
  // direct-form sections diverged (see SvfSection).
  SvfSection lp_[2], hp_[2];
  T sr_ = T(48000.0), freq_ = T(1000.0);
};

/** LinkwitzRiley in float, the form every plugin written before the templates
 *  uses; LinkwitzRileyT<double> is the same processor in double. */
using LinkwitzRiley = LinkwitzRileyT<float>;

/** Mid/side width control. Width 0 collapses to mono, 1 is untouched, >1
 *  exaggerates the sides. Mono compatibility is why this works on M/S rather
 *  than by panning: the mid is preserved exactly. */
template <typename T>
class StereoWidenerT {
public:
  /** The side gain, clamped to 0..2 (1 by default). Applies on the next
   *  sample, unsmoothed. */
  void setWidth(T w) { width_ = w < T(0.0) ? T(0.0) : (w > T(2.0) ? T(2.0) : w); }
  /** One stereo frame, in place: the mid kept, the side scaled. */
  inline void process(T& left, T& right) const {
    const T mid = (left + right) * T(0.5);
    const T side = (left - right) * T(0.5) * width_;
    left = mid + side;
    right = mid - side;
  }

private:
  T width_ = T(1.0);
};

/** StereoWidener in float, the form every plugin written before the templates
 *  uses; StereoWidenerT<double> is the same processor in double. */
using StereoWidener = StereoWidenerT<float>;

/** Constant-power panner. `pan` is -1 (left) .. 0 (centre) .. +1 (right);
 *  the -3 dB centre law keeps perceived loudness constant across the sweep.
 *  setLaw() picks any other of PanLaw's: -6 dB (Linear, Sine6dB) for a
 *  source that will be summed to mono, -4.5 dB for the desk compromise,
 *  Balance for a stereo balance control. */
template <typename T>
class PannerT {
public:
  /** The position, clamped to -1 .. +1; centred until it is set. Computes a
   *  sine and a cosine; the change is not smoothed. */
  void setPan(T pan) {
    pan_ = clampT<T>(pan, -T(1.0), T(1.0));
    const T p = (pan_ + T(1.0)) * T(0.5);
    panLawGains(law_, p, left_, right_);
  }
  /** The pan law: PanLaw::ConstantPower until set. Recomputes the two gains
   *  at the position last set; unsmoothed, like setPan(). */
  void setLaw(PanLaw law) {
    law_ = law;
    setPan(pan_);
  }
  /** The law setLaw() chose. */
  PanLaw law() const { return law_; }
  /** A mono sample out to a stereo pair at the set position. */
  inline void process(T in, T& left, T& right) const {
    left = in * left_;
    right = in * right_;
  }
  /** A stereo pair scaled by the same two gains, in place: a balance
   *  control on the same law, so at the centre both sides are 3 dB down
   *  (and not at all with PanLaw::Balance, the law that is for this). */
  inline void processStereo(T& left, T& right) const {
    left *= left_;
    right *= right_;
  }

private:
  T left_ = T(0.7071), right_ = T(0.7071);
  T pan_ = T(0.0);
  PanLaw law_ = PanLaw::ConstantPower;
};

/** Panner in float, the form every plugin written before the templates
 *  uses; PannerT<double> is the same processor in double. */
using Panner = PannerT<float>;

} // namespace sonore
