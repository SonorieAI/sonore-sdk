// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: rate conversion.
//
// Two jobs that share one piece of maths:
//
//   Oversampler<Stages>  runs a nonlinearity at 2×, 4×, 8×… the host's rate, so
//                        the harmonics it creates land above Nyquist and get
//                        filtered out instead of folding back into the audio as
//                        inharmonic grit. This is the difference between a
//                        saturator that measures clean on the aliasing probe
//                        and one that does not.
//
//   Resampler            arbitrary-ratio conversion, for material that arrives
//                        at the wrong rate: an impulse response recorded at
//                        44.1 kHz loaded into a 96 kHz session, a wavetable
//                        shipped at one rate and played at another.
//
// The kernels are COMPUTED, not hand-typed. A windowed-sinc table pasted into
// source is a set of magic numbers nobody can check; computing it from the
// formula is both verifiable and lets the length be a parameter.
#pragma once
#include <cmath>
#include <cstddef>
#include "audio.h"
#include "simd.h"

namespace sonore {

/**
 * A half-band FIR: passes everything below fs/4, stops everything above.
 *
 * Half-band is the right filter for power-of-two rate changes because HALF ITS
 * TAPS ARE EXACTLY ZERO: every even tap either side of the centre vanishes, so
 * the filter costs half of what its length suggests. `Taps` must be odd.
 *
 * The state is a circular buffer rather than a shifted array: shifting is O(n)
 * per sample and shows up immediately in a plugin running eight of these.
 */
template <int Taps = 33, typename T = float>
class HalfBandFilter {
  static_assert(Taps % 2 == 1, "a half-band filter needs an odd tap count");
  static_assert(Taps >= 7, "too few taps to filter anything");

public:
  /** The sample type it filters: float, or double for a double oversampler. */
  using SampleType = T;

  /** Builds the Blackman-windowed kernel, normalised to unity at DC; the
   *  state starts silent. No allocation. */
  HalfBandFilter() { buildKernel(); }

  /** Clear every history, process()'s and the polyphase paths' alike; the
   *  kernel is kept. No allocation. */
  void reset() {
    for (int i = 0; i < Taps; ++i) state_[i] = T(0.0);
    write_ = 0;
    for (int i = 0; i < kRing; ++i) evenHist_[i] = T(0.0);
    for (int i = 0; i < 2 * kRing; ++i) upAhead_[i] = upBehind_[i] = oddAhead_[i] = oddBehind_[i] = T(0.0);
    upW_ = upR_ = downW_ = oddW_ = oddR_ = 0;
  }

  /** Group delay, in samples AT THIS FILTER'S OWN RATE. Linear phase, so it
   *  is the same at every frequency, which is what makes it exact. */
  static constexpr int latency() { return (Taps - 1) / 2; }

  /**
   * One sample in at the low rate, two out at the high rate: the same two
   * values as process(u) then process(0) -- zero-stuffing and filtering --
   * computed without the arithmetic on zeros.
   *
   * With the centre at an even offset, an even output meets only the centre
   * tap (every other tap lands on a stuffed zero), so it is the input scaled
   * and delayed; an odd output meets only the odd taps, so it is an FIR of
   * Taps/4 symmetric pairs over the low-rate history. That is a third of
   * the multiplies, and summed in the same order process() sums them, so
   * the results are the same bits. `u` is the input already scaled for the
   * zero-stuffing (the caller's x2). Low-rate state of its own: a filter is
   * driven through process() or through upsample(), never both.
   */
  inline void upsample(T u, T& even, T& odd) {
    if constexpr (kCentre % 2 != 0) {
      even = process(u);
      odd = process(T(0.0));
    } else {
      const int w = (upW_ + 1) & kMask, r = (upR_ - 1) & kMask;
      upW_ = w;
      upR_ = r;
      put(upAhead_, upBehind_, w, r, u);
      const T* ahead = upAhead_ + w + kRing; // ahead[-b]: b samples back
      even = flushDenormalT(kernel_[kCentre] * ahead[-kHalf]);
      // The centre meets a stuffed zero; multiplied anyway so the sum starts
      // from the same signed zero process() starts from. Pair q (k = 2q + 1)
      // is the samples kHalf - 1 - q and kHalf + q back.
      odd = flushDenormalT(pairSum(kernel_[kCentre] * T(0.0), ahead - (kHalf - 1),
                                  upBehind_ + r + kHalf, pairKernel_));
    }
  }

  /**
   * Two samples in at the high rate, one out at the low rate: the value
   * process(v0) returns when process(v1) is then called and discarded, as a
   * decimator keeps phase 0. Only the kept output is computed -- the centre
   * against the even samples, the odd taps against the odd ones -- in
   * process()'s summation order, so it is the same bits for half the work.
   */
  inline T downsample(T v0, T v1) {
    if constexpr (kCentre % 2 != 0) {
      const T y = process(v0);
      process(v1);
      return y;
    } else {
      const int e = (downW_ + 1) & kMask, w = oddW_, r = oddR_;
      downW_ = e;
      evenHist_[e] = v0;
      // Pair q is the odd samples kHalf - q and kHalf + 1 + q pairs back,
      // counting the previous pair's as one back: the newest in the rings.
      const T acc = pairSum(kernel_[kCentre] * lowBack(evenHist_, e, kHalf),
                                oddAhead_ + w + kRing - (kHalf - 1), oddBehind_ + r + kHalf, pairKernel_);
      // This pair's odd sample after: it is newer than the output.
      oddW_ = (w + 1) & kMask;
      oddR_ = (r - 1) & kMask;
      put(oddAhead_, oddBehind_, oddW_, oddR_, v1);
      return flushDenormalT(acc);
    }
  }

  /**
   * upsample() over a block: `n` inputs from `u` (already scaled, as for
   * upsample()), 2n outputs to `out`, the same bits as n calls. The polyphase
   * sums are the same sums in the same order, worked four outputs at a time
   * from a straight copy of the history rather than one output at a time from
   * the ring: each output's sum was a chain of dependent adds, and the ring
   * was written and read back for every sample. Shares upsample()'s state, so
   * the two can be mixed on one filter.
   */
  void upsampleBlock(const T* u, int n, T* out) {
    if constexpr (kCentre % 2 != 0) {
      for (int i = 0; i < n; ++i) upsample(u[i], out[2 * i], out[2 * i + 1]);
    } else {
      // Reach: the odd sum goes back (kCentre - 1 + kCentre) / 2 samples.
      // Not a one-letter name: stb_vorbis.c defines L, C and R as macros and
      // leaves them defined, so a plugin that includes its own copy before
      // this header turned `R` into `(PLAYBACK_RIGHT | PLAYBACK_MONO)` here,
      // which gcc and clang refuse even in a branch never instantiated.
      constexpr int kReach = kCentre - 1;
      T lin[kReach + kBlock];
      while (n > 0) {
        const int m = n < kBlock ? n : kBlock;
        const T* ahead = upAhead_ + upW_ + kRing; // already in time order
        for (int b = 0; b < kReach; ++b) lin[b] = ahead[b - (kReach - 1)];
        for (int i = 0; i < m; ++i) lin[kReach + i] = u[i];
        const T* h = lin + kReach; // h[i] is input i; h[i - b] is b before it
        int i = 0;
        for (; i + 4 <= m; i += 4) {
          const simd::Vec4<T> even =
              simd::Vec4<T>::splat(kernel_[kCentre]) * simd::Vec4<T>::load(h + i - kCentre / 2);
          simd::Vec4<T> acc = simd::Vec4<T>::splat(kernel_[kCentre]) * simd::Vec4<T>::zero();
          for (int k = 1; k <= kCentre; k += 2)
            acc = acc + simd::Vec4<T>::splat(kernel_[kCentre + k]) *
                            (simd::Vec4<T>::load(h + i - (kCentre - 1 - k) / 2) +
                             simd::Vec4<T>::load(h + i - (kCentre - 1 + k) / 2));
          T e[4], o[4];
          even.store(e);
          acc.store(o);
          for (int q = 0; q < 4; ++q) {
            out[2 * (i + q)] = flushDenormalT(e[q]);
            out[2 * (i + q) + 1] = flushDenormalT(o[q]);
          }
        }
        for (; i < m; ++i) {
          out[2 * i] = flushDenormalT(kernel_[kCentre] * h[i - kCentre / 2]);
          T acc = kernel_[kCentre] * T(0.0);
          for (int k = 1; k <= kCentre; k += 2)
            acc += kernel_[kCentre + k] * (h[i - (kCentre - 1 - k) / 2] + h[i - (kCentre - 1 + k) / 2]);
          out[2 * i + 1] = flushDenormalT(acc);
        }
        putLatest(upAhead_, upBehind_, upW_, upR_, u, m);
        u += m;
        out += 2 * m;
        n -= m;
      }
    }
  }

  /**
   * downsample() over a block: 2n inputs from `v`, n outputs to `y`, the same
   * bits as n calls, four outputs at a time (see upsampleBlock()).
   */
  void downsampleBlock(const T* v, int n, T* y) {
    if constexpr (kCentre % 2 != 0) {
      for (int i = 0; i < n; ++i) y[i] = downsample(v[2 * i], v[2 * i + 1]);
    } else {
      // The even sample is read kCentre/2 back from the newest, the odd ones
      // from 1 to kCentre back: the current pair's odd sample enters after.
      constexpr int RE = kCentre / 2, RO = kCentre;
      T even[RE + kBlock], odd[RO + kBlock];
      while (n > 0) {
        const int m = n < kBlock ? n : kBlock;
        for (int b = 0; b < RE; ++b) even[RE - 1 - b] = lowBack(evenHist_, downW_, b);
        const T* ahead = oddAhead_ + oddW_ + kRing; // already in time order
        for (int b = 0; b < RO; ++b) odd[b] = ahead[b - (RO - 1)];
        for (int i = 0; i < m; ++i) {
          even[RE + i] = v[2 * i];
          odd[RO + i] = v[2 * i + 1];
        }
        const T* e = even + RE; // e[i]: pair i's even sample
        const T* o = odd + RO;  // o[i - b]: the odd sample b pairs back
        int i = 0;
        for (; i + 4 <= m; i += 4) {
          simd::Vec4<T> acc = simd::Vec4<T>::splat(kernel_[kCentre]) * simd::Vec4<T>::load(e + i - kCentre / 2);
          for (int k = 1; k <= kCentre; k += 2)
            acc = acc + simd::Vec4<T>::splat(kernel_[kCentre + k]) *
                            (simd::Vec4<T>::load(o + i - (kCentre + 1 - k) / 2) +
                             simd::Vec4<T>::load(o + i - (kCentre + 1 + k) / 2));
          T r[4];
          acc.store(r);
          for (int q = 0; q < 4; ++q) y[i + q] = flushDenormalT(r[q]);
        }
        for (; i < m; ++i) {
          T acc = kernel_[kCentre] * e[i - kCentre / 2];
          for (int k = 1; k <= kCentre; k += 2)
            acc += kernel_[kCentre + k] * (o[i - (kCentre + 1 - k) / 2] + o[i - (kCentre + 1 + k) / 2]);
          y[i] = flushDenormalT(acc);
        }
        // Only the newest kHalf even samples are ever read back.
        const int fromEven = m > kHalf ? m - kHalf : 0;
        downW_ = (downW_ + fromEven) & kMask;
        for (int j = fromEven; j < m; ++j) {
          downW_ = (downW_ + 1) & kMask;
          evenHist_[downW_] = e[j];
        }
        putLatest(oddAhead_, oddBehind_, oddW_, oddR_, o, m);
        v += 2 * m;
        y += m;
        n -= m;
      }
    }
  }

  /** [audio-thread] One sample through the filter at its own rate, out
   *  latency() samples later. A filter is driven through this or through
   *  the upsample() and downsample() calls, never both: they keep separate
   *  histories. No allocation. */
  inline T process(T x) {
    state_[write_] = x;
    // Only the centre tap and the ODD offsets contribute; the rest are zero by
    // construction, so walking them would be arithmetic on nothing.
    //
    // NOT vectorised, and that is a measured decision: gathering the folded
    // pairs out of the ring buffer into a contiguous scratch so a vector dot
    // could run made this 35% SLOWER (203x -> 132x realtime). The dot is only
    // eight elements; the scratch round-trip costs more than it saves.
    T acc = kernel_[kCentre] * tap(kCentre);
    for (int k = 1; k <= kCentre; k += 2)
      acc += kernel_[kCentre + k] * (tap(kCentre - k) + tap(kCentre + k));
    write_ = write_ == 0 ? Taps - 1 : write_ - 1;
    return flushDenormalT(acc);
  }

private:
  static constexpr int kCentre = (Taps - 1) / 2;
  /** Low-rate history for the polyphase paths: a power of two past the
   *  deepest reach, kCentre samples back, so the wrap is a mask. */
  static constexpr int ringFor(int need) {
    int size = 4;
    while (size < need) size <<= 1;
    return size;
  }
  static constexpr int kRing = ringFor(kCentre + 2);
  static constexpr int kMask = kRing - 1;
  static constexpr int kBlock = 64; // the block functions' working size

  /** The low-rate sample `back` positions before the newest at `newest`. */
  static inline T lowBack(const T* ring, int newest, int back) {
    return ring[(newest - back) & kMask];
  }

  /** The odd taps' symmetric pairs: kHalf of them, k = 1, 3, ... kCentre - 1. */
  static constexpr int kHalf = kCentre / 2;

  /**
   * A low-rate sample into a history kept twice over, so that the per-sample
   * paths read their taps as straight runs instead of one masked index each.
   *
   * `ahead` holds the samples in time order and `behind` in reverse. The
   * newest goes to slot w of one -- a step up each sample -- and slot r of
   * the other -- a step down -- and again one ring further on in each, so
   * that from it ahead[w + kRing - b] and behind[r + b] are the sample b
   * back, for every b under kRing, with no wrap. A pair's two taps sit either
   * side of the filter's centre, one walking back as the other walks forward,
   * which is why the reversed copy: both halves of four pairs are then one
   * load each.
   */
  static inline void put(T* ahead, T* behind, int w, int r, T x) {
    ahead[w] = x;
    ahead[w + kRing] = x;
    behind[r] = x;
    behind[r + kRing] = x;
  }

  /** `n` samples in at once, x[n - 1] the newest, for the block paths. Every
   *  read of these histories reaches at most kCentre back, so only that many
   *  are written; the slots behind them are passed over, never read. */
  static inline void putLatest(T* ahead, T* behind, int& w, int& r, const T* x, int n) {
    const int from = n > kCentre ? n - kCentre : 0;
    int wi = (w + from) & kMask, ri = (r - from) & kMask;
    for (int j = from; j < n; ++j) {
      wi = (wi + 1) & kMask;
      ri = (ri - 1) & kMask;
      put(ahead, behind, wi, ri, x[j]);
    }
    w = wi;
    r = ri;
  }

  /**
   * acc plus pair[q] * (left[q] + right[q]) for every pair, added one at a
   * time in pair order: process()'s sums, the same bits. Only the pair sums
   * and the products -- which no order touches -- are worked four at a time;
   * the chain of additions is left as it was.
   */
  static inline T pairSum(T acc, const T* left, const T* right, const T* pair) {
    int q = 0;
    for (; q + 4 <= kHalf; q += 4) {
      T t[4];
      ((simd::Vec4<T>::load(left + q) + simd::Vec4<T>::load(right + q)) * simd::Vec4<T>::load(pair + q))
          .store(t);
      acc += t[0];
      acc += t[1];
      acc += t[2];
      acc += t[3];
    }
    if constexpr (kHalf % 4 != 0)
      for (; q < kHalf; ++q) acc += pair[q] * (left[q] + right[q]);
    return acc;
  }

  /** The sample `offset` positions back from the newest one. */
  inline T tap(int offset) const {
    int i = write_ + offset;
    if (i >= Taps) i -= Taps;
    return state_[i];
  }

  void buildKernel() {
    const double pi = (double) piT<T>;
    double sum = 0.0;
    for (int n = 0; n < Taps; ++n) {
      const int k = n - kCentre;
      double h;
      if (k == 0) {
        h = 0.5; // sinc(0) * 0.5: the half-band centre tap
      } else if (k % 2 == 0) {
        h = 0.0; // every even offset is a zero of sinc(k/2): this IS half-band
      } else {
        const double x = 0.5 * (double) k;
        h = std::sin(pi * x) / (pi * x) * 0.5;
      }
      // Blackman window: ~-74 dB sidelobes, which is what actually decides how
      // much aliasing survives. A rectangular window would leak far more than
      // the extra taps could ever make up for.
      const double w = 0.42 - 0.5 * std::cos(2.0 * pi * n / (Taps - 1)) +
                       0.08 * std::cos(4.0 * pi * n / (Taps - 1));
      const double v = h * w;
      kernel_[n] = (T) v;
      sum += v;
    }
    // Unity DC gain, so a signal that passes through unchanged really is
    // unchanged: the windowing above would otherwise leave a small droop.
    if (sum > 1e-12) {
      for (int n = 0; n < Taps; ++n) kernel_[n] = (T) (kernel_[n] / sum);
    }
    for (int q = 0; q < kHalf; ++q) pairKernel_[q] = kernel_[kCentre + 1 + 2 * q];
  }

  T kernel_[Taps]{};
  T pairKernel_[kHalf > 0 ? kHalf : 1]{}; // kernel_[kCentre + 1 + 2q], pair q's tap
  T state_[Taps]{};
  int write_ = 0;
  // The polyphase paths' low-rate histories: upsample()'s input, and
  // downsample()'s even and odd samples. The two read as pairs are kept
  // twice over (see put()); the even one is read at a single tap.
  T upAhead_[2 * kRing]{}, upBehind_[2 * kRing]{};
  T oddAhead_[2 * kRing]{}, oddBehind_[2 * kRing]{};
  T evenHist_[kRing]{};
  int upW_ = 0, upR_ = 0, downW_ = 0, oddW_ = 0, oddR_ = 0;
};

/**
 * A half-band built from two ALLPASS chains instead of an FIR.
 *
 * The idea is standard (a polyphase IIR half-band, as in Valimaki's work and
 * every serious oversampler): a half-band response can be written as
 *
 *     H(z) = ½·( A₀(z²) + z⁻¹·A₁(z²) )
 *
 * where A₀ and A₁ are cascades of second-order allpass sections. Because
 * allpass sections are unit-gain by construction, the filtering costs ONE
 * multiply per section per sample (roughly a quarter of the FIR's work for the
 * same stopband), and the phase response is minimum-phase, so the delay is a
 * few samples, varying with frequency, rather than the FIR's sixteen (see
 * latency()).
 *
 * The trade against the FIR path is real and worth stating: phase is NOT
 * linear, so this is the wrong choice when a plugin is summed against a dry
 * path that has to stay phase-coherent. The FIR is the default for that reason;
 * this is the option for a plugin that wants latency-free saturation.
 *
 * The coefficients are DERIVED, not tabulated: a Butterworth half-band's poles
 * are purely imaginary in the z-plane, and squaring them gives the allpass
 * coefficients directly. That means the order is a parameter rather than a
 * number of magic constants nobody can check, and the test measures the
 * resulting response instead of trusting it.
 */
template <int Order = 5, typename T = float>
class PolyphaseIirHalfBand {
  static_assert(Order % 2 == 1, "a polyphase half-band needs an odd order");
  static_assert(Order >= 3 && Order <= 15, "order outside the useful range");

public:
  /** The sample type it filters: float, or double for a double oversampler. */
  using SampleType = T;

  /** Allpass sections across both branches: (Order - 1) / 2. */
  static constexpr int kSectionsTotal = (Order - 1) / 2;

  /** Minimum phase: the delay varies with frequency, so there is no single
   *  integer that describes it. Zero is what gets reported, and the trade is
   *  worth stating plainly rather than implying it away.
   *
   *  This comment used to say "a fraction of a sample". That was measured and
   *  it is not true: cross-correlated against broadband noise, a 2x chain
   *  delays by about THREE samples, and more with more stages. Sixty
   *  microseconds is nothing to a listener and is not nothing to a null test
   *  -- a plugin using this filter and reporting zero sits three samples
   *  behind an unprocessed parallel track, which is a notch around 8 kHz.
   *
   *  It is still reported as zero, and deliberately: the delay is not the same
   *  at every frequency, so compensating by any one integer would leave the
   *  rest of the spectrum wrong in the other direction. A DSP that needs
   *  sample-exact parallel behaviour should use the linear-phase Oversampler,
   *  which has an exact integer delay and declares it. That is the actual
   *  choice between the two filters, and it was buried under a sentence that
   *  made this one sound free.
   *
   *  sdk_tests pins the measured figure, so it cannot grow unnoticed. */
  static constexpr int latency() { return 0; }

  /** Derives the allpass coefficients for this order; the state starts
   *  silent. No allocation. */
  PolyphaseIirHalfBand() { design(); }

  /** Clear both branches' state; the design is kept. */
  void reset() {
    for (int i = 0; i < kSectionsTotal; ++i) sections_[i].reset();
    delay_ = T(0.0);
  }

  /** Filter one sample. Both branches advance every call. */
  inline T process(T x) {
    // The z^-1 sits on branch 0, not branch 1. With it on the other branch the
    // filter is the power-complementary HIGHPASS: the response comes out
    // mirrored about fs/4, which measures as a perfectly healthy-looking filter
    // doing exactly the wrong job.
    T a = delay_;
    delay_ = x;
    for (int i = 0; i < branch0_; ++i) a = sections_[i].process(a);
    T b = x;
    for (int i = branch0_; i < kSectionsTotal; ++i) b = sections_[i].process(b);
    return T(0.5) * (a + b);
  }

  /** The oversampler's interface, as HalfBandFilter offers it: here simply
   *  the filter run on the stuffed and on the discarded samples, as before. */
  inline void upsample(T u, T& even, T& odd) {
    even = process(u);
    odd = process(T(0.0));
  }
  /** Two samples in at the high rate, one out: the output for `v0`, with
   *  `v1` filtered and discarded, keeping phase 0 as HalfBandFilter's
   *  downsample() does. */
  inline T downsample(T v0, T v1) {
    const T y = process(v0);
    process(v1);
    return y;
  }

  /**
   * The two branch outputs, which is what an oversampler actually wants: for
   * upsampling they ARE the two output phases, and for downsampling averaging
   * them is the filtered result. Splitting the work this way is what makes the
   * polyphase form cheap: neither branch ever computes a sample that is then
   * thrown away.
   */
  inline void processPhases(T x, T* phase0, T* phase1) {
    T a = delay_;
    delay_ = x;
    for (int i = 0; i < branch0_; ++i) a = sections_[i].process(a);
    T b = x;
    for (int i = branch0_; i < kSectionsTotal; ++i) b = sections_[i].process(b);
    *phase0 = a;
    *phase1 = b;
  }

private:
  /** Allpass of the form (a + z⁻²)/(1 + a·z⁻²). Four state words, one multiply. */
  struct Allpass2 {
    T a = T(0.0), x1 = T(0.0), x2 = T(0.0), y1 = T(0.0), y2 = T(0.0);
    /** Clear the state; the coefficient `a` is kept. */
    void reset() { x1 = x2 = y1 = y2 = T(0.0); }
    /** One sample through the section. */
    inline T process(T x) {
      // (a + z^-2) / (1 + a z^-2)  =>  y[n] = a*x[n] + x[n-2] - a*y[n-2].
      // The signs are the whole filter: get them wrong and the section is still
      // stable and still unit-gain, so it LOOKS fine and simply does not filter.
      const T y = a * (x - y2) + x2;
      x2 = x1; x1 = x;
      // The STATE flushed, not only what is returned. y1 kept the raw value,
      // so one NaN or infinity came back through every later sample -- the
      // section returned 0 from then on and the IIR oversampler was silent
      // for good -- and a decaying tail sat in the feedback as denormals.
      // The input's own copies leave after two samples; this is the one that
      // fed itself.
      y2 = y1; y1 = flushDenormalT(y);
      return y1;
    }
  };

  void design() {
    // Butterworth poles of order `Order`, bilinear-transformed with the
    // half-band's cutoff (ω = π/2, so the prewarp factor tan(π/4) is exactly 1).
    // The resulting z-plane poles sit on the imaginary axis; their squared
    // magnitudes are the allpass coefficients.
    const double pi = (double) piT<T>;
    double coeffs[kSectionsTotal > 0 ? kSectionsTotal : 1]{};
    int found = 0;
    for (int k = 0; k < Order && found < kSectionsTotal; ++k) {
      const double theta = pi * (2.0 * k + 1.0) / (2.0 * Order);
      // Analog LHP pole s = -sin(theta) + j·cos(theta); take the upper half only,
      // since conjugates give the same coefficient.
      if (std::cos(theta) <= 1e-12) continue;
      const double sr = -std::sin(theta);
      const double si = std::cos(theta);
      // Bilinear: z = (1 + s)/(1 - s)
      const double dr = 1.0 - sr, di = -si;
      const double nr = 1.0 + sr, ni = si;
      const double den = dr * dr + di * di;
      const double zr = (nr * dr + ni * di) / den;
      const double zi = (ni * dr - nr * di) / den;
      // Half-band poles are purely imaginary; |z|² is the allpass coefficient.
      coeffs[found++] = zr * zr + zi * zi;
    }

    // The poles come out of the formula in the order the two polyphase branches
    // want them; alternating from there is the decomposition.
    branch0_ = (found + 1) / 2;
    int b0 = 0, b1 = branch0_;
    for (int i = 0; i < found; ++i) {
      const int slot = (i % 2 == 0) ? b0++ : b1++;
      sections_[slot].a = (T) coeffs[i];
    }
  }

  Allpass2 sections_[kSectionsTotal > 0 ? kSectionsTotal : 1];
  int branch0_ = 0;
  T delay_ = T(0.0);
};

namespace oversampledetail {
/** A filter's block path when it has one, else its per-sample calls. */
template <typename F, typename = void>
struct HasBlock : std::false_type {};
/** The match for HasBlock: F declares upsampleBlock(), so the block path is
 *  used (downsampleBlock() is assumed beside it). */
template <typename F>
struct HasBlock<F, std::void_t<decltype(std::declval<F&>().upsampleBlock(nullptr, 0, nullptr))>>
    : std::true_type {};
template <typename Filter, typename T>
inline void upBlock(Filter& f, const T* u, int n, T* out) {
  if constexpr (HasBlock<Filter>::value) f.upsampleBlock(u, n, out);
  else
    for (int i = 0; i < n; ++i) f.upsample(u[i], out[2 * i], out[2 * i + 1]);
}
template <typename Filter, typename T>
inline void downBlock(Filter& f, const T* v, int n, T* y) {
  if constexpr (HasBlock<Filter>::value) f.downsampleBlock(v, n, y);
  else
    for (int i = 0; i < n; ++i) y[i] = f.downsample(v[2 * i], v[2 * i + 1]);
}
} // namespace oversampledetail

/**
 * Oversampling by 2^Stages, built from cascaded half-band stages.
 *
 * Usage is one sample in, one sample out. The shaper simply runs `2^Stages`
 * times in between:
 *
 *   float y = os.process(x, [drive](float s) { return std::tanh(s * drive); });
 *
 * LATENCY IS REAL and must be declared to the host: the half-band filters delay
 * the signal, and a plugin that reports zero smears every parallel mix it sits
 * in. `latencySamples()` is the number to hand back from the DSP's own
 * `latencySamples()`.
 *
 * With Taps = 33 that latency is a whole number of samples at every factor:
 * 32·(1 − 2^−Stages), which is why 33 is the default rather than a rounder
 * looking 32 or 31.
 */
template <int Stages, typename Filter>
class OversamplerT {
  // 32x (Stages = 5) is the ceiling, and the reason is the LATENCY, not taste:
  // the cascade delay is (Taps-1)·(1 − 2^−Stages), which stays a whole number
  // of samples only while 2^Stages divides (Taps-1). With the default 33 taps
  // that holds to 32x (31 samples) and breaks at 64x (31.5). And half a sample
  // cannot be reported to a host, so the plugin would be permanently
  // mis-compensated. A deeper factor needs a longer kernel, deliberately chosen.
  static_assert(Stages >= 0 && Stages <= 5, "beyond 32x the cascade latency stops being an integer");
  // The same rule for any tap count, not only the default: Oversampler<2, 31>
  // measured 22.5 samples and declared 22, and <3, 35> 29.81 against 29 --
  // a plugin reporting either would be mis-compensated for good.
  static_assert(Stages == 0 || Filter::latency() == 0 ||
                    (2 * Filter::latency() * ((1 << Stages) - 1)) % (1 << Stages) == 0,
                "this tap count gives a fractional latency at this factor: (Taps - 1) must be a "
                "multiple of 2^Stages");

public:
  /** The sample type, the filters': float, or double (Oversampler<S, Taps,
   *  double>) for 64-bit processing end to end. */
  using T = typename Filter::SampleType;
  /** The oversampling factor, 2^Stages: how many times process() calls its
   *  shaper for each input sample. */
  static constexpr int factor() { return 1 << Stages; }

  /** Delay through the up/down filter chain, in samples at the BASE rate.
   *  Each stage contributes two filter delays at its own (higher) rate; folded
   *  back to the base rate the series telescopes to 2·L·(1 − 2^−Stages), where
   *  L is the filter's own-rate delay. Zero for the minimum-phase filter. */
  static constexpr int latencySamples() {
    return (Stages == 0 || Filter::latency() == 0)
               ? 0
               : (2 * Filter::latency() * ((1 << Stages) - 1)) / (1 << Stages);
  }

  /** Clear every stage's filters. No allocation. */
  void reset() {
    for (int s = 0; s < Stages; ++s) {
      up_[s].reset();
      down_[s].reset();
    }
  }

  /** [audio-thread] One sample at the base rate: raised by factor(), each
   *  of the factor() samples passed through `shaper` (any callable taking
   *  and returning a float), and brought back down. The result is
   *  latencySamples() late. The working buffers are on the stack: no
   *  allocation. */
  template <typename Shaper>
  inline T process(T x, Shaper&& shaper) {
    // `if constexpr`: Stages is a template argument, and MSVC 2022 calls a
    // plain `if` on it a constant condition (C4127) in every instance.
    if constexpr (Stages == 0) {
      return shaper(x);
    } else {
      // ── Up ────────────────────────────────────────────────────────────────
      // Each stage doubles the block by inserting a zero after every sample and
      // filtering. The ×2 restores the amplitude zero-stuffing halved.
      T buffer[factor()];
      buffer[0] = x;
      int count = 1;
      for (int s = 0; s < Stages; ++s) {
        T expanded[factor()];
        for (int i = 0; i < count; ++i) {
          // Order matters: the filter is stateful, so samples must reach it in
          // time order. Filling forwards is the only correct direction. The
          // polyphase form computes the pair without filtering the stuffed zero.
          up_[s].upsample(buffer[i] * T(2.0), expanded[i * 2], expanded[i * 2 + 1]);
        }
        count *= 2;
        for (int i = 0; i < count; ++i) buffer[i] = expanded[i];
      }

      // ── The nonlinearity, at the oversampled rate ─────────────────────────
      for (int i = 0; i < count; ++i) buffer[i] = shaper(buffer[i]);

      // ── Down ──────────────────────────────────────────────────────────────
      // Every sample enters the filter's state; only every second result is
      // kept, and only that one is computed.
      for (int s = Stages - 1; s >= 0; --s) {
        const int half = count / 2;
        for (int i = 0; i < half; ++i) {
          // Keep phase 0: the FIRST of each pair. Keeping the second lands the
          // cascade's symmetric peak between two output samples and makes the
          // total delay a half-integer, which cannot be reported to a host.
          // The polyphase form computes only the kept output.
          buffer[i] = down_[s].downsample(buffer[i * 2], buffer[i * 2 + 1]);
        }
        count = half;
      }
      return buffer[0];
    }
  }

  /**
   * process() over a block, in place: the same bits as n calls, for a fraction
   * of the cost when the filter has the polyphase block path (HalfBandFilter;
   * the IIR half-band falls back to its per-sample calls). Each stage runs over
   * the whole block before the next, which every filter sees as the same
   * sequence it would have seen sample by sample. 64-sample pieces, so the
   * working buffers stay on the stack: 64 x factor() floats.
   */
  template <typename Shaper>
  void processBlock(T* io, int n, Shaper&& shaper) {
    if constexpr (Stages == 0) {
      for (int i = 0; i < n; ++i) io[i] = shaper(io[i]);
    } else {
      constexpr int kPiece = 64;
      T a[kPiece * factor()], b[kPiece * factor()];
      while (n > 0) {
        const int m = n < kPiece ? n : kPiece;
        // Up, a stage at a time; x2 restores what zero-stuffing halves.
        for (int i = 0; i < m; ++i) a[i] = io[i] * T(2.0);
        int count = m;
        T* src = a;
        T* dst = b;
        for (int s = 0; s < Stages; ++s) {
          upBlock(up_[s], src, count, dst);
          count *= 2;
          if (s + 1 < Stages)
            for (int i = 0; i < count; ++i) dst[i] *= T(2.0);
          T* t = src;
          src = dst;
          dst = t;
        }
        for (int i = 0; i < count; ++i) src[i] = shaper(src[i]);
        // Down, keeping phase 0, as process() does.
        for (int s = Stages - 1; s >= 0; --s) {
          downBlock(down_[s], src, count / 2, dst);
          count /= 2;
          T* t = src;
          src = dst;
          dst = t;
        }
        for (int i = 0; i < m; ++i) io[i] = src[i];
        io += m;
        n -= m;
      }
    }
  }

private:
  static void upBlock(Filter& f, const T* u, int n, T* out) { oversampledetail::upBlock(f, u, n, out); }
  static void downBlock(Filter& f, const T* v, int n, T* y) { oversampledetail::downBlock(f, v, n, y); }

  Filter up_[Stages > 0 ? Stages : 1];
  Filter down_[Stages > 0 ? Stages : 1];
};

/**
 * The two flavours, and the choice between them is a real one:
 *
 *   Oversampler      linear phase, EXACT integer latency, ~17 multiplies per
 *                    filter pass. The right default, and the only correct
 *                    choice when the plugin is summed against a dry path that
 *                    must stay phase-coherent.
 *
 *   OversamplerIir   minimum phase, latency small enough to report as zero, and
 *                    2–4 multiplies per pass instead of 17. The right choice for
 *                    saturation where a few samples of frequency-dependent
 *                    delay does not matter, at the cost of a phase response
 *                    that is not flat. NOT latency-free: about three samples
 *                    at 2x, measured. Use the linear-phase Oversampler where
 *                    a parallel path has to null.
 */
template <int Stages = 1, int Taps = 33, typename T = float>
using Oversampler = OversamplerT<Stages, HalfBandFilter<Taps, T>>;

/** The minimum-phase flavour: 2^Stages oversampling through polyphase IIR
 *  half-bands of `Order` (9 by default), latency reported as 0. */
template <int Stages = 1, int Order = 9, typename T = float>
using OversamplerIir = OversamplerT<Stages, PolyphaseIirHalfBand<Order, T>>;

/** 2x, linear phase, 33 taps: 16 samples of latency at the base rate. */
using Oversampler2x = Oversampler<1>;
/** 4x, linear phase: 24 samples of latency at the base rate. */
using Oversampler4x = Oversampler<2>;
/** 8x, linear phase: 28 samples of latency at the base rate. */
using Oversampler8x = Oversampler<3>;
/** 16x. Worth it only for something violently nonlinear (a hard clipper, a
 *  fold-back) where even 8x leaves audible folded product. It costs about
 *  twice 8x for a few dB, so it is a deliberate choice, not a default. */
using Oversampler16x = Oversampler<4>;
/** 32x. The ceiling, and past the point of audible return: measured gain
 *  over 8x is a handful of dB for four times the work. Here because being
 *  unable to do it would be a limitation; not because it is a good default. */
using Oversampler32x = Oversampler<5>;
/** 2x, minimum phase: reports 0, delays about three samples (measured). */
using OversamplerIir2x = OversamplerIir<1>;
/** 4x, minimum phase: reports 0, delays more than the 2x does. */
using OversamplerIir4x = OversamplerIir<2>;
/** 8x, minimum phase: reports 0; the real delay grows with the stages. */
using OversamplerIir8x = OversamplerIir<3>;
/** 16x, minimum phase: reports 0; the real delay grows with the stages. */
using OversamplerIir16x = OversamplerIir<4>;
/** 32x, minimum phase: reports 0; the real delay grows with the stages. */
using OversamplerIir32x = OversamplerIir<5>;

/**
 * Oversampling whose FACTOR IS CHOSEN AT RUN TIME: 1x, 2x, 4x ... up to
 * 2^MaxStages, set while the plugin is being prepared rather than when it is
 * compiled. An "oversampling: 2x / 4x / 8x" menu, or a lighter factor for
 * live use and a heavier one for the offline render, without holding one
 * OversamplerT of every size.
 *
 * It is the same cascade as OversamplerT and it computes the same bits: at
 * factor 2^s it does exactly what OversamplerT<s, Filter> does, sample for
 * sample (tested, process() and processBlock(), every factor). The filters
 * for every stage up to MaxStages are held, so the memory is the largest
 * factor's; the work is the chosen one's.
 *
 * Changing the factor changes the LATENCY, so it belongs where the host can
 * be told -- prepare(), or a parameter change followed by the format's
 * latency re-notification -- and it clears the filters, which is a click on
 * a running signal. setFactor() does not allocate.
 */
template <int MaxStages, typename Filter>
class DynamicOversamplerT {
  static_assert(MaxStages >= 1 && MaxStages <= 5, "beyond 32x the cascade latency stops being an integer");
  // If the latency is a whole number at the deepest factor it is at every
  // shallower one: 2^s divides 2L(2^s - 1) exactly when 2^(s-1) divides L.
  static_assert(Filter::latency() == 0 ||
                    (2 * Filter::latency() * ((1 << MaxStages) - 1)) % (1 << MaxStages) == 0,
                "this tap count gives a fractional latency at the largest factor: (Taps - 1) must "
                "be a multiple of 2^MaxStages");

public:
  /** The filters' sample type: float, or double. */
  using T = typename Filter::SampleType;
  /** The largest factor setFactor() accepts, 2^MaxStages. */
  static constexpr int maxFactor() { return 1 << MaxStages; }

  /** The factor, a power of two from 1 (no oversampling) to maxFactor().
   *  Anything else is REFUSED, returning false and leaving the factor as it
   *  was: a factor of 3 or 48 cannot be built from half-band stages, and
   *  quietly rounding it would give the plugin a latency nobody asked for.
   *  Clears every filter (see the class comment). 1 until set. */
  bool setFactor(int factor) {
    int stages = -1;
    for (int s = 0; s <= MaxStages; ++s)
      if (factor == (1 << s)) stages = s;
    if (stages < 0) return false;
    stages_ = stages;
    reset();
    return true;
  }
  /** The factor in use: 2^stages(). */
  int factor() const { return 1 << stages_; }
  /** How many half-band stages are in use, 0 .. MaxStages. */
  int stages() const { return stages_; }

  /** Delay through the chain at the current factor, in samples at the BASE
   *  rate: OversamplerT's figure for the same number of stages. */
  int latencySamples() const {
    return (stages_ == 0 || Filter::latency() == 0)
               ? 0
               : (2 * Filter::latency() * ((1 << stages_) - 1)) / (1 << stages_);
  }

  /** Clear every stage's filters, the unused ones too. No allocation. */
  void reset() {
    for (int s = 0; s < MaxStages; ++s) {
      up_[s].reset();
      down_[s].reset();
    }
  }

  /** [audio-thread] One sample at the base rate, as OversamplerT::process():
   *  raised by factor(), each sample passed through `shaper`, brought back
   *  down; latencySamples() late. Stack buffers, no allocation. */
  template <typename Shaper>
  inline T process(T x, Shaper&& shaper) {
    if (stages_ == 0) return shaper(x);
    T buffer[1 << MaxStages];
    buffer[0] = x;
    int count = 1;
    for (int s = 0; s < stages_; ++s) {
      T expanded[1 << MaxStages];
      for (int i = 0; i < count; ++i) up_[s].upsample(buffer[i] * T(2.0), expanded[i * 2], expanded[i * 2 + 1]);
      count *= 2;
      for (int i = 0; i < count; ++i) buffer[i] = expanded[i];
    }
    for (int i = 0; i < count; ++i) buffer[i] = shaper(buffer[i]);
    for (int s = stages_ - 1; s >= 0; --s) {
      const int half = count / 2;
      for (int i = 0; i < half; ++i) buffer[i] = down_[s].downsample(buffer[i * 2], buffer[i * 2 + 1]);
      count = half;
    }
    return buffer[0];
  }

  /** process() over a block, in place: the same bits as n calls, as
   *  OversamplerT::processBlock(). 64-sample pieces on the stack:
   *  2 x 64 x maxFactor() samples. */
  template <typename Shaper>
  void processBlock(T* io, int n, Shaper&& shaper) {
    if (stages_ == 0) {
      for (int i = 0; i < n; ++i) io[i] = shaper(io[i]);
      return;
    }
    constexpr int kPiece = 64;
    T a[kPiece * (1 << MaxStages)], b[kPiece * (1 << MaxStages)];
    while (n > 0) {
      const int m = n < kPiece ? n : kPiece;
      for (int i = 0; i < m; ++i) a[i] = io[i] * T(2.0);
      int count = m;
      T* src = a;
      T* dst = b;
      for (int s = 0; s < stages_; ++s) {
        oversampledetail::upBlock(up_[s], src, count, dst);
        count *= 2;
        if (s + 1 < stages_)
          for (int i = 0; i < count; ++i) dst[i] *= T(2.0);
        T* t = src;
        src = dst;
        dst = t;
      }
      for (int i = 0; i < count; ++i) src[i] = shaper(src[i]);
      for (int s = stages_ - 1; s >= 0; --s) {
        oversampledetail::downBlock(down_[s], src, count / 2, dst);
        count /= 2;
        T* t = src;
        src = dst;
        dst = t;
      }
      for (int i = 0; i < m; ++i) io[i] = src[i];
      io += m;
      n -= m;
    }
  }

private:
  Filter up_[MaxStages];
  Filter down_[MaxStages];
  int stages_ = 0;
};

/** The linear-phase runtime-factor oversampler: up to 2^MaxStages (32x by
 *  default) through HalfBandFilter<Taps>, whose latency is a whole number at
 *  every factor with the default 33 taps. */
template <int MaxStages = 5, int Taps = 33, typename T = float>
using DynamicOversampler = DynamicOversamplerT<MaxStages, HalfBandFilter<Taps, T>>;

/** The minimum-phase runtime-factor oversampler: polyphase IIR half-bands,
 *  latency reported as 0 at every factor. */
template <int MaxStages = 5, int Order = 9, typename T = float>
using DynamicOversamplerIir = DynamicOversamplerT<MaxStages, PolyphaseIirHalfBand<Order, T>>;

/**
 * Arbitrary-ratio resampling, for material that arrives at the wrong rate.
 *
 * Windowed-sinc interpolation: for each output position, sum the input samples
 * around it weighted by a shifted sinc. When DOWN-sampling the sinc is widened
 * to the output's Nyquist, because otherwise the conversion aliases: the most
 * common mistake in a hand-rolled resampler, and it sounds like the material
 * has grit that was never in it.
 *
 * Offline by design: this is for prepare()-time work such as fitting an impulse
 * response to the host's rate, not for the audio thread. It reads and writes
 * caller-owned buffers and allocates nothing.
 */
class Resampler {
public:
  /** How many output samples `inputLength` will produce at this ratio. */
  static size_t outputLength(size_t inputLength, double ratio) {
    if (ratio <= 0.0) return 0;
    // A hair above, relative: a ratio p/q is itself rounded, so n * ratio for
    // an n that IS a multiple of q lands a few ulps under the integer --
    // 49 * (1/49) is 0.99999... -- and floor() took the sample away. A true
    // fraction is at least 1/q under the next integer, far more than this.
    const double exact = (double) inputLength * ratio;
    return (size_t) std::floor(exact + exact * 1e-12);
  }

  /**
   * `ratio` is outputRate / inputRate: 48000/44100 to bring a 44.1 kHz impulse
   * up to a 48 kHz session. Returns the number of samples written.
   */
  static size_t resample(const float* in, size_t inLength, float* out, size_t outCapacity,
                         double ratio, int halfWidth = 16) {
    if (!in || !out || inLength == 0 || ratio <= 0.0) return 0;
    // The kernel width is a quality/cost dial, not a free variable: below 1 it
    // is meaningless, and past 64 the window is wider than any audible gain.
    if (halfWidth < 1) halfWidth = 1;
    if (halfWidth > 64) halfWidth = 64;
    const size_t produced = outputLength(inLength, ratio);
    const size_t count = produced < outCapacity ? produced : outCapacity;

    // Downsampling must lower the sinc's cutoff to the NEW Nyquist, or the
    // content between the two Nyquists folds back as alias.
    const double cutoff = ratio < 1.0 ? ratio : 1.0;
    const double step = 1.0 / ratio;
    // ...and widen the kernel by the same factor. A sinc stretched by
    // 1/cutoff has its zero crossings that much further apart, so a span of
    // halfWidth INPUT samples held fewer crossings the harder it decimated:
    // at 192k -> 48k, a quarter of them. A 30 kHz tone then aliased to 18 kHz
    // at -17 dB, and a 20 kHz tone in the passband lost 2.4 dB. Measured in
    // crossings of the stretched sinc, the filter is the same at every ratio.
    const int span = (int) std::ceil((double) halfWidth / cutoff);

    for (size_t o = 0; o < count; ++o) {
      const double centre = (double) o * step;
      const long base = (long) std::floor(centre);
      double acc = 0.0;
      double norm = 0.0;
      for (int k = -span + 1; k <= span; ++k) {
        const long index = base + k;
        if (index < 0 || index >= (long) inLength) continue;
        const double distance = centre - (double) index;
        const double w = windowedSinc(distance, cutoff, halfWidth);
        acc += (double) in[index] * w;
        norm += w;
      }
      // Normalising by the weights actually used keeps the gain right at the
      // edges, where part of the kernel hangs off the end of the input.
      out[o] = (float) (norm > 1e-12 ? acc / norm : 0.0);
    }
    return count;
  }

private:
  static double windowedSinc(double distance, double cutoff, int halfWidth) {
    const double pi = (double) kPi;
    const double x = distance * cutoff;
    const double sinc = (std::fabs(x) < 1e-9) ? 1.0 : std::sin(pi * x) / (pi * x);
    // Blackman again, over the kernel's own span -- in crossings of the
    // stretched sinc, which is `x`, so the window widens with the kernel.
    const double t = (x + halfWidth) / (2.0 * halfWidth);
    if (t < 0.0 || t > 1.0) return 0.0;
    const double w = 0.42 - 0.5 * std::cos(2.0 * pi * t) + 0.08 * std::cos(4.0 * pi * t);
    return sinc * w * cutoff;
  }
};

} // namespace sonore
