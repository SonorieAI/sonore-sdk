// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: the phase vocoder: pitch shifting without the grain.
//
// pitch.h's PitchShifter is two read heads crossfading on a delay line, and
// it sounds like it -- a faint tremolo at the crossfade rate, a comb when
// the two heads carry the same partial. The phase vocoder does the job in
// the frequency domain, where every partial can be moved on its own and
// resynthesised with a phase advanced by exactly what its new frequency
// needs, so there is no crossfade to hear.
//
// The method is Laroche & Dolson's ("Improved phase vocoder time-scale
// modification of audio", IEEE TSAP 1999), and specifically two of its
// ideas together:
//
//   PEAK SHIFTING. The naive route is time-stretch by the ratio, then read
//   the stretched signal back faster. It needs a resampler, its synthesis
//   hop is a non-integer that has to be dithered, the overlap-add level
//   moves with the ratio and -- the part that matters to a host -- the
//   latency depends on the ratio HISTORY, because a reader lagging the writer
//   by a fixed distance in the stretched signal lags it by distance/ratio in
//   input time. Instead each spectral peak's REGION is moved along the
//   frequency axis by the ratio and resynthesised at the same hop it was
//   analysed with: no resampler, an exact overlap-add, and a latency of
//   exactly Size at every ratio, which is what the plugin reports.
//
//   IDENTITY PHASE LOCKING. The bins around a peak are not each given their
//   own freely-running phase -- that is where the classic "phasiness" comes
//   from -- but keep the phase RELATIONSHIP they had to the peak in the
//   analysis frame. The peak carries the propagation, advanced by its true
//   frequency times the ratio; the neighbours ride with it. Regions are
//   split at the magnitude minimum between adjacent peaks, found once per
//   frame in a single pass rather than re-searched per bin.
//
// The overlap-add is normalised the way stft.h does it -- the summed squared
// window at each position within a hop -- so the identity ratio gives the
// input back and every other ratio comes out at the level it went in.
//
// Included by dsp.h. Depends on Fft<> from fft.h.
#pragma once
#include <cmath>
#include <cstddef>
#include "audio.h"

namespace sonore {

/**
 * SIZE: about 120 KB at the defaults (a 2048 FFT with its tables, the rings
 * and the per-bin phases). A member of the plugin, never a local. 2048 / 4
 * at 48 kHz: 43 ms frames, 11 ms hop, 43 ms latency. Overlap 8 costs twice
 * the FFTs and buys smoother phase tracking on large upward shifts.
 */
template <size_t Size = 2048, size_t Overlap = 4>
class PhaseVocoder {
  static_assert(isPowerOfTwo(Size) && Size >= 256, "Size must be a power of two, 256 or more");
  static_assert(Overlap >= 2 && Size % Overlap == 0, "Overlap must divide Size");

public:
  static constexpr size_t size() { return Size; }
  static constexpr size_t hop() { return Size / Overlap; }
  static constexpr size_t numBins() { return Size / 2 + 1; }
  /** One frame, at every ratio. */
  static constexpr int latencySamples() { return (int) Size; }

  PhaseVocoder() { prepare(); }

  void prepare() {
    // The periodic Hann, whose overlapped copies sum to a constant.
    for (size_t i = 0; i < Size; ++i)
      window_[i] = 0.5f - 0.5f * std::cos(2.0f * kPi * (float) i / (float) Size);
    for (size_t p = 0; p < hop(); ++p) {
      float s = 0.0f;
      for (size_t k = 0; k < Overlap; ++k) {
        const float w = window_[p + k * hop()];
        s += w * w;
      }
      norm_[p] = s > 1e-12f ? 1.0f / s : 0.0f;
    }
    reset();
  }

  void reset() {
    for (size_t i = 0; i < Size; ++i) { input_[i] = 0.0f; ola_[i] = 0.0f; outQueue_[i] = 0.0f; }
    for (size_t k = 0; k < numBins(); ++k) { lastPhase_[k] = 0.0f; synthPhase_[k] = 0.0f; }
    inPos_ = 0;
    sinceHop_ = 0;
    olaPos_ = 0;
    outRead_ = 0;
    outWrite_ = hop(); // see stft.h: the writer starts one hop ahead of the reader
  }

  void setSemitones(float semitones) { setRatio(std::pow(2.0f, semitones / 12.0f)); }
  /** 0.25 .. 4: two octaves either way. */
  void setRatio(float ratio) { ratio_ = clampf(ratio, 0.25f, 4.0f); }
  float ratio() const { return ratio_; }

  inline float process(float x) {
    input_[inPos_] = x;
    inPos_ = (inPos_ + 1) % Size;
    if (++sinceHop_ >= hop()) {
      sinceHop_ = 0;
      frame();
    }
    const float y = outQueue_[outRead_];
    outRead_ = (outRead_ + 1) % Size;
    return y;
  }

private:
  static constexpr size_t kMaxPeaks = Size / 4;

  void frame() {
    const size_t nb = numBins();
    // ── Analysis ──
    for (size_t i = 0; i < Size; ++i) {
      re_[i] = input_[(inPos_ + i) % Size] * window_[i];
      im_[i] = 0.0f;
    }
    fft_.transform(re_, im_, false);
    for (size_t k = 0; k < nb; ++k) {
      mag_[k] = std::sqrt(re_[k] * re_[k] + im_[k] * im_[k]);
      phase_[k] = std::atan2(im_[k], re_[k]);
    }

    // ── Peaks and their regions ──
    // A peak out-magnitudes its two neighbours on each side. The boundary
    // between two adjacent peaks is the magnitude minimum between them, and
    // bound_[i] is the first bin of region i + 1.
    size_t peakCount = 0;
    for (size_t k = 2; k + 2 < nb && peakCount < kMaxPeaks; ++k) {
      const float m = mag_[k];
      if (m > mag_[k - 1] && m > mag_[k - 2] && m >= mag_[k + 1] && m >= mag_[k + 2] && m > 1e-6f)
        peaks_[peakCount++] = k;
    }
    for (size_t i = 0; i + 1 < peakCount; ++i) {
      size_t valley = peaks_[i];
      for (size_t j = peaks_[i] + 1; j < peaks_[i + 1]; ++j)
        if (mag_[j] < mag_[valley]) valley = j;
      bound_[i] = valley + 1;
    }

    for (size_t k = 0; k < nb; ++k) { outRe_[k] = 0.0f; outIm_[k] = 0.0f; }
    const float hopF = (float) hop();

    if (peakCount == 0) {
      // Nothing peaked (silence, or a floor with no structure): every bin
      // is its own partial, moved by the ratio and propagated on its own.
      for (size_t k = 0; k < nb; ++k) {
        const float w = trueFrequency(k);
        const long t = (long) k + shiftBins(w);
        if (t < 0 || t >= (long) nb) continue;
        synthPhase_[(size_t) t] = wrapPhase(synthPhase_[(size_t) t] + w * ratio_ * hopF);
        place((size_t) t, mag_[k], synthPhase_[(size_t) t]);
      }
    } else {
      for (size_t i = 0; i < peakCount; ++i) {
        const size_t p = peaks_[i];
        const size_t lo = i == 0 ? 0 : bound_[i - 1];
        const size_t hi = i + 1 < peakCount ? bound_[i] : nb;
        const float w = trueFrequency(p);
        const long shift = shiftBins(w);
        const long tp = (long) p + shift;
        if (tp < 0 || tp >= (long) nb) continue; // the whole region lands outside the band
        // The peak carries the propagation, at its shifted frequency.
        const float thetaP = wrapPhase(synthPhase_[(size_t) tp] + w * ratio_ * hopF);
        synthPhase_[(size_t) tp] = thetaP;
        for (size_t k = lo; k < hi; ++k) {
          const long t = (long) k + shift;
          if (t < 0 || t >= (long) nb) continue;
          // Identity locking: the neighbour keeps the phase offset it had
          // from its peak in the analysis frame.
          const float theta = k == p ? thetaP : wrapPhase(thetaP + (phase_[k] - phase_[p]));
          if (k != p) synthPhase_[(size_t) t] = theta;
          place((size_t) t, mag_[k], theta);
        }
      }
    }
    for (size_t k = 0; k < nb; ++k) lastPhase_[k] = phase_[k];

    // ── Synthesis ──
    for (size_t k = 0; k < nb; ++k) { re_[k] = outRe_[k]; im_[k] = outIm_[k]; }
    im_[0] = 0.0f;
    im_[Size / 2] = 0.0f;
    for (size_t k = 1; k < Size / 2; ++k) {
      re_[Size - k] = re_[k];
      im_[Size - k] = -im_[k];
    }
    fft_.transform(re_, im_, true);
    for (size_t i = 0; i < Size; ++i) ola_[(olaPos_ + i) % Size] += re_[i] * window_[i];
    for (size_t i = 0; i < hop(); ++i) {
      const size_t idx = (olaPos_ + i) % Size;
      outQueue_[outWrite_] = ola_[idx] * norm_[i];
      outWrite_ = (outWrite_ + 1) % Size;
      ola_[idx] = 0.0f;
    }
    olaPos_ = (olaPos_ + hop()) % Size;
  }

  inline void place(size_t t, float mag, float theta) {
    outRe_[t] += mag * std::cos(theta);
    outIm_[t] += mag * std::sin(theta);
  }

  /** How many whole bins a partial at `omega` (radians per sample) moves
   *  when its frequency is multiplied by the ratio. */
  inline long shiftBins(float omega) const {
    const float bin = omega * (float) Size / (2.0f * kPi);
    return (long) std::floor((ratio_ - 1.0f) * bin + 0.5f);
  }

  /** The bin's actual frequency this frame, in radians per sample, from
   *  the phase it advanced since the last frame against what a bin-centred
   *  tone would have advanced. */
  inline float trueFrequency(size_t k) const {
    const float expected = 2.0f * kPi * (float) k * (float) hop() / (float) Size;
    const float delta = wrapPhase(phase_[k] - lastPhase_[k] - expected);
    return (2.0f * kPi * (float) k / (float) Size) + delta / (float) hop();
  }
  static inline float wrapPhase(float p) {
    p = std::fmod(p + kPi, 2.0f * kPi);
    if (p < 0.0f) p += 2.0f * kPi;
    return p - kPi;
  }

  Fft<Size> fft_;
  float window_[Size]{};
  float norm_[Size / Overlap]{};
  float input_[Size]{};
  float re_[Size]{}, im_[Size]{};
  float mag_[Size / 2 + 1]{}, phase_[Size / 2 + 1]{};
  float lastPhase_[Size / 2 + 1]{}, synthPhase_[Size / 2 + 1]{};
  float outRe_[Size / 2 + 1]{}, outIm_[Size / 2 + 1]{};
  size_t peaks_[kMaxPeaks]{}, bound_[kMaxPeaks]{};
  float ola_[Size]{};
  float outQueue_[Size]{};
  size_t inPos_ = 0, sinceHop_ = 0, olaPos_ = 0, outRead_ = 0, outWrite_ = 0;
  float ratio_ = 1.0f;
};

} // namespace sonore
