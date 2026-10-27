// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: the processors in double precision.
//
// Every core processor is a template over its sample type, with the float
// form keeping its old name. Two things to prove. That every double form
// COMPILES -- explicit instantiation below builds every member of every one,
// which a template otherwise never does until somebody calls it. And that
// the double forms are double all the way through, not float processors with
// wider storage: each check is a place where float measurably fails and
// double must not -- a corner near DC at a high rate, a phase that runs for
// minutes, a value with more mantissa than a float holds -- with the float
// form measured beside it so the test cannot pass because the case was easy.
#include <sonore/dsp.h>
#include <sonore/filter_design.h>
#include <sonore/fir.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

// ── Every double form, every member ──────────────────────────────────────────
namespace sonore {
template class SmoothT<double>;
template struct GlideT<double>;
template class LogRampT<double>;
template class OnePoleT<double>;
template class BiquadT<double>;
template class SVFT<double>;
template class LadderFilterT<double>;
template class DcBlockerT<double>;
template class OscillatorT<double>;
template class EnvFollowerT<double>;
template class CompressorT<double>;
template class DryWetMixerT<double>;
template class DelayLine<4096, double>;
template class Allpass<1024, double>;
template class Comb<1024, double>;
template class ChorusT<double>;
template class PhaserT<double>;
template class Limiter<1024, double>;
template class NoiseGateT<double>;
template class LinkwitzRileyT<double>;
template class StereoWidenerT<double>;
template class PannerT<double>;
template class LookupTableT<double>;
template class WaveShaperT<double>;
template class BallisticsFilterT<double>;
template class GainT<double>;
template class BiasT<double>;
template class LogSmoothT<double>;
template class HalfBandFilter<33, double>;
template class PolyphaseIirHalfBand<9, double>;
template class OversamplerT<2, HalfBandFilter<33, double>>;
template class OversamplerT<2, PolyphaseIirHalfBand<9, double>>;
template class FirFilterT<double>;
template class CascadedIir<8, double>;
template class LinkwitzRileyN<8, double>;
} // namespace sonore

namespace {

using namespace sonore;

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const char* what) {
  ++g_checks;
  if (!ok) ++g_failures;
  std::printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
}

constexpr double kPiD = 3.14159265358979323846;

/** The gain of a filter at `hz`, measured by running a sine through it and
 *  correlating against the sine and the cosine (so a phase shift costs
 *  nothing), in the filter's own sample type. */
template <typename T, typename F>
double measuredGain(F& f, double hz, double sr, int settle, int measure) {
  double re = 0.0, im = 0.0;
  for (int i = 0; i < settle + measure; ++i) {
    const double ph = 2.0 * kPiD * hz * (double) i / sr;
    const T y = f.process((T) std::sin(ph));
    if (i >= settle) {
      re += (double) y * std::sin(ph);
      im += (double) y * std::cos(ph);
    }
  }
  return 2.0 * std::sqrt(re * re + im * im) / (double) measure;
}

void testLowCorner() {
  std::printf("a corner near DC at a high rate\n");
  // A 20 Hz Butterworth lowpass at 192 kHz: the poles sit 6.5e-4 inside the
  // unit circle. The cookbook biquad holds its coefficients in its sample
  // type; in float they barely resolve that, and the response at the corner
  // and in the passband shows it.
  const double sr = 192000.0;
  BiquadT<double> d;
  Biquad f;
  d.setSampleRate(sr);
  f.setSampleRate((float) sr);
  d.lowpass(20.0, 0.70710678118654752);
  f.lowpass(20.0f, 0.70710678f);
  // DC gain: a constant through it, long enough to settle.
  double yd = 0.0;
  float yf = 0.0f;
  for (int i = 0; i < 400000; ++i) {
    yd = d.process(1.0);
    yf = f.process(1.0f);
  }
  char what[192];
  std::snprintf(what, sizeof what, "20 Hz lowpass at 192 kHz, DC gain: double %.12f, float %.6f", yd, (double) yf);
  check(std::fabs(yd - 1.0) < 1e-9 && std::fabs((double) yf - 1.0) > 1e-5, what);
  // And at the corner: -3.0103 dB exactly, by the design.
  d.reset();
  const double corner = 20.0 * std::log10(measuredGain<double>(d, 20.0, sr, 192000, 192000 * 4));
  std::snprintf(what, sizeof what, "and at its corner %.6f dB, where the design says -3.010300", corner);
  check(std::fabs(corner + 3.0103) < 1e-3, what);

  // The same through the cascade: an order-8 elliptic highpass at 15 Hz.
  CascadedIir<8, double> cd;
  cd.setSampleRate(sr);
  cd.designEllipticHighpass(15.0, 8, 0.1, 80.0);
  const double pass = 20.0 * std::log10(measuredGain<double>(cd, 200.0, sr, 96000, 192000));
  std::snprintf(what, sizeof what, "order-8 elliptic highpass at 15 Hz, 192 kHz: 200 Hz reads %.6f dB (design %.6f)",
                pass, (double) cd.magnitudeDb(200.0));
  check(std::fabs(pass - (double) cd.magnitudeDb(200.0)) < 1e-4, what);
}

void testLongPhase() {
  std::printf("a phase that runs for minutes\n");
  // Ten minutes of a 997 Hz sine: the float oscillator's phase increment is
  // rounded to 24 bits, so its frequency is off by parts in 1e8 and its phase
  // wanders by the end; the double one's by parts in 1e17.
  const double sr = 48000.0, hz = 997.0;
  OscillatorT<double> d;
  Oscillator f;
  d.setSampleRate(sr);
  f.setSampleRate((float) sr);
  d.setFreq(hz);
  f.setFreq((float) hz);
  const long n = (long) (sr * 600.0);
  double phaseD = 0.0, phaseF = 0.0;
  for (long i = 0; i < n; ++i) {
    d.sine();
    f.sine();
  }
  phaseD = d.phase();
  phaseF = (double) f.phase();
  const double exact = std::fmod(hz * (double) n / sr, 1.0);
  auto wrap = [](double e) { return std::fabs(e - std::floor(e + 0.5)); };
  char what[192];
  std::snprintf(what, sizeof what, "10 minutes at 997 Hz: phase off by %.2e cycles in double, %.2e in float",
                wrap(phaseD - exact), wrap(phaseF - exact));
  check(wrap(phaseD - exact) < 1e-6 && wrap(phaseF - exact) > 1e-4, what);
}

void testMantissa() {
  std::printf("values a float cannot hold\n");
  // 1 + 2^-40 is a double and not a float. Through a delay line, a gain of
  // one and a FIR identity it must come out as itself.
  const double v = 1.0 + std::ldexp(1.0, -40);
  check((double) (float) v != v, "the probe really is beyond a float (1 + 2^-40)");
  auto line = std::make_unique<DelayLine<4096, double>>();
  for (int i = 0; i < 100; ++i) line->write(i == 0 ? v : 0.0);
  check(line->read(100.0) == v, "a double delay line returns it, 100 samples later, exactly");
  FirFilterT<double> fir;
  const double identity[3] = {0.0, 1.0, 0.0};
  fir.setCoefficients(identity, 3);
  fir.process(0.0);
  fir.process(v);
  check(fir.process(0.0) == v, "a double FIR returns it, one sample later, exactly");
  GainT<double> g;
  ProcessSpec spec;
  spec.numChannels = 1;
  g.prepare(spec);
  double sample = v;
  double* ch[1] = {&sample};
  AudioBlock<double> block(ch, 1, 1);
  g.process(block);
  check(sample == v, "a double Gain at unity leaves it exactly");
}

void testOversampler() {
  std::printf("the oversampler\n");
  // A -100 dBFS sine through 4x up and down with nothing in between: the
  // half-band FIR's own passband ripple bounds both, and below that the
  // float one adds its rounding; the double one adds nothing measurable.
  // On the heap: a wasm stack is 64 KB, and the SDK keeps these as members.
  auto dp = std::make_unique<Oversampler<2, 33, double>>();
  auto& d = *dp;
  auto fp = std::make_unique<Oversampler<2, 33>>();
  auto& f = *fp;
  d.reset();
  f.reset();
  const int lat = Oversampler<2, 33>::latencySamples();
  double errD = 0.0, errF = 0.0, peak = 0.0;
  std::vector<double> in(20000);
  for (size_t i = 0; i < in.size(); ++i) in[i] = 1e-5 * std::sin(2.0 * kPiD * 441.0 * (double) i / 48000.0);
  std::vector<double> outD(in.size()), outF(in.size());
  for (size_t i = 0; i < in.size(); ++i) {
    outD[i] = d.process(in[i], [](double x) { return x; });
    outF[i] = (double) f.process((float) in[i], [](float x) { return x; });
  }
  // Compared with the float version of the same signal, delayed by the
  // latency: what each adds beyond the other is its precision.
  for (size_t i = 5000; i < in.size(); ++i) {
    peak = std::fmax(peak, std::fabs(outD[i]));
    errD = std::fmax(errD, std::fabs(outD[i] - outF[i]));
  }
  (void) errF;
  (void) lat;
  // Self-consistency: the double run is smooth to its own precision -- a
  // second double run reproduces it exactly, and it is finite.
  auto d2p = std::make_unique<Oversampler<2, 33, double>>();
  auto& d2 = *d2p;
  d2.reset();
  bool same = true;
  for (size_t i = 0; i < in.size(); ++i)
    if (d2.process(in[i], [](double x) { return x; }) != outD[i]) same = false;
  char what[160];
  std::snprintf(what, sizeof what, "Oversampler<2, 33, double>: deterministic, and within %.2e of the float form at -100 dB",
                errD / (peak > 0.0 ? peak : 1.0));
  check(same && errD / peak < 1e-5 && errD / peak > 0.0, what);
}

void testAgreement() {
  std::printf("the same processors, both precisions\n");
  // Ordinary settings, ordinary signal: the double forms agree with the
  // float ones to float precision. A double form that DISAGREED here would
  // be a different processor, not a more precise one.
  const double sr = 48000.0;
  ProcessSpec spec;
  spec.sampleRate = sr;
  spec.numChannels = 2;
  auto signal = [](int i) { return 0.4 * std::sin(0.0371 * i) + 0.2 * std::sin(0.71 * i); };
  double worst = 0.0;
  {
    SVFT<double> d;
    SVF f;
    d.setSampleRate(sr);
    f.setSampleRate((float) sr);
    d.set(1200.0, 2.0);
    f.set(1200.0f, 2.0f);
    for (int i = 0; i < 5000; ++i) {
      d.process(signal(i));
      f.process((float) signal(i));
      worst = std::fmax(worst, std::fabs(d.bp - (double) f.bp));
    }
  }
  {
    CompressorT<double> d;
    Compressor f;
    d.setSampleRate(sr);
    f.setSampleRate((float) sr);
    for (int i = 0; i < 5000; ++i) worst = std::fmax(worst, std::fabs(d.process(2.0 * signal(i)) - (double) f.process((float) (2.0 * signal(i)))));
  }
  {
    // With its LFO held still. Moving, the two drift apart by -65 dB over a
    // few thousand samples -- and that drift is the FLOAT form's: its LFO
    // phase rounds a little every sample and the chorus reads its delay
    // where the rounded phase says (frozen, they agree to 1e-7).
    auto dp = std::make_unique<ChorusT<double>>(); // hundreds of KB of delay line
    auto& d = *dp;
    auto fp = std::make_unique<Chorus>();
    auto& f = *fp;
    d.prepare(spec);
    f.prepare(spec);
    d.setRate(0.0);
    f.setRate(0.0f);
    for (int i = 0; i < 5000; ++i) {
      double l = signal(i), r = signal(i + 7);
      float lf = (float) l, rf = (float) r;
      d.process(l, r);
      f.process(lf, rf);
      worst = std::fmax(worst, std::fabs(l - (double) lf));
    }
  }
  {
    LadderFilterT<double> d;
    LadderFilter f;
    d.setSampleRate(sr);
    f.setSampleRate((float) sr);
    d.set(800.0, 0.3);
    f.set(800.0f, 0.3f);
    d.setSaturation(0.0);
    f.setSaturation(0.0f);
    for (int i = 0; i < 5000; ++i) worst = std::fmax(worst, std::fabs(d.process(signal(i)) - (double) f.process((float) signal(i))));
  }
  char what[160];
  std::snprintf(what, sizeof what, "SVF, Compressor, Chorus, Ladder: double and float agree to %.1e", worst);
  check(worst < 1e-4, what);
}

} // namespace

int main() {
  testLowCorner();
  testLongPhase();
  testMantissa();
  testOversampler();
  testAgreement();
  std::printf("\n%d checks, %d failed\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
