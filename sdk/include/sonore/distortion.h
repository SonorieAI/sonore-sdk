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
#include <cmath>
#include "audio.h"

namespace sonore {

template<int Stages = 3>
class Distortion {
public:
  enum class Curve { Soft, Tanh, Hard, Cubic, Asymmetric, Fold };

  void setSampleRate(float sr) {
    sr_ = sr > 1.0f ? sr : 48000.0f;
    blocker_.setSampleRate(sr_);
    tone_.setSampleRate(sr_);
    setTone(toneHz_);
    update();
  }
  /** 0 .. 40 dB into the curve. */
  void setDrive(float db) { driveDb_ = clampf(db, 0.0f, 40.0f); update(); }
  void setCurve(Curve c) { os_.shaper().curve = c; update(); }
  /** A DC push before the curve: even harmonics from an odd shape. */
  void setBias(float bias) { os_.shaper().bias = clampf(bias, -0.5f, 0.5f); update(); }
  /** A lowpass after the curve, the "tone" knob: 1 kHz .. 20 kHz. */
  void setTone(float hz) {
    toneHz_ = clampf(hz, 1000.0f, 20000.0f);
    tone_.setCutoff(toneHz_ < sr_ * 0.45f ? toneHz_ : sr_ * 0.45f);
  }
  void setMix(float mix) { mixer_.setMix(mix); }
  /** Hold a -12 dBFS sine at its RMS whatever the drive (default on). */
  void setAutoGain(bool on) { autoGain_ = on; update(); }
  void reset() {
    os_.reset();
    blocker_.reset();
    tone_.reset();
  }
  static constexpr int factor() { return 1 << Stages; }
  static constexpr int latencySamples() { return Oversampled<Shaper, Stages>::latencySamples(); }

  inline float process(float x) {
    float y = os_.process(x);
    y = blocker_.process(y) * makeup_;
    y = tone_.lp(y);
    return mixer_.process(x, y);
  }

private:
  struct Shaper {
    Curve curve = Curve::Soft;
    float drive = 1.0f, bias = 0.0f;
    inline float process(float x) const { return shape(x * drive + bias); }
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
    os_.shaper().drive = dbToGain(driveDb_);
    if (!autoGain_) { makeup_ = 1.0f; return; }
    // One cycle of a -12 dBFS sine through the curve, DC removed (the
    // blocker does the same to the signal), RMS matched to the sine's.
    constexpr int kPoints = 64;
    const float ref = 0.25f;
    float y[kPoints];
    float mean = 0.0f;
    for (int i = 0; i < kPoints; ++i) {
      const float x = ref * std::sin(2.0f * kPi * (float) i / (float) kPoints);
      y[i] = os_.shaper().process(x);
      mean += y[i];
    }
    mean /= (float) kPoints;
    float power = 0.0f;
    for (int i = 0; i < kPoints; ++i) power += (y[i] - mean) * (y[i] - mean);
    const float rmsOut = std::sqrt(power / (float) kPoints);
    const float rmsIn = ref * 0.70710678f;
    makeup_ = rmsOut > 1e-6f ? rmsIn / rmsOut : 1.0f;
  }

  Oversampled<Shaper, Stages> os_;
  DcBlocker blocker_;
  OnePole tone_;
  DryWetMixer mixer_;
  float sr_ = 48000.0f, driveDb_ = 0.0f, toneHz_ = 20000.0f, makeup_ = 1.0f;
  bool autoGain_ = true;
};

} // namespace sonore
