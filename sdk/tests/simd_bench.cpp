// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: does the SIMD path actually pay, and does it change the sound?
//
// Two questions, and the second matters more. A vector path that is faster but
// produces different numbers is not an optimisation, it is a bug that ships a
// different plugin depending on which CPU built it. So this prints a CHECKSUM
// of everything it computes; running it with and without SONORE_NO_SIMD and
// comparing those checksums is the real test. The timings are the other half.
//
//   simd_bench            # vector path
//   simd_bench (built -DSONORE_NO_SIMD)   # scalar reference
#include <sonore/dsp.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

using Clock = std::chrono::steady_clock;

/** A checksum that is sensitive to every sample, not just the last one. */
static double checksum(const std::vector<float>& v) {
  double acc = 0.0;
  for (size_t i = 0; i < v.size(); ++i) acc += (double) v[i] * (double) (1 + (i % 977));
  return acc;
}

static double seconds(Clock::time_point a, Clock::time_point b) {
  return std::chrono::duration<double>(b - a).count();
}

int main() {
  std::printf("Sonore SIMD bench: backend: %s\n\n", sonore::simd::backend());

  const double sr = 48000.0;
  const size_t n = 48000 * 20; // 20 seconds of audio
  std::vector<float> input(n);
  for (size_t i = 0; i < n; ++i)
    input[i] = 0.4f * (float) std::sin(2.0 * 3.14159265358979 * 220.0 * (double) i / sr) +
               0.2f * (float) std::sin(2.0 * 3.14159265358979 * 3300.0 * (double) i / sr);

  // ── Oversampled saturation: the half-band dot product, run hot ────────────
  {
    sonore::Oversampler<2> os; // 4x: two stages up, two down
    os.reset();
    std::vector<float> out(n);
    const auto start = Clock::now();
    for (size_t i = 0; i < n; ++i)
      out[i] = os.process(input[i], [](float x) { return std::tanh(x * 4.0f); });
    const double took = seconds(start, Clock::now());
    std::printf("oversampler 4x : %6.1f x realtime   checksum %.6f\n",
                (double) n / sr / took, checksum(out));
  }

  // ── Convolution: the frequency-domain accumulate, the hottest loop here ───
  {
    // STATIC, not on the stack: this object is ~400 KB of inline partition
    // tables, and WebAssembly gives a function 64 KB. A real plugin puts its
    // DSP in static storage (the wasm ABI does exactly that), so this matches
    // how it is actually used, and is why the same code runs there.
    static sonore::Convolver<256, 32> conv;
    std::vector<float> ir(256 * 32);
    // A decaying noise-ish impulse, deterministic so the checksum is comparable.
    uint32_t seed = 12345;
    for (size_t i = 0; i < ir.size(); ++i) {
      seed = seed * 1664525u + 1013904223u;
      const float noise = (float) ((int32_t) (seed >> 8) % 2000 - 1000) / 1000.0f;
      ir[i] = noise * (float) std::exp(-3.0 * (double) i / (double) ir.size());
    }
    conv.loadImpulse(ir.data(), ir.size());

    std::vector<float> out(n);
    const auto start = Clock::now();
    for (size_t i = 0; i < n; ++i) out[i] = conv.process(input[i]);
    const double took = seconds(start, Clock::now());
    std::printf("convolver 8k   : %6.1f x realtime   checksum %.6f\n",
                (double) n / sr / took, checksum(out));
  }

  // ── The block helpers themselves ──────────────────────────────────────────
  {
    std::vector<float> buffer(input);
    const auto start = Clock::now();
    double guard = 0.0;
    for (int pass = 0; pass < 40; ++pass) {
      sonore::simd::applyGain(buffer.data(), 0.999f, buffer.size());
      guard += sonore::simd::sumSquares(buffer.data(), buffer.size());
      guard += (double) sonore::simd::peakAbs(buffer.data(), buffer.size());
    }
    const double took = seconds(start, Clock::now());
    std::printf("block helpers  : %6.1f Msamples/s   checksum %.6f\n",
                (double) buffer.size() * 40.0 / took / 1e6, guard);
  }

  return 0;
}
