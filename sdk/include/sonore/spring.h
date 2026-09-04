// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: a spring reverb.
//
// A spring is not a room. Its impulse response is a train of echoes, one per
// trip along the coil, and every echo is a CHIRP: the coil is dispersive, so
// the frequencies travel at different speeds and a click comes out as a
// sweep. That sweep is the "boing", and no delay-and-filter reverb makes it.
//
// The model is the dispersive waveguide of Abel, Berners, Costello & Smith
// ("Spring reverb emulation using dispersive allpass filters in a waveguide
// structure", AES 121, 2006), in the form Parker & Välimäki make cheap
// ("Efficient dispersion generation structures for spring reverb emulation",
// EURASIP JASP 2011): the dispersion is a long cascade of identical allpass
// sections inside a feedback loop with the coil's transit delay and a
// lowpass for the loss. Each pass through the loop disperses the pulse a
// little more, which is exactly what a real spring does to it.
//
// The sections are SECOND order, with their pole angle at the spring's
// transition frequency. The first draft used first-order allpasses, whose
// group delay grows monotonically toward Nyquist: 0.2 samples per stage at
// DC, 19 at Nyquist and 0.6 at 15 kHz -- a chirp that lives entirely in the
// last kilohertz below Nyquist, where nobody hears it. A helical spring's
// dispersion peaks at a transition frequency of a few kilohertz, and the
// audible "boing" is the sweep up to it. A second-order allpass with its
// poles at that frequency puts the group-delay peak there: at radius r the
// peak is about (1+r)/(1-r) samples per section, so 64 sections at r = 0.9
// hold the transition frequency back by ~1200 samples (25 ms at 48 kHz)
// while DC goes through almost at once. `tension` is the radius.
//
// Two coils with different lengths give a stereo pair that decorrelate the
// way two springs in a tank do. A short, lightly dispersed parallel path
// carries the high-frequency "wire" component that arrives ahead of the
// chirps.
//
// Included by dsp.h.
#pragma once
#include <cmath>
#include "audio.h"

namespace sonore {

template <int Stages = 64, int MaxDelay = 8192>
class SpringReverb {
  static_assert(Stages >= 4 && Stages <= 512, "4..512 allpass stages");

public:
  struct Parameters {
    float decay = 2.0f;           // seconds to -60 dB
    float tension = 0.6f;         // 0..1: how far the chirp spreads (pole radius 0.6..0.95)
    float transitionHz = 2500.0f; // where the dispersion peaks: the spring's transition frequency
    float resonanceDb = 3.0f;     // the spring's own resonance at that frequency, on the output
    float damping = 0.4f;         // 0..1: high-frequency loss per trip
    float size = 0.5f;            // 0..1: transit time, ~30..70 ms
    float mix = 0.3f;
  };

  void prepare(const ProcessSpec& spec) {
    sr_ = (float) spec.sampleRate;
    for (int c = 0; c < 2; ++c) {
      damp_[c].setSampleRate(sr_);
      wireDamp_[c].setSampleRate(sr_);
      resonance_[c].setSampleRate(sr_);
    }
    setParameters(params_);
    reset();
  }
  void reset() {
    for (int c = 0; c < 2; ++c) {
      line_[c].reset();
      wire_[c].reset();
      damp_[c].reset();
      wireDamp_[c].reset();
      resonance_[c].reset();
      for (int s = 0; s < Stages; ++s) ap1_[c][s] = ap2_[c][s] = 0.0f;
    }
  }

  void setParameters(const Parameters& p) {
    params_ = p;
    // Transit time: two coils, the right one a little longer so the echoes
    // interleave. Clamped so the line always has room.
    const float baseMs = 30.0f + clampf(p.size, 0.0f, 1.0f) * 40.0f;
    transit_[0] = clampf(baseMs * 0.001f * sr_, 8.0f, (float) (MaxDelay - 8));
    transit_[1] = clampf(baseMs * 1.13f * 0.001f * sr_, 8.0f, (float) (MaxDelay - 8));
    wireTransit_[0] = clampf(transit_[0] * 0.23f, 4.0f, (float) (MaxDelay - 8));
    wireTransit_[1] = clampf(transit_[1] * 0.19f, 4.0f, (float) (MaxDelay - 8));
    // The dispersive sections: poles at the transition frequency, radius
    // from the tension. Allpass, so the loop gain is the feedback alone.
    const float r = 0.6f + clampf(p.tension, 0.0f, 1.0f) * 0.35f;
    const float fc = clampf(p.transitionHz, 200.0f, sr_ * 0.4f);
    const float theta = 2.0f * kPi * fc / sr_;
    apA1_ = -2.0f * r * std::cos(theta);
    apA2_ = r * r;
    // The spring rings at its transition frequency (Välimäki, Parker & Abel,
    // "Parametric spring reverberation effect", JAES 2010, shape the spectrum
    // with a resonance there). On the OUTPUT path, not in the loop: a peak
    // inside the loop raises the loop gain at fc and a 2 s decay at +3 dB
    // would not decay at all.
    for (int c = 0; c < 2; ++c) resonance_[c].peak(fc, 1.5f, clampf(p.resonanceDb, 0.0f, 12.0f));
    // Decay: -60 dB in `decay` seconds, per trip of the transit time.
    const float t60 = p.decay > 0.05f ? p.decay : 0.05f;
    for (int c = 0; c < 2; ++c) feedback_[c] = std::pow(10.0f, -3.0f * transit_[c] / (t60 * sr_));
    const float d = clampf(p.damping, 0.0f, 1.0f);
    const float cutoff = 12000.0f * std::pow(1500.0f / 12000.0f, d);
    for (int c = 0; c < 2; ++c) {
      damp_[c].setCutoff(cutoff);
      wireDamp_[c].setCutoff(cutoff * 1.5f);
    }
    mixer_.setMix(clampf(p.mix, 0.0f, 1.0f));
  }

  int tailSamples(float floorDb = -80.0f) const {
    const float t60 = params_.decay > 0.05f ? params_.decay : 0.05f;
    return (int) (t60 * (-floorDb / 60.0f) * sr_) + MaxDelay;
  }

  /** The chain's group delay at a frequency, in samples: what the
   *  dispersion does to a partial, from the section's own formula. */
  float chainGroupDelay(float hz) const {
    const float r = std::sqrt(apA2_);
    const float theta = std::acos(clampf(-apA1_ / (2.0f * r), -1.0f, 1.0f));
    const float w = 2.0f * kPi * hz / sr_;
    const float one = (1.0f - r * r) / (1.0f - 2.0f * r * std::cos(w - theta) + r * r);
    const float two = (1.0f - r * r) / (1.0f - 2.0f * r * std::cos(w + theta) + r * r);
    return (float) Stages * (one + two);
  }

  /** One stereo frame, in place. Mono in (the sum), two coils out. */
  inline void process(float& left, float& right) {
    const float dryL = left, dryR = right;
    const float in = (dryL + dryR) * 0.5f;
    float wet[2];
    for (int c = 0; c < 2; ++c) {
      // The dispersive loop: read the coil's far end, lose some top, run
      // the pulse through the allpass chain, and send it back.
      float v = line_[c].read(transit_[c]);
      v = damp_[c].lp(v);
      v = disperse(c, v);
      line_[c].write(flushDenormal(in + v * feedback_[c]));
      // The wire path: short, bright, barely dispersed.
      const float w = wireDamp_[c].lp(wire_[c].read(wireTransit_[c]));
      wire_[c].write(flushDenormal(in * 0.5f + w * feedback_[c] * 0.6f));
      wet[c] = resonance_[c].process(v + 0.3f * w);
    }
    left = mixer_.process(dryL, wet[0]);
    right = mixer_.process(dryR, wet[1]);
  }

  /** The dispersive chain on its own, for measuring it. */
  inline float disperse(int c, float x) {
    // Second-order allpass (a2 + a1 z^-1 + z^-2) / (1 + a1 z^-1 + a2 z^-2),
    // transposed direct form II, Stages times.
    const float a1 = apA1_, a2 = apA2_;
    float* s1 = ap1_[c];
    float* s2 = ap2_[c];
    for (int s = 0; s < Stages; ++s) {
      const float y = a2 * x + s1[s];
      s1[s] = a1 * x - a1 * y + s2[s];
      s2[s] = flushDenormal(x - a2 * y);
      x = y;
    }
    return x;
  }

private:
  DelayLine<MaxDelay> line_[2], wire_[2];
  OnePole damp_[2], wireDamp_[2];
  Biquad resonance_[2];
  float ap1_[2][Stages]{}, ap2_[2][Stages]{};
  float transit_[2] = {2400.0f, 2712.0f}, wireTransit_[2] = {552.0f, 515.0f};
  float feedback_[2] = {0.9f, 0.9f}, apA1_ = -1.7f, apA2_ = 0.81f, sr_ = 48000.0f;
  Parameters params_{};
  DryWetMixer mixer_;
};

} // namespace sonore
