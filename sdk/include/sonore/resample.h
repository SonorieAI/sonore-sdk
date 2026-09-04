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
template <int Taps = 33>
class HalfBandFilter {
  static_assert(Taps % 2 == 1, "a half-band filter needs an odd tap count");
  static_assert(Taps >= 7, "too few taps to filter anything");

public:
  HalfBandFilter() { buildKernel(); }

  void reset() {
    for (int i = 0; i < Taps; ++i) state_[i] = 0.0f;
    write_ = 0;
  }

  /** Group delay, in samples AT THIS FILTER'S OWN RATE. Linear phase, so it
   *  is the same at every frequency, which is what makes it exact. */
  static constexpr int latency() { return (Taps - 1) / 2; }

  inline float process(float x) {
    state_[write_] = x;
    // Only the centre tap and the ODD offsets contribute; the rest are zero by
    // construction, so walking them would be arithmetic on nothing.
    //
    // NOT vectorised, and that is a measured decision: gathering the folded
    // pairs out of the ring buffer into a contiguous scratch so a vector dot
    // could run made this 35% SLOWER (203x -> 132x realtime). The dot is only
    // eight elements; the scratch round-trip costs more than it saves.
    float acc = kernel_[kCentre] * tap(kCentre);
    for (int k = 1; k <= kCentre; k += 2)
      acc += kernel_[kCentre + k] * (tap(kCentre - k) + tap(kCentre + k));
    write_ = write_ == 0 ? Taps - 1 : write_ - 1;
    return flushDenormal(acc);
  }

private:
  static constexpr int kCentre = (Taps - 1) / 2;

  /** The sample `offset` positions back from the newest one. */
  inline float tap(int offset) const {
    int i = write_ + offset;
    if (i >= Taps) i -= Taps;
    return state_[i];
  }

  void buildKernel() {
    const double pi = (double) kPi;
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
      kernel_[n] = (float) v;
      sum += v;
    }
    // Unity DC gain, so a signal that passes through unchanged really is
    // unchanged: the windowing above would otherwise leave a small droop.
    if (sum > 1e-12) {
      for (int n = 0; n < Taps; ++n) kernel_[n] = (float) (kernel_[n] / sum);
    }
  }

  float kernel_[Taps]{};
  float state_[Taps]{};
  int write_ = 0;
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
 * multiply per section per sample: roughly a quarter of the FIR's work for the
 * same stopband, and the phase response is minimum-phase, so the delay is a
 * fraction of a sample rather than sixteen of them.
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
template <int Order = 5>
class PolyphaseIirHalfBand {
  static_assert(Order % 2 == 1, "a polyphase half-band needs an odd order");
  static_assert(Order >= 3 && Order <= 15, "order outside the useful range");

public:
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
   *  sdk_tests pins the measured figure, so it cannot grow unnoticed
   *  for host compensation, and being able to say that is the whole reason
   *  to choose this over the FIR. */
  static constexpr int latency() { return 0; }

  PolyphaseIirHalfBand() { design(); }

  void reset() {
    for (int i = 0; i < kSectionsTotal; ++i) sections_[i].reset();
    delay_ = 0.0f;
  }

  /** Filter one sample. Both branches advance every call. */
  inline float process(float x) {
    // The z^-1 sits on branch 0, not branch 1. With it on the other branch the
    // filter is the power-complementary HIGHPASS: the response comes out
    // mirrored about fs/4, which measures as a perfectly healthy-looking filter
    // doing exactly the wrong job.
    float a = delay_;
    delay_ = x;
    for (int i = 0; i < branch0_; ++i) a = sections_[i].process(a);
    float b = x;
    for (int i = branch0_; i < kSectionsTotal; ++i) b = sections_[i].process(b);
    return 0.5f * (a + b);
  }

  /**
   * The two branch outputs, which is what an oversampler actually wants: for
   * upsampling they ARE the two output phases, and for downsampling averaging
   * them is the filtered result. Splitting the work this way is what makes the
   * polyphase form cheap: neither branch ever computes a sample that is then
   * thrown away.
   */
  inline void processPhases(float x, float* phase0, float* phase1) {
    float a = delay_;
    delay_ = x;
    for (int i = 0; i < branch0_; ++i) a = sections_[i].process(a);
    float b = x;
    for (int i = branch0_; i < kSectionsTotal; ++i) b = sections_[i].process(b);
    *phase0 = a;
    *phase1 = b;
  }

private:
  /** Allpass of the form (a + z⁻²)/(1 + a·z⁻²). Two state words, one multiply. */
  struct Allpass2 {
    float a = 0.0f, x1 = 0.0f, x2 = 0.0f, y1 = 0.0f, y2 = 0.0f;
    void reset() { x1 = x2 = y1 = y2 = 0.0f; }
    inline float process(float x) {
      // (a + z^-2) / (1 + a z^-2)  =>  y[n] = a*x[n] + x[n-2] - a*y[n-2].
      // The signs are the whole filter: get them wrong and the section is still
      // stable and still unit-gain, so it LOOKS fine and simply does not filter.
      const float y = a * (x - y2) + x2;
      x2 = x1; x1 = x;
      y2 = y1; y1 = y;
      return flushDenormal(y);
    }
  };

  void design() {
    // Butterworth poles of order `Order`, bilinear-transformed with the
    // half-band's cutoff (ω = π/2, so the prewarp factor tan(π/4) is exactly 1).
    // The resulting z-plane poles sit on the imaginary axis; their squared
    // magnitudes are the allpass coefficients.
    const double pi = (double) kPi;
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
      sections_[slot].a = (float) coeffs[i];
    }
  }

  Allpass2 sections_[kSectionsTotal > 0 ? kSectionsTotal : 1];
  int branch0_ = 0;
  float delay_ = 0.0f;
};

/**
 * Oversampling by 2^Stages, built from cascaded half-band stages.
 *
 * Usage is one sample in, one sample out: the shaper simply runs `2^Stages`
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
  // that holds to 32x (31 samples) and breaks at 64x (31.5), and half a sample
  // cannot be reported to a host, so the plugin would be permanently
  // mis-compensated. A deeper factor needs a longer kernel, deliberately chosen.
  static_assert(Stages >= 0 && Stages <= 5, "beyond 32x the cascade latency stops being an integer");

public:
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

  void reset() {
    for (int s = 0; s < Stages; ++s) {
      up_[s].reset();
      down_[s].reset();
    }
  }

  template <typename Shaper>
  inline float process(float x, Shaper&& shaper) {
    if (Stages == 0) return shaper(x);

    // ── Up ────────────────────────────────────────────────────────────────
    // Each stage doubles the block by inserting a zero after every sample and
    // filtering. The ×2 restores the amplitude zero-stuffing halved.
    float buffer[factor()];
    buffer[0] = x;
    int count = 1;
    for (int s = 0; s < Stages; ++s) {
      float expanded[factor()];
      for (int i = 0; i < count; ++i) {
        // Order matters: the filter is stateful, so samples must reach it in
        // time order. Filling forwards is the only correct direction.
        expanded[i * 2] = up_[s].process(buffer[i] * 2.0f);
        expanded[i * 2 + 1] = up_[s].process(0.0f);
      }
      count *= 2;
      for (int i = 0; i < count; ++i) buffer[i] = expanded[i];
    }

    // ── The nonlinearity, at the oversampled rate ─────────────────────────
    for (int i = 0; i < count; ++i) buffer[i] = shaper(buffer[i]);

    // ── Down ──────────────────────────────────────────────────────────────
    // Every sample must go through the filter to keep its state coherent; only
    // every second result is kept.
    for (int s = Stages - 1; s >= 0; --s) {
      const int half = count / 2;
      for (int i = 0; i < half; ++i) {
        // Keep phase 0: the FIRST of each pair. Keeping the second lands the
        // cascade's symmetric peak between two output samples and makes the
        // total delay a half-integer, which cannot be reported to a host.
        buffer[i] = down_[s].process(buffer[i * 2]);
        down_[s].process(buffer[i * 2 + 1]);
      }
      count = half;
    }
    return buffer[0];
  }

private:
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
 *                    2-4 multiplies per pass instead of 17. The right choice for
 *                    saturation where a few samples of frequency-dependent
 *                    delay does not matter, at the cost of a phase response
 *                    that is not flat. NOT latency-free: about three samples
 *                    at 2x, measured. Use the linear-phase Oversampler where
 *                    a parallel path has to null.
 */
template <int Stages = 1, int Taps = 33>
using Oversampler = OversamplerT<Stages, HalfBandFilter<Taps>>;

template <int Stages = 1, int Order = 9>
using OversamplerIir = OversamplerT<Stages, PolyphaseIirHalfBand<Order>>;

using Oversampler2x = Oversampler<1>;
using Oversampler4x = Oversampler<2>;
using Oversampler8x = Oversampler<3>;
/** 16x. Worth it only for something violently nonlinear: a hard clipper, a
 *  fold-back: where even 8x leaves audible folded product. It costs about
 *  twice 8x for a few dB, so it is a deliberate choice, not a default. */
using Oversampler16x = Oversampler<4>;
/** 32x. The ceiling, and past the point of audible return: measured gain
 *  over 8x is a handful of dB for four times the work. Here because being
 *  unable to do it would be a limitation; not because it is a good default. */
using Oversampler32x = Oversampler<5>;
using OversamplerIir2x = OversamplerIir<1>;
using OversamplerIir4x = OversamplerIir<2>;
using OversamplerIir8x = OversamplerIir<3>;
using OversamplerIir16x = OversamplerIir<4>;
using OversamplerIir32x = OversamplerIir<5>;

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
    return (size_t) std::floor((double) inputLength * ratio);
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

    for (size_t o = 0; o < count; ++o) {
      const double centre = (double) o * step;
      const long base = (long) std::floor(centre);
      double acc = 0.0;
      double norm = 0.0;
      for (int k = -halfWidth + 1; k <= halfWidth; ++k) {
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
    // Blackman again, over the kernel's own span.
    const double t = (distance + halfWidth) / (2.0 * halfWidth);
    if (t < 0.0 || t > 1.0) return 0.0;
    const double w = 0.42 - 0.5 * std::cos(2.0 * pi * t) + 0.08 * std::cos(4.0 * pi * t);
    return sinc * w * cutoff;
  }
};

} // namespace sonore
