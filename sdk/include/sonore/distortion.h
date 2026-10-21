// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: distortion as one object.
//
// "Add some drive" was thirty lines every time: a gain, a curve, an
// oversampler around the curve, a DC blocker after it because the curve was
// asymmetric, a gain to undo the level the drive added, a tone control, a
// mix. Each of the thirty was written before and got right before; the
// object is the thirty at once, on the SDK's rules -- the curve runs
// oversampled, the DC comes off, the level is compensated so the drive knob
// changes the tone and not the volume, and the latency is reported.
//
// The compensation is EQUAL RMS: a sine at -12 dBFS comes out at the RMS it
// went in with, whatever the curve did to its shape. Matching peaks instead
// (the first draft) let a hard-driven square read 2 dB louder than the sine
// it came from, because a square's fundamental is 4/pi of its peak -- and
// loudness is what the ear compares, not peaks.
//
// The oversampling factor is a template parameter because it is a real
// trade and the right answer depends on the curve. Measured on a hot
// (-6 dBFS) 15 kHz tone, worst folded product below the fundamental:
//   tanh at 20 dB of drive:  8x is under -40 dB, where the generation
//                            harness's aliasing gate sits; 4x reads -38.6;
//   hard clip at 30 dB:      a full square at 15 kHz has 1/n harmonics to
//                            infinity, and no affordable factor makes that
//                            clean -- 4x leaves the 11th in the second
//                            half-band's transition band, 8x reaches the
//                            25th folding into the passband.
// So: Stages=3 (8x, 28 samples of latency) by default -- measured, tanh at
// 20 dB holds its folds under -40 dB there and 4x reads -38.6 -- with
// Stages=2 (4x, 24 samples) the documented lighter option, and the test
// file records both numbers.
//
// For a diode-clipper pedal use clipper.h, which is the circuit; this is
// the family of memoryless shapes.
//
// Included by dsp.h.
#pragma once
#ifndef SONORE_DSP_H_INCLUDED
#error "distortion.h is part of dsp.h and needs what it includes first: include <sonore/dsp.h> instead."
#endif
#include <cmath>
#include "audio.h"

namespace sonore {

/**
 * A memoryless distortion with everything around the curve done: drive,
 * the curve oversampled by 2 to the power `Stages`, DC removal, equal-RMS
 * level compensation, a tone lowpass and a dry/wet mix whose dry path waits
 * for the wet one (see the file comment for the measurements behind the
 * default 8x).
 *
 * One channel, per sample. Call setSampleRate() in prepare() and report
 * latencySamples() to the host: 28 samples at the default 8x, 24 at 4x.
 * Drive is 0..40 dB, bias -0.5..0.5, tone 1..20 kHz, mix 0..1 (fully wet
 * until set). The setters and process() run on the audio thread and can be
 * called every block: the compensation is measured again, over 256 points
 * of the curve, only when the drive, curve, bias or auto gain changed, and
 * drive and makeup glide together in decibels. For a diode-clipper pedal
 * use clipper.h, which is the circuit.
 */
template<int Stages = 3>
class Distortion {
public:
  /** The shape the driven signal goes through. Every one but Asymmetric is
   *  odd-symmetric, so it adds odd harmonics only until a bias is set. */
  enum class Curve {
    Soft,       ///< `x / (1 + |x|)`: the gentlest, never quite reaching +/-1 (the default)
    Tanh,       ///< tanh, by a rational approximation (fastmath::tanhApprox)
    Hard,       ///< clipped flat at +/-1
    Cubic,      ///< dsp.h's cubic(): `1.5y - 0.5y^3` of y = x held to +/-1, flat at +/-1 beyond it
    Asymmetric, ///< `tanh(x + 0.3) - tanh(0.3)`: lopsided, so even harmonics too
    Fold        ///< WaveFolder::fold: reflected back into +/-1 as often as it takes
  };

  /** Fully WET by default. It is a distortion: the mixer it is built on
   *  starts dry, and a Distortion nobody called setMix on passed its input
   *  through untouched. */
  Distortion() {
    mixer_.setWetLatency(latencySamples());
    mixer_.setMix(1.0f);
  }

  /** The host's rate in Hz; one of 1 or less, or NaN, is taken as 48000.
   *  The DC blocker, the tone filter, the 10 ms glides and the mix's 20 ms
   *  glide are set up for it, the tone held under 0.45 x the rate. Call it
   *  in prepare(). */
  void setSampleRate(float sr) {
    sr_ = sr > 1.0f ? sr : 48000.0f;
    blocker_.setSampleRate(sr_);
    tone_.setSampleRate(sr_);
    driveGlide_.setSampleRate(sr_);
    makeupGlide_.setSampleRate(sr_);
    biasGlide_.setSampleRate(sr_);
    mixer_.setSampleRate(sr_);
    setTone(toneHz_);
    update();
  }
  /** 0 .. 40 dB into the curve. */
  void setDrive(float db) { driveDb_ = clampf(db, 0.0f, 40.0f); update(); }
  /** The shape, Soft until set. It changes at once; with auto gain on, the
   *  makeup for the new shape is measured and glides in. */
  void setCurve(Curve c) { os_.shaper().curve = c; update(); }
  /** A DC push before the curve: even harmonics from an odd shape. Glides
   *  with the drive and the makeup: moved in one sample, the curve's DC
   *  stepped with it and the blocker let the step through -- 0 -> 0.5 on a
   *  110 Hz tone at 12 dB of drive stepped the output 6.5 times its largest
   *  natural step. */
  void setBias(float bias) {
    bias_ = clampf(bias, -0.5f, 0.5f);
    biasGlide_.set(bias_);
    update();
  }
  /** A lowpass after the curve, the "tone" knob: 1 kHz .. 20 kHz. */
  void setTone(float hz) {
    toneHz_ = clampf(hz, 1000.0f, 20000.0f);
    tone_.setCutoff(toneHz_ < sr_ * 0.45f ? toneHz_ : sr_ * 0.45f);
  }
  /** 0 dry .. 1 wet (the default), held to that range: an equal-power
   *  crossfade, the dry path delayed by latencySamples() to line up. A new
   *  mix glides in once audio is running. */
  void setMix(float mix) { mixer_.setMix(mix); }
  /** Hold a -12 dBFS sine at its RMS whatever the drive (default on). */
  void setAutoGain(bool on) { autoGain_ = on; update(); }
  /** Silence: the oversampler, DC blocker, tone filter and dry delay cleared,
   *  and drive, makeup, bias and mix landed on their targets. The settings
   *  stay. */
  void reset() {
    os_.reset();
    blocker_.reset();
    tone_.reset();
    mixer_.reset();
    driveGlide_.snap();
    makeupGlide_.snap();
    biasGlide_.snap();
  }
  /** The oversampling factor, 2^Stages: 8 at the default. */
  static constexpr int factor() { return 1 << Stages; }
  /** The delay in samples at the host's rate, wet and dry alike: the number
   *  to report to the host. 28 at the default 8x, 24 at 4x. */
  static constexpr int latencySamples() { return Oversampled<Shaper, Stages>::latencySamples(); }

  /** [audio-thread] One sample: drive and bias, the curve oversampled, DC off,
   *  makeup, tone, then the mix with the delayed dry. latencySamples() late.
   *  Allocation-free. */
  inline float process(float x) {
    // The drive is applied BEFORE the oversampler, which is linear, so it
    // is the same gain the curve used to apply inside -- and out here it
    // can glide. Drive and makeup move together (see Glide): stepped in one
    // sample, 0 -> 40 dB of drive stepped the output 1.7 times its largest
    // natural step.
    // Both glide in DECIBELS: the compensation is close to the inverse of
    // the drive in dB once the curve saturates, so moving them together
    // that way keeps the level steady through the move. The bias is added
    // out here too, for the same reason: the oversampler passes DC at unity,
    // so it is the same push the curve used to add inside, and here it glides.
    float y = os_.process(x * dbToGain(driveGlide_.next()) + biasGlide_.next());
    y = blocker_.process(y) * dbToGain(makeupGlide_.next());
    y = tone_.lp(y);
    // The dry signal WAITS for the wet one, which is latencySamples() late
    // through the oversampler. Summed undelayed it was a comb -- its first
    // notch at sr / (2 * latency), 857 Hz at 8x and 48 kHz -- at every mix
    // but the two ends, and worst at the middle where people leave the knob.
    return mixer_.process(0, x, y);
  }

private:
  /** The curve as the oversampler runs it, at the oversampled rate. Its
   *  drive stays at 1: process() applies the drive and the bias before the
   *  oversampler, where they can glide. */
  struct Shaper {
    Curve curve = Curve::Soft; ///< the shape setCurve() chose
    float drive = 1.0f;        ///< linear gain before the shape; update() keeps it at 1
    /** `x` through the shape, at the oversampled rate. */
    inline float process(float x) const { return shape(x * drive); }
    /** The bare shape, which update() also samples to measure the makeup. */
    inline float shape(float v) const {
      switch (curve) {
        case Curve::Tanh: return fastmath::tanhApprox(v);
        case Curve::Hard: return hardClip(v);
        case Curve::Cubic: return cubic(v, 1.0f);
        case Curve::Asymmetric: return asymmetric(v, 0.3f);
        case Curve::Fold: return WaveFolder::fold(v);
        default: return softClip(v);
      }
    }
  };

  void update() {
    // The curve itself runs at unity drive; process() applies the drive.
    os_.shaper().drive = 1.0f;
    const float drive = dbToGain(driveDb_);
    driveGlide_.set(driveDb_);
    // Worked out again only when something it depends on moved: a plugin
    // calls the setters every block.
    const Shaper& shaper = os_.shaper();
    if (drive == madeFor_.drive && shaper.curve == madeFor_.curve && bias_ == madeFor_.bias &&
        autoGain_ == madeFor_.autoGain)
      return;
    madeFor_ = {drive, bias_, shaper.curve, autoGain_};
    if (!autoGain_) {
      makeup_ = 1.0f;
      makeupGlide_.set(0.0f);
      return;
    }
    // One cycle of a -12 dBFS sine through the curve, DC removed (the
    // blocker does the same to the signal), RMS matched to the sine's.
    // 256 points: the fold at 40 dB of drive turns the sine over some fifty
    // times a cycle, more than 64 points can follow. What this cannot know
    // is the band: the curve's output is measured full-band, the plugin's
    // is what the oversampler lets through, so a curve that throws most of
    // its energy past Nyquist -- the fold, driven hard, on a high note --
    // still comes out quieter (4 dB at 1 kHz and 40 dB of fold).
    constexpr int kPoints = 256;
    const float ref = 0.25f;
    double sum = 0.0, sumSq = 0.0;
    for (int i = 0; i < kPoints; ++i) {
      const float x = ref * std::sin(2.0f * kPi * (float) i / (float) kPoints);
      const double v = (double) shaper.shape(x * drive + bias_);
      sum += v;
      sumSq += v * v;
    }
    const double mean = sum / kPoints;
    const double variance = sumSq / kPoints - mean * mean;
    const float rmsOut = (float) std::sqrt(variance > 0.0 ? variance : 0.0);
    const float rmsIn = ref * 0.70710678f;
    makeup_ = rmsOut > 1e-6f ? rmsIn / rmsOut : 1.0f;
    makeupGlide_.set(gainToDb(makeup_));
  }

  /** What the makeup gain was last measured for: update() skips the
   *  measurement while all four still match. */
  struct MadeFor {
    float drive = -1.0f, /**< linear drive; -1 measures on the first update() */ bias = 0.0f; ///< the bias
    Curve curve = Curve::Soft; ///< the curve
    bool autoGain = false;     ///< whether auto gain was on
  } madeFor_;
  Glide driveGlide_{0.0f, 0.0f}, makeupGlide_{0.0f, 0.0f}; // dB
  Glide biasGlide_{0.0f, 0.0f};
  float bias_ = 0.0f;

  Oversampled<Shaper, Stages> os_;
  DcBlocker blocker_;
  OnePole tone_;
  // Sized from the oversampler directly: the class's own latencySamples() is
  // not a constant expression until the class is complete.
  static constexpr int kWetLatency = Oversampled<Shaper, Stages>::latencySamples();
  CompensatedDryWetMixer<1, (kWetLatency > 0 ? kWetLatency : 1)> mixer_;
  float sr_ = 48000.0f, driveDb_ = 0.0f, toneHz_ = 20000.0f, makeup_ = 1.0f;
  bool autoGain_ = true;
};

} // namespace sonore
