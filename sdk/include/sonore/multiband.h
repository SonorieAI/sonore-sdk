// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: multiband dynamics, wired once.
//
// crossover.h splits into bands that sum flat and compressors.h has the
// gain computers; what every multiband plugin then rewrites is the wiring --
// two splitters for stereo, a stereo-linked detector per band, downward AND
// upward compression per band (the OTT shape that made "multiband" a
// household word), per-band bypass and solo, and a gain-reduction meter per
// band for the faceplate. Rewired by hand it comes out with the bands
// detected separately on the two channels (the image wanders), or with the
// upward stage's floor unbounded (a fade-out ends in a roar), or without the
// meters. This is the wiring.
//
// Per band, downward compression through a VcaCompressor (the exact,
// clean gain law) and upward compression through dynamics.h's
// UpwardCompressor, both from ONE stereo-linked peak detector, the gains
// multiplied. The bands then sum, and because the splitter's bands sum flat,
// a band at ratio 1 is transparent: the unit test asks for the sum to be
// the input to 0.05 dB with every band idle.
//
// Included by dsp.h.
#pragma once
#ifndef SONORE_DSP_H_INCLUDED
#error "multiband.h is part of dsp.h and needs what it includes first: include <sonore/dsp.h> instead."
#endif
#include <cmath>
#include "audio.h"

namespace sonore {

/**
 * Stereo multiband compression, downward and upward in every band, over a
 * MultibandSplitter whose bands sum flat (see the file comment for the
 * wiring and why).
 *
 * Call prepare() before audio; it applies every band's settings for the
 * rate and clears the state. Until setCrossover() the splits are the
 * splitter's, log-spaced between 100 Hz and 8 kHz. setCrossover(),
 * setBand() and process() -- one stereo frame -- run on the audio thread.
 * Each band's detector is stereo-linked, so the image stays put, and keeps
 * running while the band is bypassed; soloing any band mutes the bands not
 * soloed. gainChangeDb() is the faceplate's meter, makeup included.
 */
template <int Bands = 3>
class MultibandDynamics {
  static_assert(Bands >= 2 && Bands <= 8, "2..8 bands");

public:
  /** One band's settings. The downward stage takes the threshold, ratio
   *  (N for N:1), knee, attack, release and makeup, in dB and ms; the upward
   *  stage shares the knee and the timing and has its own threshold, ratio
   *  and largest boost. With both ratios at 1, the default, the band passes
   *  unchanged. */
  struct BandSettings {
    /** The downward stage's threshold, in dBFS of the linked peak level. */
    float thresholdDb = -20.0f, ratio = 1.0f, /**< downward N:1; under 1, or NaN, is 1 */ kneeDb = 6.0f; ///< knee width in dB, both stages; negative is 0
    /** The detectors' attack time constant in ms, both stages. */
    float attackMs = 10.0f, releaseMs = 100.0f, /**< the release time constant in ms, both stages */ makeupDb = 0.0f; ///< dB on the band; glides in, and the meter includes it
    /** Upward compression below this, by this ratio, up to this much boost. */
    float upwardThresholdDb = -60.0f, upwardRatio = 1.0f, /**< upward N:1; 1 is off */ upwardMaxDb = 12.0f; ///< the largest upward boost in dB; negative is 0
    /** Bypass: the band at unity, makeup too, its detector still running. */
    bool bypass = false, solo = false; ///< any band soloed mutes every band not soloed
  };

  /** Take spec.sampleRate, apply every band's settings for it and clear the
   *  state (reset()). Nothing is allocated. */
  void prepare(const ProcessSpec& spec) {
    sr_ = (float) spec.sampleRate;
    splitL_.setSampleRate(sr_);
    splitR_.setSampleRate(sr_);
    for (int b = 0; b < Bands; ++b) {
      down_[b].setSampleRate(sr_);
      down_[b].setDetector(VcaCompressor::Detector::Peak);
      up_[b].setSampleRate(sr_);
      apply(b);
    }
    reset();
  }
  /** Silence: the splitters' filters and every band's detector cleared, the
   *  gliding settings landed. The settings stay. */
  void reset() {
    splitL_.reset();
    splitR_.reset();
    for (int b = 0; b < Bands; ++b) {
      down_[b].reset();
      up_[b].reset();
    }
  }
  /** Crossover k (ascending) in Hz. */
  void setCrossover(int k, float hz) {
    splitL_.setCrossover(k, hz);
    splitR_.setCrossover(k, hz);
  }
  /** Band `b`, 0-based from the lowest; an index outside 0..Bands-1 is
   *  ignored. Applied at once: threshold, ratio, makeup and the upward
   *  boost glide once audio runs, the knee and timing change in one sample.
   *  Allocation-free. */
  void setBand(int b, const BandSettings& s) {
    if (b < 0 || b >= Bands) return;
    settings_[b] = s;
    apply(b);
  }
  /** The settings band `b` was last given (the defaults until then); an
   *  index outside 0..Bands-1 reads the nearest band. */
  const BandSettings& band(int b) const { return settings_[b < 0 ? 0 : (b >= Bands ? Bands - 1 : b)]; }
  /** The band's gain change in dB as of the last sample: negative is
   *  reduction, positive is the upward stage boosting. */
  float gainChangeDb(int b) const { return b >= 0 && b < Bands ? gainDb_[b] : 0.0f; }

  /** [audio-thread] One stereo frame, in place: split, each band's linked
   *  gain, solo, sum. Allocation-free. */
  inline void process(float& left, float& right) {
    float l[Bands], r[Bands];
    splitL_.process(left, l);
    splitR_.process(right, r);
    bool anySolo = false;
    for (int b = 0; b < Bands; ++b) anySolo = anySolo || settings_[b].solo;
    float outL = 0.0f, outR = 0.0f;
    for (int b = 0; b < Bands; ++b) {
      // Stereo-linked: one detector hears the louder side, both sides get
      // the same gain, and the image stays where it was.
      const float key = std::fabs(l[b]) > std::fabs(r[b]) ? std::fabs(l[b]) : std::fabs(r[b]);
      // The detectors run whether or not the band is bypassed. Skipped while
      // it was, they froze, and un-bypassing applied the gain reduction from
      // before: 25 dB of it, on a signal by then 40 dB under threshold.
      const float dynamic = down_[b].computeGain(key) * up_[b].computeGain(key);
      float g = 1.0f;
      if (!settings_[b].bypass) {
        g = dynamic;
        gainDb_[b] = -down_[b].gainReduction() + up_[b].gainChange() + settings_[b].makeupDb;
      } else {
        gainDb_[b] = 0.0f;
      }
      if (anySolo && !settings_[b].solo) continue;
      outL += l[b] * g;
      outR += r[b] * g;
    }
    left = outL;
    right = outR;
  }

private:
  void apply(int b) {
    const BandSettings& s = settings_[b];
    down_[b].setThreshold(s.thresholdDb);
    down_[b].setRatio(s.ratio);
    down_[b].setKnee(s.kneeDb);
    down_[b].setAttack(s.attackMs);
    down_[b].setRelease(s.releaseMs);
    down_[b].setMakeup(s.makeupDb);
    up_[b].setThreshold(s.upwardThresholdDb);
    up_[b].setRatio(s.upwardRatio);
    up_[b].setKnee(s.kneeDb);
    up_[b].setMaxGain(s.upwardMaxDb);
    up_[b].setAttack(s.attackMs);
    up_[b].setRelease(s.releaseMs);
  }

  MultibandSplitter<Bands> splitL_, splitR_;
  VcaCompressor down_[Bands];
  UpwardCompressor up_[Bands];
  BandSettings settings_[Bands];
  float gainDb_[Bands]{};
  float sr_ = 48000.0f;
};

} // namespace sonore
