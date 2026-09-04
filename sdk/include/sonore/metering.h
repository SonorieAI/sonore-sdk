// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: programme loudness and true peak, the way broadcast measures it.
//
// ITU-R BS.1770-4 K-weighting, momentary / short-term / integrated loudness
// with the two-stage gate, loudness range (EBU Tech 3342), true peak by
// oversampling, and a stereo correlation meter. Nothing here allocates after
// prepare(): the integrated measurement, which in the standard needs every
// gating block kept until the end, is held as a HISTOGRAM instead: fixed
// memory, and exact to a tenth of an LU, which is finer than the display.
//
// Included by dsp.h. The filters are Biquads from dsp.h; the coefficients are
// computed for the host's rate from the parameters that regenerate the
// standard's own 48 kHz table, rather than that table being pasted in and
// wrong at every other rate.
#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>
#include "audio.h"

namespace sonore {

// ── K-weighting ──────────────────────────────────────────────────────────────

/**
 * The BS.1770 pre-filter pair: a +4 dB high shelf modelling the head's
 * acoustic effect, then the RLB high-pass that takes the rumble out.
 *
 * The standard publishes coefficients for 48 kHz only. These are the
 * parameters (corner, gain, Q) and the bilinear design that regenerate that
 * table to eleven digits, and because they are PARAMETERS rather than
 * coefficients they design the same filter at 44.1, 96 or 192 kHz. The odd
 * decimals are what makes the 48 kHz result match the published numbers to
 * the last digit.
 *
 * NOT the RBJ cookbook shelf, and this is the thing worth never re-learning:
 * the product's own measurement harness (measure.ts) used the RBJ high-shelf
 * formula with these same constants, and that filter is 0.26 dB LOW at 1 kHz
 * -- close enough that nothing noticed, wrong enough that every absolute
 * LUFS it reported was a quarter of an LU off. The first draft of this header
 * copied it, and its unit test agreed with it perfectly, which is what
 * agreeing with your own reference is worth. The test now checks against the
 * standard's stated calibration instead: a full-scale 997 Hz sine reads
 * -3.01 LKFS, i.e. the K gain at 997 Hz is the +0.691 dB the offset cancels.
 * The shelf below reads +0.691 at 48 kHz; the RBJ version read +0.433.
 */
class KWeighting {
public:
  void setSampleRate(float sr) {
    sr_ = sr > 1.0f ? sr : 48000.0f;
    design();
    reset();
  }
  void reset() { shelf_.reset(); highpass_.reset(); }

  inline float process(float x) { return highpass_.process(shelf_.process(x)); }

private:
  void design() {
    const double fs = (double) sr_;
    // Shelf. Bilinear design of the analogue prototype with the corner
    // pre-warped by K = tan(pi f0 / fs); Vh is the shelf gain and Vb its
    // square-root-ish midpoint (the exponent is what the standard's own
    // fit implies, not 0.5). Reproduces the published 48 kHz coefficients
    // 1.53512485958697, -2.69169618940638, 1.19839281085285 /
    // -1.69065929318241, 0.73248077421585.
    {
      const double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
      const double K = std::tan((double) kPi * f0 / fs);
      const double Vh = std::pow(10.0, G / 20.0);
      const double Vb = std::pow(Vh, 0.4996667741545416);
      const double a0 = 1.0 + K / Q + K * K;
      shelf_.setCoefficients((float) ((Vh + Vb * K / Q + K * K) / a0),
                             (float) (2.0 * (K * K - Vh) / a0),
                             (float) ((Vh - Vb * K / Q + K * K) / a0),
                             (float) (2.0 * (K * K - 1.0) / a0),
                             (float) ((1.0 - K / Q + K * K) / a0));
    }
    // RLB high-pass. The numerator is the standard's literal 1, -2, 1 --
    // NOT normalised by a0, which is how the published table has it; the
    // passband gain that leaves is part of the calibration.
    {
      const double f0 = 38.13547087602444, Q = 0.5003270373238773;
      const double K = std::tan((double) kPi * f0 / fs);
      const double a0 = 1.0 + K / Q + K * K;
      highpass_.setCoefficients(1.0f, -2.0f, 1.0f,
                                (float) (2.0 * (K * K - 1.0) / a0),
                                (float) ((1.0 - K / Q + K * K) / a0));
    }
  }

  Biquad shelf_, highpass_;
  float sr_ = 48000.0f;
};

// ── Loudness ─────────────────────────────────────────────────────────────────

/**
 * Momentary (400 ms), short-term (3 s) and integrated loudness in LUFS, and
 * loudness range in LU, from any channel count up to MaxChannels.
 *
 * ── The gate, and why the memory is a histogram ──
 *
 * Integrated loudness is not the average of everything. BS.1770 first drops
 * every 400 ms block under -70 LKFS (silence and fades must not drag a
 * programme down), then computes the mean of what is left, drops every block
 * more than 10 LU under THAT, and averages again. The second pass needs the
 * whole distribution of blocks, so a literal implementation stores one number
 * per 100 ms for the length of the programme -- an allocation that grows with
 * the material, on the audio thread.
 *
 * A histogram of block loudness at 0.1 LU resolution holds the same
 * distribution in 800 counters, for ever. Each bin contributes its centre's
 * power times its count, so the error is bounded by half a bin: 0.05 LU, a
 * fifth of what any display resolves. Loudness range uses a second histogram
 * of the short-term values with its own gate (-20 LU relative) and reports the
 * spread between the 10th and 95th percentiles, per EBU Tech 3342.
 *
 * ── Channel weights ──
 *
 * BS.1770 weights surround channels by +1.5 dB (1.41) and ignores the LFE.
 * The defaults follow the standard's layouts for 1, 2 and 6 channels (the 5.1
 * order being L R C LFE Ls Rs, the same one defaultChannelMask() uses) and
 * weight everything else by 1.0; setChannelWeight() overrides one.
 */
template <int MaxChannels = 8>
class LoudnessMeter {
public:
  /** The reading for "nothing measured yet" and for silence. -100 rather than
   *  -infinity, for the same reason gainToDbFloor() exists: a meter that
   *  hands the faceplate -inf has to be special-cased by whoever draws it. */
  static constexpr float kSilence = -100.0f;

  void prepare(const ProcessSpec& spec) {
    sr_ = (float) spec.sampleRate;
    channels_ = (int) spec.numChannels;
    if (channels_ > MaxChannels) channels_ = MaxChannels;
    if (channels_ < 1) channels_ = 1;
    for (int c = 0; c < MaxChannels; ++c) filter_[c].setSampleRate(sr_);
    defaultWeights();
    // A gating block is 400 ms at 75% overlap, which is a new block every
    // 100 ms: one accumulator per 100 ms, four of them make a block.
    subBlock_ = (int) (sr_ * 0.1f);
    if (subBlock_ < 1) subBlock_ = 1;
    reset();
  }

  void reset() {
    for (int c = 0; c < MaxChannels; ++c) filter_[c].reset();
    for (int i = 0; i < kMomentaryBlocks; ++i) momentaryRing_[i] = 0.0;
    for (int i = 0; i < kShortTermBlocks; ++i) shortRing_[i] = 0.0;
    for (int i = 0; i < kBins; ++i) integratedHist_[i] = rangeHist_[i] = 0;
    acc_ = 0.0;
    inSub_ = 0;
    ringPos_ = 0;
    filled_ = 0;
    momentary_ = shortTerm_ = kSilence;
  }

  void setChannelWeight(int channel, float weight) {
    if (channel >= 0 && channel < MaxChannels) weight_[channel] = weight;
  }

  /** One frame. `frame` holds one sample per channel. [audio thread] */
  inline void process(const float* frame) {
    double z = 0.0;
    for (int c = 0; c < channels_; ++c) {
      const float w = filter_[c].process(frame[c]);
      z += (double) weight_[c] * (double) w * (double) w;
    }
    acc_ += z;
    if (++inSub_ >= subBlock_) {
      inSub_ = 0;
      const double meanPower = acc_ / (double) subBlock_;
      acc_ = 0.0;
      momentaryRing_[ringPos_ % kMomentaryBlocks] = meanPower;
      shortRing_[ringPos_ % kShortTermBlocks] = meanPower;
      // Wrapped at a common multiple of both ring lengths, so the two modulo
      // positions stay consistent for ever rather than for 2^31 blocks.
      if (++ringPos_ >= kMomentaryBlocks * kShortTermBlocks) ringPos_ = 0;
      if (filled_ < kShortTermBlocks) ++filled_;

      // Momentary: the last four sub-blocks -- one 400 ms gating block.
      if (filled_ >= kMomentaryBlocks) {
        double m = 0.0;
        for (int i = 0; i < kMomentaryBlocks; ++i) m += momentaryRing_[i];
        momentary_ = toLufs(m / kMomentaryBlocks);
        // The absolute gate belongs to the INTEGRATED measurement; the
        // momentary reading itself is what it is.
        if (momentary_ > kAbsoluteGate) ++integratedHist_[binOf(momentary_)];
      }
      if (filled_ >= kShortTermBlocks) {
        double s = 0.0;
        for (int i = 0; i < kShortTermBlocks; ++i) s += shortRing_[i];
        shortTerm_ = toLufs(s / kShortTermBlocks);
        if (shortTerm_ > kAbsoluteGate) ++rangeHist_[binOf(shortTerm_)];
      }
    }
  }

  /** Stereo convenience: one frame from two samples. */
  inline void process(float left, float right) {
    float f[2] = {left, right};
    process(f);
  }

  float momentary() const { return momentary_; }
  float shortTerm() const { return shortTerm_; }

  /** Integrated loudness over everything since reset(), gated as BS.1770
   *  specifies. Cheap enough to call from a UI timer: it walks 800 bins. */
  float integrated() const {
    double power = 0.0;
    uint64_t count = 0;
    for (int i = 0; i < kBins; ++i) {
      if (!integratedHist_[i]) continue;
      power += binPower(i) * (double) integratedHist_[i];
      count += integratedHist_[i];
    }
    if (count == 0) return kSilence;
    const float relativeGate = toLufs(power / (double) count) - 10.0f;
    power = 0.0;
    count = 0;
    for (int i = binOf(relativeGate); i < kBins; ++i) {
      if (!integratedHist_[i]) continue;
      power += binPower(i) * (double) integratedHist_[i];
      count += integratedHist_[i];
    }
    return count ? toLufs(power / (double) count) : kSilence;
  }

  /** Loudness range in LU: how much the short-term loudness moved around,
   *  ignoring the quietest and loudest 10% and 5% so one silence or one
   *  explosion does not define it. Zero until 3 s have been measured. */
  float range() const {
    double power = 0.0;
    uint64_t count = 0;
    for (int i = 0; i < kBins; ++i) {
      if (!rangeHist_[i]) continue;
      power += binPower(i) * (double) rangeHist_[i];
      count += rangeHist_[i];
    }
    if (count == 0) return 0.0f;
    const int from = binOf(toLufs(power / (double) count) - 20.0f);
    count = 0;
    for (int i = from; i < kBins; ++i) count += rangeHist_[i];
    if (count < 2) return 0.0f;
    // Percentiles by walking the counts.
    const double p10 = (double) count * 0.10, p95 = (double) count * 0.95;
    double seen = 0.0;
    float lo = kSilence, hi = kSilence;
    bool haveLo = false;
    for (int i = from; i < kBins; ++i) {
      if (!rangeHist_[i]) continue;
      seen += rangeHist_[i];
      if (!haveLo && seen >= p10) { lo = binCentre(i); haveLo = true; }
      if (seen >= p95) { hi = binCentre(i); break; }
    }
    return hi > lo ? hi - lo : 0.0f;
  }

private:
  static constexpr int kMomentaryBlocks = 4;   // 4 x 100 ms
  static constexpr int kShortTermBlocks = 30;  // 30 x 100 ms
  static constexpr float kAbsoluteGate = -70.0f;
  static constexpr float kHistFloor = -70.0f;  // the absolute gate is the floor
  static constexpr float kHistCeiling = 10.0f; // nothing legal reads above this
  static constexpr float kBinWidth = 0.1f;
  static constexpr int kBins = (int) ((kHistCeiling - kHistFloor) / kBinWidth); // 800

  static float toLufs(double meanPower) {
    return meanPower > 1e-12 ? (float) (-0.691 + 10.0 * std::log10(meanPower)) : kSilence;
  }
  static int binOf(float lufs) {
    int b = (int) ((lufs - kHistFloor) / kBinWidth);
    return b < 0 ? 0 : (b >= kBins ? kBins - 1 : b);
  }
  static float binCentre(int bin) { return kHistFloor + ((float) bin + 0.5f) * kBinWidth; }
  static double binPower(int bin) {
    return std::pow(10.0, ((double) binCentre(bin) + 0.691) / 10.0);
  }

  void defaultWeights() {
    for (int c = 0; c < MaxChannels; ++c) weight_[c] = 1.0f;
    // `if constexpr` so a meter sized for stereo never even compiles the
    // 5.1 branch -- gcc's -Warray-bounds correctly points out that
    // weight_[5] does not exist in a LoudnessMeter<2>.
    if constexpr (MaxChannels >= 6) {
      if (channels_ == 6) {
        weight_[3] = 0.0f;  // LFE
        weight_[4] = 1.41f; // Ls
        weight_[5] = 1.41f; // Rs
      }
    }
  }

  KWeighting filter_[MaxChannels];
  float weight_[MaxChannels]{};
  double momentaryRing_[kMomentaryBlocks]{};
  double shortRing_[kShortTermBlocks]{};
  uint32_t integratedHist_[kBins]{};
  uint32_t rangeHist_[kBins]{};
  double acc_ = 0.0;
  float sr_ = 48000.0f;
  int channels_ = 2, subBlock_ = 4800, inSub_ = 0, ringPos_ = 0, filled_ = 0;
  float momentary_ = kSilence, shortTerm_ = kSilence;
};

// ── True peak ────────────────────────────────────────────────────────────────

/**
 * Peak level between the samples, in dBTP.
 *
 * A sample peak reads the samples; a true peak reads the WAVEFORM the samples
 * describe, which can pass between two of them higher than either -- a sine
 * near a quarter of the rate sampled at 45 degrees reads -3 dBFS sample peak
 * while the wave itself touches 0 dBTP. A limiter that watches sample peaks
 * ships intersample overs to every DAC, and a loudness spec that says "-1 dBTP"
 * means this measurement, not that one.
 *
 * Four-times oversampling by a 16-tap Blackman-windowed sinc per phase, the
 * taps computed here rather than pasted (BS.1770 Annex 2 tabulates a 48-tap
 * filter for 48 kHz; the windowed sinc is the same design at any rate). The
 * three interpolated phases and the sample itself are all compared, so this
 * can never read LOWER than the sample peak.
 */
class TruePeakMeter {
public:
  TruePeakMeter() { buildTaps(); reset(); }

  void reset() {
    for (int i = 0; i < 2 * kTaps; ++i) history_[i] = 0.0f;
    write_ = 0;
    held_ = 0.0f;
  }

  /** Feed one sample; returns the true-peak ABSOLUTE level around it (linear),
   *  and holds the maximum until reset(). [audio thread] */
  inline float process(float x) {
    // Written twice, a ring's length apart, so the last kTaps samples are
    // always CONTIGUOUS starting at write_+1 -- no modulo in the inner loop,
    // which at 48 multiplies per sample is the difference between a meter
    // and a load.
    history_[write_] = x;
    history_[write_ + kTaps] = x;
    const float* window = history_ + write_ + 1; // oldest first, newest last
    write_ = (write_ + 1) % kTaps;
    float peak = std::fabs(x);
    for (int ph = 0; ph < kPhases - 1; ++ph) {
      float v = 0.0f;
      for (int k = 0; k < kTaps; ++k) v += window[k] * taps_[ph][k];
      const float a = std::fabs(v);
      if (a > peak) peak = a;
    }
    if (peak > held_) held_ = peak;
    return peak;
  }

  /** The highest true peak since reset(), in dBTP. */
  float peakDb() const { return held_ > 1e-9f ? 20.0f * std::log10(held_) : -180.0f; }
  float peakLinear() const { return held_; }

private:
  static constexpr int kPhases = 4;
  static constexpr int kHalf = 8;
  static constexpr int kTaps = 2 * kHalf;

  void buildTaps() {
    for (int ph = 1; ph < kPhases; ++ph) {
      const double frac = (double) ph / (double) kPhases;
      double sum = 0.0;
      for (int k = 0; k < kTaps; ++k) {
        const double t = (double) (k - kHalf) + frac;
        const double sinc = std::fabs(t) < 1e-12 ? 1.0
                                                 : std::sin((double) kPi * t) / ((double) kPi * t);
        const double n = (double) k, m = (double) (kTaps - 1);
        const double w = 0.42 - 0.5 * std::cos(2.0 * (double) kPi * n / m) +
                         0.08 * std::cos(4.0 * (double) kPi * n / m);
        // Tap k meets the sample (k - kHalf) positions from the window's
        // centre, oldest first, which is exactly the order process() walks.
        // Whichever way round the window is read, the three fractions land on
        // the same three points between samples -- but only one order makes
        // this comment true, so it is that one.
        taps_[ph - 1][k] = (float) (sinc * w);
        sum += sinc * w;
      }
      // Each phase normalised to unity DC gain: a windowed sinc's taps sum to
      // a little under one, and a true-peak reading a fraction of a percent
      // LOW on a flat top is a limiter that lets a fraction of a percent past.
      if (sum > 1e-12)
        for (int k = 0; k < kTaps; ++k) taps_[ph - 1][k] = (float) ((double) taps_[ph - 1][k] / sum);
    }
  }

  float taps_[kPhases - 1][kTaps]{};
  float history_[2 * kTaps]{};
  int write_ = 0;
  float held_ = 0.0f;
};

// ── Stereo correlation ───────────────────────────────────────────────────────

/**
 * Phase correlation between two channels, -1..+1, over a sliding window.
 *
 * +1 is mono, 0 is unrelated, -1 is one channel inverted -- the reading a
 * goniometer's needle shows, and the number that says whether a mix survives
 * being summed to mono. Three exponential averages (L·R, L², R²) with one
 * time constant, so the reading settles in the stated milliseconds rather
 * than jittering per sample.
 */
class CorrelationMeter {
public:
  void setSampleRate(float sr) { sr_ = sr > 1.0f ? sr : 48000.0f; setTime(windowMs_); }
  /** Averaging time. 300 ms is what hardware meters use. */
  void setTime(float ms) {
    windowMs_ = ms > 1.0f ? ms : 1.0f;
    a_ = std::exp(-1.0f / (0.001f * windowMs_ * sr_));
  }
  void reset() { lr_ = ll_ = rr_ = 0.0f; }

  inline void process(float left, float right) {
    const float b = 1.0f - a_;
    lr_ = flushDenormal(a_ * lr_ + b * left * right);
    ll_ = flushDenormal(a_ * ll_ + b * left * left);
    rr_ = flushDenormal(a_ * rr_ + b * right * right);
  }

  /** The correlation now. Zero for silence rather than NaN. */
  float value() const {
    const float denom = std::sqrt(ll_ * rr_);
    if (denom < 1e-12f) return 0.0f;
    const float c = lr_ / denom;
    return c < -1.0f ? -1.0f : (c > 1.0f ? 1.0f : c);
  }

private:
  float sr_ = 48000.0f, windowMs_ = 300.0f, a_ = 0.9999f;
  float lr_ = 0.0f, ll_ = 0.0f, rr_ = 0.0f;
};

} // namespace sonore
