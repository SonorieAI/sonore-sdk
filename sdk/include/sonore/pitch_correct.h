// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: pitch correction: detect, decide, shift.
//
// "Autotune" is three things glued: YIN finds the pitch, a scale says where
// it should be, and the voice is moved there. The glue is where it goes
// wrong -- a corrector that snaps on every frame warbles, one that trusts a
// low-confidence estimate on a breath jumps an octave, one that decides in
// hertz instead of cents snaps unevenly. So: decisions are made in cents
// against a twelve-bit scale mask, only when the detector is confident, and
// the correcting ratio glides to its target over a retune time -- zero is
// the robot, a hundred milliseconds is a singer.
//
// The moving is pitch-synchronous overlap-add (PSOLA), the method made for
// one voice: the input is cut into grains two periods long, centred on its
// own pitch marks, and laid down again a period divided by the ratio apart.
// Every harmonic moves by exactly the ratio, at any rate, and the formants
// stay where the throat put them. It replaced a phase vocoder with a fixed
// 2048-point frame, whose bins were too wide for a low voice: at 96 kHz a
// voice at 112 Hz has its harmonics under three bins apart, and the
// vocoder's phase locking moved them all by the fundamental's offset instead
// of scaling them -- harmonics 2 to 5 landed 16 to 25 cents sharp of the
// note it aimed at, a 63 Hz voice at 48 kHz 17 to 23 cents, and at 384 kHz
// the fundamental was lost.
//
// Latency is two and a half of the longest period the corrector handles --
// 42 ms with the default 60 Hz floor, the same at every rate -- and the
// detector's hop is what the correction lags the pitch by, which is the
// trade every real-time corrector makes.
//
// The detector hears the input decimated to about 48 kHz, whatever the
// session runs at. Its window is a fixed 2048 samples and holds a period of
// at most 1022: 47 Hz at 48 kHz, but 94 at 96 and 188 at 192 -- above the
// corrector's own 60 Hz floor, so a low voice at a high rate was misread
// at the longest lag there was and "corrected" to the wrong note.
//
// Included by dsp.h.
#pragma once
#ifndef SONORE_DSP_H_INCLUDED
#error "pitch_correct.h is part of dsp.h and needs what it includes first: include <sonore/dsp.h> instead."
#endif
#include <cmath>
#include <cstdint>
#include <vector>
#include "audio.h"

namespace sonore {

/**
 * SIZE: the detector (~70 KB, a member) and the shifter's buffers, about
 * seven of the longest period long, allocated by setSampleRate().
 */
class PitchCorrector {
public:
  /** The scale mask with all twelve pitch classes allowed: every note is a
   *  target, so the voice goes to the nearest semitone. The default. */
  static constexpr uint16_t kChromatic = 0x0FFF;
  /** Pitch-class masks from C = bit 0: a major scale rooted at `root`
   *  (0 = C). */
  static uint16_t majorScale(int root) { return rotate(0x0AB5, root); }
  /** The natural minor scale rooted at `root` (0 = C, 9 = A; any integer,
   *  taken modulo 12), as a mask for setScale(). */
  static uint16_t minorScale(int root) { return rotate(0x05AD, root); }

  /** Ready at 48 kHz. The retune glide is worked out in setSampleRate, and a
   *  corrector used without one snapped straight to every target -- the 40 ms
   *  the default promises was 0. */
  PitchCorrector() { setSampleRate(48000.0f); }

  /** [not the audio thread] Allocates the shifter's buffers, sized by the
   *  rate and the lowest pitch setRange() allows: call setRange() first, and
   *  read latencySamples() after. */
  void setSampleRate(float sr) {
    sr_ = sr > 1.0f ? sr : 48000.0f;
    decimate_ = (int) std::floor(sr_ / 48000.0f + 0.5f);
    if (decimate_ < 1) decimate_ = 1;
    const float detectRate = sr_ / (float) decimate_;
    // Fourth-order Butterworth at 45% of the decimated Nyquist: what folds
    // back is 28 dB down at the fold and falling, under anything YIN keys on.
    antiAlias_[0].setSampleRate(sr_);
    antiAlias_[1].setSampleRate(sr_);
    antiAlias_[0].lowpass(0.225f * detectRate, 0.54119610f);
    antiAlias_[1].lowpass(0.225f * detectRate, 1.30656296f);
    detector_.setSampleRate(detectRate);
    detector_.setRange(minHz_, maxHz_);
    // The shifter. Its delay must hold a grain -- two periods -- a period
    // past the mark it is taken from, so it is sized by the longest period
    // it may meet: the range's floor, which latencySamples() then answers.
    floorHz_ = minHz_ > kLowestHz ? minHz_ : kLowestHz;
    maxPeriod_ = (int) std::ceil(sr_ / floorHz_);
    delay_ = (int) std::ceil(2.5 * (double) maxPeriod_) + 2;
    in_.assign(nextPow2((size_t) delay_ + 2u * (size_t) maxPeriod_ + 64u), 0.0f);
    acc_.assign(nextPow2(4u * (size_t) maxPeriod_ + 64u), 0.0f);
    weight_.assign(acc_.size(), 0.0f);
    setRetune(retuneMs_);
    reset();
  }
  /** The pitch classes a voice may be moved to: bit 0 is C, bit 11 is B.
   *  Bits above 11 are ignored, and a mask with none of the twelve set
   *  means kChromatic rather than "no note at all". No allocation. */
  void setScale(uint16_t mask) { mask_ = (mask & 0x0FFF) ? (mask & 0x0FFF) : kChromatic; }
  /** The tuning reference: the frequency of A4 in Hz, 440 by default. A
   *  value of 100 Hz or less is taken as 440. */
  void setReference(float a4Hz) { a4_ = a4Hz > 100.0f ? a4Hz : 440.0f; }
  /** How fast the correction reaches its target: 0 snaps. */
  void setRetune(float ms) {
    retuneMs_ = ms < 0.0f ? 0.0f : ms;
    glide_ = retuneMs_ > 0.0f ? std::exp(-1.0f / (0.001f * retuneMs_ * sr_)) : 0.0f;
  }
  /** Pitches outside this are left alone. The floor also sizes the shifter
   *  and so the latency: it takes effect at the next setSampleRate(), and
   *  until then a pitch below the old floor is left alone too. */
  void setRange(float minHz, float maxHz) {
    minHz_ = minHz;
    maxHz_ = maxHz;
    detector_.setRange(minHz, maxHz);
  }
  /** How much of the distance to the target is corrected: 1 is all of it. */
  void setAmount(float amount) { amount_ = clampf(amount, 0.0f, 1.0f); }
  /** Forget the input: the detector, the shifter's buffers and the glide
   *  start again, the ratio at 1 and detectedHz() and targetHz() at 0. The
   *  settings are kept. Clears the buffers setSampleRate() sized, without
   *  allocating. */
  void reset() {
    detector_.reset();
    antiAlias_[0].reset();
    antiAlias_[1].reset();
    decimatePhase_ = 0;
    ratio_ = 1.0f;
    target_ = 1.0f;
    detected_ = 0.0f;
    targetHz_ = 0.0f;
    for (float& v : in_) v = 0.0f;
    for (float& v : acc_) v = 0.0f;
    for (float& v : weight_) v = 0.0f;
    now_ = 0;
    period_ = (int) std::lround(sr_ / 150.0f);
    if (period_ > maxPeriod_) period_ = maxPeriod_;
    periodExact_ = (double) period_;
    voiced_ = false;
    marks_ = 0;
    nextMark_ = 0;
    // The first grain is laid exactly where its mark falls, a delay later,
    // so with nothing to correct the output is the input, delayed.
    nextGrain_ = (double) delay_;
  }
  /** The delay through the corrector, in samples: two and a half of the
   *  longest period it handles. Fixed by setSampleRate(). */
  int latencySamples() const { return delay_; }

  /** [audio-thread] One sample in, one out, latencySamples() later. Pitch
   *  decisions are made each time the detector finishes a hop, and the ratio
   *  glides toward them sample by sample. No allocation, no lock. */
  inline float process(float x) {
    if (decimate_ == 1) {
      if (detector_.push(x)) decide();
    } else {
      const float heard = antiAlias_[1].process(antiAlias_[0].process(x));
      if (++decimatePhase_ >= decimate_) {
        decimatePhase_ = 0;
        if (detector_.push(heard)) decide();
      }
    }
    ratio_ = target_ + glide_ * (ratio_ - target_);
    return shift(x);
  }

  /** The last pitch the detector was confident about, in Hz. It holds that
   *  value through breaths and unvoiced stretches rather than dropping to 0;
   *  0 only before the first confident detection and after reset(). */
  float detectedHz() const { return detected_; }
  /** The scale note, in Hz, that the last confident detection was being
   *  moved to; held and reset like detectedHz(). */
  float targetHz() const { return targetHz_; }
  /** The pitch ratio being applied on this sample: 1 is no correction. It
   *  glides to the target over the retune time, and back to 1 when the
   *  detector is not confident. */
  float ratio() const { return ratio_; }

private:
  /** Below this the corrector does not go, whatever the range says: a
   *  period sets the delay, and 40 Hz is already 62 ms of it. */
  static constexpr float kLowestHz = 40.0f;
  static constexpr int kMarks = 32;

  static uint16_t rotate(uint16_t mask, int by) {
    by = ((by % 12) + 12) % 12;
    return (uint16_t) (((mask << by) | (mask >> (12 - by))) & 0x0FFF);
  }
  static size_t nextPow2(size_t n) {
    size_t p = 1;
    while (p < n) p <<= 1;
    return p;
  }

  void decide() {
    const float hz = detector_.frequency();
    if (detector_.confidence() < 0.6f || hz < minHz_ || hz > maxHz_ || hz < floorHz_) {
      // Nothing to correct against: glide home, and cut the grains evenly,
      // which with a ratio of one gives the input back.
      target_ = 1.0f;
      voiced_ = false;
      return;
    }
    detected_ = hz;
    voiced_ = true;
    // Exact for the grains' spacing, which is the pitch: rounded to whole
    // samples it was up to half a sample a period off -- a 435 Hz A aimed at
    // 440 came out at 441.4. Whole samples for the windows and the marks.
    double exact = (double) sr_ / (double) hz;
    if (exact < 8.0) exact = 8.0;
    if (exact > (double) maxPeriod_) exact = (double) maxPeriod_;
    periodExact_ = exact;
    period_ = (int) std::lround(exact);
    // In cents from A4, to the nearest allowed pitch class.
    const float note = 69.0f + 12.0f * std::log2(hz / a4_);
    const int nearest = (int) std::floor(note + 0.5f);
    int best = nearest;
    float bestDist = 1e9f;
    for (int n = nearest - 6; n <= nearest + 6; ++n) {
      const int pc = ((n % 12) + 12) % 12;
      if (!(mask_ & (1u << pc))) continue;
      const float dist = std::fabs((float) n - note);
      if (dist < bestDist) { bestDist = dist; best = n; }
    }
    targetHz_ = a4_ * std::pow(2.0f, ((float) best - 69.0f) / 12.0f);
    const float full = targetHz_ / hz;
    target_ = std::pow(full, amount_);
  }

  /** One sample through the shifter. */
  inline float shift(float x) {
    const size_t inMask = in_.size() - 1, accMask = acc_.size() - 1;
    in_[(size_t) now_ & inMask] = x;

    // Pitch marks, one a period after the last. A voiced one is moved to the
    // period's peak nearby, so every grain is cut at the same point of the
    // waveform; an unvoiced one stays where the spacing puts it, so with a
    // ratio of one the grains tile the input exactly.
    const int p = period_;
    while (now_ >= nextMark_ + p / 2) {
      int64_t at = nextMark_;
      if (voiced_) {
        float peak = -1e30f;
        for (int64_t k = nextMark_ - p / 4; k <= nextMark_ + p / 4; ++k) {
          const float v = in_[(size_t) k & inMask];
          if (k >= 0 && v > peak) {
            peak = v;
            at = k;
          }
        }
      }
      markAt_[marks_ % kMarks] = at;
      ++marks_;
      nextMark_ = at + p;
    }

    // Grains, each laid down when its window reaches the present: centred a
    // delay after the input point it stands for, cut from the mark nearest
    // that point whose grain is all in the past. Spaced a period divided by
    // the ratio apart, which is the new pitch.
    while (nextGrain_ - (double) p <= (double) now_) {
      const double from = nextGrain_ - (double) delay_;
      int64_t mark = (int64_t) std::llround(from);
      double bestGap = 1e300;
      const int64_t newest = marks_, oldest = marks_ > kMarks ? marks_ - kMarks : 0;
      for (int64_t i = oldest; i < newest; ++i) {
        const int64_t m = markAt_[i % kMarks];
        if (m + p > now_) continue;
        const double gap = std::fabs((double) m - from);
        if (gap < bestGap) {
          bestGap = gap;
          mark = m;
        }
      }
      if (mark + p > now_) mark = now_ - p; // no mark yet: the latest grain there is
      const int64_t centre = (int64_t) std::llround(nextGrain_);
      const float scale = 3.14159265358979f / (float) p;
      for (int k = -p; k <= p; ++k) {
        const int64_t o = centre + k;
        if (o < now_) continue; // already played
        const float w = 0.5f + 0.5f * std::cos(scale * (float) k);
        const int64_t src = mark + k;
        const float v = src >= 0 ? in_[(size_t) src & inMask] : 0.0f;
        acc_[(size_t) o & accMask] += w * v;
        weight_[(size_t) o & accMask] += w;
      }
      // Voiced, the exact period; unvoiced, the marks' own whole-sample
      // spacing, so that a ratio of one lays every grain back where it was.
      const double step = (voiced_ ? periodExact_ : (double) p) / (double) ratio_;
      nextGrain_ += step > 1.0 ? step : 1.0;
    }

    // Out, normalised by the windows that covered it: one where the grains
    // tile, more where a higher pitch packs them closer. Not raised past
    // double where a lower pitch spreads them -- the gap between two periods
    // is quiet in the voice too.
    const size_t o = (size_t) now_ & accMask;
    const float w = weight_[o];
    const float y = acc_[o] / (w > 0.5f ? w : 0.5f);
    acc_[o] = 0.0f;
    weight_[o] = 0.0f;
    ++now_;
    return y;
  }

  PitchDetector<2048> detector_;
  Biquad antiAlias_[2];
  int decimate_ = 1, decimatePhase_ = 0;
  uint16_t mask_ = kChromatic;
  float sr_ = 48000.0f, a4_ = 440.0f, minHz_ = 60.0f, maxHz_ = 1500.0f, retuneMs_ = 40.0f, glide_ = 0.0f;
  float amount_ = 1.0f, ratio_ = 1.0f, target_ = 1.0f, detected_ = 0.0f, targetHz_ = 0.0f;

  // The shifter: the input, the output being laid down and the windows'
  // sum over it, the marks, and where the next mark and grain go.
  std::vector<float> in_, acc_, weight_;
  float floorHz_ = 60.0f;
  int maxPeriod_ = 800, delay_ = 2002, period_ = 320;
  double periodExact_ = 320.0;
  bool voiced_ = false;
  int64_t now_ = 0, nextMark_ = 0, marks_ = 0;
  int64_t markAt_[kMarks]{};
  double nextGrain_ = 0.0;
};

} // namespace sonore
