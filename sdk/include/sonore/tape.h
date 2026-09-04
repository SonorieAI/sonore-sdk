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
// knee. The saturation ceiling is then 1/(chi0 * 2.5), about +4.7 dB above
// unity at the default reversibility. TapeSaturator divides the drive back
// out after the loop, so its drive knob changes how far into the loop the
// signal goes and not how loud it comes out; `makeupDb` is the volume.
//
// Playback loss is the head model from the same paper (Kadis's spacing,
// thickness and gap losses: e^{-kd}, (1 - e^{-k delta})/(k delta) and
// sin(kg/2)/(kg/2) with k = 2 pi f / v). The whole curve slides along the
// frequency axis with the tape speed -- which is why 7.5 ips is dull by
// 10 kHz and 30 ips is not -- and that scaling law, H(f, v) = H(f/2, v/2),
// is what the unit test asserts. It is realised as a 65-tap linear-phase FIR
// designed from the magnitude by inverse Fourier sum at prepare (and on a
// speed change): 32 samples of latency at every rate. The default head
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
#include <cmath>
#include "audio.h"
#include "random.h"

namespace sonore {

/**
 * The hysteresis loop on its own: a per-sample shaper with memory, meant to
 * run inside Oversampled<> (its state advances at the oversampled rate,
 * which is the rate RK4 wants). TapeSaturator does that for you.
 */
class JilesAtherton {
public:
  /** How the loop is integrated. RK4 is the paper's baseline; NewtonRaphson
   *  is the implicit trapezoidal rule solved by four Newton steps per sample
   *  (Chowdhury's NR4, from his 2020 comparison of the two), which holds its
   *  accuracy at larger field steps and is what the plugin offers as its
   *  "accurate" mode. Both get the sub-stepping. */
  enum class Solver { RK4, NewtonRaphson };

  JilesAtherton() { cook(); }

  void setSolver(Solver s) { solver_ = s; }
  Solver solver() const { return solver_; }

  /** How far a full-scale input pushes into the loop. 1 = the knee. */
  void setDrive(float linear) { drive_ = linear < 0.0f ? 0.0f : linear; }
  /** Coercivity k, in units of the anhysteretic scale: how WIDE the loop is.
   *  0..1 maps onto 0.3..2.0; 0.55 is the paper's 27/22 = 1.23. */
  void setWidth(float w) {
    kWant_ = 0.3f + clampf(w, 0.0f, 1.0f) * 1.7f;
    cook();
  }
  /** How much of the magnetisation is irreversible, 0..1 (c = 1 - h). At 0
   *  the loop collapses onto the anhysteretic curve, a soft saturator with no
   *  memory; at 1 nothing is reversible. Clamped for well-posedness -- see the
   *  header -- so reversibility() reports what was actually installed. */
  void setHysteresis(float h) {
    hWant_ = clampf(h, 0.0f, 1.0f);
    cook();
  }
  void reset() { m_ = 0.0f; h1_ = 0.0f; }

  float coercivity() const { return k_; }
  float reversibility() const { return c_; }
  /** The gain the loop output is scaled by, so a small signal comes out at
   *  unity. The saturation ceiling is this times Ms = 1. */
  float normalisation() const { return norm_; }

  inline float process(float x) {
    // Field from the input, in units where the knee sits at |H| ~ 2.5.
    const float h = x * drive_ * kFieldScale;
    const float dh = h - h1_;
    const float adh = std::fabs(dh);
    int steps = 1;
    if (adh > kMaxStep) {
      steps = (int) (adh / kMaxStep) + 1;
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

  void cook() {
    k_ = kWant_;
    // The denominator's zero sits at Man - M = (1-c)k/alpha; a swing can
    // reach |Man - M| ~ 2, so (1-c)k is kept at least four alphas clear.
    const float cMax = 1.0f - 4.0f * kAlpha / k_;
    float c = 1.0f - hWant_;
    if (c > cMax) c = cMax;
    if (c < 0.01f) c = 0.01f;
    c_ = c;
    // Initial susceptibility: at M = 0 the irreversible term vanishes with
    // H, leaving c L'(0) / (1 - c alpha L'(0)) with L'(0) = 1/3.
    const float chi0 = (c_ / 3.0f) / (1.0f - c_ * kAlpha / 3.0f);
    norm_ = 1.0f / (chi0 * kFieldScale);
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
  float drive_ = 1.0f, kWant_ = 1.235f, hWant_ = 0.3f;
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
enum class TapeEq { Off, NAB, IEC, AES };

/**
 * The whole machine, one channel: record equalisation into the hysteresis at
 * 2x, the head's playback loss for the chosen speed, playback equalisation,
 * wow and flutter, makeup. Two of these for stereo -- they hold state and the
 * transports should not be shared. Rates up to 192 kHz: the transport ring
 * is sized for 4 ms + the deepest wobble at that rate.
 */
class TapeSaturator {
public:
  struct Parameters {
    float driveDb = 0.0f;       // into the loop; 0 dB = full scale at the knee
    float width = 0.55f;        // hysteresis loop width 0..1 (0.55 = the paper's k)
    float hysteresis = 0.3f;    // irreversible share 0..1 (0.3 = c of 0.7)
    float speedIps = 15.0f;     // 3.75 .. 30: slides the head loss along frequency
    float spacingUm = 1.0f;     // head-to-tape spacing, microns
    float gapUm = 2.5f;         // playback head gap, microns
    float thicknessUm = 2.0f;   // effective recorded depth, microns
    float wow = 0.0f;           // 0..1, slow pitch drift (up to 2 ms)
    float flutter = 0.0f;       // 0..1, fast shimmer (up to 0.3 ms)
    float makeupDb = 0.0f;
    bool accurate = false;      // the implicit Newton-Raphson solver instead of RK4
    TapeEq eq = TapeEq::NAB;    // record/playback equalisation around the loop
  };

  void setSampleRate(float sr) {
    sr_ = sr > 1.0f ? sr : 48000.0f;
    // 4 ms, rounded to a whole sample so the unmodulated read is exact and
    // the reported latency is the real one.
    nominal_ = (float) (int) (0.004f * sr_ + 0.5f);
    if (nominal_ < 8.0f) nominal_ = 8.0f;
    // The flutter's noise component: pink noise smoothed to ~15 Hz.
    noiseCoef_ = 1.0f - std::exp(-2.0f * kPi * 15.0f / sr_);
    setParameters(params_);
    reset();
  }
  void reset() {
    loop_.reset();
    loop_.shaper().reset();
    loss_.reset();
    eq_.reset();
    transport_.reset();
    wowPhase_ = 0.0f;
    flutterPhase_[0] = flutterPhase_[1] = 0.0f;
    noiseZ_ = 0.0f;
  }

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
    const float ips = clampf(p.speedIps, 3.75f, 30.0f);
    loss_.design(sr_, ips * 0.0254f, clampf(p.spacingUm, 0.0f, 50.0f) * 1e-6f,
                 clampf(p.gapUm, 0.1f, 50.0f) * 1e-6f, clampf(p.thicknessUm, 0.1f, 50.0f) * 1e-6f);
    eq_.design(sr_, p.eq, ips);
    wowDepth_ = clampf(p.wow, 0.0f, 1.0f) * 0.002f * sr_;
    flutterDepth_ = clampf(p.flutter, 0.0f, 1.0f) * 0.0003f * sr_;
    makeup_ = dbToGain(clampf(p.makeupDb, -24.0f, 24.0f));
  }

  inline float process(float x) {
    // Record equalisation, then the loop at 2x -- its RK4 wants the field to
    // move a little per step -- then the head, then playback equalisation.
    float y = loop_.process(eq_.record(x));
    y = eq_.playback(loss_.process(y));
    // Wow and flutter are the read point moving. Flutter is two
    // incommensurate sines plus a little smoothed noise, because a single
    // sine is vibrato, not flutter.
    wowPhase_ += 0.7f / sr_;
    if (wowPhase_ >= 1.0f) wowPhase_ -= 1.0f;
    flutterPhase_[0] += 8.3f / sr_;
    flutterPhase_[1] += 14.7f / sr_;
    if (flutterPhase_[0] >= 1.0f) flutterPhase_[0] -= 1.0f;
    if (flutterPhase_[1] >= 1.0f) flutterPhase_[1] -= 1.0f;
    noiseZ_ += (flutterNoise_.next() - noiseZ_) * noiseCoef_;
    const float wow = wowDepth_ * fastmath::sinTurns(wowPhase_);
    const float flutter = flutterDepth_ * (0.5f * fastmath::sinTurns(flutterPhase_[0]) +
                                           0.3f * fastmath::sinTurns(flutterPhase_[1]) + 2.0f * noiseZ_);
    // Read BEFORE writing: DelayLine's read(d) after a write is d - 1, and
    // the host is told the nominal figure. The cross-correlation in the test
    // measured 239 against a reported 240 the other way round.
    const float out = (wowDepth_ > 0.0f || flutterDepth_ > 0.0f)
                          ? transport_.readCubic(nominal_ + wow + flutter)
                          : transport_.read(nominal_);
    transport_.write(y);
    return out * driveComp_ * makeup_;
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

  JilesAtherton& loop() { return loop_.shaper(); }
  const JilesAtherton& loop() const { return loop_.shaper(); }

private:
  static constexpr int kTransportSamples = 2048; // 4 ms + 2.3 ms of wobble at 192 kHz

  /** Spacing, thickness and gap losses as one linear-phase FIR. */
  struct HeadLoss {
    static constexpr int kTaps = 65;   // odd, so the centre tap exists: latency 32
    static constexpr int kGrid = 128;  // frequency samples of the target, 0..Nyquist

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

    void design(float sr, float v, float d, float g, float t) {
      v_ = v; d_ = d; g_ = g; t_ = t;
      const int centre = kTaps / 2;
      // h[n] = (1/pi) int_0^pi |H(w)| cos(w (n - centre)) dw, midpoint rule,
      // then a Hann window to tame the truncation and DC renormalised to 1.
      const double dw = 3.14159265358979323846 / (double) kGrid;
      double sum = 0.0;
      for (int n = 0; n < kTaps; ++n) {
        double acc = 0.0;
        for (int q = 0; q < kGrid; ++q) {
          const double w = ((double) q + 0.5) * dw;
          const float hz = (float) (w / (2.0 * 3.14159265358979323846) * (double) sr);
          acc += (double) magnitude(hz, v, d, g, t) * std::cos(w * (double) (n - centre));
        }
        const double win = 0.5 - 0.5 * std::cos(2.0 * 3.14159265358979323846 * (double) (n + 1) / (double) (kTaps + 1));
        taps_[n] = (float) (acc * dw / 3.14159265358979323846 * win);
        sum += taps_[n];
      }
      if (sum > 1e-9) for (float& c : taps_) c = (float) ((double) c / sum);
    }
    void reset() {
      for (float& h : hist_) h = 0.0f;
      pos_ = 0;
    }
    inline float process(float x) {
      hist_[pos_] = x;
      float acc = 0.0f;
      int i = pos_;
      for (int n = 0; n < kTaps; ++n) {
        acc += taps_[n] * hist_[i];
        if (--i < 0) i = kTaps - 1;
      }
      if (++pos_ >= kTaps) pos_ = 0;
      return acc;
    }
    static constexpr int latencySamples() { return kTaps / 2; }
    float targetDb(float hz) const {
      const float m = magnitude(hz, v_, d_, g_, t_);
      return 20.0f * std::log10(m > 1e-9f ? m : 1e-9f);
    }

    float taps_[kTaps]{};
    float hist_[kTaps]{};
    int pos_ = 0;
    float v_ = 0.381f, d_ = 1e-6f, g_ = 2.5e-6f, t_ = 2e-6f;
  };

  /** The record/playback pair: a high-frequency pre-emphasis (1 + s t2) held
   *  to +20 dB by a pole at 10/t2, and under NAB a low-frequency cut
   *  (1 + s t1)/(10 + s t1); the playback sections are the same rational
   *  functions inverted, discretised with the SAME bilinear constant, so the
   *  digital cascade is the identity to float precision. */
  struct Equalisation {
    struct Section {
      double b0 = 1.0, b1 = 0.0, a1 = 0.0, x1 = 0.0, y1 = 0.0;
      /** (1 + s tz) / (1 + s tp), bilinear with the constant c. */
      void design(double tz, double tp, double c, double gain) {
        const double n0 = 1.0 + c * tz, n1 = 1.0 - c * tz;
        const double d0 = 1.0 + c * tp, d1 = 1.0 - c * tp;
        b0 = gain * n0 / d0;
        b1 = gain * n1 / d0;
        a1 = d1 / d0;
      }
      void bypass() { b0 = 1.0; b1 = 0.0; a1 = 0.0; }
      void reset() { x1 = y1 = 0.0; }
      inline double process(double x) {
        const double y = b0 * x + b1 * x1 - a1 * y1;
        x1 = x;
        y1 = y;
        return y;
      }
    };
    static constexpr double kLimit = 10.0; // the emphasis levels off at +20 dB

    void design(float sr, TapeEq eq, float ips) {
      t1_ = t2_ = 0.0;
      if (eq != TapeEq::Off) timeConstants(eq, ips, t1_, t2_);
      const double pi = 3.14159265358979323846;
      auto warp = [&](double tau) {
        const double w = 1.0 / tau;
        return w / std::tan(w / (2.0 * (double) sr));
      };
      if (t2_ > 0.0) {
        const double c = warp(t2_);
        recordHf_.design(t2_, t2_ / kLimit, c, 1.0);
        playHf_.design(t2_ / kLimit, t2_, c, 1.0);
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
    void reset() { recordHf_.reset(); recordLf_.reset(); playHf_.reset(); playLf_.reset(); }
    inline float record(float x) { return (float) recordLf_.process(recordHf_.process((double) x)); }
    inline float playback(float x) { return (float) playLf_.process(playHf_.process((double) x)); }
    /** The record side's analogue gain in dB at hz. */
    float recordDb(float hz) const {
      const double w = 2.0 * 3.14159265358979323846 * (double) hz;
      double g = 1.0;
      if (t2_ > 0.0) g *= std::sqrt((1.0 + w * w * t2_ * t2_) / (1.0 + w * w * t2_ * t2_ / (kLimit * kLimit)));
      if (t1_ > 0.0) g *= std::sqrt((1.0 + w * w * t1_ * t1_) / (kLimit * kLimit + w * w * t1_ * t1_));
      return (float) (20.0 * std::log10(g));
    }
    Section recordHf_, recordLf_, playHf_, playLf_;
    double t1_ = 0.0, t2_ = 0.0;
  };

  Oversampled<JilesAtherton, 1> loop_;
  HeadLoss loss_;
  Equalisation eq_;
  DelayLine<kTransportSamples> transport_;
  PinkNoise flutterNoise_;
  Parameters params_{};
  float sr_ = 48000.0f, nominal_ = 192.0f, noiseCoef_ = 0.002f;
  float wowDepth_ = 0.0f, flutterDepth_ = 0.0f, makeup_ = 1.0f, driveComp_ = 1.0f;
  float wowPhase_ = 0.0f, flutterPhase_[2] = {0.0f, 0.0f}, noiseZ_ = 0.0f;
};

} // namespace sonore
