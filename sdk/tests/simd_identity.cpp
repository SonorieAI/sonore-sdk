// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: the SIMD paths produce the same NUMBERS as the scalar one.
//
// simd.h promises that a vector unit changes the speed and never the sound:
// the browser preview (wasm, or scalar without -msimd128) must match the
// shipped plugin bit for bit. simd_bench printed checksums for a person to
// compare by hand. This is built twice -- as is, and with SONORE_NO_SIMD --
// and simd_identity.cmake compares the hashes of the raw float bits each one
// prints: any difference in the last bit of any output fails.
#include <sonore/dsp.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

static uint64_t g_hash = 1469598103934665603ull;

static void mix(const float* v, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    uint32_t bits;
    std::memcpy(&bits, &v[i], sizeof(bits));
    for (int b = 0; b < 4; ++b) {
      g_hash ^= (bits >> (8 * b)) & 0xFFu;
      g_hash *= 1099511628211ull;
    }
  }
}

static void mix(double d) {
  const float f[2] = {(float) d, (float) (d - (double) (float) d)};
  mix(f, 2);
}

// Static: the convolver is hundreds of KB of inline partition tables.
static sonore::Convolver<256, 16> g_conv;

int main() {
  std::printf("backend %s\n", sonore::simd::backend());

  // Deterministic material, irregular enough that every lane carries
  // different magnitudes -- a sum whose order matters.
  std::vector<float> a(4099), b(4099);
  uint32_t seed = 2463534242u;
  for (size_t i = 0; i < a.size(); ++i) {
    seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
    a[i] = (float) ((int32_t) (seed >> 8) % 100000) * 1e-5f * (float) (1 + (i % 7) * 1000);
    seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
    b[i] = (float) ((int32_t) (seed >> 8) % 100000) * 1e-5f / (float) (1 + (i % 5) * 300);
  }

  // The block helpers, at lengths with and without a scalar tail.
  for (size_t n : {4u, 7u, 64u, 255u, 4096u, 4099u}) {
    const float d = sonore::simd::dot(a.data(), b.data(), n);
    mix(&d, 1);
    mix(sonore::simd::sumSquares(a.data(), n));
    const float p = sonore::simd::peakAbs(b.data(), n);
    mix(&p, 1);
    std::vector<float> acc(b.begin(), b.begin() + (long) n), scaled(n);
    sonore::simd::multiplyAccumulate(acc.data(), a.data(), b.data(), n);
    sonore::simd::scale(scaled.data(), a.data(), 0.737f, n);
    mix(acc.data(), n);
    mix(scaled.data(), n);
  }

  // The DSP built on them: the oversampler's half-band dot products and the
  // convolver's frequency-domain accumulate.
  const size_t frames = 24000;
  std::vector<float> in(frames), out(frames);
  for (size_t i = 0; i < frames; ++i)
    in[i] = 0.4f * (float) std::sin(0.0287 * (double) i) + 0.2f * (float) std::sin(0.4309 * (double) i);

  sonore::Oversampler<2> os;
  os.reset();
  for (size_t i = 0; i < frames; ++i) out[i] = os.process(in[i], [](float x) { return std::tanh(x * 4.0f); });
  mix(out.data(), frames);

  std::vector<float> ir(256 * 16);
  for (size_t i = 0; i < ir.size(); ++i) ir[i] = b[i % b.size()] * (float) std::exp(-4.0 * (double) i / (double) ir.size());
  g_conv.loadImpulse(ir.data(), ir.size());
  for (size_t i = 0; i < frames; ++i) out[i] = g_conv.process(in[i]);
  mix(out.data(), frames);

  sonore::FirFilter fir;
  fir.designLowpass(5000.0f, 48000.0f, 127);
  for (size_t i = 0; i < frames; ++i) out[i] = fir.process(in[i]);
  mix(out.data(), frames);

  std::printf("HASH %016llx\n", (unsigned long long) g_hash);
  return 0;
}
