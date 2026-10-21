// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: frequency domain.
//
// A radix-2 Cooley-Tukey FFT, window functions, a spectrum helper, and
// partitioned convolution. Our own implementation of the standard algorithm; no
// external library, and nothing here allocates once it is prepared.
//
// Why we need it: spectral analysers on a faceplate, convolution reverb and
// cabinet impulses, and the measurement side of the toolkit. The FFT is
// compile-time sized so the twiddle table and scratch buffers are inline:
// a plugin's process() must not be the first place memory is asked for.
#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>
#include "audio.h"
#include "simd.h"

namespace sonore {

/** True for 1, 2, 4, 8, ... (the FFT's only structural requirement). */
constexpr bool isPowerOfTwo(size_t n) { return n && ((n & (n - 1)) == 0); }

/**
 * In-place complex FFT over `Size` points.
 *
 * Usage is deliberately plain arrays rather than a complex type: the DSP that
 * feeds this is real audio, and forcing every caller through std::complex would
 * cost a copy for no clarity.
 */
template <size_t Size>
class Fft {
  static_assert(isPowerOfTwo(Size), "FFT size must be a power of two");
  static_assert(Size >= 4, "FFT size must be at least 4");

public:
  /** The transform length in points: `Size`. */
  static constexpr size_t size() { return Size; }
  /** Usable spectrum bins for real input (DC through Nyquist). */
  static constexpr size_t numBins() { return Size / 2 + 1; }

  /** Builds the twiddle and bit-reversal tables. They are members (two
   *  floats and a size_t per point), so nothing is allocated, and a large
   *  Fft belongs in a member rather than on the stack. */
  Fft() { buildTables(); }

  /** Forward transform, in place. `inverse` runs the conjugate transform and
   *  scales by 1/N, so forward-then-inverse returns the original signal. */
  void transform(float* real, float* imag, bool inverse = false) const {
    // The direction is a template argument rather than a test inside every
    // butterfly, which the compiler could not hoist.
    if (inverse) run<true>(real, imag);
    else run<false>(real, imag);
  }

  /** Magnitude spectrum of real input. `out` receives numBins() values. */
  void magnitude(const float* input, float* out) const {
    float re[Size], im[Size];
    for (size_t i = 0; i < Size; ++i) {
      re[i] = input[i];
      im[i] = 0.0f;
    }
    transform(re, im, false);
    for (size_t i = 0; i < numBins(); ++i)
      out[i] = std::sqrt(re[i] * re[i] + im[i] * im[i]);
  }

private:
  /**
   * The radix-2 transform. Three things make it about twice as fast as the
   * first version, for the same bits: the direction is decided once, not per
   * butterfly; the first stage, whose only twiddle is 1, is additions alone;
   * and each stage reads its twiddles from a run of its own, in order, where
   * they were picked out of one table at a stride that grew stage by stage.
   * Measured on a 512-point transform, 5.9-6.8 us before; a one-partition
   * Convolver 60 -> 33 ns a sample.
   *
   * Two more, again for the same bits: the first two stages run together on
   * groups of four held in registers, where each was a pass through memory;
   * and every later stage does four butterflies at a time, each lane the
   * scalar arithmetic. A 1024-point transform 1.6x, a 512-point one 1.7x.
   */
  template <bool Inverse>
  void run(float* real, float* imag) const {
    // Bit-reversal permutation.
    for (size_t i = 0; i < Size; ++i) {
      const size_t j = reversed_[i];
      if (j > i) {
        float t = real[i]; real[i] = real[j]; real[j] = t;
        t = imag[i]; imag[i] = imag[j]; imag[j] = t;
      }
    }
    // The first two stages together, a group of four at a time, held in
    // registers between them: pairs, whose twiddle is 1 + 0i so the products
    // are the values themselves, then the stage of four with its two
    // twiddles, multiplied out as that stage always has.
    {
      const float w1r = stageCos_[2], w1i = Inverse ? stageSin_[2] : -stageSin_[2];
      const float w2r = stageCos_[3], w2i = Inverse ? stageSin_[3] : -stageSin_[3];
      for (size_t i = 0; i < Size; i += 4) {
        const float r0 = real[i] + real[i + 1], r1 = real[i] - real[i + 1];
        const float i0 = imag[i] + imag[i + 1], i1 = imag[i] - imag[i + 1];
        const float r2 = real[i + 2] + real[i + 3], r3 = real[i + 2] - real[i + 3];
        const float i2 = imag[i + 2] + imag[i + 3], i3 = imag[i + 2] - imag[i + 3];
        const float xr0 = r2 * w1r - i2 * w1i, xi0 = r2 * w1i + i2 * w1r;
        const float xr1 = r3 * w2r - i3 * w2i, xi1 = r3 * w2i + i3 * w2r;
        real[i] = r0 + xr0;
        imag[i] = i0 + xi0;
        real[i + 2] = r0 - xr0;
        imag[i + 2] = i0 - xi0;
        real[i + 1] = r1 + xr1;
        imag[i + 1] = i1 + xi1;
        real[i + 3] = r1 - xr1;
        imag[i + 3] = i1 - xi1;
      }
    }
    // Then stage by stage, each half-length's twiddles contiguous.
    for (size_t len = 8; len <= Size; len <<= 1) {
      const size_t half = len >> 1;
      const float* twr = stageCos_ + half;
      const float* twi = stageSin_ + half;
      for (size_t i = 0; i < Size; i += len) {
        float* ar = real + i;
        float* ai = imag + i;
        float* br = real + i + half;
        float* bi = imag + i + half;
        // Four butterflies at a time -- half is a multiple of four from here
        // on -- each lane the scalar butterfly's own arithmetic, so the same
        // bits. The forward twiddle's negated sine is folded into the signs:
        // b * -s is -(b * s) exactly, and a - -p is a + p.
        for (size_t k = 0; k < half; k += 4) {
          const simd::Vec4f wr = simd::Vec4f::load(twr + k);
          const simd::Vec4f ws = simd::Vec4f::load(twi + k);
          const simd::Vec4f vr = simd::Vec4f::load(br + k);
          const simd::Vec4f vi = simd::Vec4f::load(bi + k);
          const simd::Vec4f xr = Inverse ? vr * wr - vi * ws : vr * wr + vi * ws;
          const simd::Vec4f xi = Inverse ? vr * ws + vi * wr : vi * wr - vr * ws;
          const simd::Vec4f ur = simd::Vec4f::load(ar + k);
          const simd::Vec4f ui = simd::Vec4f::load(ai + k);
          (ur - xr).store(br + k);
          (ui - xi).store(bi + k);
          (ur + xr).store(ar + k);
          (ui + xi).store(ai + k);
        }
      }
    }
    if (Inverse) {
      const float scale = 1.0f / (float) Size;
      for (size_t i = 0; i < Size; ++i) {
        real[i] *= scale;
        imag[i] *= scale;
      }
    }
  }

  void buildTables() {
    // Stage `half`'s k-th twiddle at [half + k]: the angle 2 pi k / (2 half),
    // worked out exactly as the single table worked out entry k * Size/(2 half).
    for (size_t half = 1; half < Size; half <<= 1) {
      const size_t step = Size / (2 * half);
      for (size_t k = 0; k < half; ++k) {
        const double a = 2.0 * (double) kPi * (double) (k * step) / (double) Size;
        stageCos_[half + k] = (float) std::cos(a);
        stageSin_[half + k] = (float) std::sin(a);
      }
    }
    size_t bits = 0;
    while ((size_t(1) << bits) < Size) ++bits;
    for (size_t i = 0; i < Size; ++i) {
      size_t r = 0;
      for (size_t b = 0; b < bits; ++b)
        if (i & (size_t(1) << b)) r |= size_t(1) << (bits - 1 - b);
      reversed_[i] = r;
    }
  }

  float stageCos_[Size]{}; // [half + k]: stage `half`'s twiddles; [0] unused
  float stageSin_[Size]{};
  size_t reversed_[Size]{};
};

/** Analysis windows. A rectangular window smears a tone across every bin;
 *  which window to use is a resolution-vs-leakage trade, so both classics are
 *  here rather than one baked-in choice. */
enum class Window {
  Rectangular,    ///< no shaping: the narrowest main lobe and the most leakage
  Hann,           ///< raised cosine to zero at both ends; the usual choice for a display
  Hamming,        ///< raised cosine on a pedestal: a lower first sidelobe than Hann, slower falloff
  Blackman,       ///< three terms: lower sidelobes than Hann, a wider main lobe
  BlackmanHarris, ///< four terms: sidelobes far below Blackman's, for a wider main lobe still
  /** For MEASURING A LEVEL rather than seeing a shape.
   *
   *  Every window above trades amplitude accuracy for resolution: a tone
   *  sitting between two bins reads LOW, by up to 1.4 dB with Hann, because
   *  its energy is split across bins and neither holds all of it. Flat-top is
   *  designed the other way round -- a main lobe wide enough that an off-bin
   *  tone still reads its true amplitude, at the cost of telling two nearby
   *  tones apart. A meter wants this; a spectrogram does not. */
  FlatTop,
};

/** Writes `n` samples of the window `kind` to `out`, in the symmetric form:
 *  sample 0 and sample n-1 are the window's two ends. Every shape's peak
 *  value is 1 (a sample lands on it when n is odd) except FlatTop's, which
 *  is divided by its constant term so that its mean is about 1. One sample
 *  is 1; zero samples writes nothing. No allocation. */
inline void fillWindow(float* out, size_t n, Window kind) {
  // One sample is the whole window, as FirWindow's windowValue has it. Its
  // position below was 0 / 0, and every shape but the rectangle came out NaN.
  if (n == 1) {
    out[0] = 1.0f;
    return;
  }
  for (size_t i = 0; i < n; ++i) {
    const double x = (double) i / (double) (n - 1);
    switch (kind) {
      case Window::BlackmanHarris:
        out[i] = (float) (0.35875 - 0.48829 * std::cos(2.0 * (double) kPi * x) +
                          0.14128 * std::cos(4.0 * (double) kPi * x) -
                          0.01168 * std::cos(6.0 * (double) kPi * x));
        break;
      case Window::FlatTop:
        // The five-term coefficients, normalised so the window's MEAN is one
        // -- which is what makes an amplitude read off a bin come out in the
        // signal's own units instead of needing a correction factor nobody
        // remembers to apply.
        out[i] = (float) ((0.21557895 - 0.41663158 * std::cos(2.0 * (double) kPi * x) +
                           0.277263158 * std::cos(4.0 * (double) kPi * x) -
                           0.083578947 * std::cos(6.0 * (double) kPi * x) +
                           0.006947368 * std::cos(8.0 * (double) kPi * x)) /
                          0.21557895);
        break;
      case Window::Hann:
        out[i] = (float) (0.5 - 0.5 * std::cos(2.0 * (double) kPi * x));
        break;
      case Window::Hamming:
        out[i] = (float) (0.54 - 0.46 * std::cos(2.0 * (double) kPi * x));
        break;
      case Window::Blackman:
        out[i] = (float) (0.42 - 0.5 * std::cos(2.0 * (double) kPi * x) +
                          0.08 * std::cos(4.0 * (double) kPi * x));
        break;
      case Window::Rectangular:
      default:
        out[i] = 1.0f;
        break;
    }
  }
}

/**
 * A spectrum analyser for a faceplate.
 *
 * Feeds samples in, produces a windowed magnitude spectrum in dB whenever a
 * frame fills. Hop is half the window, so a moving display stays smooth without
 * paying for a transform every sample.
 */
template <size_t Size = 1024>
class SpectrumAnalyser {
public:
  /** Bins in the spectrum, DC through Nyquist: Size / 2 + 1. */
  static constexpr size_t numBins() { return Fft<Size>::numBins(); }

  /** A Hann window; every bin reads -120 dB, the floor, until the first
   *  frame is analysed. */
  SpectrumAnalyser() {
    fillWindow(window_, Size, Window::Hann);
    for (size_t i = 0; i < numBins(); ++i) magnitudeDb_[i] = -120.0f;
  }

  /** The rate in Hz that binFrequency() converts with (48000 until set). The
   *  analysis itself does not depend on it, and the value is not checked. */
  void setSampleRate(double sr) { sampleRate_ = sr; }

  /** Push one sample. Returns true when a new spectrum became available. */
  inline bool push(float x) {
    buffer_[writePos_++] = x;
    if (writePos_ < Size) return false;
    analyse();
    // 50% overlap: keep the newer half, so the next frame is a hop away.
    for (size_t i = 0; i < Size / 2; ++i) buffer_[i] = buffer_[i + Size / 2];
    writePos_ = Size / 2;
    return true;
  }

  /** Magnitudes in dBFS, DC through Nyquist. */
  const float* magnitudeDb() const { return magnitudeDb_; }
  /** Centre frequency of a bin. */
  double binFrequency(size_t bin) const {
    return (double) bin * sampleRate_ / (double) Size;
  }

private:
  void analyse() {
    float windowed[Size];
    for (size_t i = 0; i < Size; ++i) windowed[i] = buffer_[i] * window_[i];
    float mag[Fft<Size>::numBins()];
    fft_.magnitude(windowed, mag);
    // Coherent gain of the window, so a full-scale sine reads 0 dBFS rather
    // than whatever the window happened to attenuate it to.
    double gain = 0.0;
    for (size_t i = 0; i < Size; ++i) gain += window_[i];
    // A sine's energy is split between bin k and its mirror N-k, hence the 2.
    // DC and Nyquist are their own mirrors and hold all of theirs: they take
    // half, or a DC of 0.5 read the same 0 dBFS as a full-scale sine.
    const float norm = (float) (2.0 / (gain > 1e-9 ? gain : 1.0));
    for (size_t i = 0; i < numBins(); ++i) {
      const float m = mag[i] * ((i == 0 || i == numBins() - 1) ? 0.5f * norm : norm);
      magnitudeDb_[i] = 20.0f * std::log10(m > 1e-6f ? m : 1e-6f);
    }
  }

  Fft<Size> fft_;
  float window_[Size]{};
  float buffer_[Size]{};
  float magnitudeDb_[Fft<Size>::numBins()]{};
  size_t writePos_ = 0;
  double sampleRate_ = 48000.0;
};

/**
 * Uniformly-partitioned convolution: cabinet impulses, convolution reverb.
 *
 * Direct convolution costs O(N) per sample and is unusable past a few hundred
 * taps; this transforms once per block instead. The impulse is split into
 * `BlockSize` partitions, each pre-transformed at load time, and the running
 * input spectrum is multiplied against all of them (frequency-domain delay
 * line). Latency is exactly one block, reported through the plugin's latency
 * extension.
 *
 * Everything is sized at compile time: loading an impulse allocates nothing,
 * so it can be done on the audio thread. Not from another thread while
 * process() runs -- the spectra would be read half-written; load a second
 * convolver and hand it over instead, as the reverb example's Room does.
 */
template <size_t BlockSize = 256, size_t MaxPartitions = 64>
class Convolver {
  static_assert(isPowerOfTwo(BlockSize), "convolution block must be a power of two");

public:
  /** The transform length: one block of input and one of zero padding, so a
   *  block's convolution with a partition does not wrap round. */
  static constexpr size_t kFftSize = BlockSize * 2;
  /** The longest impulse that plays whole, in samples: BlockSize times
   *  MaxPartitions, 16384 with the defaults. loadImpulse() cuts and fades a
   *  longer one. */
  static constexpr size_t maxImpulseLength() { return BlockSize * MaxPartitions; }

  /** Load an impulse response, and return how much of it will play.
   *
   *  Longer impulses are cut to what the compile-time partition count holds
   *  (growing would mean allocating on whatever thread called this), and
   *  the cut is faded over its last eighth (8192 samples at most). It was a
   *  silent hard cut: a long reverb stopped dead mid-decay with a click, and
   *  a plugin reported the tail of an impulse it was not playing. The return
   *  is the length that does play, which is the tail to report.
   *
   *  The input history is KEPT: a new impulse changes the sound from the next
   *  block on, convolved with what was already playing. It was reset, which
   *  dropped a block to silence and clicked -- 64 samples of exact zero and a
   *  step seven times the signal's own, reloading the very same impulse.
   *
   *  So is the stream from no impulse to the first one, and from an impulse
   *  (null or empty) back to none: the passthrough plays on until the block in
   *  progress is whole, and the convolution of that block takes over on time.
   *  Both reset, and went silent for a block, every time a plugin loaded its
   *  first impulse while audio ran or cleared its last. */
  size_t loadImpulse(const float* ir, size_t length) {
    if (!ir || length == 0) {
      // The passthrough reads the same input ring the convolution writes, in
      // the same order, so it continues from here a block late, as before.
      partitions_ = 0;
      return 0;
    }
    if (partitions_ == 0) {
      // From the passthrough there is no convolution history to continue,
      // and what an earlier impulse left in it is stale. The input ring is
      // the block in progress: the rest of this block plays what the
      // passthrough would have -- the previous block's samples not yet
      // overwritten -- and the first convolved block follows it.
      clearHistory();
      for (size_t i = fill_; i < BlockSize; ++i) output_[i] = inputBuffer_[i];
    }
    const bool cut = length > maxImpulseLength();
    if (cut) length = maxImpulseLength();
    const size_t fade = cut ? (length / 8 < 8192 ? length / 8 : 8192) : 0;
    partitions_ = (length + BlockSize - 1) / BlockSize;

    for (size_t p = 0; p < partitions_; ++p) {
      float re[kFftSize]{}, im[kFftSize]{};
      const size_t offset = p * BlockSize;
      const size_t n = (offset + BlockSize <= length) ? BlockSize : (length - offset);
      for (size_t i = 0; i < n; ++i) {
        const size_t at = offset + i;
        float v = ir[at];
        // Raised cosine over the last `fade` samples, from 1 down to 0.
        if (at + fade >= length && fade > 0)
          v *= 0.5f - 0.5f * std::cos(3.14159265358979f * (float) (length - at) / (float) fade);
        re[i] = v;
      }
      fft_.transform(re, im, false);
      for (size_t i = 0; i < kBins; ++i) {
        irReal_[p][i] = re[i];
        irImag_[p][i] = im[i];
      }
    }
    return length;
  }

  /** Silence: clears the input history, the block being gathered and the
   *  block still being handed out. The impulse is kept. */
  void reset() {
    clearHistory();
    // output_ too: it holds the block process() is still handing out, and a
    // reset that left it played the last block once more into silence.
    for (size_t i = 0; i < BlockSize; ++i) inputBuffer_[i] = output_[i] = 0.0f;
    fill_ = 0;
  }

  /** True while an impulse is loaded. Without one, process() passes the
   *  input through one block late, so the reported latency still holds. */
  bool hasImpulse() const { return partitions_ > 0; }
  /** Latency in samples the host must be told about. */
  static constexpr size_t latency() { return BlockSize; }

  /** Push one sample, get one sample. Buffers internally, so callers do not
   *  have to align their block size with ours. */
  inline float process(float x) {
    if (partitions_ == 0) {
      // No impulse is the identity -- one block late, like any other. It
      // was the input on time, while latency() still told the host a block:
      // a block early against everything the host had aligned.
      const float y = inputBuffer_[fill_];
      inputBuffer_[fill_] = x;
      if (++fill_ >= BlockSize) fill_ = 0;
      return y;
    }
    const float y = output_[fill_];
    inputBuffer_[fill_] = x;
    if (++fill_ >= BlockSize) {
      fill_ = 0;
      processBlock();
    }
    return y;
  }

private:
  /** The convolution's own memory: past spectra and the overlap carried. */
  void clearHistory() {
    for (size_t p = 0; p < MaxPartitions; ++p)
      for (size_t i = 0; i < kRow; ++i) fdlReal_[p][i] = fdlImag_[p][i] = 0.0f;
    for (size_t i = 0; i < BlockSize; ++i) overlap_[i] = 0.0f;
    fdlPos_ = 0;
  }

  void processBlock() {
    // Transform this block, zero-padded to twice its length so the circular
    // convolution the FFT performs equals the linear one we want.
    float re[kFftSize]{}, im[kFftSize]{};
    for (size_t i = 0; i < BlockSize; ++i) re[i] = inputBuffer_[i];
    fft_.transform(re, im, false);

    // Newest spectrum at the head of the frequency-domain delay line.
    fdlPos_ = (fdlPos_ + MaxPartitions - 1) % MaxPartitions;
    for (size_t i = 0; i < kBins; ++i) {
      fdlReal_[fdlPos_][i] = re[i];
      fdlImag_[fdlPos_][i] = im[i];
    }

    // Multiply-accumulate every partition against its matching history block
    // -- over HALF the spectrum. Input and impulse are real, so both spectra
    // are Hermitian: bin N-k is the conjugate of bin k, and so is their
    // product's. Bins 0..N/2 are accumulated and the rest mirrored before the
    // inverse transform; accumulating all N did half this loop for nothing.
    float accRe[kFftSize], accIm[kFftSize]; // the upper half is mirrored in below
    for (size_t i = 0; i < kBins; ++i) accRe[i] = accIm[i] = 0.0f;
    for (size_t p = 0; p < partitions_; ++p) {
      const size_t slot = (fdlPos_ + p) % MaxPartitions;
      const float* xr = fdlReal_[slot];
      const float* xi = fdlImag_[slot];
      const float* hr = irReal_[p];
      const float* hi = irImag_[p];
      // Complex multiply-accumulate, four bins at a time. This is the hottest
      // loop in the SDK (partitions x blocks x bins), and every bin is
      // independent of every other, which is exactly what vectorises.
      size_t i = 0;
      for (; i + 4 <= kBins; i += 4) {
        const simd::Vec4f a = simd::Vec4f::load(xr + i);
        const simd::Vec4f b = simd::Vec4f::load(xi + i);
        const simd::Vec4f c = simd::Vec4f::load(hr + i);
        const simd::Vec4f d = simd::Vec4f::load(hi + i);
        (simd::Vec4f::load(accRe + i) + a * c - b * d).store(accRe + i);
        (simd::Vec4f::load(accIm + i) + a * d + b * c).store(accIm + i);
      }
      for (; i < kBins; ++i) {
        accRe[i] += xr[i] * hr[i] - xi[i] * hi[i];
        accIm[i] += xr[i] * hi[i] + xi[i] * hr[i];
      }
    }
    // The upper half is the lower half's mirror, conjugated. DC and Nyquist
    // are their own mirrors and stay where the sum put them.
    for (size_t k = kBins; k < kFftSize; ++k) {
      accRe[k] = accRe[kFftSize - k];
      accIm[k] = -accIm[kFftSize - k];
    }

    fft_.transform(accRe, accIm, true);

    // Overlap-add: the first half is this block's output, the tail carries.
    for (size_t i = 0; i < BlockSize; ++i) {
      output_[i] = accRe[i] + overlap_[i];
      overlap_[i] = accRe[i + BlockSize];
    }
  }

  /** DC to Nyquist: the bins of a real signal's spectrum that its mirror
   *  half does not repeat, and the only ones the multiply-accumulate reads.
   *  Only these are kept, for the impulse and the history alike, in rows
   *  padded to a whole number of four-bin vectors. All kFftSize were kept,
   *  half of them never read: a Convolver<512, 48> was 809 KB, 786 KB of it
   *  spectra, and is 419 KB. */
  static constexpr size_t kBins = kFftSize / 2 + 1;
  static constexpr size_t kRow = (kBins + 3) / 4 * 4;

  Fft<kFftSize> fft_;
  size_t partitions_ = 0;
  size_t fill_ = 0;
  size_t fdlPos_ = 0;
  float irReal_[MaxPartitions][kRow]{};
  float irImag_[MaxPartitions][kRow]{};
  float fdlReal_[MaxPartitions][kRow]{};
  float fdlImag_[MaxPartitions][kRow]{};
  float inputBuffer_[BlockSize]{};
  float output_[BlockSize]{};
  float overlap_[BlockSize]{};
};

} // namespace sonore
