// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: the options on the processors -- the ladder's six modes, the
// pan and dry/wet laws, the one-pole's allpass -- plus the fast tan / sinh /
// cosh, the oversampler whose factor is chosen at run time, and decoding
// audio files from memory.
//
// Each is held to something outside itself: a filter mode to its transfer
// function evaluated in double, a law to the arithmetic that defines it, an
// approximation to the library function over a dense sweep, the runtime
// oversampler to the compile-time one bit for bit, and a decode from memory
// to the decode of the same file from disk, bit for bit.
#include <sonore/dsp.h>
#include <sonore/audiofile.h>
#include <sonore/fast_math.h>
#include <sonore/impulse.h>

#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

#ifndef SONORE_TEST_DATA_DIR
#define SONORE_TEST_DATA_DIR "tests/data"
#endif

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const char* what) {
  ++g_checks;
  if (!ok) ++g_failures;
  std::printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
}

constexpr double kPi = 3.14159265358979323846;

uint32_t bitsOf(float f) {
  uint32_t u;
  std::memcpy(&u, &f, 4);
  return u;
}

// The steady-state response of `process` at `hz`: a sine run long enough for
// the transient to die, then correlated against sine and cosine over a whole
// number of cycles. Returns the complex gain (magnitude and phase).
template <typename F>
std::complex<double> measure(F&& process, double hz, double sr) {
  const int settle = (int) sr / 2;
  const int cycles = 40;
  const int window = (int) std::lround(cycles * sr / hz);
  double re = 0.0, im = 0.0;
  for (int n = 0; n < settle + window; ++n) {
    const double phase = 2.0 * kPi * hz * n / sr;
    const double y = process(std::sin(phase));
    if (n >= settle) {
      re += y * std::sin(phase);
      im += y * std::cos(phase);
    }
  }
  return std::complex<double>(re, im) * (2.0 / window);
}

// ── LadderFilter's modes ─────────────────────────────────────────────────────

// The ladder's response in double from its definition: four TPT one-poles
// H(z) = G (1 + z^-1) / (1 - (1 - 2G) z^-1), u = x / (1 + k H^4), and each
// mode a polynomial in H times u.
std::complex<double> ladderTheory(sonore::LadderMode mode, double hz, double cutoff, double k, double sr) {
  const double g = std::tan(kPi * cutoff / sr);
  const double G = g / (1.0 + g);
  const std::complex<double> zInv = std::polar(1.0, -2.0 * kPi * hz / sr);
  const std::complex<double> H = G * (1.0 + zInv) / (1.0 - (1.0 - 2.0 * G) * zInv);
  const std::complex<double> u = 1.0 / (1.0 + k * H * H * H * H);
  const std::complex<double> hp = 1.0 - H;
  using M = sonore::LadderMode;
  switch (mode) {
  case M::Lowpass24: return H * H * H * H * u;
  case M::Lowpass12: return H * H * u;
  case M::Highpass24: return hp * hp * hp * hp * u;
  case M::Highpass12: return hp * hp * u;
  case M::Bandpass24: return 4.0 * H * H * hp * hp * u;
  case M::Bandpass12: return 2.0 * H * hp * u;
  }
  return 0.0;
}

template <typename T>
void testLadderModes(const char* precision) {
  using M = sonore::LadderMode;
  const M modes[] = {M::Lowpass24, M::Lowpass12, M::Highpass24, M::Highpass12, M::Bandpass24, M::Bandpass12};
  const char* names[] = {"Lowpass24", "Lowpass12", "Highpass24", "Highpass12", "Bandpass24", "Bandpass12"};
  const double sr = 48000.0, cutoff = 1000.0;
  // Linear (saturation 0) so the theory is exact; resonance 0 and 0.5. The
  // response is compared as a COMPLEX number, so the phase is held too.
  // Relative to the response, with a floor: a highpass two decades down is
  // -80 dB, where a float's rounding of the five-term sum is what is left.
  const double floor = std::is_same<T, float>::value ? 2e-6 : 1e-12;
  for (double resonance : {0.0, 0.5}) {
    const double k = 4.2 * resonance;
    for (int m = 0; m < 6; ++m) {
      double worst = 0.0;
      bool ok = true;
      for (double hz : {100.0, 400.0, 1000.0, 2500.0, 8000.0}) {
        sonore::LadderFilterT<T> f;
        f.setSampleRate((T) sr);
        f.setSaturation(T(0.0));
        f.set((T) cutoff, (T) resonance);
        f.setMode(modes[m]);
        const std::complex<double> got = measure([&](double x) { return (double) f.process((T) x); }, hz, sr);
        const std::complex<double> want = ladderTheory(modes[m], hz, cutoff, k, sr);
        const double err = std::abs(got - want);
        ok = ok && err <= 1e-4 * std::abs(want) + floor;
        if (err / std::abs(want) > worst) worst = err / std::abs(want);
      }
      char what[180];
      std::snprintf(what, sizeof what,
                    "ladder %s %s, resonance %.1f: gain and phase are its transfer function's (worst %.2g relative)",
                    precision, names[m], resonance, worst);
      check(ok, what);
    }
  }
  // The highpass and bandpass really are: DC is rejected, the bandpass peaks
  // at 0 dB at the cutoff with no resonance.
  {
    sonore::LadderFilterT<T> hp;
    hp.setSampleRate((T) sr);
    hp.setSaturation(T(0.0));
    hp.set((T) cutoff, T(0.3));
    hp.setMode(M::Highpass24);
    T last = 0;
    for (int n = 0; n < 48000; ++n) last = hp.process(T(1.0));
    char what[120];
    std::snprintf(what, sizeof what, "ladder %s Highpass24: a second of DC settles to %.2g", precision, (double) last);
    check(std::fabs((double) last) < 1e-5, what);
    for (M bp : {M::Bandpass12, M::Bandpass24}) {
      sonore::LadderFilterT<T> f;
      f.setSampleRate((T) sr);
      f.setSaturation(T(0.0));
      f.set((T) cutoff, T(0.0));
      f.setMode(bp);
      const double db = 20.0 * std::log10(std::abs(measure([&](double x) { return (double) f.process((T) x); }, cutoff, sr)));
      std::snprintf(what, sizeof what, "ladder %s %s peaks at %.4f dB at the cutoff with no resonance", precision,
                    bp == M::Bandpass12 ? "Bandpass12" : "Bandpass24", db);
      check(std::fabs(db) < 0.01, what);
    }
  }
  // Lowpass24 is the default, and the default is the filter it always was.
  {
    sonore::LadderFilterT<T> a, b;
    for (auto* f : {&a, &b}) {
      f->setSampleRate((T) sr);
      f->set(T(700.0), T(0.8));
    }
    b.setMode(M::Lowpass24);
    bool same = a.mode() == M::Lowpass24;
    for (int n = 0; n < 4000; ++n) {
      const T x = (T) std::sin(0.031 * n) * T(0.9);
      same = same && a.process(x) == b.process(x);
    }
    char what[100];
    std::snprintf(what, sizeof what, "ladder %s: Lowpass24 is the default mode", precision);
    check(same, what);
  }
}

// ── Pan laws, the Panner, the mixers ─────────────────────────────────────────

void testPanLaws() {
  using sonore::PanLaw;
  struct Law {
    PanLaw law;
    const char* name;
    double centreDb;
    int complement; // 2: a^2 + b^2 = 1; 1: a + b = 1; 0: max(a, b) = 1
  };
  const Law laws[] = {
      {PanLaw::ConstantPower, "ConstantPower", -3.0103, 2}, {PanLaw::Linear, "Linear", -6.0206, 1},
      {PanLaw::Balance, "Balance", 0.0, 0},                 {PanLaw::Sine4_5dB, "Sine4_5dB", -4.5154, -1},
      {PanLaw::Sine6dB, "Sine6dB", -6.0206, 1},             {PanLaw::SquareRoot3dB, "SquareRoot3dB", -3.0103, 2},
      {PanLaw::SquareRoot4_5dB, "SquareRoot4_5dB", -4.5154, -1},
  };
  for (const Law& l : laws) {
    double a0, b0, a1, b1, ac, bc;
    sonore::panLawGains(l.law, 0.0, a0, b0);
    sonore::panLawGains(l.law, 1.0, a1, b1);
    sonore::panLawGains(l.law, 0.5, ac, bc);
    bool ends = std::fabs(a0 - 1.0) < 1e-12 && std::fabs(b0) < 1e-12 && std::fabs(a1) < 1e-12 &&
                std::fabs(b1 - 1.0) < 1e-12;
    const double centre = 20.0 * std::log10(ac);
    bool monotonic = true, complement = true, finite = true;
    double pa = 2.0, pb = -1.0;
    for (int i = 0; i <= 1000; ++i) {
      double a, b;
      sonore::panLawGains(l.law, i / 1000.0, a, b);
      finite = finite && std::isfinite(a) && std::isfinite(b) && a >= 0.0 && b >= 0.0;
      monotonic = monotonic && a <= pa + 1e-15 && b >= pb - 1e-15;
      pa = a;
      pb = b;
      if (l.complement == 2) complement = complement && std::fabs(a * a + b * b - 1.0) < 1e-12;
      if (l.complement == 1) complement = complement && std::fabs(a + b - 1.0) < 1e-12;
      if (l.complement == 0) complement = complement && std::fabs((a > b ? a : b) - 1.0) < 1e-12;
    }
    char what[200];
    std::snprintf(what, sizeof what,
                  "PanLaw::%s: 1/0 at one end, 0/1 at the other, the centre at %.4f dB (both sides equal), "
                  "monotonic, finite%s",
                  l.name, centre,
                  l.complement == 2 ? ", a^2 + b^2 = 1" : l.complement == 1 ? ", a + b = 1" : l.complement == 0 ? ", the near side at unity" : "");
    check(ends && std::fabs(centre - l.centreDb) < 1e-3 && std::fabs(ac - bc) < 1e-12 && monotonic && complement && finite,
          what);
  }
  // Out of range is held, not extrapolated.
  {
    float a, b;
    sonore::panLawGains(PanLaw::Linear, 1.7f, a, b);
    float c, d;
    sonore::panLawGains(PanLaw::Linear, -3.0f, c, d);
    check(a == 0.0f && b == 1.0f && c == 1.0f && d == 0.0f, "panLawGains holds the position to 0 .. 1");
  }

  // The Panner, untouched: the same bits as the cos/sin it always computed.
  {
    bool same = true;
    for (float pan : {-1.0f, -0.73f, -0.2f, 0.0f, 0.31f, 0.999f, 1.0f, 2.0f}) {
      sonore::Panner p;
      p.setPan(pan);
      const float q = ((pan < -1.0f ? -1.0f : pan > 1.0f ? 1.0f : pan) + 1.0f) * 0.5f;
      const float wantL = std::cos(q * sonore::piT<float> * 0.5f), wantR = std::sin(q * sonore::piT<float> * 0.5f);
      float l, r;
      p.process(0.8f, l, r);
      same = same && bitsOf(l) == bitsOf(0.8f * wantL) && bitsOf(r) == bitsOf(0.8f * wantR);
    }
    check(same, "Panner: ConstantPower (the default) gives the bits the old cos/sin did");
  }
  {
    sonore::Panner p;
    p.setLaw(PanLaw::Linear);
    float l, r;
    p.process(1.0f, l, r);
    const bool centre = l == 0.5f && r == 0.5f;
    p.setPan(-1.0f);
    p.process(1.0f, l, r);
    const bool left = l == 1.0f && r == 0.0f;
    p.setLaw(PanLaw::Balance);
    p.setPan(0.0f);
    float sl = 0.7f, sr = -0.4f;
    p.processStereo(sl, sr);
    check(centre && left && sl == 0.7f && sr == -0.4f && p.law() == PanLaw::Balance,
          "Panner::setLaw: Linear is 0.5/0.5 centred and 1/0 hard left; Balance passes a centred stereo pair untouched");
  }

  // The DryWetMixer: default bits unchanged, laws applied, a law change glides.
  {
    bool same = true;
    for (float mix : {0.0f, 0.13f, 0.5f, 0.77f, 1.0f}) {
      sonore::DryWetMixer m(mix);
      const float wet = std::sin(mix * sonore::piT<float> * 0.5f), dry = std::cos(mix * sonore::piT<float> * 0.5f);
      same = same && bitsOf(m.process(0.6f, -0.3f)) == bitsOf(dry * 0.6f + wet * -0.3f);
    }
    check(same, "DryWetMixer: equal power (the default) gives the bits the old cos/sin did");
  }
  {
    sonore::DryWetMixer m;
    m.setLaw(PanLaw::Linear);
    m.setMix(0.5f);
    const bool linear = m.process(1.0f, 1.0f) == 1.0f; // 0.5 + 0.5: correlated signals hold their level
    sonore::DryWetMixer b;
    b.setLaw(PanLaw::Balance);
    b.setMix(0.25f);
    const bool balance = b.process(1.0f, 0.0f) == 1.0f && b.law() == PanLaw::Balance;
    // Running now: a law change glides rather than stepping.
    sonore::DryWetMixer g(0.5f);
    g.setSampleRate(48000.0f);
    const float before = g.process(1.0f, 0.0f); // cos(pi/4)
    g.setLaw(PanLaw::Balance);                    // dry target 1.0
    const float next = g.process(1.0f, 0.0f);
    float settled = next;
    for (int n = 0; n < 48000; ++n) settled = g.process(1.0f, 0.0f);
    check(linear && balance && next > before && next < 1.0f && settled == 1.0f,
          "DryWetMixer::setLaw: Linear sums a correlated pair to unity at 0.5, Balance keeps the dry at full level "
          "below 0.5, and a change of law glides");
  }
  {
    // A glide toward a gain of 1 used to STALL in float, ~3e-5 short, so a
    // mix automated back to 0 % never gave back the dry signal exactly.
    sonore::DryWetMixer m(1.0f);
    m.setSampleRate(48000.0f);
    m.process(0.0f, 0.0f);
    m.setMix(0.0f);
    float y = 0.0f;
    for (int n = 0; n < 96000; ++n) y = m.process(0.37f, -0.81f);
    sonore::DryWetMixer w(0.0f);
    w.setSampleRate(48000.0f);
    w.process(0.0f, 0.0f);
    w.setMix(1.0f);
    float z = 0.0f;
    for (int n = 0; n < 96000; ++n) z = w.process(0.37f, -0.81f);
    char what[160];
    std::snprintf(what, sizeof what,
                  "DryWetMixer: a mix glided to 0 %% lands on exactly the dry signal (%.9g), to 100 %% on the wet "
                  "(%.9g)",
                  (double) y, (double) z);
    check(y == 0.37f && z == -0.81f, what);
  }
  {
    sonore::CompensatedDryWetMixer<2, 64> m;
    m.setLaw(PanLaw::Linear);
    m.setMix(0.5f);
    check(m.process(0, 1.0f, 2.0f) == 1.5f, "CompensatedDryWetMixer::setLaw reaches every channel's mixer");
  }
}

// ── OnePole's allpass ────────────────────────────────────────────────────────

void testOnePoleAllpass() {
  const double sr = 48000.0;
  double worstMag = 0.0, phaseAtCutoff = 0.0;
  for (double cutoff : {200.0, 1000.0, 6000.0}) {
    for (double hz : {50.0, 300.0, 1000.0, 3000.0, 6000.0, 15000.0}) {
      sonore::OnePoleT<double> f;
      f.setSampleRate(sr);
      f.setCutoff(cutoff);
      const std::complex<double> h = measure([&](double x) { return f.ap(x); }, hz, sr);
      const double db = std::fabs(20.0 * std::log10(std::abs(h)));
      if (db > worstMag) worstMag = db;
      if (cutoff == 1000.0 && hz == 1000.0) phaseAtCutoff = std::arg(h) * 180.0 / kPi;
    }
  }
  char what[160];
  std::snprintf(what, sizeof what, "OnePole::ap: unity gain everywhere (worst %.5f dB), -90 degrees at the cutoff (%.3f)",
                worstMag, phaseAtCutoff);
  check(worstMag < 1e-4 && std::fabs(phaseAtCutoff + 90.0) < 0.01, what);
  sonore::OnePole a, b;
  a.setCutoff(2500.0f);
  b.setCutoff(2500.0f);
  bool same = true;
  for (int n = 0; n < 1000; ++n) {
    const float x = (float) std::sin(0.07 * n);
    same = same && bitsOf(a.ap(x)) == bitsOf(2.0f * b.lp(x) - x);
  }
  check(same, "OnePole::ap is 2 lp(x) - x on the same state, bit for bit");
}

// ── fastTan, fastSinh, fastCosh ──────────────────────────────────────────────

double relTan(float x) {
  const double t = std::tan((double) x);
  return t == 0.0 ? 0.0 : std::fabs(((double) sonore::fastTan(x) - t) / t);
}

void testFastMath() {
  double main = 0.0, poles = 0.0, wide = 0.0;
  for (long i = -2000000; i <= 2000000; ++i) {
    const float x = (float) (i * (1.5707963 / 2000000.0));
    if (std::fabs((double) x) >= kPi / 2) continue;
    const double e = relTan(x);
    if (e > main) main = e;
  }
  // The 20000 floats on the inside of each pole of the principal range...
  {
    float below = (float) (kPi / 2), above = (float) (-kPi / 2);
    if ((double) below >= kPi / 2) below = std::nextafter(below, 0.0f);
    if ((double) above <= -kPi / 2) above = std::nextafter(above, 0.0f);
    for (int i = 0; i < 20000; ++i) {
      const double e = relTan(below) > relTan(above) ? relTan(below) : relTan(above);
      if (e > poles) poles = e;
      below = std::nextafter(below, 0.0f);
      above = std::nextafter(above, 0.0f);
    }
  }
  // ...and the 20000 on either side of every pole out to 5 pi.
  for (int k = -6; k <= 5; ++k) {
    const float pole = (float) (k * kPi + kPi / 2);
    float lo = pole, hi = pole;
    for (int i = 0; i < 20000; ++i) {
      lo = std::nextafter(lo, -1e9f);
      hi = std::nextafter(hi, 1e9f);
      const double w = relTan(lo) > relTan(hi) ? relTan(lo) : relTan(hi);
      if (w > wide) wide = w;
    }
  }
  for (long i = -1000000; i <= 1000000; ++i) {
    const double e = relTan((float) (i * 1e-3));
    if (e > wide) wide = e;
  }
  char what[200];
  std::snprintf(what, sizeof what,
                "fastTan: relative error %.2g over (-pi/2, pi/2), %.2g beside the poles there, %.2g out to |x| < 1000",
                main, poles, wide);
  check(main < 3e-7 && poles < 3e-7 && wide < 2e-6, what);

  double s1 = 0.0, s20 = 0.0, s88 = 0.0, c20 = 0.0, c88 = 0.0;
  bool symmetric = true;
  for (long i = -880000; i <= 880000; ++i) {
    const float x = (float) (i * 1e-4);
    const double s = std::sinh((double) x), c = std::cosh((double) x);
    const double es = s == 0.0 ? 0.0 : std::fabs(((double) sonore::fastSinh(x) - s) / s);
    const double ec = std::fabs(((double) sonore::fastCosh(x) - c) / c);
    if (std::fabs(x) < 1.0f && es > s1) s1 = es;
    if (std::fabs(x) < 20.0f) {
      if (es > s20) s20 = es;
      if (ec > c20) c20 = ec;
    }
    if (es > s88) s88 = es;
    if (ec > c88) c88 = ec;
    symmetric = symmetric && sonore::fastSinh(-x) == -sonore::fastSinh(x) && sonore::fastCosh(-x) == sonore::fastCosh(x);
  }
  std::snprintf(what, sizeof what,
                "fastSinh: relative %.2g below 1, %.2g below 20, %.2g below 88; fastCosh %.2g and %.2g; odd and even "
                "exactly",
                s1, s20, s88, c20, c88);
  check(s1 < 2e-7 && s20 < 5e-6 && s88 < 1e-5 && c20 < 5e-6 && c88 < 1e-5 && symmetric, what);
}

// ── DynamicOversampler against the compile-time one ──────────────────────────

template <int S, typename Dynamic, typename Fixed>
bool sameAsFixed(Dynamic& dyn, bool block) {
  Fixed fixed;
  if (!dyn.setFactor(1 << S)) return false;
  if (dyn.latencySamples() != Fixed::latencySamples() || dyn.factor() != Fixed::factor()) return false;
  using T = typename Fixed::T;
  auto shaper = [](T s) { return (T) std::tanh(s * T(2.5)); };
  std::vector<T> a(3000), b(3000);
  for (size_t n = 0; n < a.size(); ++n) a[n] = b[n] = (T) (0.8 * std::sin(0.0123 * n) + 0.15 * std::sin(0.31 * n));
  if (block) {
    // Blocks of uneven sizes, so the 64-sample pieces fall differently.
    size_t done = 0;
    int size = 1;
    while (done < a.size()) {
      const int n = (int) (a.size() - done < (size_t) size ? a.size() - done : (size_t) size);
      dyn.processBlock(a.data() + done, n, shaper);
      fixed.processBlock(b.data() + done, n, shaper);
      done += (size_t) n;
      size = size * 3 % 211 + 1;
    }
  } else {
    for (size_t n = 0; n < a.size(); ++n) {
      a[n] = dyn.process(a[n], shaper);
      b[n] = fixed.process(b[n], shaper);
    }
  }
  return std::memcmp(a.data(), b.data(), a.size() * sizeof(T)) == 0;
}

template <template <int, int, typename> class Fixed, template <int, int, typename> class Dynamic, int P, typename T>
bool allFactors(bool block) {
  Dynamic<5, P, T> d;
  return sameAsFixed<0, Dynamic<5, P, T>, Fixed<0, P, T>>(d, block) &&
         sameAsFixed<1, Dynamic<5, P, T>, Fixed<1, P, T>>(d, block) &&
         sameAsFixed<2, Dynamic<5, P, T>, Fixed<2, P, T>>(d, block) &&
         sameAsFixed<3, Dynamic<5, P, T>, Fixed<3, P, T>>(d, block) &&
         sameAsFixed<4, Dynamic<5, P, T>, Fixed<4, P, T>>(d, block) &&
         sameAsFixed<5, Dynamic<5, P, T>, Fixed<5, P, T>>(d, block);
}

void testDynamicOversampler() {
  using namespace sonore;
  check(allFactors<Oversampler, DynamicOversampler, 33, float>(false) &&
            allFactors<Oversampler, DynamicOversampler, 33, float>(true),
        "DynamicOversampler at 1x .. 32x: the same bits and latency as Oversampler<0..5>, per sample and per block");
  check(allFactors<OversamplerIir, DynamicOversamplerIir, 9, float>(false) &&
            allFactors<OversamplerIir, DynamicOversamplerIir, 9, double>(true),
        "DynamicOversamplerIir (float and double): the same bits as OversamplerIir<0..5>");
  DynamicOversampler<3> d;
  d.setFactor(4);
  const bool refused = !d.setFactor(3) && !d.setFactor(16) && !d.setFactor(0) && !d.setFactor(-4) && d.factor() == 4 &&
                       d.stages() == 2 && DynamicOversampler<3>::maxFactor() == 8;
  check(refused, "DynamicOversampler::setFactor refuses a non-power of two and anything past maxFactor(), keeping its "
                 "factor");
  int lat[6];
  DynamicOversampler<> e;
  for (int s = 0; s <= 5; ++s) {
    e.setFactor(1 << s);
    lat[s] = e.latencySamples();
  }
  char what[120];
  std::snprintf(what, sizeof what, "DynamicOversampler latency at 1x..32x: %d %d %d %d %d %d", lat[0], lat[1], lat[2],
                lat[3], lat[4], lat[5]);
  check(lat[0] == 0 && lat[1] == 16 && lat[2] == 24 && lat[3] == 28 && lat[4] == 30 && lat[5] == 31, what);
}

// ── Audio files from memory ──────────────────────────────────────────────────

std::vector<uint8_t> slurp(const std::string& path) {
  std::vector<uint8_t> out;
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return out;
  std::fseek(f, 0, SEEK_END);
  const long size = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  out.resize(size > 0 ? (size_t) size : 0);
  if (!out.empty() && std::fread(out.data(), 1, out.size(), f) != out.size()) out.clear();
  std::fclose(f);
  return out;
}

bool sameAudio(const sonore::WavData& a, const sonore::WavData& b) {
  return a.sampleRate == b.sampleRate && a.numChannels == b.numChannels && a.samples.size() == b.samples.size() &&
         !a.samples.empty() && std::memcmp(a.samples.data(), b.samples.data(), a.samples.size() * sizeof(float)) == 0;
}

// A WAV written byte by byte in memory: decodable with no file system at all,
// which is the point -- and the WebAssembly build has none.
std::vector<uint8_t> handMadeWav(const int16_t* samples, int frames, int channels, uint32_t rate) {
  std::vector<uint8_t> w;
  auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) w.push_back((uint8_t) (v >> (8 * i))); };
  auto u16 = [&](uint16_t v) { w.push_back((uint8_t) v); w.push_back((uint8_t) (v >> 8)); };
  const uint32_t data = (uint32_t) (frames * channels * 2);
  w.insert(w.end(), {'R', 'I', 'F', 'F'});
  u32(36 + data);
  w.insert(w.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
  u32(16);
  u16(1);
  u16((uint16_t) channels);
  u32(rate);
  u32(rate * (uint32_t) channels * 2);
  u16((uint16_t) (channels * 2));
  u16(16);
  w.insert(w.end(), {'d', 'a', 't', 'a'});
  u32(data);
  for (int i = 0; i < frames * channels; ++i) u16((uint16_t) samples[i]);
  return w;
}

void testMemoryDecoding() {
  using namespace sonore;
  {
    const int16_t pcm[] = {0, 32767, -32768, 1, -1, 12345, -23456, 100};
    const std::vector<uint8_t> wav = handMadeWav(pcm, 4, 2, 44100);
    WavData a, b;
    bool ok = readAudioFile(wav.data(), wav.size(), &a) && readWav(wav.data(), wav.size(), &b) &&
              a.sampleRate == 44100 && a.numChannels == 2 && a.samples.size() == 8 && sameAudio(a, b);
    for (int i = 0; ok && i < 8; ++i) ok = bitsOf(a.samples[(size_t) i]) == bitsOf((float) pcm[i] / 32768.0f);
    check(ok, "readAudioFile from memory: a WAV built in memory decodes to its samples, with no file anywhere");
  }
#if defined(__EMSCRIPTEN__)
  // A standalone WebAssembly build has no file system to compare against.
  std::puts("  SKIP the file-against-memory comparisons: no file system in this build");
  return;
#endif
  // COMMITTED fixtures only. tests/data also holds written.aiff,
  // written16.aiff and encoded.flac, but sdk_tests writes those when it runs
  // and they are gitignored: tests run in any order, and in a fresh checkout
  // this one ran first and found three files that did not exist yet.
  const char* files[] = {"ref16.wav",     "ref24.wav",  "refmono.wav",  "tonal.wav",   "ref16be.aiff",
                         "ref16_c0.flac", "ref16_c8.flac", "ref24.flac", "refmono.flac", "ref.mp3",
                         "refmono.mp3",   "tonal.mp3",  "ref.ogg"};
  int matched = 0, total = 0, twins = 0;
  for (const char* name : files) {
    const std::string path = std::string(SONORE_TEST_DATA_DIR) + "/" + name;
    const std::vector<uint8_t> bytes = slurp(path);
    ++total;
    WavData fromFile, fromMemory, twin;
    const bool okFile = readAudioFile(path.c_str(), &fromFile);
    const bool okMemory = readAudioFile(bytes.data(), bytes.size(), &fromMemory);
    if (okFile && okMemory && sameAudio(fromFile, fromMemory)) ++matched;
    else std::printf("       %s: file %d, memory %d\n", name, okFile, okMemory);
    // The format's own twin too.
    const std::string n(name);
    bool okTwin = false;
    if (n.find(".wav") != std::string::npos) okTwin = readWav(bytes.data(), bytes.size(), &twin);
    else if (n.find(".aiff") != std::string::npos) okTwin = readAiff(bytes.data(), bytes.size(), &twin);
    else if (n.find(".flac") != std::string::npos) okTwin = readFlac(bytes.data(), bytes.size(), &twin);
    else if (n.find(".mp3") != std::string::npos) okTwin = readMp3(bytes.data(), bytes.size(), &twin);
    else if (n.find(".ogg") != std::string::npos) okTwin = readOgg(bytes.data(), bytes.size(), &twin);
    if (okTwin && sameAudio(twin, fromFile)) ++twins;
  }
  char what[160];
  std::snprintf(what, sizeof what,
                "readAudioFile from memory: %d of %d WAV/AIFF/FLAC/MP3/Ogg files decode to the file reader's bits, "
                "%d through the format's own twin",
                matched, total, twins);
  check(matched == total && twins == total, what);

  // Refusals: what the file reader refuses, the memory one does too.
  const std::vector<uint8_t> wav = slurp(std::string(SONORE_TEST_DATA_DIR) + "/ref16.wav");
  WavData out;
  const uint8_t garbage[64] = {'R', 'I', 'F', 'F', 1, 2, 3, 4, 'W', 'A', 'V', 'E'};
  const bool refused = !readAudioFile(nullptr, 100, &out) && !readAudioFile(wav.data(), 0, &out) &&
                       !readAudioFile(wav.data(), 3, &out) && !readAudioFile(wav.data(), wav.size(), nullptr) &&
                       !readAudioFile(garbage, sizeof garbage, &out) && !readWav(wav.data(), 30, &out) &&
                       !readFlac(wav.data(), wav.size(), &out) && !readOgg(wav.data(), wav.size(), &out) &&
                       out.samples.empty();
  check(refused, "readAudioFile from memory refuses null, empty, short, truncated and mislabelled bytes, leaving out "
                 "empty");

  // An impulse response from the bytes of a file compiled into a plugin.
  ImpulseResponse fromFile, fromBytes;
  const std::string flac = std::string(SONORE_TEST_DATA_DIR) + "/ref24.flac";
  const std::vector<uint8_t> flacBytes = slurp(flac);
  const bool loaded = fromFile.loadFromFile(flac.c_str(), 44100.0) &&
                      fromBytes.loadFromFileData(flacBytes.data(), flacBytes.size(), 44100.0);
  bool same = loaded && fromFile.numChannels() == fromBytes.numChannels() && fromFile.length() == fromBytes.length();
  for (size_t c = 0; same && c < fromFile.numChannels(); ++c)
    same = std::memcmp(fromFile.channel(c), fromBytes.channel(c), fromFile.length() * sizeof(float)) == 0;
  check(same, "ImpulseResponse::loadFromFileData: the same response, resampled, as loadFromFile on the same file");
}

} // namespace

int main() {
  testLadderModes<float>("float");
  testLadderModes<double>("double");
  testPanLaws();
  testOnePoleAllpass();
  testFastMath();
  testDynamicOversampler();
  testMemoryDecoding();
  std::printf("\n%d checks, %d failed\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
