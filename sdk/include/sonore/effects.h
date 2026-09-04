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
 * The delay lengths are mutually prime primes at 44.1 kHz: the classic choice,
 * because common factors make the comb resonances line up and the tail rings on
 * a pitch instead of decaying evenly.
 */
class Reverb {
public:
  struct Parameters {
    float roomSize = 0.6f;  // 0..1, maps to comb feedback
    float damping = 0.4f;   // 0..1, HF loss per pass
    float width = 1.0f;     // 0..1 stereo spread
    float mix = 0.3f;       // 0..1 dry/wet
  };

  void prepare(const ProcessSpec& spec) {
    sampleRate_ = (float) spec.sampleRate;
    // The tunings are quoted at 44.1 kHz; scale so the room keeps its size at
    // any rate rather than shrinking as the host's rate rises.
    const float scale = sampleRate_ / 44100.0f;
    for (int c = 0; c < 2; ++c) {
      // ~23 samples of offset is what decorrelates the two sides.
      const float spread = c == 0 ? 0.0f : 23.0f;
      for (int i = 0; i < kNumCombs; ++i) combDelay_[c][i] = kCombTuning[i] * scale + spread;
      for (int i = 0; i < kNumAllpasses; ++i) {
        allpassDelay_[c][i] = kAllpassTuning[i] * scale + spread;
        // Found by audit: this array was computed and never APPLIED: the
        // allpasses ran at their 100-sample defaults at every rate, so the
        // diffusion was wrong and did not scale. Everything still sounded
        // like a reverb, which is exactly why nobody heard it.
        allpass_[c][i].set(allpassDelay_[c][i], 0.5f);
      }
    }
    reset();
    setParameters(params_);
  }

  void reset() {
    for (int c = 0; c < 2; ++c) {
      for (int i = 0; i < kNumCombs; ++i) {
        comb_[c][i].reset();
        combStore_[c][i] = 0.0f;
      }
      for (int i = 0; i < kNumAllpasses; ++i) allpass_[c][i].reset();
    }
  }

  void setParameters(const Parameters& p) {
    params_ = p;
    feedback_ = 0.7f + clampf(p.roomSize, 0.0f, 1.0f) * 0.28f; // 0.70 .. 0.98
    damp_ = clampf(p.damping, 0.0f, 1.0f) * 0.4f;
    mixer_.setMix(clampf(p.mix, 0.0f, 1.0f));
  }

  /** One stereo frame, in place. */
  inline void process(float& left, float& right) {
    const float dryL = left, dryR = right;
    const float input = (dryL + dryR) * 0.015f; // the classic input gain

    float wet[2] = {0.0f, 0.0f};
    for (int c = 0; c < 2; ++c) {
      for (int i = 0; i < kNumCombs; ++i) {
        const float delayed = comb_[c][i].read(combDelay_[c][i]);
        // One-pole damping inside the loop: each pass loses more highs, which
        // is what makes a tail sound like a room rather than a metal pipe.
        combStore_[c][i] = flushDenormal(delayed * (1.0f - damp_) + combStore_[c][i] * damp_);
        comb_[c][i].write(flushDenormal(input + combStore_[c][i] * feedback_));
        wet[c] += delayed;
      }
      for (int i = 0; i < kNumAllpasses; ++i) wet[c] = allpass_[c][i].process(wet[c]);
    }

    // Width: 1 = fully decorrelated, 0 = mono tail.
    const float w = clampf(params_.width, 0.0f, 1.0f);
    const float wetL = wet[0] * w + wet[1] * (1.0f - w);
    const float wetR = wet[1] * w + wet[0] * (1.0f - w);

    left = mixer_.process(dryL, wetL);
    right = mixer_.process(dryR, wetR);
  }

private:
  static constexpr int kNumCombs = 4;
  static constexpr int kNumAllpasses = 2;
  // Mutually prime lengths, in samples at 44.1 kHz.
  static constexpr float kCombTuning[kNumCombs] = {1116.0f, 1188.0f, 1277.0f, 1356.0f};
  static constexpr float kAllpassTuning[kNumAllpasses] = {556.0f, 441.0f};

  // Sized for 192 kHz: the tunings scale with the rate, and the longest comb
  // is 1356 samples at 44.1k -> ~5927 at 192k. The old 3072-sample lines
  // CLAMPED there, which silently shrank the room at high rates: the worst
  // kind of wrong, because everything still sounds like a reverb.
  DelayLine<6144> comb_[2][kNumCombs];
  Allpass<2560> allpass_[2][kNumAllpasses];
  float combDelay_[2][kNumCombs]{};
  float allpassDelay_[2][kNumAllpasses]{};
  float combStore_[2][kNumCombs]{};
  float sampleRate_ = 48000.0f;
  float feedback_ = 0.84f, damp_ = 0.16f;
  Parameters params_{};
  DryWetMixer mixer_;
};

// ── Modulation ───────────────────────────────────────────────────────────────

/**
 * Chorus / flanger. The difference is only depth and delay range: a flanger
 * modulates a very short delay (under ~10 ms) with feedback to make the comb
 * notches audible; a chorus uses a longer one and no feedback so it thickens
 * rather than whooshes. One implementation covers both.
 */
class Chorus {
public:
  void prepare(const ProcessSpec& spec) {
    sampleRate_ = (float) spec.sampleRate;
    lfo_.setSampleRate(sampleRate_);
    lfo_.setFreq(rate_);
    for (int c = 0; c < 2; ++c) line_[c].reset();
  }

  void setRate(float hz) { rate_ = hz; lfo_.setFreq(hz); }
  void setDepth(float ms) { depthMs_ = ms; }
  void setCentreDelay(float ms) { centreMs_ = ms; }
  void setFeedback(float fb) { feedback_ = clampf(fb, -0.95f, 0.95f); }
  void setMix(float mix) { mixer_.setMix(mix); }

  inline void process(float& left, float& right) {
    // Quadrature between the sides -- sine on the left, cosine on the right --
    // which is what makes a chorus wide: the two delays are never both at
    // rest at once, so the sides' pitch wobbles never line up and the comb
    // notches of a flanger never coincide. (The first version used -sine for
    // the right side and called it quadrature: that is antiphase, and both
    // sides sit still at the same instant twice a cycle.)
    const float turns = lfo_.phase();
    const float phase = lfo_.sine();
    const float cosine = fastmath::sinTurns(turns + 0.25f);
    const float dryL = left, dryR = right;
    for (int c = 0; c < 2; ++c) {
      const float mod = c == 0 ? phase : cosine;
      const float ms = centreMs_ + mod * depthMs_ * 0.5f;
      // The ceiling comes from the BUFFER, not from a constant: 45 ms is the
      // musical maximum, but at a rate where that no longer fits, the honest
      // behaviour is a stated shorter maximum rather than a silent clamp
      // deep inside the delay line.
      const float maxMs = (float) (kChorusBuffer - 8) * 1000.0f / sampleRate_;
      const float samples =
          clampf(ms, 0.1f, maxMs < 45.0f ? maxMs : 45.0f) * 0.001f * sampleRate_;
      // CUBIC, not linear, and this is the one place in the SDK where that
      // matters. Linear interpolation's magnitude response depends on the
      // fractional part of the delay, so a delay that SWEEPS sweeps a lowpass
      // with it -- measured here at 0.66 dB down at 8 kHz, 1.58 dB at 16 kHz
      // against the third-order read. A static delay loses nothing, which is
      // why the reverb combs and the limiter lookahead still use read().
      //
      // readCubic() had documented exactly this and had no callers at all.
      const float wet = line_[c].readCubic(samples);
      const float in = (c == 0 ? dryL : dryR) + wet * feedback_;
      line_[c].write(flushDenormal(in));
      if (c == 0) left = mixer_.process(dryL, wet);
      else right = mixer_.process(dryR, wet);
    }
  }

private:
  static constexpr int kChorusBuffer = 8192; // 45 ms up to 96k; ~42 ms at 192k
  DelayLine<kChorusBuffer> line_[2];
  Oscillator lfo_;
  DryWetMixer mixer_;
  float sampleRate_ = 48000.0f;
  float rate_ = 0.6f, depthMs_ = 6.0f, centreMs_ = 14.0f, feedback_ = 0.0f;
};

/**
 * Phaser: a chain of modulated allpass stages mixed with the dry signal. The
 * notches come from cancellation between the two paths, so unlike a flanger the
 * spacing is not harmonic, which is what makes it sound like sweeping rather
 * than like a jet.
 */
class Phaser {
public:
  void prepare(const ProcessSpec& spec) {
    sampleRate_ = (float) spec.sampleRate;
    lfo_.setSampleRate(sampleRate_);
    lfo_.setFreq(rate_);
    for (int c = 0; c < 2; ++c)
      for (int s = 0; s < kStages; ++s) {
        stage_[c][s].setSampleRate(sampleRate_);
        stage_[c][s].reset();
      }
  }

  void setRate(float hz) { rate_ = hz; lfo_.setFreq(hz); }
  void setDepth(float d) { depth_ = clampf(d, 0.0f, 1.0f); }
  void setFeedback(float fb) { feedback_ = clampf(fb, -0.95f, 0.95f); }
  void setMix(float mix) { mixer_.setMix(mix); }

  inline void process(float& left, float& right) {
    const float lfo = lfo_.sine() * 0.5f + 0.5f;   // 0..1
    // Sweep logarithmically: linear frequency movement sounds like it slows
    // down at the top, because pitch is exponential.
    const float freq = 200.0f * std::pow(40.0f, lfo * depth_);
    float* io[2] = {&left, &right};
    for (int c = 0; c < 2; ++c) {
      float x = *io[c] + fbStore_[c] * feedback_;
      for (int s = 0; s < kStages; ++s) {
        stage_[c][s].allpass(freq * (1.0f + 0.1f * s), 0.7f);
        x = stage_[c][s].process(x);
      }
      fbStore_[c] = flushDenormal(x);
      *io[c] = mixer_.process(*io[c], x);
    }
  }

private:
  static constexpr int kStages = 4;
  Biquad stage_[2][kStages];
  Oscillator lfo_;
  DryWetMixer mixer_;
  float sampleRate_ = 48000.0f;
  float rate_ = 0.5f, depth_ = 0.7f, feedback_ = 0.3f;
  float fbStore_[2] = {0.0f, 0.0f};
};

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
// The default look-ahead buffer holds 3 ms up to 340 kHz: the old 512 ran out
// just past 170 kHz and silently shortened the look-ahead.
template <int MaxLookaheadSamples = 1024>
class Limiter {
public:
  void prepare(const ProcessSpec& spec) {
    sampleRate_ = (float) spec.sampleRate;
    setRelease(releaseMs_);
    setLookahead(lookaheadMs_);
  }

  void setCeiling(float db) { ceiling_ = dbToGain(db); }
  void setRelease(float ms) {
    releaseMs_ = ms;
    release_ = std::exp(-1.0f / (0.001f * (ms > 0.1f ? ms : 0.1f) * sampleRate_));
  }
  /** Rounded to whole samples and clamped to the buffer. Clears the state:
   *  a window that changes length mid-stream would leave the sliding minimum
   *  spanning the wrong samples. */
  void setLookahead(float ms) {
    lookaheadMs_ = ms;
    int n = (int) (ms * 0.001f * sampleRate_ + 0.5f);
    if (n < 1) n = 1;
    if (n > kMaxWindow - 1) n = kMaxWindow - 1;
    lookahead_ = n;
    reset();
  }
  void reset() {
    for (int c = 0; c < 2; ++c) delay_[c].reset();
    for (int i = 0; i < kMaxWindow; ++i) {
      targets_[i] = 1.0f;
      box_[i] = 1.0f;
    }
    head_ = tail_ = 0;
    counter_ = 0;
    boxPos_ = 0;
    boxSum_ = (double) (lookahead_ + 1);
    gain_ = 1.0f;
  }
  /** Latency the host must be told about: the look-ahead IS a delay. */
  int latencySamples() const { return lookahead_; }
  float gainReduction() const { return gainToDb(gain_); }
  /** The gain applied to the sample that just left, before the safety clamp. */
  float lastGain() const { return gain_; }

  inline void process(float& left, float& right) {
    const float peak = std::fabs(left) > std::fabs(right) ? std::fabs(left) : std::fabs(right);
    const float target = peak > ceiling_ ? ceiling_ / peak : 1.0f;
    const int window = lookahead_ + 1; // this sample and the N before it

    // ── Sliding minimum over the last `window` targets ──
    // The deque holds counters of candidate minima in increasing order of
    // value; a new target evicts every older candidate it undercuts, and the
    // front expires once it is older than the window.
    const uint32_t now = counter_++;
    targets_[now % (uint32_t) kMaxWindow] = target;
    while (head_ != tail_) {
      const uint32_t back = deque_[(tail_ + kMaxWindow - 1) % kMaxWindow];
      if (targets_[back % (uint32_t) kMaxWindow] < target) break;
      tail_ = (tail_ + kMaxWindow - 1) % kMaxWindow;
    }
    deque_[tail_] = now;
    tail_ = (tail_ + 1) % kMaxWindow;
    while (head_ != tail_ && deque_[head_] + (uint32_t) window <= now) head_ = (head_ + 1) % kMaxWindow;
    const float minimum = targets_[deque_[head_] % (uint32_t) kMaxWindow];

    // ── Boxcar of the same length: the attack ramp ──
    boxSum_ += (double) minimum - (double) box_[boxPos_];
    box_[boxPos_] = minimum;
    if (++boxPos_ >= window) boxPos_ = 0;
    const float smooth = (float) (boxSum_ / (double) window);

    // ── Release: down with the ramp, up exponentially ──
    gain_ = smooth < gain_ ? smooth : smooth + release_ * (gain_ - smooth);
    if (gain_ > 1.0f) gain_ = 1.0f;

    const float dl = delay_[0].tap((float) lookahead_, left);
    const float dr = delay_[1].tap((float) lookahead_, right);
    left = dl * gain_;
    right = dr * gain_;
    // The construction above guarantees the ceiling; the clamp is for the
    // last ulp of float rounding, and the test asserts it never has to act.
    left = clampf(left, -ceiling_, ceiling_);
    right = clampf(right, -ceiling_, ceiling_);
  }

private:
  static constexpr int kMaxWindow = MaxLookaheadSamples + 2;

  DelayLine<MaxLookaheadSamples> delay_[2];
  float targets_[kMaxWindow]{};
  uint32_t deque_[kMaxWindow]{};
  float box_[kMaxWindow]{};
  double boxSum_ = 1.0;
  uint32_t counter_ = 0;
  int head_ = 0, tail_ = 0, boxPos_ = 0;
  float sampleRate_ = 48000.0f;
  float ceiling_ = 0.98f, gain_ = 1.0f, release_ = 0.999f;
  float releaseMs_ = 50.0f, lookaheadMs_ = 3.0f;
  int lookahead_ = 144;
};

/**
 * Noise gate with hysteresis and a hold time.
 *
 * One threshold chatters: a signal sitting exactly at it opens and closes every
 * few samples. A separate (lower) close threshold plus a minimum hold is what
 * makes a gate usable on a real drum track.
 */
class NoiseGate {
public:
  void prepare(const ProcessSpec& spec) {
    sampleRate_ = (float) spec.sampleRate;
    env_.setSampleRate(sampleRate_);
    env_.setTimes(1.0f, 50.0f);
    setTimes(attackMs_, holdMs_, releaseMs_);
    gain_ = 0.0f;
    open_ = false;
    holdLeft_ = 0;
  }

  void setThreshold(float db) {
    thresholdDb_ = db;
    openThresh_ = dbToGain(db);
    closeThresh_ = dbToGain(db - hysteresisDb_);
  }
  // Recomputes closeThresh_ from the STORED threshold. Before this it only set
  // hysteresisDb_, so turning the hysteresis knob did nothing until the
  // threshold knob was next touched -- a dead control, because the threshold
  // in dB was never kept to recompute from.
  void setHysteresis(float db) {
    hysteresisDb_ = db > 0.0f ? db : 0.0f;
    closeThresh_ = dbToGain(thresholdDb_ - hysteresisDb_);
  }
  void setTimes(float attackMs, float holdMs, float releaseMs) {
    attackMs_ = attackMs; holdMs_ = holdMs; releaseMs_ = releaseMs;
    attack_ = 1.0f - std::exp(-1.0f / (0.001f * (attackMs > 0.05f ? attackMs : 0.05f) * sampleRate_));
    release_ = std::exp(-1.0f / (0.001f * (releaseMs > 0.05f ? releaseMs : 0.05f) * sampleRate_));
    holdSamples_ = (int) (0.001f * holdMs * sampleRate_);
  }
  bool isOpen() const { return open_; }

  inline float process(float x) {
    const float level = env_.process(x);
    if (!open_ && level > openThresh_) {
      open_ = true;
      holdLeft_ = holdSamples_;
    } else if (open_ && level < closeThresh_) {
      if (holdLeft_ > 0) --holdLeft_;
      else open_ = false;
    } else if (open_) {
      holdLeft_ = holdSamples_;
    }
    const float target = open_ ? 1.0f : 0.0f;
    gain_ = open_ ? gain_ + (target - gain_) * attack_ : gain_ * release_;
    gain_ = flushDenormal(gain_);
    return x * gain_;
  }

private:
  EnvFollower env_;
  float sampleRate_ = 48000.0f;
  float openThresh_ = 0.01f, closeThresh_ = 0.005f, hysteresisDb_ = 6.0f;
  float thresholdDb_ = -40.0f; // kept so setHysteresis can recompute closeThresh_
  float attackMs_ = 1.0f, holdMs_ = 50.0f, releaseMs_ = 100.0f;
  float attack_ = 0.5f, release_ = 0.999f, gain_ = 0.0f;
  int holdSamples_ = 2400, holdLeft_ = 0;
  bool open_ = false;
};

// ── Stereo + routing ─────────────────────────────────────────────────────────

/**
 * Linkwitz-Riley 4th-order crossover: two cascaded Butterworth sections per
 * band. The defining property is that the low and high outputs sum FLAT, which
 * is what makes it the right tool for multiband processing: a plain pair of
 * filters leaves a peak or a dip at the crossover point.
 */
class LinkwitzRiley {
public:
  void setSampleRate(float sr) {
    sr_ = sr;
    for (int i = 0; i < 2; ++i) {
      lp_[i].setSampleRate(sr);
      hp_[i].setSampleRate(sr);
    }
    setCrossover(freq_);
  }
  void setCrossover(float hz) {
    freq_ = hz;
    for (int i = 0; i < 2; ++i) {
      lp_[i].lowpass(hz, 0.7071f);
      hp_[i].highpass(hz, 0.7071f);
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
  static int tailSamples(float sr, float crossoverHz, float floorDb = -80.0f) {
    const float fc = crossoverHz > 1.0f ? crossoverHz : 1.0f;
    const float sigma = 2.0f * kPi * fc * 0.70710678f; // w0 / 2Q, the pole's real part

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
    const float target = -std::log(std::pow(10.0f, floorDb * 0.05f)); // -ln(floor)
    float x = target;
    for (int i = 0; i < 24; ++i) {
      const float f = std::log(1.0f + x) - x + target;
      const float df = 1.0f / (1.0f + x) - 1.0f;
      if (std::fabs(df) < 1e-9f) break;
      const float step = f / df;
      x -= step;
      if (std::fabs(step) < 1e-6f) break;
    }

    // The model lands within 2% of the measured decay across the whole range
    // the control covers; the margin is for that 2% and for the assumption
    // that the state was at full scale when the input stopped. Erring long
    // costs a host a few milliseconds of extra render, erring short costs the
    // user the end of their sound.
    return (int) std::ceil(1.1f * x / sigma * sr);
  }
  void reset() {
    for (int i = 0; i < 2; ++i) { lp_[i].reset(); hp_[i].reset(); }
  }
  /** Split one sample into a low and a high band that sum back to it. */
  inline void process(float x, float& low, float& high) {
    low = lp_[1].process(lp_[0].process(x));
    high = hp_[1].process(hp_[0].process(x));
  }

private:
  Biquad lp_[2], hp_[2];
  float sr_ = 48000.0f, freq_ = 1000.0f;
};

/** Mid/side width control. Width 0 collapses to mono, 1 is untouched, >1
 *  exaggerates the sides. Mono compatibility is why this works on M/S rather
 *  than by panning: the mid is preserved exactly. */
class StereoWidener {
public:
  void setWidth(float w) { width_ = w < 0.0f ? 0.0f : (w > 2.0f ? 2.0f : w); }
  inline void process(float& left, float& right) const {
    const float mid = (left + right) * 0.5f;
    const float side = (left - right) * 0.5f * width_;
    left = mid + side;
    right = mid - side;
  }

private:
  float width_ = 1.0f;
};

/** Constant-power panner. `pan` is -1 (left) .. 0 (centre) .. +1 (right);
 *  the -3 dB centre law keeps perceived loudness constant across the sweep. */
class Panner {
public:
  void setPan(float pan) {
    const float p = (clampf(pan, -1.0f, 1.0f) + 1.0f) * 0.5f;
    left_ = std::cos(p * kPi * 0.5f);
    right_ = std::sin(p * kPi * 0.5f);
  }
  inline void process(float in, float& left, float& right) const {
    left = in * left_;
    right = in * right_;
  }
  inline void processStereo(float& left, float& right) const {
    left *= left_;
    right *= right_;
  }

private:
  float left_ = 0.7071f, right_ = 0.7071f;
};

} // namespace sonore
