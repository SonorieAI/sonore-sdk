// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: a valve gain stage and the tone stack after it.
//
// "Make me an amp" produced a tanh every time, and a tanh is what a valve is
// not: it is symmetric, and a single-ended triode stage is not. The stage's
// asymmetry -- cut-off on one side, grid conduction on the other, the plate
// swinging further one way than the other -- is where the even harmonics come
// from, and the even harmonics are the sound.
//
// TubeStage is the common-cathode triode stage WITH ITS CAPACITORS. The
// static transfer -- plate voltage against grid voltage with the plate load
// in the loop -- is Koren's triode ("Improved vacuum-tube models for SPICE
// simulations", 1996) solved once at prepare() into a table. That curve is
// what the first version had, and it is most of the sound of a stage played
// quietly. What it is not is the sound of a stage played hard, because the
// two things a driven triode does that a curve cannot are both capacitors:
//
//   BLOCKING. Past 0 V the grid conducts, and the current it draws charges the
//   input coupling capacitor through the source's impedance. The capacitor
//   holds that charge for R_g C_in (22 ms here) and holds the grid negative
//   with it, so after a loud transient the stage is biased towards cut-off
//   and the next few milliseconds come out GATED -- the "blocking distortion"
//   every overdriven preamp has, and the reason a hard-hit chord chokes and
//   then swells. The grid current is Dempwolf & Zölzer's ("A physically-
//   motivated triode model for circuit simulations", DAFx 2011) with their
//   ECC83 fit, the node solved by Newton each sample with the source
//   impedance in series -- which is what bounds the current and the charging
//   rate, and the reason a stage driven from a low impedance blocks harder.
//
//   BIAS SHIFT. The cathode bypass capacitor holds the cathode voltage at
//   its idle value only on average: a loud passage raises the mean cathode
//   current, the capacitor charges, the bias moves negative and the stage's
//   duty cycle and gain move with it, over R_k C_k (37 ms). Set the bypass to
//   zero and the cathode is degenerated instead: the voltage is solved from
//   the current each sample and the gain halves, which is the other stage
//   every amp has.
//
// That is the model Pakarinen & Yeh's review (CMJ 2009) puts between the
// static waveshaper and a full nodal simulation, and it is Macak & Schimmel's
// (DAFx 2010) arrangement: the reactive parts as first-order states, the
// tube as a static nonlinearity between them. Not a K-method matrix solve --
// a plate load that is a resistor needs none -- but every capacitor the
// circuit has, doing what it does.
//
// A single stage INVERTS. The default output gain is 1/(small-signal gain),
// which is negative, so a quiet signal comes out at unity and the right way
// up -- a plugin that blends a stage with its dry input would otherwise
// cancel. Pass a negative output gain to keep the inversion.
//
// ToneStack is the '59 Fender Bassman passive stack, from Yeh & Smith's
// "Discretization of the '59 Fender Bassman Tone Stack" (DAFx 2006): the
// three-knob network's transfer function is derived symbolically from the
// component values, so the knobs interact exactly as the circuit's do (the
// mid scoop with everything at noon is the circuit's, not a shape drawn to
// look like it), and the bilinear transform turns the cubic into a third-
// order digital filter. The bilinear expansion here is derived from the
// substitution rather than copied, and the unit test checks the digital
// response against the analogue one at the pre-warped frequency, where the
// two are equal by construction. The filter runs in DOUBLE: the network's
// slowest pole (R2 C2, ~8 Hz) puts a z-pole at 0.999 at 48 kHz, which a
// float transposed-direct-form section renders with a low-frequency error
// the ear can hear as a tilt.
//
// Included by dsp.h.
#pragma once
#include <cmath>
#include "audio.h"

namespace sonore {

// ── Triode stage ─────────────────────────────────────────────────────────────

/**
 * One common-cathode triode stage, 12AX7 by default: 300 V supply, 100 kΩ
 * plate load, 1.5 kΩ cathode bypassed by 25 µF, driven through 38 kΩ (the
 * plate impedance of the stage before it) and a 22 nF coupling capacitor
 * into a 1 MΩ grid leak. The input is in VOLTS at the source (a ±1 signal is
 * ±1 V, which is a hot line level into a first stage -- `setInputGain` is the
 * volume knob before it); the output is the plate swing normalised so a
 * small signal comes out at unity, the right way up.
 */
class TubeStage {
public:
  struct Triode {
    // Koren's plate-current parameters, 12AX7.
    float mu = 100.0f, ex = 1.4f, kg1 = 1060.0f, kp = 600.0f, kvb = 300.0f;
    // Dempwolf & Zölzer's grid-current parameters, ECC83: Ig = Gg (ln(1 + e^(Cg Vgk)) / Cg)^xi.
    float gg = 6.177e-4f, cg = 9.901f, xi = 1.314f;
  };

  void setSampleRate(float sr) {
    sr_ = sr > 1.0f ? sr : 48000.0f;
    blocker_.setSampleRate(sr_);
    dtOverCin_ = 1.0f / (sr_ * cin_);
    dtOverCk_ = ck_ > 0.0f ? 1.0f / (sr_ * ck_) : 0.0f;
    if (!built_) build();
  }
  /** Volume before the grid, linear. */
  void setInputGain(float g) { inputGain_ = g < 0.0f ? 0.0f : g; }
  /** A plain multiplier on the unity-normalised output. Negative keeps the
   *  stage's own inversion. */
  void setOutputGain(float g) { outputGain_ = g; }
  void setTriode(const Triode& t) {
    triode_ = t;
    build();
  }
  void setCircuit(float supplyVolts, float plateKohm, float cathodeKohm) {
    supply_ = supplyVolts;
    // Koren's current comes out in AMPS with these constants, so the
    // resistors are held in ohms. The first build had them in kΩ and got a
    // stage gain of -0.4: a 0.2 V plate swing instead of 200.
    rp_ = plateKohm * 1000.0f;
    rk_ = cathodeKohm * 1000.0f;
    build();
  }
  /** The input network: coupling capacitor (nF), grid leak (MΩ) and the
   *  impedance of whatever drives the stage (kΩ, floored at 5 -- below that
   *  the capacitor's charging time falls under a few samples). */
  void setCoupling(float couplingNf, float gridLeakMohm, float sourceKohm) {
    cin_ = clampf(couplingNf, 0.1f, 10000.0f) * 1e-9f;
    rg_ = clampf(gridLeakMohm, 0.01f, 100.0f) * 1e6f;
    rs_ = clampf(sourceKohm, 5.0f, 10000.0f) * 1e3f;
    dtOverCin_ = 1.0f / (sr_ * cin_);
    build();
  }
  /** Cathode bypass capacitor in µF; 0 = unbypassed, the degenerated stage. */
  void setCathodeBypass(float microfarads) {
    ck_ = microfarads > 0.0f ? microfarads * 1e-6f : 0.0f;
    dtOverCk_ = ck_ > 0.0f ? 1.0f / (sr_ * ck_) : 0.0f;
    build();
  }
  void reset() {
    blocker_.reset();
    vc_ = 0.0f;
    vk_ = vk0_;
    vg_ = 0.0f;
  }

  inline float process(float x) {
    const float vs = x * inputGain_;
    // The grid node, with the coupling capacitor's charge subtracted from
    // the source and the source impedance in series: solved for Vg.
    const float a = vs - vc_;
    const float vg = solveGrid(a);
    vg_ = vg;
    const float vgk = vg - vk_;
    const float ig = gridCurrent(vgk);
    // The current through the source impedance is what charges the coupling
    // capacitor: the grid leak's share and the grid's own.
    vc_ = flushDenormal(vc_ + ((a - vg) / rs_) * dtOverCin_);
    const float vp = plateVolts(vgk);
    const float ip = (supply_ - vp) / rp_;
    if (ck_ > 0.0f) {
      // Bypassed: the cathode voltage follows the mean current through R_k C_k.
      vk_ += ((ip + ig) - vk_ / rk_) * dtOverCk_;
    } else {
      // Unbypassed: the cathode sits at I_k R_k this instant, solved.
      vk_ = solveCathode(vg);
    }
    return blocker_.process((vp - vp0_) * norm_ * outputGain_);
  }

  /** The stage's small-signal voltage gain from the source, negative (it
   *  inverts), for the current bypass setting. */
  float smallSignalGain() const { return gain_; }
  /** Idle plate voltage and cathode bias. */
  float idlePlateVolts() const { return vp0_; }
  float cathodeBiasVolts() const { return vk0_; }
  /** The live states: the cathode voltage now (the bias shift), the charge on
   *  the coupling capacitor now (the blocking), the grid current at the last
   *  sample in mA. */
  float cathodeVolts() const { return vk_; }
  float couplingVolts() const { return vc_; }
  float gridCurrentMilliamps() const { return 1000.0f * gridCurrent(vg_ - vk_); }
  /** The DC transfer at a grid-cathode voltage: plate swing in volts about
   *  the idle point, for checking the curve against the tube's data. */
  float plateSwingVolts(float gridToCathodeVolts) const { return plateVolts(gridToCathodeVolts) - vp0_; }

private:
  static constexpr int kPoints = 2048;
  static constexpr float kVgkMin = -24.0f, kVgkMax = 6.0f;

  /** Koren: plate current in AMPS for a grid-cathode and plate-cathode voltage. */
  float plateCurrent(float vgk, float vpk) const {
    if (vpk <= 0.0f) return 0.0f;
    const float inner = triode_.kp * (1.0f / triode_.mu + vgk / std::sqrt(triode_.kvb + vpk * vpk));
    // log1p(exp(inner)) without overflow for a large positive inner.
    const float e1 = (vpk / triode_.kp) * (inner > 30.0f ? inner : std::log1p(std::exp(inner)));
    if (e1 <= 0.0f) return 0.0f;
    return 2.0f * std::pow(e1, triode_.ex) / triode_.kg1;
  }

  /** Dempwolf: grid current in AMPS, a soft diode that opens around 0 V. */
  inline float gridCurrent(float vgk) const {
    const float y = triode_.cg * vgk;
    if (y < -30.0f) return 0.0f;
    const float soft = (y > 30.0f ? y : std::log1p(std::exp(y))) / triode_.cg;
    return triode_.gg * std::pow(soft, triode_.xi);
  }
  inline float gridCurrentSlope(float vgk) const {
    const float y = triode_.cg * vgk;
    if (y < -30.0f) return 0.0f;
    const float soft = (y > 30.0f ? y : std::log1p(std::exp(y))) / triode_.cg;
    const float sigmoid = y > 30.0f ? 1.0f : 1.0f / (1.0f + std::exp(-y));
    return triode_.gg * triode_.xi * std::pow(soft > 1e-9f ? soft : 1e-9f, triode_.xi - 1.0f) * sigmoid;
  }

  /** The grid node: (a - Vg)/Rs = Vg/Rg + Ig(Vg - Vk), solved for Vg. The
   *  left side falls and the right rises with Vg, so the root is unique;
   *  Newton from the linear (no grid current) solution lands in a few steps
   *  and the closed form is exact while the grid is not conducting. */
  inline float solveGrid(float a) const {
    float vg = a * rg_ / (rg_ + rs_);
    if (vg - vk_ < -1.0f) return vg; // Dempwolf's current is 1e-6 of its 1 V value here
    for (int i = 0; i < 6; ++i) {
      const float vgk = vg - vk_;
      const float f = (a - vg) / rs_ - vg / rg_ - gridCurrent(vgk);
      const float df = -1.0f / rs_ - 1.0f / rg_ - gridCurrentSlope(vgk);
      const float step = f / df;
      vg -= step;
      if (std::fabs(step) < 1e-5f) break;
    }
    return vg;
  }

  /** Plate voltage for a grid-cathode voltage, from the table. */
  inline float plateVolts(float vgk) const {
    const float t = (vgk - kVgkMin) * tableScale_;
    if (t <= 0.0f) return table_[0];
    if (t >= (float) (kPoints - 1)) return table_[kPoints - 1];
    const int i = (int) t;
    const float frac = t - (float) i;
    return table_[i] + frac * (table_[i + 1] - table_[i]);
  }

  /** Plate voltage for a grid-cathode voltage: solve Ip(Vgk, Vp)·Rp = B+ − Vp. Ip
   *  rises with Vp, so the residual is monotonic and bisection is exact. */
  float solvePlate(float vgk) const {
    float lo = 0.0f, hi = supply_;
    for (int i = 0; i < 40; ++i) {
      const float mid = 0.5f * (lo + hi);
      const float residual = plateCurrent(vgk, mid) * rp_ + mid - supply_;
      if (residual > 0.0f) hi = mid; else lo = mid;
    }
    return 0.5f * (lo + hi);
  }

  /** The unbypassed cathode: Vk = (Ip + Ig)(Vg - Vk) R_k. The residual falls
   *  monotonically with Vk, so bisection finds it, from the table. */
  inline float solveCathode(float vg) const {
    float lo = 0.0f, hi = supply_ > 50.0f ? 50.0f : supply_;
    for (int i = 0; i < 22; ++i) {
      const float mid = 0.5f * (lo + hi);
      const float vgk = vg - mid;
      const float ik = (supply_ - plateVolts(vgk)) / rp_ + gridCurrent(vgk);
      if (ik * rk_ - mid > 0.0f) lo = mid; else hi = mid;
    }
    return 0.5f * (lo + hi);
  }

  void build() {
    // The curve first: plate voltage over a grid swing wide enough to be
    // fully cut off and fully driven into conduction, with a bias shift's
    // worth of room below.
    tableScale_ = (float) (kPoints - 1) / (kVgkMax - kVgkMin);
    for (int i = 0; i < kPoints; ++i) table_[i] = solvePlate(kVgkMin + (float) i / tableScale_);
    // Operating point: the cathode sits at Ik·Rk above ground with the grid
    // at 0 V. The residual Ik(-Vk)·Rk - Vk falls monotonically with Vk (a
    // deeper bias means less current AND a larger subtrahend), so bisection
    // finds it for any cathode resistor.
    vk0_ = solveCathode(0.0f);
    vp0_ = plateVolts(-vk0_);
    // Small-signal gain from the source, numerically, through the whole
    // static path -- the input divider, the grid, the cathode as configured.
    const float dv = 0.01f;
    const float up = staticPlate(dv), down = staticPlate(-dv);
    gain_ = (up - down) / (2.0f * dv);
    norm_ = std::fabs(gain_) > 1e-3f ? 1.0f / gain_ : 1.0f;
    built_ = true;
    reset();
  }
  /** The plate voltage for a source voltage with every capacitor at rest. */
  float staticPlate(float vs) const {
    const float vg = vs * rg_ / (rg_ + rs_);
    const float vk = ck_ > 0.0f ? vk0_ : solveCathode(vg);
    return plateVolts(vg - vk);
  }

  Triode triode_{};
  float supply_ = 300.0f, rp_ = 100e3f, rk_ = 1.5e3f;
  float cin_ = 22e-9f, rg_ = 1e6f, rs_ = 38e3f, ck_ = 25e-6f;
  float table_[kPoints]{};
  float tableScale_ = 1.0f, vk0_ = 1.0f, vp0_ = 200.0f, gain_ = -60.0f;
  float inputGain_ = 1.0f, outputGain_ = 1.0f, norm_ = -1.0f / 60.0f;
  float sr_ = 48000.0f, dtOverCin_ = 0.0f, dtOverCk_ = 0.0f;
  float vc_ = 0.0f, vk_ = 1.0f, vg_ = 0.0f;
  bool built_ = false;
  DcBlocker blocker_;
};

// ── Tone stack ───────────────────────────────────────────────────────────────

/**
 * The Bassman stack: bass, mid and treble pots on a passive network of
 * three capacitors and four resistors. Knobs are 0..1 (the pot's travel);
 * everything at 0.5 gives the deep mid scoop the circuit is famous for, and
 * the knobs interact -- turning the bass up moves the mid notch -- because
 * that is what the network does. Passive, so it only ever cuts: the peak of
 * the response is a few dB below unity and the scoop is 20 dB under that.
 */
class ToneStack {
public:
  /** The component values. Bassman defaults; a Marshall or a Vox is the
   *  same topology with different parts. Resistances in ohms, capacitances
   *  in farads. */
  struct Components {
    float c1 = 250e-12f, c2 = 20e-9f, c3 = 20e-9f;
    float r1 = 250e3f, r2 = 1e6f, r3 = 25e3f, r4 = 56e3f;
  };

  void setSampleRate(float sr) {
    sr_ = sr > 1.0f ? sr : 48000.0f;
    update();
  }
  void setComponents(const Components& c) {
    parts_ = c;
    update();
  }
  void setBass(float v) {
    l_ = clampf(v, 0.0f, 1.0f);
    update();
  }
  void setMid(float v) {
    m_ = clampf(v, 0.0f, 1.0f);
    update();
  }
  void setTreble(float v) {
    t_ = clampf(v, 0.0f, 1.0f);
    update();
  }
  void reset() { z1_ = z2_ = z3_ = 0.0; }

  inline float process(float x) {
    // Transposed direct form II, third order, in double (see the header).
    const double in = x;
    const double y = b0_ * in + z1_;
    z1_ = b1_ * in - a1_ * y + z2_;
    z2_ = b2_ * in - a2_ * y + z3_;
    z3_ = b3_ * in - a3_ * y;
    return (float) y;
  }

  /** The analogue transfer function's coefficients, H(s) = (b1 s + b2 s² +
   *  b3 s³) / (1 + a1 s + a2 s² + a3 s³), for anyone who wants to check the
   *  digital response against the circuit. Filled by update(). */
  void analogCoefficients(double* b, double* a) const {
    b[0] = 0.0; b[1] = sb1_; b[2] = sb2_; b[3] = sb3_;
    a[0] = 1.0; a[1] = sa1_; a[2] = sa2_; a[3] = sa3_;
  }

  /** |H(s)| of the circuit in dB at an analogue frequency, from the same
   *  coefficients the digital filter was derived from. */
  double analogMagnitudeDb(double hz) const {
    const double w = 2.0 * 3.14159265358979323846 * hz;
    // s = jw: numerator = j b1 w - b2 w² - j b3 w³; denominator = 1 - a2 w² + j (a1 w - a3 w³).
    const double nr = -sb2_ * w * w, ni = sb1_ * w - sb3_ * w * w * w;
    const double dr = 1.0 - sa2_ * w * w, di = sa1_ * w - sa3_ * w * w * w;
    const double mag = std::sqrt((nr * nr + ni * ni) / (dr * dr + di * di));
    return 20.0 * std::log10(mag > 1e-12 ? mag : 1e-12);
  }

private:
  void update() {
    const double C1 = parts_.c1, C2 = parts_.c2, C3 = parts_.c3;
    const double R1 = parts_.r1, R2 = parts_.r2, R3 = parts_.r3, R4 = parts_.r4;
    const double l = l_, m = m_, t = t_;
    // Yeh & Smith, the symbolic solution of the network.
    const double b1 = t * C1 * R1 + m * C3 * R3 + l * (C1 * R2 + C2 * R2) + (C1 * R3 + C2 * R3);
    const double b2 = t * (C1 * C2 * R1 * R4 + C1 * C3 * R1 * R4) - m * m * (C1 * C3 * R3 * R3 + C2 * C3 * R3 * R3) +
                      m * (C1 * C3 * R1 * R3 + C1 * C3 * R3 * R3 + C2 * C3 * R3 * R3) +
                      l * (C1 * C2 * R1 * R2 + C1 * C2 * R2 * R4 + C1 * C3 * R2 * R4) +
                      l * m * (C1 * C3 * R2 * R3 + C2 * C3 * R2 * R3) +
                      (C1 * C2 * R1 * R3 + C1 * C2 * R3 * R4 + C1 * C3 * R3 * R4);
    const double b3 = l * m * (C1 * C2 * C3 * R1 * R2 * R3 + C1 * C2 * C3 * R2 * R3 * R4) -
                      m * m * (C1 * C2 * C3 * R1 * R3 * R3 + C1 * C2 * C3 * R3 * R3 * R4) +
                      m * (C1 * C2 * C3 * R1 * R3 * R3 + C1 * C2 * C3 * R3 * R3 * R4) +
                      t * C1 * C2 * C3 * R1 * R3 * R4 - t * m * C1 * C2 * C3 * R1 * R3 * R4 +
                      t * l * C1 * C2 * C3 * R1 * R2 * R4;
    const double a1 = (C1 * R1 + C1 * R3 + C2 * R3 + C2 * R4 + C3 * R4) + m * C3 * R3 + l * (C1 * R2 + C2 * R2);
    const double a2 = m * (C1 * C3 * R1 * R3 - C2 * C3 * R3 * R4 + C1 * C3 * R3 * R3 + C2 * C3 * R3 * R3) +
                      l * m * (C1 * C3 * R2 * R3 + C2 * C3 * R2 * R3) - m * m * (C1 * C3 * R3 * R3 + C2 * C3 * R3 * R3) +
                      l * (C1 * C2 * R2 * R4 + C1 * C2 * R1 * R2 + C1 * C3 * R2 * R4 + C2 * C3 * R2 * R4) +
                      (C1 * C2 * R1 * R4 + C1 * C3 * R1 * R4 + C1 * C2 * R3 * R4 + C1 * C2 * R1 * R3 + C1 * C3 * R3 * R4 +
                       C2 * C3 * R3 * R4);
    const double a3 = l * m * (C1 * C2 * C3 * R1 * R2 * R3 + C1 * C2 * C3 * R2 * R3 * R4) -
                      m * m * (C1 * C2 * C3 * R1 * R3 * R3 + C1 * C2 * C3 * R3 * R3 * R4) +
                      m * (C1 * C2 * C3 * R3 * R3 * R4 + C1 * C2 * C3 * R1 * R3 * R3 - C1 * C2 * C3 * R1 * R3 * R4) +
                      l * C1 * C2 * C3 * R1 * R2 * R4 + l * C1 * C2 * C3 * R2 * R3 * R4 + C1 * C2 * C3 * R1 * R3 * R4;
    sb1_ = b1; sb2_ = b2; sb3_ = b3; sa1_ = a1; sa2_ = a2; sa3_ = a3;

    // Bilinear transform, s = c (1 - z^-1)/(1 + z^-1), c = 2 fs, the cubic
    // multiplied through by (1 + z^-1)^3 and expanded:
    //   (1-z)(1+z)^2 = 1 + z - z^2 - z^3     (1-z)^2(1+z) = 1 - z - z^2 + z^3
    //   (1-z)^3      = 1 - 3z + 3z^2 - z^3   (1+z)^3      = 1 + 3z + 3z^2 + z^3
    const double c = 2.0 * (double) sr_, c2 = c * c, c3 = c2 * c;
    const double B0 = b1 * c + b2 * c2 + b3 * c3;
    const double B1 = b1 * c - b2 * c2 - 3.0 * b3 * c3;
    const double B2 = -b1 * c - b2 * c2 + 3.0 * b3 * c3;
    const double B3 = -b1 * c + b2 * c2 - b3 * c3;
    const double A0 = 1.0 + a1 * c + a2 * c2 + a3 * c3;
    const double A1 = 3.0 + a1 * c - a2 * c2 - 3.0 * a3 * c3;
    const double A2 = 3.0 - a1 * c - a2 * c2 + 3.0 * a3 * c3;
    const double A3 = 1.0 - a1 * c + a2 * c2 - a3 * c3;
    b0_ = B0 / A0; b1_ = B1 / A0; b2_ = B2 / A0; b3_ = B3 / A0;
    a1_ = A1 / A0; a2_ = A2 / A0; a3_ = A3 / A0;
  }

  Components parts_{};
  float sr_ = 48000.0f, l_ = 0.5f, m_ = 0.5f, t_ = 0.5f;
  double sb1_ = 0, sb2_ = 0, sb3_ = 0, sa1_ = 0, sa2_ = 0, sa3_ = 0;
  double b0_ = 1, b1_ = 0, b2_ = 0, b3_ = 0, a1_ = 0, a2_ = 0, a3_ = 0;
  double z1_ = 0, z2_ = 0, z3_ = 0;
};

} // namespace sonore
