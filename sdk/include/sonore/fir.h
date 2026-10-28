// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: FIR filters, designed and run.
//
// The biquads in dsp.h cover everything that wants to be cheap and minimum
// phase. An FIR is what you reach for when the requirement is LINEAR PHASE
// (a mastering EQ, a crossover whose bands must sum flat, a filter that must
// not smear transients), and the price is latency of exactly half the kernel.
//
// Design and processing are separate on purpose: design() allocates and
// belongs in prepare(), process() allocates nothing and is real-time safe.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

// Included by dsp.h, so `#include <sonore/dsp.h>` stays the single entry point.
#include "special.h"
#include "audio.h"
#include "simd.h"

namespace sonore {

/** Window shapes for kernel design. The trade is always the same: a narrower
 *  transition band costs worse stopband rejection, and vice versa. */
enum class FirWindow {
  Rectangular, ///< sharpest transition, -21 dB stopband: almost never right
  Hann,        ///< -44 dB
  Hamming,     ///< -53 dB
  Blackman,    ///< -74 dB, the safe default
  BlackmanHarris, ///< -92 dB, for when nothing may leak through
  /** The only one with a DIAL.
   *
   *  Every window above is a fixed trade: a stopband figure and a transition
   *  width that come as a pair, take it or leave it. Kaiser has a parameter,
   *  so a caller can ask for the attenuation the job needs and pay for
   *  exactly that in taps -- which is what makes designing to a SPECIFICATION
   *  possible rather than picking the closest name off a list. */
  Kaiser,
};

/** How sharp a Kaiser window is. Only meaningful with FirWindow::Kaiser; the
 *  fixed windows ignore it.
 *
 *  8.6 is roughly -90 dB, which is where a float kernel stops being the limit
 *  and the format is. */
struct FirWindowParams {
  /** Kaiser's beta: 0 is the rectangular window, and each step up widens
   *  the transition and deepens the stopband. designLowpass(),
   *  designHighpass() and designBandpass() take one after the window (none
   *  did, and the struct was a setting nothing read); designLowpassToSpec()
   *  works out its own. */
  double kaiserBeta = 8.6;
};

namespace firdetail {

inline double windowValue(FirWindow kind, size_t i, size_t n, double kaiserBeta = 8.6) {
  if (n <= 1) return 1.0;
  const double t = (double) i / (double) (n - 1);
  const double pi = 3.14159265358979323846;
  switch (kind) {
    case FirWindow::Rectangular:
      return 1.0;
    case FirWindow::Hann:
      return 0.5 - 0.5 * std::cos(2.0 * pi * t);
    case FirWindow::Hamming:
      return 0.54 - 0.46 * std::cos(2.0 * pi * t);
    case FirWindow::Blackman:
      return 0.42 - 0.5 * std::cos(2.0 * pi * t) + 0.08 * std::cos(4.0 * pi * t);
    case FirWindow::BlackmanHarris:
      return 0.35875 - 0.48829 * std::cos(2.0 * pi * t) + 0.14128 * std::cos(4.0 * pi * t) -
             0.01168 * std::cos(6.0 * pi * t);
    case FirWindow::Kaiser:
      // r runs -1..1 across the kernel. The shape itself lives in special.h,
      // because a resampling kernel needed the same one and grew its own copy.
      return special::kaiser(2.0 * t - 1.0, kaiserBeta);
  }
  return 1.0;
}

/** The ideal lowpass impulse response: a sinc centred on the kernel, sampled.
 *  `normalised` is the cutoff as a fraction of the sample rate (0..0.5). */
inline double sincAt(size_t i, size_t taps, double normalised) {
  const double pi = 3.14159265358979323846;
  const double centre = (double) (taps - 1) * 0.5;
  const double x = (double) i - centre;
  const double w = 2.0 * pi * normalised;
  if (std::fabs(x) < 1e-9) return 2.0 * normalised;
  return std::sin(w * x) / (pi * x);
}

/** One band of an optimal design, in cycles per sample (0 .. 0.5): the
 *  amplitude wanted across it and how much an error there counts. */
struct DesignBand {
  double lo = 0.0, hi = 0.0, desired = 0.0, weight = 1.0;
};

/** Bands that can be designed against: each inside [0, 0.5], low edge below
 *  the high one, in order and not overlapping, with a positive weight. */
inline bool validBands(const DesignBand* bands, size_t count) {
  if (count == 0) return false;
  double previous = -1.0;
  for (size_t b = 0; b < count; ++b) {
    const DesignBand& band = bands[b];
    if (!(band.lo >= 0.0) || !(band.hi <= 0.5) || !(band.lo < band.hi)) return false;
    if (!(band.lo > previous) && b > 0) return false;
    if (!(band.weight > 0.0) || !std::isfinite(band.desired)) return false;
    previous = band.hi;
  }
  return true;
}

/**
 * A symmetric (linear-phase) kernel from its amplitude response.
 *
 * The amplitude of a symmetric length-N kernel is fixed by its values at the
 * N frequencies m/N, and the kernel is their inverse DFT -- which, for a
 * symmetric kernel and a real amplitude, is a cosine sum about the centre:
 *     h[n] = (1/N) sum_{m=0}^{N-1} A(m/N) cos(2 pi m (n - (N-1)/2) / N).
 * Used by both the least-squares and the equiripple designs, which find the
 * amplitude first and the taps from it.
 */
template <typename Amplitude>
inline void kernelFromAmplitude(size_t taps, Amplitude&& amplitude, std::vector<double>& out) {
  const double pi = 3.14159265358979323846;
  std::vector<double> a(taps);
  for (size_t m = 0; m < taps; ++m) a[m] = amplitude((double) m / (double) taps);
  out.assign(taps, 0.0);
  const double centre = (double) (taps - 1) * 0.5;
  for (size_t n = 0; n <= (taps - 1) / 2; ++n) {
    double acc = 0.0;
    for (size_t m = 0; m < taps; ++m)
      acc += a[m] * std::cos(2.0 * pi * (double) m * ((double) n - centre) / (double) taps);
    out[n] = out[taps - 1 - n] = acc / (double) taps;
  }
}

/**
 * The weighted least-squares kernel of odd length `taps` (a Type I filter).
 *
 * The amplitude of a symmetric odd kernel is a cosine series,
 *     A(w) = sum_{k=0}^{M} c_k cos(k w),   M = (taps - 1) / 2,
 * and the error to minimise is sum over the bands of W_b * integral (A - D_b)^2
 * dw. Setting its gradient to zero gives the normal equations Q c = p with
 *     Q_kl = sum_b W_b integral cos(k w) cos(l w) dw,
 *     p_k  = sum_b W_b D_b integral cos(k w) dw,
 * every integral in closed form (products of cosines are sums of cosines).
 * Q is symmetric positive definite whenever the bands have any width, so a
 * Cholesky factorisation solves it. Unlike the windowed sinc this is OPTIMAL
 * for the error it measures: the total energy of the error across the bands,
 * which is the right measure when a little ripple everywhere matters less
 * than its sum. The transition bands are left out of the integrals: what the
 * kernel does there is free, and that freedom is what it spends on the bands.
 *
 * Design time only: an (M+1)-square system, solved in O(M^3).
 */
inline bool leastSquaresKernel(size_t taps, const DesignBand* bands, size_t count,
                               std::vector<double>& out) {
  if (taps < 3 || (taps & 1) == 0 || !validBands(bands, count)) return false;
  const double pi = 3.14159265358979323846;
  const size_t n = (taps - 1) / 2 + 1;
  // S(m) = integral over the bands of W cos(m w), and the same with D: both
  // are needed for 0 .. 2M, since Q uses k + l.
  std::vector<double> s(2 * n), sd(2 * n);
  for (size_t m = 0; m < 2 * n; ++m) {
    double acc = 0.0, accD = 0.0;
    for (size_t b = 0; b < count; ++b) {
      const double w1 = 2.0 * pi * bands[b].lo, w2 = 2.0 * pi * bands[b].hi;
      const double integral =
          m == 0 ? (w2 - w1)
                 : (std::sin((double) m * w2) - std::sin((double) m * w1)) / (double) m;
      acc += bands[b].weight * integral;
      accD += bands[b].weight * bands[b].desired * integral;
    }
    s[m] = acc;
    sd[m] = accD;
  }
  // Q_kl = (S(k-l) + S(k+l)) / 2, from cos a cos b = (cos(a-b) + cos(a+b)) / 2.
  std::vector<double> q(n * n), p(n);
  for (size_t k = 0; k < n; ++k) {
    p[k] = sd[k];
    for (size_t l = 0; l < n; ++l) {
      const size_t d = k > l ? k - l : l - k;
      q[k * n + l] = 0.5 * (s[d] + s[k + l]);
    }
  }
  // Cholesky: Q = L L^T, in place in the lower triangle.
  for (size_t j = 0; j < n; ++j) {
    double diag = q[j * n + j];
    for (size_t k = 0; k < j; ++k) diag -= q[j * n + k] * q[j * n + k];
    if (!(diag > 0.0)) return false;
    diag = std::sqrt(diag);
    q[j * n + j] = diag;
    for (size_t i = j + 1; i < n; ++i) {
      double v = q[i * n + j];
      for (size_t k = 0; k < j; ++k) v -= q[i * n + k] * q[j * n + k];
      q[i * n + j] = v / diag;
    }
  }
  for (size_t i = 0; i < n; ++i) { // L y = p
    double v = p[i];
    for (size_t k = 0; k < i; ++k) v -= q[i * n + k] * p[k];
    p[i] = v / q[i * n + i];
  }
  for (size_t i = n; i-- > 0;) { // L^T c = y
    double v = p[i];
    for (size_t k = i + 1; k < n; ++k) v -= q[k * n + i] * p[k];
    p[i] = v / q[i * n + i];
  }
  // c_0 is the centre tap; c_k is split between the two taps k either side.
  const size_t centre = n - 1;
  out.assign(taps, 0.0);
  out[centre] = p[0];
  for (size_t k = 1; k < n; ++k) out[centre - k] = out[centre + k] = 0.5 * p[k];
  for (double v : out)
    if (!std::isfinite(v)) return false;
  return true;
}

/**
 * The Parks-McClellan (Remez exchange) design: the symmetric kernel of length
 * `taps` whose largest WEIGHTED error over the bands is the smallest possible.
 *
 * By the alternation theorem a cosine series of r terms is that optimum
 * exactly when its weighted error reaches its maximum, with alternating
 * signs, at r + 1 frequencies. The exchange finds them: guess r + 1 points,
 * solve for the polynomial that alternates on them with a common deviation
 * delta, look where its error actually peaks across a dense grid, move the
 * points there, and repeat until the peaks are delta. Parks and McClellan,
 * "Chebyshev approximation for nonrecursive digital filters with linear
 * phase" (IEEE Trans. Circuit Theory, 1972), with the polynomial carried in
 * barycentric Lagrange form on x = cos(2 pi f), as their program does --
 * here with the barycentric weights in logarithms, so a few thousand taps
 * cannot overflow or underflow the products.
 *
 * Odd `taps` is a Type I filter (amplitude a cosine series); even is Type
 * II, whose amplitude is cos(pi f) times one -- zero at Nyquist by its
 * symmetry, so a band reaching 0.5 with a non-zero gain is refused. Returns
 * false if the bands are invalid or the exchange does not settle, and on
 * success the kernel and the final |delta| (the weighted deviation every band
 * achieves: the actual ripple in a band is delta over that band's weight).
 */
inline bool remezKernel(size_t taps, const DesignBand* bands, size_t count,
                        std::vector<double>& out, double& deviation) {
  if (taps < 3 || !validBands(bands, count)) return false;
  const bool typeTwo = (taps & 1) == 0;
  const double pi = 3.14159265358979323846;
  const size_t r = typeTwo ? taps / 2 : (taps + 1) / 2;
  if (typeTwo)
    for (size_t b = 0; b < count; ++b)
      if (bands[b].hi >= 0.5 && bands[b].desired != 0.0) return false;

  // The dense grid, about sixteen points per coefficient spread over the
  // bands' total width, every band edge on it.
  double total = 0.0;
  for (size_t b = 0; b < count; ++b) total += bands[b].hi - bands[b].lo;
  const double step = total / (16.0 * (double) r);
  std::vector<double> gridF, gridD, gridW;
  std::vector<size_t> bandStart; // first grid index of each band
  for (size_t b = 0; b < count; ++b) {
    double lo = bands[b].lo, hi = bands[b].hi;
    // Type II: the amplitude is cos(pi f) P, so D and W move into P's
    // problem as D / cos and W cos -- which is singular at Nyquist, so the
    // grid stops a step short of it (the zero there is structural anyway).
    if (typeTwo && hi > 0.5 - step) hi = 0.5 - step;
    if (hi <= lo) hi = lo;
    const size_t points = (size_t) (std::max)(2.0, std::ceil((hi - lo) / step) + 1.0);
    bandStart.push_back(gridF.size());
    for (size_t j = 0; j < points; ++j) {
      const double f = lo + (hi - lo) * (double) j / (double) (points - 1);
      const double q = typeTwo ? std::cos(pi * f) : 1.0;
      gridF.push_back(f);
      gridD.push_back(bands[b].desired / q);
      gridW.push_back(bands[b].weight * q);
    }
  }
  bandStart.push_back(gridF.size());
  const size_t grid = gridF.size();
  if (grid < r + 1) return false;
  std::vector<double> gridX(grid);
  for (size_t j = 0; j < grid; ++j) gridX[j] = std::cos(2.0 * pi * gridF[j]);
  std::vector<int> bandOf(grid);
  for (size_t b = 0; b < count; ++b)
    for (size_t j = bandStart[b]; j < bandStart[b + 1]; ++j) bandOf[j] = (int) b;

  // Initial extremals: evenly through the grid.
  std::vector<size_t> ext(r + 1);
  for (size_t i = 0; i <= r; ++i) ext[i] = (size_t) std::llround((double) i * (double) (grid - 1) / (double) r);

  // Barycentric weights 1 / prod_{j != i} (x_i - x_j) over the first `n`
  // points of x, held as a scaled value: the common scale cancels in every
  // ratio they are used in.
  auto weights = [](const std::vector<double>& x, size_t n, std::vector<double>& w) {
    std::vector<double> lg(n);
    std::vector<int> sign(n, 1);
    double top = -1e300;
    for (size_t i = 0; i < n; ++i) {
      double sum = 0.0;
      for (size_t j = 0; j < n; ++j) {
        if (j == i) continue;
        const double d = x[i] - x[j];
        if (d < 0.0) sign[i] = -sign[i];
        sum -= std::log(std::fabs(d) + 1e-300);
      }
      lg[i] = sum;
      if (sum > top) top = sum;
    }
    w.resize(n);
    for (size_t i = 0; i < n; ++i) w[i] = (double) sign[i] * std::exp(lg[i] - top);
  };

  std::vector<double> x(r + 1), y(r + 1), ad, err(grid);
  double delta = 0.0;
  bool settled = false;
  auto evaluate = [&](double at) {
    // P at x = `at`, by the barycentric form over ALL r + 1 points, as the
    // original program evaluates it. Through the first r alone, P meets the
    // last point only as well as delta was computed, and at 4095 taps and
    // 120 dB that is 2% of delta: the exchange then saw its peak error ON an
    // extremal and stalled there. Through all of them every extremal is met
    // exactly; the degree the extra point adds is zero to rounding.
    double num = 0.0, den = 0.0;
    for (size_t i = 0; i <= r; ++i) {
      const double d = at - x[i];
      if (std::fabs(d) < 1e-15) return y[i];
      const double t = ad[i] / d;
      num += t * y[i];
      den += t;
    }
    return num / den;
  };

  // The best solution seen, by its actual peak error on the grid: the one
  // that is kept, whatever the last iteration did.
  double bestPeak = std::numeric_limits<double>::infinity(), bestDelta = 0.0;
  std::vector<double> bestX, bestY, bestAd;

  for (int iteration = 0; iteration < 250; ++iteration) {
    for (size_t i = 0; i <= r; ++i) x[i] = gridX[ext[i]];
    weights(x, r + 1, ad);
    double num = 0.0, den = 0.0;
    for (size_t i = 0; i <= r; ++i) {
      const double sign = (i & 1) ? -1.0 : 1.0;
      num += ad[i] * gridD[ext[i]];
      den += ad[i] * sign / gridW[ext[i]];
    }
    if (den == 0.0) break;
    delta = num / den;
    for (size_t i = 0; i <= r; ++i) {
      const double sign = (i & 1) ? -1.0 : 1.0;
      y[i] = gridD[ext[i]] - sign * delta / gridW[ext[i]];
    }

    double peak = 0.0;
    for (size_t j = 0; j < grid; ++j) {
      err[j] = gridW[j] * (gridD[j] - evaluate(gridX[j]));
      if (std::fabs(err[j]) > peak) peak = std::fabs(err[j]);
    }
    const double absDelta = std::fabs(delta);
    if (peak < bestPeak) {
      bestPeak = peak;
      bestDelta = absDelta;
      bestX = x;
      bestY = y;
      bestAd = ad;
    }
    if (absDelta > 0.0 && (peak - absDelta) <= 1e-7 * absDelta) {
      settled = true;
      break;
    }

    // The new extremals, by the original program's single exchange: each one
    // climbs from where it was to the nearest peak of the error with its own
    // sign, never past its neighbours, so none can be lost and they cannot
    // all crowd into one place. (Moving every point to the largest peaks at
    // once -- the "multiple exchange" -- converges in fewer steps when it
    // works, and on long designs it piled the points into the transition
    // band's Gibbs peaks and never recovered: an 801-tap, 120 dB lowpass
    // went from delta 1e-16 to garbage in one step.)
    const double sgn = delta < 0.0 ? -1.0 : 1.0;
    std::vector<size_t> alt(r + 1);
    for (size_t i = 0; i <= r; ++i) {
      const double s = ((i & 1) ? -1.0 : 1.0) * sgn;
      const size_t lo = i == 0 ? 0 : alt[i - 1] + 1;
      const size_t hi = i == r ? grid - 1 : ext[i + 1] - 1;
      size_t at = ext[i] < lo ? lo : ext[i];
      if (at < hi && s * err[at + 1] > s * err[at]) {
        while (at < hi && s * err[at + 1] > s * err[at]) ++at;
      } else {
        while (at > lo && s * err[at - 1] > s * err[at]) --at;
      }
      alt[i] = at;
    }
    // A peak beyond either end with the OTHER sign, larger than the end
    // point it would displace, shifts the whole alternation by one: the
    // optimum's extremal set need not start where the guess did.
    {
      const double s0 = sgn, sr = ((r & 1) ? -1.0 : 1.0) * sgn;
      size_t left = grid, right = grid;
      double vLeft = 0.0, vRight = 0.0;
      for (size_t j = 0; j < alt[0]; ++j)
        if (-s0 * err[j] > vLeft) {
          vLeft = -s0 * err[j];
          left = j;
        }
      for (size_t j = alt[r] + 1; j < grid; ++j)
        if (-sr * err[j] > vRight) {
          vRight = -sr * err[j];
          right = j;
        }
      const double gainLeft = left < grid ? vLeft - std::fabs(err[alt[r]]) : -1.0;
      const double gainRight = right < grid ? vRight - std::fabs(err[alt[0]]) : -1.0;
      if (gainLeft > 0.0 && gainLeft >= gainRight) {
        alt.pop_back();
        alt.insert(alt.begin(), left);
      } else if (gainRight > 0.0) {
        alt.erase(alt.begin());
        alt.push_back(right);
      }
    }
    if (alt == ext) {
      // Nothing moved, yet the peak is above delta: it sits where no
      // extremal's climb reaches -- an extra ripple between two of them, or
      // past an end. Exchange that one point, keeping the alternation: it
      // replaces the neighbour of its own sign, which always raises |delta|
      // (de la Vallee Poussin). A 4095-tap, 120 dB design sat on a plateau
      // 2.6% short of the optimum without it.
      size_t worst = 0;
      for (size_t j = 1; j < grid; ++j)
        if (std::fabs(err[j]) > std::fabs(err[worst])) worst = j;
      const bool positive = err[worst] > 0.0;
      auto positiveAt = [&](size_t i) { return ((i & 1) ? -1.0 : 1.0) * sgn > 0.0; };
      size_t k = 0;
      while (k <= r && alt[k] < worst) ++k; // the first extremal at or after it
      if (k <= r && alt[k] == worst) break;  // already one: nothing to exchange
      if (k == 0) {
        if (positiveAt(0) == positive) {
          alt[0] = worst;
        } else {
          alt.pop_back();
          alt.insert(alt.begin(), worst);
        }
      } else if (k > r) {
        if (positiveAt(r) == positive) {
          alt[r] = worst;
        } else {
          alt.erase(alt.begin());
          alt.push_back(worst);
        }
      } else if (positiveAt(k - 1) == positive) {
        alt[k - 1] = worst; // between two of opposite signs: it takes its own
      } else {
        alt[k] = worst;
      }
      if (alt == ext) break;
    }
    ext = alt;
  }
  // Settled, or within a percent of it: the peak is then a hair above the
  // level the exchange was closing on, and the kernel is as good as the
  // optimum to every figure that matters. Anything further off did not
  // converge, and is refused rather than passed off as optimal.
  if (!(bestPeak < std::numeric_limits<double>::infinity()) || !(bestDelta > 0.0)) return false;
  if (!settled && bestPeak > 1.01 * bestDelta) return false;

  x = bestX;
  y = bestY;
  ad = bestAd;
  kernelFromAmplitude(taps, [&](double f) {
    const double p = evaluate(std::cos(2.0 * pi * f));
    return typeTwo ? std::cos(pi * f) * p : p;
  }, out);
  // The peak actually reached on the grid, not the level the exchange aimed
  // at: equal when settled, and the honest figure when not quite.
  deviation = bestPeak;
  for (double v : out)
    if (!std::isfinite(v)) return false;
  return true;
}

/**
 * The largest weighted error of a symmetric kernel over the bands, measured
 * on a grid eight times denser than the design's: the check a design that
 * claims to MEET a specification is held to. The exchange's own deviation is
 * read on its design grid, and the true peak between two grid points runs a
 * percent or so higher -- enough to turn "at least 80 dB" into 79.9.
 */
inline double peakWeightedError(const std::vector<double>& kernel, const DesignBand* bands,
                                size_t count) {
  // An odd symmetric kernel's amplitude is a cosine series in the taps about
  // the centre, c_0 = h[M] and c_k = 2 h[M + k], i.e. a Chebyshev series in
  // x = cos(2 pi f) -- summed by Clenshaw's recurrence, with no cosine per tap.
  const double pi = 3.14159265358979323846;
  const size_t taps = kernel.size();
  if ((taps & 1) == 0) return std::numeric_limits<double>::infinity();
  const size_t m = (taps - 1) / 2;
  double peak = 0.0;
  for (size_t b = 0; b < count; ++b) {
    const size_t points = (size_t) std::ceil(128.0 * (double) taps * (bands[b].hi - bands[b].lo)) + 2;
    for (size_t j = 0; j < points; ++j) {
      const double f = bands[b].lo + (bands[b].hi - bands[b].lo) * (double) j / (double) (points - 1);
      const double x = std::cos(2.0 * pi * f);
      double b1 = 0.0, b2 = 0.0;
      for (size_t k = m; k >= 1; --k) {
        const double b0 = 2.0 * kernel[m + k] + 2.0 * x * b1 - b2;
        b2 = b1;
        b1 = b0;
      }
      const double a = kernel[m] + x * b1 - b2;
      const double e = bands[b].weight * std::fabs(a - bands[b].desired);
      if (e > peak) peak = e;
    }
  }
  return peak;
}

} // namespace firdetail

/** One band of an optimal FIR design (FirFilter::designEquiripple and
 *  designLeastSquares): edges in Hz, the gain wanted across it (1 for a
 *  passband, 0 for a stopband), and how heavily an error there counts. */
struct FirBand {
  float lowHz = 0.0f;  ///< the band's lower edge, Hz
  float highHz = 0.0f; ///< the band's upper edge, Hz (at most Nyquist)
  float gain = 0.0f;   ///< the amplitude wanted across the band
  float weight = 1.0f; ///< how much an error here counts against the others
};

/**
 * A linear-phase FIR filter.
 *
 * Kernels are designed by the windowed-sinc method, which is the one that
 * gives a predictable, symmetric kernel, and symmetry is exactly what makes
 * the phase linear. An ODD tap count is forced for highpass and bandstop
 * designs because an even-length kernel has a null at Nyquist, which silently
 * turns a highpass into something that is not one.
 */
template <typename T>
class FirFilterT {
public:
  /**
   * A lowpass that MEETS A SPECIFICATION, rather than one built from a tap
   * count somebody guessed.
   *
   * The usual way round is backwards: a caller picks 63 taps and a Blackman
   * window and then measures what they got. What a caller actually knows is
   * the job -- "nothing above 18 kHz may get through by more than -80 dB, and
   * I can afford a 2 kHz transition" -- and the Kaiser method turns exactly
   * that into a beta and a tap count.
   *
   * `transitionHz` is the width of the ramp from passband to stopband, and it
   * is the expensive parameter: halving it doubles the taps. `stopbandDb` is
   * how far down the stopband must be, as a POSITIVE number of decibels.
   *
   * Returns the tap count it settled on, or 0 if the request cannot be met
   * within the tap limit -- refused rather than quietly delivered as a
   * shallower filter, because a caller that asked for -100 dB and got -40
   * would ship the -40.
   */
  size_t designLowpassToSpec(T cutoffHz, T sampleRate, T transitionHz,
                             T stopbandDb) {
    if (!(transitionHz > T(0.0)) || !(sampleRate > T(0.0)) || !(stopbandDb > T(0.0))) return 0;
    // Design for a little MORE than was asked for.
    //
    // Kaiser's formulas are empirical -- fitted to measured designs, not
    // derived -- and land within a decibel or two either way. Asking for -100
    // and delivering -98.9 is inside that error and still a filter that does
    // not do what it was told. Two decibels of guard band costs a handful of
    // taps and turns "about right" into "at least this much", which is what a
    // specification is for.
    const double a = (double) stopbandDb + 2.0;

    // Kaiser's own empirical formulas, and they are empirical -- fitted to
    // measured designs rather than derived. That is why the test checks the
    // filter that comes out instead of trusting the arithmetic.
    double beta = 0.0;
    if (a > 50.0)
      beta = 0.1102 * (a - 8.7);
    else if (a >= 21.0)
      beta = 0.5842 * std::pow(a - 21.0, 0.4) + 0.07886 * (a - 21.0);

    const double pi = 3.14159265358979323846;
    const double deltaOmega = 2.0 * pi * (double) transitionHz / (double) sampleRate;
    // Negative for a stopband under 6 dB, which any kernel meets -- and a
    // negative double cast to size_t is undefined, not small.
    const double exact = (std::max)(0.0, (a - 8.0) / (2.285 * deltaOmega));
    size_t taps = (size_t) std::ceil(exact) + 1;
    taps |= 1; // odd, so there is a centre tap and the phase stays linear
    if (taps < 3) taps = 3;
    if (taps > kMaxTaps) return 0;

    const double normalised = clampCutoff(cutoffHz, sampleRate);
    build(taps, FirWindow::Kaiser,
          [normalised](size_t i, size_t n) { return firdetail::sincAt(i, n, normalised); }, beta);
    return taps;
  }

  /** Lowpass at `cutoffHz`. More taps = steeper, at one sample of latency per
   *  two taps. An even count is rounded up to odd: an even kernel delays by
   *  a half-sample, which no latency report can state. `params` is read only
   *  with FirWindow::Kaiser: its beta, 8.6 unless set. */
  void designLowpass(T cutoffHz, T sampleRate, size_t taps,
                     FirWindow window = FirWindow::Blackman, FirWindowParams params = {}) {
    const double normalised = clampCutoff(cutoffHz, sampleRate);
    build(taps | 1, window, [normalised](size_t i, size_t n) {
      return firdetail::sincAt(i, n, normalised);
    }, params.kaiserBeta);
  }

  /** Highpass, by spectral inversion of the matching lowpass: negate every
   *  coefficient and add 1 at the centre. That identity is why the tap count
   *  must be odd: there has to BE a centre.
   *
   *  The lowpass is NORMALISED to unity DC before it is inverted. Inverting
   *  the raw windowed sinc -- the first version -- leaves the highpass with a
   *  DC gain of 1 minus whatever the window made the sum, which for a
   *  201-tap Blackman is -40 dB of DC coming through a filter whose one job
   *  is to stop it. Normalised, the coefficients sum to zero to float
   *  precision and the null is real. `params` as designLowpass(). */
  void designHighpass(T cutoffHz, T sampleRate, size_t taps,
                      FirWindow window = FirWindow::Blackman, FirWindowParams params = {}) {
    designLowpass(cutoffHz, sampleRate, taps | 1, window, params);
    for (T& c : coefficients_) c = -c;
    coefficients_[(coefficients_.size() - 1) / 2] += T(1.0);
  }

  /** Bandpass: the difference of two lowpass kernels, each normalised to
   *  unity DC first so their difference is zero there exactly. `params` as
   *  designLowpass(). */
  void designBandpass(T lowHz, T highHz, T sampleRate, size_t taps,
                      FirWindow window = FirWindow::Blackman, FirWindowParams params = {}) {
    const T a = lowHz < highHz ? lowHz : highHz;
    const T b = lowHz < highHz ? highHz : lowHz;
    designLowpass(b, sampleRate, taps | 1, window, params);
    std::vector<T> upper = coefficients_; // design time: allocation is allowed here
    designLowpass(a, sampleRate, taps | 1, window, params);
    for (size_t i = 0; i < coefficients_.size() && i < upper.size(); ++i)
      coefficients_[i] = upper[i] - coefficients_[i];
  }

  /**
   * The weighted LEAST-SQUARES kernel over a set of bands: the one whose error
   * energy, summed over the bands with their weights, is the smallest any
   * kernel of this length can have. The gaps between the bands are transition
   * bands, free to do anything; nothing is windowed.
   *
   * Against the windowed sinc this is the better kernel for the same length
   * whenever ripple is judged by its total rather than its peak, and the bands
   * and weights are a DESIGN rather than a window's side effect. `taps` is
   * rounded up to odd, as everywhere here. Returns false, with the filter
   * unchanged, for bands that overlap, leave [0, Nyquist], or have no width.
   * Design time: allocates, and solves a (taps/2)-square system.
   */
  bool designLeastSquares(const FirBand* bands, size_t numBands, T sampleRate, size_t taps) {
    std::vector<firdetail::DesignBand> design;
    if (!toDesignBands(bands, numBands, sampleRate, design)) return false;
    std::vector<double> kernel;
    if (!firdetail::leastSquaresKernel(clampTaps(taps | 1), design.data(), design.size(), kernel))
      return false;
    install(kernel);
    return true;
  }

  /** A least-squares lowpass: unity up to cutoffHz - transitionHz / 2, zero
   *  from cutoffHz + transitionHz / 2, the stopband's error weighted by
   *  `stopbandWeight` against the passband's 1 (larger buys a deeper stopband
   *  with a little more passband ripple). Returns false, unchanged, when the
   *  transition does not fit between 0 and Nyquist. */
  bool designLowpassLeastSquares(T cutoffHz, T sampleRate, size_t taps, T transitionHz,
                                 T stopbandWeight = T(1.0)) {
    // FirBand describes a design, in float whatever the filter's own type.
    const FirBand bands[2] = {{0.0f, (float) (cutoffHz - transitionHz * T(0.5)), 1.0f, 1.0f},
                              {(float) (cutoffHz + transitionHz * T(0.5)), (float) (sampleRate * T(0.5)), 0.0f,
                               (float) stopbandWeight}};
    return designLeastSquares(bands, 2, sampleRate, taps);
  }

  /**
   * The EQUIRIPPLE kernel over a set of bands (Parks-McClellan): the one whose
   * largest weighted error is the smallest any kernel of this length can have.
   *
   * That is the optimum a specification is written in -- "ripple at most this,
   * rejection at least that" are both statements about a PEAK -- so for a given
   * spec this is the shortest kernel there is, often by a third against a
   * Kaiser window. Its signature is the ripple: every band ripples by the same
   * weighted amount, all the way across.
   *
   * Returns the weighted deviation achieved (the error in a band is it divided
   * by that band's weight), or a negative number -- the filter unchanged -- when
   * the bands are invalid or the exchange does not settle. `taps` is rounded up
   * to odd. Design time: allocates; a few hundred taps take milliseconds.
   */
  double designEquiripple(const FirBand* bands, size_t numBands, T sampleRate, size_t taps) {
    std::vector<firdetail::DesignBand> design;
    if (!toDesignBands(bands, numBands, sampleRate, design)) return -1.0;
    std::vector<double> kernel;
    double deviation = 0.0;
    if (!firdetail::remezKernel(clampTaps(taps | 1), design.data(), design.size(), kernel, deviation))
      return -1.0;
    install(kernel);
    return deviation;
  }

  /**
   * The shortest equiripple lowpass that meets a specification: flat to within
   * `rippleDb` (peak to peak) up to `passEdgeHz`, at least `stopbandDb` down
   * from `stopEdgeHz` to Nyquist. The counterpart of designLowpassToSpec() for
   * the optimal design rather than the Kaiser window, and usually a good deal
   * shorter for the same job.
   *
   * The length starts at Kaiser's estimate for an equiripple design and moves
   * by two until it is the shortest that the exchange proves meets the spec.
   * Returns the tap count, or 0 -- the filter unchanged -- if the spec cannot be
   * met within the tap limit.
   */
  size_t designEquirippleLowpassToSpec(T passEdgeHz, T stopEdgeHz, T sampleRate,
                                       T rippleDb, T stopbandDb) {
    if (!(sampleRate > T(0.0)) || !(passEdgeHz > T(0.0)) || !(stopEdgeHz > passEdgeHz) ||
        !(stopEdgeHz < sampleRate * T(0.5)) || !(rippleDb > T(0.0)) || !(stopbandDb > T(0.0)))
      return 0;
    const double g = std::pow(10.0, (double) rippleDb / 20.0);
    const double dp = (g - 1.0) / (g + 1.0); // peak deviation of a ripple measured peak to peak
    const double ds = std::pow(10.0, -(double) stopbandDb / 20.0);
    const firdetail::DesignBand bands[2] = {
        {0.0, (double) passEdgeHz / sampleRate, 1.0, 1.0},
        {(double) stopEdgeHz / sampleRate, 0.5, 0.0, dp / ds}};
    // Kaiser's estimate of an equiripple filter's length.
    const double df = (double) (stopEdgeHz - passEdgeHz) / (double) sampleRate;
    const double estimate = (-20.0 * std::log10(std::sqrt(dp * ds)) - 13.0) / (14.6 * df) + 1.0;
    return searchLength(estimate, [&](size_t taps, std::vector<double>& kernel) {
      double deviation = 0.0;
      return firdetail::remezKernel(taps, bands, 2, kernel, deviation) &&
             firdetail::peakWeightedError(kernel, bands, 2) <= dp;
    }, 1);
  }

  /**
   * An equiripple HALF-BAND lowpass: the filter a 2x resampler wants, with its
   * cutoff at a quarter of the rate and every other tap exactly zero.
   *
   * Symmetric about fs/4, so the passband ripple and the stopband rejection
   * are one number, and the zeros halve the multiplies. Built by the
   * Vaidyanathan-Nguyen construction (IEEE Trans. Circuits Syst., 1987): an
   * equiripple Type II kernel g of half the length is designed over the single
   * band [0, 2 fp], and the half-band is g spread onto the even taps, halved,
   * with 1/2 at the centre. Optimal, exactly half-band, from one exchange.
   *
   * `transitionHz` is the full width of the transition, centred on fs/4.
   * `taps` is rounded up to the next 4m - 1, the only lengths a half-band has.
   * Returns the rejection achieved, in positive dB, or 0 with the filter
   * unchanged if the request cannot be designed.
   */
  T designHalfBand(T transitionHz, T sampleRate, size_t taps) {
    if (!(sampleRate > T(0.0)) || !(transitionHz > T(0.0)) || transitionHz >= sampleRate * T(0.5)) return T(0.0);
    const size_t m = (clampTaps(taps) + 1 + 3) / 4;
    std::vector<double> kernel;
    double deviation = 0.0;
    const double fp = 0.25 - 0.5 * (double) transitionHz / (double) sampleRate;
    if (!halfBandKernel(m, fp, kernel, deviation)) return T(0.0);
    install(kernel);
    return deviation > 0.0 ? (T) (-20.0 * std::log10(deviation)) : T(300.0);
  }

  /** The shortest equiripple half-band with at least `stopbandDb` of
   *  rejection (and so at most that much passband ripple) across a transition
   *  `transitionHz` wide, centred on fs/4. Returns the tap count, or 0 with the
   *  filter unchanged if it cannot be met within the tap limit. */
  size_t designHalfBandToSpec(T transitionHz, T sampleRate, T stopbandDb) {
    if (!(sampleRate > T(0.0)) || !(transitionHz > T(0.0)) || transitionHz >= sampleRate * T(0.5) ||
        !(stopbandDb > T(0.0)))
      return 0;
    const double target = std::pow(10.0, -(double) stopbandDb / 20.0);
    const double fp = 0.25 - 0.5 * (double) transitionHz / (double) sampleRate;
    const double df = (double) transitionHz / (double) sampleRate;
    const double estimate = ((double) stopbandDb - 7.95) / (14.36 * df) + 1.0;
    return searchLength(estimate, [&](size_t taps, std::vector<double>& kernel) {
      double deviation = 0.0;
      if (!halfBandKernel((taps + 1) / 4, fp, kernel, deviation)) return false;
      const firdetail::DesignBand stop{0.25 + 0.5 * df, 0.5, 0.0, 1.0};
      return firdetail::peakWeightedError(kernel, &stop, 1) <= target;
    }, 2);
  }

  /** Install a kernel designed elsewhere (a measured impulse response, say). */
  void setCoefficients(const T* taps, size_t count) {
    coefficients_.assign(taps, taps + count);
    history_.assign(count, T(0.0));
    position_ = 0;
  }

  /** Clear the history; the kernel is kept. No allocation. */
  void reset() {
    std::fill(history_.begin(), history_.end(), T(0.0));
    position_ = 0;
  }

  /** The kernel's length in taps: odd for every kernel designed here, 0
   *  before the first design. */
  size_t numTaps() const { return coefficients_.size(); }

  /** Latency in samples: half the kernel, which is what linear phase costs.
   *  A plugin using this MUST report it or the host cannot time-align it. */
  int latencySamples() const {
    return coefficients_.empty() ? 0 : (int) ((coefficients_.size() - 1) / 2);
  }

  /** [audio-thread] One sample through the kernel: a dot product over
   *  numTaps() taps, no allocation. With no kernel yet the input comes back
   *  unchanged. */
  inline T process(T x) {
    if (coefficients_.empty()) return x;
    const size_t n = coefficients_.size();
    history_[position_] = x;

    // Two straight runs rather than one modulo per tap: the wrap is computed
    // once instead of n times, which is most of the cost at short kernels.
    T acc = T(0.0);
    const size_t first = n - position_;
    acc += simd::dot(coefficients_.data(), history_.data() + position_, first);
    if (position_ > 0) acc += simd::dot(coefficients_.data() + first, history_.data(), position_);

    position_ = position_ == 0 ? n - 1 : position_ - 1;
    return acc;
  }

  /** The kernel, tap 0 first: the coefficient the newest sample is
   *  multiplied by. The reference is valid until the next design or
   *  setCoefficients() call. */
  const std::vector<T>& coefficients() const { return coefficients_; }

private:
  static size_t clampTaps(size_t taps) { return taps < 3 ? 3 : (taps > kMaxTaps ? kMaxTaps : taps); }

  /** Hz bands to the normalised ones the designs take; false for a rate or a
   *  band that cannot be designed against. */
  static bool toDesignBands(const FirBand* bands, size_t count, T sampleRate,
                            std::vector<firdetail::DesignBand>& out) {
    if (!bands || count == 0 || !(sampleRate > T(0.0))) return false;
    out.clear();
    for (size_t b = 0; b < count; ++b) {
      firdetail::DesignBand d;
      d.lo = (double) bands[b].lowHz / (double) sampleRate;
      d.hi = (double) bands[b].highHz / (double) sampleRate;
      // A top edge given as Nyquist in float lands a rounding either side.
      if (d.hi > 0.5 && d.hi < 0.5 + 1e-6) d.hi = 0.5;
      d.desired = (double) bands[b].gain;
      d.weight = (double) bands[b].weight;
      out.push_back(d);
    }
    return firdetail::validBands(out.data(), out.size());
  }

  /** The half-band of length 4m - 1 from the Type II kernel of length 2m:
   *  h[2i] = g[i] / 2, the centre 1/2, every other odd tap zero. */
  static bool halfBandKernel(size_t m, double fp, std::vector<double>& kernel, double& deviation) {
    if (m < 1 || !(fp > 0.0) || !(fp < 0.25) || 4 * m - 1 > kMaxTaps) return false;
    const firdetail::DesignBand band{0.0, 2.0 * fp, 1.0, 1.0};
    std::vector<double> g;
    if (!firdetail::remezKernel(2 * m, &band, 1, g, deviation)) return false;
    kernel.assign(4 * m - 1, 0.0);
    for (size_t i = 0; i < 2 * m; ++i) kernel[2 * i] = 0.5 * g[i];
    kernel[2 * m - 1] = 0.5;
    deviation *= 0.5;
    return true;
  }

  /** The shortest length, in steps of `stride` pairs of taps (1: odd lengths,
   *  2: the 4m - 1 of a half-band), at which `meets` designs a kernel that
   *  passes. Starts at `estimate`, walks down while it still passes and up
   *  until it does, and installs the shortest kernel found. */
  template <typename Meets>
  size_t searchLength(double estimate, Meets&& meets, size_t stride) {
    const size_t step = 2 * stride;
    auto snap = [&](double t) {
      size_t n = (size_t) (std::max)(3.0, std::ceil(t));
      if (stride == 1) return n | 1;
      return ((n + 1 + 3) / 4) * 4 - 1; // 4m - 1
    };
    size_t taps = snap((std::min)(estimate, (double) kMaxTaps));
    std::vector<double> best, trial;
    size_t found = 0;
    if (meets(taps, trial)) {
      found = taps;
      best = trial;
      while (found > step + 2 && meets(found - step, trial)) {
        found -= step;
        best = trial;
      }
    } else {
      for (size_t t = taps + step; t <= kMaxTaps; t += step)
        if (meets(t, trial)) {
          found = t;
          best = trial;
          break;
        }
    }
    if (!found) return 0;
    install(best);
    return found;
  }

  /** A kernel designed in double, installed as the filter's taps. */
  void install(const std::vector<double>& kernel) {
    coefficients_.resize(kernel.size());
    for (size_t i = 0; i < kernel.size(); ++i) coefficients_[i] = (T) kernel[i];
    history_.assign(kernel.size(), T(0.0));
    position_ = 0;
  }

  static double clampCutoff(T hz, T sampleRate) {
    const double sr = sampleRate > T(1.0) ? (double) sampleRate : 48000.0;
    double normalised = (double) hz / sr;
    if (normalised < 1e-5) normalised = 1e-5;
    if (normalised > 0.4999) normalised = 0.4999;
    return normalised;
  }

  /** Past this a kernel costs more than any audible gain, and a caller that
   *  asked for more has asked for something that will not fit. ODD, like
   *  every kernel designed here: clamping to an even limit took the centre
   *  tap away from a highpass after it had been made odd to have one. */
  static constexpr size_t kMaxTaps = 4095;

  template <typename Ideal>
  void build(size_t taps, FirWindow window, Ideal&& ideal, double kaiserBeta = 8.6) {
    if (taps < 3) taps = 3;
    if (taps > kMaxTaps) taps = kMaxTaps;
    coefficients_.assign(taps, T(0.0));
    double sum = 0.0;
    for (size_t i = 0; i < taps; ++i) {
      const double v = ideal(i, taps) * firdetail::windowValue(window, i, taps, kaiserBeta);
      coefficients_[i] = (T) v;
      sum += v;
    }
    // Normalise to unity DC gain. Every kernel built here is a LOWPASS sinc:
    // the highpass and the bandpass are made from normalised lowpasses after
    // this returns, so nothing that sums to zero by design ever arrives. The
    // guard was 1e-3, which skipped exactly the kernels that need it most --
    // a short lowpass at a very low cutoff sums to almost nothing, and a
    // 3-tap one at 5 Hz passed DC at 0.0002, -74 dB: a lowpass that was mute.
    if (std::fabs(sum) > 1e-12)
      for (T& c : coefficients_) c = (T) ((double) c / sum);

    // The delay line is stored REVERSED relative to the kernel, so the dot
    // product above needs no index arithmetic per tap.
    history_.assign(taps, T(0.0));
    position_ = 0;
  }

  std::vector<T> coefficients_;
  std::vector<T> history_;
  size_t position_ = 0;
};

/** FirFilter in float, the form every plugin written before the templates
 *  uses; FirFilterT<double> is the same processor in double. */
using FirFilter = FirFilterT<float>;

} // namespace sonore
