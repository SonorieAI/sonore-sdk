// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: frequency domain.
//
// A radix-2 Cooley-Tukey FFT, window functions, a spectrum helper, and
// partitioned convolution. Our own implementation of the standard algorithm; no
// external library, and nothing here allocates once it is prepared.
//
// Why we need it: spectral analysers on a faceplate, convolution reverb and
// cabinet impulses, and the measurement side of the toolkit. Fft<Size> is
// compile-time sized so the twiddle table and scratch buffers are inline:
// a plugin's process() must not be the first place memory is asked for.
// DynamicFft is the same transform sized at run time (setSize() allocates),
// and both have REAL-signal transforms that do the work of one complex
// transform of half the size.
//
// Measured against the reference framework's own portable FFT (the one it
// uses wherever it has no vendor library -- Windows and Linux), MSVC,
// release, on the same machine, two runs each: 1024 complex points 1.8 us
// against 5.8-6.1, 4096 8.0-8.4 against 26.1-26.9, 8192 17-20 against 70.
// Its "real-only" transform is a full complex one (1024 points 6.1 us),
// where realForward() here is half of one (1.0 us).
#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>
#include "audio.h"
#include "simd.h"
#include "special.h"
#if defined(SONORE_FFT_ACCELERATE) && defined(__APPLE__)
// Opt-in: DynamicFft's complex transforms on vDSP. Link -framework Accelerate.
#include <Accelerate/Accelerate.h>
#endif

namespace sonore {

/** True for 1, 2, 4, 8, ... (the FFT's only structural requirement). */
constexpr bool isPowerOfTwo(size_t n) { return n && ((n & (n - 1)) == 0); }

namespace fftdetail {

/** The per-stage twiddles and the bit-reversal permutation for an `size`-
 *  point radix-2 transform. Stage `half`'s k-th twiddle is at [half + k]:
 *  the angle 2 pi k / (2 half), which does not depend on `size` -- so a
 *  table built for N serves every smaller power of two, and the real
 *  transforms run their half-size complex transform on the full-size table.
 *  The reversal of the low b - 1 bits of i < N/2 is the b-bit reversal
 *  shifted down one, for the same reason. */
inline void buildTables(size_t size, float* stageCos, float* stageSin, uint32_t* reversed) {
  for (size_t half = 1; half < size; half <<= 1) {
    const size_t step = size / (2 * half);
    for (size_t k = 0; k < half; ++k) {
      const double a = 2.0 * (double) kPi * (double) (k * step) / (double) size;
      stageCos[half + k] = (float) std::cos(a);
      stageSin[half + k] = (float) std::sin(a);
    }
  }
  stageCos[0] = 1.0f;
  stageSin[0] = 0.0f;
  size_t bits = 0;
  while ((size_t(1) << bits) < size) ++bits;
  for (size_t i = 0; i < size; ++i) {
    size_t r = 0;
    for (size_t b = 0; b < bits; ++b)
      if (i & (size_t(1) << b)) r |= size_t(1) << (bits - 1 - b);
    reversed[i] = (uint32_t) r;
  }
}

/**
 * The radix-2 transform of `n` points (a power of two, at least 4), in
 * place, with the tables of a transform of n << `shift` points.
 *
 * Three things make it about twice as fast as the first version, for the
 * same bits: the direction is decided once, not per butterfly; the first
 * stage, whose only twiddle is 1, is additions alone; and each stage reads
 * its twiddles from a run of its own, in order, where they were picked out of
 * one table at a stride that grew stage by stage. Measured on a 512-point
 * transform, 5.9-6.8 us before; a one-partition Convolver 60 -> 33 ns a
 * sample.
 *
 * Two more, again for the same bits: the first two stages run together on
 * groups of four held in registers, where each was a pass through memory;
 * and every later stage does four butterflies at a time, each lane the
 * scalar arithmetic. A 1024-point transform 1.6x, a 512-point one 1.7x.
 */
template <bool Inverse, bool Permute = true>
inline void radix2(size_t n, unsigned shift, const float* stageCos, const float* stageSin,
                   const uint32_t* reversed, float* real, float* imag) {
  // Bit-reversal permutation -- unless the caller already put the data in
  // bit-reversed order, as realForward() does while it packs it.
  if (Permute) {
    for (size_t i = 0; i < n; ++i) {
      const size_t j = reversed[i] >> shift;
      if (j > i) {
        float t = real[i]; real[i] = real[j]; real[j] = t;
        t = imag[i]; imag[i] = imag[j]; imag[j] = t;
      }
    }
  }
  // The first two stages together, a group of four at a time, held in
  // registers between them: pairs, whose twiddle is 1 + 0i so the products
  // are the values themselves, then the stage of four with its two
  // twiddles, multiplied out as that stage always has.
  {
    const float w1r = stageCos[2], w1i = Inverse ? stageSin[2] : -stageSin[2];
    const float w2r = stageCos[3], w2i = Inverse ? stageSin[3] : -stageSin[3];
    for (size_t i = 0; i < n; i += 4) {
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
  for (size_t len = 8; len <= n; len <<= 1) {
    const size_t half = len >> 1;
    const float* twr = stageCos + half;
    const float* twi = stageSin + half;
    for (size_t i = 0; i < n; i += len) {
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
    const float scale = 1.0f / (float) n;
    for (size_t i = 0; i < n; ++i) {
      real[i] *= scale;
      imag[i] *= scale;
    }
  }
}

/**
 * The spectrum of `n` REAL samples (n a power of two, at least 8), from one
 * complex transform of n / 2 points: the even samples go in as its real
 * parts and the odd ones as its imaginary parts, and one pass of twiddles
 * pulls the two half-spectra apart,
 *     X[k] = E[k] + W^k O[k],  E[k] = (Z[k] + Z*[n/2 - k]) / 2,
 *                              O[k] = -j (Z[k] - Z*[n/2 - k]) / 2,
 * W = exp(-2 pi j / n). Half the work of transforming the samples as complex
 * numbers with zero imaginary parts, which is what a real signal otherwise
 * costs. `re` and `im` receive bins 0 .. n/2 (n/2 + 1 of each).
 */
inline void realForward(size_t n, const float* stageCos, const float* stageSin,
                        const uint32_t* reversed, const float* input, float* re, float* im) {
  const size_t h = n / 2;
  // Packed straight into bit-reversed order, which spares the transform its
  // permutation pass: element i of a half-size transform goes where the
  // half-size reversal sends it, the full-size reversal shifted down one.
  for (size_t i = 0; i < h; ++i) {
    const size_t j = reversed[i] >> 1;
    re[j] = input[2 * i];
    im[j] = input[2 * i + 1];
  }
  radix2<false, false>(h, 1, stageCos, stageSin, reversed, re, im);
  // DC and Nyquist are the sum and the difference of Z[0]'s two parts.
  const float z0r = re[0], z0i = im[0];
  re[0] = z0r + z0i;
  im[0] = 0.0f;
  re[h] = z0r - z0i;
  im[h] = 0.0f;
  // The rest in mirrored pairs, k and h - k, both read before either is
  // written. The twiddle for k is stage h's k-th: cos and sin of 2 pi k / n.
  const float* cs = stageCos + h;
  const float* sn = stageSin + h;
  // Four pairs at a time: k .. k+3 ascending against h-k-3 .. h-k, whose
  // lanes are reversed so lane j of both holds one pair. Each lane is the
  // scalar formula below, operation for operation -- the same bits. The two
  // blocks never touch: they stop while 2k + 6 < h.
  size_t k = 1;
  {
    using simd::Vec4f;
    const Vec4f half = Vec4f::splat(0.5f), negHalf = Vec4f::splat(-0.5f);
    for (; 2 * k + 6 < h; k += 4) {
      const size_t m = h - k - 3;
      const Vec4f a = Vec4f::load(re + k), b = Vec4f::load(im + k);
      const Vec4f c = Vec4f::load(re + m).reversed(), d = Vec4f::load(im + m).reversed();
      const Vec4f wk = Vec4f::load(cs + k), sk = Vec4f::load(sn + k);
      const Vec4f wm = Vec4f::load(cs + m).reversed(), sm = Vec4f::load(sn + m).reversed();
      const Vec4f erk = half * (a + c), eik = half * (b - d);
      const Vec4f ork = half * (b + d), oik = negHalf * (a - c);
      const Vec4f erm = half * (c + a), eim = half * (d - b);
      const Vec4f orm = half * (d + b), oim = negHalf * (c - a);
      (erk + (wk * ork + sk * oik)).store(re + k);
      (eik + (wk * oik - sk * ork)).store(im + k);
      (erm + (wm * orm + sm * oim)).reversed().store(re + m);
      (eim + (wm * oim - sm * orm)).reversed().store(im + m);
    }
  }
  for (; k <= h / 2; ++k) {
    const size_t m = h - k;
    const float a = re[k], b = im[k], c = re[m], d = im[m];
    // k
    {
      const float er = 0.5f * (a + c), ei = 0.5f * (b - d);
      const float orr = 0.5f * (b + d), oi = -0.5f * (a - c); // -j (Zk - Zm*) / 2
      const float w = cs[k], s = sn[k];                       // W^k = w - j s
      re[k] = er + (w * orr + s * oi);
      im[k] = ei + (w * oi - s * orr);
    }
    // m, by the same formula with the roles of the pair swapped.
    if (m != k) {
      const float er = 0.5f * (c + a), ei = 0.5f * (d - b);
      const float orr = 0.5f * (d + b), oi = -0.5f * (c - a);
      const float w = cs[m], s = sn[m];
      re[m] = er + (w * orr + s * oi);
      im[m] = ei + (w * oi - s * orr);
    }
  }
}

/**
 * The inverse of realForward(): bins 0 .. n/2 of a real signal's spectrum
 * back to its `n` samples, scaled by 1/n so the round trip is the identity
 * (as transform(..., true) is). The two half-spectra are put back together
 * as Z[k] = E[k] + j O[k], E = (X[k] + X*[n/2 - k]) / 2,
 * O = (X[k] - X*[n/2 - k]) / 2 W^-k, and one inverse transform of n / 2
 * points gives the even samples as its real parts and the odd ones as its
 * imaginary parts. `re` and `im` are used as the working space and are
 * overwritten; X[0] and X[n/2] are read as real (their imaginary parts of a
 * real signal's spectrum are zero).
 */
inline void realInverse(size_t n, const float* stageCos, const float* stageSin,
                        const uint32_t* reversed, float* re, float* im, float* output) {
  const size_t h = n / 2;
  const float x0 = re[0], xh = re[h];
  re[0] = 0.5f * (x0 + xh);
  im[0] = 0.5f * (x0 - xh);
  const float* cs = stageCos + h;
  const float* sn = stageSin + h;
  // Four pairs at a time, as realForward() does: lane for lane the scalar
  // formula, so the same bits.
  size_t k = 1;
  {
    using simd::Vec4f;
    const Vec4f half = Vec4f::splat(0.5f);
    for (; 2 * k + 6 < h; k += 4) {
      const size_t mm = h - k - 3;
      const Vec4f p = Vec4f::load(re + k), q = Vec4f::load(im + k);
      const Vec4f r = Vec4f::load(re + mm).reversed(), s = Vec4f::load(im + mm).reversed();
      const Vec4f wk = Vec4f::load(cs + k), tk = Vec4f::load(sn + k);
      const Vec4f wm = Vec4f::load(cs + mm).reversed(), tm = Vec4f::load(sn + mm).reversed();
      const Vec4f erk = half * (p + r), eik = half * (q - s);
      const Vec4f drk = half * (p - r), dik = half * (q + s);
      const Vec4f ork = drk * wk - dik * tk, oik = drk * tk + dik * wk;
      const Vec4f erm = half * (r + p), eim = half * (s - q);
      const Vec4f drm = half * (r - p), dim = half * (s + q);
      const Vec4f orm = drm * wm - dim * tm, oim = drm * tm + dim * wm;
      (erk - oik).store(re + k);
      (eik + ork).store(im + k);
      (erm - oim).reversed().store(re + mm);
      (eim + orm).reversed().store(im + mm);
    }
  }
  for (; k <= h / 2; ++k) {
    const size_t m = h - k;
    const float p = re[k], q = im[k], r = re[m], s = im[m];
    {
      const float er = 0.5f * (p + r), ei = 0.5f * (q - s);
      const float dr = 0.5f * (p - r), di = 0.5f * (q + s);
      const float w = cs[k], t = sn[k]; // W^-k = w + j t
      const float orr = dr * w - di * t, oi = dr * t + di * w;
      re[k] = er - oi;
      im[k] = ei + orr;
    }
    if (m != k) {
      const float er = 0.5f * (r + p), ei = 0.5f * (s - q);
      const float dr = 0.5f * (r - p), di = 0.5f * (s + q);
      const float w = cs[m], t = sn[m];
      const float orr = dr * w - di * t, oi = dr * t + di * w;
      re[m] = er - oi;
      im[m] = ei + orr;
    }
  }
  radix2<true>(h, 1, stageCos, stageSin, reversed, re, im);
  // h is a power of two of at least 4: whole vectors, no tail.
  for (size_t i = 0; i < h; i += 4)
    simd::Vec4f::interleave(simd::Vec4f::load(re + i), simd::Vec4f::load(im + i), output + 2 * i);
}

} // namespace fftdetail

/**
 * In-place complex FFT over `Size` points, plus the real-signal transforms.
 *
 * Usage is deliberately plain arrays rather than a complex type: the DSP that
 * feeds this is real audio, and forcing every caller through std::complex would
 * cost a copy for no clarity.
 *
 * Sized at compile time, so its tables are members and nothing is allocated
 * anywhere; DynamicFft is the same transform with a size chosen at run time.
 * Always our own portable code, on every platform -- the transform the
 * browser preview runs, bit for bit.
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
   *  floats and an index per point), so nothing is allocated, and a large
   *  Fft belongs in a member rather than on the stack. */
  Fft() { fftdetail::buildTables(Size, stageCos_, stageSin_, reversed_); }

  /** Forward transform, in place. `inverse` runs the conjugate transform and
   *  scales by 1/N, so forward-then-inverse returns the original signal. */
  void transform(float* real, float* imag, bool inverse = false) const {
    // The direction is a template argument rather than a test inside every
    // butterfly, which the compiler could not hoist.
    if (inverse) fftdetail::radix2<true>(Size, 0, stageCos_, stageSin_, reversed_, real, imag);
    else fftdetail::radix2<false>(Size, 0, stageCos_, stageSin_, reversed_, real, imag);
  }

  /** The spectrum of `Size` real samples, bins 0 .. Size/2 into `re` and
   *  `im` (numBins() each), unscaled like transform(): from one complex
   *  transform of HALF the size, so about twice as fast as transforming the
   *  samples with zero imaginary parts. Every Size from 8; Size 4 goes
   *  through the complex transform. No allocation. */
  void realForward(const float* input, float* re, float* im) const {
    if constexpr (Size >= 8) {
      fftdetail::realForward(Size, stageCos_, stageSin_, reversed_, input, re, im);
    } else {
      float r[Size], i[Size];
      for (size_t k = 0; k < Size; ++k) {
        r[k] = input[k];
        i[k] = 0.0f;
      }
      transform(r, i, false);
      for (size_t k = 0; k < numBins(); ++k) {
        re[k] = r[k];
        im[k] = i[k];
      }
    }
  }

  /** The inverse: numBins() bins in `re` and `im` back to `Size` real
   *  samples in `output`, scaled by 1/Size so realForward then realInverse
   *  is the identity. `re` and `im` are the working space and are
   *  OVERWRITTEN. No allocation. */
  void realInverse(float* re, float* im, float* output) const {
    if constexpr (Size >= 8) {
      fftdetail::realInverse(Size, stageCos_, stageSin_, reversed_, re, im, output);
    } else {
      float r[Size], i[Size];
      for (size_t k = 0; k < numBins(); ++k) {
        r[k] = re[k];
        i[k] = im[k];
      }
      for (size_t k = numBins(); k < Size; ++k) {
        r[k] = re[Size - k];
        i[k] = -im[Size - k];
      }
      transform(r, i, true);
      for (size_t k = 0; k < Size; ++k) output[k] = r[k];
    }
  }

  /** Magnitude spectrum of real input. `out` receives numBins() values. */
  void magnitude(const float* input, float* out) const {
    float re[numBins()], im[numBins()];
    realForward(input, re, im);
    for (size_t i = 0; i < numBins(); ++i)
      out[i] = std::sqrt(re[i] * re[i] + im[i] * im[i]);
  }

private:
  float stageCos_[Size]{}; // [half + k]: stage `half`'s twiddles; [0] unused
  float stageSin_[Size]{};
  uint32_t reversed_[Size]{};
};

/**
 * The same FFT with its size chosen at RUN time: an analyser whose
 * resolution is a setting, a convolution engine sized to the impulse it was
 * given, anything whose length is not known when the plugin is compiled.
 *
 * setSize() allocates (tables, and scratch for magnitude()) and belongs in
 * prepare() or on a background thread; every transform afterwards allocates
 * nothing. Without SONORE_FFT_ACCELERATE it is Fft<N>'s code exactly -- the
 * same bits for the same size.
 *
 * SONORE_FFT_ACCELERATE (Apple only, link the Accelerate framework) hands
 * the complex transforms to vDSP, Apple's own FFT, and keeps the real-signal
 * packing above it. It is FASTER on a Mac and it is NOT bit-identical to the
 * portable transform (a different order of operations, so the last bits
 * differ) -- which is exactly why it is opt-in: a plugin built with it no
 * longer matches its own browser preview to the bit. A plugin that needs
 * both should leave it off; one that ships natively only can turn it on.
 */
class DynamicFft {
public:
  DynamicFft() = default;
  /** An FFT of `size` points; see setSize(). */
  explicit DynamicFft(size_t size) { setSize(size); }
  DynamicFft(const DynamicFft& other) { setSize(other.size_); }
  DynamicFft& operator=(const DynamicFft& other) {
    if (this != &other) setSize(other.size_);
    return *this;
  }
  ~DynamicFft() { releaseBackend(); }

  /** Size the transform: a power of two from 4 to 2^24 points. Allocates.
   *  Returns false, and keeps the size it had, for anything else. */
  bool setSize(size_t size) {
    if (size == size_) return size != 0;
    if (!isPowerOfTwo(size) || size < 4 || size > (size_t(1) << 24)) return false;
    releaseBackend();
    size_ = size;
    stageCos_.assign(size, 0.0f);
    stageSin_.assign(size, 0.0f);
    reversed_.assign(size, 0u);
    fftdetail::buildTables(size, stageCos_.data(), stageSin_.data(), reversed_.data());
    scratchRe_.assign(size / 2 + 1, 0.0f);
    scratchIm_.assign(size / 2 + 1, 0.0f);
    order_ = 0;
    while ((size_t(1) << order_) < size) ++order_;
#if defined(SONORE_FFT_ACCELERATE) && defined(__APPLE__)
    setup_ = vDSP_create_fftsetup((vDSP_Length) order_, kFFTRadix2);
#endif
    return true;
  }

  /** Points per transform; 0 before setSize(). */
  size_t size() const { return size_; }
  /** Bins of a real signal's spectrum, DC through Nyquist: size() / 2 + 1. */
  size_t numBins() const { return size_ ? size_ / 2 + 1 : 0; }
  /** The backend the transforms run on: "portable" or "accelerate". */
  static const char* backend() {
#if defined(SONORE_FFT_ACCELERATE) && defined(__APPLE__)
    return "accelerate";
#else
    return "portable";
#endif
  }

  /** In-place complex transform of size() points, as Fft::transform():
   *  the inverse is scaled by 1/N. Does nothing before setSize(). */
  void transform(float* real, float* imag, bool inverse = false) const {
    if (!size_) return;
    complex(size_, 0, real, imag, inverse);
  }

  /** The spectrum of size() real samples, as Fft::realForward(). */
  void realForward(const float* input, float* re, float* im) const {
    if (!size_) return;
    if (size_ < 8) {
      float r[4], i[4];
      for (size_t k = 0; k < 4; ++k) {
        r[k] = input[k];
        i[k] = 0.0f;
      }
      complex(4, 0, r, i, false);
      for (size_t k = 0; k < 3; ++k) {
        re[k] = r[k];
        im[k] = i[k];
      }
      return;
    }
#if defined(SONORE_FFT_ACCELERATE) && defined(__APPLE__)
    const size_t h = size_ / 2;
    for (size_t i = 0; i < h; ++i) {
      re[i] = input[2 * i];
      im[i] = input[2 * i + 1];
    }
    complex(h, 1, re, im, false);
    unpackReal(re, im);
#else
    fftdetail::realForward(size_, stageCos_.data(), stageSin_.data(), reversed_.data(), input, re, im);
#endif
  }

  /** The inverse, as Fft::realInverse(): `re` and `im` are overwritten. */
  void realInverse(float* re, float* im, float* output) const {
    if (!size_) return;
    if (size_ < 8) {
      float r[4] = {re[0], re[1], re[2], re[1]}, i[4] = {im[0], im[1], im[2], -im[1]};
      complex(4, 0, r, i, true);
      for (size_t k = 0; k < 4; ++k) output[k] = r[k];
      return;
    }
#if defined(SONORE_FFT_ACCELERATE) && defined(__APPLE__)
    packReal(re, im);
    const size_t h = size_ / 2;
    complex(h, 1, re, im, true);
    for (size_t i = 0; i < h; ++i) {
      output[2 * i] = re[i];
      output[2 * i + 1] = im[i];
    }
#else
    fftdetail::realInverse(size_, stageCos_.data(), stageSin_.data(), reversed_.data(), re, im, output);
#endif
  }

  /** Magnitude spectrum of size() real samples into `out` (numBins()
   *  values). Uses the object's own scratch, so it is not const and two
   *  threads must not call it on one object at once. */
  void magnitude(const float* input, float* out) {
    if (!size_) return;
    realForward(input, scratchRe_.data(), scratchIm_.data());
    for (size_t i = 0; i < numBins(); ++i)
      out[i] = std::sqrt(scratchRe_[i] * scratchRe_[i] + scratchIm_[i] * scratchIm_[i]);
  }

private:
  /** A complex transform of n points (n == size_, or size_ / 2 for the real
   *  transforms, `shift` 1), inverse scaled by 1/n. */
  void complex(size_t n, unsigned shift, float* real, float* imag, bool inverse) const {
#if defined(SONORE_FFT_ACCELERATE) && defined(__APPLE__)
    DSPSplitComplex split;
    split.realp = real;
    split.imagp = imag;
    const vDSP_Length log2n = (vDSP_Length) (order_ - shift);
    vDSP_fft_zip(setup_, &split, 1, log2n, inverse ? kFFTDirection_Inverse : kFFTDirection_Forward);
    if (inverse) {
      // vDSP leaves both directions unscaled; ours scales the inverse.
      const float scale = 1.0f / (float) n;
      vDSP_vsmul(real, 1, &scale, real, 1, (vDSP_Length) n);
      vDSP_vsmul(imag, 1, &scale, imag, 1, (vDSP_Length) n);
    }
#else
    if (inverse) fftdetail::radix2<true>(n, shift, stageCos_.data(), stageSin_.data(), reversed_.data(), real, imag);
    else fftdetail::radix2<false>(n, shift, stageCos_.data(), stageSin_.data(), reversed_.data(), real, imag);
#endif
  }

#if defined(SONORE_FFT_ACCELERATE) && defined(__APPLE__)
  // The real transforms' packing, above vDSP's complex transform: the same
  // formulas as fftdetail::realForward/realInverse, after and before it.
  void unpackReal(float* re, float* im) const {
    const size_t h = size_ / 2;
    const float z0r = re[0], z0i = im[0];
    re[0] = z0r + z0i;
    im[0] = 0.0f;
    re[h] = z0r - z0i;
    im[h] = 0.0f;
    const float* cs = stageCos_.data() + h;
    const float* sn = stageSin_.data() + h;
    for (size_t k = 1; k <= h / 2; ++k) {
      const size_t m = h - k;
      const float a = re[k], b = im[k], c = re[m], d = im[m];
      {
        const float er = 0.5f * (a + c), ei = 0.5f * (b - d);
        const float orr = 0.5f * (b + d), oi = -0.5f * (a - c);
        re[k] = er + (cs[k] * orr + sn[k] * oi);
        im[k] = ei + (cs[k] * oi - sn[k] * orr);
      }
      if (m != k) {
        const float er = 0.5f * (c + a), ei = 0.5f * (d - b);
        const float orr = 0.5f * (d + b), oi = -0.5f * (c - a);
        re[m] = er + (cs[m] * orr + sn[m] * oi);
        im[m] = ei + (cs[m] * oi - sn[m] * orr);
      }
    }
  }
  void packReal(float* re, float* im) const {
    const size_t h = size_ / 2;
    const float x0 = re[0], xh = re[h];
    re[0] = 0.5f * (x0 + xh);
    im[0] = 0.5f * (x0 - xh);
    const float* cs = stageCos_.data() + h;
    const float* sn = stageSin_.data() + h;
    for (size_t k = 1; k <= h / 2; ++k) {
      const size_t m = h - k;
      const float p = re[k], q = im[k], r = re[m], s = im[m];
      {
        const float er = 0.5f * (p + r), ei = 0.5f * (q - s);
        const float dr = 0.5f * (p - r), di = 0.5f * (q + s);
        const float orr = dr * cs[k] - di * sn[k], oi = dr * sn[k] + di * cs[k];
        re[k] = er - oi;
        im[k] = ei + orr;
      }
      if (m != k) {
        const float er = 0.5f * (r + p), ei = 0.5f * (s - q);
        const float dr = 0.5f * (r - p), di = 0.5f * (s + q);
        const float orr = dr * cs[m] - di * sn[m], oi = dr * sn[m] + di * cs[m];
        re[m] = er - oi;
        im[m] = ei + orr;
      }
    }
  }
  FFTSetup setup_ = nullptr;
#endif

  void releaseBackend() {
#if defined(SONORE_FFT_ACCELERATE) && defined(__APPLE__)
    if (setup_) vDSP_destroy_fftsetup(setup_);
    setup_ = nullptr;
#endif
  }

  size_t size_ = 0;
  size_t order_ = 0;
  std::vector<float> stageCos_, stageSin_;
  std::vector<uint32_t> reversed_;
  std::vector<float> scratchRe_, scratchIm_;
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
  /** A straight ramp up to the middle and back down to zero at both ends
   *  (Bartlett): the window a running average applied twice is. Its first
   *  sidelobe is -27 dB and they fall off at 12 dB an octave. */
  Triangular,
  /** Kaiser: the window with a DIAL, its beta (fillWindow's last argument,
   *  8.6 unless given). 0 is the rectangle; every step up deepens the
   *  sidelobes and widens the main lobe -- about -30 dB at beta 4, -60 at 8,
   *  -90 at 12 -- so an analysis can buy exactly the leakage it can afford.
   *  The shape is special.h's, the one fir.h designs kernels with. */
  Kaiser,
};

/** Writes `n` samples of the window `kind` to `out`, in the symmetric form:
 *  sample 0 and sample n-1 are the window's two ends. Every shape's peak
 *  value is 1 (a sample lands on it when n is odd) except FlatTop's, which
 *  is divided by its constant term so that its mean is about 1. One sample
 *  is 1; zero samples writes nothing. `kaiserBeta` is read only by
 *  Window::Kaiser. No allocation. */
inline void fillWindow(float* out, size_t n, Window kind, double kaiserBeta = 8.6) {
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
      case Window::Triangular:
        out[i] = (float) (1.0 - std::fabs(2.0 * x - 1.0));
        break;
      case Window::Kaiser:
        out[i] = (float) special::kaiser(2.0 * x - 1.0, kaiserBeta);
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
