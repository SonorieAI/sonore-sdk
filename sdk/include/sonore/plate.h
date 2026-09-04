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
// it earns its place beside fdn.h and spring.h because its sound: fast
// even density, metallic-smooth tail: comes from this exact wiring, not
// from a parameter set another topology could copy.
//
// Faithfulness, stated precisely:
//   * every delay length and output tap is the paper's figure, scaled by
//     sr / 29761 (the paper's rate), so the plate keeps its SIZE in
//     seconds at any sample rate;
//   * decay is applied where the figure applies it: after each half's
//     damping filter, and again on the cross-feed into the opposite half,
//     so the loop loses decay^2 per half-lap of ~0.36 s, which is the
//     T60 formula the test asserts by Schroeder integral;
//   * the two tank allpasses are modulated (the paper's excursion), rates
//     incommensurate so the wander never loops audibly;
//   * damping and bandwidth are exposed in HERTZ and designed at the real
//     rate, not as the paper's rate-bound coefficients;
//   * the allpass signs follow this SDK's lattice convention; the paper's
//     depend on its diagram orientation. What the ear and the tests
//     measure, density, decay, decorrelation, is orientation-blind.
//
// SIZE: ~740 KB of delay lines (sized for 192 kHz). A MEMBER of SonoreDsp,
// never a local: the wasm stack is 64 KB.
//
// Included by dsp.h.
#pragma once
#include <cmath>
#include "audio.h"

namespace sonore {

class PlateReverb {
public:
  struct Parameters {
    float predelayMs = 10.0f;
    float bandwidthHz = 16000.0f; // input lowpass
    float dampingHz = 5500.0f;    // in-tank lowpass
    float decay = 0.5f;           // 0 .. 0.9999, the figure's loop gain
    float modDepth = 1.0f;        // 0 .. 1 of the paper's excursion
    float modRateHz = 0.7f;       // the second LFO runs 1.19x this
    float mix = 0.35f;
  };

  void prepare(const ProcessSpec& spec) {
    sr_ = (float) spec.sampleRate;
    scale_ = sr_ / kPaperRate;
    if (scale_ > kMaxScale) scale_ = kMaxScale; // sized for 192 kHz
    bw_.setSampleRate(sr_);
    for (int c = 0; c < 2; ++c) damp_[c].setSampleRate(sr_);
    setParameters(params_);
    reset();
  }
  void setParameters(const Parameters& p) {
    params_ = p;
    params_.decay = clampf(p.decay, 0.0f, 0.9999f);
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
  void reset() {
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
    const float perLapDb = 40.0f * std::log10(params_.decay < 1e-4f ? 1e-4f : params_.decay);
    const float laps = floorDb / perLapDb; // both negative
    return (int) (laps * kLapSeconds * sr_) + (int) pre_;
  }

  inline void process(float& left, float& right) {
    const float dryL = left, dryR = right;
    const float in = 0.5f * (left + right);

    // Output taps first: they read the PREVIOUS tick's tank (one sample of
    // extra loop delay, invisible at reverb scale, and it keeps every read
    // ahead of every write: the read(d)-after-write-is-d-minus-1 trap).
    float yL = 0.6f * (dR1_.read(s(266)) + dR1_.read(s(2974))) - 0.6f * apfR2_.read(s(1913)) +
               0.6f * dR2_.read(s(1996)) - 0.6f * dL1_.read(s(1990)) -
               0.6f * apfL2_.read(s(187)) - 0.6f * dL2_.read(s(1066));
    float yR = 0.6f * (dL1_.read(s(353)) + dL1_.read(s(3627))) - 0.6f * apfL2_.read(s(1228)) +
               0.6f * dL2_.read(s(2673)) - 0.6f * dR1_.read(s(2111)) -
               0.6f * apfR2_.read(s(335)) - 0.6f * dR2_.read(s(121));
    const float crossL = dL2_.read(s(3720)); // left half's end, feeds the right sum
    const float crossR = dR2_.read(s(3163));

    // Input: predelay, bandwidth, four diffusers.
    const float pd = preLine_.read(pre_);
    preLine_.write(in);
    float v = bw_.lp(pd);
    v = apf(din1_, s(142), 0.750f, v);
    v = apf(din2_, s(107), 0.750f, v);
    v = apf(din3_, s(379), 0.625f, v);
    v = apf(din4_, s(277), 0.625f, v);

    const float decay = params_.decay;
    // Left half.
    {
      float t = v + crossR * decay;
      phL_ += incL_;
      if (phL_ >= 1.0f) phL_ -= 1.0f;
      t = apfMod(apfL_, s(672) + excursion_ * fastmath::sinTurns(phL_), 0.70f, t);
      const float d1 = dL1_.read(s(4453));
      dL1_.write(flushDenormal(t));
      t = damp_[0].lp(d1) * decay;
      t = apf(apfL2_, s(1800), 0.50f, t);
      dL2_.write(flushDenormal(t));
    }
    // Right half.
    {
      float t = v + crossL * decay;
      phR_ += incR_;
      if (phR_ >= 1.0f) phR_ -= 1.0f;
      t = apfMod(apfR_, s(908) + excursion_ * fastmath::sinTurns(phR_), 0.70f, t);
      const float d1 = dR1_.read(s(4217));
      dR1_.write(flushDenormal(t));
      t = damp_[1].lp(d1) * decay;
      t = apf(apfR2_, s(2656), 0.50f, t);
      dR2_.write(flushDenormal(t));
    }

    left = mixL_.process(dryL, yL);
    right = mixR_.process(dryR, yR);
  }

private:
  static constexpr float kPaperRate = 29761.0f;
  static constexpr float kMaxScale = 192000.0f / 29761.0f;
  // Half a lap of the figure-of-eight, in seconds (the mean of the two
  // halves' delay sums at the paper's rate; delays scale with sr, so this
  // is rate-independent).
  static constexpr float kLapSeconds = 0.5f * (10645.0f + 10944.0f) / 29761.0f;

  /** A paper delay, scaled to the running rate. */
  inline float s(int paperSamples) const { return (float) paperSamples * scale_; }

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

  // Sizes: the paper's figures times 192000/29761, plus excursion headroom.
  DelayLine<40960> preLine_;
  DelayLine<928> din1_;
  DelayLine<704> din2_;
  DelayLine<2452> din3_;
  DelayLine<1796> din4_;
  DelayLine<4452> apfL_;
  DelayLine<28736> dL1_;
  DelayLine<11620> apfL2_;
  DelayLine<24008> dL2_;
  DelayLine<5972> apfR_;
  DelayLine<27212> dR1_;
  DelayLine<17144> apfR2_;
  DelayLine<20412> dR2_;
  OnePole bw_, damp_[2];
  DryWetMixer mixL_, mixR_;
  Parameters params_{};
  float sr_ = 48000.0f, scale_ = 48000.0f / 29761.0f, pre_ = 480.0f;
  float excursion_ = 8.0f, incL_ = 0.0f, incR_ = 0.0f, phL_ = 0.0f, phR_ = 0.25f;
};

} // namespace sonore
