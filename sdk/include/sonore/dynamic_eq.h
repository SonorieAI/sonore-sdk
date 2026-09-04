// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: the dynamic equaliser: a band that compresses itself.
//
// The modern mixing tool this library kept ALMOST having: eq.h knows how to
// make a matched bell, compressors.h knows how a gain computer behaves, and
// a dynamic EQ is exactly their composition. Per band, a detector listens
// to the band's own slice of the spectrum (of the input, or of a sidechain
// key), and the measured level drives the band's gain through the standard
// compressor law: Giannoulis, Massberg & Reiss, "Digital Dynamic Range
// Compressor Design: A Tutorial and Analysis" (JAES 2012): rectified
// envelope, soft knee in the log domain, dB-domain attack/release smoothing
// of the control signal. The band itself is eq.h's Vicanek matched biquad,
// redesigned at control rate as the gain moves.
//
// Why this beats a multiband compressor for surgical work: a crossover
// splits and re-sums the WHOLE signal, so its band edges are in the sound
// even at rest; a dynamic EQ is bell-shaped gain on the intact signal: at
// rest (ratio 1, static gain 0) a band is exactly unity.
//
// Each band works downward (cut when its slice is loud: resonance taming,
// de-essing) or upward (lift when it is quiet), always through a RANGE cap,
// because an uncapped upward band on silence is a noise amplifier.
//
// The detector for a bell is the RBJ constant-0dB-peak bandpass at the same
// frequency and Q, so the level it reads IS the level in the band; shelf
// bands detect through the matching low/highpass. Dynamics apply to Bell,
// LowShelf and HighShelf bands; pass/notch shapes are static by nature.
//
// ONE PER CHANNEL; for linked stereo, feed both instances the same key.
//
// Included by dsp.h.
#pragma once
#include <cmath>
#include "audio.h"

namespace sonore {

template<int Bands = 4>
class DynamicEq {
public:
  struct Band {
    eqdetail::Shape shape = eqdetail::Shape::Bell;
    float hz = 1000.0f;
    float q = 1.0f;
    float staticGainDb = 0.0f;
    float thresholdDb = -24.0f;
    float ratio = 1.0f;          // 1 = a static band
    float kneeDb = 6.0f;
    float attackMs = 5.0f;
    float releaseMs = 80.0f;
    float rangeDb = 12.0f;       // the dynamic movement never exceeds this
    bool upward = false;         // lift when quiet instead of cut when loud
    bool enabled = false;
  };

  void setSampleRate(float sr) {
    sr_ = sr > 1.0f ? sr : 48000.0f;
    for (int i = 0; i < Bands; ++i) {
      filter_[i].setSampleRate(sr_);
      det_[i].setSampleRate(sr_);
      configure(i);
    }
    counter_ = 0;
  }
  void setBand(int i, const Band& b) {
    if (i < 0 || i >= Bands) return;
    band_[i] = b;
    configure(i);
  }
  void reset() {
    for (int i = 0; i < Bands; ++i) {
      filter_[i].reset();
      det_[i].reset();
      env_[i] = 0.0f;
      dynDb_[i] = 0.0f;
      designedDb_[i] = 1e9f;
      design(i);
    }
    counter_ = 0;
  }
  /** The band's current dynamic gain in dB (negative = cutting). */
  float gainDb(int i) const { return i >= 0 && i < Bands ? dynDb_[i] : 0.0f; }

  inline float process(float x) { return process(x, x); }
  /** `key` feeds the detectors (sidechain); `x` is what gets equalised. */
  inline float process(float x, float key) {
    for (int i = 0; i < Bands; ++i) {
      if (!band_[i].enabled || !dynamic(i)) continue;
      const float d = std::fabs(det_[i].process(key));
      // Instant attack, band-period-aware release: the envelope's only job
      // is to hold the rectified sine still; the USER ballistics live on
      // the control signal below, where the JAES tutorial puts them.
      env_[i] = flushDenormal(d > env_[i] ? d : env_[i] * envRelease_[i]);
    }
    if (++counter_ >= kUpdateEvery) {
      counter_ = 0;
      update();
    }
    for (int i = 0; i < Bands; ++i)
      if (band_[i].enabled) x = filter_[i].process(x);
    return x;
  }

private:
  static constexpr int kUpdateEvery = 16;

  bool dynamic(int i) const {
    if (band_[i].ratio <= 1.001f) return false;
    const eqdetail::Shape s = band_[i].shape;
    return s == eqdetail::Shape::Bell || s == eqdetail::Shape::LowShelf ||
           s == eqdetail::Shape::HighShelf;
  }

  void configure(int i) {
    const Band& b = band_[i];
    // Detector: the band's own slice of the spectrum.
    if (b.shape == eqdetail::Shape::LowShelf)
      det_[i].lowpass(b.hz, 0.7071f);
    else if (b.shape == eqdetail::Shape::HighShelf)
      det_[i].highpass(b.hz, 0.7071f);
    else
      det_[i].bandpass(b.hz, b.q); // RBJ 0 dB-peak variant: |H(f0)| = 1
    // Envelope release: at least three cycles of the band, or a low band's
    // own ripple reads as gain modulation.
    const float envMs = 3000.0f / (b.hz > 20.0f ? b.hz : 20.0f);
    const float ms = envMs > 10.0f ? envMs : 10.0f;
    envRelease_[i] = std::exp(-1.0f / (0.001f * ms * sr_));
    // Control-signal ballistics run at the UPDATE rate (the eq.h lesson:
    // set them up at sr/16 or the declared times are silently sixteenfold).
    const float ur = sr_ / (float) kUpdateEvery;
    att_[i] = std::exp(-1.0f / (0.001f * (b.attackMs > 0.05f ? b.attackMs : 0.05f) * ur));
    rel_[i] = std::exp(-1.0f / (0.001f * (b.releaseMs > 1.0f ? b.releaseMs : 1.0f) * ur));
    designedDb_[i] = 1e9f;
    design(i);
  }

  void update() {
    for (int i = 0; i < Bands; ++i) {
      const Band& b = band_[i];
      if (!b.enabled || !dynamic(i)) continue;
      const float levelDb = gainToDbFloor(env_[i]);
      float want;
      if (!b.upward) {
        // Downward: the standard computer, then the range cap.
        float gr = -compdetail::kneeGainDb(levelDb - b.thresholdDb, 1.0f / b.ratio - 1.0f,
                                           b.kneeDb);
        if (gr > b.rangeDb) gr = b.rangeDb;
        want = -gr;
      } else {
        // Upward: the mirrored law: lift grows as the band falls below
        // the threshold, capped where told.
        float lift = compdetail::kneeGainDb(b.thresholdDb - levelDb, 1.0f - 1.0f / b.ratio,
                                            b.kneeDb);
        if (lift > b.rangeDb) lift = b.rangeDb;
        want = lift;
      }
      // dB-domain smoothing: attack toward more action, release back.
      const bool toward = std::fabs(want) > std::fabs(dynDb_[i]);
      dynDb_[i] = flushDenormal(want + (toward ? att_[i] : rel_[i]) * (dynDb_[i] - want));
      if (std::fabs(dynDb_[i] - designedDb_[i]) > 0.05f) design(i);
    }
  }

  void design(int i) {
    const Band& b = band_[i];
    const float gain = b.staticGainDb + (dynamic(i) ? dynDb_[i] : 0.0f);
    designedDb_[i] = dynDb_[i];
    float b0, b1, b2, a1, a2;
    if (eqdetail::matched(b.shape, b.hz, b.q, gain, sr_, &b0, &b1, &b2, &a1, &a2))
      filter_[i].setCoefficients(b0, b1, b2, a1, a2);
    else
      filter_[i].setCoefficients(1.0f, 0.0f, 0.0f, 0.0f, 0.0f);
  }

  Band band_[Bands];
  Biquad filter_[Bands], det_[Bands];
  float env_[Bands]{}, envRelease_[Bands]{}, att_[Bands]{}, rel_[Bands]{};
  float dynDb_[Bands]{}, designedDb_[Bands]{};
  float sr_ = 48000.0f;
  int counter_ = 0;
};

} // namespace sonore
