// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: the two other synth filters people ask for by name.
//
// dsp.h has the Moog ladder. The next two requests are always "a 303 filter"
// and "an MS-20 filter", and they are not the Moog with different numbers:
//
//   DiodeLadder    the TB-303's four-pole. The stages are coupled through
//                  diodes rather than buffered, so each pole sees the one
//                  after it -- which is why its resonance is thinner and its
//                  cutoff slides as the resonance rises. Zavalishin's
//                  zero-delay-feedback solution of that coupled ladder
//                  ("The Art of VA Filter Design"), in the form Pirkle sets
//                  out ("Designing Software Synthesizer Plug-Ins in C++"):
//                  each one-pole carries its neighbour's feedback and the
//                  whole thing is solved as one implicit equation.
//   SallenKeyFilter the Korg35 that the MS-20 is built on: a two-pole
//                  Sallen-Key with the resonance taken from a HIGH-pass in
//                  the feedback loop, and the diode clipper in that loop
//                  that makes its scream. Zero-delay-feedback again, derived
//                  below from the one-pole's split into "what depends on
//                  this input" and "what was already there".
//
// Both keep the LINEAR core exact (TPT / ZDF -- the cutoff is where it is
// set, at any resonance, at any rate) and apply the nonlinearity to the
// solved loop signal, which is the standard trade every shipping VA filter
// makes; the alternative is a Newton loop per sample.
//
// Included by dsp.h.
#pragma once
#include <cmath>
#include "audio.h"
#include "fast_math.h"

namespace sonore {

// ── Diode ladder ─────────────────────────────────────────────────────────────

/**
 * The cutoff control sets the one-pole sections' corner, as in the circuit;
 * the FILTER's corner is somewhere else, because the sections are coupled.
 * Measured with the control at 1 kHz: -4.5 dB at 100 Hz and -31 dB at 1 kHz
 * with no resonance (the -3 dB point is about three octaves below the
 * setting), and an 18 dB peak at 653 Hz with the resonance at 0.8 -- the
 * corner slides up toward the setting as the resonance rises, which is the
 * 303's behaviour and the reason its sweeps sound the way they do. A plugin
 * that wants a Moog-like corner at the number on the knob should scale the
 * control up, and say so.
 */
class DiodeLadder {
public:
  void setSampleRate(float sr) {
    sr_ = sr > 1.0f ? sr : 48000.0f;
    update();
  }
  void setCutoff(float hz) {
    cutoff_ = clampf(hz, 20.0f, sr_ * 0.45f);
    update();
  }
  /** 0..1. Self-oscillation arrives at the top (K = 17). */
  void setResonance(float r) { k_ = clampf(r, 0.0f, 1.0f) * 17.0f; }
  /** Input drive into the ladder's tanh; 0 = linear. */
  void setSaturation(float s) { sat_ = clampf(s, 0.0f, 4.0f); }
  void reset() {
    for (auto& s : stage_) s.reset();
  }

  inline float process(float x) {
    // Each stage's feedback is the NEXT stage's state, top down.
    stage_[3].feedback = 0.0f;
    stage_[2].feedback = stage_[3].feedbackOut();
    stage_[1].feedback = stage_[2].feedbackOut();
    stage_[0].feedback = stage_[1].feedbackOut();
    // Everything already in the ladder that reaches the output this sample
    // without new input, weighted by the chain of stage gains.
    const float sigma = sg_[0] * stage_[0].feedbackOut() + sg_[1] * stage_[1].feedbackOut() +
                        sg_[2] * stage_[2].feedbackOut() + sg_[3] * stage_[3].feedbackOut();
    // The implicit equation for the ladder input, solved.
    float u = (x - k_ * sigma) / (1.0f + k_ * gammaAll_);
    if (sat_ > 0.0f) u = fastTanh(u * sat_) / sat_;
    return stage_[3].process(stage_[2].process(stage_[1].process(stage_[0].process(u))));
  }

private:
  /** A TPT one-pole with the extra inputs the diode ladder needs: a feedback
   *  term, and coefficients that make it part of a coupled chain. */
  struct Stage {
    float alpha = 0.0f, beta = 0.0f, gamma = 1.0f, delta = 0.0f, epsilon = 0.0f, a0 = 1.0f;
    float z1 = 0.0f, feedback = 0.0f;

    inline float feedbackOut() const { return beta * (z1 + feedback * delta); }
    inline float process(float x) {
      x = x * gamma + feedback + epsilon * feedbackOut();
      const float vn = (a0 * x - z1) * alpha;
      const float lp = vn + z1;
      z1 = flushDenormal(vn + lp);
      return lp;
    }
    void reset() {
      z1 = 0.0f;
      feedback = 0.0f;
    }
  };

  void update() {
    const float g = std::tan(kPi * cutoff_ / sr_);
    const float G4 = 0.5f * g / (1.0f + g);
    const float G3 = 0.5f * g / (1.0f + g - 0.5f * g * G4);
    const float G2 = 0.5f * g / (1.0f + g - 0.5f * g * G3);
    const float G1 = g / (1.0f + g - g * G2);
    gammaAll_ = G4 * G3 * G2 * G1;
    sg_[0] = G4 * G3 * G2;
    sg_[1] = G4 * G3;
    sg_[2] = G4;
    sg_[3] = 1.0f;
    const float alpha = g / (1.0f + g);
    for (auto& s : stage_) s.alpha = alpha;
    stage_[0].beta = 1.0f / (1.0f + g - g * G2);
    stage_[1].beta = 1.0f / (1.0f + g - 0.5f * g * G3);
    stage_[2].beta = 1.0f / (1.0f + g - 0.5f * g * G4);
    stage_[3].beta = 1.0f / (1.0f + g);
    stage_[0].delta = g;
    stage_[1].delta = 0.5f * g;
    stage_[2].delta = 0.5f * g;
    stage_[3].delta = 0.0f;
    stage_[0].epsilon = G2;
    stage_[1].epsilon = G3;
    stage_[2].epsilon = G4;
    stage_[3].epsilon = 0.0f;
    stage_[0].a0 = 1.0f;
    stage_[1].a0 = 0.5f;
    stage_[2].a0 = 0.5f;
    stage_[3].a0 = 0.5f;
    stage_[0].gamma = 1.0f + G1 * G2;
    stage_[1].gamma = 1.0f + G2 * G3;
    stage_[2].gamma = 1.0f + G3 * G4;
    stage_[3].gamma = 1.0f;
  }

  Stage stage_[4];
  float sg_[4]{}, gammaAll_ = 0.0f;
  float sr_ = 48000.0f, cutoff_ = 1000.0f, k_ = 0.0f, sat_ = 0.0f;
};

// ── Sallen-Key (Korg35) ──────────────────────────────────────────────────────

/**
 * The linear response is 1 / (s² + (2 − K) s + 1) at the cutoff: K is the
 * resonance, and at K = 2 the damping is gone and it oscillates. Lowpass and
 * highpass are two different circuits on the MS-20 and two modes here.
 *
 * The derivation the code follows, for the lowpass: with G = g/(1+g), a TPT
 * one-pole's output is G·x + (1−G)·s, so its "already there" part is
 * (1−G)·s, and a one-pole highpass's is −(1−G)·s. The loop is
 * u = y1 + K·HP(LP2(u)); expanding LP2 and HP into their input-dependent and
 * state parts and solving for u gives u = (y1 + S) / (1 − K·G·(1−G)), with
 * S = K·(1−G)·((1−G)·s2 − sH). Zero delay: the current input is inside.
 */
class SallenKeyFilter {
public:
  enum class Mode { Lowpass, Highpass };

  void setSampleRate(float sr) {
    sr_ = sr > 1.0f ? sr : 48000.0f;
    update();
  }
  void setMode(Mode m) { mode_ = m; }
  void setCutoff(float hz) {
    cutoff_ = clampf(hz, 20.0f, sr_ * 0.45f);
    update();
  }
  /** 0..1 maps onto K = 0..1.99; it screams at the top. */
  void setResonance(float r) { k_ = clampf(r, 0.0f, 1.0f) * 1.99f; }
  /** The diode clipper in the resonance loop, 0 = linear. */
  void setSaturation(float s) { sat_ = clampf(s, 0.0f, 4.0f); }
  void reset() { s1_ = s2_ = sH_ = 0.0f; }

  inline float process(float x) {
    const float G = G_, oneMinusG = 1.0f - G_;
    if (mode_ == Mode::Lowpass) {
      // First one-pole, straight through.
      const float y1 = lp(x, s1_);
      // The loop: solve for its input, clip, then run the two filters in it.
      const float S = k_ * oneMinusG * (oneMinusG * s2_ - sH_);
      float u = (y1 + S) / (1.0f - k_ * G * oneMinusG);
      if (sat_ > 0.0f) u = fastTanh(u * sat_) / sat_;
      const float y = lp(u, s2_);
      (void) hp(y, sH_);
      return y;
    }
    // Highpass: the same structure mirrored -- HP first, loop of HP and LP.
    const float y1 = hp(x, sH_);
    const float S = k_ * oneMinusG * (s1_ - G * s2_);
    float u = (y1 + S) / (1.0f - k_ * G * oneMinusG);
    if (sat_ > 0.0f) u = fastTanh(u * sat_) / sat_;
    const float y = hp(u, s2_);
    (void) lp(y, s1_);
    return y;
  }

private:
  inline float lp(float x, float& s) const {
    const float v = (x - s) * G_;
    const float y = v + s;
    s = flushDenormal(y + v);
    return y;
  }
  inline float hp(float x, float& s) const { return x - lp(x, s); }
  void update() {
    const float g = std::tan(kPi * cutoff_ / sr_);
    G_ = g / (1.0f + g);
  }

  Mode mode_ = Mode::Lowpass;
  float sr_ = 48000.0f, cutoff_ = 1000.0f, G_ = 0.0f, k_ = 0.0f, sat_ = 0.0f;
  float s1_ = 0.0f, s2_ = 0.0f, sH_ = 0.0f;
};

} // namespace sonore
