// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: the FFT -- Fft<Size>, DynamicFft, and the real-signal
// transforms of both.
//
// Every spectrum is checked against the definition: a DFT summed directly
// in double, which shares no code with any transform here. The runtime-sized
// transform must give the compile-time one's bits exactly (it is the same
// kernel with its tables on the heap), and the round trips must give back the
// signal. Built a second time on a Mac with SONORE_FFT_ACCELERATE, where the
// same checks hold the vDSP backend to the definition instead of to the bits.
#include <sonore/dsp.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
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

using sonore::DynamicFft;
using sonore::Fft;

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const char* what) {
  ++g_checks;
  if (!ok) ++g_failures;
  std::printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
}

constexpr double kPiD = 3.14159265358979323846;

struct Lcg {
  uint32_t s = 12345u;
  float next() {
    s = s * 1664525u + 1013904223u;
    return (float) ((double) (s >> 8) / 8388608.0 - 1.0);
  }
};

/** The DFT by its definition, in double: X[k] = sum x[n] e^(-2 pi j k n / N). */
void dft(const std::vector<double>& re, const std::vector<double>& im, std::vector<double>& outRe,
         std::vector<double>& outIm, bool inverse) {
  const size_t n = re.size();
  outRe.assign(n, 0.0);
  outIm.assign(n, 0.0);
  const double sign = inverse ? 1.0 : -1.0;
  for (size_t k = 0; k < n; ++k) {
    double sr = 0.0, si = 0.0;
    for (size_t t = 0; t < n; ++t) {
      // The angle reduced exactly, so a large k t costs no precision.
      const double a = sign * 2.0 * kPiD * (double) ((k * t) % n) / (double) n;
      sr += re[t] * std::cos(a) - im[t] * std::sin(a);
      si += re[t] * std::sin(a) + im[t] * std::cos(a);
    }
    outRe[k] = inverse ? sr / (double) n : sr;
    outIm[k] = inverse ? si / (double) n : si;
  }
}

/** The error of `got` against `want` as a relative L2 norm, |got - want| /
 *  |want| over the bins: the measure FFT accuracy is stated in, and the one
 *  that grows like log2(N) times the format's epsilon whatever the spectrum
 *  looks like. (A peak error over the RMS reads a peaky spectrum as worse.) */
double relError(const float* gotRe, const float* gotIm, const std::vector<double>& wantRe,
                const std::vector<double>& wantIm, size_t count) {
  double norm = 0.0, err = 0.0;
  for (size_t k = 0; k < count; ++k) {
    norm += wantRe[k] * wantRe[k] + wantIm[k] * wantIm[k];
    const double er = (double) gotRe[k] - wantRe[k];
    const double ei = gotIm ? (double) gotIm[k] - wantIm[k] : 0.0;
    err += er * er + ei * ei;
  }
  return std::sqrt(err / (norm > 0.0 ? norm : 1.0));
}

template <size_t N>
void testSize() {
  // Fft<8192> is 96 KB of tables: a member, never a local.
  static Fft<N> fixed;
  DynamicFft dynamic(N);
  Lcg lcg;
  std::vector<float> re(N), im(N), sig(N);
  std::vector<double> dre(N), dim(N), dsig(N), zero(N, 0.0), wantRe, wantIm;
  for (size_t i = 0; i < N; ++i) {
    re[i] = lcg.next();
    im[i] = lcg.next();
    sig[i] = 0.7f * (float) std::sin(0.031 * (double) i) + 0.3f * lcg.next();
    dre[i] = re[i];
    dim[i] = im[i];
    dsig[i] = sig[i];
  }
  // A float FFT's relative L2 error grows like log2(N) times epsilon (6e-8);
  // 2e-7 a stage is generous for float and still a thousand times tighter
  // than a wrong twiddle or a misplaced bin would leave.
  double stages = 0.0;
  for (size_t m = N; m > 1; m >>= 1) stages += 1.0;
  const double tolerance = 2e-7 * stages + 1e-7;

  char what[192];
  // Complex, forward and inverse, against the definition.
  {
    std::vector<float> r = re, i = im;
    fixed.transform(r.data(), i.data(), false);
    dft(dre, dim, wantRe, wantIm, false);
    const double fwd = relError(r.data(), i.data(), wantRe, wantIm, N);
    std::vector<float> r2 = re, i2 = im;
    fixed.transform(r2.data(), i2.data(), true);
    dft(dre, dim, wantRe, wantIm, true);
    const double inv = relError(r2.data(), i2.data(), wantRe, wantIm, N);
    std::snprintf(what, sizeof what, "%5zu points, complex: the DFT's definition to %.1e forward, %.1e inverse",
                  N, fwd, inv);
    check(fwd < tolerance && inv < tolerance, what);
  }
  // Real forward, against the definition (and against the complex transform
  // of the same samples with zero imaginary parts).
  {
    std::vector<float> r(N / 2 + 1), i(N / 2 + 1);
    fixed.realForward(sig.data(), r.data(), i.data());
    dft(dsig, zero, wantRe, wantIm, false);
    const double err = relError(r.data(), i.data(), wantRe, wantIm, N / 2 + 1);
    std::snprintf(what, sizeof what, "%5zu points, real forward: bins 0..N/2 of the definition to %.1e", N, err);
    check(err < tolerance, what);
    // The round trip.
    std::vector<float> back(N);
    fixed.realInverse(r.data(), i.data(), back.data());
    double worst = 0.0;
    for (size_t k = 0; k < N; ++k) worst = std::fmax(worst, std::fabs((double) back[k] - (double) sig[k]));
    std::snprintf(what, sizeof what, "%5zu points, realInverse(realForward(x)) is x to %.1e", N, worst);
    check(worst < tolerance, what);
  }
  // The runtime-sized transform is the same kernel: the same bits.
  {
    std::vector<float> r1 = re, i1 = im, r2 = re, i2 = im;
    fixed.transform(r1.data(), i1.data(), false);
    dynamic.transform(r2.data(), i2.data(), false);
    bool same = std::memcmp(r1.data(), r2.data(), N * 4) == 0 && std::memcmp(i1.data(), i2.data(), N * 4) == 0;
    std::vector<float> a(N / 2 + 1), b(N / 2 + 1), c(N / 2 + 1), d(N / 2 + 1), oa(N), ob(N);
    fixed.realForward(sig.data(), a.data(), b.data());
    dynamic.realForward(sig.data(), c.data(), d.data());
    const bool fwdSame = std::memcmp(a.data(), c.data(), a.size() * 4) == 0 &&
                         std::memcmp(b.data(), d.data(), b.size() * 4) == 0;
    fixed.realInverse(a.data(), b.data(), oa.data());
    dynamic.realInverse(c.data(), d.data(), ob.data());
    const bool invSame = std::memcmp(oa.data(), ob.data(), N * 4) == 0;
    if (std::strcmp(DynamicFft::backend(), "portable") == 0) {
      std::snprintf(what, sizeof what, "%5zu points: DynamicFft gives Fft<N>'s bits, complex and real, both ways", N);
      check(same && fwdSame && invSame, what);
    } else {
      // vDSP orders its arithmetic differently: held to the definition.
      std::vector<float> r3 = re, i3 = im;
      dynamic.transform(r3.data(), i3.data(), false);
      dft(dre, dim, wantRe, wantIm, false);
      const double err = relError(r3.data(), i3.data(), wantRe, wantIm, N);
      dynamic.realForward(sig.data(), c.data(), d.data());
      dft(dsig, zero, wantRe, wantIm, false);
      const double errReal = relError(c.data(), d.data(), wantRe, wantIm, N / 2 + 1);
      dynamic.realInverse(c.data(), d.data(), ob.data());
      double worst = 0.0;
      for (size_t k = 0; k < N; ++k) worst = std::fmax(worst, std::fabs((double) ob[k] - (double) sig[k]));
      std::snprintf(what, sizeof what,
                    "%5zu points, %s backend: complex %.1e, real %.1e, real round trip %.1e of the definition",
                    N, DynamicFft::backend(), err, errReal, worst);
      check(err < tolerance && errReal < tolerance && worst < tolerance, what);
    }
  }
}

void testDynamic() {
  std::printf("DynamicFft\n");
  DynamicFft f;
  check(f.size() == 0 && f.numBins() == 0, "before setSize(): no size, no bins");
  float r[4] = {1, 2, 3, 4}, i[4] = {0, 0, 0, 0};
  f.transform(r, i, false);
  check(r[0] == 1.0f && r[3] == 4.0f, "and a transform before setSize() leaves the data alone");
  check(!f.setSize(1000) && !f.setSize(2) && !f.setSize(0) && f.size() == 0,
        "a size that is not a power of two from 4 is refused, and nothing changes");
  check(f.setSize(512) && f.size() == 512 && f.numBins() == 257, "setSize(512): 512 points, 257 bins");
  check(!f.setSize(3000) && f.size() == 512, "a refused resize keeps the size it had");
  DynamicFft copy = f;
  check(copy.size() == 512, "a copy has the same size (and its own tables)");

  // Every transform allocates nothing once sized.
  std::vector<float> sig(512), re(257), im(257), out(512), mag(257), cr(512), ci(512);
  for (size_t k = 0; k < 512; ++k) sig[k] = (float) std::sin(0.1 * (double) k);
  long before = g_allocs.load();
  g_armed = true;
  for (int rep = 0; rep < 100; ++rep) {
    f.realForward(sig.data(), re.data(), im.data());
    f.realInverse(re.data(), im.data(), out.data());
    f.magnitude(sig.data(), mag.data());
    f.transform(cr.data(), ci.data(), rep & 1);
  }
  g_armed = false;
  check(g_allocs.load() == before, "realForward, realInverse, magnitude and transform: zero allocations, 400 calls");

  // magnitude() reads a sine's amplitude where the definition says.
  for (size_t k = 0; k < 512; ++k) sig[k] = (float) std::cos(2.0 * kPiD * 32.0 * (double) k / 512.0);
  f.magnitude(sig.data(), mag.data());
  check(std::fabs(mag[32] - 256.0f) < 1e-3f && mag[31] < 1e-3f && mag[33] < 1e-3f,
        "magnitude(): a bin-centred cosine is N/2 in its bin and nothing beside it");

  // Fft<4> and DynamicFft(4): the smallest size, through the complex path.
  Fft<4> four;
  DynamicFft dfour(4);
  const float x[4] = {1.0f, -2.0f, 0.5f, 3.0f};
  float a[3], b[3], c[3], d[3], back[4];
  four.realForward(x, a, b);
  dfour.realForward(x, c, d);
  // By hand: X[0] = 1 - 2 + 0.5 + 3 = 2.5; X[1] = 1 + 2j - 0.5 + 3j =
  // 0.5 + 5j; X[2] = 1 + 2 + 0.5 - 3 = 0.5. (Within a few ulps: the float
  // twiddle cos(pi/2) is 6e-17, not 0.)
  auto near = [](float v, float want) { return std::fabs(v - want) < 1e-6f; };
  // The two run the SAME code, so the same bits -- unless DynamicFft is on
  // vDSP, which is not bit-identical to the portable transform by design (the
  // reason the backend is opt-in). There each must match the hand-worked
  // answer on its own; asking for identical bits failed the first macOS run.
  const bool portable = std::strcmp(DynamicFft::backend(), "portable") == 0;
  const bool same = portable ? std::memcmp(a, c, sizeof a) == 0 && std::memcmp(b, d, sizeof b) == 0
                             : near(c[0], 2.5f) && near(c[1], 0.5f) && near(d[1], 5.0f) && near(c[2], 0.5f) &&
                                   d[0] == 0.0f;
  check(near(a[0], 2.5f) && near(a[1], 0.5f) && near(b[1], 5.0f) && near(a[2], 0.5f) && b[0] == 0.0f && same,
        portable ? "4 points: the spectrum worked by hand, from both, to the same bits"
                 : "4 points: the spectrum worked by hand, from both (vDSP is not bit-identical by design)");
  four.realInverse(a, b, back);
  check(near(back[0], 1.0f) && near(back[1], -2.0f) && near(back[2], 0.5f) && near(back[3], 3.0f),
        "and back again");
}

void testRealHalf() {
  std::printf("real transforms\n");
  // The real transform of a 2N-point signal is not merely close to the
  // complex transform of it with zero imaginary parts -- it is the same
  // spectrum, to float rounding.
  static Fft<2048> f;
  std::vector<float> sig(2048), re(2048), im(2048), rr(1025), ri(1025);
  Lcg lcg;
  for (size_t k = 0; k < 2048; ++k) re[k] = sig[k] = lcg.next();
  f.transform(re.data(), im.data(), false);
  f.realForward(sig.data(), rr.data(), ri.data());
  double worst = 0.0, scale = 0.0;
  for (size_t k = 0; k <= 1024; ++k) {
    worst = std::fmax(worst, std::fabs((double) rr[k] - re[k]) + std::fabs((double) ri[k] - im[k]));
    scale = std::fmax(scale, std::fabs((double) re[k]));
  }
  check(worst < 1e-5 * scale, "realForward is the complex transform's half-spectrum, to float rounding");
  check(ri[0] == 0.0f && ri[1024] == 0.0f, "with DC and Nyquist exactly real");
}

} // namespace

int main() {
  std::printf("FFT, against the DFT's definition\n");
  testSize<4>();
  testSize<8>();
  testSize<16>();
  testSize<64>();
  testSize<256>();
  testSize<1024>();
  testSize<4096>();
  testSize<8192>();
  testDynamic();
  testRealHalf();
#if defined(SONORE_FFT_ACCELERATE) && defined(__APPLE__)
  check(std::strcmp(DynamicFft::backend(), "accelerate") == 0, "built with SONORE_FFT_ACCELERATE: on vDSP");
#endif
  std::printf("\nbackend %s\n%d checks, %d failed\n", DynamicFft::backend(), g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
