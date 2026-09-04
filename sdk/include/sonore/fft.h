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

/** True for 1, 2, 4, 8, ...: the FFT's only structural requirement. */
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
  static constexpr size_t size() { return Size; }
  /** Usable spectrum bins for real input (DC through Nyquist). */
  static constexpr size_t numBins() { return Size / 2 + 1; }

  Fft() { buildTables(); }

  /** Forward transform, in place. `inverse` runs the conjugate transform and
   *  scales by 1/N, so forward-then-inverse returns the original signal. */
  void transform(float* real, float* imag, bool inverse = false) const {
    // Bit-reversal permutation.
    for (size_t i = 0; i < Size; ++i) {
      const size_t j = reversed_[i];
      if (j > i) {
        float t = real[i]; real[i] = real[j]; real[j] = t;
        t = imag[i]; imag[i] = imag[j]; imag[j] = t;
      }
    }
    // Butterflies, stage by stage.
    for (size_t len = 2; len <= Size; len <<= 1) {
      const size_t half = len >> 1;
      const size_t step = Size / len;
      for (size_t i = 0; i < Size; i += len) {
        for (size_t k = 0; k < half; ++k) {
          const size_t t = k * step;
          const float wr = cosTable_[t];
          const float wi = inverse ? sinTable_[t] : -sinTable_[t];
          const size_t a = i + k, b = i + k + half;
          const float xr = real[b] * wr - imag[b] * wi;
          const float xi = real[b] * wi + imag[b] * wr;
          real[b] = real[a] - xr;
          imag[b] = imag[a] - xi;
          real[a] += xr;
          imag[a] += xi;
        }
      }
    }
    if (inverse) {
      const float scale = 1.0f / (float) Size;
      for (size_t i = 0; i < Size; ++i) {
        real[i] *= scale;
        imag[i] *= scale;
      }
    }
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
  void buildTables() {
    for (size_t i = 0; i < Size / 2; ++i) {
      const double a = 2.0 * (double) kPi * (double) i / (double) Size;
      cosTable_[i] = (float) std::cos(a);
      sinTable_[i] = (float) std::sin(a);
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

  float cosTable_[Size / 2]{};
  float sinTable_[Size / 2]{};
  size_t reversed_[Size]{};
};

/** Analysis windows. A rectangular window smears a tone across every bin;
 *  which window to use is a resolution-vs-leakage trade, so both classics are
 *  here rather than one baked-in choice. */
enum class Window {
  Rectangular,
  Hann,
  Hamming,
  Blackman,
  BlackmanHarris,
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

inline void fillWindow(float* out, size_t n, Window kind) {
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
  static constexpr size_t numBins() { return Fft<Size>::numBins(); }

  SpectrumAnalyser() {
    fillWindow(window_, Size, Window::Hann);
    for (size_t i = 0; i < numBins(); ++i) magnitudeDb_[i] = -120.0f;
  }

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
    const float norm = (float) (2.0 / (gain > 1e-9 ? gain : 1.0));
    for (size_t i = 0; i < numBins(); ++i) {
      const float m = mag[i] * norm;
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
 * because a host can call it while audio is running.
 */
template <size_t BlockSize = 256, size_t MaxPartitions = 64>
class Convolver {
  static_assert(isPowerOfTwo(BlockSize), "convolution block must be a power of two");

public:
  static constexpr size_t kFftSize = BlockSize * 2;
  static constexpr size_t maxImpulseLength() { return BlockSize * MaxPartitions; }

  /** Load an impulse response. Longer impulses are truncated to what the
   *  compile-time partition count can hold: silently growing would mean
   *  allocating on whatever thread called this. */
  void loadImpulse(const float* ir, size_t length) {
    reset();
    if (!ir || length == 0) {
      partitions_ = 0;
      return;
    }
    if (length > maxImpulseLength()) length = maxImpulseLength();
    partitions_ = (length + BlockSize - 1) / BlockSize;

    for (size_t p = 0; p < partitions_; ++p) {
      float re[kFftSize]{}, im[kFftSize]{};
      const size_t offset = p * BlockSize;
      const size_t n = (offset + BlockSize <= length) ? BlockSize : (length - offset);
      for (size_t i = 0; i < n; ++i) re[i] = ir[offset + i];
      fft_.transform(re, im, false);
      for (size_t i = 0; i < kFftSize; ++i) {
        irReal_[p][i] = re[i];
        irImag_[p][i] = im[i];
      }
    }
  }

  void reset() {
    for (size_t p = 0; p < MaxPartitions; ++p)
      for (size_t i = 0; i < kFftSize; ++i) fdlReal_[p][i] = fdlImag_[p][i] = 0.0f;
    for (size_t i = 0; i < BlockSize; ++i) overlap_[i] = inputBuffer_[i] = 0.0f;
    fill_ = 0;
    fdlPos_ = 0;
  }

  bool hasImpulse() const { return partitions_ > 0; }
  /** Latency in samples the host must be told about. */
  static constexpr size_t latency() { return BlockSize; }

  /** Push one sample, get one sample. Buffers internally, so callers do not
   *  have to align their block size with ours. */
  inline float process(float x) {
    if (partitions_ == 0) return x;
    const float y = output_[fill_];
    inputBuffer_[fill_] = x;
    if (++fill_ >= BlockSize) {
      fill_ = 0;
      processBlock();
    }
    return y;
  }

private:
  void processBlock() {
    // Transform this block, zero-padded to twice its length so the circular
    // convolution the FFT performs equals the linear one we want.
    float re[kFftSize]{}, im[kFftSize]{};
    for (size_t i = 0; i < BlockSize; ++i) re[i] = inputBuffer_[i];
    fft_.transform(re, im, false);

    // Newest spectrum at the head of the frequency-domain delay line.
    fdlPos_ = (fdlPos_ + MaxPartitions - 1) % MaxPartitions;
    for (size_t i = 0; i < kFftSize; ++i) {
      fdlReal_[fdlPos_][i] = re[i];
      fdlImag_[fdlPos_][i] = im[i];
    }

    // Multiply-accumulate every partition against its matching history block.
    float accRe[kFftSize]{}, accIm[kFftSize]{};
    for (size_t p = 0; p < partitions_; ++p) {
      const size_t slot = (fdlPos_ + p) % MaxPartitions;
      const float* xr = fdlReal_[slot];
      const float* xi = fdlImag_[slot];
      const float* hr = irReal_[p];
      const float* hi = irImag_[p];
      // Complex multiply-accumulate, four bins at a time. This is the hottest
      // loop in the SDK, partitions x blocks x bins, and every bin is
      // independent of every other, which is exactly what vectorises.
      size_t i = 0;
      for (; i + 4 <= kFftSize; i += 4) {
        const simd::Vec4f a = simd::Vec4f::load(xr + i);
        const simd::Vec4f b = simd::Vec4f::load(xi + i);
        const simd::Vec4f c = simd::Vec4f::load(hr + i);
        const simd::Vec4f d = simd::Vec4f::load(hi + i);
        (simd::Vec4f::load(accRe + i) + a * c - b * d).store(accRe + i);
        (simd::Vec4f::load(accIm + i) + a * d + b * c).store(accIm + i);
      }
      for (; i < kFftSize; ++i) {
        accRe[i] += xr[i] * hr[i] - xi[i] * hi[i];
        accIm[i] += xr[i] * hi[i] + xi[i] * hr[i];
      }
    }

    fft_.transform(accRe, accIm, true);

    // Overlap-add: the first half is this block's output, the tail carries.
    for (size_t i = 0; i < BlockSize; ++i) {
      output_[i] = accRe[i] + overlap_[i];
      overlap_[i] = accRe[i + BlockSize];
    }
  }

  Fft<kFftSize> fft_;
  size_t partitions_ = 0;
  size_t fill_ = 0;
  size_t fdlPos_ = 0;
  float irReal_[MaxPartitions][kFftSize]{};
  float irImag_[MaxPartitions][kFftSize]{};
  float fdlReal_[MaxPartitions][kFftSize]{};
  float fdlImag_[MaxPartitions][kFftSize]{};
  float inputBuffer_[BlockSize]{};
  float output_[BlockSize]{};
  float overlap_[BlockSize]{};
};

} // namespace sonore
