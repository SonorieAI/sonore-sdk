// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: the diode clipper, solved.
//
// The heart of every overdrive and distortion pedal is two diodes back to
// back across a capacitor, fed through a resistor: the Tube Screamer's
// feedback loop, the DS-1's output stage, the Klon's. Asked for one, a model
// writes tanh. A diode clipper is not tanh -- its knee is exponential rather
// than polynomial, and because the diodes sit across a capacitor the CORNER
// of the circuit moves with drive: the harder the diodes conduct, the lower
// their dynamic resistance and the more of the top they shunt, so a hot
// signal comes out darker as well as flatter.
//
// The model is Yeh, Abel & Smith's ("Simulation of the diode limiter in
// guitar distortion circuits by numerical solution of ordinary differential
// equations", DAFx 2007): the circuit's one state equation,
//
//     C dv/dt = (vin - v)/R - 2 Is sinh(v / (n Vt)),
//
// integrated by the trapezoidal rule with a Newton solve at each step -- the
// method that paper finds stable where explicit ones are not -- at an
// oversampled rate, because the exponential makes harmonics well past
// Nyquist. Their component values are the defaults: R = 2.2 kOhm, C = 10 nF,
// 1N914 diodes (Is = 2.52 nA, n = 1.752). Asymmetric clipping is the pedal
// trick of unequal diode counts each way, which the sinh splits into two
// exponentials with different scales.
//
// The input is in VOLTS: a guitar is a tenth of a volt, the op-amp before a
// clipper makes it several. setInputGain is that op-amp.
//
// Included by dsp.h. Uses Oversampled<> from lofi.h.
#pragma once
#ifndef SONORE_DSP_H_INCLUDED
#error "clipper.h is part of dsp.h and needs what it includes first: include <sonore/dsp.h> instead."
#endif
#include <cmath>
#include "audio.h"

namespace sonore {

/**
 * The clipper alone, at whatever rate it is told. Run it inside
 * Oversampled<> -- or use OversampledDiodeClipper below, which does that and
 * keeps the rates straight.
 */
class DiodeClipper {
public:
  /** The rate this clipper runs at, in Hz: the OVERSAMPLED rate when it sits
   *  inside Oversampled<> (1 or less is taken as 48000). It sets only the
   *  integration step; the capacitor's state is kept. */
  void setSampleRate(float sr) { dt_ = 1.0f / (sr > 1.0f ? sr : 48000.0f); }
  /** Series resistance in ohms, shunt capacitance in farads. */
  void setCircuit(float ohms, float farads) {
    r_ = ohms > 1.0f ? ohms : 1.0f;
    c_ = farads > 1e-12f ? farads : 1e-12f;
  }
  /** Saturation current (A), emission coefficient, diodes in series each way. */
  void setDiodes(float saturationAmps, float emission, int forward = 1, int reverse = 1) {
    is_ = saturationAmps > 1e-15f ? saturationAmps : 1e-15f;
    n_ = emission > 0.5f ? emission : 0.5f;
    nf_ = forward < 1 ? 1 : forward;
    nr_ = reverse < 1 ? 1 : reverse;
  }
  /** The op-amp before the clipper, as a plain gain: the input sample times
   *  `g` is the voltage driving the network. 1 by default; a negative gain is
   *  taken as 0. */
  void setInputGain(float g) { gain_ = g < 0.0f ? 0.0f : g; }
  /** Discharges the capacitor: the output and the remembered input back to
   *  0 V. */
  void reset() { v_ = 0.0f; vin1_ = 0.0f; }

  /** One sample at this clipper's rate: `x` times the input gain, in volts,
   *  in; the capacitor's voltage, in volts, out. At most twelve Newton steps
   *  a sample and no allocation, so it is safe on the audio thread. */
  inline float process(float x) {
    const float vin = x * gain_;
    // Trapezoidal: v1 = v0 + (dt/2)(f(v0, vin0) + f(v1, vin1)), solved for v1
    // as the root of g(v) = v - v0 - (dt/2)(f0 + f(v, vin1)).
    //
    // The solve is SAFEGUARDED, and that is a numerical decision, not a change
    // to the model. g is strictly increasing (its slope is 1 + (dt/2C)(1/R +
    // the diodes' conductance), never below 1), and without the diodes it is a
    // straight line whose root vLin is closed-form. The diodes' current always
    // has the sign of v, so it can only pull the root from vLin toward zero:
    // the root is ALWAYS between 0 and vLin. Newton runs inside that bracket,
    // starts from the previous solution -- right to within one sample's motion
    // at the oversampled rate -- and bisects whenever a step would leave it.
    //
    // It used to start from an explicit Euler predictor and trust Newton from
    // there. Driven hard at 384 kHz (a 10 V sine at 10 kHz, a -6 dBFS saw at
    // input gain 20) the predictor threw v past the point where the
    // exponential is capped; beyond it the current went flat while the slope
    // it reported did not, so every Newton step crept by a fraction of a
    // millivolt, the iterations ran out, and the next predictor threw further.
    // The output alternated around +/-8e10 V and stayed there through half a
    // second of silence.
    const float f0 = rate(v_, vin1_);
    const float k = 0.5f * dt_ / (r_ * c_);
    const float vLin = (v_ + 0.5f * dt_ * f0 + k * vin) / (1.0f + k);
    float lo = vLin < 0.0f ? vLin : 0.0f, hi = vLin < 0.0f ? 0.0f : vLin;
    float v = clampf(v_, lo, hi);
    for (int i = 0; i < kNewton; ++i) {
      float f1, df1;
      rateAndSlope(v, vin, f1, df1);
      const float g = v - v_ - 0.5f * dt_ * (f0 + f1);
      if (g > 0.0f) hi = v;
      else lo = v;
      const float dg = 1.0f - 0.5f * dt_ * df1;
      float next = v - g / dg;
      // Converged BEFORE the bracket is consulted. At the root, g's sign is
      // rounding noise: it can make v itself the bracket's end, so a Newton
      // step of nothing lands ON the end, fails a strict containment test,
      // and a bisection throws away a solution that was already exact.
      if (std::fabs(next - v) < 1e-6f) {
        v = next;
        break;
      }
      // A step OUTWARD up a conducting junction moves by its logarithm, not
      // its size -- SPICE's junction limiting. Newton's tangent at a point low
      // on the exponential aims volts past a root that is millivolts away;
      // the logarithm lands near it instead of somewhere the bisection then
      // has to walk back from.
      const float scale = n_ * kVt * (float) (next > 0.0f ? nf_ : nr_);
      const float out = std::fabs(next) - std::fabs(v);
      if (out > 2.0f * scale && std::fabs(next) > 10.0f * scale) {
        const float limited = std::fabs(v) + scale * std::log(1.0f + out / scale);
        next = next > 0.0f ? limited : -limited;
      }
      if (!(next >= lo && next <= hi)) next = 0.5f * (lo + hi);
      v = next;
    }
    v_ = flushDenormal(v);
    vin1_ = vin;
    return v_;
  }

private:
  static constexpr int kNewton = 12;
  static constexpr float kVt = 0.02585f; // thermal voltage at 300 K

  /** e^x, continued past x = 40 as its own TANGENT, so the value and the slope
   *  stay one function. A float overflows not far beyond; the old cap held the
   *  value at e^40 while the slope went on reporting it, and Newton, handed a
   *  slope its residual did not have, could not get back from there. Below -40
   *  the term is ~4e-18 of the other and is held, slope zero, which is also
   *  consistent. */
  static inline float expTangent(float x, float& d) {
    // A literal, not a function-local static: this runs on the audio thread,
    // and a static's first use takes a lock.
    constexpr float kE40 = 2.35385267e17f; // e^40
    if (x > 40.0f) {
      d = kE40;
      return kE40 * (1.0f + (x - 40.0f));
    }
    if (x < -40.0f) {
      d = 0.0f;
      return 4.248354e-18f; // e^-40
    }
    const float e = std::exp(x);
    d = e;
    return e;
  }

  /** The diodes' current, and its slope with respect to v. */
  inline float diodeCurrent(float v, float& slope) const {
    const float sf = 1.0f / (n_ * kVt * (float) nf_), sr = 1.0f / (n_ * kVt * (float) nr_);
    float def = 0.0f, der = 0.0f;
    const float ef = expTangent(v * sf, def), er = expTangent(-v * sr, der);
    slope = is_ * (sf * def + sr * der);
    return is_ * (ef - er);
  }
  inline float rate(float v, float vin) const {
    float slope;
    return ((vin - v) / r_ - diodeCurrent(v, slope)) / c_;
  }
  inline void rateAndSlope(float v, float vin, float& f, float& df) const {
    float slope;
    const float id = diodeCurrent(v, slope);
    f = ((vin - v) / r_ - id) / c_;
    df = (-1.0f / r_ - slope) / c_;
  }

  float dt_ = 1.0f / 48000.0f, r_ = 2200.0f, c_ = 10e-9f, is_ = 2.52e-9f, n_ = 1.752f, gain_ = 1.0f;
  int nf_ = 1, nr_ = 1;
  float v_ = 0.0f, vin1_ = 0.0f;
};

/**
 * The clipper at 2^Stages times the rate, with the cascade's latency. 8x is
 * the paper's recommendation and the default.
 */
template <int Stages = 3>
class OversampledDiodeClipper {
public:
  /** The oversampling cascade's delay at the host rate, in whole samples:
   *  the latency to report. */
  static constexpr int latencySamples() { return Oversampled<DiodeClipper, Stages>::latencySamples(); }
  /** The host rate in Hz; the clipper inside runs at 2^Stages times it. Its
   *  state is kept: call reset() for a clean start. */
  void setSampleRate(float sr) { os_.shaper().setSampleRate(sr * (float) Oversampled<DiodeClipper, Stages>::factor()); }
  /** The clipper inside, for setCircuit(), setDiodes() and setInputGain().
   *  Leave its rate alone: setSampleRate() here sets it to the oversampled
   *  one. */
  DiodeClipper& clipper() { return os_.shaper(); }
  /** Clears the oversampling filters and discharges the clipper. */
  void reset() { os_.reset(); }
  /** One host-rate sample, in volts as DiodeClipper::process() takes them,
   *  through the oversampled clipper. No allocation. */
  inline float process(float x) { return os_.process(x); }

private:
  Oversampled<DiodeClipper, Stages> os_;
};

} // namespace sonore
