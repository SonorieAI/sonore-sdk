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
#ifndef SONORE_DSP_H_INCLUDED
#error "spring.h is part of dsp.h and needs what it includes first: include <sonore/dsp.h> instead."
#endif
#include <cmath>
#include "audio.h"

namespace sonore {

/**
 * A spring reverb tank: two dispersive coils, each a feedback loop of a
 * transit delay, a lowpass and `Stages` second-order allpasses, so every
 * echo comes back as the chirp that makes the "boing", plus a short bright
 * "wire" path ahead of them (see the file comment for the model).
 *
 * Stereo in and out, the input summed to mono. Call prepare() before audio;
 * it applies the parameters for the rate and clears the coils. A coil is
 * at most MaxDelay samples, so a long spring at a high rate shrinks both
 * coils together; transitSamples() says what each got. setParameters() and
 * process() run on the audio thread, and a change lands at once, the decay
 * and the coil lengths unsmoothed. Report tailSamples() to the host.
 */
template <int Stages = 64, int MaxDelay = 8192>
class SpringReverb {
  static_assert(Stages >= 4 && Stages <= 512, "4..512 allpass stages");

public:
  /** The tank's settings, set together with setParameters(): decay as a T60
   *  in seconds (at least 0.05); tension 0..1, how far the chirp spreads;
   *  the transition frequency in Hz (200 Hz up to 0.4 of the rate) and the
   *  spring's resonance there, 0..12 dB on the output; damping 0..1, the
   *  loop's lowpass from 12 kHz down to 1.5 kHz; size 0..1, a transit of
   *  about 30 to 70 ms; mix 0..1. */
  struct Parameters {
    float decay = 2.0f;           ///< seconds to -60 dB, 0.05 at the least
    float tension = 0.6f;         ///< 0..1: how far the chirp spreads (pole radius 0.6..0.95 at 48 kHz)
    float transitionHz = 2500.0f; ///< where the dispersion peaks, Hz: the spring's transition frequency
    float resonanceDb = 3.0f;     ///< the spring's own resonance at that frequency, 0..12 dB, on the output
    float damping = 0.4f;         ///< 0..1: high-frequency loss per trip, a lowpass from 12 kHz to 1.5 kHz
    float size = 0.5f;            ///< 0..1: transit time, ~30..70 ms; the right coil is 1.13x the left
    float mix = 0.3f;             ///< 0 dry only, 1 wet only, equal-power between; a change glides
  };

  /** Sets the rate, applies the last Parameters again for it and clears the
   *  tank as reset() does. Nothing is allocated: the coils are fixed arrays.
   *  The channel count is not read: the tank is always stereo. */
  void prepare(const ProcessSpec& spec) {
    sr_ = (float) spec.sampleRate;
    for (int c = 0; c < 2; ++c) {
      damp_[c].setSampleRate(sr_);
      wireDamp_[c].setSampleRate(sr_);
      resonance_[c].setSampleRate(sr_);
    }
    for (DryWetMixer& m : mixer_) m.setSampleRate(sr_);
    setParameters(params_);
    reset();
  }
  /** Silence: clears both coils, the wire paths, their filters and every
   *  allpass section, and lands the mix on its setting. */
  void reset() {
    for (DryWetMixer& m : mixer_) m.snap();
    for (int c = 0; c < 2; ++c) {
      line_[c].reset();
      wire_[c].reset();
      damp_[c].reset();
      wireDamp_[c].reset();
      resonance_[c].reset();
      for (int s = 0; s < Stages; ++s) ap1_[c][s] = ap2_[c][s] = 0.0f;
    }
  }

  /** Stores the settings (the ranges are on Parameters) and works out the
   *  coil lengths, the dispersion, the resonance and the loop gains for the
   *  rate. Everything but the mix lands at once; the mix glides once process()
   *  has run. No allocation, but it calls pow, exp and cos: once a block, not
   *  per sample. */
  void setParameters(const Parameters& p) {
    params_ = p;
    // Transit time: two coils, the right one a little longer so the echoes
    // interleave -- the difference IS the stereo. When the line is too short
    // for the rate (a 70 ms coil is 15,000 samples at 192 kHz), BOTH shrink
    // by the same factor. Clamped one at a time, from about a third of the
    // size range at 192 kHz both coils came out the same 8184 samples, and
    // the reverb was mono.
    const float baseMs = 30.0f + clampf(p.size, 0.0f, 1.0f) * 40.0f;
    float left = baseMs * 0.001f * sr_, right = left * 1.13f;
    const float room = (float) (MaxDelay - 8);
    if (right > room) {
      left *= room / right;
      right = room;
    }
    transit_[0] = clampf(left, 8.0f, room);
    transit_[1] = clampf(right, 8.0f, room);
    wireTransit_[0] = clampf(transit_[0] * 0.23f, 4.0f, (float) (MaxDelay - 8));
    wireTransit_[1] = clampf(transit_[1] * 0.19f, 4.0f, (float) (MaxDelay - 8));
    // The dispersive sections: poles at the transition frequency, radius
    // from the tension. Allpass, so the loop gain is the feedback alone.
    //
    // The radius is a BANDWIDTH in hertz, set so 48 kHz keeps the radius
    // the tension always gave there. Fixed in the z-plane it meant a fixed
    // fraction of the rate, so the chirp shrank as the rate rose -- its
    // group delay at 2.5 kHz 15.0 ms at 44.1 kHz, 5.2 at 192 -- and from
    // 96 kHz the high partials arrived BEFORE the low ones: the spring's
    // sound turned round.
    const float r48 = 0.6f + clampf(p.tension, 0.0f, 1.0f) * 0.35f;
    const float bandwidthHz = -std::log(r48) * 48000.0f / kPi;
    const float r = std::exp(-kPi * bandwidthHz / sr_);
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
    for (DryWetMixer& m : mixer_) m.setMix(clampf(p.mix, 0.0f, 1.0f));
  }

  /** One coil's transit time in samples (0 = left, 1 = right). */
  float transitSamples(int coil) const { return transit_[coil == 1 ? 1 : 0]; }

  /** How long the tank rings, in samples, for the host's tail report: the
   *  decay's time to `floorDb` (20 dB below the T60 by default) plus a whole
   *  coil, capped at INT_MAX. */
  int tailSamples(float floorDb = -80.0f) const {
    // In double and capped, as the FDN's: a long enough decay ran the float
    // product past INT_MAX, which is undefined as an int.
    const double t60 = params_.decay > 0.05f ? (double) params_.decay : 0.05;
    const double samples = t60 * (-floorDb / 60.0) * sr_ + MaxDelay;
    return samples < 2147483647.0 ? (int) samples : 2147483647;
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
    left = mixer_[0].process(dryL, wet[0]);
    right = mixer_[1].process(dryR, wet[1]);
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
      s1[s] = flushDenormal(a1 * x - a1 * y + s2[s]);
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
  /** One mix glide per side, set to the rate in prepare(). One shared by
   *  both stepped twice a frame, gliding in half its time, and was never
   *  given a rate. */
  DryWetMixer mixer_[2];
};

} // namespace sonore
