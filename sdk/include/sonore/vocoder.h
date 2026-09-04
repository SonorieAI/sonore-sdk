// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: the channel vocoder and the formant filter.
//
// Both are banks of bandpass filters; what differs is who sets the levels.
// In the vocoder a second signal does -- the modulator's energy in each band
// is measured and imposed on the carrier's band, and a synth "talks". In the
// formant filter a vowel does -- five resonances at the frequencies a human
// vocal tract puts them, with the bandwidths and levels measured from
// singers, and a saw "sings" an A.
//
// The formant table is the classic one from the Csound manual (the soprano
// set, five formants per vowel, frequency / level / bandwidth), which traces
// back to the measurements used in the CHANT vocal synthesiser at IRCAM.
// It is here as data rather than as five hand-picked numbers because the
// third, fourth and fifth formants are what make a vowel sound like a voice
// rather than like a wah pedal, and nobody remembers them.
//
// One detail that decides whether the levels mean anything: the SVF's `bp`
// output is Simper's un-normalised band, whose peak gain is Q. A band at
// Q = 10 read straight off it sits 20 dB hot, and two in cascade 40 dB. Both
// banks here scale each stage by 1/Q so every band peaks at unity -- which is
// what makes the formant table's decibels come out as the decibels in the
// table, and the vocoder's output land at (modulator level × carrier level)
// instead of at a number nobody chose.
//
// Included by dsp.h.
#pragma once
#include <cmath>
#include "audio.h"

namespace sonore {

// ── Channel vocoder ──────────────────────────────────────────────────────────

/**
 * Bands are log-spaced across the range, constant-Q, each a pair of state-
 * variable bandpasses in cascade (a single one is too broad for speech to
 * stay intelligible). The modulator's per-band envelope drives the carrier's
 * per-band level; attack and release are the intelligibility knobs -- fast
 * enough to follow consonants, slow enough not to buzz on the pitch. The
 * bands meet at about -3 dB, so a flat carrier through open bands comes out
 * near flat.
 */
template <int Bands = 16>
class ChannelVocoder {
  static_assert(Bands >= 4 && Bands <= 64, "4..64 bands");

public:
  static constexpr int kBands = Bands;

  void setSampleRate(float sr) {
    sr_ = sr > 1.0f ? sr : 48000.0f;
    for (int b = 0; b < Bands; ++b) {
      for (int s = 0; s < 2; ++s) {
        mod_[b][s].setSampleRate(sr_);
        car_[b][s].setSampleRate(sr_);
      }
      env_[b].setSampleRate(sr_);
    }
    setRange(lowHz_, highHz_);
    setTimes(attackMs_, releaseMs_);
  }
  void setRange(float lowHz, float highHz) {
    lowHz_ = clampf(lowHz, 20.0f, sr_ * 0.4f);
    highHz_ = clampf(highHz, lowHz_ * 2.0f, sr_ * 0.45f);
    const float ratio = std::pow(highHz_ / lowHz_, 1.0f / (float) (Bands - 1));
    // Constant Q from the spacing: a band's width is the gap to its neighbours.
    const float q = std::sqrt(ratio) / (ratio - 1.0f);
    norm_ = 1.0f / q; // unity peak per SVF stage
    for (int b = 0; b < Bands; ++b) {
      const float f = lowHz_ * std::pow(ratio, (float) b);
      centre_[b] = f;
      for (int s = 0; s < 2; ++s) {
        mod_[b][s].set(f, q);
        car_[b][s].set(f, q);
      }
    }
  }
  void setTimes(float attackMs, float releaseMs) {
    attackMs_ = attackMs;
    releaseMs_ = releaseMs;
    for (int b = 0; b < Bands; ++b) env_[b].setTimes(attackMs, releaseMs);
  }
  void setOutputGain(float g) { gain_ = g; }
  void reset() {
    for (int b = 0; b < Bands; ++b) {
      for (int s = 0; s < 2; ++s) {
        mod_[b][s].reset();
        car_[b][s].reset();
      }
      env_[b].reset();
    }
  }
  float bandCentre(int b) const { return b >= 0 && b < Bands ? centre_[b] : 0.0f; }
  /** The modulator's envelope in band b, as of the last sample. */
  float bandLevel(int b) const { return b >= 0 && b < Bands ? env_[b].value() : 0.0f; }

  inline float process(float modulator, float carrier) {
    float out = 0.0f;
    for (int b = 0; b < Bands; ++b) {
      mod_[b][0].process(modulator);
      mod_[b][1].process(mod_[b][0].bp * norm_);
      const float level = env_[b].process(mod_[b][1].bp * norm_);
      car_[b][0].process(carrier);
      car_[b][1].process(car_[b][0].bp * norm_);
      out += car_[b][1].bp * norm_ * level;
    }
    return out * gain_;
  }

private:
  SVF mod_[Bands][2], car_[Bands][2];
  EnvFollower env_[Bands];
  float centre_[Bands]{};
  float sr_ = 48000.0f, lowHz_ = 80.0f, highHz_ = 8000.0f, attackMs_ = 5.0f, releaseMs_ = 30.0f;
  float gain_ = 1.0f, norm_ = 1.0f;
};

// ── Formant filter ───────────────────────────────────────────────────────────

/**
 * Five resonances per vowel; `setVowel` takes a position 0..4 across
 * A, E, I, O, U and interpolates the formants between neighbours, so a knob
 * sweeps through the vowels the way a mouth does. The first formant peaks at
 * unity; the rest sit at the table's levels below it.
 */
class FormantFilter {
public:
  enum Vowel { A = 0, E = 1, I = 2, O = 3, U = 4 };

  void setSampleRate(float sr) {
    sr_ = sr > 1.0f ? sr : 48000.0f;
    for (auto& f : filter_) f.setSampleRate(sr_);
    setVowel(position_);
  }
  /** 0..4: A, E, I, O, U, with the fractions in between morphed. */
  void setVowel(float position) {
    position_ = clampf(position, 0.0f, 4.0f);
    const int a = (int) position_;
    const int b = a < 4 ? a + 1 : 4;
    const float t = position_ - (float) a;
    for (int f = 0; f < kFormants; ++f) {
      // Frequencies and bandwidths interpolate geometrically, levels in dB:
      // a vowel halfway between two should sound halfway, not average to
      // a formant that belongs to neither.
      const float hz = kTable[a][f].hz * std::pow(kTable[b][f].hz / kTable[a][f].hz, t);
      const float bw = kTable[a][f].bw * std::pow(kTable[b][f].bw / kTable[a][f].bw, t);
      const float db = kTable[a][f].db + (kTable[b][f].db - kTable[a][f].db) * t;
      const float q = clampf(hz / bw, 0.5f, 60.0f);
      filter_[f].set(clampf(hz, 20.0f, sr_ * 0.45f), q);
      gain_[f] = dbToGain(db) / q; // unity peak, then the table's level
      centre_[f] = hz;
    }
  }
  void setVowel(Vowel v) { setVowel((float) v); }
  void reset() {
    for (auto& f : filter_) f.reset();
  }
  /** Formant f's centre as installed (interpolated), for the curious. */
  float formantHz(int f) const { return f >= 0 && f < kFormants ? centre_[f] : 0.0f; }

  inline float process(float x) {
    float out = 0.0f;
    for (int f = 0; f < kFormants; ++f) {
      filter_[f].process(x);
      out += filter_[f].bp * gain_[f];
    }
    return out;
  }

private:
  static constexpr int kFormants = 5;
  struct Formant { float hz, db, bw; };
  // Soprano, from the Csound manual's formant table.
  static constexpr Formant kTable[5][kFormants] = {
    {{800.0f, 0.0f, 80.0f}, {1150.0f, -6.0f, 90.0f}, {2900.0f, -32.0f, 120.0f}, {3900.0f, -20.0f, 130.0f}, {4950.0f, -50.0f, 140.0f}},  // A
    {{350.0f, 0.0f, 60.0f}, {2000.0f, -20.0f, 100.0f}, {2800.0f, -15.0f, 120.0f}, {3600.0f, -40.0f, 150.0f}, {4950.0f, -56.0f, 200.0f}}, // E
    {{270.0f, 0.0f, 60.0f}, {2140.0f, -12.0f, 90.0f}, {2950.0f, -26.0f, 100.0f}, {3900.0f, -26.0f, 120.0f}, {4950.0f, -44.0f, 120.0f}}, // I
    {{450.0f, 0.0f, 70.0f}, {800.0f, -11.0f, 80.0f}, {2830.0f, -22.0f, 100.0f}, {3800.0f, -22.0f, 130.0f}, {4950.0f, -50.0f, 135.0f}},  // O
    {{325.0f, 0.0f, 50.0f}, {700.0f, -16.0f, 60.0f}, {2700.0f, -35.0f, 170.0f}, {3800.0f, -40.0f, 180.0f}, {4950.0f, -60.0f, 200.0f}},  // U
  };

  SVF filter_[kFormants];
  float gain_[kFormants]{};
  float centre_[kFormants]{};
  float sr_ = 48000.0f, position_ = 0.0f;
};

} // namespace sonore
