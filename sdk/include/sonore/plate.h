// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: the Dattorro plate.
//
// Jon Dattorro, "Effect Design, Part 1: Reverberator and Other Filters"
// (JAES 45(9), 1997): the plate-class reverberator in the figure-of-eight:
// a predelay, a bandwidth one-pole, four input diffusers, then a TANK of
// two cross-coupled halves, each one modulated-allpass -> delay -> damping
// -> decay -> allpass -> delay, with the stereo outputs assembled from
// SEVEN taps each into the opposite half's lines (the paper's Table 2).
// It is the topology behind half the digital plates in commercial use, and
// it earns its place beside fdn.h and spring.h because its sound (fast
// even density, metallic-smooth tail) comes from this exact wiring, not
// from a parameter set another topology could copy.
//
// Faithfulness, stated precisely:
//   * every delay length and output tap is the paper's figure, scaled by
//     sr / 29761 (the paper's rate), so the plate keeps its SIZE in
//     seconds at any sample rate;
//   * decay is applied where the figure applies it (after each half's
//     damping filter, and again on the cross-feed into the opposite half),
//     so the loop loses decay^2 per half-lap of ~0.36 s, which is the
//     T60 formula the test asserts by Schroeder integral;
//   * the two tank allpasses are modulated (the paper's excursion), rates
//     incommensurate so the wander never loops audibly;
//   * damping and bandwidth are exposed in HERTZ and designed at the real
//     rate, not as the paper's rate-bound coefficients;
//   * the allpass signs follow this SDK's lattice convention; the paper's
//     depend on its diagram orientation. What the ear and the tests
//     measure (density, decay, decorrelation) is orientation-blind.
//
// SIZE: ~1.5 MB of delay lines (sized for 384 kHz). A MEMBER of SonoreDsp,
// never a local: the wasm stack is 64 KB.
//
// Included by dsp.h.
#pragma once
#ifndef SONORE_DSP_H_INCLUDED
#error "plate.h is part of dsp.h and needs what it includes first: include <sonore/dsp.h> instead."
#endif
#include <cmath>
#include "audio.h"

namespace sonore {

/**
 * Dattorro's 1997 plate: a pre-delay, a bandwidth lowpass and four input
 * diffusers into a modulated figure-of-eight tank, the stereo outputs taken
 * from the paper's seven taps a side (see the file comment for what is
 * faithful to the paper and what is not).
 *
 * Stereo in and out, the input summed to mono. Call prepare() before audio;
 * it scales the paper's delays to the rate (up to 384 kHz) and clears the
 * tank. Set the controls with setParameters(), or the decay as a T60 with
 * setDecayTime(); both, and process(), run on the audio thread. Report
 * tailSamples() to the host. About 1.5 MB of delay lines: a member, never a
 * local.
 */
class PlateReverb {
public:
  /** The plate's settings, set together with setParameters(), which clamps
   *  them: pre-delay 0..195 ms; the bandwidth (input) and damping (in-tank)
   *  lowpasses from 100 Hz up to 0.45 of the rate; decay the tank's loop
   *  gain 0..0.9999, which setDecayTime() works out from a T60; modulation
   *  depth 0..1 of the paper's excursion at 0.01..5 Hz; mix 0..1. */
  struct Parameters {
    float predelayMs = 10.0f;     ///< before the diffusers, in ms, 0..195; a change jumps rather than glides
    float bandwidthHz = 16000.0f; ///< input lowpass, Hz: how bright the plate is struck
    float dampingHz = 5500.0f;    ///< in-tank lowpass, Hz: each lap darker, so the tail darkens as it decays
    float decay = 0.5f;           ///< 0 .. 0.9999, the figure's loop gain; a change glides over about 20 ms
    float modDepth = 1.0f;        ///< 0 .. 1 of the paper's excursion (8 samples at its rate, scaled)
    float modRateHz = 0.7f;       ///< left tank LFO, 0.01..5 Hz; the second LFO runs 1.19x this
    float mix = 0.35f;            ///< 0 dry only, 1 wet only, equal-power between; a change glides
  };

  /** Delays scaled for 48 kHz until prepare() gives the real rate. */
  PlateReverb() { scaleTaps(); }

  /** Sets the rate, scales the paper's delays to it (above 384 kHz they stay
   *  at their 384 kHz lengths), applies the last Parameters again and
   *  clears the tank as reset() does. No allocation: every line is a fixed
   *  array. The channel count is not read: the plate is always stereo. */
  void prepare(const ProcessSpec& spec) {
    sr_ = (float) spec.sampleRate;
    scale_ = sr_ / kPaperRate;
    if (scale_ > kMaxScale) scale_ = kMaxScale; // sized for 384 kHz
    scaleTaps();
    bw_.setSampleRate(sr_);
    for (int c = 0; c < 2; ++c) damp_[c].setSampleRate(sr_);
    decayGlide_.setSampleRate(sr_, 20.0f);
    // The mix glides 20 ms at this rate: the mixers were never given one,
    // and took 960 samples at any rate.
    mixL_.setSampleRate(sr_);
    mixR_.setSampleRate(sr_);
    setParameters(params_);
    reset();
  }
  /** Stores and clamps the settings (the ranges are on Parameters). Once
   *  process() has run, decay and mix glide to the new values; pre-delay,
   *  filters and modulation take effect at once. Before the first process()
   *  everything lands. No allocation: safe to call every block. */
  void setParameters(const Parameters& p) {
    params_ = p;
    params_.decay = clampf(p.decay, 0.0f, 0.9999f);
    // Glides once audio runs (see Glide): the tank's loop gain moved in one
    // sample stepped the tail 6.5 times its largest natural step.
    decayGlide_.set(params_.decay);
    params_.mix = clampf(p.mix, 0.0f, 1.0f);
    pre_ = clampf(p.predelayMs, 0.0f, 195.0f) * 0.001f * sr_;
    if (pre_ < 1.0f) pre_ = 1.0f;
    bw_.setCutoff(clampf(p.bandwidthHz, 100.0f, sr_ * 0.45f));
    for (int c = 0; c < 2; ++c) damp_[c].setCutoff(clampf(p.dampingHz, 100.0f, sr_ * 0.45f));
    excursion_ = clampf(p.modDepth, 0.0f, 1.0f) * 8.0f * scale_;
    incL_ = clampf(p.modRateHz, 0.01f, 5.0f) / sr_;
    incR_ = incL_ * 1.19f;
    mixL_.setMix(params_.mix);
    mixR_.setMix(params_.mix);
  }
  /** Set `decay` from a target mid-band T60: the loop loses decay^2 per
   *  half-lap of kLapSeconds. */
  void setDecayTime(float t60Seconds) {
    Parameters p = params_;
    const float t = t60Seconds > 0.05f ? t60Seconds : 0.05f;
    p.decay = std::pow(10.0f, -1.5f * kLapSeconds / t);
    setParameters(p);
  }
  /** Silence: clears the pre-delay, the diffusers, the tank and its filters,
   *  restarts both LFOs, and lands decay and mix on their settings. */
  void reset() {
    mixL_.snap();
    mixR_.snap();
    decayGlide_.snap();
    preLine_.reset();
    bw_.reset();
    for (int c = 0; c < 2; ++c) damp_[c].reset();
    din1_.reset(); din2_.reset(); din3_.reset(); din4_.reset();
    apfL_.reset(); dL1_.reset(); apfL2_.reset(); dL2_.reset();
    apfR_.reset(); dR1_.reset(); apfR2_.reset(); dR2_.reset();
    phL_ = 0.0f;
    phR_ = 0.25f;
  }
  /** The tail's length to `floorDb`, for the host's tail report. */
  int tailSamples(float floorDb = -80.0f) const {
    // In double and capped: at the documented 0.9999 a lap loses 0.002 dB,
    // and the float product ran past INT_MAX at 192 kHz -- converting that
    // to int is undefined, and it came out as -2147481728.
    const double perLapDb = 40.0 * std::log10(params_.decay < 1e-4f ? 1e-4 : (double) params_.decay);
    if (!(perLapDb < 0.0)) return 2147483647; // a loop gain of 1 never decays
    const double samples = (double) floorDb / perLapDb * kLapSeconds * sr_ + pre_;
    return samples < 2147483647.0 ? (int) samples : 2147483647;
  }

  /** One stereo frame, in place: the input summed to mono into the plate,
   *  the paper's taps out, mixed with the dry signal per channel. No
   *  allocation; the audio thread. */
  inline void process(float& left, float& right) {
    const float dryL = left, dryR = right;
    const float in = 0.5f * (left + right);

    // Output taps first: they read the PREVIOUS tick's tank (one sample of
    // extra loop delay, invisible at reverb scale, and it keeps every read
    // ahead of every write: the read(d)-after-write-is-d-minus-1 trap).
    float yL = 0.6f * (dR1_.read(tap_[T266]) + dR1_.read(tap_[T2974])) - 0.6f * apfR2_.read(tap_[T1913]) +
               0.6f * dR2_.read(tap_[T1996]) - 0.6f * dL1_.read(tap_[T1990]) -
               0.6f * apfL2_.read(tap_[T187]) - 0.6f * dL2_.read(tap_[T1066]);
    float yR = 0.6f * (dL1_.read(tap_[T353]) + dL1_.read(tap_[T3627])) - 0.6f * apfL2_.read(tap_[T1228]) +
               0.6f * dL2_.read(tap_[T2673]) - 0.6f * dR1_.read(tap_[T2111]) -
               0.6f * apfR2_.read(tap_[T335]) - 0.6f * dR2_.read(tap_[T121]);
    const float crossL = dL2_.read(tap_[T3720]); // left half's end, feeds the right sum
    const float crossR = dR2_.read(tap_[T3163]);

    // Input: predelay, bandwidth, four diffusers.
    const float pd = preLine_.read(pre_);
    preLine_.write(in);
    float v = bw_.lp(pd);
    v = apf(din1_, tap_[T142], 0.750f, v);
    v = apf(din2_, tap_[T107], 0.750f, v);
    v = apf(din3_, tap_[T379], 0.625f, v);
    v = apf(din4_, tap_[T277], 0.625f, v);

    const float decay = decayGlide_.next();
    // Left half.
    {
      float t = v + crossR * decay;
      phL_ += incL_;
      if (phL_ >= 1.0f) phL_ -= 1.0f;
      t = apfMod(apfL_, tap_[T672] + excursion_ * fastmath::sinTurns(phL_), 0.70f, t);
      const float d1 = dL1_.read(tap_[T4453]);
      dL1_.write(flushDenormal(t));
      t = damp_[0].lp(d1) * decay;
      t = apf(apfL2_, tap_[T1800], 0.50f, t);
      dL2_.write(flushDenormal(t));
    }
    // Right half.
    {
      float t = v + crossL * decay;
      phR_ += incR_;
      if (phR_ >= 1.0f) phR_ -= 1.0f;
      t = apfMod(apfR_, tap_[T908] + excursion_ * fastmath::sinTurns(phR_), 0.70f, t);
      const float d1 = dR1_.read(tap_[T4217]);
      dR1_.write(flushDenormal(t));
      t = damp_[1].lp(d1) * decay;
      t = apf(apfR2_, tap_[T2656], 0.50f, t);
      dR2_.write(flushDenormal(t));
    }

    left = mixL_.process(dryL, yL);
    right = mixR_.process(dryR, yR);
  }

private:
  static constexpr float kPaperRate = 29761.0f;
  // 384 kHz, not 192: above the rate the lines were sized for, the tank kept
  // its 192 kHz delays -- half the plate in time, a shorter and brighter
  // tail -- and a 150 ms predelay stopped at 107 ms. Twice the memory, for
  // the same plate at every rate a host may run.
  static constexpr float kMaxScale = 384000.0f / 29761.0f;
  // Half a lap of the figure-of-eight, in seconds (the mean of the two
  // halves' delay sums at the paper's rate; delays scale with sr, so this
  // is rate-independent).
  static constexpr float kLapSeconds = 0.5f * (10645.0f + 10944.0f) / 29761.0f;

  /** A paper delay, scaled to the running rate -- and ROUNDED to whole
   *  samples. Scaled, it is fractional, and a linear read of a fractional
   *  delay is a lowpass that depends on the fraction; inside the tank it
   *  compounds every lap, so the top of the tail was a function of the
   *  host's rate: at 44.1 kHz the 12 kHz RT60 came out 1.68 s where whole
   *  samples give 2.15. The modulated allpass adds its fractional excursion
   *  to this and reads cubic, as before. */
  inline float s(int paperSamples) const {
    return (float) (int) ((float) paperSamples * scale_ + 0.5f);
  }

  /** The delays process() reads, named by their length in the paper's
   *  samples and scaled to this rate once, here. They were scaled in process()
   *  -- about thirty multiplies, roundings and conversions a sample, for
   *  numbers that change only with the rate: 193 -> 163 ns a frame without. */
  enum Tap : int { T266, T2974, T1913, T1996, T1990, T187, T1066, T353, T3627, T1228, T2673, T2111, T335, T121, T3720, T3163, T142, T107, T379, T277, T672, T4453, T1800, T908, T4217, T2656, kTapCount };
  static constexpr int kPaperTap[kTapCount] = {266, 2974, 1913, 1996, 1990, 187, 1066, 353, 3627, 1228, 2673, 2111, 335, 121, 3720, 3163, 142, 107, 379, 277, 672, 4453, 1800, 908, 4217, 2656};
  void scaleTaps() {
    for (int k = 0; k < kTapCount; ++k) tap_[k] = s(kPaperTap[k]);
  }
  float tap_[kTapCount]{};

  template<int N>
  static inline float apf(DelayLine<N>& line, float d, float g, float x) {
    const float del = line.read(d);
    const float v = x + g * del;
    line.write(flushDenormal(v));
    return del - g * v;
  }
  template<int N>
  static inline float apfMod(DelayLine<N>& line, float d, float g, float x) {
    const float del = line.readCubic(d); // modulated: cubic, or the sweep is a lowpass
    const float v = x + g * del;
    line.write(flushDenormal(v));
    return del - g * v;
  }

  // Sizes: the paper's figures times 384000/29761, plus excursion headroom.
  DelayLine<81920> preLine_;
  DelayLine<1856> din1_;
  DelayLine<1408> din2_;
  DelayLine<4904> din3_;
  DelayLine<3592> din4_;
  DelayLine<8904> apfL_;
  DelayLine<57472> dL1_;
  DelayLine<23240> apfL2_;
  DelayLine<48016> dL2_;
  DelayLine<11944> apfR_;
  DelayLine<54424> dR1_;
  DelayLine<34288> apfR2_;
  DelayLine<40824> dR2_;
  OnePole bw_, damp_[2];
  DryWetMixer mixL_, mixR_;
  Parameters params_{};
  float sr_ = 48000.0f, scale_ = 48000.0f / 29761.0f, pre_ = 480.0f;
  Glide decayGlide_{0.5f, 0.5f};
  float excursion_ = 8.0f, incL_ = 0.0f, incR_ = 0.0f, phL_ = 0.0f, phR_ = 0.25f;
};

} // namespace sonore
