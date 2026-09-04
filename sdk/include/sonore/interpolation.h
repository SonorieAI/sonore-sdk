// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: reading a stream at a rate that is not one sample per sample.
//
// Every varispeed thing in audio is this: a tape effect, a pitch shifter, a
// sampler transposing, a chorus whose delay is moving, a session running at a
// rate the material was not recorded at. The SDK could already do the whole-
// buffer case (resample.h) and the sampler had a Catmull-Rom reader welded
// into it, and there was nothing in between: nothing that takes a STREAM,
// block after block, remembering what it had from last time.
//
// That memory is the entire difficulty. An interpolator needs samples on both
// sides of the point it is reading, and at a block boundary the ones on the
// left belong to the previous call. Get that wrong and the result is a click
// every block, at exactly the block rate, which people diagnose as anything
// except an interpolator.
//
// Four points about the kernels, since choosing between them is the reason
// there is more than one:
//
//   Zero-order hold  no cost, no delay, and audible aliasing. Real use: a
//                    control signal, or a deliberate lo-fi effect.
//   Linear           a lowpass whose corner moves with the fraction, which is
//                    why a linearly-interpolated delay sounds duller as it
//                    sweeps.
//   Catmull-Rom      four points, C1 continuous, passes through its points.
//   Lagrange (3rd)   four points, the polynomial that fits them exactly.
//   Windowed sinc    what the maths actually says, at the cost of a table and
//                    a wide window; the only one whose error keeps falling as
//                    you spend more on it.
//
// Every kernel declares its LATENCY, in samples, and the test measures that
// rather than believing it. A latency that is documented and wrong is worse
// than one that is undocumented, because the host compensates by it.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>

#include "special.h"

namespace sonore {

namespace interpdetail {

constexpr double kPi = 3.14159265358979323846;

/** sin(pi x)/(pi x), with the removable singularity removed. */
inline double sinc(double x) {
  if (std::fabs(x) < 1e-9) return 1.0;
  const double a = kPi * x;
  return std::sin(a) / a;
}

} // namespace interpdetail

// ── The kernels ──────────────────────────────────────────────────────────────
//
// Each is a pure function of the last `kPoints` samples, NEWEST FIRST, and a
// fraction in [0,1). Newest-first because that is the order a stream arrives
// in, and reversing it at every call would be paying for a convention.

/** No interpolation at all: the newest sample, held. */
struct ZeroOrderHoldKernel {
  static constexpr int kPoints = 1;
  static constexpr double kLatency = 0.0;
  static constexpr const char* kName = "zero-order hold";
  static float at(const float* newestFirst, float) { return newestFirst[0]; }
};

/** Straight line between the two samples the point falls between. */
struct LinearKernel {
  static constexpr int kPoints = 2;
  static constexpr double kLatency = 1.0;
  static constexpr const char* kName = "linear";
  static float at(const float* newestFirst, float frac) {
    const float y1 = newestFirst[1]; // the older one; frac 0 lands here
    const float y2 = newestFirst[0];
    return y1 + frac * (y2 - y1);
  }
};

/** Catmull-Rom: a cubic through the middle two points, with the outer two
 *  setting the slopes. Continuous in value and first derivative, which is why
 *  a swept delay using it does not buzz. */
struct CatmullRomKernel {
  static constexpr int kPoints = 4;
  static constexpr double kLatency = 2.0;
  static constexpr const char* kName = "Catmull-Rom";
  static float at(const float* newestFirst, float frac) {
    return at4(newestFirst[3], newestFirst[2], newestFirst[1], newestFirst[0], frac);
  }

  /** The same cubic with its points spelled out OLDEST first, which is the
   *  order a delay line reads them in. Two orders, one implementation: this
   *  arithmetic used to exist twice, and the SDK has already been bitten once
   *  by a rule living in more than one place with only one copy tested. */
  static float at4(float y0, float y1, float y2, float y3, float frac) {
    const float a0 = -0.5f * y0 + 1.5f * y1 - 1.5f * y2 + 0.5f * y3;
    const float a1 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
    const float a2 = -0.5f * y0 + 0.5f * y2;
    return ((a0 * frac + a1) * frac + a2) * frac + y1;
  }
};

/** Third-order Lagrange: the unique cubic through all four points.
 *
 *  Not the same thing as Catmull-Rom, which only passes through the middle
 *  two. Fitting all four exactly costs some smoothness at the joins, and
 *  which of the two sounds better depends on the material -- hence both. */
struct Lagrange3Kernel {
  static constexpr int kPoints = 4;
  static constexpr double kLatency = 2.0;
  static constexpr const char* kName = "Lagrange 3rd order";
  static float at(const float* newestFirst, float frac) {
    // Positions -1, 0, 1, 2 with frac measured from y1.
    const float y0 = newestFirst[3], y1 = newestFirst[2];
    const float y2 = newestFirst[1], y3 = newestFirst[0];
    const float x = frac;
    const float c0 = -(x) * (x - 1.0f) * (x - 2.0f) / 6.0f;
    const float c1 = (x + 1.0f) * (x - 1.0f) * (x - 2.0f) / 2.0f;
    const float c2 = -(x + 1.0f) * (x) * (x - 2.0f) / 2.0f;
    const float c3 = (x + 1.0f) * (x) * (x - 1.0f) / 6.0f;
    return c0 * y0 + c1 * y1 + c2 * y2 + c3 * y3;
  }
};

/** Windowed sinc, from a table.
 *
 *  The weights depend only on the fraction, so they are computed ONCE for a
 *  grid of fractions and looked up. Computing them per sample would be
 *  correct and would put sixteen sin() calls in an audio callback, which is
 *  the kind of correct that gets a plugin uninstalled.
 *
 *  Between grid points the two neighbouring filters are blended. That is an
 *  approximation of an approximation, and it is why the grid is 512 wide: at
 *  that spacing the blend error is far below the window's own stopband. */
template <int HalfWidth = 8>
struct WindowedSincKernel {
  static constexpr int kPoints = HalfWidth * 2;
  static constexpr double kLatency = (double) HalfWidth;
  static constexpr const char* kName = "windowed sinc";
  static constexpr int kPhases = 512;

  static float at(const float* newestFirst, float frac) {
    const Table& table = tableInstance();
    const float scaled = frac * (float) kPhases;
    int phase = (int) scaled;
    if (phase < 0) phase = 0;
    if (phase >= kPhases) phase = kPhases - 1;
    const float blend = scaled - (float) phase;
    const float* a = table.weights[phase];
    const float* b = table.weights[phase + 1];

    float sum = 0.0f;
    for (int i = 0; i < kPoints; ++i) {
      const float w = a[i] + blend * (b[i] - a[i]);
      // newestFirst[0] is the NEWEST; tap 0 is the OLDEST, so the index runs
      // backwards. Getting this the wrong way round reverses the kernel,
      // which for a symmetric window is almost invisible and for the
      // fractional part is a delay of the wrong sign.
      sum += w * newestFirst[kPoints - 1 - i];
    }
    return sum;
  }

private:
  struct Table {
    // kPhases + 1 rows: the last one is the endpoint the blend reaches for.
    float weights[kPhases + 1][kPoints];
    Table() {
      for (int p = 0; p <= kPhases; ++p) {
        const double frac = (double) p / (double) kPhases;
        // The point being read sits at HalfWidth - 1 + frac in tap
        // coordinates, which is what makes the latency HalfWidth.
        const double centre = (double) (HalfWidth - 1) + frac;
        double sum = 0.0;
        for (int i = 0; i < kPoints; ++i) {
          const double x = (double) i - centre;
          // Kaiser, beta 8.6: about -90 dB of stopband, which is the point
          // past which the float format is the limit rather than the window.
          // The same window FIR design uses, from the same place. Both had
          // their own copy of the Bessel function it is built on until they
          // were noticed within a few weeks of each other.
          const double window = special::kaiser(x / (double) HalfWidth, 8.6);
          const double w = interpdetail::sinc(x) * window;
          weights[p][i] = (float) w;
          sum += w;
        }
        // Normalised so a constant input comes out constant. Without this the
        // gain wobbles with the fraction, which on a slow sweep is an audible
        // tremolo nobody asked for.
        if (sum > 1e-12)
          for (int i = 0; i < kPoints; ++i) weights[p][i] = (float) (weights[p][i] / sum);
      }
    }
  };

  /** Built once, on first use. A function-local static so the cost lands on
   *  whoever uses it rather than on every plugin that includes the header. */
  static const Table& tableInstance() {
    static const Table table;
    return table;
  }
};

// ── The stream reader ────────────────────────────────────────────────────────

/**
 * Reads a stream at `speedRatio` input samples per output sample, carrying
 * the samples it needs across block boundaries.
 *
 * Ratio above 1 reads faster than real time (pitch up, shorter); below 1 is
 * slower. Nothing here band-limits on the way DOWN in rate: reading at 2x
 * without a lowpass first will alias, and it will alias whatever kernel is
 * chosen, because the aliasing happened before the interpolation did. For a
 * whole buffer at a fixed ratio, resample.h does it properly.
 */
template <typename Kernel>
class Interpolator {
public:
  /** The delay this kernel imposes, in input samples. Declared by the kernel
   *  and measured by the test, because a documented latency that is wrong is
   *  worse than none: a host compensates by it. */
  static constexpr double baseLatency() { return Kernel::kLatency; }
  static constexpr const char* name() { return Kernel::kName; }

  void reset() {
    for (int i = 0; i < Kernel::kPoints; ++i) history_[i] = 0.0f;
    // ONE, not zero. At zero the first output is produced before anything has
    // been pushed, so it reads an empty history: every kernel came out one
    // sample later than it declared, and 256 outputs consumed 255 inputs.
    // Starting at one means the first output pushes its input first, which is
    // what makes zero-order hold have zero latency instead of one.
    position_ = 1.0;
  }

  /** Produce `numOut` samples, consuming from `in`. Returns how many INPUT
   *  samples were used, which is what a caller advances its own read pointer
   *  by -- it is NOT numOut * speedRatio rounded, because the fraction
   *  carried in from the last call moves it by one either way.
   *
   *  The caller must supply at least ceil(numOut * speedRatio) + 1 inputs.
   *  Passing fewer is the one thing this cannot check, since a bare pointer
   *  does not carry its length. */
  int process(double speedRatio, const float* in, float* out, int numOut) {
    if (!in || !out || numOut <= 0) return 0;
    if (speedRatio <= 0.0) speedRatio = 1.0;

    int used = 0;
    double position = position_;
    for (int i = 0; i < numOut; ++i) {
      while (position >= 1.0) {
        push(in[used++]);
        position -= 1.0;
      }
      out[i] = Kernel::at(history_, (float) position);
      position += speedRatio;
    }
    position_ = position;
    return used;
  }

  /** The same, added to what is already in `out` rather than replacing it --
   *  the shape a voice mixing into a shared bus needs. */
  int processAdding(double speedRatio, const float* in, float* out, int numOut, float gain) {
    if (!in || !out || numOut <= 0) return 0;
    if (speedRatio <= 0.0) speedRatio = 1.0;

    int used = 0;
    double position = position_;
    for (int i = 0; i < numOut; ++i) {
      while (position >= 1.0) {
        push(in[used++]);
        position -= 1.0;
      }
      out[i] += gain * Kernel::at(history_, (float) position);
      position += speedRatio;
    }
    position_ = position;
    return used;
  }

private:
  void push(float sample) {
    // Newest first, so the kernels read in the order the stream arrived.
    for (int i = Kernel::kPoints - 1; i > 0; --i) history_[i] = history_[i - 1];
    history_[0] = sample;
  }

  float history_[Kernel::kPoints] = {};
  double position_ = 1.0;
};

using ZeroOrderHoldInterpolator = Interpolator<ZeroOrderHoldKernel>;
using LinearInterpolator = Interpolator<LinearKernel>;
using CatmullRomInterpolator = Interpolator<CatmullRomKernel>;
using LagrangeInterpolator = Interpolator<Lagrange3Kernel>;
template <int HalfWidth = 8>
using WindowedSincInterpolator = Interpolator<WindowedSincKernel<HalfWidth>>;

// ── Point sampling ───────────────────────────────────────────────────────────

/**
 * Read a buffer at a fractional index, with no state.
 *
 * The other half of the problem: a sampler does not walk forwards at a fixed
 * ratio, it JUMPS -- a loop point, a new note, a reversed playback. A stream
 * reader cannot express that and this can.
 *
 * Out of range reads as silence rather than wrapping or clamping. Clamping
 * would smear the last sample into a DC step at the end of every one-shot,
 * and wrapping would splice the start of the file onto its end.
 */
template <typename Kernel>
inline float sampleAt(const float* buffer, size_t length, double position) {
  if (!buffer || length == 0) return 0.0f;
  const double base = std::floor(position);
  const float frac = (float) (position - base);
  const long index = (long) base;

  float history[Kernel::kPoints];
  // Derived from the stream reader rather than guessed, so a kernel behaves
  // identically whichever way it is fed. After pushing n samples the reader's
  // history[i] is in[n-1-i], and its output stands for time n-1-latency+frac.
  // Setting that equal to `position` gives n = index + latency + 1, so
  // history[i] = buffer[index + latency - i].
  //
  // The first version had `index + kPoints - latency - 1 - i`, which is the
  // same thing only when kPoints happens to be 2*latency+1 -- true for none
  // of these kernels. It read a ramp back crooked, which is what caught it.
  const long centre = index + (long) Kernel::kLatency;
  for (int i = 0; i < Kernel::kPoints; ++i) {
    const long at = centre - i;
    history[i] = (at >= 0 && at < (long) length) ? buffer[at] : 0.0f;
  }
  return Kernel::at(history, frac);
}

} // namespace sonore
