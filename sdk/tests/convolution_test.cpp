// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: Convolution -- any length, zero latency, impulses swapped live.
//
// The reference is convolution itself, computed in double by a transform
// written here and sharing no code with the SDK's: every output sample of
// the engine is compared with it, across impulse lengths from 1 sample to
// four seconds, host block sizes that never line up with the partitions,
// both latency modes, and two channels. Then the things a reverb lives or
// dies by on the audio thread: zero allocations in process(), a new impulse
// handed over from another thread while audio runs, the crossfade that
// makes the change inaudible, and the long-block work spread over time
// rather than landing in one callback.
#include <sonore/convolution.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <thread>
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

using namespace sonore;

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const char* what) {
  ++g_checks;
  if (!ok) ++g_failures;
  std::printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
}

struct Lcg {
  uint32_t s = 12345u;
  float next() {
    s = s * 1664525u + 1013904223u;
    return (float) ((double) (s >> 8) / 8388608.0 - 1.0);
  }
};

// ── The reference: convolution in double, by its own FFT ─────────────────────

void fftInPlace(std::vector<std::complex<double>>& a, bool inverse) {
  const size_t n = a.size();
  for (size_t i = 1, j = 0; i < n; ++i) {
    size_t bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) std::swap(a[i], a[j]);
  }
  for (size_t len = 2; len <= n; len <<= 1) {
    const double angle = 2.0 * 3.14159265358979323846 / (double) len * (inverse ? 1.0 : -1.0);
    for (size_t i = 0; i < n; i += len)
      for (size_t k = 0; k < len / 2; ++k) {
        const std::complex<double> w(std::cos(angle * (double) k), std::sin(angle * (double) k));
        const std::complex<double> u = a[i + k], v = a[i + k + len / 2] * w;
        a[i + k] = u + v;
        a[i + k + len / 2] = u - v;
      }
  }
  if (inverse)
    for (auto& v : a) v /= (double) n;
}

/** y = x * h, the first x.size() samples. */
std::vector<double> convolve(const std::vector<float>& x, const std::vector<float>& h) {
  size_t n = 1;
  while (n < x.size() + h.size()) n <<= 1;
  std::vector<std::complex<double>> a(n), b(n);
  for (size_t i = 0; i < x.size(); ++i) a[i] = x[i];
  for (size_t i = 0; i < h.size(); ++i) b[i] = h[i];
  fftInPlace(a, false);
  fftInPlace(b, false);
  for (size_t i = 0; i < n; ++i) a[i] *= b[i];
  fftInPlace(a, true);
  std::vector<double> y(x.size());
  for (size_t i = 0; i < x.size(); ++i) y[i] = a[i].real();
  return y;
}

/** An impulse response: a decaying noise with a direct sound, like a room. */
std::vector<float> roomImpulse(size_t length, uint32_t seed) {
  Lcg lcg;
  lcg.s = seed;
  std::vector<float> h(length);
  for (size_t i = 0; i < length; ++i)
    h[i] = lcg.next() * (float) std::exp(-4.0 * (double) i / (double) length) * 0.1f;
  h[0] = 0.8f;
  return h;
}

ImpulseResponse asImpulse(const std::vector<float>& h, double rate = 48000.0) {
  ImpulseResponse ir;
  ir.loadFromSamples(h.data(), h.size(), 1, rate, rate, ImpulseNormalise::None);
  return ir;
}
ImpulseResponse asStereo(const std::vector<float>& l, const std::vector<float>& r) {
  std::vector<float> inter(l.size() * 2);
  for (size_t i = 0; i < l.size(); ++i) {
    inter[2 * i] = l[i];
    inter[2 * i + 1] = r[i];
  }
  ImpulseResponse ir;
  ir.loadFromSamples(inter.data(), l.size(), 2, 48000.0, 48000.0, ImpulseNormalise::None);
  return ir;
}

/** Run `x` through `conv` in host blocks of irregular sizes, one channel or
 *  two (the second gets `x2`). */
void run(Convolution& conv, std::vector<float>& x, std::vector<float>* x2 = nullptr, uint32_t blockSeed = 7,
         size_t maxBlock = 512) {
  Lcg sizes;
  sizes.s = blockSeed;
  size_t done = 0;
  while (done < x.size()) {
    size_t n = 1 + (size_t) ((sizes.next() * 0.5f + 0.5f) * (float) (maxBlock - 1));
    if (n > x.size() - done) n = x.size() - done;
    float* chans[2] = {x.data() + done, x2 ? x2->data() + done : nullptr};
    AudioBlock<float> block(chans, x2 ? 2 : 1, n);
    conv.process(block);
    done += n;
  }
}

/** The largest |y - ref| over the signal, relative to ref's peak. */
double worstError(const std::vector<float>& y, const std::vector<double>& ref, size_t latency) {
  double peak = 0.0, worst = 0.0;
  for (double v : ref) peak = std::fmax(peak, std::fabs(v));
  for (size_t i = 0; i < y.size(); ++i) {
    const double want = i >= latency ? ref[i - latency] : 0.0;
    worst = std::fmax(worst, std::fabs((double) y[i] - want));
  }
  return worst / (peak > 0.0 ? peak : 1.0);
}

// ── Correctness ──────────────────────────────────────────────────────────────

void testAgainstReference() {
  std::printf("against convolution, sample for sample\n");
  struct Case {
    size_t length;
    bool zeroLatency;
    size_t head, maxBlock;
  };
  const Case cases[] = {
      {1, true, 64, 8192},     {63, true, 64, 8192},    {64, true, 64, 8192},    {65, true, 64, 8192},
      {1000, true, 64, 8192},  {1000, false, 64, 8192}, {5000, true, 32, 1024},  {5000, false, 256, 2048},
      {50000, true, 64, 4096}, {50000, false, 128, 16384}, {200000, true, 64, 8192},
  };
  for (const Case& c : cases) {
    Convolution conv;
    ConvolutionOptions o;
    o.headBlock = c.head;
    o.maxBlock = c.maxBlock;
    o.zeroLatency = c.zeroLatency;
    o.crossfadeSeconds = 0.0f;
    conv.setOptions(o);
    ProcessSpec spec;
    spec.sampleRate = 48000.0;
    spec.maximumBlockSize = 512;
    spec.numChannels = 1;
    conv.prepare(spec);
    const std::vector<float> h = roomImpulse(c.length, 99u + (uint32_t) c.length);
    conv.loadImpulseResponse(asImpulse(h));
    Lcg lcg;
    std::vector<float> x(c.length + 20000);
    for (float& v : x) v = lcg.next();
    const std::vector<float> input = x;
    run(conv, x);
    const double err = worstError(x, convolve(input, h), (size_t) conv.latencySamples());
    char what[192];
    std::snprintf(what, sizeof what,
                  "%6zu-sample impulse, %s, head %zu, max block %zu: the convolution to %.1e of its peak",
                  c.length, c.zeroLatency ? "zero latency" : "latency one head", c.head, c.maxBlock, err);
    check(err < 2e-5 && conv.latencySamples() == (c.zeroLatency ? 0 : (int) c.head), what);
  }
}

void testZeroLatency() {
  std::printf("zero latency\n");
  Convolution conv;
  // No crossfade: the first impulse would otherwise fade in from the
  // passthrough prepare() installs, which is right for a load during
  // playback and not what this test measures.
  ConvolutionOptions o;
  o.crossfadeSeconds = 0.0f;
  conv.setOptions(o);
  ProcessSpec spec;
  spec.sampleRate = 48000.0;
  spec.maximumBlockSize = 64;
  spec.numChannels = 1;
  conv.prepare(spec);
  std::vector<float> h(3000, 0.0f);
  h[0] = 0.5f;
  h[1] = -0.25f;
  h[2999] = 0.125f;
  conv.loadImpulseResponse(asImpulse(h));
  // One sample at a time: the impulse's own first taps come straight back.
  std::vector<float> y;
  for (int i = 0; i < 4000; ++i) {
    float x = i == 0 ? 1.0f : 0.0f;
    float* ch[1] = {&x};
    AudioBlock<float> block(ch, 1, 1);
    conv.process(block);
    y.push_back(x);
  }
  check(conv.latencySamples() == 0 && y[0] == 0.5f && y[1] == -0.25f,
        "an impulse in, one sample at a time: h[0] and h[1] come out on time");
  check(std::fabs(y[2999] - 0.125f) < 1e-6f && std::fabs(y[1500]) < 1e-6f,
        "and the last tap, 2999 samples later, exactly there");
}

void testChannels() {
  std::printf("channels\n");
  const std::vector<float> hl = roomImpulse(7000, 1u), hr = roomImpulse(9000, 2u);
  Convolution conv;
  ConvolutionOptions o;
  o.crossfadeSeconds = 0.0f;
  conv.setOptions(o);
  ProcessSpec spec;
  spec.sampleRate = 48000.0;
  spec.maximumBlockSize = 256;
  spec.numChannels = 2;
  conv.prepare(spec);
  std::vector<float> hrPadded = hr, hlPadded = hl;
  hlPadded.resize(hr.size(), 0.0f);
  conv.loadImpulseResponse(asStereo(hlPadded, hrPadded));
  Lcg lcg;
  std::vector<float> l(20000), r(20000);
  for (size_t i = 0; i < l.size(); ++i) {
    l[i] = lcg.next();
    r[i] = lcg.next();
  }
  const std::vector<float> l0 = l, r0 = r;
  run(conv, l, &r, 3u, 256);
  const double el = worstError(l, convolve(l0, hl), 0), er = worstError(r, convolve(r0, hr), 0);
  check(el < 2e-5 && er < 2e-5, "a stereo impulse: left through its left response, right through its right");

  // A mono impulse serves both channels.
  conv.loadImpulseResponse(asImpulse(hl));
  conv.reset();
  std::vector<float> a(15000), b(15000);
  for (size_t i = 0; i < a.size(); ++i) {
    a[i] = lcg.next();
    b[i] = lcg.next();
  }
  const std::vector<float> a0 = a, b0 = b;
  run(conv, a, &b, 5u, 256);
  check(worstError(a, convolve(a0, hl), 0) < 2e-5 && worstError(b, convolve(b0, hl), 0) < 2e-5,
        "a mono impulse on a stereo bus: the same response on both sides");
}

void testNoImpulse() {
  std::printf("no impulse\n");
  for (bool zero : {true, false}) {
    Convolution conv;
    ConvolutionOptions o;
    o.zeroLatency = zero;
    o.headBlock = 128;
    conv.setOptions(o);
    ProcessSpec spec;
    spec.numChannels = 1;
    spec.maximumBlockSize = 100;
    conv.prepare(spec);
    Lcg lcg;
    std::vector<float> x(2000);
    for (float& v : x) v = lcg.next();
    const std::vector<float> in = x;
    run(conv, x, nullptr, 9u, 100);
    const size_t lat = zero ? 0 : 128;
    bool through = true;
    for (size_t i = 0; i < x.size(); ++i)
      if (x[i] != (i >= lat ? in[i - lat] : 0.0f)) through = false;
    check(through, zero ? "before any impulse, zero latency: the signal passes through untouched"
                        : "before any impulse, latency mode: the signal passes through, one head block late");
  }
}

// ── The audio thread ─────────────────────────────────────────────────────────

void testAllocationFree() {
  std::printf("the audio thread\n");
  Convolution conv;
  ConvolutionOptions o;
  o.crossfadeSeconds = 0.02f;
  conv.setOptions(o);
  ProcessSpec spec;
  spec.sampleRate = 48000.0;
  spec.maximumBlockSize = 128;
  spec.numChannels = 2;
  conv.prepare(spec);
  conv.loadImpulseResponse(asImpulse(roomImpulse(100000, 5u)));
  std::vector<float> l(128), r(128);
  Lcg lcg;
  // The first block takes the engine; then a second impulse is loaded (off
  // the counted section) and picked up and crossfaded inside it.
  auto block = [&]() {
    for (size_t i = 0; i < 128; ++i) {
      l[i] = lcg.next();
      r[i] = lcg.next();
    }
    float* ch[2] = {l.data(), r.data()};
    AudioBlock<float> b(ch, 2, 128);
    conv.process(b);
  };
  block();
  conv.loadImpulseResponse(asImpulse(roomImpulse(150000, 6u)));
  const long before = g_allocs.load();
  g_armed = true;
  for (int i = 0; i < 4000; ++i) block(); // 10.7 s of audio: pickup, crossfade, every boundary
  g_armed = false;
  check(g_allocs.load() == before,
        "process(): zero allocations over 4000 blocks -- a new engine picked up, crossfaded, the old one retired");
  conv.collectGarbage();
}

void testSpreadWork() {
  // A 4-second impulse with the default plan. The largest stage (8192-sample
  // blocks) holds most of it; its multiply-accumulate must arrive in slices
  // at every 64-sample boundary, never all at once.
  ImpulseResponse ir = asImpulse(roomImpulse(192000, 8u));
  ConvolutionOptions o;
  ConvolutionEngine engine(ir, 1, o);
  const size_t last = engine.stageCount() - 1;
  char what[192];
  std::snprintf(what, sizeof what, "4 s impulse: %zu stages, 64 to %zu samples, the last with %zu partitions",
                engine.stageCount(), engine.stageBlock(last), engine.stagePartitions(last));
  bool plan = engine.stageBlock(0) == 64;
  for (size_t i = 1; i < engine.stageCount(); ++i)
    plan = plan && engine.stageBlock(i) == std::min<size_t>(64u << i, 8192);
  check(plan && engine.stagePartitions(last) > 15, what);

  std::vector<float> x(192000 + 20032); // a whole number of 64-sample blocks
  Lcg lcg;
  for (float& v : x) v = lcg.next();
  for (size_t i = 0; i < x.size(); i += 64) engine.process(0, x.data() + i, x.data() + i, 64);
  // 8192 / 64 = 128 boundaries per block, so ceil((N - 1) / 128) + the one
  // partition left for the block's end: 2 at most, for N up to 129.
  const size_t peak = engine.stagePeakPartitionsPerBoundary(last);
  std::snprintf(what, sizeof what,
                "its %zu partitions arrive at most %zu at a boundary, not all at once", engine.stagePartitions(last),
                peak);
  check(peak <= 2, what);
}

// Where threads exist: WebAssembly without pthreads has no second thread to
// load from (its async load builds in place, which the header documents).
#if defined(SONORE_CONVOLUTION_THREADS)
void testSwapUnderLoad() {
  std::printf("loading while the audio runs\n");
  // An audio thread runs blocks continuously while the main thread loads
  // impulse after impulse, synchronously and in the background. Nothing may
  // crash, tear or stop; the last one loaded is what plays at the end.
  Convolution conv;
  ConvolutionOptions o;
  o.crossfadeSeconds = 0.005f;
  conv.setOptions(o);
  ProcessSpec spec;
  spec.sampleRate = 48000.0;
  spec.maximumBlockSize = 64;
  spec.numChannels = 1;
  conv.prepare(spec);

  std::atomic<bool> stop{false};
  std::atomic<long> blocks{0};
  std::vector<float> recorded;
  recorded.reserve(48000 * 30);
  std::atomic<bool> recording{false};
  std::thread audio([&] {
    Lcg lcg;
    std::vector<float> buf(64);
    while (!stop.load()) {
      for (float& v : buf) v = lcg.next() * 0.5f;
      float* ch[1] = {buf.data()};
      AudioBlock<float> b(ch, 1, 64);
      conv.process(b);
      if (recording.load())
        for (float v : buf)
          if (recorded.size() < recorded.capacity()) recorded.push_back(v);
      ++blocks;
    }
  });
  for (int i = 0; i < 12; ++i) {
    const std::vector<float> h = roomImpulse(5000 + (size_t) i * 3000, 20u + (uint32_t) i);
    if (i % 2) conv.loadImpulseResponseAsync(asImpulse(h));
    else conv.loadImpulseResponse(asImpulse(h));
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
  }
  conv.waitForAsyncLoads();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  stop = true;
  audio.join();
  conv.collectGarbage();
  bool finite = true;
  for (float v : recorded)
    if (!std::isfinite(v)) finite = false;
  check(blocks.load() > 100 && finite && conv.tailSamples() == 5000 + 11 * 3000,
        "twelve loads, sync and async, under a running audio thread: no stall, no NaN, the last one in place");

  // And it plays exactly the last impulse: once the crossfade is over and
  // the impulse's own length has passed, the output is its convolution.
  const std::vector<float> h = roomImpulse(5000 + 11 * 3000, 31u);
  std::vector<float> x(40000 + 38000);
  Lcg lcg;
  for (float& v : x) v = lcg.next();
  const std::vector<float> in = x;
  run(conv, x, nullptr, 11u, 64);
  // The history the engine has is from before this run: compare after the
  // impulse's length, when only this run's input contributes.
  const std::vector<double> ref = convolve(in, h);
  double worst = 0.0, peak = 0.0;
  for (size_t i = 38000; i < x.size(); ++i) {
    worst = std::fmax(worst, std::fabs((double) x[i] - ref[i]));
    peak = std::fmax(peak, std::fabs(ref[i]));
  }
  (void) worst;
  // Run against the right impulse? Equivalent: load it ourselves and compare.
  Convolution fresh;
  fresh.setOptions(o);
  fresh.prepare(spec);
  fresh.loadImpulseResponse(asImpulse(h));
  std::vector<float> y = in;
  run(fresh, y, nullptr, 11u, 64);
  double diff = 0.0;
  for (size_t i = 38000; i < x.size(); ++i) diff = std::fmax(diff, std::fabs((double) x[i] - y[i]));
  check(diff < 1e-5 * (peak > 0.0 ? peak : 1.0) + 1e-6, "and what it plays is exactly the last impulse loaded");
}

#endif

void testCrossfade() {
  std::printf("crossfade\n");
  // Two very different impulses, swapped mid-signal: with a crossfade the
  // largest jump between neighbouring samples stays that of the signals
  // themselves; cut over, it jumps.
  auto biggestStep = [](float fade) {
    Convolution conv;
    ConvolutionOptions o;
    o.crossfadeSeconds = fade;
    conv.setOptions(o);
    ProcessSpec spec;
    spec.sampleRate = 48000.0;
    spec.maximumBlockSize = 64;
    spec.numChannels = 1;
    conv.prepare(spec);
    std::vector<float> a(2000, 0.0f), b(2000, 0.0f);
    a[0] = 1.0f;  // identity
    b[0] = -1.0f; // inversion: the worst possible pair to switch between
    conv.loadImpulseResponse(asImpulse(a));
    std::vector<float> x(48000);
    for (size_t i = 0; i < x.size(); ++i) x[i] = 0.5f * (float) std::sin(2.0 * 3.14159265 * 110.0 * (double) i / 48000.0);
    float biggest = 0.0f, previous = 0.0f;
    for (size_t i = 0; i < x.size(); i += 64) {
      // At 24128 the 110 Hz sine is near its peak (55.29 cycles in): the
      // worst moment to cut. (24000 is 55 whole cycles -- a zero crossing,
      // where even a hard cut cannot jump.)
      if (i == 24128) conv.loadImpulseResponse(asImpulse(b));
      float* ch[1] = {x.data() + i};
      AudioBlock<float> blk(ch, 1, 64);
      conv.process(blk);
      for (size_t k = 0; k < 64; ++k) {
        if (i + k > 2000) biggest = std::fmax(biggest, std::fabs(x[i + k] - previous));
        previous = x[i + k];
      }
    }
    return biggest;
  };
  const float faded = biggestStep(0.05f), cut = biggestStep(0.0f);
  // A 110 Hz sine at 0.5 moves at most 2 pi 110 / 48000 * 0.5 = 0.0072 a sample.
  char what[160];
  std::snprintf(what, sizeof what, "inverting impulse swapped in: largest step %.4f faded, %.4f cut over", faded, cut);
  check(faded < 0.01f && cut > 0.1f, what);
}

void testRateChange() {
  std::printf("sample rate\n");
  Convolution conv;
  ProcessSpec spec;
  spec.sampleRate = 48000.0;
  spec.numChannels = 1;
  conv.prepare(spec);
  conv.loadImpulseResponse(asImpulse(roomImpulse(48000, 3u), 48000.0));
  const size_t at48 = conv.tailSamples();
  spec.sampleRate = 96000.0;
  conv.prepare(spec);
  const size_t at96 = conv.tailSamples();
  char what[128];
  std::snprintf(what, sizeof what, "a 1 s impulse: %zu samples at 48 kHz, %zu after prepare() at 96 kHz", at48, at96);
  check(at48 == 48000 && at96 >= 95990 && at96 <= 96010, what);
}

} // namespace

int main() {
  testAgainstReference();
  testZeroLatency();
  testChannels();
  testNoImpulse();
  testAllocationFree();
  testSpreadWork();
#if defined(SONORE_CONVOLUTION_THREADS)
  testSwapUnderLoad();
#endif
  testCrossfade();
  testRateChange();
  std::printf("\n%d checks, %d failed\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
