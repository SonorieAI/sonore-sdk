// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: a short-time Fourier transform you can process in the middle of.
//
// fft.h has the transform, an analyser that only LOOKS, and a convolver that
// does one specific thing in the frequency domain. What was missing was the
// general frame: window, transform, hand the spectrum to the caller, transform
// back, overlap-add -- the substrate of a spectral gate, a denoiser, a freeze,
// a vocoder, a phase vocoder. This is that frame, and only that: the
// processing is the functor you pass in.
//
// Reconstruction is EXACT, not approximately: the overlap-add normalisation
// is computed at prepare() by summing the squared window over one hop's
// worth of positions, and every output sample is divided by the sum at its
// position. That holds for any window and any overlap where the sum is never
// zero, which is every sensible combination, and it means the identity
// processor gives back the input to float precision -- asserted in the unit
// test -- rather than the 1.5x-with-ripple that a "Hann at 75% is COLA"
// shortcut produces with a symmetric window.
//
// Included by dsp.h. Depends on Fft<> from fft.h.
#pragma once
#include <cmath>
#include <cstddef>
#include "audio.h"

namespace sonore {

/**
 * Size is the frame; Overlap the number of frames covering each sample, so
 * the hop is Size / Overlap. 1024 / 4 is the general-purpose choice: 21 ms
 * frames at 48 kHz, 5 ms hop. Latency is exactly Size, and it is reported.
 */
template <size_t Size = 1024, size_t Overlap = 4>
class Stft {
  static_assert(isPowerOfTwo(Size), "Size must be a power of two");
  static_assert(Overlap >= 2 && Size % Overlap == 0, "Overlap must divide Size and be >= 2");

public:
  static constexpr size_t size() { return Size; }
  static constexpr size_t hop() { return Size / Overlap; }
  static constexpr size_t numBins() { return Size / 2 + 1; }
  static constexpr int latencySamples() { return (int) Size; }

  Stft() { prepare(); }

  void prepare() {
    // The PERIODIC Hann -- cos over N, not N-1 -- because it is the one whose
    // overlapped copies sum to a constant. The symmetric form in fillWindow
    // is right for a spectrum and wrong for resynthesis; the normalisation
    // below would rescue it, but starting from the right window means the
    // normalisation is near-constant instead of doing real work.
    for (size_t i = 0; i < Size; ++i)
      window_[i] = 0.5f - 0.5f * std::cos(2.0f * kPi * (float) i / (float) Size);
    // Sum of the squared window (analysis AND synthesis) at each position
    // within a hop, over all the frames that overlap it.
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
    for (size_t i = 0; i < Size; ++i) input_[i] = 0.0f;
    for (size_t i = 0; i < Size; ++i) ola_[i] = 0.0f;
    inPos_ = 0;
    sinceHop_ = 0;
    olaPos_ = 0;
    // The output queue starts FULL of silence: that Size samples of nothing
    // before the first real output IS the latency, made explicit rather than
    // left as undefined reads until Overlap frames have been seen.
    for (size_t i = 0; i < Size; ++i) outQueue_[i] = 0.0f;
    outRead_ = 0;
    // The first frame lands after hop() samples and its output is read over
    // the hop() samples that FOLLOW it, so the writer starts one hop ahead of
    // the reader. Start both at zero and every read trails its frame by a
    // whole hop, which reads as "latency Size + hop" -- the unit test measures
    // the impulse and would say so.
    outWrite_ = hop();
  }

  /** Bin k's centre frequency at a given rate. */
  static double binFrequency(size_t k, double sampleRate) {
    return (double) k * sampleRate / (double) Size;
  }

  /**
   * One sample in, one sample out. `fn(real, imag, numBins)` is called once
   * per hop with bins 0..Size/2; edit them in place. The upper half of the
   * spectrum is regenerated from the lower by conjugate symmetry afterwards,
   * so the output is always real whatever fn does. [audio thread; fn runs on it]
   */
  template <typename Fn>
  inline float process(float x, Fn&& fn) {
    input_[inPos_] = x;
    inPos_ = (inPos_ + 1) % Size;
    if (++sinceHop_ >= hop()) {
      sinceHop_ = 0;
      frame(fn);
    }
    const float y = outQueue_[outRead_];
    outRead_ = (outRead_ + 1) % Size;
    return y;
  }

private:
  template <typename Fn>
  void frame(Fn&& fn) {
    // The last Size samples, oldest first, windowed.
    for (size_t i = 0; i < Size; ++i) {
      re_[i] = input_[(inPos_ + i) % Size] * window_[i];
      im_[i] = 0.0f;
    }
    fft_.transform(re_, im_, false);
    fn(re_, im_, numBins());
    // Conjugate symmetry: bin Size-k is the conjugate of bin k. DC and
    // Nyquist are their own mirror and must be real for a real signal.
    im_[0] = 0.0f;
    im_[Size / 2] = 0.0f;
    for (size_t k = 1; k < Size / 2; ++k) {
      re_[Size - k] = re_[k];
      im_[Size - k] = -im_[k];
    }
    fft_.transform(re_, im_, true);
    // Overlap-add with the synthesis window into the accumulator, then emit
    // the hop that is now complete: the oldest hop of the accumulator has
    // been touched by all Overlap frames.
    for (size_t i = 0; i < Size; ++i) ola_[(olaPos_ + i) % Size] += re_[i] * window_[i];
    for (size_t i = 0; i < hop(); ++i) {
      const size_t idx = (olaPos_ + i) % Size;
      outQueue_[outWrite_] = ola_[idx] * norm_[i];
      outWrite_ = (outWrite_ + 1) % Size;
      ola_[idx] = 0.0f;
    }
    olaPos_ = (olaPos_ + hop()) % Size;
  }

  Fft<Size> fft_;
  float window_[Size]{};
  float norm_[Size / Overlap]{};
  float input_[Size]{};
  float re_[Size]{};
  float im_[Size]{};
  float ola_[Size]{};
  float outQueue_[Size]{};
  size_t inPos_ = 0, sinceHop_ = 0, olaPos_ = 0, outRead_ = 0, outWrite_ = 0;
};

} // namespace sonore
