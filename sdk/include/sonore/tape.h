// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: tape, modelled rather than named.
//
// Every "tape saturation" this pipeline shipped was a tanh with the word tape
// on the faceplate. What makes tape sound like tape is not a curve, it is
// HYSTERESIS: the magnetisation the tape keeps depends on where it has BEEN,
// so the output on the way up through a value differs from the output on the
// way down, and the loop between them is where the compression, the
// softening of transients and the harmonic signature all come from. Then the
// playback head loses highs in a way that depends on speed, and the transport
// wobbles.
//
// The hysteresis is the Jiles-Atherton model of ferromagnetism, integrated
// per sample the way Jatin Chowdhury does it in "Real-time physical modelling
// for analog tape machines" (DAFx 2019): fourth-order Runge-Kutta over each
// step at twice the rate. The model is dimensionless here -- saturation
// magnetisation Ms = 1 and the anhysteretic scale a = 1, so the paper's
// physical constants become alpha = 1.6e-3 * Ms/a = 0.0255 and k = 27/22 =
// 1.23 -- because the physical units (A/m, Tesla) buy nothing a plugin can
// use and cost every parameter a unit conversion. Three things the paper's
// equations do not say and this file had to learn:
//
//   * The Langevin derivative 1/x^2 - 1/sinh^2(x) cancels catastrophically in
//     float below |x| ~ 0.05: both terms are ~10^3 and the answer is 1/3. The
//     series is used below 0.1, where its truncation error is 1e-9.
//   * The irreversible term's denominator (1-c)*delta*k - alpha*(Man - M) has
//     a ZERO at Man - M = (1-c)k/alpha, and a fast swing reaches it whenever
//     (1-c)k is not comfortably larger than alpha. The model is well-posed
//     only inside that region, so setHysteresis() is clamped to keep it
//     there: a narrow loop cannot also be a nearly-reversible one, and the
//     floor on the hysteresis rises as the width falls.
//   * RK4 over one step assumes the field moves a little per step. A hot
//     10 kHz signal at +36 dB moves it by a hundred knee-widths per sample,
//     so a large step is SUBDIVIDED (up to 8 sub-steps) rather than trusting
//     the clamp on M to catch the overshoot.
//
// The loop is normalised to UNITY small-signal gain -- the initial
// susceptibility c/3 / (1 - c*alpha/3) is known in closed form -- so quiet
// material passes at the level it came in and the compression starts at the
// knee. That holds while most of the magnetisation is reversible. As the
// hysteresis control takes the reversible share c towards 0, the initial
// susceptibility goes with it, but a full-scale swing does not: the
// irreversible part carries it, and it magnetises the loop about as far at
// every setting (0.12 to 0.26 per unit field). Normalised by the small-signal
// slope alone, hysteresis 1 came out +34 dB at full scale. So the slope the
// gain is taken from is never below 0.2 (kMinSusceptibility): past
// hysteresis 0.4 at the default width, a full-scale swing stays near unity
// and quiet material comes out quieter -- a mostly irreversible material
// barely answers a small field, which is the physics and why tape is biased.
// The saturation ceiling is 1/(slope * 2.5): about +4.7 dB above unity at
// the default reversibility, +6 dB at most. TapeSaturator divides the drive
// back out after the loop, so its drive knob changes how far into the loop
// the signal goes and not how loud it comes out; `makeupDb` is the volume.
//
// Playback loss is the head model from the same paper (Kadis's spacing,
// thickness and gap losses: e^{-kd}, (1 - e^{-k delta})/(k delta) and
// sin(kg/2)/(kg/2) with k = 2 pi f / v). The whole curve slides along the
// frequency axis with the tape speed -- which is why 7.5 ips is dull by
// 10 kHz and 30 ips is not -- and that scaling law, H(f, v) = H(f/2, v/2),
// is what the unit test asserts. It is realised as a 65-tap linear-phase FIR
// designed from the magnitude by inverse Fourier sum at prepare (and on a
// speed change): 32 samples of latency up to 48 kHz, and above it the same
// span in time (about 0.67 ms, 257 taps at most). The default head
// geometry is CALIBRATED, not measured: real machines run record and
// playback EQ that this model does not carry, so the defaults are the ones
// that give a pro 15 ips machine's net response (-3 dB at 10 kHz, -6 dB at
// 20 kHz). The physics gives the shape and the speed law; the numbers give
// the level.
//
// Wow is a slow sine on the transport, flutter a faster shimmer -- both are
// the tape's read point moving, so they are a modulated delay with the cubic
// tap around a 4 ms nominal position, which is reported as latency.
//
// Included by dsp.h. Uses Oversampled<> from lofi.h and PinkNoise.
#pragma once
#ifndef SONORE_DSP_H_INCLUDED
#error "tape.h is part of dsp.h and needs what it includes first: include <sonore/dsp.h> instead."
#endif
#include <cmath>
#include "audio.h"
#include "random.h"
#include "simd.h"

namespace sonore {

/**
 * The hysteresis loop on its own: a per-sample shaper with memory, meant to
 * run inside Oversampled<> (its state advances at the oversampled rate,
 * which is the rate RK4 wants). TapeSaturator does that for you.
 *
 * The output is the magnetisation, and the magnetisation is what the loop
 * REMEMBERS: when a loud signal stops, it stays where the loop left it, a
 * standing DC offset that grows with the hysteresis (about 0.1 at 0.3 after
 * a full-scale tone, 0.5 at 1). TapeSaturator removes it after the loop;
 * used on its own, follow it with a DC blocker.
 */
class JilesAtherton {
public:
  /** How the loop is integrated. RK4 is the paper's baseline; NewtonRaphson
   *  is the implicit trapezoidal rule solved by four Newton steps per sample
   *  (Chowdhury's NR4, from his 2020 comparison of the two), which holds its
   *  accuracy at larger field steps and is what the plugin offers as its
   *  "accurate" mode. Both get the sub-stepping. */
  enum class Solver {
    RK4,          ///< explicit fourth-order Runge-Kutta, the default
    NewtonRaphson ///< implicit trapezoidal rule, four Newton steps per sample
  };

  /** The paper's loop at unity drive: width 0.55 (k = 1.235), hysteresis
   *  0.3 (c = 0.7), RK4, no glide, nothing magnetised. */
  JilesAtherton() { cook(); }

  /** Switch the integrator. Takes effect on the next process() and keeps the
   *  loop's state, so it can change mid-stream. */
  void setSolver(Solver s) { solver_ = s; }
  /** The integrator setSolver() installed. */
  Solver solver() const { return solver_; }

  /** How far a full-scale input pushes into the loop. 1 = the knee. Glides
   *  with the loop's shape when setGlide() is on. */
  void setDrive(float linear) {
    driveWant_ = linear < 0.0f ? 0.0f : linear;
    if (glide_ <= 0.0f) drive_ = driveWant_;
  }
  /** Coercivity k, in units of the anhysteretic scale: how WIDE the loop is.
   *  0..1 maps onto 0.3..2.0; 0.55 is the paper's 27/22 = 1.23. */
  void setWidth(float w) {
    kWant_ = 0.3f + clampf(w, 0.0f, 1.0f) * 1.7f;
    if (glide_ <= 0.0f) {
      kNow_ = kWant_;
      cook();
    }
  }
  /** How much of the magnetisation is irreversible, 0..1 (c = 1 - h). At 0
   *  the loop collapses onto the anhysteretic curve, a soft saturator with no
   *  memory; at 1 nothing is reversible. Clamped for well-posedness -- see the
   *  header -- so reversibility() reports what was actually installed. */
  void setHysteresis(float h) {
    hWant_ = clampf(h, 0.0f, 1.0f);
    if (glide_ <= 0.0f) {
      hNow_ = hWant_;
      cook();
    }
  }
  /** Let width and hysteresis GLIDE to a new setting over about `samples`
   *  calls of process(), instead of landing at once; 0 (the default) lands
   *  at once. The output gain is 1/(initial susceptibility), which moves
   *  with the hysteresis -- landed at once, it met the magnetisation the old
   *  loop had left, and the next few milliseconds came out at the wrong
   *  level. TapeSaturator sets 20 ms. */
  void setGlide(float samples) {
    glide_ = samples > 1.0f ? 1.0f - std::exp(-1.0f / samples) : 0.0f;
    if (glide_ <= 0.0f) {
      kNow_ = kWant_;
      hNow_ = hWant_;
      drive_ = driveWant_;
      cook();
    }
  }
  /** Land any glide in progress: reset() means a fresh start, not the tail
   *  end of a knob movement. */
  void reset() {
    m_ = 0.0f;
    h1_ = 0.0f;
    drive_ = driveWant_;
    if (kNow_ != kWant_ || hNow_ != hWant_) {
      kNow_ = kWant_;
      hNow_ = hWant_;
      cook();
    }
  }

  /** The coercivity k now in use, 0.3..2.0 in units of the anhysteretic
   *  scale: setWidth()'s value, or the point a glide towards it has reached. */
  float coercivity() const { return k_; }
  /** The reversible share c now in use, 1 - hysteresis after the
   *  well-posedness clamp: between 0.01 and 1 - 4 alpha / k, so a narrow loop
   *  reports less than it was asked for. */
  float reversibility() const { return c_; }
  /** The gain the loop output is scaled by, so a small signal comes out at
   *  unity while most of the magnetisation is reversible (see the header for
   *  past that). The saturation ceiling is this times Ms = 1: 2 at most. */
  float normalisation() const { return norm_; }

  /** One sample through the loop: the input times the drive becomes the
   *  field, the output is the magnetisation times normalisation(), within
   *  +-1.2 times that gain. Advances any glide by one step. Call it at the
   *  oversampled rate (see the class comment). No allocation; a field step
   *  too large for one RK4 step is split into up to 8, and the state a NaN
   *  or infinite input leaves is flushed to zero rather than kept. */
  inline float process(float x) {
    if (drive_ != driveWant_) {
      drive_ += (driveWant_ - drive_) * glide_;
      if (std::fabs(driveWant_ - drive_) < 1e-5f * (1.0f + driveWant_)) drive_ = driveWant_;
    }
    if (kNow_ != kWant_ || hNow_ != hWant_) {
      kNow_ += (kWant_ - kNow_) * glide_;
      hNow_ += (hWant_ - hNow_) * glide_;
      // Snapped when close: a one-pole never quite arrives, and cook() per
      // sample for ever after would be paying for a movement that is over.
      if (std::fabs(kWant_ - kNow_) < 1e-4f) kNow_ = kWant_;
      if (std::fabs(hWant_ - hNow_) < 1e-4f) hNow_ = hWant_;
      cook();
    }
    // Field from the input, in units where the knee sits at |H| ~ 2.5.
    const float h = x * drive_ * kFieldScale;
    const float dh = h - h1_;
    const float adh = std::fabs(dh);
    int steps = 1;
    if (adh > kMaxStep) {
      // Capped BEFORE the cast: an infinite input is an infinite step, and an
      // infinite float cast to int is undefined. The state it leaves is
      // flushed below, so the next finite sample starts the loop again.
      steps = adh < kMaxStep * (float) kMaxSubSteps ? (int) (adh / kMaxStep) + 1 : kMaxSubSteps;
      if (steps > kMaxSubSteps) steps = kMaxSubSteps;
    }
    const float sub = dh / (float) steps;
    for (int s = 0; s < steps; ++s) {
      if (solver_ == Solver::RK4) {
        // RK4 on M along the field step, with H linear across it. Each stage
        // evaluates dM/dH, so the increment is dH/6 (g1 + 2 g2 + 2 g3 + g4).
        const float g1 = slope(m_, h1_, sub);
        const float g2 = slope(m_ + 0.5f * sub * g1, h1_ + 0.5f * sub, sub);
        const float g3 = slope(m_ + 0.5f * sub * g2, h1_ + 0.5f * sub, sub);
        const float g4 = slope(m_ + sub * g3, h1_ + sub, sub);
        m_ += sub * (1.0f / 6.0f) * (g1 + 2.0f * g2 + 2.0f * g3 + g4);
      } else {
        // Trapezoidal: M1 = M0 + (dH/2)(s(M0, H0) + s(M1, H1)), Newton on M1
        // from the explicit predictor, with the analytic dM'/dM.
        const float s0 = slope(m_, h1_, sub);
        const float hNext = h1_ + sub;
        float m1 = m_ + sub * s0;
        for (int it = 0; it < kNewtonIterations; ++it) {
          float s1, ds1;
          slopeAndDerivative(m1, hNext, sub, s1, ds1);
          const float g = m1 - m_ - 0.5f * sub * (s0 + s1);
          const float dg = 1.0f - 0.5f * sub * ds1;
          if (std::fabs(dg) < 1e-6f) break;
          m1 -= g / dg;
        }
        m_ = m1;
      }
      m_ = clampf(flushDenormal(m_), -1.2f, 1.2f);
      h1_ += sub;
    }
    h1_ = h;
    return m_ * norm_;
  }

private:
  static constexpr float kFieldScale = 2.5f;  // full scale lands at the knee of L(x)
  static constexpr float kAlpha = 0.0255f;    // mean-field coupling, the paper's 1.6e-3 * Ms/a
  static constexpr float kMaxStep = 0.5f;     // largest field move one RK4 step is trusted with
  static constexpr int kMaxSubSteps = 8;
  static constexpr int kNewtonIterations = 4; // Chowdhury's NR4
  // The least slope the gain is taken from: see the header. Below the default
  // hysteresis's 0.235, so the default loop is normalised as it always was.
  static constexpr float kMinSusceptibility = 0.2f;

  void cook() {
    k_ = kNow_;
    // The denominator's zero sits at Man - M = (1-c)k/alpha; a swing can
    // reach |Man - M| ~ 2, so (1-c)k is kept at least four alphas clear.
    const float cMax = 1.0f - 4.0f * kAlpha / k_;
    float c = 1.0f - hNow_;
    if (c > cMax) c = cMax;
    if (c < 0.01f) c = 0.01f;
    c_ = c;
    // Initial susceptibility: at M = 0 the irreversible term vanishes with
    // H, leaving c L'(0) / (1 - c alpha L'(0)) with L'(0) = 1/3.
    const float chi0 = (c_ / 3.0f) / (1.0f - c_ * kAlpha / 3.0f);
    norm_ = 1.0f / ((chi0 > kMinSusceptibility ? chi0 : kMinSusceptibility) * kFieldScale);
  }

  /** Langevin function and its derivative, the anhysteretic curve. Series
   *  near zero (the closed form cancels in float), asymptote past 20 (sinh
   *  overflows float past 89, and coth is 1 to float precision long before). */
  static inline void langevin(float x, float& l, float& dl) {
    const float ax = std::fabs(x);
    if (ax < 0.1f) {
      const float x2 = x * x;
      l = x * (1.0f / 3.0f - x2 * (1.0f / 45.0f) + x2 * x2 * (2.0f / 945.0f));
      dl = 1.0f / 3.0f - x2 * (1.0f / 15.0f) + x2 * x2 * (2.0f / 189.0f);
      return;
    }
    if (ax > 20.0f) {
      l = (x > 0.0f ? 1.0f : -1.0f) - 1.0f / x;
      dl = 1.0f / (x * x);
      return;
    }
    const float s = std::sinh(x);
    l = 1.0f / std::tanh(x) - 1.0f / x;
    dl = 1.0f / (x * x) - 1.0f / (s * s);
  }
  /** ...and its second derivative, which the implicit solver's Jacobian
   *  needs: L'' = -2/x^3 + 2 cosh(x)/sinh^3(x), series -2x/15 + 8x^3/189. */
  static inline float langevinSecond(float x) {
    const float ax = std::fabs(x);
    if (ax < 0.1f) return x * (-2.0f / 15.0f + x * x * (8.0f / 189.0f));
    if (ax > 20.0f) return -2.0f / (x * x * x);
    const float s = std::sinh(x);
    return -2.0f / (x * x * x) + 2.0f * std::cosh(x) / (s * s * s);
  }

  /** dM/dH and its derivative with respect to M, for Newton. The chain:
   *  Q = H + alpha M, D = L(Q) - M; irreversible term A D / (B - alpha D)
   *  with A = (1-c) deltaM and B = (1-c) delta k; N = irr + c L'(Q);
   *  den = 1 - c alpha L'(Q); slope = N / den. */
  inline void slopeAndDerivative(float m, float h, float dh, float& s, float& ds) const {
    float man, dman;
    const float q = h + kAlpha * m;
    langevin(q, man, dman);
    const float d2man = langevinSecond(q);
    const float delta = dh >= 0.0f ? 1.0f : -1.0f;
    const float diff = man - m;
    const float deltaM = (diff >= 0.0f) == (delta >= 0.0f) ? 1.0f : 0.0f;
    const float A = (1.0f - c_) * deltaM, B = (1.0f - c_) * delta * k_;
    float denom = B - kAlpha * diff;
    if (std::fabs(denom) < kAlpha) denom = denom < 0.0f ? -kAlpha : kAlpha;
    const float irr = A * diff / denom;
    const float num = irr + c_ * dman;
    const float den = 1.0f - c_ * kAlpha * dman;
    s = num / den;
    const float dDiff = kAlpha * dman - 1.0f;
    const float dIrr = A * B / (denom * denom) * dDiff;
    const float dNum = dIrr + c_ * kAlpha * d2man;
    const float dDen = -c_ * kAlpha * kAlpha * d2man;
    ds = (dNum * den - num * dDen) / (den * den);
  }

  /** dM/dH at a state (Jiles-Atherton). delta is the direction of H, and
   *  deltaM zeroes the irreversible term when it would make the
   *  susceptibility negative -- the standard correction to the original
   *  equations. */
  inline float slope(float m, float h, float dh) const {
    float man, dman;
    langevin(h + kAlpha * m, man, dman);
    const float delta = dh >= 0.0f ? 1.0f : -1.0f;
    const float diff = man - m;
    const float deltaM = (diff >= 0.0f) == (delta >= 0.0f) ? 1.0f : 0.0f;
    float denom = (1.0f - c_) * delta * k_ - kAlpha * diff;
    // cook() keeps this clear of zero for any ordinary swing; the floor is
    // for the clamp-at-1.2 case, so nothing can divide by nothing.
    if (std::fabs(denom) < kAlpha) denom = denom < 0.0f ? -kAlpha : kAlpha;
    const float irreversible = (1.0f - c_) * deltaM * diff / denom;
    const float num = irreversible + c_ * dman;
    const float den = 1.0f - c_ * kAlpha * dman;
    return num / den;
  }

  Solver solver_ = Solver::RK4;
  float drive_ = 1.0f, driveWant_ = 1.0f, kWant_ = 1.235f, hWant_ = 0.3f, kNow_ = 1.235f, hNow_ = 0.3f;
  float glide_ = 0.0f;
  float k_ = 1.235f, c_ = 0.7f, norm_ = 1.7f, m_ = 0.0f, h1_ = 0.0f;
};

/**
 * The record and playback equalisation standards. A tape machine does not
 * record the signal flat: the record amplifier pre-emphasises the top (and,
 * under NAB, cuts the bottom) and the playback amplifier undoes it, so the
 * chain is flat and the TAPE sees the emphasised signal. That is why a slow
 * machine softens the top when driven -- at 7.5 ips the top is 10 dB hotter
 * on the tape than in the signal -- and why IEC at 15 ips has more high-
 * frequency headroom than NAB: less pre-emphasis. The time constants are the
 * standards' own (NAB: 3180 + 50 us at 7.5 and 15 ips; IEC/CCIR: 70 us at
 * 7.5, 35 us at 15, 17.5 us at 30, no low-frequency term above 7.5; AES is
 * the 30 ips 17.5 us), realised as exact inverse pairs around the loop.
 */
enum class TapeEq {
  Off, ///< no equalisation: the loop sees the signal flat
  NAB, ///< 3180 us low cut with 50 us emphasis (90 us at 3.75 ips, 17.5 us at 30)
  IEC, ///< 120 us at 3.75 ips, 70 at 7.5, 35 at 15, 17.5 at 30; the 3180 us low cut at 7.5 and below
  AES  ///< 17.5 us at any speed, no low-frequency term
};

/**
 * The whole machine, one channel: record equalisation into the hysteresis at
 * 2x, the head's playback loss for the chosen speed, playback equalisation,
 * wow and flutter, makeup. Two of these for stereo -- they hold state and the
 * transports should not be shared. Rates up to 192 kHz: the transport ring
 * is sized for 4 ms + the deepest wobble at that rate.
 */
class TapeSaturator {
public:
  /** The machine's settings, set together with setParameters(), which
   *  clamps them: drive -24..36 dB, undone on the way out so it changes the
   *  saturation and not the level; the hysteresis loop's width and its
   *  irreversible share 0..1; tape speed 3.75..30 ips; the head's spacing,
   *  gap and recorded depth in microns, up to 50; wow and flutter 0..1;
   *  makeup -24..24 dB; the solver and the EQ standard. The head filter and
   *  the equalisers are redesigned only when the speed, the head or the
   *  standard change, so passing these every block is cheap. Before the
   *  first process() a call lands at once rather than gliding. */
  struct Parameters {
    float driveDb = 0.0f;       ///< into the loop, -24..36 dB; 0 dB = full scale at the knee
    float width = 0.55f;        ///< hysteresis loop width 0..1 (0.55 = the paper's k)
    float hysteresis = 0.3f;    ///< irreversible share 0..1 (0.3 = c of 0.7)
    float speedIps = 15.0f;     ///< 3.75 .. 30: slides the head loss along frequency
    float spacingUm = 1.0f;     ///< head-to-tape spacing, microns, 0..50
    float gapUm = 2.5f;         ///< playback head gap, microns, 0.1..50
    float thicknessUm = 2.0f;   ///< effective recorded depth, microns, 0.1..50
    float wow = 0.0f;           ///< 0..1, slow pitch drift at 0.7 Hz (up to 2 ms)
    float flutter = 0.0f;       ///< 0..1, fast shimmer at 8.3 and 14.7 Hz plus noise (up to 0.3 ms)
    float makeupDb = 0.0f;      ///< output gain, -24..24 dB: the machine's volume control
    bool accurate = false;      ///< the implicit Newton-Raphson solver instead of RK4
    TapeEq eq = TapeEq::NAB;    ///< record/playback equalisation around the loop
  };

  /** Prepare for a sample rate in Hz (1 or less means 48000): sets the
   *  transport delay to 4 ms in whole samples and the 20 ms glides,
   *  redesigns the head FIR and the equalisers for the current parameters,
   *  and reset()s. Call it before the first process() and again on a rate
   *  change: a machine that has had neither this nor setParameters() has no
   *  head-filter taps yet and plays silence. No allocation, but the designs
   *  cost thousands of transcendentals: call it from prepare, not per block. */
  void setSampleRate(float sr) {
    sr_ = sr > 1.0f ? sr : 48000.0f;
    // 4 ms, rounded to a whole sample so the unmodulated read is exact and
    // the reported latency is the real one.
    nominal_ = (float) (int) (0.004f * sr_ + 0.5f);
    if (nominal_ < 8.0f) nominal_ = 8.0f;
    // The flutter's noise component: pink noise smoothed to ~15 Hz.
    noiseCoef_ = 1.0f - std::exp(-2.0f * kPi * 15.0f / sr_);
    // The loop runs at twice the rate; 20 ms there. The output gain and the
    // wobble depths glide over the same 20 ms out here: drive is undone on
    // the way out, so the two halves must move together, and a read point
    // that jumps is a click -- wow 0 -> 0.5 stepped the output 35 times its
    // largest natural step, makeup +6 dB twice.
    loop_.shaper().setGlide(0.02f * 2.0f * sr_);
    gainSmooth_.setup(sr_, 20.0f);
    wowSmooth_.setup(sr_, 20.0f);
    flutterSmooth_.setup(sr_, 20.0f);
    dcR_ = 1.0f - 2.0f * kPi * kDcCornerHz / sr_;
    setParameters(params_);
    reset();
  }
  /** Clear the machine: the loop's magnetisation, the oversampler, head and
   *  equaliser histories, the transport and the wobble phases. Glides land
   *  on their targets, and the next setParameters() lands at once again
   *  until process() runs. No allocation. */
  void reset() {
    loop_.reset();
    loop_.shaper().reset();
    loss_.reset();
    eq_.reset();
    transport_.reset();
    wowPhase_ = 0.0f;
    flutterPhase_[0] = flutterPhase_[1] = 0.0f;
    noiseZ_ = 0.0f;
    dcX1_ = dcY1_ = 0.0f;
    gainSmooth_.snap(driveComp_ * makeup_);
    wowSmooth_.snap(wowDepth_);
    flutterSmooth_.snap(flutterDepth_);
    running_ = false;
  }

  /** Install a set of Parameters, clamped to the ranges listed there. Once
   *  process() has run, drive, width, hysteresis, makeup, wow and flutter
   *  glide over 20 ms; before it, the call lands at once (it resets). The
   *  head FIR and equalisers are redesigned only when the speed, the head
   *  geometry or the standard changed -- a CPU spike on that block, no
   *  allocation -- so calling this every block with the same values is
   *  cheap. */
  void setParameters(const Parameters& p) {
    params_ = p;
    // Drive is compensated on the way out: turning it up pushes further into
    // the loop without getting louder, so the knob is "how saturated", not a
    // volume control wearing a costume. Past the knee the output falls (all
    // of it is saturation), which is what makeupDb is for.
    const float drive = dbToGain(clampf(p.driveDb, -24.0f, 36.0f));
    loop_.shaper().setDrive(drive);
    driveComp_ = 1.0f / drive;
    loop_.shaper().setWidth(p.width);
    loop_.shaper().setHysteresis(p.hysteresis);
    loop_.shaper().setSolver(p.accurate ? JilesAtherton::Solver::NewtonRaphson : JilesAtherton::Solver::RK4);
    // The head FIR and the equalisers move only with the rate, the speed, the
    // head geometry and the EQ standard. setParameters is what a plugin calls
    // every block, and redesigning the FIR there -- 65 taps against a
    // 128-point magnitude grid, tens of thousands of transcendentals -- made
    // every drive or wow knob cost a CPU spike per block.
    const float ips = clampf(p.speedIps, 3.75f, 30.0f);
    const float spacing = clampf(p.spacingUm, 0.0f, 50.0f) * 1e-6f;
    const float gap = clampf(p.gapUm, 0.1f, 50.0f) * 1e-6f;
    const float thickness = clampf(p.thicknessUm, 0.1f, 50.0f) * 1e-6f;
    if (sr_ != designedSr_ || ips != designedIps_ || spacing != designedSpacing_ ||
        gap != designedGap_ || thickness != designedThickness_) {
      loss_.design(sr_, ips * 0.0254f, spacing, gap, thickness);
      designedSpacing_ = spacing;
      designedGap_ = gap;
      designedThickness_ = thickness;
    }
    if (sr_ != designedSr_ || ips != designedIps_ || p.eq != designedEq_) {
      eq_.design(sr_, p.eq, ips);
      designedEq_ = p.eq;
    }
    designedSr_ = sr_;
    designedIps_ = ips;
    wowDepth_ = clampf(p.wow, 0.0f, 1.0f) * 0.002f * sr_;
    flutterDepth_ = clampf(p.flutter, 0.0f, 1.0f) * 0.0003f * sr_;
    makeup_ = dbToGain(clampf(p.makeupDb, -24.0f, 24.0f));
    // Until the first process() these are settings, and they land: set after
    // setSampleRate() -- the plugin flow -- +18 dB of drive glided in from
    // 0 dB, the output 4 to 6 dB hot for its first 40 ms. Nothing has run,
    // so landing is a reset.
    if (!running_) reset();
  }

  /** One sample through the machine, at the rate setSampleRate() was given:
   *  record EQ, the loop at 2x, the head loss, playback EQ, a 5 Hz DC
   *  blocker, wow and flutter, drive compensation and makeup. The output is
   *  late by latencySamples(). No allocation and no lock. */
  inline float process(float x) {
    running_ = true;
    // Record equalisation, then the loop at 2x -- its RK4 wants the field to
    // move a little per step -- then the head, then playback equalisation.
    float y = loop_.process(eq_.record(x));
    y = eq_.playback(loss_.process(y));
    // No DC off the tape. The loop keeps its remanence when the signal stops
    // -- that is what hysteresis is -- and a playback head reads the flux
    // CHANGING, never a standing magnetisation. Without this a tone cut off
    // mid-cycle left the loop magnetised, NAB playback multiplied that by
    // its +20 dB at DC, and the output sat at -0.92 for good. 5 Hz: 0.07 dB
    // down at 40 Hz, and a standing offset gone in a few hundred ms.
    {
      const float blocked = y - dcX1_ + dcR_ * dcY1_;
      dcX1_ = y;
      dcY1_ = flushDenormal(blocked);
      y = blocked;
    }
    // Wow and flutter are the read point moving. Flutter is two
    // incommensurate sines plus a little smoothed noise, because a single
    // sine is vibrato, not flutter. The noise's 0.7 keeps the 48 kHz depth
    // it had when PinkNoise was white down here (2.0 then); being pink now,
    // it no longer fades as the rate rises -- it lost 5.8 dB by 192 kHz.
    wowPhase_ += 0.7f / sr_;
    if (wowPhase_ >= 1.0f) wowPhase_ -= 1.0f;
    flutterPhase_[0] += 8.3f / sr_;
    flutterPhase_[1] += 14.7f / sr_;
    if (flutterPhase_[0] >= 1.0f) flutterPhase_[0] -= 1.0f;
    if (flutterPhase_[1] >= 1.0f) flutterPhase_[1] -= 1.0f;
    noiseZ_ += (flutterNoise_.next() - noiseZ_) * noiseCoef_;
    const float wowDepth = wowSmooth_.next(wowDepth_), flutterDepth = flutterSmooth_.next(flutterDepth_);
    const float wow = wowDepth * fastmath::sinTurns(wowPhase_);
    const float flutter = flutterDepth * (0.5f * fastmath::sinTurns(flutterPhase_[0]) +
                                          0.3f * fastmath::sinTurns(flutterPhase_[1]) + 0.7f * noiseZ_);
    // Read BEFORE writing: DelayLine's read(d) after a write is d - 1, and
    // the host is told the nominal figure. The cross-correlation in the test
    // measured 239 against a reported 240 the other way round.
    const float out = (wowDepth > 0.0f || flutterDepth > 0.0f)
                          ? transport_.readCubic(nominal_ + wow + flutter)
                          : transport_.read(nominal_);
    transport_.write(y);
    return out * gainSmooth_.next(driveComp_ * makeup_);
  }

  /** The transport delay, the oversampler and the loss FIR, which the host
   *  must be told about. */
  int latencySamples() const { return (int) nominal_ + loop_.latencySamples() + loss_.latencySamples(); }

  /** The head-loss target the FIR was designed to, in dB at `hz`: the
   *  closed-form curve, for checking the filter against the physics. */
  float headLossDb(float hz) const { return loss_.targetDb(hz); }
  /** The record equalisation's gain at `hz`, in dB: what the TAPE sees over
   *  what came in. Zero with the equalisation off. */
  float recordEmphasisDb(float hz) const { return eq_.recordDb(hz); }
  /** The same, as this rate's digital sections actually realise it. */
  float recordEmphasisBuiltDb(float hz) const { return eq_.recordBuiltDb(hz); }

  /** The hysteresis loop inside the oversampler, for reading its state
   *  (coercivity(), reversibility(), normalisation()). Drive, width,
   *  hysteresis or solver set through it are overwritten by the next
   *  setParameters(), the glide by the next setSampleRate(). */
  JilesAtherton& loop() { return loop_.shaper(); }
  /** As above, read-only. */
  const JilesAtherton& loop() const { return loop_.shaper(); }

private:
  static constexpr int kTransportSamples = 2048; // 4 ms + 2.3 ms of wobble at 192 kHz
  static constexpr float kDcCornerHz = 5.0f;

  /** Spacing, thickness and gap losses as one linear-phase FIR. */
  struct HeadLoss {
    // Odd, so the centre tap exists. 65 up to 48 kHz and the same span in
    // TIME above it: a fixed 65 resolved the curve to sr/65, which is 3 kHz
    // at 192 kHz, and the loss there came out a decibel short from 1 to
    // 10 kHz -- the same tape a decibel brighter at the higher rate.
    static constexpr int kBaseTaps = 65; ///< taps at 48 kHz and below
    static constexpr int kMaxTaps = 257; ///< the cap above it, reached at 192 kHz

    /** The closed-form loss at `hz`, linear (1 at DC): tape speed `v` in m/s,
     *  spacing `d`, gap `g` and thickness `t` in metres. */
    static float magnitude(float hz, float v, float d, float g, float t) {
      if (hz <= 0.0f) return 1.0f;
      const float k = 2.0f * kPi * hz / v; // wavenumber on the tape
      const float spacing = std::exp(-k * d);
      const float kt = k * t;
      const float thickness = kt > 1e-6f ? (1.0f - std::exp(-kt)) / kt : 1.0f;
      const float kg = 0.5f * k * g;
      const float gap = kg > 1e-6f ? std::fabs(std::sin(kg) / kg) : 1.0f;
      return spacing * thickness * gap;
    }

    /** Design the taps for rate `sr` from magnitude() with these arguments,
     *  normalised to unity at DC. Clears the history only when the tap count
     *  changed. No allocation. */
    void design(float sr, float v, float d, float g, float t) {
      v_ = v; d_ = d; g_ = g; t_ = t;
      const int before = count_;
      int n = kBaseTaps;
      if (sr > 48000.0f) n = ((int) (kBaseTaps * sr / 48000.0f + 0.5f)) | 1;
      count_ = n < kMaxTaps ? n : kMaxTaps;
      const int centre = count_ / 2;
      // h[n] = (1/pi) int_0^pi |H(w)| cos(w (n - centre)) dw, midpoint rule
      // on a grid twice the taps, then a Hann window to tame the truncation
      // and DC renormalised to 1. The target is sampled once, and the taps
      // mirrored, so the filter is exactly linear phase.
      const double pi = 3.14159265358979323846;
      const int grid = 2 * count_;
      const double dw = pi / (double) grid;
      double mag[2 * kMaxTaps];
      for (int q = 0; q < grid; ++q) {
        const double w = ((double) q + 0.5) * dw;
        mag[q] = (double) magnitude((float) (w / (2.0 * pi) * (double) sr), v, d, g, t);
      }
      double sum = 0.0;
      for (int k = 0; k <= centre; ++k) {
        double acc = 0.0;
        for (int q = 0; q < grid; ++q) acc += mag[q] * std::cos(((double) q + 0.5) * dw * (double) (k - centre));
        const double win = 0.5 - 0.5 * std::cos(2.0 * pi * (double) (k + 1) / (double) (count_ + 1));
        const double tap = acc * dw / pi * win;
        taps_[k] = taps_[count_ - 1 - k] = (float) tap;
        sum += k == centre ? tap : 2.0 * tap;
      }
      if (sum > 1e-9)
        for (int k = 0; k < count_; ++k) taps_[k] = (float) ((double) taps_[k] / sum);
      // A new length re-lays the history; a new curve at the same length
      // (a speed change mid-stream) keeps it, as it always did.
      if (count_ != before) reset();
    }
    /** Clear the history. */
    void reset() {
      for (float& h : hist_) h = 0.0f;
      pos_ = 0;
    }
    /** One sample through the FIR. */
    inline float process(float x) {
      // Twice the history, each sample written at pos and pos + count, so
      // the newest count samples always sit in one run: one dot product, no
      // wrap. The taps are symmetric, so oldest-first is the same filter.
      hist_[pos_] = x;
      hist_[pos_ + count_] = x;
      if (++pos_ >= count_) pos_ = 0;
      return simd::dot(taps_, &hist_[pos_], (size_t) count_);
    }
    /** The linear-phase delay, half the taps rounded down. */
    int latencySamples() const { return count_ / 2; }
    /** magnitude() for the last design, in dB at `hz` (floored at -180). */
    float targetDb(float hz) const {
      const float m = magnitude(hz, v_, d_, g_, t_);
      return 20.0f * std::log10(m > 1e-9f ? m : 1e-9f);
    }

    float taps_[kMaxTaps]{};     ///< the first count_ are in use, symmetric
    float hist_[2 * kMaxTaps]{}; ///< each input written twice, count_ apart
    /** Taps in use, and the next write position in hist_. */
    int count_ = kBaseTaps, pos_ = 0;
    /** The last design's speed (m/s), spacing, gap and thickness (m). */
    float v_ = 0.381f, d_ = 1e-6f, g_ = 2.5e-6f, t_ = 2e-6f;
  };

  /** The record/playback pair: a high-frequency pre-emphasis (1 + s t2) held
   *  to +20 dB by a pole at 10/t2, and under NAB a low-frequency cut
   *  (1 + s t1)/(10 + s t1); the playback sections are the same rational
   *  functions inverted, discretised with the SAME bilinear constant, so the
   *  digital cascade is the identity to float precision. */
  struct Equalisation {
    /** One first-order section in double: (1 + s tz) / (1 + s tp) designed
     *  bilinear or fitted, or the exact inverse of another section, which is
     *  how each playback section undoes its record one. */
    struct Section {
      /** y = b0 x + b1 x1 - a1 y1, with x1 and y1 the previous input and
       *  output; the defaults pass the signal unchanged. */
      double b0 = 1.0, b1 = 0.0, a1 = 0.0, x1 = 0.0, y1 = 0.0;
      /** (1 + s tz) / (1 + s tp), bilinear with the constant c. */
      void design(double tz, double tp, double c, double gain) {
        const double n0 = 1.0 + c * tz, n1 = 1.0 - c * tz;
        const double d0 = 1.0 + c * tp, d1 = 1.0 - c * tp;
        b0 = gain * n0 / d0;
        b1 = gain * n1 / d0;
        a1 = d1 / d0;
      }
      /** gain (1 + s tz) / (1 + s tp), FITTED rather than bilinear: the gain
       *  at DC and at Nyquist exactly the analogue's, and the pole placed so
       *  the worst error in dB from 100 Hz to 0.45 fs is least -- half a
       *  decibel at worst, for every standard at every rate. Bilinear,
       *  prewarped at the corner, squeezed the curve above it towards
       *  Nyquist: NAB's 15 ips emphasis was +4.9 dB hot at 20 kHz in a
       *  44.1 kHz session and +0.2 at 192, so the same tape hit the loop
       *  harder at the lower rate and compressed its top 3.4 dB where
       *  192 kHz compressed 2.2. (Matching the pole by impulse invariance
       *  instead was tried, and is 1.8 dB off at 10 kHz.) Designed once per
       *  rate and standard; a golden-section search, a few thousand cosines. */
      void fitted(double tz, double tp, double fs, double gain) {
        const double pi = 3.14159265358979323846;
        const double wn = pi * fs;
        const double gNy = gain * std::sqrt((1.0 + wn * wn * tz * tz) / (1.0 + wn * wn * tp * tp));
        const double gainDbDc = 20.0 * std::log10(gain);
        auto place = [&](double pole) {
          a1 = pole;
          b0 = 0.5 * (gain * (1.0 + a1) + gNy * (1.0 - a1));
          b1 = 0.5 * (gain * (1.0 + a1) - gNy * (1.0 - a1));
        };
        auto worst = [&](double pole) {
          place(pole);
          double e = 0.0;
          for (int k = 0; k < 32; ++k) {
            const double hz = 100.0 * std::pow(0.45 * fs / 100.0, (double) k / 31.0);
            const double w = 2.0 * pi * hz;
            const double want = gainDbDc + 10.0 * std::log10((1.0 + w * w * tz * tz) / (1.0 + w * w * tp * tp));
            const double miss = std::fabs(gainDb(hz, fs) - want);
            if (miss > e) e = miss;
          }
          return e;
        };
        const double phi = 0.6180339887498949;
        double lo = -0.999, hi = 0.999;
        double c1 = hi - phi * (hi - lo), c2 = lo + phi * (hi - lo);
        double f1 = worst(c1), f2 = worst(c2);
        for (int it = 0; it < 60; ++it) {
          if (f1 < f2) {
            hi = c2; c2 = c1; f2 = f1;
            c1 = hi - phi * (hi - lo); f1 = worst(c1);
          } else {
            lo = c1; c1 = c2; f1 = f2;
            c2 = lo + phi * (hi - lo); f2 = worst(c2);
          }
        }
        place(0.5 * (lo + hi));
      }
      /** The exact digital inverse of another section -- its zero becomes
       *  the pole -- so record then playback is the identity to rounding. */
      void inverseOf(const Section& s) {
        b0 = 1.0 / s.b0;
        b1 = s.a1 / s.b0;
        a1 = s.b1 / s.b0;
      }
      /** Gain at hz, in dB, from the coefficients. */
      double gainDb(double hz, double fs) const {
        const double w = 2.0 * 3.14159265358979323846 * hz / fs;
        const double cw = std::cos(w), sw = std::sin(w);
        const double nr = b0 + b1 * cw, ni = -b1 * sw;
        const double dr = 1.0 + a1 * cw, di = -a1 * sw;
        return 10.0 * std::log10((nr * nr + ni * ni) / (dr * dr + di * di));
      }
      /** Coefficients of a wire; the state is kept. */
      void bypass() { b0 = 1.0; b1 = 0.0; a1 = 0.0; }
      /** Clear the state. */
      void reset() { x1 = y1 = 0.0; }
      /** One sample; state below 1e-30 or above 1e300 in size, NaN or
       *  infinite, is stored as zero. */
      inline double process(double x) {
        const double y = b0 * x + b1 * x1 - a1 * y1;
        // Tiny state to zero, NaN and Inf included, as IirSection keeps its
        // own. One NaN or infinite sample was kept here for good: the record
        // side fed the loop NaN from then on, the loop flushed it to zero
        // every sample, and the machine played finite SILENCE -- a tone at
        // rms 0.18 came out 0.0000 a second later, with nothing for an
        // output guard to catch.
        const double mx = std::fabs(x), my = std::fabs(y);
        x1 = (mx >= 1e-30 && mx <= 1e300) ? x : 0.0;
        y1 = (my >= 1e-30 && my <= 1e300) ? y : 0.0;
        return y;
      }
    };
    static constexpr double kLimit = 10.0; ///< the emphasis levels off at +20 dB

    /** Design all four sections for rate `sr`, standard `eq` and the speed
     *  bucket nearest `ips`; a standard with no term at that speed bypasses
     *  its sections. No allocation. */
    void design(float sr, TapeEq eq, float ips) {
      t1_ = t2_ = 0.0;
      if (eq != TapeEq::Off) timeConstants(eq, ips, t1_, t2_);
      const double pi = 3.14159265358979323846;
      auto warp = [&](double tau) {
        const double w = 1.0 / tau;
        return w / std::tan(w / (2.0 * (double) sr));
      };
      fs_ = (double) sr;
      if (t2_ > 0.0) {
        recordHf_.fitted(t2_, t2_ / kLimit, fs_, 1.0);
        playHf_.inverseOf(recordHf_);
      } else {
        recordHf_.bypass();
        playHf_.bypass();
      }
      if (t1_ > 0.0) {
        // (1 + s t1) / (kLimit + s t1) = (1/kLimit) (1 + s t1) / (1 + s t1/kLimit)
        const double c = warp(t1_);
        recordLf_.design(t1_, t1_ / kLimit, c, 1.0 / kLimit);
        playLf_.design(t1_ / kLimit, t1_, c, kLimit);
      } else {
        recordLf_.bypass();
        playLf_.bypass();
      }
      (void) pi;
    }
    /** The standards' time constants, seconds, for the nearest tape speed.
     *  Zero means no term. */
    static void timeConstants(TapeEq eq, float ips, double& t1, double& t2) {
      int speed = 15;
      if (ips < 5.6f) speed = 4; else if (ips < 11.0f) speed = 7; else if (ips < 22.0f) speed = 15; else speed = 30;
      switch (eq) {
        case TapeEq::NAB:
          t1 = 3180e-6;
          t2 = speed == 4 ? 90e-6 : (speed == 30 ? 17.5e-6 : 50e-6);
          break;
        case TapeEq::IEC:
          if (speed == 4) { t1 = 3180e-6; t2 = 120e-6; }
          else if (speed == 7) { t1 = 3180e-6; t2 = 70e-6; }
          else if (speed == 15) { t1 = 0.0; t2 = 35e-6; }
          else { t1 = 0.0; t2 = 17.5e-6; }
          break;
        case TapeEq::AES:
          t1 = 0.0;
          t2 = 17.5e-6;
          break;
        default:
          t1 = t2 = 0.0;
          break;
      }
    }
    /** Clear all four sections' state. */
    void reset() { recordHf_.reset(); recordLf_.reset(); playHf_.reset(); playLf_.reset(); }
    /** One sample of the record side: what the loop sees. */
    inline float record(float x) { return (float) recordLf_.process(recordHf_.process((double) x)); }
    /** One sample of the playback side, the record side's inverse. */
    inline float playback(float x) { return (float) playLf_.process(playHf_.process((double) x)); }
    /** The record side's gain in dB at hz as the digital sections realise it. */
    float recordBuiltDb(float hz) const {
      return (float) (recordHf_.gainDb((double) hz, fs_) + recordLf_.gainDb((double) hz, fs_));
    }
    /** The record side's analogue gain in dB at hz. */
    float recordDb(float hz) const {
      const double w = 2.0 * 3.14159265358979323846 * (double) hz;
      double g = 1.0;
      if (t2_ > 0.0) g *= std::sqrt((1.0 + w * w * t2_ * t2_) / (1.0 + w * w * t2_ * t2_ / (kLimit * kLimit)));
      if (t1_ > 0.0) g *= std::sqrt((1.0 + w * w * t1_ * t1_) / (kLimit * kLimit + w * w * t1_ * t1_));
      return (float) (20.0 * std::log10(g));
    }
    /** The record emphasis and low cut, and their playback inverses. */
    Section recordHf_, recordLf_, playHf_, playLf_;
    /** The low (t1) and high (t2) time constants in seconds, 0 for none,
     *  and the rate in Hz the sections were designed at. */
    double t1_ = 0.0, t2_ = 0.0, fs_ = 48000.0;
  };

  Oversampled<JilesAtherton, 1> loop_;
  HeadLoss loss_;
  Equalisation eq_;
  DelayLine<kTransportSamples> transport_;
  PinkNoise flutterNoise_;
  Parameters params_{};
  float sr_ = 48000.0f, nominal_ = 192.0f, noiseCoef_ = 0.002f;
  float wowDepth_ = 0.0f, flutterDepth_ = 0.0f, makeup_ = 1.0f, driveComp_ = 1.0f;
  /** What the head FIR and equalisers were last designed for; zero until the
   *  first design, so that one always happens. */
  float designedSr_ = 0.0f, designedIps_ = 0.0f, designedSpacing_ = -1.0f;
  float designedGap_ = 0.0f, designedThickness_ = 0.0f;
  TapeEq designedEq_ = TapeEq::Off;
  float wowPhase_ = 0.0f, flutterPhase_[2] = {0.0f, 0.0f}, noiseZ_ = 0.0f;
  float dcR_ = 0.9993f, dcX1_ = 0.0f, dcY1_ = 0.0f;
  Smooth gainSmooth_, wowSmooth_, flutterSmooth_;
  bool running_ = false; // process() has run since reset()
};

} // namespace sonore
