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
// Everything is `sonore::` (filters, dynamics, modulation, voices). This is our
// own implementation of standard, published DSP (RBJ's EQ cookbook, Simper's
// TPT structures, polyBLEP, Schroeder/Moorer reverb topology): textbook maths,
// written here from the maths rather than adapted from any framework's source.
#pragma once
#include <cmath>
#include <cstdint>
#include <type_traits>
#include <utility>
#include "special.h" // fastmath, which the oscillator uses per sample
#include "audio.h"
#include "interpolation.h"

namespace sonore {

// kPi / kTwoPi live in audio.h (see the note there).

/** x held to [lo, hi], and a NaN to lo.
 *
 *  A NaN fails both comparisons of the obvious clamp and came through it
 *  untouched, into every setter that clamps its argument. A NaN frequency
 *  became a NaN phase increment, and a phase that has taken a NaN never
 *  wraps back: the oscillator, the LFOs, the FM and hard-sync oscillators
 *  put out NaN for good, after the next good setting too, and the chorus,
 *  the delay's modulation and the plate read their shortest delay from then
 *  on. Every value in a clamp's range is one its caller accepts, so the
 *  bottom of it is where a NaN setting lands. Any other x clamps as before,
 *  to the bit. */
inline float clampf(float x, float lo, float hi) { return x >= lo ? (x > hi ? hi : x) : lo; }
/** Decibels to a linear amplitude gain: 0 dB is 1, -6.02 dB is 0.5. No
 *  floor: minus infinity gives 0, and a large negative number a tiny gain
 *  that is not silence -- dbToGainFloor() maps a floor to exactly 0. */
inline float dbToGain(float db) { return std::pow(10.0f, db * 0.05f); }
/** A linear amplitude gain to decibels. Anything at or below 1e-9 reads
 *  -180 dB: zero, a NaN, and a NEGATIVE gain too, so pass a magnitude.
 *  gainToDbFloor() is the one a meter wants. */
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
/** a + (b - a) t: `a` at t = 0, `b` at t = 1. `t` is not clamped, so a
 *  value outside 0..1 extrapolates. */
inline float lerp(float a, float b, float t) { return a + (b - a) * t; }

/** Flush denormals. Subnormal floats cost hundreds of cycles on x86 and are the
 *  classic cause of a reverb tail that makes a DAW stutter as it fades out.
 *
 *  And NaN, written so the comparison that fails for NaN is the one that
 *  keeps a value: one NaN or Inf sample used to latch into every feedback
 *  state that passes through here -- an envelope follower stuck at NaN read
 *  as -180 dB, so an expander sat at its full range, an upward compressor at
 *  +20 dB and a compressor switched itself off, with FINITE output that no
 *  NaN guard downstream would ever catch. Zero is where state recovers from.
 *
 *  And Inf, for the same reason. It used to pass, and a Biquad's state kept
 *  it: a lowpass or bandpass re-read it as Inf - Inf, so one infinite sample
 *  left every DeEsser in split mode, every MultibandDynamics, every
 *  Linkwitz-Riley crossover and the spring reverb putting out NaN until
 *  reset() -- and a wideband DeEsser finite, but no longer de-essing. */
inline float flushDenormal(float x) {
  const float a = std::fabs(x);
  return (a >= 1e-18f && a <= 3.402823466e38f) ? x : 0.0f;
}

// ── Waveshaping ──────────────────────────────────────────────────────────────

/** x / (1 + |x|): unity slope at zero, approaching +-1 without reaching it.
 *  The cheapest smooth curve here, and a softer one than tanh: 1 in gives
 *  0.5 where tanhClip() gives 0.76. Symmetric, so odd harmonics only. */
inline float softClip(float x) { return x / (1.0f + std::fabs(x)); }
/** std::tanh: unity slope at zero, saturating to +-1. The standard curve;
 *  per sample in many voices, fastmath::tanhApprox in special.h is the
 *  cheaper one. */
inline float tanhClip(float x) { return std::tanh(x); }
/** x held to [-1, 1]: a hard corner at full scale, whose harmonics reach
 *  much higher, and alias more, than the smooth curves'. A NaN comes out
 *  as -1, as clampf() sends it to the bottom of the range. */
inline float hardClip(float x) { return clampf(x, -1.0f, 1.0f); }

/** Cubic soft saturation with drive (1 = clean-ish, higher = grittier):
 *  `1.5y - 0.5y^3` of y = x * drive held to [-1, 1], the textbook cubic
 *  soft clipper scaled to unity peak. Slope 1.5 at zero, 0 at |y| = 1,
 *  where it reaches +-1 and stays: monotonic, so more drive is never
 *  quieter. The hold was at +-1.5, past the peak: from |y| = 1 to 1.5 the
 *  curve fell back to 0.5625 and held there, so a harder drive came out
 *  quieter. A NaN comes out as -1, as clampf() sends it to the bottom. */
inline float cubic(float x, float drive) {
  const float y = clampf(x * drive, -1.0f, 1.0f);
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
  /** The time constant, `ms` milliseconds (floored at 0.01) at `sampleRate`
   *  Hz (48 kHz if not above 1: a negative rate made the pole above 1, and
   *  the value ran away from its target): a step is 63% of the way there
   *  after `ms`, 99% after 4.6 times it. Before setup() there is no lag at
   *  all: next() returns its target. */
  void setup(float sampleRate, float ms = 8.0f) {
    const float sr = sampleRate > 1.0f ? sampleRate : 48000.0f;
    a_ = std::exp(-1.0f / (0.001f * (ms > 0.01f ? ms : 0.01f) * sr));
  }
  /** Jump to `v` with no glide. The value starts at 0, so a control that is
   *  not snapped in prepare() or reset() sweeps up from silence. */
  void snap(float v) { z_ = v; }
  /** One sample's step toward `target`; returns the smoothed value. Call
   *  once per sample, with the control's current value. */
  inline float next(float target) {
    // The STORED value is flushed, not only the one returned: settling on
    // zero, the raw state decayed through the subnormal range for thousands
    // of samples, each one slow arithmetic on a thread without flush-to-zero.
    z_ = flushDenormal(target + a_ * (z_ - target));
    return z_;
  }
  /** The value next() last returned, or snap() set, without stepping. */
  float value() const { return z_; }

private:
  float a_ = 0.0f, z_ = 0.0f;
};

/**
 * A control that GLIDES to a new value once audio is running, and lands at
 * once before it is (or after snap()).
 *
 * Smooth needs its caller to remember to snap it at the start; this one
 * knows. A plugin sets its knobs, then processes: those first values are a
 * setting, not a movement, and land. After that every change is a movement,
 * and a gain that moves in one sample is a click -- a compressor's makeup,
 * input or threshold automated stepped the output up to 8.6 times its
 * largest natural step. reset() on the owner should snap() it.
 */
struct Glide {
  /** The glide's length against the rate it runs at; 10 ms by default. */
  void setSampleRate(float sr, float ms = 10.0f) {
    const float n = 0.001f * (ms > 0.1f ? ms : 0.1f) * (sr > 1.0f ? sr : 48000.0f);
    k = 1.0f - std::exp(-1.0f / n);
  }
  /** A new target. Before the first next(), or after snap(), the value
   *  lands on it at once; after that next() glides to it. */
  void set(float v) {
    target = v;
    if (!running) now = v;
  }
  /** Land on the target now, and take the next set() as a setting again,
   *  not a movement: what the owner's reset() calls. */
  void snap() {
    now = target;
    running = false;
  }
  /** One sample's step toward the target; returns the value. Within a
   *  millionth of the target (relative) it lands exactly, so a finished
   *  glide costs one comparison. Call once per sample. */
  inline float next() {
    running = true;
    if (now != target) {
      now += (target - now) * k;
      if (std::fabs(target - now) <= 1e-6f * (1.0f + std::fabs(target))) now = target;
    }
    return now;
  }
  ///@{
  /** The value next() last returned, where it is going, and the fraction of
   *  the remaining distance covered per sample (set by setSampleRate()).
   *  Public so a Glide can be brace-initialised as `{now, target}`; read
   *  them, but change them through set() and snap(). */
  float now = 0.0f, target = 0.0f, k = 0.00208116f; // 10 ms at 48 kHz
  ///@}
  /** next() has run since construction or the last snap(), so set() glides
   *  rather than lands. */
  bool running = false;
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

  /** The ramp's length: `milliseconds` (floored at 0.01) at `sampleRate` Hz
   *  (48 kHz if not positive), at least one sample. A ramp already running
   *  keeps the length it started with. Before setup(), setTarget() jumps. */
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

  /** Ramp from the current value to `target`, straight in dB, arriving in
   *  exactly the setup() length. The same target again while a ramp to it
   *  runs changes nothing. From silence (at or below kFloor) it jumps; to a
   *  target at or below kFloor it ramps to the floor and lands on the target
   *  on the last sample. For gains: the ramp works on magnitudes and keeps
   *  the current value's sign until it lands. One pow() per call. */
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

  /** One sample of the ramp; returns the new value. Once it has landed it
   *  returns the target, exactly, until the next setTarget(). */
  inline float next() {
    if (remaining_ <= 0) return current_;
    current_ *= factor_;
    if (--remaining_ == 0) current_ = snapTo_; // land exactly, not nearly
    return current_;
  }

  /** The current value, without stepping. */
  float value() const { return current_; }
  /** Where the ramp is going, or has arrived. */
  float target() const { return target_; }
  /** True while steps remain; false once landed, after snap(), or after a
   *  setTarget() that jumped. */
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
  /** Sample rate in Hz (48 kHz if not above 1). Call setCutoff() after it:
   *  the coefficient is worked out there, not here. */
  void setSampleRate(float sr) { sr_ = sr > 1.0f ? sr : 48000.0f; }
  /** The corner in Hz, held to 1 Hz .. 0.49 of the sample rate: -3 dB there,
   *  exactly. One tan() per call; the state is kept, so it may move while
   *  audio runs. */
  inline void setCutoff(float hz) {
    const float g = std::tan(kPi * clampf(hz, 1.0f, sr_ * 0.49f) / sr_);
    G_ = g / (1.0f + g);
  }
  /** One sample through the lowpass; returns it. lp() and hp() share one
   *  state, so call one of them per sample, not both. */
  inline float lp(float x) {
    const float v = (x - z_) * G_;
    const float y = v + z_;
    z_ = flushDenormal(y + v);
    return y;
  }
  /** One sample through the matching highpass, x - lp(x). It advances the
   *  same state as lp(): one or the other per sample. */
  inline float hp(float x) { return x - lp(x); }
  /** Clear the state, as after silence. */
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
  /** G = g / (1 + g), the gain into the state per sample, between 0 and 1:
   *  the number in the transfer function above, for a caller that models a
   *  loop around this filter. */
  float coefficient() const { return G_; }

private:
  float sr_ = 48000.0f, G_ = 0.1f, z_ = 0.0f;
};

/** Transposed-Direct-Form-II biquad with RBJ cookbook coefficients. */
class Biquad {
public:
  /** Sample rate in Hz (48 kHz until set, and if not above 1: a design
   *  divides by it, and a rate of 0 made every coefficient NaN). Set it
   *  before a design call, which reads it; changing it does not redesign
   *  the section. */
  void setSampleRate(float sr) { sr_ = sr > 1.0f ? sr : 48000.0f; }
  /** Clear the two state values, as after silence; the coefficients stay. */
  void reset() { z1_ = z2_ = 0.0f; }
  /** One sample through the section; returns it. The state is passed
   *  through flushDenormal(), so a NaN or Inf sample does not stay in it.
   *  Until a design call the section passes its input unchanged. */
  inline float process(float x) {
    const float y = b0_ * x + z1_;
    z1_ = flushDenormal(b1_ * x - a1_ * y + z2_);
    z2_ = flushDenormal(b2_ * x - a2_ * y);
    return y;
  }
  /** 12 dB/oct lowpass at `freq` Hz, held to 10 Hz .. 0.49 of the sample
   *  rate, with resonance `q`, floored at 0.05 (0.7071 is Butterworth, the
   *  flattest with no peak). Every design call recomputes the coefficients
   *  and keeps the state; for a corner that moves every few samples, SVF or
   *  SvfSection is the structure that stays clean. */
  void lowpass(float freq, float q) { rbj(freq, q, 0); }
  /** 12 dB/oct highpass; `freq` and `q` as lowpass(). */
  void highpass(float freq, float q) { rbj(freq, q, 1); }
  /** Bandpass with 0 dB at `freq` (the cookbook's constant-peak form); a
   *  higher `q` is a narrower band. Ranges as lowpass(). */
  void bandpass(float freq, float q) { rbj(freq, q, 2); }
  /** Notch: no output at `freq`, a higher `q` a narrower notch. Ranges as
   *  lowpass(). */
  void notch(float freq, float q) { rbj(freq, q, 3); }
  /** Second-order allpass: unity gain at every frequency, the phase turning
   *  through 180 degrees at `freq`, more abruptly for a higher `q`. */
  void allpass(float freq, float q) { rbj(freq, q, 4); }
  /** Peaking EQ: `gainDb` of boost (or cut, negative) centred on `freq`,
   *  with bandwidth set by `q`; 0 dB is flat. Ranges as lowpass(). */
  void peak(float freq, float q, float gainDb) { rbjPeak(freq, q, gainDb); }
  /** Low shelf: `gainDb` below `freq`, 0 dB above, half the gain in dB at
   *  `freq` itself. `q` is the cookbook's shelf Q: 0.7071 is the steepest
   *  slope with no overshoot; higher overshoots on both sides of it. */
  void lowShelf(float freq, float q, float gainDb) { rbjShelf(freq, q, gainDb, false); }
  /** High shelf: `gainDb` above `freq`, 0 dB below; `q` as lowShelf(). */
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

  /** Another section's coefficients, leaving this one's state alone: two
   *  channels through the same filter need one design, not two. */
  void copyCoefficientsFrom(const Biquad& other) {
    b0_ = other.b0_; b1_ = other.b1_; b2_ = other.b2_; a1_ = other.a1_; a2_ = other.a2_;
  }

  /** The five numbers, normalised so a0 is 1 -- what setCoefficients takes. */
  void getCoefficients(float& b0, float& b1, float& b2, float& a1, float& a2) const {
    b0 = b0_; b1 = b1_; b2 = b2_; a1 = a1_; a2 = a2_;
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

/**
 * One second-order section in DOUBLE precision -- coefficients, state and
 * response all.
 *
 * A float biquad is fine for a corner a few hundred hertz up and falls apart
 * as the corner nears DC. At 2 Hz in a 48 kHz session the poles sit 2.6e-4
 * inside the unit circle, which float coefficients barely resolve, and the
 * TDF-II state loses what is left: an order-6 Butterworth lowpass there
 * measured -19 dB at its own corner instead of -3, and a Chebyshev order 8 at
 * 30 Hz measured -8 dB at 192 kHz where -1 was designed. A rumble filter, a
 * DC blocker and every crossover at a high session rate live in that corner.
 * Doubles cost a few cycles per section and buy all of it back.
 */
class IirSection {
public:
  /** Sample rate in Hz (48 kHz until set, and if not above 1, as Biquad
   *  takes it). Set it before a design call, which reads it; changing it
   *  does not redesign the section. */
  void setSampleRate(float sr) { sr_ = sr > 1.0f ? sr : 48000.0f; }
  /** Clear the state, as after silence; the coefficients stay. */
  void reset() { z1_ = z2_ = 0.0; }
  /** Coefficients from elsewhere, normalised so a0 is 1, as
   *  Biquad::setCoefficients() takes them but in double: what the EQ and
   *  filter designs hand each section. The state is kept. */
  void setCoefficients(double b0, double b1, double b2, double a1, double a2) {
    b0_ = b0;
    b1_ = b1;
    b2_ = b2;
    a1_ = a1;
    a2_ = a2;
  }
  /** The cookbook lowpass, highpass and allpass, designed in double: the
   *  crossovers need them, and Biquad's float cosine of a 40 Hz corner at
   *  192 kHz has too few digits left to put the corner where it was asked.
   *  The corner is held to 10 Hz .. 0.49 fs, as Biquad holds it. */
  void lowpass(double hz, double q) { design(hz, q, 0); }
  /** The cookbook highpass in double; `hz` and `q` as lowpass(). */
  void highpass(double hz, double q) { design(hz, q, 1); }
  /** The cookbook second-order allpass in double: flat, 180 degrees of phase
   *  at `hz`; `hz` and `q` as lowpass(). */
  void allpass(double hz, double q) { design(hz, q, 2); }
  /** The cookbook shelves, as Biquad has them, in double: a 50 Hz shelf at
   *  384 kHz in float sat 1.1 dB off at its own corner. */
  void lowShelf(double hz, double q, double gainDb) { shelf(hz, q, gainDb, false); }
  /** The high shelf, in double: `gainDb` above `hz`, as Biquad::highShelf(). */
  void highShelf(double hz, double q, double gainDb) { shelf(hz, q, gainDb, true); }
  /** One sample through the section, computed in double; returns it. Tiny,
   *  NaN and Inf state is zeroed, as flushDenormal() does for float. Until
   *  a design call the section passes its input unchanged. */
  inline float process(float in) {
    const double x = (double) in;
    const double y = b0_ * x + z1_;
    z1_ = b1_ * x - a1_ * y + z2_;
    z2_ = b2_ * x - a2_ * y;
    // Tiny state to zero, NaN and Inf included (the comparisons that fail
    // for NaN are the ones that keep a value), as flushDenormal does for
    // float: an Inf kept here came back as Inf - Inf.
    const double m1 = std::fabs(z1_), m2 = std::fabs(z2_);
    z1_ = (m1 >= 1e-30 && m1 <= 1e300) ? z1_ : 0.0;
    z2_ = (m2 >= 1e-30 && m2 <= 1e300) ? z2_ : 0.0;
    return (float) y;
  }
  /** The section's gain at a frequency, in dB, from its own coefficients. */
  float magnitudeDb(float freq) const {
    const double w = kTwoPi * (double) freq / (double) sr_;
    const double cw = std::cos(w), sw = std::sin(w);
    const double c2 = std::cos(2.0 * w), s2 = std::sin(2.0 * w);
    const double nr = b0_ + b1_ * cw + b2_ * c2;
    const double ni = -(b1_ * sw + b2_ * s2);
    const double dr = 1.0 + a1_ * cw + a2_ * c2;
    const double di = -(a1_ * sw + a2_ * s2);
    const double num = std::sqrt(nr * nr + ni * ni);
    const double den = std::sqrt(dr * dr + di * di);
    if (den < 1e-300) return -300.0f;
    const double mag = num / den;
    return (float) (20.0 * std::log10(mag > 1e-15 ? mag : 1e-15));
  }

private:
  void design(double hz, double q, int type) {
    const double fs = (double) sr_;
    const double f = hz < 10.0 ? 10.0 : (hz > 0.49 * fs ? 0.49 * fs : hz);
    const double w0 = kTwoPi * f / fs;
    const double cw = std::cos(w0), sw = std::sin(w0), sh = std::sin(0.5 * w0);
    const double alpha = sw / (2.0 * (q < 0.05 ? 0.05 : q));
    const double a0 = 1.0 + alpha;
    double b0, b1, b2;
    if (type == 0) {
      b0 = sh * sh; // (1 - cos) / 2, with its digits
      b1 = 2.0 * sh * sh;
      b2 = sh * sh;
    } else if (type == 1) {
      b0 = 0.5 * (1.0 + cw);
      b1 = -(1.0 + cw);
      b2 = 0.5 * (1.0 + cw);
    } else {
      b0 = 1.0 - alpha;
      b1 = -2.0 * cw;
      b2 = 1.0 + alpha;
    }
    setCoefficients(b0 / a0, b1 / a0, b2 / a0, -2.0 * cw / a0, (1.0 - alpha) / a0);
  }
  void shelf(double hz, double q, double gainDb, bool high) {
    const double fs = (double) sr_;
    const double f = hz < 10.0 ? 10.0 : (hz > 0.49 * fs ? 0.49 * fs : hz);
    const double A = std::pow(10.0, gainDb / 40.0);
    const double w0 = kTwoPi * f / fs;
    const double cw = std::cos(w0), sw = std::sin(w0);
    const double alpha = sw / (2.0 * (q < 0.05 ? 0.05 : q));
    const double tsa = 2.0 * std::sqrt(A) * alpha, Ap1 = A + 1.0, Am1 = A - 1.0;
    double a0, a1, a2, b0, b1, b2;
    if (!high) {
      a0 = Ap1 + Am1 * cw + tsa;
      a1 = -2.0 * (Am1 + Ap1 * cw);
      a2 = Ap1 + Am1 * cw - tsa;
      b0 = A * (Ap1 - Am1 * cw + tsa);
      b1 = 2.0 * A * (Am1 - Ap1 * cw);
      b2 = A * (Ap1 - Am1 * cw - tsa);
    } else {
      a0 = Ap1 - Am1 * cw + tsa;
      a1 = 2.0 * (Am1 - Ap1 * cw);
      a2 = Ap1 - Am1 * cw - tsa;
      b0 = A * (Ap1 + Am1 * cw + tsa);
      b1 = -2.0 * A * (Am1 + Ap1 * cw);
      b2 = A * (Ap1 + Am1 * cw - tsa);
    }
    setCoefficients(b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0);
  }

  double b0_ = 1.0, b1_ = 0.0, b2_ = 0.0, a1_ = 0.0, a2_ = 0.0;
  double z1_ = 0.0, z2_ = 0.0;
  float sr_ = 48000.0f;
};

/**
 * IirSection's lowpass, highpass and allpass as a state-variable filter --
 * the same three responses, in a structure that survives its corner MOVING.
 *
 * Both are the bilinear transform of the same analogue prototype, prewarped
 * at the corner, so for a fixed corner they are one filter and differ only in
 * rounding. They part company when the corner is automated. A direct-form
 * section's state is made of the previous coefficients' outputs, and with
 * new coefficients it no longer describes any signal: a Linkwitz-Riley
 * crossover swept from 500 Hz to Nyquist and back every sample diverged to
 * infinity, and one stepped every few samples spiked 20 dB above its input.
 * A TPT state-variable filter (Zavalishin; Simper's linear trapezoidal SVF)
 * keeps two integrator charges, which are valid under any coefficients.
 *
 * In double, as IirSection, for the same reason: a 40 Hz corner at 192 kHz.
 * The corner is held to 10 Hz .. 0.49 fs, as IirSection holds it.
 */
class SvfSection {
public:
  /** Sample rate in Hz (48 kHz if not above 1). Set it before a design call,
   *  which reads it; changing it does not redesign the section. */
  void setSampleRate(float sr) { sr_ = sr > 1.0f ? sr : 48000.0f; }
  /** Clear the two integrator states, as after silence; the response stays. */
  void reset() { ic1_ = ic2_ = 0.0; }
  /** 12 dB/oct lowpass at `hz`, resonance `q` (floored at 0.05; 0.7071 is
   *  Butterworth). One tan() per call, and safe to call every sample: the
   *  state stays valid under the new coefficients. Until a design call the
   *  section puts out silence. */
  void lowpass(double hz, double q) { design(hz, q, 1.0, 0.0, 0.0); }
  /** 12 dB/oct highpass; `hz` and `q` as lowpass(). */
  void highpass(double hz, double q) { design(hz, q, -1.0, -1.0, 1.0); }
  /** Second-order allpass: flat, 180 degrees of phase at `hz`; `hz` and `q`
   *  as lowpass(). */
  void allpass(double hz, double q) { design(hz, q, 0.0, -2.0, 1.0); }
  /** One sample through the response last designed, in double; returns it. */
  inline float process(float in) {
    const double x = (double) in;
    const double v3 = x - ic2_;
    const double v1 = a1_ * ic1_ + a2_ * v3; // band
    const double v2 = ic2_ + a2_ * ic1_ + a3_ * v3; // low
    ic1_ = 2.0 * v1 - ic1_;
    ic2_ = 2.0 * v2 - ic2_;
    // Tiny state to zero, NaN and Inf included, as IirSection does.
    const double m1 = std::fabs(ic1_), m2 = std::fabs(ic2_);
    ic1_ = (m1 >= 1e-30 && m1 <= 1e300) ? ic1_ : 0.0;
    ic2_ = (m2 >= 1e-30 && m2 <= 1e300) ? ic2_ : 0.0;
    return (float) (mixLow_ * v2 + mixBand_ * v1 + mixIn_ * x);
  }

private:
  /** The output is a mix of the low and band states and the input: low is
   *  v2, high is x - k v1 - v2, allpass x - 2k v1. The band weights given are
   *  in units of k. */
  void design(double hz, double q, double low, double bandPerK, double input) {
    const double fs = (double) sr_;
    const double f = hz < 10.0 ? 10.0 : (hz > 0.49 * fs ? 0.49 * fs : hz);
    const double g = std::tan(kPi * f / fs);
    const double k = 1.0 / (q < 0.05 ? 0.05 : q);
    a1_ = 1.0 / (1.0 + g * (g + k));
    a2_ = g * a1_;
    a3_ = g * a2_;
    mixLow_ = low;
    mixBand_ = bandPerK * k;
    mixIn_ = input;
  }

  double a1_ = 1.0, a2_ = 0.0, a3_ = 0.0;
  double mixLow_ = 1.0, mixBand_ = 0.0, mixIn_ = 0.0;
  double ic1_ = 0.0, ic2_ = 0.0;
  float sr_ = 48000.0f;
};

/** State-variable filter (TPT topology): stable under heavy modulation and
 *  gives LP/HP/BP/notch simultaneously. The default filter for anything swept. */
class SVF {
public:
  /** Sample rate in Hz (48 kHz until set, and if not above 1, as Biquad
   *  takes it). Call set() after it, which reads it. */
  void setSampleRate(float sr) { sr_ = sr > 1.0f ? sr : 48000.0f; }
  /** Corner (or centre) in Hz, held to 10 Hz .. 0.49 of the sample rate,
   *  and resonance `q`, floored at 0.05 (0.7071 is Butterworth). One tan()
   *  per call, and safe to call every sample: that is what the structure is
   *  for. Call it before the first process(): until then the lowpass and
   *  bandpass are silent and the highpass and notch pass the input. */
  inline void set(float freq, float q) {
    g_ = std::tan(kPi * clampf(freq, 10.0f, sr_ * 0.49f) / sr_);
    k_ = 1.0f / (q < 0.05f ? 0.05f : q);
    a1_ = 1.0f / (1.0f + g_ * (g_ + k_));
    a2_ = g_ * a1_;
    a3_ = g_ * a2_;
  }
  /** One sample in. Returns nothing: the four responses land in lp, hp, bp
   *  and notch, so read whichever the caller wants after the call. */
  inline void process(float x) {
    const float v3 = x - ic2_;
    const float v1 = a1_ * ic1_ + a2_ * v3;
    const float v2 = ic2_ + a2_ * ic1_ + a3_ * v3;
    ic1_ = flushDenormal(2 * v1 - ic1_);
    ic2_ = flushDenormal(2 * v2 - ic2_);
    lp = v2; bp = v1; hp = x - k_ * v1 - v2; notch = x - k_ * v1;
  }
  ///@{
  /** The last process() call's outputs: 12 dB/oct lowpass and highpass, the
   *  bandpass, and the notch (lowpass plus highpass). The bandpass's gain at
   *  the centre is `q`, not 1; times 1/q it peaks at 0 dB. */
  float lp = 0, hp = 0, bp = 0, notch = 0;
  ///@}
  /** Clear the integrators, as after silence. The outputs keep their last
   *  values until the next process(). */
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
  /** Sample rate in Hz (48 kHz if not above 1). Call set() after it, which
   *  reads it; until set() has run the filter puts out silence. */
  void setSampleRate(float sr) { sr_ = sr > 1.0f ? sr : 48000.0f; }
  /** Clear the four stage states and the loop's last output, as after
   *  silence; the settings stay. */
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
  /** One sample through the ladder; returns the 24 dB/oct lowpass. As in
   *  the hardware, resonance thins the passband: the linear ladder's DC gain
   *  is 1 / (1 + k), about -14 dB at full resonance. No allocation; one
   *  tanhApprox per sample with saturation on, four more with drive. */
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
   *  which is also why it takes so long to settle; see tailSamples(). */
  static constexpr float kCornerHz = 10.0f;

  /** Sample rate in Hz (48 kHz if not above 1: at 0 the pole was minus
   *  infinity): puts the corner at kCornerHz. Until it is called the pole is
   *  0.999, about 7.6 Hz at 48 kHz. */
  void setSampleRate(float sr) { r_ = 1.0f - (kTwoPi * kCornerHz / (sr > 1.0f ? sr : 48000.0f)); }

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

  /** One sample through the highpass; returns it. */
  inline float process(float x) {
    const float y = x - x1_ + r_ * y1_;
    x1_ = x;
    y1_ = flushDenormal(y);
    return y;
  }
  /** Clear the state, as after silence. */
  void reset() { x1_ = y1_ = 0.0f; }

private:
  float r_ = 0.999f, x1_ = 0.0f, y1_ = 0.0f;
};

// ── Oscillators ──────────────────────────────────────────────────────────────

/** polyBLEP band-limited oscillator. The naive shapes alias badly at high notes
 *  (the aliasing gate measures exactly that), so saw/square subtract a
 *  polynomial approximation of a band-limited step at each discontinuity. */
class Oscillator {
public:
  /** Sample rate in Hz (48 kHz until set, and if not above 1: at 0 the
   *  phase increment was 0 / 0 and every shape NaN for good). Call setFreq()
   *  after it: the phase increment is worked out there. */
  void setSampleRate(float sr) { sr_ = sr > 1.0f ? sr : 48000.0f; }
  /** Frequency in Hz, held to 0 .. half the sample rate; 0 (the default)
   *  holds the phase still. Takes effect on the next sample with no phase
   *  jump, so it may be called every sample for vibrato or FM. */
  inline void setFreq(float hz) { inc_ = clampf(hz, 0.0f, sr_ * 0.5f) / sr_; }
  /** Restart at `phase`, in turns, 0 up to (not including) 1: 0 is the
   *  start of a cycle, the sine rising through zero and the saw at its
   *  jump. Also clears the triangle's integrator, for a retriggered voice. */
  void reset(float phase = 0.0f) { phase_ = phase; tri_ = 0.0f; }

  /** One sample of a sine, -1..1, then the phase advances. EVERY shape call
   *  advances the phase, so call one per sample: two calls run the
   *  oscillator at twice its frequency (triangle() is already one). */
  inline float sine() {
    // The phase is already in turns, so this is the library call AND the
    // multiply by 2*pi that would have preceded it. A polyphonic synth runs
    // one of these per voice per sample.
    const float y = fastmath::sinTurns(phase_);
    adv();
    return y;
  }
  /** One sample of a rising sawtooth, -1 to 1, band-limited by polyBLEP;
   *  advances the phase. */
  inline float saw() {
    float y = 2.0f * phase_ - 1.0f;
    y -= blep(phase_);
    adv();
    return y;
  }
  /** One sample of a pulse: +1 for the first `pulseWidth` of each cycle and
   *  -1 for the rest, `pulseWidth` held to 0.02 .. 0.98 (0.5 is a square),
   *  band-limited by polyBLEP at both edges; advances the phase. */
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
  /** The phase the next shape call will use, in turns, 0..1. */
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
  /** Sample rate in Hz (48 kHz until set, and if not above 1: a negative
   *  rate made both coefficients above 1, and the envelope grew without
   *  bound). Call setTimes() after it: the coefficients are worked out
   *  there, from this rate. */
  void setSampleRate(float sr) { sr_ = sr > 1.0f ? sr : 48000.0f; }
  /** Attack and release time constants in ms, each floored at 0.01. Until
   *  this is called both are instant and process() returns |x|. Two exp()
   *  per call. */
  void setTimes(float attackMs, float releaseMs) {
    aA_ = std::exp(-1.0f / (0.001f * (attackMs > 0.01f ? attackMs : 0.01f) * sr_));
    aR_ = std::exp(-1.0f / (0.001f * (releaseMs > 0.01f ? releaseMs : 0.01f) * sr_));
  }
  /** One sample: returns the envelope of |x|, a linear amplitude, rising at
   *  the attack rate while |x| is above the envelope and falling at the
   *  release rate otherwise. */
  inline float process(float x) {
    const float r = std::fabs(x);
    const float a = r > env_ ? aA_ : aR_;
    env_ = flushDenormal(r + a * (env_ - r));
    return env_;
  }
  /** The envelope after the last process(), without stepping. */
  float value() const { return env_; }
  /** Back to 0, as after silence; the times stay. */
  void reset() { env_ = 0.0f; }

private:
  float sr_ = 48000.0f, aA_ = 0, aR_ = 0, env_ = 0;
};

/** ADSR in seconds, exponential-ish segments. Drives one voice's amplitude. */
class ADSR {
public:
  /** The envelope's settings, as the reference's ADSR::Parameters has them:
   *  attack, decay and release in seconds, sustain a level 0..1. The attack
   *  is a straight ramp to 1 from wherever the level is; decay and release
   *  are exponential, and their time is the time to fall 60 dB, so the
   *  number on the control is the number a meter reads. */
  struct Parameters {
    ///@{
    /** 10 ms attack, 100 ms decay, sustain 0.8, 200 ms release by default.
     *  A time at or under 0.1 ms takes a sample or two. Sustain is not
     *  clamped: a level above 1 makes the decay rise to it. */
    float attack = 0.01f, decay = 0.1f, sustain = 0.8f, release = 0.2f;
    ///@}
  };
  /** Idle at level 0, at 48 kHz with the default Parameters. */
  ADSR() { updateCoefficients(); }
  /** Sample rate in Hz (48 kHz if not above 1). The segments are worked out
   *  again, so this may come before or after setParameters(). */
  void setSampleRate(float sr) {
    sr_ = sr > 1.0f ? sr : 48000.0f;
    updateCoefficients();
  }
  /** New settings, from the next sample, mid-note too: a segment already
   *  running carries on at its new rate, toward the new sustain. Two exp()
   *  per call, so once per block rather than per sample. */
  void setParameters(const Parameters& p) {
    p_ = p;
    // A sustain that is not finite is none. The level decays toward it and
    // became NaN; a NaN level never reaches a target or falls under the
    // release floor, so the voice sounded NaN and stayed active through
    // noteOff and the next noteOn alike, until reset(). The times need no
    // such line: a NaN fails their "long enough" tests and takes the floor.
    if (!std::isfinite(p_.sustain)) p_.sustain = 0.0f;
    updateCoefficients();
  }
  /** Start the attack, from the current level: a retriggered note ramps up
   *  from where the voice is instead of clicking to zero first. */
  void noteOn() { stage_ = Attack; }
  /** Start the release from the current level, whatever the stage; nothing
   *  when idle. */
  void noteOff() { if (stage_ != Idle) stage_ = Release; }
  /** False when idle: before the first noteOn(), after reset(), and once a
   *  release falls under -80 dB, about 1.33 times the release time from
   *  full level. A voice is free to reuse when this is false. */
  bool isActive() const { return stage_ != Idle; }
  /** Idle at level 0 at once, with no release: for a voice being reused or
   *  a transport reset, where a tail is not wanted. */
  void reset() { stage_ = Idle; level_ = 0.0f; }

  /** Advance one sample and return the level (0 when idle, 1 at the top of
   *  the attack): the gain for that sample of the voice. */
  inline float getNextSample() {
    switch (stage_) {
      case Attack: {
        level_ += attackStep_;
        if (level_ >= 1.0f) { level_ = 1.0f; stage_ = Decay; }
        break;
      }
      case Decay: {
        level_ = p_.sustain + decayK_ * (level_ - p_.sustain);
        if (std::fabs(level_ - p_.sustain) < 1e-4f) { level_ = p_.sustain; stage_ = Sustain; }
        break;
      }
      case Sustain:
        level_ = p_.sustain;
        break;
      case Release: {
        level_ *= releaseK_;
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
  /** Worked out when the settings or the rate change, not per sample: the
   *  envelope runs on every sample of every voice, and decay and release
   *  were an exp() each time. */
  void updateCoefficients() {
    attackStep_ = 1.0f / (p_.attack > 1e-4f ? p_.attack * sr_ : 1.0f);
    decayK_ = std::exp(-kDecay60 / (p_.decay > 1e-4f ? p_.decay * sr_ : 1.0f));
    releaseK_ = std::exp(-kDecay60 / (p_.release > 1e-4f ? p_.release * sr_ : 1.0f));
  }

  Stage stage_ = Idle;
  float sr_ = 48000.0f, level_ = 0.0f;
  Parameters p_{};
  float attackStep_ = 0.0f, decayK_ = 0.0f, releaseK_ = 0.0f;
};

/** Feed-forward compressor with a peak detector, soft knee and makeup.
 *  `gainReduction()` reports the last computed GR in positive dB, as every
 *  dynamics class here does; the acceptance gates measure exactly this
 *  number. */
class Compressor {
public:
  /** -18 dB threshold, 4:1, a 6 dB knee, no makeup, and the detector's
   *  10 ms attack and 100 ms release, at 48 kHz. The times used to wait for
   *  setSampleRate() or setAttack()/setRelease(): until then the detector
   *  followed the peak sample by sample, and a 100 Hz tone over the
   *  threshold came out with its gain modulated at 200 Hz. */
  Compressor() {
    refreshQuiet();
    setSampleRate(48000.0f);
  }
  /** Sample rate in Hz (48 kHz if not above 1, as EnvFollower takes it);
   *  works the detector's attack and release out again for it (10 ms and
   *  100 ms unless set). */
  void setSampleRate(float sr) {
    sr_ = sr > 1.0f ? sr : 48000.0f;
    env_.setSampleRate(sr_);
    env_.setTimes(attackMs_, releaseMs_);
  }
  /** Threshold in dBFS: where the knee is centred. */
  void setThreshold(float db) {
    thresholdDb_ = db;
    refreshQuiet();
  }
  /** Ratio, n:1 above the threshold; under 1 is taken as 1, no compression,
   *  and so is a NaN, which the old `ratio < 1` test let through: every gain
   *  above the knee came out NaN, and so did the output. There is no
   *  infinite setting: a very large ratio is a limiter's curve. */
  void setRatio(float ratio) { ratio_ = ratio >= 1.0f ? ratio : 1.0f; }
  /** Knee width in dB, centred on the threshold (half below, half above);
   *  0 is a hard knee, a negative width is taken as 0. */
  void setKnee(float db) {
    kneeDb_ = db < 0.0f ? 0.0f : db;
    refreshQuiet();
  }
  /** The detector's attack time constant in ms (floored at 0.01). Two exp()
   *  per call. */
  void setAttack(float ms) { attackMs_ = ms; env_.setTimes(attackMs_, releaseMs_); }
  /** The detector's release time constant in ms (floored at 0.01). Two
   *  exp() per call. */
  void setRelease(float ms) { releaseMs_ = ms; env_.setTimes(attackMs_, releaseMs_); }
  /** Gain after the compression, in dB, applied to every sample. */
  void setMakeup(float db) {
    makeupDb_ = db;
    refreshQuiet();
  }
  /** Clear the detector and the reported gain reduction; the settings stay. */
  void reset() { env_.reset(); grDb_ = 0.0f; }

  /** Compute the gain for this sample from a (possibly external) detector input. */
  inline float computeGain(float detector) {
    const float level = env_.process(detector);
    // Below where the knee starts the reduction is exactly zero and the gain
    // is the makeup, a constant: a log and a pow every sample, for nothing,
    // in the range a compressor spends most of its time in.
    if (level < quietBelow_) {
      grDb_ = 0.0f;
      return quietGain_;
    }
    const float levelDb = gainToDb(level);
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
  /** One sample, compressed by its own level: x times computeGain(x). For a
   *  sidechain, or one gain for linked channels, call computeGain() with the
   *  detector signal and multiply each channel by it. */
  inline float process(float x) { return x * computeGain(x); }
  /** Gain reduction in dB, POSITIVE (6 means 6 dB quieter), which is what a
   *  meter reads and what every other dynamics class here reports. The signed
   *  gain change is `-gainReduction()`. */
  float gainReduction() const { return grDb_ < 0.0f ? -grDb_ : 0.0f; }

private:
  float sr_ = 48000.0f;
  float thresholdDb_ = -18.0f, ratio_ = 4.0f, kneeDb_ = 6.0f;
  float attackMs_ = 10.0f, releaseMs_ = 100.0f, makeupDb_ = 0.0f, grDb_ = 0.0f;
  /** The level under which the knee has not begun, a hair inside it so the
   *  shortcut never claims a sample the curve would have touched, and the
   *  gain there. */
  float quietBelow_ = 0.0f, quietGain_ = 1.0f;
  void refreshQuiet() {
    quietBelow_ = dbToGain(thresholdDb_ - kneeDb_ * 0.5f) * 0.9999f;
    quietGain_ = dbToGain(0.0f + makeupDb_);
  }
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
 * 2 kHz, 0.66 dB at 8 kHz, 1.58 dB at 16 kHz. A STATIC fractional delay
 * lowpasses too, by a fixed amount: at 10.5 samples, 8 kHz is -1.25 dB
 * through read() and -0.23 dB through readCubic(). At a whole-sample delay
 * both are exact.
 *
 * And a third, which is a different trade rather than a better one:
 *
 *   readThiran() first-order allpass (Thiran). Its magnitude is flat BY
 *                CONSTRUCTION at every frequency and every fraction (within
 *                5e-8 in float, measured) -- the one read here
 *                that does not lowpass a half-sample delay -- and its delay is
 *                exact at DC. The price is phase: the delay drifts with
 *                frequency (dispersion), and because it is recursive it has a
 *                state, so each read head needs its own ThiranState and must
 *                be read once per sample. The read for a fractional delay
 *                inside a FEEDBACK loop -- a waveguide string, a tuned comb --
 *                where a lowpass on every pass would decay the high partials
 *                and detune the loop.
 */
template <int MaxSamples>
class DelayLine {
public:
  /**
   * One Thiran read head's memory: the allpass's previous output, and the
   * delay it was last read at.
   *
   * Separate from the line because one line can feed several heads (the
   * crossfade in delay.h opens a second one) and each recursion needs its
   * own past. The line keeps one for readThiran(float) and resets it with
   * itself; a head declared outside the line is reset by its owner.
   */
  struct ThiranState {
    float y1 = 0.0f;          ///< the allpass's previous output
    float lastDelay = -1.0f;  ///< negative: nothing read since reset, so seed
    float coefficient = 0.0f; ///< the allpass's a, cached while the delay holds still
    int integerPart = 1;      ///< N, the whole samples read straight from the buffer
    /** Forget the past: the next read seeds itself, as a head just opened
     *  does. A head kept outside the line is reset by its owner, with the
     *  line. */
    void reset() {
      y1 = 0.0f;
      lastDelay = -1.0f;
    }
  };

  /** The spline used by readCubic(). Declared here rather than pulled from
   *  shaping.h because dsp.h defines DelayLine BEFORE that header is
   *  chained in: the alternative is a circular include. */
  static inline float catmullRomTap(float y0, float y1, float y2, float y3, float frac) {
    // Forwarded, not copied. The same cubic lived here and in the
    // interpolator family, which is one copy too many for a rule this SDK has
    // already watched drift once.
    return CatmullRomKernel::at4(y0, y1, y2, y3, frac);
  }

  /** Silence the whole buffer and reset the line's own Thiran head:
   *  MaxSamples writes, so from prepare() or a transport reset rather than
   *  every block for a long line. No allocation. */
  void reset() {
    for (int i = 0; i < MaxSamples; ++i) buf_[i] = 0.0f;
    w_ = 0;
    thiran_.reset();
  }
  /** Write the next sample, over the oldest one. Once per sample; every
   *  read counts its delay back from the last sample written. */
  inline void write(float x) {
    buf_[w_] = x;
    if (++w_ >= MaxSamples) w_ = 0;
  }

  /**
   * Read through a first-order Thiran allpass. Call ONCE PER SAMPLE per head,
   * on the same side of write() every time, like every other read here.
   *
   * The delay is split into an integer part N read straight from the buffer
   * and a fraction d that the allpass  H(z) = (a + z^-1) / (1 + a z^-1),
   * a = (1 - d) / (1 + d)  supplies; its group delay at DC is exactly d.
   *
   * The split keeps d in [0.618, 1.618), not [0, 1). The pole sits at -a, and
   * a fraction near zero puts it near -1: a filter that rings for hundreds of
   * samples after every change, with the rounding error ringing along. At
   * the golden-ratio bounds a is +-0.236 at worst -- symmetric, so neither
   * end is the bad one -- and any disturbance is gone 12.5 dB per sample.
   * The one place it cannot hold is the bottom of the range: a delay
   * between 1 and 1.618 samples has no buffer sample to borrow from, and
   * rings longer there.
   *
   * Two things a recursive read has to get right that a stateless one gets
   * free:
   *   - a delay that SLIDES needs nothing: the two taps are read at absolute
   *     delays N and N+1, so the previous output is still a valid sample of
   *     the same signal when N steps;
   *   - a delay that JUMPS (a preset, a tap-tempo, a head that has just been
   *     opened) would feed the recursion an output from somewhere else in
   *     the buffer. A move of a sample or more reseeds the state with what
   *     the previous output would have been at the NEW delay, so the first
   *     sample out is not a transient.
   * The corollary: a head that moves a whole sample EVERY sample -- an
   * octave-up pitch shift -- is reseeded every sample and is no longer an
   * allpass. That is readCubic()'s job.
   */
  inline float readThiran(float delaySamples, ThiranState& state) const {
    // A NaN delay is the shortest, as in readCubic; this read holds state,
    // so a NaN here would also stay in it.
    const float d = delaySamples == delaySamples
                        ? clampf(delaySamples, 1.0f, (float) (MaxSamples - 3))
                        : 1.0f;
    if (d != state.lastDelay) {
      int n = (int) d; // d >= 1, so truncation is floor
      float frac = d - (float) n;
      // n > 1 keeps the integer tap inside the written region. An exact
      // integer is the exception: frac becomes 1, the coefficient 0, and the
      // read is the plain sample at N+1 whatever N is.
      if (frac < kThiranLow && (n > 1 || frac == 0.0f)) {
        --n;
        frac += 1.0f;
      }
      state.coefficient = (1.0f - frac) / (1.0f + frac);
      state.integerPart = n;
      const bool jumped = state.lastDelay < 0.0f || std::fabs(d - state.lastDelay) >= 1.0f;
      state.lastDelay = d;
      if (jumped) state.y1 = read(d + 1.0f);
    }
    int i1 = w_ - state.integerPart - 1; // delay N+1
    if (i1 < 0) i1 += MaxSamples;
    // Delay N. At N == 0 the coefficient is exactly 0 and the slot is never
    // weighed -- but buf_[w_] is the OLDEST sample, so read a written one.
    int i0 = state.integerPart > 0 ? i1 + 1 : i1;
    if (i0 >= MaxSamples) i0 -= MaxSamples;
    const float y = state.coefficient * (buf_[i0] - state.y1) + buf_[i1];
    state.y1 = y;
    return y;
  }
  /** The same, through the line's own head: reset() clears it. */
  inline float readThiran(float delaySamples) { return readThiran(delaySamples, thiran_); }

  /** Read with THIRD-ORDER interpolation. This is what a modulated delay
   *  should use: linear interpolation's response changes with the fraction,
   *  so sweeping the delay sweeps a lowpass along with it. */
  inline float readCubic(float delaySamples) const {
    // A NaN read position became an index far outside the buffer: a NaN
    // delay -- an LFO fed a NaN depth, a parameter divided by zero -- reads
    // as the shortest delay instead. clampf() now sends NaN there itself;
    // the test stays where an index is at stake.
    const float d = delaySamples == delaySamples
                        ? clampf(delaySamples, 2.0f, (float) (MaxSamples - 3))
                        : 2.0f;
    // The read point in whole samples and a fraction, kept apart (see
    // split()). It was w_ - d in one float, wrapped by adding MaxSamples:
    // that also rounded EXACTLY onto MaxSamples, one past the array, when a
    // modulated delay put w_ a hair under d -- UBSan's find.
    float frac;
    const int i1 = split(d, frac);
    // Neighbours wrapped by a compare, not a modulo: three signed `%` per
    // read was most of the arithmetic outside the spline itself, and every
    // modulated delay -- chorus, flanger, vibrato, the pitch shifter's two
    // heads -- pays it per sample.
    const int i0 = i1 > 0 ? i1 - 1 : MaxSamples - 1;
    int i2 = i1 + 1;
    if (i2 >= MaxSamples) i2 -= MaxSamples;
    int i3 = i1 + 2;
    if (i3 >= MaxSamples) i3 -= MaxSamples;
    return catmullRomTap(buf_[i0], buf_[i1], buf_[i2], buf_[i3], frac);
  }

  /** Linear read, `delaySamples` back: 1 is the sample last written, so a
   *  read BEFORE write() (as tap() does) delays by exactly that many
   *  samples, and a read after it by one fewer. Held to 1 .. MaxSamples - 2;
   *  a NaN reads at 1. For a delay that does not move (see the class). */
  inline float read(float delaySamples) const {
    const float d = delaySamples == delaySamples // NaN: see readCubic
                        ? clampf(delaySamples, 1.0f, (float) (MaxSamples - 2))
                        : 1.0f;
    float frac;
    const int i0 = split(d, frac);
    int i1 = i0 + 1;
    if (i1 >= MaxSamples) i1 -= MaxSamples;
    return buf_[i0] + frac * (buf_[i1] - buf_[i0]);
  }
  /** read(delaySamples), then write(x): one sample through a delay of
   *  `delaySamples`, linearly interpolated. Returns the delayed sample. */
  inline float tap(float delaySamples, float x) {
    const float y = read(delaySamples);
    write(x);
    return y;
  }

private:
  // The golden-ratio split point: (1 - d)/(1 + d) is +0.236 at 0.618 and
  // -0.236 at 1.618.
  static constexpr float kThiranLow = 0.618034f;

  /** Where a read at delay `d` (at least 1) lands: the index of the sample
   *  at or before the read point, and how far past it the point sits.
   *
   *  The whole samples are integers and the fraction is d's own, so the
   *  fraction is as fine in a long line as in a short one. Worked as one
   *  float, w_ - d, it had the precision of a number the size of the line
   *  -- a sixty-fourth of a sample past 131072, an eighth past 2^20 -- and
   *  rounded by an amount that moved as the write point went round: a
   *  static 100.3-sample read of a 5 kHz tone through the cubic tap was
   *  0.037 off in a 2^21-sample line, against 0.005 in a short one. */
  inline int split(float d, float& frac) const {
    const int whole = (int) d; // d >= 1, so truncation is floor
    const float part = d - (float) whole; // exact
    int i = w_ - whole;
    frac = 0.0f;
    if (part > 0.0f) {
      --i;
      frac = 1.0f - part;
    }
    if (i < 0) i += MaxSamples;
    return i;
  }

  float buf_[MaxSamples] = {0.0f};
  int w_ = 0;
  ThiranState thiran_;
};

/** Schroeder allpass: the diffusion element of a reverb. */
template <int MaxSamples>
class Allpass {
public:
  /** Silence the delay line (MaxSamples writes). */
  void reset() { line_.reset(); }
  /** Delay in samples, which the line holds to 1 .. MaxSamples - 2, and
   *  gain `g`, held to -0.95 .. 0.95: the nearer to +-1, the longer its
   *  echoes ring. 100 samples and 0.5 until set. */
  void set(float delaySamples, float g) { d_ = delaySamples; g_ = clampf(g, -0.95f, 0.95f); }
  /** One sample through the allpass; returns it. Flat in magnitude once
   *  settled, smeared in time. */
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
  /** Silence the delay line (MaxSamples writes) and the damping filter. */
  void reset() { line_.reset(); damp_.reset(); }
  /** The loop's length in samples (the line holds it to 1 .. MaxSamples - 2),
   *  `feedback` held to 0 .. 0.98 (higher rings longer), and the corner of
   *  the lowpass in the loop, `dampingHz` at sample rate `sr`: the lower it
   *  is, the faster the highs die. One tan() per call. */
  void set(float delaySamples, float feedback, float dampingHz, float sr) {
    d_ = delaySamples;
    fb_ = clampf(feedback, 0.0f, 0.98f);
    damp_.setSampleRate(sr);
    damp_.setCutoff(dampingHz);
  }
  /** One sample: returns the line's output, and writes the input plus the
   *  damped output times the feedback back into it. Until set(): 1000
   *  samples at 0.7 feedback. */
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
  /** All dry: a mix of 0. */
  DryWetMixer() = default;
  /** Landed on `mix` (see setMix()), not gliding to it: what an effect
   *  whose default mix is not 0 holds its mixer as. */
  explicit DryWetMixer(float mix) { setMix(mix); }
  /** The glide's length against the rate it runs at. Left alone it is 960
   *  calls: 20 ms at 48 kHz, 5 at 192 -- smoothing either way. */
  void setSampleRate(float sr, float ms = 20.0f) {
    const float n = 0.001f * (ms > 0.1f ? ms : 0.1f) * (sr > 1.0f ? sr : 48000.0f);
    k_ = 1.0f - std::exp(-1.0f / n);
  }
  /** A new mix GLIDES in once audio is running. It used to land in one
   *  sample, and a mix knob automated on a delay stepped the output 52
   *  times its largest natural step -- on every reverb, the chorus and the
   *  phaser alike. Set before the first process(), or after snap(), it
   *  lands at once: a starting mix is a setting, not a movement. */
  void setMix(float mix) {
    const float m = clampf(mix, 0.0f, 1.0f);
    wetTarget_ = std::sin(m * kPi * 0.5f);
    dryTarget_ = std::cos(m * kPi * 0.5f);
    if (!running_) snap();
  }
  /** Land on the mix now: what a reset() means. */
  void snap() {
    dry_ = dryTarget_;
    wet_ = wetTarget_;
    running_ = false;
  }
  /** One sample of the mix, `dry` times cos(mix pi/2) plus `wet` times
   *  sin(mix pi/2), taking a changing mix one glide step on. The glide
   *  steps per CALL: one mixer shared by two channels glides twice as fast
   *  as setSampleRate() says. */
  inline float process(float dry, float wet) {
    running_ = true;
    if (dry_ != dryTarget_ || wet_ != wetTarget_) {
      dry_ += (dryTarget_ - dry_) * k_;
      wet_ += (wetTarget_ - wet_) * k_;
      if (std::fabs(dryTarget_ - dry_) < 1e-6f && std::fabs(wetTarget_ - wet_) < 1e-6f) {
        dry_ = dryTarget_;
        wet_ = wetTarget_;
      }
    }
    return dry_ * dry + wet_ * wet;
  }

private:
  float dry_ = 1.0f, wet_ = 0.0f, dryTarget_ = 1.0f, wetTarget_ = 0.0f;
  float k_ = 0.00104112f; // 1 - exp(-1/960)
  bool running_ = false;
};

/**
 * A dry/wet mixer that DELAYS THE DRY PATH to match a wet path that is late.
 *
 * DryWetMixer above assumes the two signals are aligned. A wet path that runs
 * through an oversampler, a partitioned convolver or a look-ahead detector is
 * not: it comes out N samples late, and summing it against an undelayed dry
 * signal is adding a signal to a delayed copy of itself. That is a comb
 * filter (a row of notches every 1/N of the sample rate), and it is at its
 * worst at 50% mix, exactly where most users leave the control.
 *
 * There is a second failure that is easier to miss. A plugin whose wet path is
 * late usually DECLARES that latency, and the host then shifts the plugin's
 * whole output earlier to compensate. The dry signal was never late, so the
 * host has just pushed it early, against every other track in the session.
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
  /** The mix glide's length against the rate, as DryWetMixer::setSampleRate()
   *  takes them: 20 ms unless `ms` says otherwise, 960 samples until this is
   *  called. Call it from prepare(). */
  void setSampleRate(float sr, float ms = 20.0f) {
    for (DryWetMixer& m : mixer_) m.setSampleRate(sr, ms);
  }
  /** The mix, 0 (dry) .. 1 (wet), held to that range: equal-power, and
   *  gliding once audio runs, as DryWetMixer::setMix(). Each channel glides
   *  on its own, so a stereo pair takes the glide setSampleRate() set, not
   *  half of it: one glide shared by both channels was stepped twice a
   *  frame. */
  void setMix(float mix) {
    for (DryWetMixer& m : mixer_) m.setMix(mix);
  }

  /** How late the wet path is, in samples. Call from prepare(): it clears the
   *  delay buffer, which is not something to do while audio is running. */
  void setWetLatency(int samples) {
    latency_ = samples < 0 ? 0 : (samples > MaxLatency ? MaxLatency : samples);
    reset();
  }
  /** The dry delay in samples, as setWetLatency() held it: 0 .. MaxLatency. */
  int wetLatency() const { return latency_; }

  /** Silence the dry delays and land the mix on its target: MaxChannels
   *  times MaxLatency writes, so from prepare() or a transport reset, not
   *  per block. */
  void reset() {
    for (int c = 0; c < MaxChannels; ++c) {
      for (int i = 0; i < MaxLatency; ++i) dry_[c][i] = 0.0f;
      write_[c] = 0;
    }
    for (DryWetMixer& m : mixer_) m.snap();
  }

  /** One sample of ONE channel.
   *
   *  The channel is explicit and each carries its own write cursor and its
   *  own mix glide, so this is correct however the caller nests its loops. A
   *  single shared cursor would work only for `for each sample { for each
   *  channel }` and would interleave the channels' histories the other way
   *  round, a bug that sounds like a subtle stereo smear and is invisible in
   *  mono. A channel outside 0 .. MaxChannels - 1 is mixed undelayed, through
   *  one more glide that every such channel shares. */
  inline float process(int channel, float dry, float wet) {
    if (channel < 0 || channel >= MaxChannels) return mixer_[MaxChannels].process(dry, wet);
    if (latency_ == 0) return mixer_[channel].process(dry, wet);
    float* line = dry_[channel];
    int& w = write_[channel];
    const float delayed = line[w];
    line[w] = dry;
    if (++w >= latency_) w = 0;
    return mixer_[channel].process(delayed, wet);
  }

private:
  /** One glide per channel, and the last for channels out of range. */
  DryWetMixer mixer_[MaxChannels + 1];
  float dry_[MaxChannels][MaxLatency] = {};
  int write_[MaxChannels] = {};
  int latency_ = 0;
};

namespace detail {
// What a voice can do beyond the four methods VoiceManager requires, asked at
// compile time. A voice that has none of these is the voice it always was; one
// that has them gets the matching behaviour, with nothing to register.
template <typename V, typename = void>
struct VoiceRendersMono : std::false_type {};
template <typename V>
struct VoiceRendersMono<V, std::void_t<decltype(std::declval<V&>().render())>> : std::true_type {};
template <typename V, typename = void>
struct VoiceRendersBlock : std::false_type {};
template <typename V>
struct VoiceRendersBlock<V, std::void_t<decltype(std::declval<V&>().renderBlock(
                                (float*) nullptr, (float*) nullptr, 0))>> : std::true_type {};
template <typename V, typename = void>
struct VoiceRendersStereo : std::false_type {};
template <typename V>
struct VoiceRendersStereo<V, std::void_t<decltype(std::declval<V&>().render(std::declval<float&>(),
                                                                           std::declval<float&>()))>>
    : std::true_type {};
template <typename V, typename = void>
struct VoiceHasLegato : std::false_type {};
template <typename V>
struct VoiceHasLegato<V, std::void_t<decltype(std::declval<V&>().legatoTo(0, 0.0f))>> : std::true_type {};
template <typename V, typename = void>
struct VoiceHasPitchBend : std::false_type {};
template <typename V>
struct VoiceHasPitchBend<V, std::void_t<decltype(std::declval<V&>().setPitchBend(0.0f))>> : std::true_type {};
template <typename V, typename = void>
struct VoiceHasTuning : std::false_type {};
template <typename V>
struct VoiceHasTuning<V, std::void_t<decltype(std::declval<V&>().setTuning(0.0f))>> : std::true_type {};
template <typename V, typename = void>
struct VoiceHasPressure : std::false_type {};
template <typename V>
struct VoiceHasPressure<V, std::void_t<decltype(std::declval<V&>().setPressure(0.0f))>> : std::true_type {};
template <typename V, typename = void>
struct VoiceHasBrightness : std::false_type {};
template <typename V>
struct VoiceHasBrightness<V, std::void_t<decltype(std::declval<V&>().setBrightness(0.0f))>> : std::true_type {};
template <typename V, typename = void>
struct VoiceHasModWheel : std::false_type {};
template <typename V>
struct VoiceHasModWheel<V, std::void_t<decltype(std::declval<V&>().setModWheel(0.0f))>> : std::true_type {};
template <typename V, typename = void>
struct VoiceHasKill : std::false_type {};
template <typename V>
struct VoiceHasKill<V, std::void_t<decltype(std::declval<V&>().kill())>> : std::true_type {};
template <typename V, typename = void>
struct VoiceHasParamMod : std::false_type {};
template <typename V>
struct VoiceHasParamMod<V, std::void_t<decltype(std::declval<V&>().setParamMod(0, 0.0f))>>
    : std::true_type {};
} // namespace detail

/**
 * Polyphonic voice manager.
 *
 * `VoiceType` must expose `noteOn(int note, float velocity)`, `noteOff()`,
 * `bool isActive()` and `float render()`. That contract has not changed and
 * does not need to: everything below is bookkeeping this class does on the
 * voices' behalf, so a DSP written against the old four methods gains all of
 * it without being touched.
 *
 * A voice MAY offer more, and is found to at compile time:
 *
 *   void render(float& left, float& right)   a stereo voice: panned, spread
 *                                            unison. Used by render(l, r) and
 *                                            renderVoices(); may replace the
 *                                            mono render() altogether.
 *   void renderBlock(float* left, float* right, int numSamples)
 *                                            ADD this many samples to both
 *                                            buffers: a stretch between two
 *                                            events, the voice's own loop.
 *                                            renderBlock() and renderVoices()
 *                                            use it; may stand in for render().
 *   void legatoTo(int note, float velocity)  move to a new note WITHOUT
 *                                            retriggering: what Legato mode
 *                                            calls, and where a glide starts.
 *   void setPitchBend(float semitones)       the channel's pitch wheel.
 *   void setTuning(float semitones)          this note's own bend (MPE).
 *   void setPressure(float 0..1)             this note's pressure, or the
 *                                            channel's aftertouch.
 *   void setBrightness(float 0..1)           this note's timbre (MPE CC 74).
 *   void setModWheel(float 0..1)             CC 1.
 *   void kill()                              silent now: All Sound Off.
 *   void setParamMod(int param, float amount)
 *                                            a host's offset to parameter
 *                                            `param` for this note alone
 *                                            (CLAP polyphonic modulation).
 *
 * Four things here are the difference between a voice manager and a synth a
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
 * HOW it gets stolen. Restarting a voice that is mid-waveform puts a step in
 * the output, and a step is a click: every stolen voice in a fast passage,
 * every retrigger of a mono line. So the voice's next couple of milliseconds
 * are rendered first, faded out with a raised cosine, and played out under
 * the new note, which starts at once in the same voice. The listener hears
 * the old note leave and the new one arrive, never the seam.
 *
 * The SUSTAIN PEDAL. Without it a piano-style patch is unplayable: the player
 * holds the pedal, lifts their hands, and everything stops. CC 64 does not
 * reach a voice on its own -- a voice knows nothing about pedals -- so the
 * manager holds the release back instead, which is exactly what the damper on
 * a real instrument does. Sostenuto (CC 66) is the same trick applied only to
 * what was already down when the pedal fell.
 *
 * MONO and LEGATO. A bass or a lead is played one note at a time, and what
 * makes it playable is what happens between notes: which of the keys held
 * down sounds (the last, the lowest or the highest), what happens when that
 * key comes up while another is still down (the line returns to it, as a
 * trill needs), and whether an overlapping note retriggers the envelope
 * (Mono) or slides into the new pitch without one (Legato, through the
 * voice's legatoTo(), which is where a glide starts).
 */
template <typename VoiceType, int MaxVoices = 8>
class VoiceManager {
  static_assert(MaxVoices >= 1, "a voice manager needs at least one voice");
  // How a voice renders is asked only where the manager renders it -- render()
  // and the steal fade -- so a DSP that mixes its voices itself (the sampler
  // reads each channel through render(channel)) keeps using the bookkeeping.
  static constexpr bool kCanRender =
      detail::VoiceRendersMono<VoiceType>::value || detail::VoiceRendersStereo<VoiceType>::value;
  // A stretch at a time: through the voice's own renderBlock(), or its
  // per-sample render() run in a loop.
  static constexpr bool kCanRenderBlock = kCanRender || detail::VoiceRendersBlock<VoiceType>::value;

public:
  /** How many voices there are, MaxVoices: the range voice() is indexed by. */
  static constexpr int kNumVoices = MaxVoices;
  /** "Whichever voice is playing this key, on any channel." The default for
   *  every lookup, so a plain MIDI keyboard behaves exactly as it always did. */
  static constexpr int kAnyChannel = -1;
  /** The keys Mono and Legato remember, newest last: more fingers than hands. */
  static constexpr int kMaxHeldNotes = 32;
  /** The longest steal fade, in samples: 2 ms at 192 kHz. */
  static constexpr int kMaxStealFade = 384;

  /** How notes share the voices. */
  enum class Mode {
    Poly,   ///< every note its own voice, up to polyphony()
    Mono,   ///< one voice; every note starts it again
    Legato, ///< one voice; a note played over a held one moves it without a restart
  };
  /** Which held key a mono line plays. */
  enum class NotePriority {
    Last, ///< the most recent key: what most synths and most players expect
    Low,  ///< the lowest key held: a bass line under a hand that wanders
    High, ///< the highest key held: a lead over sustained lower notes
  };

  VoiceManager() { setSampleRate(48000.0); }

  /** Voice `i`, 0 .. kNumVoices - 1, unchecked: to set every voice's
   *  parameters, or to reach the one voiceForNote() found. */
  VoiceType& voice(int i) { return voices_[i]; }
  /** As above, read-only. */
  const VoiceType& voice(int i) const { return voices_[i]; }

  /** The rate the steal fade is timed against: 2 ms, so the same fade at any
   *  rate. Call from prepare(); before it, 48 kHz is assumed. */
  void setSampleRate(double sampleRate) {
    int n = sampleRate > 0.0 ? (int) (0.002 * sampleRate) : 96;
    if (n < 16) n = 16;
    if (n > kMaxStealFade) n = kMaxStealFade;
    fadeLength_ = n;
    // A raised cosine from 1 to 0: no corner where it starts, none where it
    // ends, which a linear ramp has at both.
    for (int k = 0; k < n; ++k)
      fade_[k] = 0.5f * (1.0f + std::cos(3.14159265358979f * (float) (k + 1) / (float) n));
  }
  /** The steal fade's length, in samples. */
  int stealFadeSamples() const { return fadeLength_; }

  /** How many voices notes may use, 1 .. kNumVoices: a "Voices" control.
   *  Voices above a lowered limit are released, not cut, and stealing then
   *  happens within the limit. */
  void setPolyphony(int voices) {
    const int n = voices < 1 ? 1 : (voices > MaxVoices ? MaxVoices : voices);
    // `i < MaxVoices` is always implied by polyphony_ and says so to a
    // compiler's bounds check, which cannot see that it is.
    for (int i = n; i < polyphony_ && i < MaxVoices; ++i) {
      held_[i] = false;
      pedalHeld_[i] = false;
      sostenuto_[i] = false;
      releaseSlot(i);
    }
    polyphony_ = n;
  }
  /** The limit setPolyphony() set; kNumVoices until then. */
  int polyphony() const { return polyphony_; }

  /** Poly, Mono or Legato. A change releases what is sounding: a chord has
   *  no single note to become, and a note left over from the old mode would
   *  have no key to end it. */
  void setMode(Mode mode) {
    if (mode == mode_) return;
    allNotesOff();
    mode_ = mode;
  }
  Mode mode() const { return mode_; }

  /** Which held key a Mono or Legato line plays. */
  void setNotePriority(NotePriority priority) { priority_ = priority; }
  NotePriority notePriority() const { return priority_; }

  /** Whether bend and pressure arrive as per-note expression (MPE, decoded
   *  by the wrapper into kExprTuning and kExprPressure) rather than as
   *  channel messages. On, applyMidi() leaves the pitch wheel and channel
   *  pressure to the expression path, which has already folded them into
   *  each note: applying them again would bend every note twice. */
  void setMpe(bool on) { mpe_ = on; }
  bool isMpe() const { return mpe_; }

  /** Start a note.
   *
   *  The CHANNEL is recorded, and that is not bookkeeping. MPE puts every
   *  finger on its own channel, so two fingers landing on the SAME key are two
   *  separate notes that arrive as (60, ch2) and (60, ch3). Without the
   *  channel this class could not tell them apart: lifting one finger released
   *  both, and a bend on one bent whichever voice was found first.
   *
   *  `noteId` is the host's name for this note, where it has one (CLAP): kept
   *  with the voice so voiceForNoteId() finds it and drainEndedNotes() can
   *  say when it is gone. */
  void noteOn(int note, float velocity, int channel = 0, int32_t noteId = -1) {
    if (mode_ != Mode::Poly) {
      monoNoteOn(note, velocity, channel, noteId);
      return;
    }
    // A key struck again while a pedal holds its last note -- the key is up,
    // the note sounds on the pedal's account -- releases that note first, as
    // the reference does and as a re-struck string is damped and struck
    // again. Each strike took a voice of its own and the pedal kept the old
    // ones, so a note repeated under the pedal stacked copies of itself: on
    // a sustaining envelope, four strikes were four voices at full level
    // until the pedal came up. The same key on another channel is another
    // finger (MPE) and is left alone.
    for (int i = 0; i < MaxVoices; ++i)
      if (notes_[i] == note && channels_[i] == channel && voices_[i].isActive() && !held_[i] &&
          !released_[i])
        releaseSlot(i);
    int slot = -1;
    for (int i = 0; i < polyphony_; ++i) {
      if (!voices_[i].isActive()) {
        slot = i;
        break;
      }
    }
    if (slot < 0) {
      slot = voiceToSteal(note, channel);
      steal(slot);
    }
    start(slot, note, velocity, channel, noteId);
  }

  /** The voice sounding this key on this channel, or -1.
   *
   *  Per-note expression is addressed by key AND channel, because that pair is
   *  what identifies a finger under MPE. Passing kAnyChannel gives the old
   *  behaviour -- the first voice on that key -- which is right for a keyboard
   *  that puts everything on one channel and wrong for a controller that does
   *  not.
   *
   *  The NEWEST such voice. A key struck again while its last note is still
   *  in its release tail sounds on two voices, and the first one found was
   *  usually the old tail: the new note's bend and pressure -- and the
   *  sampler's reset of its tuning at the note-on -- went to the voice that
   *  was dying, so the note being played kept whatever bend its voice last
   *  had and ignored the wheel. Whatever arrives for a key and channel is
   *  about the note most recently started there. */
  int voiceForNote(int note, int channel = kAnyChannel) const {
    int found = -1;
    for (int i = 0; i < MaxVoices; ++i)
      if (notes_[i] == note && voices_[i].isActive() &&
          (channel == kAnyChannel || channels_[i] == channel) &&
          (found < 0 || age_[i] > age_[found]))
        found = i;
    return found;
  }

  /** The voice playing the note the host named `noteId`, or -1: how a CLAP
   *  host addresses one note among several on the same key. */
  int voiceForNoteId(int32_t noteId) const {
    if (noteId < 0) return -1;
    for (int i = 0; i < MaxVoices; ++i)
      if (noteIds_[i] == noteId && voices_[i].isActive()) return i;
    return -1;
  }

  /** What voice `i` is playing: its key, its channel and the host's id for
   *  the note (-1 where there was none). */
  int noteOf(int i) const { return notes_[i]; }
  int channelOf(int i) const { return channels_[i]; }
  int32_t noteIdOf(int i) const { return noteIds_[i]; }

  /** Voices sounding now, release tails included: what a voice meter shows
   *  and what CLAP's voice-info and a DSP's activeVoices() report. */
  int activeVoices() const {
    int n = 0;
    for (int i = 0; i < MaxVoices; ++i)
      if (voices_[i].isActive()) ++n;
    return n;
  }

  /** Lift a key. With a channel, only the finger that played it; without one,
   *  every voice on that key.
   *
   *  The KEY coming up and the NOTE ending are different events, and a pedal
   *  is what separates them. So this always records that the key is up, and
   *  only releases the voice when nothing is holding it. */
  void noteOff(int note, int channel = kAnyChannel) {
    if (mode_ != Mode::Poly) {
      monoNoteOff(note, channel);
      return;
    }
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

  /** Whether the damper pedal is down, as sustainPedal() last set it. */
  bool isSustainDown() const { return sustainDown_; }

  /** Release everything, pedals included. This is what CC 123 and a panic
   *  button mean: a voice still sounding afterwards because a pedal was down
   *  is the exact failure the user pressed the button to stop. */
  void allNotesOff() {
    stackSize_ = 0;
    for (int i = 0; i < MaxVoices; ++i) {
      held_[i] = false;
      pedalHeld_[i] = false;
      sostenuto_[i] = false;
      releaseSlot(i);
    }
  }

  /** Silence now, CC 120 and a host's choke: what allNotesOff() does, and a
   *  voice with kill() is stopped outright rather than left to its release.
   *  Through the steal fade either way, so silence arrives without a click. */
  void allSoundOff() {
    if constexpr (detail::VoiceHasKill<VoiceType>::value) {
      for (int i = 0; i < MaxVoices; ++i)
        if (voices_[i].isActive()) {
          fadeOut(i);
          voices_[i].kill();
        }
    }
    allNotesOff();
  }

  /** The pitch wheel, in semitones, for one channel or for all of them. Voices
   *  with setPitchBend() hear it now, and a note started later on that channel
   *  starts bent. */
  void pitchBend(float semitones, int channel = kAnyChannel) {
    for (int c = 0; c < 16; ++c)
      if (channel == kAnyChannel || channel == c) bend_[c] = semitones;
    if constexpr (detail::VoiceHasPitchBend<VoiceType>::value) {
      for (int i = 0; i < MaxVoices; ++i)
        if (voices_[i].isActive() && (channel == kAnyChannel || channels_[i] == channel))
          voices_[i].setPitchBend(semitones);
    }
  }
  /** A channel's wheel, as pitchBend() last set it; 0 for kAnyChannel. */
  float pitchBendOf(int channel) const { return channel >= 0 && channel < 16 ? bend_[channel] : 0.0f; }
  /** How far a full wheel throw bends, in semitones, for applyMidi(): 2 by
   *  default, as the MIDI spec has it and as most keyboards are set. */
  void setPitchBendRange(float semitones) { bendRange_ = semitones; }
  float pitchBendRange() const { return bendRange_; }

  /** Channel aftertouch, 0 .. 1, to the voices on that channel that have
   *  setPressure(). */
  void channelPressure(float pressure, int channel = kAnyChannel) {
    if constexpr (detail::VoiceHasPressure<VoiceType>::value) {
      for (int i = 0; i < MaxVoices; ++i)
        if (voices_[i].isActive() && (channel == kAnyChannel || channels_[i] == channel))
          voices_[i].setPressure(pressure);
    } else {
      (void) pressure;
      (void) channel;
    }
  }

  /** The mod wheel, CC 1, 0 .. 1: kept, and passed to every voice that has
   *  setModWheel(), sounding or not, so a note started later starts with it. */
  void modWheel(float value) {
    modWheel_ = value;
    if constexpr (detail::VoiceHasModWheel<VoiceType>::value)
      for (int i = 0; i < MaxVoices; ++i) voices_[i].setModWheel(value);
  }
  float modWheelValue() const { return modWheel_; }

  /** Sum every active voice for one sample: the mono mix. A stereo-only voice
   *  is folded to the middle. */
  inline float render() {
    static_assert(kCanRender, "VoiceManager::render() needs a voice with float render(), or "
                              "void render(float& left, float& right)");
    float sum = 0.0f;
    for (int i = 0; i < MaxVoices; ++i) {
      if (voices_[i].isActive()) {
        if constexpr (detail::VoiceRendersMono<VoiceType>::value) {
          sum += voices_[i].render();
        } else {
          float l = 0.0f, r = 0.0f;
          voices_[i].render(l, r);
          sum += 0.5f * (l + r);
        }
        sounding_[i] = true;
      } else if (sounding_[i]) {
        sounding_[i] = false;
        noteEnded(i);
      }
      if (tailPos_[i] < tailLen_[i]) {
        sum += tailStereo_ ? 0.5f * (tailL_[i][tailPos_[i]] + tailR_[i][tailPos_[i]])
                           : tailL_[i][tailPos_[i]];
        ++tailPos_[i];
      }
    }
    return sum;
  }

  /** Sum every active voice for one sample, in stereo. A mono voice sits in
   *  the middle, the same sample on both sides. */
  inline void render(float& left, float& right) {
    static_assert(kCanRender, "VoiceManager::render(l, r) needs a voice with float render(), or "
                              "void render(float& left, float& right)");
    float l = 0.0f, r = 0.0f;
    for (int i = 0; i < MaxVoices; ++i) {
      if (voices_[i].isActive()) {
        if constexpr (detail::VoiceRendersStereo<VoiceType>::value) {
          float vl = 0.0f, vr = 0.0f;
          voices_[i].render(vl, vr);
          l += vl;
          r += vr;
        } else {
          const float s = voices_[i].render();
          l += s;
          r += s;
        }
        sounding_[i] = true;
      } else if (sounding_[i]) {
        sounding_[i] = false;
        noteEnded(i);
      }
      if (tailPos_[i] < tailLen_[i]) {
        l += tailL_[i][tailPos_[i]];
        r += tailStereo_ ? tailR_[i][tailPos_[i]] : tailL_[i][tailPos_[i]];
        ++tailPos_[i];
      }
    }
    left = l;
    right = r;
  }

  /**
   * `numSamples` samples of every voice, ADDED into `left` and `right`: the
   * same sound, to the bit, as render(l, r) once per sample, and the call
   * renderVoices() makes for each stretch between two events.
   *
   * How it renders depends on the voice. One with only render() is run
   * sample by sample, every voice in turn at each sample, which is the FAST
   * order for most voices: a filter or an envelope is a chain of operations
   * that each wait for the last, and eight voices' chains interleaved keep
   * the processor busy where one voice's chain run to the end of the block
   * leaves it waiting. Measured on the example synth's voice (a ladder
   * filter, an envelope, a glide), eight voices one after the other took 1.7
   * times as long as eight interleaved, with MSVC and with GCC alike.
   *
   * A voice whose work does run better over a block -- a sample player or a
   * wavetable read in SIMD, coefficients computed once per stretch -- says so
   * by having renderBlock(), and is handed the stretch whole. It ADDS its
   * output, falls silent once it has finished, and keeps its state in locals
   * across its loop (a store through `left` could, for all a compiler knows,
   * be a store to the voice's own floats).
   *
   * A note whose voice finishes in the stretch is found ended by its end
   * (drainEndedNotes()).
   */
  void renderBlock(float* left, float* right, int numSamples) {
    static_assert(kCanRenderBlock, "VoiceManager::renderBlock() needs a voice with renderBlock(), "
                                   "float render() or void render(float& left, float& right)");
    if constexpr (detail::VoiceRendersBlock<VoiceType>::value) {
      for (int i = 0; i < MaxVoices; ++i) {
        if (voices_[i].isActive()) {
          voices_[i].renderBlock(left, right, numSamples);
          sounding_[i] = true;
        }
        if (sounding_[i] && !voices_[i].isActive()) {
          sounding_[i] = false;
          noteEnded(i);
        }
        if (tailPos_[i] < tailLen_[i]) {
          const int remaining = tailLen_[i] - tailPos_[i];
          const int m = remaining < numSamples ? remaining : numSamples;
          const float* tl = tailL_[i] + tailPos_[i];
          const float* tr = tailStereo_ ? tailR_[i] + tailPos_[i] : tl;
          for (int k = 0; k < m; ++k) {
            left[k] += tl[k];
            right[k] += tr[k];
          }
          tailPos_[i] += m;
        }
      }
    } else {
      for (int k = 0; k < numSamples; ++k) {
        float l = 0.0f, r = 0.0f;
        render(l, r);
        left[k] += l;
        right[k] += r;
      }
    }
  }


  /** A note that has stopped sounding: its key, channel and the host's id. */
  struct EndedNote {
    int note = 0;
    int channel = 0;
    int32_t noteId = -1;
  };
  /** Hand every note that has finished since the last call to `f(EndedNote)`
   *  and forget them: voices whose release ran out, and notes a steal or All
   *  Sound Off cut short. A CLAP host needs to hear this (note end) to stop
   *  modulating a voice that is gone. The last 64 are kept. */
  template <typename F>
  void drainEndedNotes(F&& f) {
    for (int i = 0; i < endedCount_; ++i) f(ended_[i]);
    endedCount_ = 0;
  }
  /** Notes have ended since drainEndedNotes() last ran: a check cheap enough
   *  for every sample. */
  bool hasEndedNotes() const { return endedCount_ > 0; }

private:
  void start(int slot, int note, float velocity, int channel, int32_t noteId) {
    notes_[slot] = note;
    channels_[slot] = channel;
    noteIds_[slot] = noteId;
    age_[slot] = ++counter_;
    held_[slot] = true;
    released_[slot] = false;
    pedalHeld_[slot] = false;
    // A key pressed while the sostenuto pedal is already down is NOT pinned:
    // that pedal holds what was down when it fell and nothing after.
    sostenuto_[slot] = false;
    sounding_[slot] = true;
    voices_[slot].noteOn(note, velocity);
    if constexpr (detail::VoiceHasPitchBend<VoiceType>::value)
      voices_[slot].setPitchBend(channel >= 0 && channel < 16 ? bend_[channel] : 0.0f);
    if constexpr (detail::VoiceHasModWheel<VoiceType>::value) voices_[slot].setModWheel(modWheel_);
  }

  void releaseSlot(int i) {
    pedalHeld_[i] = false;
    if (!released_[i]) {
      released_[i] = true;
      voices_[i].noteOff();
    }
  }

  /** A sounding voice is taken for another note: its note ends here, and its
   *  next moments are kept as a fade so the cut is not heard. */
  void steal(int slot) {
    if (!voices_[slot].isActive()) return;
    fadeOut(slot);
    sounding_[slot] = false;
    noteEnded(slot);
  }

  /** Render the voice's next fadeLength_ samples into its tail, faded to
   *  silence, so they play out under whatever the voice does next. Whatever
   *  is left of an earlier tail in the slot is carried into the new one.
   *
   *  A voice the manager cannot render is mixed by its DSP (the sampler reads
   *  each channel itself), and keeps its own counsel about steals: no fade. */
  void fadeOut(int slot) {
    if constexpr (kCanRenderBlock) fadeOutRendered(slot);
    else (void) slot;
  }

  void fadeOutRendered(int slot) {
    if (!voices_[slot].isActive()) return;
    float* tl = tailL_[slot];
    float* tr = tailR_[slot];
    const int left = tailLen_[slot] - tailPos_[slot];
    if (left > 0 && tailPos_[slot] > 0) {
      for (int k = 0; k < left; ++k) {
        tl[k] = tl[tailPos_[slot] + k];
        tr[k] = tr[tailPos_[slot] + k];
      }
    }
    const int carried = left > 0 ? left : 0;
    const int n = fadeLength_;
    if constexpr (detail::VoiceRendersBlock<VoiceType>::value) {
      // The voice's own loop, into silence, then faded onto what is carried.
      float vl[kMaxStealFade] = {}, vr[kMaxStealFade] = {};
      voices_[slot].renderBlock(vl, vr, n);
      for (int k = 0; k < n; ++k) {
        tl[k] = (k < carried ? tl[k] : 0.0f) + vl[k] * fade_[k];
        tr[k] = (k < carried ? tr[k] : 0.0f) + vr[k] * fade_[k];
      }
    } else {
      for (int k = 0; k < n; ++k) {
        const float g = fade_[k];
        float l = 0.0f, r = 0.0f;
        if constexpr (detail::VoiceRendersStereo<VoiceType>::value) {
          voices_[slot].render(l, r);
        } else {
          l = r = voices_[slot].render();
        }
        tl[k] = (k < carried ? tl[k] : 0.0f) + l * g;
        tr[k] = (k < carried ? tr[k] : 0.0f) + r * g;
      }
    }
    tailPos_[slot] = 0;
    tailLen_[slot] = n > carried ? n : carried;
    tailStereo_ = tailStereo_ || detail::VoiceRendersStereo<VoiceType>::value ||
                  detail::VoiceRendersBlock<VoiceType>::value;
  }

  void noteEnded(int i) {
    if (endedCount_ >= kMaxEnded) return; // full: nobody drained; the oldest stay
    ended_[endedCount_].note = notes_[i];
    ended_[endedCount_].channel = channels_[i];
    ended_[endedCount_].noteId = noteIds_[i];
    ++endedCount_;
  }

  /**
   * Which voice a listener will miss least.
   *
   * In order: one already in its release tail; a voice on the same key, since
   * a repeated note replacing itself is inaudible where replacing a different
   * note is not; then the oldest that is neither the lowest nor the highest
   * note sounding. The last rule is the one that matters -- an ear follows
   * the bass and the top line, and taking the oldest voice outright takes the
   * bass of every held chord. Within the polyphony limit.
   */
  int voiceToSteal(int note, int channel) const {
    int oldestReleased = -1, sameKey = -1, oldestInner = -1, oldest = 0;
    int lowest = 0, highest = 0;
    for (int i = 1; i < polyphony_; ++i) {
      if (notes_[i] < notes_[lowest]) lowest = i;
      if (notes_[i] > notes_[highest]) highest = i;
    }
    for (int i = 0; i < polyphony_; ++i) {
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

  // ── Mono and Legato ──────────────────────────────────────────────────────
  // One voice, slot 0, and a stack of the keys that are down. The stack is
  // what makes a trill work: lift the top note of two and the line goes back
  // to the one still held, which a synth that only remembers one key cannot.

  /** One key held in Mono or Legato, as it was struck. */
  struct Held {
    int note;
    float velocity;
    int channel;
    int32_t noteId;
  };

  void forgetHeld(int note, int channel) {
    int w = 0;
    for (int r = 0; r < stackSize_; ++r)
      if (!(held(r).note == note && (channel == kAnyChannel || held(r).channel == channel)))
        stack_[w++] = stack_[r];
    stackSize_ = w;
  }
  const Held& held(int i) const { return stack_[i]; }

  /** The key the line should play: an index into the stack, or -1. */
  int chosenHeld() const {
    if (stackSize_ == 0) return -1;
    int pick = stackSize_ - 1;
    if (priority_ == NotePriority::Low) {
      for (int i = 0; i < stackSize_; ++i)
        if (stack_[i].note < stack_[pick].note) pick = i;
    } else if (priority_ == NotePriority::High) {
      for (int i = 0; i < stackSize_; ++i)
        if (stack_[i].note > stack_[pick].note) pick = i;
    }
    return pick;
  }

  bool monoSounds(int note, int channel) const {
    return voices_[0].isActive() && !released_[0] && notes_[0] == note &&
           (channel == kAnyChannel || channels_[0] == channel);
  }

  void monoNoteOn(int note, float velocity, int channel, int32_t noteId) {
    // Overlapping: another key was still down when this one came. That, and
    // not the timing, is what legato means.
    const bool overlapping = stackSize_ > 0;
    forgetHeld(note, channel);
    if (stackSize_ == kMaxHeldNotes) { // more keys than fingers: forget the oldest
      for (int i = 1; i < stackSize_; ++i) stack_[i - 1] = stack_[i];
      --stackSize_;
    }
    stack_[stackSize_++] = Held{note, velocity, channel, noteId};
    const Held& target = stack_[chosenHeld()];
    // A key that does not win the priority changes nothing audible: a low
    // note added under a High-priority lead is remembered, not played.
    if (monoSounds(target.note, target.channel)) return;
    moveTo(target, overlapping);
  }

  void monoNoteOff(int note, int channel) {
    forgetHeld(note, channel);
    if (!monoSounds(note, channel)) return; // a key that was not the one sounding
    const int next = chosenHeld();
    if (next >= 0) {
      moveTo(stack_[next], true); // back to a key still down
      return;
    }
    held_[0] = false;
    if (sustainDown_ || sostenuto_[0]) pedalHeld_[0] = true;
    else releaseSlot(0);
  }

  /** The one voice to `target`: in Legato, while it is sounding and the
   *  voice can, without a restart; otherwise a fresh start, faded across. */
  void moveTo(const Held& target, bool overlapping) {
    if constexpr (detail::VoiceHasLegato<VoiceType>::value) {
      if (mode_ == Mode::Legato && overlapping && voices_[0].isActive() && !released_[0]) {
        sounding_[0] = false;
        noteEnded(0); // the host's note ended, though the sound carries on
        notes_[0] = target.note;
        channels_[0] = target.channel;
        noteIds_[0] = target.noteId;
        held_[0] = true;
        pedalHeld_[0] = false;
        sounding_[0] = true;
        voices_[0].legatoTo(target.note, target.velocity);
        return;
      }
    }
    steal(0);
    start(0, target.note, target.velocity, target.channel, target.noteId);
  }

  static constexpr int kMaxEnded = 64;

  VoiceType voices_[MaxVoices];
  int channels_[MaxVoices] = {};
  int notes_[MaxVoices] = {0};
  int32_t noteIds_[MaxVoices] = {};
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
  /** Was active at the last render: how a note ending is noticed. */
  bool sounding_[MaxVoices] = {false};
  bool sustainDown_ = false;

  Mode mode_ = Mode::Poly;
  NotePriority priority_ = NotePriority::Last;
  int polyphony_ = MaxVoices;
  bool mpe_ = false;
  Held stack_[kMaxHeldNotes] = {};
  int stackSize_ = 0;

  float bend_[16] = {};
  float bendRange_ = 2.0f;
  float modWheel_ = 0.0f;

  // The steal fades: each slot's last notes, faded, playing out.
  float fade_[kMaxStealFade] = {};
  int fadeLength_ = 96;
  float tailL_[MaxVoices][kMaxStealFade] = {};
  float tailR_[MaxVoices][kMaxStealFade] = {};
  int tailPos_[MaxVoices] = {};
  int tailLen_[MaxVoices] = {};
  bool tailStereo_ = false;

  EndedNote ended_[kMaxEnded];
  int endedCount_ = 0;
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
    case 120: // All Sound Off: now, a voice with kill() silenced outright
      voices.allSoundOff();
      return true;
    case 123: // All Notes Off: released, tails and all
      voices.allNotesOff();
      return true;
    default: return false;
  }
}

/**
 * Everything a voice manager takes from MIDI, in one call: notes (with their
 * channel), the pedals and the panic messages, the pitch wheel at the
 * manager's bend range, channel pressure and the mod wheel. Returns true if
 * the message was one of them.
 *
 * Written once here because each synth used to write it again, and each one
 * got a different part of it wrong: a note-on at velocity 0 that is really a
 * note-off, the wheel's centre at 8192 rather than 8191.5, a channel off by
 * one. Under MPE (setMpe(true)) the wheel and pressure are left alone: the
 * wrapper has already turned them into each note's own expression.
 */
template <typename VoiceManagerType>
inline bool applyMidi(VoiceManagerType& voices, const MidiMessage& m, int32_t noteId = -1) {
  if (applyPedals(voices, m)) return true;
  const int channel = m.getChannel() - 1;
  if (m.isNoteOn()) { // velocity 0 is a note-off, and isNoteOn() says so
    voices.noteOn(m.getNoteNumber(), m.getFloatVelocity(), channel, noteId);
    return true;
  }
  if (m.isNoteOff()) {
    voices.noteOff(m.getNoteNumber(), channel);
    return true;
  }
  if (m.isController() && m.getControllerNumber() == 1) {
    voices.modWheel((float) m.getControllerValue() / 127.0f);
    return true;
  }
  if (voices.isMpe()) return false;
  if (m.isPitchWheel()) {
    const int v = m.getPitchWheelValue();
    // Centred at 8192, and a full throw either way is the range: down to 0 is
    // -8192 steps and up to 16383 is +8191, so each side divides by its own.
    const float t = v >= 8192 ? (float) (v - 8192) / 8191.0f : (float) (v - 8192) / 8192.0f;
    voices.pitchBend(t * voices.pitchBendRange(), channel);
    return true;
  }
  if (m.isChannelPressure()) {
    voices.channelPressure((float) m.getChannelPressureValue() / 127.0f, channel);
    return true;
  }
  return false;
}

/**
 * One note's expression, to the voice playing it: tuning (semitones),
 * pressure and brightness, through the voice's setTuning(), setPressure() and
 * setBrightness() where it has them. Found by the host's note id when there is
 * one, by key and channel otherwise.
 */
template <typename VoiceType>
inline void applyExpressionTo(VoiceType& voice, const NoteExpressionBuffer::Entry& e) {
  switch (e.expression) {
    case kExprTuning:
      if constexpr (detail::VoiceHasTuning<VoiceType>::value) voice.setTuning(e.value);
      break;
    case kExprPressure:
      if constexpr (detail::VoiceHasPressure<VoiceType>::value) voice.setPressure(e.value);
      break;
    case kExprBrightness:
      if constexpr (detail::VoiceHasBrightness<VoiceType>::value) voice.setBrightness(e.value);
      break;
    default:
      break;
  }
  (void) voice;
  (void) e;
}

template <typename VoiceManagerType>
inline void applyExpression(VoiceManagerType& voices, const NoteExpressionBuffer::Entry& e) {
  int v = voices.voiceForNoteId(e.noteId);
  if (v < 0 && e.key >= 0) v = voices.voiceForNote(e.key, e.channel < 0 ? VoiceManagerType::kAnyChannel : e.channel);
  if (v >= 0) {
    applyExpressionTo(voices.voice(v), e);
    return;
  }
  // No key: the expression is for every note (on its channel, if it names one).
  if (e.key >= 0 || e.noteId >= 0) return;
  for (int i = 0; i < VoiceManagerType::kNumVoices; ++i)
    if (voices.voice(i).isActive() && (e.channel < 0 || voices.channelOf(i) == e.channel))
      applyExpressionTo(voices.voice(i), e);
}

/**
 * A note's offset to one parameter (ParamModBuffer), to the voice playing it,
 * through the voice's `setParamMod(int param, float amount)` where it has one.
 * Found as an expression is: by the host's note id, then by key and channel;
 * with no key, every note on the channel, or on every channel.
 *
 * The voice keeps the amount and adds it to the parameter's value for itself
 * alone. A new note starts with none: clear the offsets in noteOn(), and the
 * host sends the new note its own.
 */
template <typename VoiceManagerType>
inline void applyParamMod(VoiceManagerType& voices, const ParamModBuffer::Entry& e) {
  using VoiceType = std::remove_reference_t<decltype(voices.voice(0))>;
  if constexpr (detail::VoiceHasParamMod<VoiceType>::value) {
    int v = voices.voiceForNoteId(e.noteId);
    if (v < 0 && e.key >= 0)
      v = voices.voiceForNote(e.key, e.channel < 0 ? VoiceManagerType::kAnyChannel : e.channel);
    if (v >= 0) {
      voices.voice(v).setParamMod((int) e.param, e.amount);
      return;
    }
    if (e.key >= 0 || e.noteId >= 0) return;
    for (int i = 0; i < VoiceManagerType::kNumVoices; ++i)
      if (voices.voice(i).isActive() && (e.channel < 0 || voices.channelOf(i) == e.channel))
        voices.voice(i).setParamMod((int) e.param, e.amount);
  } else {
    (void) voices;
    (void) e;
  }
}

/**
 * The notes the voice manager has seen end, into `out` at frame
 * `samplePosition`: what a wrapper tells a host that wants to know (CLAP's
 * NOTE_END). renderVoices() does this itself; a DSP that renders its voices
 * by hand calls it once per block with `*ctx.endedNotes`, when that is not
 * null.
 */
template <typename VoiceManagerType>
inline void reportEndedNotes(VoiceManagerType& voices, NoteIdBuffer& out, int samplePosition) {
  voices.drainEndedNotes([&](const typename VoiceManagerType::EndedNote& e) {
    out.add(samplePosition, e.note, e.channel, e.noteId);
  });
}

/**
 * A whole instrument block: the block's MIDI, note expression and per-note
 * modulation applied at their exact frames, and the voices rendered between
 * them into the main bus (stereo voices in stereo, mono voices in the middle;
 * a mono bus gets the fold). Overwrites the bus: an instrument's output
 * starts from nothing.
 *
 * Sample-accurate, which is what keeps a synth tight: a note at frame 37
 * starts at frame 37, not at the top of the block. The voices are rendered a
 * stretch at a time -- the frames between one event and the next, at most
 * 128 -- through VoiceManager::renderBlock(), which a voice can take over.
 *
 * The host's ids for the block's notes go to the voice manager with their
 * notes (ctx.noteIds), and the notes that end are reported (ctx.endedNotes):
 * a note a steal cut short at the frame of the steal, one whose release ran
 * out at the end of the stretch it ran out in.
 */
template <typename VoiceManagerType>
inline void renderVoices(VoiceManagerType& voices, ProcessContext& ctx) {
  AudioBlock<float>& io = ctx.main;
  const size_t n = io.getNumSamples();
  const size_t channels = io.getNumChannels();
  if (channels == 0) return;
  float* L = io.getChannelPointer(0);
  float* R = channels > 1 ? io.getChannelPointer(1) : nullptr;
  auto event = ctx.midi.begin();
  const auto eventEnd = ctx.midi.end();
  const NoteExpressionBuffer::Entry* expr = ctx.expression ? ctx.expression->begin() : nullptr;
  const NoteExpressionBuffer::Entry* exprEnd = ctx.expression ? ctx.expression->end() : nullptr;
  const ParamModBuffer::Entry* mod = ctx.paramMods ? ctx.paramMods->begin() : nullptr;
  const ParamModBuffer::Entry* modEnd = ctx.paramMods ? ctx.paramMods->end() : nullptr;
  constexpr size_t kStretch = 128;
  float bufL[kStretch], bufR[kStretch];
  for (size_t i = 0; i < n;) {
    // Everything due by this frame.
    while (event != eventEnd && (size_t) event->samplePosition <= i) {
      const MidiMessage& m = event->getMessage();
      // The host's name for the note goes with it, so its expression, its
      // modulation and its end can all be addressed by that name.
      const int32_t id =
          ctx.noteIds && m.isNoteOn() ? ctx.noteIds->take(m.getNoteNumber(), m.getChannel() - 1) : -1;
      applyMidi(voices, m, id);
      ++event;
    }
    while (expr != exprEnd && (size_t) expr->samplePosition <= i) {
      applyExpression(voices, *expr);
      ++expr;
    }
    while (mod != modEnd && (size_t) mod->samplePosition <= i) {
      applyParamMod(voices, *mod);
      ++mod;
    }
    // A steal ends its note here, at the frame of the note that stole it.
    if (ctx.endedNotes && voices.hasEndedNotes()) reportEndedNotes(voices, *ctx.endedNotes, (int) i);

    // The stretch: up to the next event, the scratch's length, or the end.
    size_t end = i + kStretch < n ? i + kStretch : n;
    if (event != eventEnd && (size_t) event->samplePosition < end) end = (size_t) event->samplePosition;
    if (expr != exprEnd && (size_t) expr->samplePosition < end) end = (size_t) expr->samplePosition;
    if (mod != modEnd && (size_t) mod->samplePosition < end) end = (size_t) mod->samplePosition;
    const size_t len = end - i;
    for (size_t k = 0; k < len; ++k) bufL[k] = bufR[k] = 0.0f;
    voices.renderBlock(bufL, bufR, (int) len);
    if (R) {
      for (size_t k = 0; k < len; ++k) {
        L[i + k] = bufL[k];
        R[i + k] = bufR[k];
      }
    } else {
      for (size_t k = 0; k < len; ++k) L[i + k] = 0.5f * (bufL[k] + bufR[k]);
    }
    // A release that ran out in the stretch: over by its end.
    if (ctx.endedNotes && voices.hasEndedNotes())
      reportEndedNotes(voices, *ctx.endedNotes, (int) (end < n ? end : n - 1));
    i = end;
  }
  // Any further channels of a wide bus get silence rather than whatever the
  // host left in them.
  for (size_t c = 2; c < channels; ++c) {
    float* out = io.getChannelPointer(c);
    for (size_t k = 0; k < n; ++k) out[k] = 0.0f;
  }
}

} // namespace sonore

// Composite effects, musical time and the frequency domain build on the
// primitives above, and are included here so `#include <sonore/dsp.h>` stays
// the single entry point a generated plugin needs.
// Rate conversion first: effects.h uses the oversampler.
// The toolkit headers below lean on everything above and are only ever
// included from here; each says so if it is included on its own.
#define SONORE_DSP_H_INCLUDED 1
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
