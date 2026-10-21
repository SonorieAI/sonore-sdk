// SPDX-License-Identifier: Apache-2.0
// Sonore SDK. The dynamic equaliser: a band that compresses itself.
//
// The modern mixing tool this library kept ALMOST having: eq.h knows how to
// make a matched bell, compressors.h knows how a gain computer behaves, and
// a dynamic EQ is exactly their composition. Per band, a detector listens
// to the band's own slice of the spectrum (of the input, or of a sidechain
// key), and the measured level drives the band's gain through the standard
// compressor law, Giannoulis, Massberg & Reiss, "Digital Dynamic Range
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
#ifndef SONORE_DSP_H_INCLUDED
#error "dynamic_eq.h is part of dsp.h and needs what it includes first: include <sonore/dsp.h> instead."
#endif
#include <cmath>
#include "audio.h"

namespace sonore {

/**
 * An equaliser of up to `Bands` bands, each of which can compress or expand
 * its own slice of the spectrum (see the file comment for the design).
 *
 * One channel, per sample; for linked stereo give both instances the same
 * key through process(x, key). Call setSampleRate() in prepare(), then set
 * the bands with setBand(): a band does nothing until its Band says
 * `enabled`. The setters and process() run on the audio thread. The dynamic
 * gain is worked out every 16 samples and a band's filter redesigned only
 * when that gain moved by more than 0.05 dB; setBand() itself redesigns at
 * once. gainDb() reads back each band's dynamic gain, for a meter.
 */
template<int Bands = 4>
class DynamicEq {
public:
  /** One band's settings. Frequency in Hz; static gain, threshold, knee and
   *  range in dB; attack and release in ms. The ratio applies to the Bell,
   *  LowShelf and HighShelf shapes only, and at 1 (the default) the band is
   *  a static EQ band at staticGainDb. A downward band cuts when its slice
   *  is louder than the threshold, an upward one lifts when it is quieter,
   *  and either way the movement stops at rangeDb. */
  struct Band {
    eqdetail::Shape shape = eqdetail::Shape::Bell; ///< eq.h's shapes; only Bell and the shelves move
    float hz = 1000.0f;          ///< centre or corner in Hz, held to 10 Hz .. 0.45 x the rate
    float q = 1.0f;              ///< the band's Q, at least 0.05; the bell's detector uses it too
    float staticGainDb = 0.0f;   ///< the gain at rest, in dB; the dynamic gain adds to it
    float thresholdDb = -24.0f;  ///< dBFS of the band's slice, as its peak detector reads it
    float ratio = 1.0f;          ///< 1 = a static band; above 1 (a 1.001 margin) the band moves
    float kneeDb = 6.0f;         ///< soft-knee width in dB around the threshold; 0 is a hard knee
    float attackMs = 5.0f;       ///< ms to 63% of a move toward more gain change, at least 0.05
    float releaseMs = 80.0f;     ///< ms to 63% of the way back, at least 1
    float rangeDb = 12.0f;       ///< the dynamic movement never exceeds this; keep it 0 or more
    bool upward = false;         ///< lift when quiet instead of cut when loud
    bool enabled = false;        ///< false: the band is skipped entirely, its filter not run
  };

  /** The rate in Hz; one of 1 or less, or NaN, is taken as 48000. Every
   *  band's filter, detector and ballistics are designed again for it; the
   *  filters' state and the dynamic gains are kept (reset() clears them). */
  void setSampleRate(float sr) {
    sr_ = sr > 1.0f ? sr : 48000.0f;
    for (int i = 0; i < Bands; ++i) {
      filter_[i].setSampleRate(sr_);
      det_[i].setSampleRate(sr_);
      configure(i);
    }
    counter_ = 0;
  }
  /** Band `i`, 0-based; an index outside 0..Bands-1 is ignored. The filter,
   *  detector and ballistics are redesigned at once, and the band's current
   *  envelope and dynamic gain carry on from where they were. Allocation-free. */
  void setBand(int i, const Band& b) {
    if (i < 0 || i >= Bands) return;
    band_[i] = b;
    configure(i);
  }
  /** Silence: filter and detector state, envelopes and dynamic gains to
   *  zero, each band back at its static gain. The bands' settings stay. */
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

  /** [audio-thread] One sample, equalised, with the input as its own key.
   *  Allocation-free. */
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
    // Detector: the band's own slice of the spectrum, designed and run in
    // double like the band itself. In a float biquad a low band's detector
    // read the level wrong as the rate rose -- a 40 Hz low shelf's lowpass
    // was -0.6 dB in its passband at 192 kHz and +2.3 dB at 384 kHz, so the
    // band's threshold sat that far from the number on it.
    if (b.shape == eqdetail::Shape::LowShelf)
      det_[i].lowpass((double) b.hz, 0.70710678);
    else if (b.shape == eqdetail::Shape::HighShelf)
      det_[i].highpass((double) b.hz, 0.70710678);
    else
      bandpass(det_[i], (double) b.hz, (double) b.q); // RBJ 0 dB-peak variant: |H(f0)| = 1
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
        // Upward: the mirrored law; lift grows as the band falls below
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

  /** The cookbook's constant-0 dB-peak bandpass, which IirSection does not
   *  name, with Biquad's limits: 10 Hz .. 0.49 fs (a NaN to the bottom, as
   *  clampf sends it) and Q at least 0.05. */
  void bandpass(IirSection& s, double hz, double q) const {
    const double fs = (double) sr_;
    const double f = hz >= 10.0 ? (hz > 0.49 * fs ? 0.49 * fs : hz) : 10.0;
    const double w0 = 2.0 * 3.14159265358979323846 * f / fs;
    const double alpha = std::sin(w0) / (2.0 * (q >= 0.05 ? q : 0.05));
    const double a0 = 1.0 + alpha;
    s.setCoefficients(alpha / a0, 0.0, -alpha / a0, -2.0 * std::cos(w0) / a0, (1.0 - alpha) / a0);
  }

  void design(int i) {
    const Band& b = band_[i];
    const float gain = b.staticGainDb + (dynamic(i) ? dynDb_[i] : 0.0f);
    designedDb_[i] = dynDb_[i];
    // The SAME design ParametricEq uses, cookbook fallback and clamps
    // included -- see eqdetail::designBand for what a private copy got wrong.
    eqdetail::designBand(filter_[i], b.shape, b.hz, b.q, gain, sr_);
  }

  Band band_[Bands];
  IirSection filter_[Bands]; // double: a low band at a high rate needs it (see eq.h)
  IirSection det_[Bands];    // ...and so does its detector
  float env_[Bands]{}, envRelease_[Bands]{}, att_[Bands]{}, rel_[Bands]{};
  float dynDb_[Bands]{}, designedDb_[Bands]{};
  float sr_ = 48000.0f;
  int counter_ = 0;
};

} // namespace sonore
