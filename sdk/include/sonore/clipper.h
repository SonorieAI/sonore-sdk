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
  void setInputGain(float g) { gain_ = g < 0.0f ? 0.0f : g; }
  void reset() { v_ = 0.0f; vin1_ = 0.0f; }

  inline float process(float x) {
    const float vin = x * gain_;
    // Trapezoidal: v1 = v0 + (dt/2)(f(v0, vin0) + f(v1, vin1)), Newton on v1.
    const float f0 = rate(v_, vin1_);
    float v = v_ + dt_ * f0; // explicit predictor
    for (int i = 0; i < kNewton; ++i) {
      float f1, df1;
      rateAndSlope(v, vin, f1, df1);
      const float g = v - v_ - 0.5f * dt_ * (f0 + f1);
      const float dg = 1.0f - 0.5f * dt_ * df1;
      const float step = g / dg;
      v -= step;
      if (std::fabs(step) < 1e-6f) break;
    }
    v_ = flushDenormal(v);
    vin1_ = vin;
    return v_;
  }

private:
  static constexpr int kNewton = 8;
  static constexpr float kVt = 0.02585f; // thermal voltage at 300 K

  /** dv/dt. The exponents are capped where a float would overflow: past
   *  40 thermal voltages the diode is a wire and the slope is what matters. */
  inline float diodeCurrent(float v, float& slope) const {
    const float sf = 1.0f / (n_ * kVt * (float) nf_), sr = 1.0f / (n_ * kVt * (float) nr_);
    const float ef = std::exp(clampf(v * sf, -40.0f, 40.0f)), er = std::exp(clampf(-v * sr, -40.0f, 40.0f));
    slope = is_ * (sf * ef + sr * er);
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
  static constexpr int latencySamples() { return Oversampled<DiodeClipper, Stages>::latencySamples(); }
  void setSampleRate(float sr) { os_.shaper().setSampleRate(sr * (float) Oversampled<DiodeClipper, Stages>::factor()); }
  DiodeClipper& clipper() { return os_.shaper(); }
  void reset() { os_.reset(); }
  inline float process(float x) { return os_.process(x); }

private:
  Oversampled<DiodeClipper, Stages> os_;
};

} // namespace sonore
