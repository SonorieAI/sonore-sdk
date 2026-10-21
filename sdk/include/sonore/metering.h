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
#ifndef SONORE_DSP_H_INCLUDED
#error "metering.h is part of dsp.h and needs what it includes first: include <sonore/dsp.h> instead."
#endif
#include <cmath>
#include <cstddef>
#include <cstdint>
#include "audio.h"
#include "simd.h"

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
  /** Designs both filters for `sr` Hz (1 or less is taken as 48000) and
   *  clears their state. No allocation, but it calls tan and pow: once per
   *  rate change, not per block. */
  void setSampleRate(float sr) {
    sr_ = sr > 1.0f ? sr : 48000.0f;
    design();
    reset();
  }
  /** Clears both filters' state; the design is kept. */
  void reset() { shelf_.reset(); highpass_.reset(); }

  /** [audio thread] One sample through the shelf and then the high-pass:
   *  the K-weighted signal, whose mean square BS.1770 turns into loudness. */
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
      shelf_.setCoefficients((Vh + Vb * K / Q + K * K) / a0, 2.0 * (K * K - Vh) / a0,
                             (Vh - Vb * K / Q + K * K) / a0, 2.0 * (K * K - 1.0) / a0,
                             (1.0 - K / Q + K * K) / a0);
    }
    // RLB high-pass. The numerator is the standard's literal 1, -2, 1 --
    // NOT normalised by a0, which is how the published table has it; the
    // passband gain that leaves is part of the calibration.
    {
      const double f0 = 38.13547087602444, Q = 0.5003270373238773;
      const double K = std::tan((double) kPi * f0 / fs);
      const double a0 = 1.0 + K / Q + K * K;
      highpass_.setCoefficients(1.0, -2.0, 1.0, 2.0 * (K * K - 1.0) / a0, (1.0 - K / Q + K * K) / a0);
    }
  }

  // Double, coefficients and state: the 38 Hz high-pass at 384 kHz in a float
  // biquad read 20 Hz 0.3 dB off what it reads at 192 (see IirSection).
  IirSection shelf_, highpass_;
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

  /** Sets the rate and channel count and starts again from silence, as
   *  reset() does. The count is clamped to 1..MaxChannels; the standard's
   *  channel weights for that layout are applied, except to channels
   *  setChannelWeight() overrode. Allocates nothing: every buffer is a
   *  fixed-size member. */
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

  /** Forgets everything measured: filters, the 3 s window and both
   *  histograms, so momentary(), shortTerm() and integrated() read kSilence
   *  and range() reads 0 until enough is measured again. Channel weights are
   *  kept. */
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

  /** Override one channel's weight. It KEEPS across prepare(): the defaults
   *  were rewritten there, so a 7.1 LFE set to 0 went back to 1.0 the first
   *  time a host changed the rate, and the reading rose with nothing said. */
  void setChannelWeight(int channel, float weight) {
    if (channel >= 0 && channel < MaxChannels) {
      weight_[channel] = weight;
      overridden_[channel] = true;
    }
  }

  /** Back to the standard's weights for the current layout. */
  void resetChannelWeights() {
    for (bool& o : overridden_) o = false;
    defaultWeights();
  }

  /** One frame. `frame` holds one sample per channel. [audio thread] */
  inline void process(const float* frame) {
    double z = 0.0;
    for (int c = 0; c < channels_; ++c) {
      const float w = filter_[c].process(frame[c]);
      // A sample that is not finite carries no loudness. Its power went into
      // the 100 ms block whole: momentary read +inf LUFS for 400 ms and
      // short-term for 3 s, and binOf() booked those blocks through (int) of
      // infinity -- undefined; bin 0 on x64, the top bin on ARM64, where four
      // blocks at +9.95 LUFS gate a -23 programme out of its own integrated
      // reading. (The filter has already dropped it from its state.)
      const double power = (double) weight_[c] * (double) w * (double) w;
      if (std::isfinite(power)) z += power;
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

  /** Stereo convenience: one frame from two samples. A meter prepared for
   *  more channels reads the rest of the frame as silence; one prepared for
   *  mono reads the left.
   *
   *  The frame is as wide as the meter. It was two floats, and process()
   *  reads as many as prepare() was given: a 5.1 meter fed this way read
   *  four samples past the array (ASan: stack-buffer-overflow), and a
   *  -23 LUFS stereo tone measured +9.95 on whatever lay beside it. */
  inline void process(float left, float right) {
    float f[MaxChannels] = {};
    f[0] = left;
    if constexpr (MaxChannels > 1) f[1] = right;
    process(f);
  }

  /** Loudness of the last 400 ms in LUFS, updated every 100 ms. kSilence
   *  until 400 ms have been measured since reset(), and for silence. */
  float momentary() const { return momentary_; }
  /** Loudness of the last 3 s in LUFS, updated every 100 ms. kSilence until
   *  3 s have been measured since reset(), and for silence. */
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
    float standard[MaxChannels];
    for (int c = 0; c < MaxChannels; ++c) standard[c] = 1.0f;
    // `if constexpr` so a meter sized for stereo never even compiles the
    // 5.1 branch -- gcc's -Warray-bounds correctly points out that
    // standard[5] does not exist in a LoudnessMeter<2>.
    if constexpr (MaxChannels >= 6) {
      if (channels_ == 6) {
        standard[3] = 0.0f;  // LFE
        standard[4] = 1.41f; // Ls
        standard[5] = 1.41f; // Rs
      }
    }
    for (int c = 0; c < MaxChannels; ++c)
      if (!overridden_[c]) weight_[c] = standard[c];
  }

  KWeighting filter_[MaxChannels];
  float weight_[MaxChannels]{};
  bool overridden_[MaxChannels]{};
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
  /** Computes the interpolation taps and starts from silence. The filter
   *  does not depend on the sample rate, so there is no prepare(). */
  TruePeakMeter() { buildTaps(); reset(); }

  /** Clears the history and the held maximum: peakLinear() reads 0 and
   *  peakDb() -180 until the next sample. */
  void reset() {
    for (int i = 0; i < 2 * kTaps; ++i) history_[i] = 0.0f;
    write_ = 0;
    held_ = 0.0f;
  }

  /** Feed one sample; returns the true-peak ABSOLUTE level (linear) of the
   *  stretch kHalf - 1 samples behind it -- where the interpolator's points
   *  are -- and holds the maximum until reset(). [audio thread] */
  inline float process(float x) {
    // Written twice, a ring's length apart, so the last kTaps samples are
    // always CONTIGUOUS starting at write_+1 -- no modulo in the inner loop,
    // which at 48 multiplies per sample is the difference between a meter
    // and a load.
    history_[write_] = x;
    history_[write_ + kTaps] = x;
    const float* window = history_ + write_ + 1; // oldest first, newest last
    write_ = (write_ + 1) % kTaps;
    // The interpolated points fall between window[kHalf - 1] and
    // window[kHalf], so the sample they are compared with is window[kHalf]:
    // the same instant. Comparing them with x, the newest, put a click's
    // level out seven samples before its waveform, and the true-peak limiter
    // detecting on this saw every transient that early.
    float peak = std::fabs(window[kHalf]);
    for (int ph = 0; ph < kPhases - 1; ++ph) {
      // A contiguous window against contiguous taps: simd::dot's whole job,
      // four lanes at a time, in the lane order every backend shares.
      const float v = simd::dot(window, taps_[ph], (size_t) kTaps);
      const float a = std::fabs(v);
      if (a > peak) peak = a;
    }
    // The held maximum takes the newest sample as well, so it never reads
    // under the sample peak, even of the last few samples before a reset.
    const float ax = std::fabs(x);
    if (peak > held_) held_ = peak;
    if (ax > held_) held_ = ax;
    return peak;
  }

  /** The highest true peak since reset(), in dBTP. */
  float peakDb() const { return held_ > 1e-9f ? 20.0f * std::log10(held_) : -180.0f; }
  /** The highest true peak since reset(), as a linear absolute level (1.0 is
   *  0 dBTP). Never below the highest sample fed since then. */
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
  /** The rate in Hz (1 or less is taken as 48000); recomputes the averaging
   *  coefficient for the current setTime(). Keeps the averages. */
  void setSampleRate(float sr) { sr_ = sr > 1.0f ? sr : 48000.0f; setTime(windowMs_); }
  /** Averaging time. 300 ms is what hardware meters use. */
  void setTime(float ms) {
    windowMs_ = ms > 1.0f ? ms : 1.0f;
    a_ = std::exp(-1.0f / (0.001f * windowMs_ * sr_));
  }
  /** Clears the three averages, so value() reads 0 until signal arrives. */
  void reset() { lr_ = ll_ = rr_ = 0.0f; }

  /** One stereo sample pair into the averages. No allocation. [audio thread] */
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
