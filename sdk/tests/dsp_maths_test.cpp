// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: the maths pieces: Thiran fractional delay, Matrix, Polynomial,
// Phase, and the special functions.
//
// Every check is against a number that came from somewhere else: a published
// table, a closed form, an independent quadrature, the exact rational answer.
// "The function returns what the function returns" is not a test.
//
// The audio-thread claims are measured, not asserted in a comment: operator
// new is replaced below, and the forms that say they do not allocate are run
// with the counter armed.
#include <sonore/dsp.h>
#include <sonore/matrix.h>
#include <sonore/phase.h>
#include <sonore/polynomial.h>
#include <sonore/special.h>

#include <atomic>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <vector>

// ── Allocation counter ───────────────────────────────────────────────────────
namespace {
std::atomic<bool> g_armed{false};
std::atomic<long> g_allocs{0};
} // namespace

void* operator new(std::size_t n) {
  if (g_armed.load(std::memory_order_relaxed)) g_allocs.fetch_add(1, std::memory_order_relaxed);
  void* p = std::malloc(n ? n : 1);
  if (!p) throw std::bad_alloc();
  return p;
}
void* operator new[](std::size_t n) {
  if (g_armed.load(std::memory_order_relaxed)) g_allocs.fetch_add(1, std::memory_order_relaxed);
  void* p = std::malloc(n ? n : 1);
  if (!p) throw std::bad_alloc();
  return p;
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const char* what) {
  ++g_checks;
  if (!ok) ++g_failures;
  std::printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
}

struct AllocWatch {
  long start;
  AllocWatch() : start(g_allocs.load()) { g_armed = true; }
  long stop() {
    g_armed = false;
    return g_allocs.load() - start;
  }
};

constexpr double kPiD = 3.14159265358979323846;

// Deterministic noise, so a failure reproduces.
struct Lcg {
  std::uint32_t s = 12345u;
  float next() {
    s = s * 1664525u + 1013904223u;
    return (float) ((double) (s >> 8) / 8388608.0 - 1.0);
  }
};

// ── Thiran ───────────────────────────────────────────────────────────────────

using Line = sonore::DelayLine<4096>;
Line g_line; // 16 KB: a member of something, never a local, as the SDK says

enum class Reader { Linear, Cubic, Thiran };

/** Impulse response of one reader at a fixed delay: read, then write, once per
 *  sample -- the order tap() uses. */
std::vector<double> impulseResponse(Reader reader, float delay, int length) {
  g_line.reset();
  Line::ThiranState head;
  std::vector<double> h((size_t) length);
  for (int n = 0; n < length; ++n) {
    float y = 0.0f;
    switch (reader) {
      case Reader::Linear: y = g_line.read(delay); break;
      case Reader::Cubic: y = g_line.readCubic(delay); break;
      case Reader::Thiran: y = g_line.readThiran(delay, head); break;
    }
    g_line.write(n == 0 ? 1.0f : 0.0f);
    h[(size_t) n] = (double) y;
  }
  return h;
}

/** The Lagrange kernel's four taps placed at a delay, built by feeding it unit
 *  vectors -- the existing interpolator, not a re-derivation of it. */
std::vector<double> lagrangeResponse(float delay, int length) {
  std::vector<double> h((size_t) length, 0.0);
  const int n = (int) std::floor(delay);
  // Points at delays n+2, n+1, n, n-1 (oldest first); frac from the n+1 one.
  const float frac = (float) (n + 1) - delay;
  for (int i = 0; i < 4; ++i) {
    float newestFirst[4] = {0, 0, 0, 0};
    newestFirst[i] = 1.0f;
    const double c = sonore::Lagrange3Kernel::at(newestFirst, frac);
    // newestFirst[0] is delay n-1, [3] is n+2.
    const int at = n - 1 + i;
    if (at >= 0 && at < length) h[(size_t) at] = c;
  }
  return h;
}

std::complex<double> responseAt(const std::vector<double>& h, double w) {
  std::complex<double> sum(0.0, 0.0);
  for (size_t n = 0; n < h.size(); ++n) sum += h[n] * std::polar(1.0, -w * (double) n);
  return sum;
}

double groupDelayAt(const std::vector<double>& h, double w) {
  const double dw = 1e-4;
  const double lo = w > dw ? w - dw : 0.0;
  const std::complex<double> a = responseAt(h, lo), b = responseAt(h, w + dw);
  return -std::arg(b / a) / (w + dw - lo);
}

double dcGroupDelay(const std::vector<double>& h) {
  double m0 = 0.0, m1 = 0.0;
  for (size_t n = 0; n < h.size(); ++n) {
    m0 += h[n];
    m1 += (double) n * h[n];
  }
  return m1 / m0;
}

/** The first-order allpass's group delay, analytically, for the split the
 *  read makes: N + (1 - a^2) / (1 + 2a cos w + a^2). */
double thiranAnalyticDelay(float delay, double w) {
  int n = (int) delay;
  float frac = delay - (float) n;
  if (frac < 0.618034f && (n > 1 || frac == 0.0f)) {
    --n;
    frac += 1.0f;
  }
  const double a = (double) ((1.0f - frac) / (1.0f + frac));
  return (double) n + (1.0 - a * a) / (1.0 + 2.0 * a * std::cos(w) + a * a);
}

void testThiran() {
  std::printf("\n── Thiran fractional delay ──\n");
  const double fs = 48000.0;
  const double w1k = 2.0 * kPiD * 1000.0 / fs;
  const float delays[] = {1.3f, 2.2f, 5.0f, 10.0f, 10.25f, 10.5f, 10.75f, 37.3f, 100.618f, 1000.9f};
  char msg[256];
  for (const float d : delays) {
    const std::vector<double> h = impulseResponse(Reader::Thiran, d, 2048);
    double worstMag = 0.0;
    for (int k = 0; k <= 512; ++k) {
      const double w = kPiD * (double) k / 512.0;
      const double mag = std::abs(responseAt(h, w));
      worstMag = std::fmax(worstMag, std::fabs(mag - 1.0));
    }
    const double tau0 = dcGroupDelay(h);
    const double tau1k = groupDelayAt(h, w1k);
    const double analytic = thiranAnalyticDelay(d, w1k);
    std::snprintf(msg, sizeof msg,
                  "delay %8.3f: DC group delay %.6f, 1 kHz %.6f (analytic %.6f), |H| flat to %.1e",
                  (double) d, tau0, tau1k, analytic, worstMag);
    // At 1 kHz the delay has drifted from d by the allpass's dispersion. The
    // worst the split allows is at d = 1.618 (a = -0.236): 0.0113 samples.
    check(std::fabs(tau0 - (double) d) < 1e-4 && std::fabs(tau1k - analytic) < 1e-5 &&
              std::fabs(tau1k - (double) d) < 0.0115 && worstMag < 1e-6,
          msg);
  }

  // Against the readers that were already here, at the worst fraction for a
  // stateless one: half a sample.
  {
    const float d = 10.5f;
    const double wHi = 2.0 * kPiD * 0.45;      // 21.6 kHz at 48 kHz
    const double w5k = 2.0 * kPiD * 5000.0 / fs;
    struct Row {
      const char* name;
      std::vector<double> h;
    } rows[] = {
        {"linear      read()", impulseResponse(Reader::Linear, d, 256)},
        {"Catmull-Rom readCubic()", impulseResponse(Reader::Cubic, d, 256)},
        {"Lagrange    Lagrange3Kernel", lagrangeResponse(d, 256)},
        {"Thiran      readThiran()", impulseResponse(Reader::Thiran, d, 256)},
    };
    std::printf("       at a delay of 10.5 samples, fs 48 kHz:\n");
    std::printf("       %-28s %12s %12s %12s %12s\n", "reader", "dB @ 8 kHz", "dB @21.6kHz", "delay @ DC",
                "delay @ 5k");
    double magHi[4], mag8k[4];
    for (int i = 0; i < 4; ++i) {
      mag8k[i] = 20.0 * std::log10(std::abs(responseAt(rows[i].h, 2.0 * kPiD * 8000.0 / fs)));
      magHi[i] = 20.0 * std::log10(std::fmax(std::abs(responseAt(rows[i].h, wHi)), 1e-12));
      std::printf("       %-28s %12.4f %12.4f %12.6f %12.6f\n", rows[i].name, mag8k[i], magHi[i],
                  dcGroupDelay(rows[i].h), groupDelayAt(rows[i].h, w5k));
    }
    check(magHi[0] < -15.0 && magHi[1] < -1.0 && magHi[2] < -1.0,
          "the stateless reads all lowpass a half-sample delay at the top of the band");
    check(std::fabs(magHi[3]) < 1e-5 && std::fabs(mag8k[3]) < 1e-5,
          "the Thiran read does not: 0 dB at 8 and 21.6 kHz");
  }

  // Integer delays are exact: the coefficient is 0 and the read is a sample.
  {
    g_line.reset();
    Line::ThiranState a, b;
    Lcg rng;
    std::vector<float> x(400);
    bool exact = true;
    for (int n = 0; n < 400; ++n) {
      const float y5 = g_line.readThiran(5.0f, a);
      const float y1 = g_line.readThiran(1.0f, b);
      if (n >= 5 && y5 != x[(size_t) n - 5]) exact = false;
      if (n >= 1 && y1 != x[(size_t) n - 1]) exact = false;
      x[(size_t) n] = rng.next();
      g_line.write(x[(size_t) n]);
    }
    check(exact, "an integer delay (1 and 5 samples) returns the input bit-exactly");
  }

  // A modulated delay on a low sine, against the exact delayed sine.
  {
    g_line.reset();
    Line::ThiranState head;
    const double w = 2.0 * kPiD * 300.0 / fs;
    double worstThiran = 0.0, worstCubic = 0.0, worstLinear = 0.0;
    for (int n = 0; n < 96000; ++n) {
      const double delay = 100.0 + 40.0 * std::sin(2.0 * kPiD * 0.5 * (double) n / fs);
      const float yT = g_line.readThiran((float) delay, head);
      const float yC = g_line.readCubic((float) delay);
      const float yL = g_line.read((float) delay);
      g_line.write((float) std::sin(w * (double) n));
      if (n > 200) {
        const double want = std::sin(w * ((double) n - (double) (float) delay));
        worstThiran = std::fmax(worstThiran, std::fabs((double) yT - want));
        worstCubic = std::fmax(worstCubic, std::fabs((double) yC - want));
        worstLinear = std::fmax(worstLinear, std::fabs((double) yL - want));
      }
    }
    std::snprintf(msg, sizeof msg,
                  "a 300 Hz sine through a delay swept 60..140 samples: worst error Thiran %.2e "
                  "(cubic %.2e, linear %.2e)",
                  worstThiran, worstCubic, worstLinear);
    check(worstThiran < 1e-4, msg);
  }

  // A fractional read is the same read wherever the write point is. It was
  // w_ - d in one float, whose precision is that of a number the size of the
  // line: in 2^21 samples an eighth to a quarter of a sample. A static
  // 100.3-sample read of a 5 kHz tone came out 0.037 wrong through the cubic
  // tap there, against 0.005 in a short line, and the error moved as the
  // write point went round. The long line runs past a whole lap beside a
  // short one fed the same samples, and the two must read the same bits.
  {
    static sonore::DelayLine<(1 << 21)> longLine; // 8 MB: never a local
    longLine.reset();
    g_line.reset();
    const double w = 2.0 * kPiD * 5000.0 / fs;
    const float delay = 100.3f;
    double worst = 0.0, apart = 0.0;
    for (int n = 0; n < (1 << 21) + 8192; ++n) {
      const float yLong = longLine.readCubic(delay), yLinear = longLine.read(delay);
      const float yShort = g_line.readCubic(delay), yShortLinear = g_line.read(delay);
      const float x = (float) std::sin(w * (double) n);
      longLine.write(x);
      g_line.write(x);
      if (n > 200) {
        worst = std::fmax(worst, std::fabs((double) yLong - std::sin(w * ((double) n - (double) delay))));
        apart = std::fmax(apart, std::fabs((double) yLong - (double) yShort));
        apart = std::fmax(apart, std::fabs((double) yLinear - (double) yShortLinear));
      }
    }
    std::snprintf(msg, sizeof msg,
                  "a 100.3-sample read of 5 kHz through a 2^21-sample line, over a whole lap: worst "
                  "cubic error %.2e, %.2e from a 4096-sample line's",
                  worst, apart);
    check(worst < 0.01 && apart == 0.0, msg);
  }

  // A jump: the state is reseeded at the new delay rather than carried over.
  {
    g_line.reset();
    Line::ThiranState jumping, settled, stale;
    const double w = 2.0 * kPiD * 440.0 / fs;
    for (int n = 0; n < 3000; ++n) {
      g_line.readThiran(10.3f, jumping);
      g_line.readThiran(10.3f, stale);
      g_line.readThiran(500.7f, settled);
      g_line.write((float) std::sin(w * (double) n));
    }
    // `stale` is told it was already near the new delay, so it keeps the
    // output from 10.3 -- what a read that never reseeds would do.
    stale.lastDelay = 500.2f;
    double errJump = 0.0, errStale = 0.0;
    for (int n = 3000; n < 3004; ++n) {
      const float a = g_line.readThiran(500.7f, jumping);
      const float b = g_line.readThiran(500.7f, stale);
      const float ref = g_line.readThiran(500.7f, settled);
      errJump = std::fmax(errJump, std::fabs((double) a - (double) ref));
      errStale = std::fmax(errStale, std::fabs((double) b - (double) ref));
      g_line.write((float) std::sin(w * (double) n));
    }
    std::snprintf(msg, sizeof msg,
                  "a jump 10.3 -> 500.7 samples: first outputs within %.1e of a settled head "
                  "(%.1e if the old state were kept)",
                  errJump, errStale);
    check(errJump < 1e-4 && errStale > 10.0 * errJump, msg);
  }

  // No NaN, no blow-up: a delay swept across the whole range, through
  // integers and the bottom of the range, with jumps, on noise.
  {
    g_line.reset();
    Line::ThiranState head;
    Lcg rng;
    bool finite = true;
    double peak = 0.0;
    float jump = 0.0f;
    for (int n = 0; n < 400000; ++n) {
      if (n % 997 == 0) jump = (rng.next() + 1.0f) * 1500.0f;
      double delay = 1.0 + 2000.0 * (0.5 + 0.5 * std::sin(2.0 * kPiD * 3.1 * (double) n / fs));
      if ((n / 997) % 3 == 1) delay = jump;
      if ((n / 5000) % 7 == 3) delay = 1.0 + std::fabs(std::sin((double) n * 0.001)) * 1.5; // 1..2.5
      const float y = g_line.readThiran((float) delay, head);
      if (!std::isfinite(y)) finite = false;
      peak = std::fmax(peak, std::fabs((double) y));
      g_line.write(rng.next());
    }
    // Hostile delays as well: NaN, negative, past the end.
    const float hostile[] = {std::nanf(""), -5.0f, 1e9f, 0.0f, 4093.5f};
    for (const float d : hostile) {
      const float y = g_line.readThiran(d, head);
      if (!std::isfinite(y)) finite = false;
      g_line.write(rng.next());
    }
    std::snprintf(msg, sizeof msg,
                  "400k samples of swept, jumping and hostile delays on noise: all finite, peak %.3f", peak);
    check(finite && peak < 8.0, msg);
  }

  // reset() clears the state with the buffer; a reset head equals a new one.
  {
    g_line.reset();
    Lcg rng;
    for (int n = 0; n < 1000; ++n) {
      g_line.readThiran(33.4f);
      g_line.write(rng.next());
    }
    g_line.reset();
    bool silent = true;
    for (int n = 0; n < 64; ++n)
      if (g_line.readThiran(33.4f) != 0.0f) silent = false;
    check(silent, "after reset() the line's own Thiran head reads exact silence");

    Line::ThiranState used, fresh;
    for (int n = 0; n < 500; ++n) {
      g_line.readThiran(7.9f, used);
      g_line.write(rng.next());
    }
    used.reset();
    bool same = true;
    for (int n = 0; n < 200; ++n) {
      if (g_line.readThiran(7.9f, used) != g_line.readThiran(7.9f, fresh)) same = false;
      g_line.write(rng.next());
    }
    check(same, "a ThiranState after reset() behaves exactly like a new one");
  }

  // The audio-thread claim.
  {
    Line::ThiranState head;
    Lcg rng;
    AllocWatch watch;
    float sink = 0.0f;
    for (int n = 0; n < 100000; ++n) {
      sink += g_line.readThiran(20.0f + 10.0f * (float) std::sin((double) n * 1e-3), head);
      g_line.write(rng.next());
    }
    const long allocs = watch.stop();
    std::snprintf(msg, sizeof msg, "100k modulated Thiran reads: %ld allocations (sum %.3f)", allocs,
                  (double) sink);
    check(allocs == 0, msg);
  }
}

// ── Matrix ───────────────────────────────────────────────────────────────────

using M = sonore::Matrix<double>;

void testMatrix() {
  std::printf("\n── Matrix ──\n");
  {
    // The textbook elimination example: x = 2, y = 3, z = -1.
    const double a[] = {2, 1, -1, -3, -1, 2, -2, 1, 2};
    const double b[] = {8, -11, -3};
    M A(3, 3, a), B(3, 1, b);
    const bool ok = A.solve(B);
    check(ok && std::fabs(B(0, 0) - 2) < 1e-14 && std::fabs(B(1, 0) - 3) < 1e-14 &&
              std::fabs(B(2, 0) + 1) < 1e-14,
          "solve: the 3x3 textbook system gives (2, 3, -1)");
  }
  {
    // A zero on the diagonal: only pivoting can do this one.
    const double a[] = {0, 1, 1, 1};
    const double b[] = {1, 2};
    M A(2, 2, a), B(2, 1, b);
    check(A.solve(B) && std::fabs(B(0, 0) - 1) < 1e-15 && std::fabs(B(1, 0) - 1) < 1e-15,
          "solve: [[0 1][1 1]] x = (1, 2) needs a row swap, and gets (1, 1)");
  }
  {
    // A tiny pivot that is not zero: without pivoting, 1e-20 is divided by and
    // the answer is garbage.
    const double a[] = {1e-20, 1, 1, 1};
    const double b[] = {1, 2};
    M A(2, 2, a), B(2, 1, b);
    check(A.solve(B) && std::fabs(B(0, 0) - 1) < 1e-14 && std::fabs(B(1, 0) - 1) < 1e-14,
          "solve: a 1e-20 pivot is swapped away, and the answer is still (1, 1)");
  }
  {
    // Hilbert matrices: their inverses and determinants are known exactly.
    M H3(3, 3), H4(4, 4);
    for (size_t i = 0; i < 4; ++i)
      for (size_t j = 0; j < 4; ++j) {
        if (i < 3 && j < 3) H3(i, j) = 1.0 / (double) (i + j + 1);
        H4(i, j) = 1.0 / (double) (i + j + 1);
      }
    const double inv3[] = {9, -36, 30, -36, 192, -180, 30, -180, 180};
    M I3;
    check(H3.inverse(I3) && M::compare(I3, M(3, 3, inv3), 1e-10),
          "inverse: Hilbert(3)^-1 is [[9 -36 30][-36 192 -180][30 -180 180]]");
    check(std::fabs(H3.determinant() * 2160.0 - 1.0) < 1e-12, "determinant: Hilbert(3) is 1/2160");
    check(std::fabs(H4.determinant() * 6048000.0 - 1.0) < 1e-10, "determinant: Hilbert(4) is 1/6048000");
  }
  {
    const double a[] = {1, 2, 3, 4};
    check(M(2, 2, a).determinant() == -2.0, "determinant: [[1 2][3 4]] is -2");
    const double p[] = {0, 1, 0, 1, 0, 0, 0, 0, 1};
    check(M(3, 3, p).determinant() == -1.0, "determinant: a single row swap of I is -1");
    const double s[] = {1, 2, 3, 2, 4, 6, 1, 0, 1};
    M S(3, 3, s), B(3, 1), inv;
    check(S.determinant() == 0.0 && !S.solve(B) && !S.inverse(inv),
          "a singular matrix: determinant 0, solve and inverse refuse");
    const double u[] = {2, 7, 1, 0, 3, 5, 0, 0, 4};
    check(M(3, 3, u).determinant() == 24.0, "determinant: triangular is the product of the diagonal");
  }
  {
    // A * inv(A) = I, for random well-scaled matrices of several sizes.
    Lcg rng;
    double worst = 0.0;
    bool all = true;
    for (size_t n = 1; n <= 12; ++n) {
      for (int trial = 0; trial < 5; ++trial) {
        M A(n, n);
        for (double& v : A) v = (double) rng.next();
        for (size_t i = 0; i < n; ++i) A(i, i) += 2.0; // keep it comfortably invertible
        M Ai;
        if (!A.inverse(Ai)) {
          all = false;
          continue;
        }
        const M left = A * Ai;
        const M right = Ai * A;
        const M I = M::identity(n);
        for (size_t i = 0; i < n * n; ++i) {
          worst = std::fmax(worst, std::fabs(left.getRawDataPointer()[i] - I.getRawDataPointer()[i]));
          worst = std::fmax(worst, std::fabs(right.getRawDataPointer()[i] - I.getRawDataPointer()[i]));
        }
      }
    }
    char msg[160];
    std::snprintf(msg, sizeof msg, "A * inv(A) = inv(A) * A = I for 60 random matrices up to 12x12 (worst %.1e)",
                  worst);
    check(all && worst < 1e-13, msg);
  }
  {
    // Several right-hand sides at once, against the product they came from.
    Lcg rng;
    M A(6, 6), X(6, 3);
    for (double& v : A) v = (double) rng.next();
    for (size_t i = 0; i < 6; ++i) A(i, i) += 3.0;
    for (double& v : X) v = (double) rng.next();
    M B = A * X;
    check(A.solve(B) && M::compare(B, X, 1e-13), "solve: three right-hand sides at once recover X from A X");
  }
  {
    const double a[] = {1, 2, 3, 4, 5, 6};        // 2x3
    const double b[] = {7, 8, 9, 10, 11, 12};     // 3x2
    const double ab[] = {58, 64, 139, 154};
    M A(2, 3, a), B(3, 2, b);
    check(M::compare(A * B, M(2, 2, ab)), "multiply: (2x3)(3x2) by hand");
    const double at[] = {1, 4, 2, 5, 3, 6};
    check(M::compare(A.transposed(), M(3, 2, at)), "transpose: rows become columns");
    check(M::compare((A * B).transposed(), B.transposed() * A.transposed()), "(AB)^T = B^T A^T");
    check((A * A).isNullMatrix() && (A + B).isNullMatrix(),
          "mismatched shapes give a 0x0 result, not a read past the end");
    M C(A);
    C += A;
    C -= A * 3.0;
    check(M::compare(C, A * -1.0), "A + A - 3A = -A");
    M D(A);
    D.hadamard(A);
    check(D(1, 2) == 36.0 && D(0, 1) == 4.0, "hadamard: element by element");
    check(A.getRow(1)[2] == 6.0 && A.getColumn(1)(1, 0) == 5.0 && A.getColumn(1).isOneColumnVector(),
          "row and column access");
    M E(A);
    E.swapRows(0, 1).swapColumns(0, 2);
    check(E(0, 0) == 6.0 && E(1, 2) == 1.0, "swapRows / swapColumns");
    check(A.toString() == "[[1, 2, 3], [4, 5, 6]]", "toString");
  }
  {
    const double v[] = {4, 3, 2, 1, 0, -1, -2};
    M V(7, 1, v);
    const M T = M::toeplitz(V, 4);
    bool toe = true;
    for (size_t i = 0; i < 4; ++i)
      for (size_t j = 0; j < 4; ++j)
        if (T(i, j) != v[i > j ? i - j : j - i]) toe = false;
    check(toe && M::compare(T, T.transposed()), "toeplitz: (i, j) = v[|i - j|], symmetric");
    const M H = M::hankel(V, 3, 1);
    bool han = true;
    for (size_t i = 0; i < 3; ++i)
      for (size_t j = 0; j < 3; ++j)
        if (H(i, j) != v[i + j + 1]) han = false;
    check(han, "hankel: (i, j) = v[i + j + offset]");

    // Linear prediction, the reason a Toeplitz matrix is here: the
    // autocorrelation of an AR(2) process gives back its coefficients.
    // x[n] = 1.2 x[n-1] - 0.5 x[n-2] + e[n]; r[k] from the Yule-Walker
    // recursion, r[0] = 1.
    const double a1 = 1.2, a2 = -0.5;
    const double r1 = a1 / (1.0 - a2);
    const double r2 = a1 * r1 + a2;
    const double r[] = {1.0, r1};
    const double rhs[] = {r1, r2};
    M R = M::toeplitz(M(2, 1, r), 2), B(2, 1, rhs);
    check(R.solve(B) && std::fabs(B(0, 0) - a1) < 1e-14 && std::fabs(B(1, 0) - a2) < 1e-14,
          "toeplitz + solve: Yule-Walker recovers an AR(2) model's coefficients");
  }
  {
    // The audio-thread forms, sized up front: no allocation.
    Lcg rng;
    M A(8, 8), Awork(8, 8), x(8, 1), b(8, 1), y(8, 1);
    for (double& v : A) v = (double) rng.next();
    for (size_t i = 0; i < 8; ++i) A(i, i) += 3.0;
    for (double& v : x) v = (double) rng.next();
    AllocWatch watch;
    bool ok = true;
    for (int block = 0; block < 1000; ++block) {
      ok = M::multiply(A, x, b) && ok;      // b = A x
      for (size_t i = 0; i < 64; ++i) Awork.getRawDataPointer()[i] = A.getRawDataPointer()[i];
      for (size_t i = 0; i < 8; ++i) y(i, 0) = b(i, 0);
      ok = Awork.solveInPlace(y) && ok;     // y = A^-1 b = x
      y -= x;
      y *= 2.0;
      y += x;
    }
    const long allocs = watch.stop();
    char msg[160];
    std::snprintf(msg, sizeof msg,
                  "1000 blocks of multiply + solveInPlace + compound ops: %ld allocations", allocs);
    check(ok && allocs == 0, msg);
  }
  {
    sonore::Matrix<float> A(2, 2), B(2, 1);
    A(0, 0) = 4.0f;
    A(0, 1) = 1.0f;
    A(1, 0) = 2.0f;
    A(1, 1) = 3.0f;
    B(0, 0) = 1.0f;
    B(1, 0) = 2.0f;
    // 4x + y = 1, 2x + 3y = 2 -> x = 0.1, y = 0.6
    check(A.solve(B) && std::fabs(B(0, 0) - 0.1f) < 1e-6f && std::fabs(B(1, 0) - 0.6f) < 1e-6f,
          "Matrix<float> solves too");
  }
}

// ── Polynomial ───────────────────────────────────────────────────────────────

using P = sonore::Polynomial<double>;

bool sameCoefficients(const P& p, std::initializer_list<double> want, double tol = 0.0) {
  if (p.getOrder() + 1 != (int) want.size()) return false;
  int i = 0;
  for (const double w : want)
    if (std::fabs(p[i++] - w) > tol) return false;
  return true;
}

void testPolynomial() {
  std::printf("\n── Polynomial ──\n");
  {
    // (x - 1)(x - 2)(x - 3) = x^3 - 6x^2 + 11x - 6
    const P p = P{-1, 1}.getProductWith(P{-2, 1}).getProductWith(P{-3, 1});
    check(sameCoefficients(p, {-6, 11, -6, 1}), "(x-1)(x-2)(x-3) multiplies out to x^3 - 6x^2 + 11x - 6");
    check(p(1.0) == 0.0 && p(2.0) == 0.0 && p(3.0) == 0.0 && p(4.0) == 6.0, "and is zero at its roots");
  }
  {
    // (1 + x)^10: the binomial coefficients.
    P p{1};
    for (int i = 0; i < 10; ++i) p = p.getProductWith(P{1, 1});
    check(sameCoefficients(p, {1, 10, 45, 120, 210, 252, 210, 120, 45, 10, 1}),
          "(1 + x)^10 gives row 10 of Pascal's triangle");
    check(p(1.0) == 1024.0, "and (1 + 1)^10 = 1024");
  }
  {
    const P a{1, 2, 3}, b{4, 5};
    check(sameCoefficients(a.getSumWith(b), {5, 7, 3}) && sameCoefficients(b.getSumWith(a), {5, 7, 3}),
          "sum: different lengths, either way round");
    check(sameCoefficients(a.withGain(-2.0), {-2, -4, -6}), "withGain scales every coefficient");
    check(sameCoefficients(a.getDerivative(), {2, 6}) && sameCoefficients(P{7}.getDerivative(), {0}),
          "derivative: 1 + 2x + 3x^2 -> 2 + 6x; a constant's is zero");
    check(sameCoefficients(a.getIntegral(5.0), {5, 1, 1, 1}), "integral: 5 + x + x^2 + x^3");
    check(sameCoefficients(a.getIntegral().getDerivative(), {1, 2, 3}, 1e-15),
          "the derivative of the integral is the polynomial");
    check(a[7] == 0.0 && a.getOrder() == 2 && P().getOrder() == 0 && P()(3.0) == 0.0,
          "coefficients past the order read as zero; the default is the zero polynomial");
    check(a.toString() == "1 + 2x + 3x^2", "toString");
  }
  {
    // Horner against the sum written out, over a range of x.
    const P p{0.5, -1.25, 3.0, -0.75, 0.125, 2.0};
    double worst = 0.0;
    for (int i = -200; i <= 200; ++i) {
      const double x = (double) i * 0.01;
      double direct = 0.0;
      for (int k = 0; k <= p.getOrder(); ++k) direct += p[k] * std::pow(x, (double) k);
      worst = std::fmax(worst, std::fabs(p(x) - direct) / std::fmax(1.0, std::fabs(direct)));
    }
    char msg[128];
    std::snprintf(msg, sizeof msg, "Horner evaluation matches the direct sum over [-2, 2] (worst %.1e)", worst);
    check(worst < 1e-14, msg);
  }
  {
    // A product evaluates to the product of the values.
    const P a{0.3, -1.1, 0.7, 2.5}, b{-0.4, 0.9, 1.6};
    const P ab = a.getProductWith(b);
    // Relative to the size of the terms being summed -- the evaluation with
    // every coefficient made positive at |x| -- which is what Horner's
    // rounding error scales with; relative to the result it would blow up
    // wherever the result passes through zero.
    P absAb = ab;
    for (int k = 0; k <= absAb.getOrder(); ++k) absAb[k] = std::fabs(absAb[k]);
    double worst = 0.0;
    for (int i = -50; i <= 50; ++i) {
      const double x = (double) i * 0.037;
      worst = std::fmax(worst, std::fabs(ab(x) - a(x) * b(x)) / absAb(std::fabs(x)));
    }
    char msg[128];
    std::snprintf(msg, sizeof msg, "(a * b)(x) = a(x) b(x) at 101 points (worst relative %.1e)", worst);
    check(ab.getOrder() == 5 && worst < 1e-14, msg);
  }
  {
    // Chebyshev polynomials by their recurrence, T(n+1) = 2x T(n) - T(n-1):
    // T_n(cos t) = cos(n t) is the identity a harmonic shaper rests on.
    P t0{1}, t1{0, 1};
    for (int n = 2; n <= 7; ++n) {
      const P next = P{0, 2}.getProductWith(t1).getSumWith(t0.withGain(-1.0));
      t0 = t1;
      t1 = next;
    }
    double worst = 0.0;
    for (int i = 0; i <= 100; ++i) {
      const double t = kPiD * (double) i / 100.0;
      worst = std::fmax(worst, std::fabs(t1(std::cos(t)) - std::cos(7.0 * t)));
    }
    check(sameCoefficients(t1, {0, -7, 0, 56, 0, -112, 0, 64}) && worst < 1e-12,
          "T7 built by the recurrence is 64x^7 - 112x^5 + 56x^3 - 7x, and T7(cos t) = cos 7t");
  }
  {
    const P p{1, -2, 3, -4, 5};
    AllocWatch watch;
    double sink = 0.0;
    for (int i = 0; i < 100000; ++i) sink += p((double) i * 1e-5);
    const long allocs = watch.stop();
    char msg[128];
    std::snprintf(msg, sizeof msg, "100k evaluations: %ld allocations (sum %.3f)", allocs, sink);
    check(allocs == 0, msg);
  }
}

// ── Phase ────────────────────────────────────────────────────────────────────

void testPhase() {
  std::printf("\n── Phase ──\n");
  {
    // 440 Hz at 48 kHz is 11/1200 of a turn per sample, so the exact phase
    // after n samples is (11 n mod 1200) / 1200 -- integer arithmetic, no
    // rounding anywhere. Held against it for 10^8 samples.
    sonore::Phase phase;
    phase.setFrequency(440.0, 48000.0);
    float naiveF = 0.0f;
    double naiveD = 0.0;
    const float incF = (float) (440.0 / 48000.0);
    const double incD = 440.0 / 48000.0;
    std::uint32_t exactNum = 0;
    double worst = 0.0, driftF = 0.0, driftD = 0.0;
    bool inRange = true;
    const auto circular = [](double a, double b) {
      double e = std::fabs(a - b);
      return e > 0.5 ? 1.0 - e : e;
    };
    for (long n = 0; n < 100000000L; ++n) {
      phase.advance();
      naiveF += incF;
      if (naiveF >= 1.0f) naiveF -= 1.0f;
      naiveD += incD;
      if (naiveD >= 1.0) naiveD -= 1.0;
      exactNum += 11;
      if (exactNum >= 1200) exactNum -= 1200;
      if ((n & 1023) == 1023 || n == 99999999L) {
        const double exact = (double) exactNum / 1200.0;
        const double t = phase.turns();
        if (!(t >= 0.0 && t < 1.0)) inRange = false;
        worst = std::fmax(worst, circular(t, exact));
        driftF = std::fmax(driftF, circular((double) naiveF, exact));
        driftD = std::fmax(driftD, circular(naiveD, exact));
      }
    }
    char msg[256];
    std::snprintf(msg, sizeof msg,
                  "10^8 samples of 440 Hz at 48 kHz: Phase within %.1e of a cycle of the exact answer",
                  worst);
    check(worst < 1e-9 && inRange, msg);
    std::snprintf(msg, sizeof msg,
                  "  (the same accumulator in float drifted %.3f of a cycle, in double %.1e)", driftF, driftD);
    check(driftF > 1e-3, msg);
  }
  {
    // A binary increment is exact: back to zero, not near it.
    sonore::Phase phase;
    phase.setIncrement(0.125);
    for (int i = 0; i < 8000000; ++i) phase.advance();
    check(phase.turns() == 0.0, "an increment of 1/8, 8e6 times: exactly zero, not nearly");
  }
  {
    sonore::Phase phase;
    phase.setIncrement(0.3);
    const double before = phase.advance();
    bool wrapsRight = before == 0.0 && !phase.wrapped();
    phase.advance(); // 0.6
    phase.advance(); // 0.9
    wrapsRight = wrapsRight && !phase.wrapped();
    const double atNine = phase.advance(); // returns 0.9, lands on 0.2
    wrapsRight = wrapsRight && phase.wrapped() && std::fabs(atNine - 0.9) < 1e-15 &&
                 std::fabs(phase.turns() - 0.2) < 1e-15;
    check(wrapsRight, "advance() returns the phase before the step, and wrapped() marks the crossing");

    sonore::Phase back;
    back.setIncrement(-0.25);
    back.advance();
    const bool wrappedBack = back.wrapped() && std::fabs(back.turns() - 0.75) < 1e-15;
    back.advance();
    back.advance();
    back.advance();
    check(wrappedBack && back.turns() == 0.0 && !back.wrapped(),
          "a negative increment runs backwards: 0 -> 0.75 (wrapped) ... -> 0 exactly");

    sonore::Phase big;
    big.setIncrement(2.75);
    big.advance();
    check(std::fabs(big.turns() - 0.75) < 1e-15 && std::fabs(big.getIncrement() - 0.75) < 1e-15,
          "an increment of 2.75 turns is 0.75");
  }
  {
    // The top of the range must not round up onto 1.
    sonore::Phase phase;
    phase.setTurns(0.99999999999999989); // the largest double below 1
    check(phase.turns() < 1.0 && phase.turnsFloat() < 1.0f && phase.radians() < 2.0 * kPiD &&
              phase.turns() > 0.9999999,
          "the largest phase below one turn reads below 1 (double and float) and below 2pi");
    phase.setTurns(-0.25);
    check(std::fabs(phase.turns() - 0.75) < 1e-15 &&
              std::fabs(phase.radians() - 1.5 * kPiD) < 1e-14,
          "setTurns(-0.25) is 0.75 of a turn, 3pi/2 radians");
    phase.setTurns(std::nan(""));
    check(phase.turns() == 0.0, "a NaN phase is refused, not stored");
    phase.reset();
    check(phase.turns() == 0.0 && phase.radians() == 0.0, "reset() returns to zero");
  }
  {
    sonore::Phase phase;
    phase.setFrequency(1234.5, 44100.0);
    AllocWatch watch;
    double sink = 0.0;
    for (int i = 0; i < 1000000; ++i) sink += phase.advance();
    const long allocs = watch.stop();
    char msg[128];
    std::snprintf(msg, sizeof msg, "10^6 advances: %ld allocations (sum %.1f)", allocs, sink);
    check(allocs == 0, msg);
  }
}

// ── Special functions ────────────────────────────────────────────────────────

/** I0 by its integral, (1/pi) * integral over [0, pi] of exp(x cos t): the
 *  integrand is periodic and analytic, so the trapezoid rule converges
 *  geometrically -- an independent route to the same number. */
double besselI0ByIntegral(double x) {
  const int n = 4096;
  double sum = 0.5 * (std::exp(x) + std::exp(-x));
  for (int i = 1; i < n; ++i) sum += std::exp(x * std::cos(kPiD * (double) i / (double) n));
  return sum / (double) n;
}

/** K(k) by the trapezoid rule on its (periodic, analytic) integrand. */
double ellipticKByIntegral(double k) {
  const int n = 4000;
  const auto f = [k](double t) { return 1.0 / std::sqrt(1.0 - k * k * std::sin(t) * std::sin(t)); };
  double sum = 0.5 * (f(0.0) + f(0.5 * kPiD));
  for (int i = 1; i < n; ++i) sum += f(0.5 * kPiD * (double) i / (double) n);
  return sum * 0.5 * kPiD / (double) n;
}

/** F(phi, k), the incomplete integral, by composite Simpson. */
double incompleteF(double phi, double k) {
  const int n = 20000;
  const auto f = [k](double t) { return 1.0 / std::sqrt(1.0 - k * k * std::sin(t) * std::sin(t)); };
  const double h = phi / (double) n;
  double sum = f(0.0) + f(phi);
  for (int i = 1; i < n; ++i) sum += f(h * (double) i) * ((i & 1) ? 4.0 : 2.0);
  return sum * h / 3.0;
}

void testSpecial() {
  std::printf("\n── Special functions ──\n");
  using sonore::special::besselI0;
  {
    // Published values (Abramowitz & Stegun table 9.8, to the digits given
    // there and by every library since).
    struct {
      double x, value;
    } table[] = {{0.0, 1.0},
                 {1.0, 1.2660658777520082},
                 {2.0, 2.2795853023360673},
                 {5.0, 27.239871823604442},
                 {10.0, 2815.7166284662544}};
    bool ok = true;
    for (const auto& t : table)
      if (std::fabs(besselI0(t.x) / t.value - 1.0) > 1e-14) ok = false;
    check(ok, "I0 at 0, 1, 2, 5, 10 matches the published table to 1e-14");
    double worst = 0.0;
    for (const double x : {0.5, 3.0, 8.6, 20.0, 50.0, 100.0, 300.0}) {
      worst = std::fmax(worst, std::fabs(besselI0(x) / besselI0ByIntegral(x) - 1.0));
    }
    char msg[160];
    std::snprintf(msg, sizeof msg,
                  "I0 at 0.5 .. 300 matches its integral representation (worst relative %.1e)", worst);
    check(worst < 1e-13, msg);
    check(sonore::SpecialFunctions::besselI0(3.7) == besselI0(3.7), "SpecialFunctions::besselI0 forwards");
  }
  {
    using sonore::special::ellipticK;
    // Published K(m), m = k^2.
    struct {
      double m, value;
    } table[] = {{0.0, 1.5707963267948966},
                 {0.1, 1.6124413487202192},
                 {0.25, 1.6857503548125961},
                 {0.5, 1.8540746773013719},
                 {0.9, 2.5780921133481733},
                 {0.99, 3.6956373629898747}};
    bool ok = true;
    for (const auto& t : table)
      if (std::fabs(ellipticK(std::sqrt(t.m)) / t.value - 1.0) > 2e-15) ok = false;
    check(ok, "K(k) matches the published K(m) at m = 0, 0.1, 0.25, 0.5, 0.9, 0.99");
    double worst = 0.0;
    for (const double k : {0.05, 0.3, 0.6, 0.85, 0.95})
      worst = std::fmax(worst, std::fabs(ellipticK(k) / ellipticKByIntegral(k) - 1.0));
    char msg[160];
    std::snprintf(msg, sizeof msg, "K(k) matches quadrature of its integral (worst relative %.1e)", worst);
    check(worst < 1e-13, msg);
    double K = 0, Kp = 0;
    sonore::SpecialFunctions::ellipticIntegralK(std::sqrt(0.5), K, Kp);
    check(std::fabs(K - Kp) < 1e-15 && std::fabs(K - 1.8540746773013719) < 1e-15,
          "ellipticIntegralK(1/sqrt 2): K = K' = 1.85407..., the self-complementary modulus");
    check(std::isinf(ellipticK(1.0)) && std::isnan(ellipticK(-0.1)) && std::isnan(ellipticK(1.5)),
          "K(1) is infinite; a modulus outside [0, 1] is NaN");
  }
  {
    using sonore::special::asne;
    using sonore::special::cde;
    using sonore::special::sne;
    using C = std::complex<double>;
    // sn(F(phi), k) = sin(phi): sn is the inverse of the incomplete integral.
    double worst = 0.0;
    for (const double k : {0.3, 0.8, 0.99})
      for (const double phi : {0.3, 0.9, 1.4}) {
        const double u = incompleteF(phi, k) / sonore::special::ellipticK(k);
        worst = std::fmax(worst, std::abs(sne(C(u, 0.0), k) - C(std::sin(phi), 0.0)));
      }
    char msg[200];
    std::snprintf(msg, sizeof msg,
                  "sne inverts the incomplete integral: sn(F(phi, k)) = sin phi (worst %.1e)", worst);
    check(worst < 1e-12, msg);

    bool landmarks = true;
    for (const double k : {0.0, 0.2, 0.7, 0.999}) {
      const double kp = std::sqrt(1.0 - k * k);
      landmarks = landmarks && std::abs(cde(C(0, 0), k) - C(1, 0)) < 1e-15 &&
                  std::abs(cde(C(1, 0), k)) < 1e-14 && std::abs(sne(C(1, 0), k) - C(1, 0)) < 1e-14 &&
                  std::abs(sne(C(0, 0), k)) < 1e-15 &&
                  std::abs(sne(C(0.5, 0), k) - C(1.0 / std::sqrt(1.0 + kp), 0)) < 1e-14 &&
                  std::abs(cde(C(0.5, 0), k) - C(1.0 / std::sqrt(1.0 + kp), 0)) < 1e-14;
    }
    check(landmarks, "cd(0) = 1, cd(K) = 0, sn(K) = 1, sn(K/2) = cd(K/2) = 1/sqrt(1 + k')");

    bool limits = true;
    for (const double u : {0.1, 0.4, 0.77}) {
      limits = limits && std::abs(sne(C(u, 0), 0.0) - C(std::sin(u * kPiD / 2), 0)) < 1e-15 &&
               std::abs(cde(C(u, 0), 0.0) - C(std::cos(u * kPiD / 2), 0)) < 1e-15;
      const double k = 0.999999, x = u * sonore::special::ellipticK(k);
      limits = limits && std::abs(sne(C(u, 0), k).real() - std::tanh(x)) < 1e-5;
    }
    check(limits, "k = 0 gives sin and cos; k -> 1 approaches tanh");

    // Jacobi's imaginary transformation: sn(i v, k) = i sn(v, k') / cn(v, k').
    bool imaginary = true;
    {
      const double k = 0.6, kp = 0.8;
      double K = 0, Kp = 0;
      sonore::special::ellipticIntegralK(k, K, Kp);
      for (const double t : {0.2, 0.5, 0.9}) {
        const C left = sne(C(0.0, t * Kp / K), k);
        const double s = sne(C(t, 0), kp).real();
        const C right(0.0, s / std::sqrt(1.0 - s * s));
        imaginary = imaginary && std::abs(left - right) < 1e-13 * std::fmax(1.0, std::abs(right));
      }
    }
    check(imaginary, "sn(iv, k) = i sn(v, k')/cn(v, k'): the imaginary transformation holds");

    // Periods: sn(u + 2) = -sn(u) along the real axis in quarter-period
    // units, and sn(u + 2i K'/K) = sn(u) along the imaginary one.
    bool periodic = true;
    {
      const double k = 0.45;
      double K = 0, Kp = 0;
      sonore::special::ellipticIntegralK(k, K, Kp);
      const C u(0.37, 0.21);
      periodic = std::abs(sne(u + C(2, 0), k) + sne(u, k)) < 1e-13 &&
                 std::abs(sne(u + C(0, 2.0 * Kp / K), k) - sne(u, k)) < 1e-12 &&
                 std::abs(cde(u, k) - sne(u + C(1, 0), k)) < 1e-13;
    }
    check(periodic, "sn has period 4K and 2iK'; cd(u) = sn(u + K)");

    // asne undoes sne, on the real interval, in the stopband, and off-axis.
    double worstRound = 0.0;
    bool stopband = true;
    for (const double k : {0.1, 0.5, 0.9}) {
      double K = 0, Kp = 0;
      sonore::special::ellipticIntegralK(k, K, Kp);
      for (const double w : {-0.95, -0.3, 0.0, 0.5, 0.999}) {
        const C u = asne(C(w, 0), k);
        worstRound = std::fmax(worstRound, std::abs(sne(u, k) - C(w, 0)));
        stopband = stopband && std::fabs(u.imag()) < 1e-12 && std::fabs(u.real()) <= 1.0 + 1e-12;
      }
      for (const double frac : {0.2, 0.6, 0.95}) {
        const double w = 1.0 + frac * (1.0 / k - 1.0); // in (1, 1/k]
        const C u = asne(C(w, 0), k);
        worstRound = std::fmax(worstRound, std::abs(sne(u, k) - C(w, 0)) / w);
        stopband = stopband && std::fabs(u.real() - 1.0) < 1e-10 && std::fabs(u.imag()) <= Kp / K + 1e-12;
      }
      for (const C w : {C(0.3, 0.4), C(-1.2, 0.7), C(2.0, -1.5)}) {
        worstRound = std::fmax(worstRound, std::abs(sne(asne(w, k), k) - w) / std::abs(w));
      }
    }
    std::snprintf(msg, sizeof msg, "sne(asne(w)) = w, real, stopband and complex w (worst relative %.1e)",
                  worstRound);
    check(worstRound < 1e-12, msg);
    check(stopband, "asne: real w in [-1, 1] -> real u; w in (1, 1/k] -> u = 1 + iv, 0 <= v <= K'/K");

    check(std::isnan(sne(C(0.3, 0), 1.0).real()) && std::isnan(cde(C(0.3, 0), -0.2).real()) &&
              std::isnan(asne(C(0.3, 0), 1.0).real()),
          "a modulus outside [0, 1) is NaN, not a number from a sequence that never converged");
    const C probe(0.31, -0.12);
    check(sonore::SpecialFunctions::sne(probe, 0.4) == sne(probe, 0.4) &&
              sonore::SpecialFunctions::cde(probe, 0.4) == cde(probe, 0.4) &&
              sonore::SpecialFunctions::asne(probe, 0.4) == asne(probe, 0.4),
          "SpecialFunctions forwards to sonore::special");
  }
}

} // namespace

int main() {
  std::printf("dsp maths: Thiran, Matrix, Polynomial, Phase, special functions\n");
  testThiran();
  testMatrix();
  testPolynomial();
  testPhase();
  testSpecial();
  std::printf("\n%d checks, %d failed\n", g_checks, g_failures);
  std::printf(g_failures == 0 ? "DSP MATHS TESTS PASSED\n" : "DSP MATHS TESTS FAILED\n");
  return g_failures == 0 ? 0 : 1;
}
