// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: the DSP toolkit. Header-only, allocation-free, dependency-free
// (only <cmath>): the building blocks generated plugins are written against.
//
// The SAME source compiles to WebAssembly for the live browser preview and into
// the shipped native binary, which is what makes the preview bit-identical to
// the product. That constraint is why nothing here allocates, locks, or does IO
// after prepare(): a block that isn't real-time safe in a DAW isn't real-time
// safe in an AudioWorklet either.
//
// Everything is `sonore::`: filters, dynamics, modulation, voices. This is our
// own implementation of standard, published DSP (RBJ's EQ cookbook, Simper's
// TPT structures, polyBLEP, Schroeder/Moorer reverb topology): textbook maths,
// written here from the maths rather than adapted from any framework's source.
#pragma once
#include <cmath>
#include <cstdint>
#include "special.h" // fastmath, which the oscillator uses per sample
#include "audio.h"
#include "interpolation.h"

namespace sonore {

// kPi / kTwoPi live in audio.h: see the note there.

inline float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }
inline float dbToGain(float db) { return std::pow(10.0f, db * 0.05f); }
inline float gainToDb(float g) { return 20.0f * std::log10(g > 1e-9f ? g : 1e-9f); }

/** Decibels with an explicit FLOOR, and silence reported as that floor rather
 *  than as -180. A meter that reads "-180 dB" when a track is muted is a
 *  meter that has to be special-cased by whoever draws it; saying "-100"
 *  once, here, means every caller can scale linearly from it. */
inline float gainToDbFloor(float g, float floorDb = -100.0f) {
  const float db = gainToDb(g);
  return db < floorDb ? floorDb : db;
}

/** Its inverse: the floor maps back to true silence, not to a tiny gain that
 *  would leave a muted signal audible on a big system. */
inline float dbToGainFloor(float db, float floorDb = -100.0f) {
  return db <= floorDb ? 0.0f : dbToGain(db);
}
inline float lerp(float a, float b, float t) { return a + (b - a) * t; }

/** Flush denormals. Subnormal floats cost hundreds of cycles on x86 and are the
 *  classic cause of a reverb tail that makes a DAW stutter as it fades out. */
inline float flushDenormal(float x) { return (std::fabs(x) < 1e-18f) ? 0.0f : x; }

// ── Waveshaping ──────────────────────────────────────────────────────────────

inline float softClip(float x) { return x / (1.0f + std::fabs(x)); }
inline float tanhClip(float x) { return std::tanh(x); }
inline float hardClip(float x) { return clampf(x, -1.0f, 1.0f); }

/** Cubic soft saturation with drive (1 = clean-ish, higher = grittier). */
inline float cubic(float x, float drive) {
  const float y = clampf(x * drive, -1.5f, 1.5f);
  return 1.5f * y - 0.5f * y * y * y;
}

/** Asymmetric shape: produces EVEN harmonics (tube-like) where the symmetric
 *  shapers above give odd ones only. `bias` in roughly 0..0.5. */
inline float asymmetric(float x, float bias) {
  const float b = x + bias;
  return std::tanh(b) - std::tanh(bias);
}

// ── Smoothing ────────────────────────────────────────────────────────────────

/** One-pole parameter smoother: wrap user-facing controls to kill zipper noise.
 *  Every control that reaches a coefficient should go through one of these. */
class Smooth {
public:
  void setup(float sampleRate, float ms = 8.0f) {
    a_ = std::exp(-1.0f / (0.001f * (ms > 0.01f ? ms : 0.01f) * sampleRate));
  }
  void snap(float v) { z_ = v; }
  inline float next(float target) {
    z_ = target + a_ * (z_ - target);
    return flushDenormal(z_);
  }
  float value() const { return z_; }

private:
  float a_ = 0.0f, z_ = 0.0f;
};

/**
 * A ramp that is straight in DECIBELS, and that arrives.
 *
 * Smooth is a one-pole: it approaches its target and never reaches it, which
 * is exactly right for a knob somebody is turning and wrong for a fade. Two
 * things go wrong with the obvious alternative, a straight line in amplitude:
 *
 *   It never finishes either, if you build it out of a one-pole; and if you
 *   build it out of a line it finishes but sounds wrong. A linear fade from
 *   unity is already 6 dB down at its halfway point and spends the whole
 *   second half in the bottom 6 dB, so it rushes away and then crawls. Ears
 *   hear loudness logarithmically, so a fade that sounds even has to BE
 *   logarithmic.
 *
 * NOT the same thing as LogSmooth in shaping.h, which is a one-pole working
 * in the log domain -- right for a frequency glide, where a knob is being
 * turned and there is no destination to arrive at. The difference is arrival:
 * this finishes in a stated number of samples and lands exactly, and that one
 * approaches for ever. Both are worth having and both are easy to mistake for
 * the other, which is why this paragraph is here.
 *
 * Multiplying by a constant factor each step is what makes it straight in dB,
 * and it is also what makes zero impossible: no number of multiplications
 * reaches it. A target at or below the floor is treated as the floor and then
 * snapped to exactly at the end, so a fade-out really does end in silence
 * rather than in something inaudible but not zero.
 */
class LogRamp {
public:
  /** The quietest value a ramp will pass through, as an amplitude. -120 dB is
   *  below the noise floor of anything this will ever fade. */
  static constexpr float kFloor = 1.0e-6f;

  void setup(float sampleRate, float milliseconds) {
    const float ms = milliseconds > 0.01f ? milliseconds : 0.01f;
    steps_ = (int) (0.001f * ms * (sampleRate > 0.0f ? sampleRate : 48000.0f));
    if (steps_ < 1) steps_ = 1;
  }

  /** Jump. For a state restore, where nothing is mid-flight to smooth. */
  void snap(float value) {
    current_ = value;
    target_ = value;
    remaining_ = 0;
    factor_ = 1.0f;
  }

  void setTarget(float target) {
    if (target == target_ && remaining_ > 0) return;
    target_ = target;
    snapTo_ = target;

    const float from = magnitude(current_);
    const float to = magnitude(target);
    if (steps_ <= 0 || from <= kFloor) {
      // Nothing to ramp FROM: a multiplicative ramp out of silence stays
      // silent for ever, so this is the one case that has to jump.
      current_ = target;
      remaining_ = 0;
      factor_ = 1.0f;
      return;
    }
    remaining_ = steps_;
    factor_ = (float) std::pow((double) to / (double) from, 1.0 / (double) steps_);
  }

  inline float next() {
    if (remaining_ <= 0) return current_;
    current_ *= factor_;
    if (--remaining_ == 0) current_ = snapTo_; // land exactly, not nearly
    return current_;
  }

  float value() const { return current_; }
  float target() const { return target_; }
  bool isRamping() const { return remaining_ > 0; }

private:
  static float magnitude(float v) {
    const float a = v < 0.0f ? -v : v;
    return a < kFloor ? kFloor : a;
  }

  float current_ = 0.0f, target_ = 0.0f, snapTo_ = 0.0f, factor_ = 1.0f;
  int steps_ = 0, remaining_ = 0;
};

// ── Filters ──────────────────────────────────────────────────────────────────

/**
 * One-pole low/high pass: gentle 6 dB/oct tone control.
 *
 * The TRAPEZOIDAL (bilinear, pre-warped) one-pole, so the -3 dB point is the
 * cutoff at every frequency up to Nyquist. The first version was the Euler
 * form, z += a (x - z) with a = wc / (1 + wc), whose corner sits where the
 * approximation puts it: "6 kHz" measured 4.5 kHz and "12 kHz" 7.8 kHz at
 * 48 kHz. Every damping control in this SDK -- the reverb combs, the FDN, the
 * delay, the spring, the string -- sits on this filter, so all of them read
 * a third of an octave low at the top until it was measured.
 *
 * H(z) = G (1 + z^-1) / (1 - (1 - 2G) z^-1) with G = g / (1 + g) and
 * g = tan(pi fc / fs): the RC lowpass under the bilinear transform, which the
 * response helpers below evaluate exactly, so a loop that contains one can
 * subtract its phase delay and be in tune.
 */
class OnePole {
public:
  void setSampleRate(float sr) { sr_ = sr > 1.0f ? sr : 48000.0f; }
  inline void setCutoff(float hz) {
    const float g = std::tan(kPi * clampf(hz, 1.0f, sr_ * 0.49f) / sr_);
    G_ = g / (1.0f + g);
  }
  inline float lp(float x) {
    const float v = (x - z_) * G_;
    const float y = v + z_;
    z_ = flushDenormal(y + v);
    return y;
  }
  inline float hp(float x) { return x - lp(x); }
  void reset() { z_ = 0.0f; }

  /** The lowpass's gain in dB at `hz`, from its own transfer function. */
  float magnitudeDb(float hz) const {
    const double w = kTwoPi * (double) hz / (double) sr_;
    const double p = 1.0 - 2.0 * (double) G_;
    const double num = (double) G_ * std::sqrt(2.0 + 2.0 * std::cos(w));
    const double den = std::sqrt(1.0 - 2.0 * p * std::cos(w) + p * p);
    const double mag = den > 1e-30 ? num / den : 0.0;
    return (float) (20.0 * std::log10(mag > 1e-15 ? mag : 1e-15));
  }
  /** The lowpass's PHASE DELAY at `hz`, in samples: what a feedback loop
   *  that contains it must subtract from its length to be in tune. Tends to
   *  (1 - G) / 2G at low frequency, which is 1/wc, the RC's own group delay. */
  float phaseDelaySamples(float hz) const {
    const double w = kTwoPi * (double) hz / (double) sr_;
    if (w < 1e-9) return (float) ((1.0 - (double) G_) / (2.0 * (double) G_));
    const double p = 1.0 - 2.0 * (double) G_;
    // The zero at Nyquist contributes half a sample; the pole the rest.
    return (float) (0.5 + std::atan2(p * std::sin(w), 1.0 - p * std::cos(w)) / w);
  }
  float coefficient() const { return G_; }

private:
  float sr_ = 48000.0f, G_ = 0.1f, z_ = 0.0f;
};

/** Transposed-Direct-Form-II biquad with RBJ cookbook coefficients. */
class Biquad {
public:
  void setSampleRate(float sr) { sr_ = sr; }
  void reset() { z1_ = z2_ = 0.0f; }
  inline float process(float x) {
    const float y = b0_ * x + z1_;
    z1_ = flushDenormal(b1_ * x - a1_ * y + z2_);
    z2_ = flushDenormal(b2_ * x - a2_ * y);
    return y;
  }
  void lowpass(float freq, float q) { rbj(freq, q, 0); }
  void highpass(float freq, float q) { rbj(freq, q, 1); }
  void bandpass(float freq, float q) { rbj(freq, q, 2); }
  void notch(float freq, float q) { rbj(freq, q, 3); }
  void allpass(float freq, float q) { rbj(freq, q, 4); }
  void peak(float freq, float q, float gainDb) { rbjPeak(freq, q, gainDb); }
  void lowShelf(float freq, float q, float gainDb) { rbjShelf(freq, q, gainDb, false); }
  void highShelf(float freq, float q, float gainDb) { rbjShelf(freq, q, gainDb, true); }

  /** Coefficients from somewhere else, already normalised by a0.
   *
   *  The cookbook covers what one biquad can be on its own. A section of a
   *  higher-order design is not one of those shapes -- its Q comes from where
   *  its pole pair sits on the Butterworth circle -- so the design lives in
   *  filter_design.h and hands the numbers here rather than the shapes being
   *  reinvented as named methods. */
  void setCoefficients(float b0, float b1, float b2, float a1, float a2) {
    b0_ = b0; b1_ = b1; b2_ = b2; a1_ = a1; a2_ = a2;
  }

  /** The section's gain at a frequency, in dB, from its own coefficients.
   *
   *  Evaluating H(z) on the unit circle rather than sweeping a sine through
   *  it: a caller drawing a response curve should not have to run audio, and
   *  a TEST that does run audio then has something independent to disagree
   *  with. */
  float magnitudeDb(float freq) const {
    const double w = kTwoPi * (double) freq / (double) sr_;
    const double cw = std::cos(w), sw = std::sin(w);
    const double c2 = std::cos(2.0 * w), s2 = std::sin(2.0 * w);
    const double nr = (double) b0_ + (double) b1_ * cw + (double) b2_ * c2;
    const double ni = -((double) b1_ * sw + (double) b2_ * s2);
    const double dr = 1.0 + (double) a1_ * cw + (double) a2_ * c2;
    const double di = -((double) a1_ * sw + (double) a2_ * s2);
    const double num = std::sqrt(nr * nr + ni * ni);
    const double den = std::sqrt(dr * dr + di * di);
    if (den < 1e-30) return -300.0f;
    const double mag = num / den;
    return (float) (20.0 * std::log10(mag > 1e-15 ? mag : 1e-15));
  }

private:
  void norm(float a0, float a1, float a2, float b0, float b1, float b2) {
    b0_ = b0 / a0; b1_ = b1 / a0; b2_ = b2 / a0; a1_ = a1 / a0; a2_ = a2 / a0;
  }
  void rbj(float freq, float q, int type) {
    const float w0 = kTwoPi * clampf(freq, 10.0f, sr_ * 0.49f) / sr_;
    const float cw = std::cos(w0), sw = std::sin(w0);
    const float alpha = sw / (2.0f * (q < 0.05f ? 0.05f : q));
    const float a0 = 1 + alpha, a1 = -2 * cw, a2 = 1 - alpha;
    if (type == 0)      norm(a0, a1, a2, (1 - cw) * 0.5f, 1 - cw, (1 - cw) * 0.5f);
    else if (type == 1) norm(a0, a1, a2, (1 + cw) * 0.5f, -(1 + cw), (1 + cw) * 0.5f);
    else if (type == 2) norm(a0, a1, a2, alpha, 0, -alpha);
    else if (type == 3) norm(a0, a1, a2, 1, -2 * cw, 1);
    else                norm(a0, a1, a2, 1 - alpha, -2 * cw, 1 + alpha);
  }
  void rbjPeak(float freq, float q, float gainDb) {
    const float A = std::pow(10.0f, gainDb / 40.0f);
    const float w0 = kTwoPi * clampf(freq, 10.0f, sr_ * 0.49f) / sr_;
    const float cw = std::cos(w0), sw = std::sin(w0);
    const float alpha = sw / (2.0f * (q < 0.05f ? 0.05f : q));
    norm(1 + alpha / A, -2 * cw, 1 - alpha / A, 1 + alpha * A, -2 * cw, 1 - alpha * A);
  }
  void rbjShelf(float freq, float q, float gainDb, bool high) {
    const float A = std::pow(10.0f, gainDb / 40.0f);
    const float w0 = kTwoPi * clampf(freq, 10.0f, sr_ * 0.49f) / sr_;
    const float cw = std::cos(w0), sw = std::sin(w0);
    const float alpha = sw / (2.0f * (q < 0.05f ? 0.05f : q));
    const float tsa = 2.0f * std::sqrt(A) * alpha, Ap1 = A + 1.0f, Am1 = A - 1.0f;
    if (!high)
      norm(Ap1 + Am1 * cw + tsa, -2 * (Am1 + Ap1 * cw), Ap1 + Am1 * cw - tsa,
           A * (Ap1 - Am1 * cw + tsa), 2 * A * (Am1 - Ap1 * cw), A * (Ap1 - Am1 * cw - tsa));
    else
      norm(Ap1 - Am1 * cw + tsa, 2 * (Am1 - Ap1 * cw), Ap1 - Am1 * cw - tsa,
           A * (Ap1 + Am1 * cw + tsa), -2 * A * (Am1 + Ap1 * cw), A * (Ap1 + Am1 * cw - tsa));
  }
  float sr_ = 48000.0f;
  float b0_ = 1, b1_ = 0, b2_ = 0, a1_ = 0, a2_ = 0, z1_ = 0, z2_ = 0;
};

/** State-variable filter (TPT topology): stable under heavy modulation and
 *  gives LP/HP/BP/notch simultaneously. The default filter for anything swept. */
class SVF {
public:
  void setSampleRate(float sr) { sr_ = sr; }
  inline void set(float freq, float q) {
    g_ = std::tan(kPi * clampf(freq, 10.0f, sr_ * 0.49f) / sr_);
    k_ = 1.0f / (q < 0.05f ? 0.05f : q);
    a1_ = 1.0f / (1.0f + g_ * (g_ + k_));
    a2_ = g_ * a1_;
    a3_ = g_ * a2_;
  }
  inline void process(float x) {
    const float v3 = x - ic2_;
    const float v1 = a1_ * ic1_ + a2_ * v3;
    const float v2 = ic2_ + a2_ * ic1_ + a3_ * v3;
    ic1_ = flushDenormal(2 * v1 - ic1_);
    ic2_ = flushDenormal(2 * v2 - ic2_);
    lp = v2; bp = v1; hp = x - k_ * v1 - v2; notch = x - k_ * v1;
  }
  float lp = 0, hp = 0, bp = 0, notch = 0;
  void reset() { ic1_ = ic2_ = 0; }

private:
  float sr_ = 48000.0f, g_ = 0, k_ = 1, a1_ = 0, a2_ = 0, a3_ = 0, ic1_ = 0, ic2_ = 0;
};

/**
 * Four-pole resonant ladder (24 dB/oct): the Moog transistor ladder, the
 * classic analogue-synth voice filter.
 *
 * ZERO-DELAY FEEDBACK, solved, not approximated: each TPT stage's output is
 * G·u + (1-G)·s, so the fourth stage's output is G^4·u plus a sum of the four
 * states weighted by the later stages' G's, and the loop equation
 * u = x - k·y4 has the closed form u = (x - k·S) / (1 + k·G^4) (Zavalishin,
 * "The Art of VA Filter Design"). The first version fed back the PREVIOUS
 * sample's fourth state -- a unit delay in the loop -- which is the classic
 * shortcut, and it detunes: the resonant peak and the self-oscillation
 * frequency slide away from the cutoff as the cutoff rises, by 17% at 8 kHz.
 * With the solve, the peak sits at the number on the knob at any cutoff.
 *
 * The nonlinearity is an amplitude-dependent feedback gain: k is scaled by
 * tanh(A)/A of the last output, so a quiet signal sees the exact linear
 * ladder and a loud one -- or the self-oscillation past k = 4 -- sees its
 * feedback compressed and settles at a bounded amplitude. Applying tanh to
 * the loop signal itself would put 24% of harmonic distortion on a
 * full-scale input at the filter's INPUT, which is a saturator, not a
 * filter. setSaturation(0) removes it, and then k is held under 4 because a
 * linear ladder past the threshold is an exponential.
 */
class LadderFilter {
public:
  void setSampleRate(float sr) { sr_ = sr > 1.0f ? sr : 48000.0f; }
  void reset() {
    for (int i = 0; i < 4; ++i) s_[i] = 0.0f;
    last_ = 0.0f;
  }
  /** resonance 0..1 maps onto k = 0..4.2: the threshold is 4, so the top of
   *  the control self-oscillates (bounded by the saturation). */
  inline void set(float cutoffHz, float resonance) {
    const float g = std::tan(kPi * clampf(cutoffHz, 20.0f, sr_ * 0.45f) / sr_);
    G_ = g / (1.0f + g);
    G2_ = G_ * G_;
    G3_ = G2_ * G_;
    G4_ = G2_ * G2_;
    k_ = 4.2f * clampf(resonance, 0.0f, 1.0f);
    if (sat_ <= 0.0f && k_ > 3.98f) k_ = 3.98f;
  }
  /** Drive into the feedback's tanh; 0 = linear (and then no self-oscillation). */
  void setSaturation(float s) {
    sat_ = clampf(s, 0.0f, 4.0f);
    if (sat_ <= 0.0f && k_ > 3.98f) k_ = 3.98f;
  }
  /** Saturation at every STAGE input (Huovilainen 2004; D'Angelo & Välimäki
   *  2013): the transistor pairs of the real ladder each clip, which is where
   *  the Moog's growl under drive comes from and what the feedback-only
   *  nonlinearity above does not give. 0 = linear stages (the default: a
   *  filter), 1 = tanh at each stage on a full-scale signal. The linear
   *  ZDF solve stays the predictor for the loop, exact for a quiet signal. */
  void setDrive(float d) { drive_ = clampf(d, 0.0f, 4.0f); }
  inline float process(float x) {
    // The feedback gain this sample: k compressed by how loud the loop was.
    float k = k_;
    if (sat_ > 0.0f) {
      const float a = std::fabs(last_) * sat_;
      if (a > 1e-3f) k *= fastmath::tanhApprox(a) / a;
    }
    const float oneMinusG = 1.0f - G_;
    const float sigma = G3_ * oneMinusG * s_[0] + G2_ * oneMinusG * s_[1] + G_ * oneMinusG * s_[2] +
                        oneMinusG * s_[3];
    float v = (x - k * sigma) / (1.0f + k * G4_);
    for (int i = 0; i < 4; ++i) {
      const float in = drive_ > 0.0f ? fastmath::tanhApprox(v * drive_) / drive_ : v;
      const float y = G_ * (in - s_[i]) + s_[i];
      s_[i] = flushDenormal(2.0f * y - s_[i]);
      v = y;
    }
    last_ = v;
    return v;
  }

private:
  float sr_ = 48000.0f, G_ = 0, G2_ = 0, G3_ = 0, G4_ = 0, k_ = 0, sat_ = 1.0f, drive_ = 0.0f, last_ = 0.0f;
  float s_[4] = {0, 0, 0, 0};
};

/** DC blocker: a high-pass at a few Hz. Asymmetric saturation adds DC offset;
 *  the audio gates reject it, so put one after any asymmetric shaper. */
class DcBlocker {
public:
  /** Where the highpass sits. Low enough to leave the bottom octave alone,
   *  which is also why it takes so long to settle: see tailSamples(). */
  static constexpr float kCornerHz = 10.0f;

  void setSampleRate(float sr) { r_ = 1.0f - (kTwoPi * kCornerHz / sr); }

  /** How many samples a full-scale transient takes to decay below `floorDb`.
   *
   *  A plugin that ends its chain in a DC blocker HAS a tail whether or not it
   *  meant to: a 10 Hz corner is a pole at 0.9987 per sample, so it keeps
   *  ringing for well over a hundred milliseconds after the input stops. The
   *  saturator example declared no tail at all until the host test measured
   *  2964 samples of decay coming out of it, which is audio a host truncates
   *  on stop and on export while sounding perfect when monitored.
   *
   *  Returned rather than hardcoded per plugin so the number cannot drift away
   *  from the filter it describes. */
  static int tailSamples(float sr, float floorDb = -80.0f) {
    const float r = 1.0f - (kTwoPi * kCornerHz / (sr > 1.0f ? sr : 1.0f));
    if (r <= 0.0f || r >= 1.0f) return 0;
    return (int) std::ceil(std::log(std::pow(10.0f, floorDb * 0.05f)) / std::log(r));
  }

  inline float process(float x) {
    const float y = x - x1_ + r_ * y1_;
    x1_ = x;
    y1_ = flushDenormal(y);
    return y;
  }
  void reset() { x1_ = y1_ = 0.0f; }

private:
  float r_ = 0.999f, x1_ = 0.0f, y1_ = 0.0f;
};

// ── Oscillators ──────────────────────────────────────────────────────────────

/** polyBLEP band-limited oscillator. The naive shapes alias badly at high notes
 *, the aliasing gate measures exactly that, so saw/square subtract a
 *  polynomial approximation of a band-limited step at each discontinuity. */
class Oscillator {
public:
  void setSampleRate(float sr) { sr_ = sr; }
  inline void setFreq(float hz) { inc_ = clampf(hz, 0.0f, sr_ * 0.5f) / sr_; }
  void reset(float phase = 0.0f) { phase_ = phase; tri_ = 0.0f; }

  inline float sine() {
    // The phase is already in turns, so this is the library call AND the
    // multiply by 2*pi that would have preceded it. A polyphonic synth runs
    // one of these per voice per sample.
    const float y = fastmath::sinTurns(phase_);
    adv();
    return y;
  }
  inline float saw() {
    float y = 2.0f * phase_ - 1.0f;
    y -= blep(phase_);
    adv();
    return y;
  }
  inline float square(float pulseWidth = 0.5f) {
    const float pw = clampf(pulseWidth, 0.02f, 0.98f);
    float y = phase_ < pw ? 1.0f : -1.0f;
    y += blep(phase_);
    float p2 = phase_ - pw;
    if (p2 < 0.0f) p2 += 1.0f;
    y -= blep(p2);
    adv();
    return y;
  }
  /**
   * Triangle by integrating a square -- inherently band-limited, no BLEP.
   *
   * A LEAKY integrator, and that is the whole correctness of it. A plain
   * integrator of a +/-1 square accumulates the integration constant as DC:
   * starting at 0 it swings 0..+2, a permanent +1 offset and never a centred
   * triangle -- which puts near-unity DC into whatever filter or amp follows
   * and trips this SDK's own audio sanity gate (|DC| > 0.25). The tiny leak
   * bleeds that constant away so the wave settles symmetrically on 0, at the
   * cost of a droop far below the fundamental that no ear reaches. tri_ is
   * cleared in reset() so a retriggered voice does not inherit stale state.
   */
  inline float triangle() {
    const float sq = square();
    // Leak scaled by the increment: a fixed leak would flatten a low note and
    // barely touch a high one. This keeps the corner a constant fraction of
    // the fundamental across the range.
    const float leak = 0.5f * inc_;
    tri_ = flushDenormal(tri_ * (1.0f - leak) + 4.0f * inc_ * sq);
    return tri_;
  }
  float phase() const { return phase_; }

private:
  inline void adv() {
    phase_ += inc_;
    if (phase_ >= 1.0f) phase_ -= 1.0f;
  }
  /** Two-sided polynomial BLEP residual around a discontinuity. */
  inline float blep(float t) const {
    if (inc_ <= 0.0f) return 0.0f;
    if (t < inc_) {
      const float x = t / inc_;
      return x + x - x * x - 1.0f;
    }
    if (t > 1.0f - inc_) {
      const float x = (t - 1.0f) / inc_;
      return x * x + x + x + 1.0f;
    }
    return 0.0f;
  }
  float sr_ = 48000.0f, inc_ = 0.0f, phase_ = 0.0f, tri_ = 0.0f;
};

// ── Envelopes + dynamics ─────────────────────────────────────────────────────

/** Peak envelope follower with independent attack/release. */
class EnvFollower {
public:
  void setSampleRate(float sr) { sr_ = sr; }
  void setTimes(float attackMs, float releaseMs) {
    aA_ = std::exp(-1.0f / (0.001f * (attackMs > 0.01f ? attackMs : 0.01f) * sr_));
    aR_ = std::exp(-1.0f / (0.001f * (releaseMs > 0.01f ? releaseMs : 0.01f) * sr_));
  }
  inline float process(float x) {
    const float r = std::fabs(x);
    const float a = r > env_ ? aA_ : aR_;
    env_ = flushDenormal(r + a * (env_ - r));
    return env_;
  }
  float value() const { return env_; }
  void reset() { env_ = 0.0f; }

private:
  float sr_ = 48000.0f, aA_ = 0, aR_ = 0, env_ = 0;
};

/** ADSR in seconds, exponential-ish segments. Drives one voice's amplitude. */
class ADSR {
public:
  struct Parameters {
    float attack = 0.01f, decay = 0.1f, sustain = 0.8f, release = 0.2f;
  };
  void setSampleRate(float sr) { sr_ = sr > 1.0f ? sr : 48000.0f; }
  void setParameters(const Parameters& p) { p_ = p; }
  void noteOn() { stage_ = Attack; }
  void noteOff() { if (stage_ != Idle) stage_ = Release; }
  bool isActive() const { return stage_ != Idle; }
  void reset() { stage_ = Idle; level_ = 0.0f; }

  inline float getNextSample() {
    switch (stage_) {
      case Attack: {
        level_ += 1.0f / (p_.attack > 1e-4f ? p_.attack * sr_ : 1.0f);
        if (level_ >= 1.0f) { level_ = 1.0f; stage_ = Decay; }
        break;
      }
      case Decay: {
        level_ = p_.sustain + decayK() * (level_ - p_.sustain);
        if (std::fabs(level_ - p_.sustain) < 1e-4f) { level_ = p_.sustain; stage_ = Sustain; }
        break;
      }
      case Sustain:
        level_ = p_.sustain;
        break;
      case Release: {
        level_ *= releaseK();
        // -80 dB is inaudible and frees the voice promptly; waiting for a
        // literal zero would keep a stolen voice busy for no audible reason.
        if (level_ < 1e-4f) { level_ = 0.0f; stage_ = Idle; }
        break;
      }
      case Idle:
      default:
        level_ = 0.0f;
        break;
    }
    return level_;
  }

private:
  enum Stage { Idle, Attack, Decay, Sustain, Release };

  /** Exponential segments use the -60 dB convention: the declared time IS the
   *  time to fall 60 dB (ln(1000) = 6.908). Without this the numbers on the
   *  control mean nothing measurable, and `release` is exactly what the
   *  acceptance probe measures on a shipped synth. */
  static constexpr float kDecay60 = 6.907755f;
  float decayK() const {
    return std::exp(-kDecay60 / (p_.decay > 1e-4f ? p_.decay * sr_ : 1.0f));
  }
  float releaseK() const {
    return std::exp(-kDecay60 / (p_.release > 1e-4f ? p_.release * sr_ : 1.0f));
  }

  Stage stage_ = Idle;
  float sr_ = 48000.0f, level_ = 0.0f;
  Parameters p_{};
};

/** Feed-forward compressor with a peak detector, soft knee and makeup.
 *  `gainReduction()` reports the last computed GR in dB: the acceptance gates
 *  measure exactly this number. */
class Compressor {
public:
  void setSampleRate(float sr) {
    sr_ = sr;
    env_.setSampleRate(sr);
    env_.setTimes(attackMs_, releaseMs_);
  }
  void setThreshold(float db) { thresholdDb_ = db; }
  void setRatio(float ratio) { ratio_ = ratio < 1.0f ? 1.0f : ratio; }
  void setKnee(float db) { kneeDb_ = db < 0.0f ? 0.0f : db; }
  void setAttack(float ms) { attackMs_ = ms; env_.setTimes(attackMs_, releaseMs_); }
  void setRelease(float ms) { releaseMs_ = ms; env_.setTimes(attackMs_, releaseMs_); }
  void setMakeup(float db) { makeupDb_ = db; }
  void reset() { env_.reset(); grDb_ = 0.0f; }

  /** Compute the gain for this sample from a (possibly external) detector input. */
  inline float computeGain(float detector) {
    const float levelDb = gainToDb(env_.process(detector));
    const float over = levelDb - thresholdDb_;
    float grDb = 0.0f;
    if (kneeDb_ > 0.0f && over > -kneeDb_ * 0.5f && over < kneeDb_ * 0.5f) {
      // Quadratic soft knee across the transition band.
      const float t = over + kneeDb_ * 0.5f;
      grDb = (1.0f / ratio_ - 1.0f) * (t * t) / (2.0f * kneeDb_);
    } else if (over > 0.0f) {
      grDb = (1.0f / ratio_ - 1.0f) * over;
    }
    grDb_ = grDb;
    return dbToGain(grDb + makeupDb_);
  }
  inline float process(float x) { return x * computeGain(x); }
  /** Last gain reduction, in dB (negative = attenuating). */
  float gainReduction() const { return grDb_; }

private:
  float sr_ = 48000.0f;
  float thresholdDb_ = -18.0f, ratio_ = 4.0f, kneeDb_ = 6.0f;
  float attackMs_ = 10.0f, releaseMs_ = 100.0f, makeupDb_ = 0.0f, grDb_ = 0.0f;
  EnvFollower env_;
};

// ── Delay + space ────────────────────────────────────────────────────────────

/**
 * Fractional delay line. MaxSamples is a compile-time bound so the buffer is
 * inline and nothing allocates.
 *
 * TWO reads, and choosing between them is not a matter of taste:
 *
 *   read()       linear. Correct for a delay whose length does not move --
 *                a reverb comb, a lookahead buffer. Costs nothing there.
 *   readCubic()  third-order. For a delay that SWEEPS. Linear interpolation's
 *                magnitude response depends on the fractional part, so moving
 *                the delay moves a lowpass along with it.
 *
 * The difference, measured through a 3 ms sweep at 48 kHz: nothing below
 * 2 kHz, 0.66 dB at 8 kHz, 1.58 dB at 16 kHz. Through a STATIC delay the two
 * are identical.
 */
template <int MaxSamples>
class DelayLine {
public:
  /** The spline used by readCubic(). Declared here rather than pulled from
   *  shaping.h because dsp.h defines DelayLine BEFORE that header is
   *  chained in: the alternative is a circular include. */
  static inline float catmullRomTap(float y0, float y1, float y2, float y3, float frac) {
    // Forwarded, not copied. The same cubic lived here and in the
    // interpolator family, which is one copy too many for a rule this SDK has
    // already watched drift once.
    return CatmullRomKernel::at4(y0, y1, y2, y3, frac);
  }

  void reset() {
    for (int i = 0; i < MaxSamples; ++i) buf_[i] = 0.0f;
    w_ = 0;
  }
  inline void write(float x) {
    buf_[w_] = x;
    if (++w_ >= MaxSamples) w_ = 0;
  }
  /** Read with THIRD-ORDER interpolation. This is what a modulated delay
   *  should use: linear interpolation's response changes with the fraction,
   *  so sweeping the delay sweeps a lowpass along with it. */
  inline float readCubic(float delaySamples) const {
    const float d = clampf(delaySamples, 2.0f, (float) (MaxSamples - 3));
    float rp = (float) w_ - d;
    while (rp < 0) rp += MaxSamples;
    // The wrap can land EXACTLY on MaxSamples: a tiny negative rp -- which a
    // modulated delay produces in ordinary operation, w_ minus a fractional
    // delay a hair larger -- plus MaxSamples rounds UP to it, because the
    // float just below MaxSamples is further away than MaxSamples itself.
    // (int) rp is then one past the array. UBSan's find; ASan could not see
    // it, because overflowing into the struct's next member is intra-object.
    if (rp >= (float) MaxSamples) rp -= (float) MaxSamples;
    const int i1 = (int) rp;
    const float frac = rp - (float) i1;
    const int i0 = (i1 - 1 + MaxSamples) % MaxSamples;
    const int i2 = (i1 + 1) % MaxSamples;
    const int i3 = (i1 + 2) % MaxSamples;
    return catmullRomTap(buf_[i0], buf_[i1], buf_[i2], buf_[i3], frac);
  }

  inline float read(float delaySamples) const {
    const float d = clampf(delaySamples, 1.0f, (float) (MaxSamples - 2));
    float rp = (float) w_ - d;
    while (rp < 0) rp += MaxSamples;
    if (rp >= (float) MaxSamples) rp -= (float) MaxSamples; // see readCubic
    const int i0 = (int) rp;
    const float frac = rp - i0;
    int i1 = i0 + 1;
    if (i1 >= MaxSamples) i1 -= MaxSamples;
    return buf_[i0] + frac * (buf_[i1] - buf_[i0]);
  }
  inline float tap(float delaySamples, float x) {
    const float y = read(delaySamples);
    write(x);
    return y;
  }

private:
  float buf_[MaxSamples] = {0.0f};
  int w_ = 0;
};

/** Schroeder allpass: the diffusion element of a reverb. */
template <int MaxSamples>
class Allpass {
public:
  void reset() { line_.reset(); }
  void set(float delaySamples, float g) { d_ = delaySamples; g_ = clampf(g, -0.95f, 0.95f); }
  inline float process(float x) {
    const float delayed = line_.read(d_);
    const float v = x + g_ * delayed;
    line_.write(flushDenormal(v));
    return delayed - g_ * v;
  }

private:
  DelayLine<MaxSamples> line_;
  float d_ = 100.0f, g_ = 0.5f;
};

/** Feedback comb with damping: the tail element of a Schroeder/Moorer reverb. */
template <int MaxSamples>
class Comb {
public:
  void reset() { line_.reset(); damp_.reset(); }
  void set(float delaySamples, float feedback, float dampingHz, float sr) {
    d_ = delaySamples;
    fb_ = clampf(feedback, 0.0f, 0.98f);
    damp_.setSampleRate(sr);
    damp_.setCutoff(dampingHz);
  }
  inline float process(float x) {
    const float y = line_.read(d_);
    line_.write(flushDenormal(x + fb_ * damp_.lp(y)));
    return y;
  }

private:
  DelayLine<MaxSamples> line_;
  OnePole damp_;
  float d_ = 1000.0f, fb_ = 0.7f;
};

// ── Utility ──────────────────────────────────────────────────────────────────

/** Equal-power dry/wet mixer. Keeps perceived level constant across the sweep,
 *  which is what stops "more effect" from also meaning "louder". */
class DryWetMixer {
public:
  void setMix(float mix) {
    const float m = clampf(mix, 0.0f, 1.0f);
    wet_ = std::sin(m * kPi * 0.5f);
    dry_ = std::cos(m * kPi * 0.5f);
  }
  inline float process(float dry, float wet) const { return dry_ * dry + wet_ * wet; }

private:
  float dry_ = 1.0f, wet_ = 0.0f;
};

/**
 * A dry/wet mixer that DELAYS THE DRY PATH to match a wet path that is late.
 *
 * DryWetMixer above assumes the two signals are aligned. A wet path that runs
 * through an oversampler, a partitioned convolver or a look-ahead detector is
 * not: it comes out N samples late, and summing it against an undelayed dry
 * signal is adding a signal to a delayed copy of itself. That is a comb
 * filter, a row of notches every 1/N of the sample rate, and it is at its
 * worst at 50% mix, exactly where most users leave the control.
 *
 * There is a second failure that is easier to miss. A plugin whose wet path is
 * late usually DECLARES that latency, and the host then shifts the plugin's
 * whole output earlier to compensate. The dry signal was never late, so the
 * host has just pushed it early: against every other track in the session.
 *
 * Both of these were live in the convolution reverb example. It declared 512
 * samples of latency and the host test measured its output correlating with
 * the input at lag ZERO, r = 0.956: a host would have moved the dry signal
 * 10.7 ms ahead of the rest of the mix.
 *
 * Compensate the TECHNICAL latency only. A reverb's pre-delay is a deliberate
 * offset of the wet signal and must not be cancelled, and a minimum-phase
 * filter's group delay cannot be cancelled by a fixed shift at all.
 */
template <int MaxChannels = 2, int MaxLatency = 4096>
class CompensatedDryWetMixer {
public:
  void setMix(float mix) { mixer_.setMix(mix); }

  /** How late the wet path is, in samples. Call from prepare(): it clears the
   *  delay buffer, which is not something to do while audio is running. */
  void setWetLatency(int samples) {
    latency_ = samples < 0 ? 0 : (samples > MaxLatency ? MaxLatency : samples);
    reset();
  }
  int wetLatency() const { return latency_; }

  void reset() {
    for (int c = 0; c < MaxChannels; ++c) {
      for (int i = 0; i < MaxLatency; ++i) dry_[c][i] = 0.0f;
      write_[c] = 0;
    }
  }

  /** One sample of ONE channel.
   *
   *  The channel is explicit and each carries its own write cursor, so this is
   *  correct however the caller nests its loops. A single shared cursor would
   *  work only for `for each sample { for each channel }` and would interleave
   *  the channels' histories the other way round: a bug that sounds like a
   *  subtle stereo smear and is invisible in mono. */
  inline float process(int channel, float dry, float wet) {
    if (latency_ == 0 || channel < 0 || channel >= MaxChannels)
      return mixer_.process(dry, wet);
    float* line = dry_[channel];
    int& w = write_[channel];
    const float delayed = line[w];
    line[w] = dry;
    if (++w >= latency_) w = 0;
    return mixer_.process(delayed, wet);
  }

private:
  DryWetMixer mixer_;
  float dry_[MaxChannels][MaxLatency] = {};
  int write_[MaxChannels] = {};
  int latency_ = 0;
};

/**
 * Polyphonic voice manager.
 *
 * `VoiceType` must expose `noteOn(int note, float velocity)`, `noteOff()`,
 * `bool isActive()` and `float render()`. That contract has not changed and
 * does not need to: everything below is bookkeeping this class does on the
 * voices' behalf, so a DSP written against the old four methods gains all of
 * it without being touched.
 *
 * Two things here are the difference between a voice manager and a synth a
 * player will tolerate:
 *
 * WHICH voice gets stolen. Taking the oldest is the obvious rule and it is
 * the wrong one: in a held chord the oldest note is usually the bass, so a
 * ninth note on an eight-voice synth silences the root and the chord falls
 * apart. This steals in the order a listener misses least -- a voice already
 * released (its tail is the quietest thing sounding), then a repeat of the
 * same key, then the oldest voice that is neither the lowest nor the highest
 * note down. Bass and melody are the two lines an ear tracks, so they are the
 * last to go.
 *
 * The SUSTAIN PEDAL. Without it a piano-style patch is unplayable: the player
 * holds the pedal, lifts their hands, and everything stops. CC 64 does not
 * reach a voice on its own -- a voice knows nothing about pedals -- so the
 * manager holds the release back instead, which is exactly what the damper on
 * a real instrument does. Sostenuto (CC 66) is the same trick applied only to
 * what was already down when the pedal fell.
 */
template <typename VoiceType, int MaxVoices = 8>
class VoiceManager {
public:
  static constexpr int kNumVoices = MaxVoices;
  /** "Whichever voice is playing this key, on any channel." The default for
   *  every lookup, so a plain MIDI keyboard behaves exactly as it always did. */
  static constexpr int kAnyChannel = -1;

  VoiceType& voice(int i) { return voices_[i]; }
  const VoiceType& voice(int i) const { return voices_[i]; }

  /** Start a note.
   *
   *  The CHANNEL is recorded, and that is not bookkeeping. MPE puts every
   *  finger on its own channel, so two fingers landing on the SAME key are two
   *  separate notes that arrive as (60, ch2) and (60, ch3). Without the
   *  channel this class could not tell them apart: lifting one finger released
   *  both, and a bend on one bent whichever voice was found first. */
  void noteOn(int note, float velocity, int channel = 0) {
    int slot = -1;
    for (int i = 0; i < MaxVoices; ++i) {
      if (!voices_[i].isActive()) { slot = i; break; }
    }
    if (slot < 0) slot = voiceToSteal(note, channel);
    notes_[slot] = note;
    channels_[slot] = channel;
    age_[slot] = ++counter_;
    held_[slot] = true;
    released_[slot] = false;
    pedalHeld_[slot] = false;
    // A key pressed while the sostenuto pedal is already down is NOT pinned:
    // that pedal holds what was down when it fell and nothing after.
    sostenuto_[slot] = false;
    voices_[slot].noteOn(note, velocity);
  }

  /** The voice sounding this key on this channel, or -1.
   *
   *  Per-note expression is addressed by key AND channel, because that pair is
   *  what identifies a finger under MPE. Passing kAnyChannel gives the old
   *  behaviour -- the first voice on that key -- which is right for a keyboard
   *  that puts everything on one channel and wrong for a controller that does
   *  not. */
  int voiceForNote(int note, int channel = kAnyChannel) const {
    for (int i = 0; i < MaxVoices; ++i)
      if (notes_[i] == note && voices_[i].isActive() &&
          (channel == kAnyChannel || channels_[i] == channel))
        return i;
    return -1;
  }

  /** Lift a key. With a channel, only the finger that played it; without one,
   *  every voice on that key.
   *
   *  The KEY coming up and the NOTE ending are different events, and a pedal
   *  is what separates them. So this always records that the key is up, and
   *  only releases the voice when nothing is holding it. */
  void noteOff(int note, int channel = kAnyChannel) {
    for (int i = 0; i < MaxVoices; ++i)
      if (notes_[i] == note && voices_[i].isActive() &&
          (channel == kAnyChannel || channels_[i] == channel)) {
        held_[i] = false;
        if (sustainDown_ || sostenuto_[i]) {
          pedalHeld_[i] = true; // still sounding, on the pedal's account
        } else {
          releaseSlot(i);
        }
      }
  }

  /**
   * The damper pedal, CC 64.
   *
   * Down, a key coming up stops damping its string; up, everything the pedal
   * was holding is released at once. Notes played WHILE it is down are held
   * too -- that is the pedal, not a memory of which keys were down when it
   * fell, which is the other pedal.
   */
  void sustainPedal(bool down) {
    sustainDown_ = down;
    if (down) return;
    for (int i = 0; i < MaxVoices; ++i)
      if (pedalHeld_[i] && !sostenuto_[i] && !held_[i]) releaseSlot(i);
  }

  /**
   * The sostenuto pedal, CC 66.
   *
   * Pins the notes that are down at the moment it falls and lets everything
   * played afterwards behave normally -- a held bass note under a moving
   * line. Rarer than the damper and worth having for the same reason: without
   * it the pedal simply does nothing, which a player reads as a broken plugin
   * rather than an unimplemented feature.
   */
  void sostenutoPedal(bool down) {
    if (down) {
      for (int i = 0; i < MaxVoices; ++i)
        if (held_[i] && voices_[i].isActive()) sostenuto_[i] = true;
      return;
    }
    for (int i = 0; i < MaxVoices; ++i) {
      if (!sostenuto_[i]) continue;
      sostenuto_[i] = false;
      if (!held_[i] && !sustainDown_) releaseSlot(i);
    }
  }

  bool isSustainDown() const { return sustainDown_; }

  /** Release everything, pedals included. This is what CC 123 and a panic
   *  button mean: a voice still sounding afterwards because a pedal was down
   *  is the exact failure the user pressed the button to stop. */
  void allNotesOff() {
    for (int i = 0; i < MaxVoices; ++i) {
      held_[i] = false;
      pedalHeld_[i] = false;
      sostenuto_[i] = false;
      releaseSlot(i);
    }
  }

  /** Sum every active voice for one sample. */
  inline float render() {
    float sum = 0.0f;
    for (int i = 0; i < MaxVoices; ++i)
      if (voices_[i].isActive()) sum += voices_[i].render();
    return sum;
  }

private:
  void releaseSlot(int i) {
    pedalHeld_[i] = false;
    if (!released_[i]) {
      released_[i] = true;
      voices_[i].noteOff();
    }
  }

  /**
   * Which voice a listener will miss least.
   *
   * In order: one already in its release tail; a voice on the same key, since
   * a repeated note replacing itself is inaudible where replacing a different
   * note is not; then the oldest that is neither the lowest nor the highest
   * note sounding. The last rule is the one that matters -- an ear follows
   * the bass and the top line, and taking the oldest voice outright takes the
   * bass of every held chord.
   */
  int voiceToSteal(int note, int channel) const {
    int oldestReleased = -1, sameKey = -1, oldestInner = -1, oldest = 0;
    int lowest = 0, highest = 0;
    for (int i = 1; i < MaxVoices; ++i) {
      if (notes_[i] < notes_[lowest]) lowest = i;
      if (notes_[i] > notes_[highest]) highest = i;
    }
    for (int i = 0; i < MaxVoices; ++i) {
      if (age_[i] < age_[oldest]) oldest = i;
      if (released_[i] && (oldestReleased < 0 || age_[i] < age_[oldestReleased]))
        oldestReleased = i;
      if (notes_[i] == note && (channel == kAnyChannel || channels_[i] == channel) &&
          (sameKey < 0 || age_[i] < age_[sameKey]))
        sameKey = i;
      if (i != lowest && i != highest && (oldestInner < 0 || age_[i] < age_[oldestInner]))
        oldestInner = i;
    }
    if (oldestReleased >= 0) return oldestReleased;
    if (sameKey >= 0) return sameKey;
    if (oldestInner >= 0) return oldestInner;
    return oldest; // two voices, both of them extremes: something has to go
  }

  VoiceType voices_[MaxVoices];
  int channels_[MaxVoices] = {};
  int notes_[MaxVoices] = {0};
  uint64_t age_[MaxVoices] = {0};
  uint64_t counter_ = 0;
  /** The KEY is down. Distinct from the voice sounding, which a pedal can
   *  outlast, and from the voice being released, which a pedal can delay. */
  bool held_[MaxVoices] = {false};
  /** noteOff() has been passed on. Kept here rather than asked of the voice,
   *  because the VoiceType contract is four methods and widening it would
   *  break every DSP already written against it. */
  bool released_[MaxVoices] = {false};
  /** Sounding only because a pedal is down. */
  bool pedalHeld_[MaxVoices] = {false};
  /** Pinned by the sostenuto pedal. */
  bool sostenuto_[MaxVoices] = {false};
  bool sustainDown_ = false;
};

/**
 * Route the pedal controllers to a voice manager. Returns true if the message
 * was one of them and needs no further handling.
 *
 * The pedals are MIDI's, not any one synth's, and the numbers are easy to get
 * subtly wrong: CC 64 is down at 64 and above rather than at 127, and CC 123
 * is All Notes Off while CC 120 is All Sound Off. Written twice -- once in a
 * synth and once in a sampler -- those details drift apart, and the way that
 * shows up is one instrument in a project holding notes the other does not.
 *
 * Channel is ignored on purpose. A pedal is a foot, and a keyboard that sends
 * it on its global channel expects it to apply to everything it plays.
 */
template <typename VoiceManagerType>
inline bool applyPedals(VoiceManagerType& voices, const MidiMessage& m) {
  if (!m.isController()) return false;
  const int cc = m.getControllerNumber();
  const bool down = m.getControllerValue() >= 64; // the MIDI spec's threshold
  switch (cc) {
    case 64: voices.sustainPedal(down); return true;
    case 66: voices.sostenutoPedal(down); return true;
    case 120: // All Sound Off
    case 123: // All Notes Off
      voices.allNotesOff();
      return true;
    default: return false;
  }
}

} // namespace sonore

// Composite effects, musical time and the frequency domain build on the
// primitives above, and are included here so `#include <sonore/dsp.h>` stays
// the single entry point a generated plugin needs.
// Rate conversion first: effects.h uses the oversampler.
#include "resample.h"
#include "effects.h"
#include "transport.h"
#include "fft.h"
// Shaping and FIR come last: they use DcBlocker and flushDenormal from above.
#include "shaping.h"
#include "fir.h"
// The 2026-08-29 round: metering, the rest of dynamics, lo-fi, an FDN, pitch,
// a processable STFT, the synth parts, tone controls. Each uses primitives
// from above and from the headers just chained (dynamics.h needs the
// crossover from effects.h; stft.h and synth.h need Fft from fft.h; lofi.h
// needs the oversampler), so the order here is the dependency order.
#include "metering.h"
#include "dynamics.h"
#include "lofi.h"
#include "fdn.h"
#include "pitch.h"
#include "stft.h"
#include "synth.h"
#include "tone.h"
// The second round, same day: the machines a plugin is NAMED after. delay.h
// needs transport.h, tape.h needs lofi.h's Oversampled<> and random.h,
// crossover.h needs effects.h's LinkwitzRiley, tube.h needs DcBlocker,
// granular.h needs Random and the Catmull-Rom tap, phase_vocoder.h needs
// Fft, spring.h needs DelayLine and OnePole, vocoder.h needs SVF and the
// envelope follower.
#include "delay.h"
#include "tape.h"
#include "tube.h"
#include "crossover.h"
#include "va_filters.h"
#include "granular.h"
#include "phase_vocoder.h"
#include "spring.h"
#include "vocoder.h"
// The hardware compressors: opto, FET, VCA. Need Biquad and the dB helpers.
#include "compressors.h"
// The things a plugin is asked for by name that were still being rebuilt:
// the EQ machine (matched biquads + the FIR designer), the diode clipper
// (Oversampled<>), the Hilbert pair and shifter, the modulation family
// (Chorus, DelayLine, Oscillator).
#include "eq.h"
#include "clipper.h"
#include "hilbert.h"
#include "modulation.h"
// ...and the compositions: multiband dynamics (splitter + compressors), a
// room's early reflections, pitch correction (detector + vocoder), the
// distortion object (Oversampled<> + DcBlocker + OnePole).
#include "multiband.h"
#include "reflections.h"
#include "pitch_correct.h"
#include "distortion.h"
// ...and the circuit tier: the nodal DK engine, the Fuzz Face that runs on
// it, and the Pultec-style program EQ.
#include "circuit.h"
#include "fuzz.h"
#include "passive_eq.h"
// ...and the 2026-08-30 second round: the band that compresses itself, the
// true-peak ceiling, the Dattorro plate, and the op-amp tier of circuits.
#include "dynamic_eq.h"
#include "limiting.h"
#include "plate.h"
#include "screamer.h"
