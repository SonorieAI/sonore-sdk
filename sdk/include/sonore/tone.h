// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: the two tone controls every second plugin has, done once.
//
// A "Tone" knob is a tilt: one control that leans the whole spectrum, treble
// up and bass down or the reverse, pivoting on a frequency where nothing
// changes. FERRIC's tone control was exactly this and was written by hand,
// as every other one had been. It is an RBJ shelf pair from dsp.h with the
// right Q, and it is here so the right Q is chosen once: a shelf at Q = 0.71
// has no overshoot at the corner, which is what makes a tilt read 0 dB at its
// pivot instead of a bump there.
//
// Baxandall's 1952 bass/treble pair is the hi-fi tone stack, and here it is
// the NETWORK, not a pair of shelves drawn to look like it. The circuit is
// the symmetric feedback arrangement of his Wireless World article: an
// inverting amplifier whose input and feedback arms are the two halves of
// each pot, the bass pot bypassed by a capacitor across each half so it only
// divides at low frequencies, the treble pot coupled by a capacitor at each
// end so it only divides at high ones. With the wiper at the amplifier's
// virtual earth, each arm is an admittance from its end to the wiper, the
// arms of one side sum, and the gain is the ratio of the input side's sum to
// the feedback side's -- a rational function of s whose numerator and
// denominator are the two sides' polynomials multiplied out. That is what
// update() does, in double, and the bilinear transform turns the quartic
// into a fourth-order section (its own derivation, in tonedetail::bilinear).
//
// Two properties fall out of the circuit rather than being designed in, and
// the test asks for both: with both pots centred the two sides are
// identical and the response is EXACTLY flat at every frequency; and moving
// a pot from one end to the other swaps the two sides, so cut is the exact
// mirror of boost, |H_cut| = 1/|H_boost|, which no shelf pair delivers.
//
// Included by dsp.h.
#pragma once
#include <cmath>
#include "audio.h"

namespace sonore {

namespace tonedetail {

/** Multiply two polynomials in ascending powers. `out` holds na + nb - 1. */
inline void polyMul(const double* a, int na, const double* b, int nb, double* out) {
  for (int i = 0; i < na + nb - 1; ++i) out[i] = 0.0;
  for (int i = 0; i < na; ++i)
    for (int j = 0; j < nb; ++j) out[i + j] += a[i] * b[j];
}

/**
 * Bilinear transform of an analogue rational function of order N -- `b` and
 * `a` in ascending powers of s -- into a digital one in ascending powers of
 * z^-1, normalised so az[0] = 1. s = c (1 - z^-1)/(1 + z^-1); each s^k is
 * c^k (1 - z^-1)^k (1 + z^-1)^(N-k), expanded by repeated multiplication
 * rather than from a table of binomials, so any order up to 8 works. With
 * warpHz > 0 the frequency c is chosen so that frequency lands exactly.
 */
inline void bilinear(int order, const double* b, const double* a, double fs, double warpHz,
                     double* bz, double* az) {
  const double pi = 3.14159265358979323846;
  double c = 2.0 * fs;
  if (warpHz > 0.0) {
    const double w = 2.0 * pi * warpHz;
    c = w / std::tan(w / (2.0 * fs));
  }
  double numAcc[9] = {0.0}, denAcc[9] = {0.0};
  const double minus[2] = {1.0, -1.0}, plus[2] = {1.0, 1.0};
  for (int k = 0; k <= order; ++k) {
    // (1 - z)^k (1 + z)^(N-k), ascending.
    double term[9] = {1.0};
    int len = 1;
    double scratch[9];
    for (int i = 0; i < k; ++i) { polyMul(term, len, minus, 2, scratch); ++len; for (int j = 0; j < len; ++j) term[j] = scratch[j]; }
    for (int i = 0; i < order - k; ++i) { polyMul(term, len, plus, 2, scratch); ++len; for (int j = 0; j < len; ++j) term[j] = scratch[j]; }
    const double ck = std::pow(c, (double) k);
    for (int j = 0; j <= order; ++j) {
      numAcc[j] += b[k] * ck * term[j];
      denAcc[j] += a[k] * ck * term[j];
    }
  }
  const double norm = std::fabs(denAcc[0]) > 1e-300 ? 1.0 / denAcc[0] : 1.0;
  for (int j = 0; j <= order; ++j) {
    bz[j] = numAcc[j] * norm;
    az[j] = denAcc[j] * norm;
  }
}

} // namespace tonedetail

/**
 * Tilt equaliser. +6 dB of tilt is +3 dB at the top of the spectrum and -3 dB
 * at the bottom, with the pivot untouched; negative tilts the other way.
 */
class TiltEq {
public:
  void setSampleRate(float sr) {
    sr_ = sr > 1.0f ? sr : 48000.0f;
    low_.setSampleRate(sr_);
    high_.setSampleRate(sr_);
    update();
  }
  /** The frequency that does not move. 1 kHz is the convention. */
  void setPivot(float hz) { pivot_ = clampf(hz, 50.0f, 10000.0f); update(); }
  /** Total tilt in dB, end to end: the extremes move by half of this each. */
  void setTilt(float db) { tiltDb_ = clampf(db, -24.0f, 24.0f); update(); }
  void reset() { low_.reset(); high_.reset(); }

  inline float process(float x) { return high_.process(low_.process(x)); }

private:
  void update() {
    low_.lowShelf(pivot_, 0.7071f, -tiltDb_ * 0.5f);
    high_.highShelf(pivot_, 0.7071f, tiltDb_ * 0.5f);
  }
  Biquad low_, high_;
  float sr_ = 48000.0f, pivot_ = 1000.0f, tiltDb_ = 0.0f;
};

/**
 * The Baxandall network. Pots run 0..1 (cut .. boost, 0.5 flat);
 * setBass/setTreble take decibels and place the pot where the circuit's own
 * asymptote reaches them. The default parts give +/-21 dB of bass at DC with
 * a 145 Hz turnover and +/-14 dB of treble at the top, which is the classic
 * hi-fi range; a different flavour is a different set of parts.
 */
class Baxandall {
public:
  /** Ohms and farads. rIn and rFeedback are the fixed arms, rBass/rTreble
   *  the pots, cBass across each half of the bass pot, cTreble in series with
   *  each end of the treble pot, rTrebleEnd the stop that bounds the treble. */
  struct Components {
    float rIn = 10e3f, rFeedback = 10e3f;
    float rBass = 100e3f, cBass = 22e-9f;
    float rTreble = 100e3f, cTreble = 4.7e-9f, rTrebleEnd = 2.2e3f;
  };

  void setSampleRate(float sr) {
    sr_ = sr > 1.0f ? sr : 48000.0f;
    update();
  }
  void setComponents(const Components& c) {
    parts_ = c;
    update();
  }
  /** Pot positions, 0..1: cut, flat at 0.5, boost. */
  void setBassPot(float p) { bass_ = clampf(p, 0.0f, 1.0f); update(); }
  void setTreblePot(float p) { treble_ = clampf(p, 0.0f, 1.0f); update(); }
  /** Decibels AT 30 Hz: the pot is placed by bisection on the circuit's own
   *  response, so the number on the knob is what a meter reads at the bottom
   *  of the band rather than an asymptote the shelf never quite reaches. The
   *  response is monotone in the pot, which is what makes bisection exact. */
  void setBass(float db) { bass_ = seek(true, clampf(db, -30.0f, 30.0f), 30.0); update(); }
  /** Decibels at 15 kHz, the same way. */
  void setTreble(float db) { treble_ = seek(false, clampf(db, -30.0f, 30.0f), 15000.0); update(); }
  float bassPot() const { return bass_; }
  float treblePot() const { return treble_; }
  void reset() { z1_ = z2_ = z3_ = z4_ = 0.0; }

  inline float process(float x) {
    // Fourth-order transposed direct form II, in double.
    const double in = x;
    const double y = b_[0] * in + z1_;
    z1_ = b_[1] * in - a_[1] * y + z2_;
    z2_ = b_[2] * in - a_[2] * y + z3_;
    z3_ = b_[3] * in - a_[3] * y + z4_;
    z4_ = b_[4] * in - a_[4] * y;
    return (float) y;
  }

  /** |H(jw)| of the circuit in dB, from the same polynomials the digital
   *  filter came from. */
  double analogMagnitudeDb(double hz) const {
    const double w = 2.0 * 3.14159265358979323846 * hz;
    double nr = 0.0, ni = 0.0, dr = 0.0, di = 0.0;
    // (jw)^k: k even -> real (+/-), odd -> imaginary (+/-).
    double p = 1.0;
    for (int k = 0; k <= 4; ++k) {
      switch (k & 3) {
        case 0: nr += sb_[k] * p; dr += sa_[k] * p; break;
        case 1: ni += sb_[k] * p; di += sa_[k] * p; break;
        case 2: nr -= sb_[k] * p; dr -= sa_[k] * p; break;
        default: ni -= sb_[k] * p; di -= sa_[k] * p; break;
      }
      p *= w;
    }
    const double mag = std::sqrt((nr * nr + ni * ni) / (dr * dr + di * di));
    return 20.0 * std::log10(mag > 1e-12 ? mag : 1e-12);
  }

private:
  /** The pot position at which the circuit reads `db` at `hz`, by bisection
   *  on the analogue polynomials (the other pot held where it is). */
  float seek(bool bassPot, float db, double hz) {
    const float keepBass = bass_, keepTreble = treble_;
    double lo = 0.0, hi = 1.0;
    for (int i = 0; i < 40; ++i) {
      const double mid = 0.5 * (lo + hi);
      if (bassPot) bass_ = (float) mid; else treble_ = (float) mid;
      buildAnalog();
      if (analogMagnitudeDb(hz) < (double) db) lo = mid; else hi = mid;
    }
    bass_ = keepBass;
    treble_ = keepTreble;
    return (float) (0.5 * (lo + hi));
  }

  void update() {
    buildAnalog();
    // The amplifier inverts; a tone control in a plugin should not.
    tonedetail::bilinear(4, sb_, sa_, (double) sr_, 0.0, b_, a_);
  }

  void buildAnalog() {
    const double rIn = parts_.rIn, rFb = parts_.rFeedback, rB = parts_.rBass, cB = parts_.cBass;
    const double rT = parts_.rTreble, cT = parts_.cTreble, rE = parts_.rTrebleEnd;
    // Pot halves. Position 1 puts the wiper at the input end: the input arm
    // is short (R_u = 0) and the feedback arm long, which is boost.
    const double rU = (1.0 - bass_) * rB, rL = bass_ * rB;
    const double rTu = (1.0 - treble_) * rT, rTl = treble_ * rT;
    // Each arm as numerator/denominator in s. Bass: 1/(R + R_half || 1/sC)
    // = (1 + s R_half C)/(R + R_half + s R R_half C). Treble: sC/(1 + sC(R_e + R_half)).
    const double nbIn[2] = {1.0, rU * cB}, dbIn[2] = {rIn + rU, rIn * rU * cB};
    const double nbOut[2] = {1.0, rL * cB}, dbOut[2] = {rFb + rL, rFb * rL * cB};
    const double ntIn[2] = {0.0, cT}, dtIn[2] = {1.0, cT * (rE + rTu)};
    const double ntOut[2] = {0.0, cT}, dtOut[2] = {1.0, cT * (rE + rTl)};
    // Y_in = (nbIn dtIn + ntIn dbIn) / (dbIn dtIn), Y_out likewise;
    // H = Y_in / Y_out = (numIn * denOut) / (denIn * numOut).
    double t1[3], t2[3], numIn[3], denIn[3], numOut[3], denOut[3];
    tonedetail::polyMul(nbIn, 2, dtIn, 2, t1);
    tonedetail::polyMul(ntIn, 2, dbIn, 2, t2);
    for (int i = 0; i < 3; ++i) numIn[i] = t1[i] + t2[i];
    tonedetail::polyMul(dbIn, 2, dtIn, 2, denIn);
    tonedetail::polyMul(nbOut, 2, dtOut, 2, t1);
    tonedetail::polyMul(ntOut, 2, dbOut, 2, t2);
    for (int i = 0; i < 3; ++i) numOut[i] = t1[i] + t2[i];
    tonedetail::polyMul(dbOut, 2, dtOut, 2, denOut);
    tonedetail::polyMul(numIn, 3, denOut, 3, sb_);
    tonedetail::polyMul(denIn, 3, numOut, 3, sa_);
  }

  Components parts_{};
  float sr_ = 48000.0f, bass_ = 0.5f, treble_ = 0.5f;
  double sb_[5]{}, sa_[5]{};
  double b_[5] = {1, 0, 0, 0, 0}, a_[5] = {1, 0, 0, 0, 0};
  double z1_ = 0, z2_ = 0, z3_ = 0, z4_ = 0;
};

} // namespace sonore
