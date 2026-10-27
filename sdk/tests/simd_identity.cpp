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

  // The wider types: Vec8f and Vec4d are one AVX register or two 128-bit
  // ones, Vec2d a double register or two doubles, Vec4i four ints -- and
  // every one must give the scalar build's bits. A dot product through each
  // float type, so the documented sum order is in the hash, and every other
  // operation once over the material.
  {
    using namespace sonore::simd;
    Vec8f acc8 = Vec8f::zero();
    for (size_t i = 0; i + 8 <= 4096; i += 8) {
      const Vec8f x = Vec8f::load(a.data() + i), y = Vec8f::load(b.data() + i);
      acc8 += x * y;
      Vec8f r = (x.abs() / (y.abs() + Vec8f::splat(1.0f))).sqrt();
      r = Vec8f::select(x.lessThan(y), (r.min)(x), (r.max)(y)) - x;
      float lanes[8];
      r.store(lanes);
      mix(lanes, 8);
    }
    const float dot8 = acc8.sum();
    mix(&dot8, 1);

    std::vector<double> da(a.begin(), a.end()), db(b.begin(), b.end());
    Vec4d acc4 = Vec4d::zero();
    Vec2d acc2 = Vec2d::zero();
    for (size_t i = 0; i + 4 <= 4096; i += 4) {
      const Vec4d x = Vec4d::load(da.data() + i), y = Vec4d::load(db.data() + i);
      acc4 += x * y;
      acc2 += Vec2d::load(da.data() + i) / (Vec2d::load(db.data() + i).abs() + Vec2d::splat(0.5));
      const Vec4d r = Vec4d::select(x.greaterThan(y), (x - y).sqrt(), (x.min)(y) * y);
      mix(r.lane(0));
      mix(r.lane(3));
    }
    mix(acc4.sum());
    mix(acc2.sum());

    Vec4i state = Vec4i::splat(0x2545F491);
    for (int i = 0; i < 1000; ++i) {
      // An xorshift per lane, then a multiply-low and a convert: integer
      // lanes through every operation the type has.
      state = state ^ state.shiftLeft(13);
      state = state ^ state.shiftRightLogical(17);
      state = state ^ state.shiftLeft(5);
      const Vec4i m = (state * Vec4i::splat(1103515245) + Vec4i::splat(12345)).shiftRight(3);
      float lanes[4];
      ((m.max)(state.shiftRight(1)) - (m.min)(state)).toFloat().store(lanes);
      mix(lanes, 4);
    }
    const float isum = (float) state.sum();
    mix(&isum, 1);
  }

  // The narrower and wider integer lanes, the unsigned forms, the widths
  // between them, and the complex lanes: every operation they have, chained
  // so that one wrong lane anywhere changes everything after it.
  {
    using namespace sonore::simd;
    Vec4i s32 = Vec4i::splat(0x2545F491);
    Vec8i16 s16 = Vec8i16::splat(0x1234);
    Vec16i8 s8 = Vec16i8::splat(0x5A);
    Vec2i64 s64 = Vec2i64::splat(0x0123456789ABCDEFll);
    for (int i = 0; i < 500; ++i) {
      s32 = s32 ^ s32.shiftLeft(13);
      s32 = s32 ^ s32.shiftRightLogical(17);
      s32 = s32 ^ s32.shiftLeft(5);
      Vec8i16 w = narrowSaturate(s32, s32.shiftRight(i % 32));
      s16 = (s16 * w + Vec8i16::splat((int16_t) i)).addSaturate(w.shiftRight(i % 16)) ^ w.shiftLeft(i);
      s16 = Vec8i16::select(s16.lessThanUnsigned(w), s16.subSaturateUnsigned(w), (s16.maxUnsigned)(w) - (s16.min)(w));
      s8 = (s8 * narrowSaturate(s16, w)).subSaturate(Vec16i8::splat((int8_t) i)) + s8.shiftRightLogical(i);
      s8 = Vec16i8::select(s8.greaterThan(Vec16i8::zero()), s8.addSaturateUnsigned(s8), (s8.minUnsigned)(s8.shiftRight(1)));
      s64 = s64 * Vec2i64::splat(6364136223846793005ll) + widenLow(s32) - widenHigh(s32).shiftRight(i);
      s64 = Vec2i64::select(s64.lessThan(Vec2i64::zero()), s64.shiftRightLogical(i % 64), (s64.max)(s64 ^ s64.shiftLeft(7)));
      Vec4i lo, hi;
      widen(s16, lo, hi);
      Vec8i16 b0, b1;
      widen(s8, b0, b1);
      float lanes[4];
      ((lo + hi).minUnsigned(s32) + (s32.maxUnsigned(lo)).shiftRight(3)).toFloat().store(lanes);
      mix(lanes, 4);
      const Vec2d d = toDouble(s64 ^ Vec2i64::splat((int64_t) (b0 - b1).sum()));
      mix(d.lane(0));
      mix(d.lane(1));
    }
    const float isum = (float) s8.sum() + (float) s16.sum() + (float) s64.sum() + (float) s32.sum();
    mix(&isum, 1);

    Vec4cf zf = Vec4cf::splat({0.9f, 0.1f});
    Vec4cd zd = Vec4cd::splat({0.9, 0.1});
    std::complex<float> cf[4];
    std::complex<double> cd[4];
    for (size_t i = 0; i + 8 <= 4096; i += 8) {
      for (int k = 0; k < 4; ++k) {
        cf[k] = std::complex<float>(a[i + 2 * k], b[i + 2 * k + 1]);
        cd[k] = std::complex<double>(b[i + 2 * k], a[i + 2 * k + 1]);
      }
      const Vec4cf xf = Vec4cf::load(cf);
      const Vec4cd xd = Vec4cd::load(cd);
      zf = (zf * xf.conj() + xf).scale(Vec4f::splat(0.5f)) - Vec4cf(xf.normSquared(), xf.imag());
      zd = (zd * xd.conj() + xd).scale(Vec4d::splat(0.5)) - Vec4cd(xd.normSquared(), xd.imag());
      zf.store(cf);
      zd.store(cd);
      for (int k = 0; k < 4; ++k) {
        const float pair[2] = {cf[k].real(), cf[k].imag()};
        mix(pair, 2);
        mix(cd[k].real());
        mix(cd[k].imag());
      }
    }
    const std::complex<float> sf = zf.sum();
    const std::complex<double> sd = zd.sum();
    const float sums[2] = {sf.real(), sf.imag()};
    mix(sums, 2);
    mix(sd.real());
    mix(sd.imag());
  }

  // The real-signal transforms, whose packing passes run four pairs to a
  // vector: Fft and DynamicFft, forward and back.
  {
    static sonore::Fft<1024> fixed;
    sonore::DynamicFft dynamic(2048);
    std::vector<float> re(1025), im(1025), back(2048);
    fixed.realForward(a.data(), re.data(), im.data());
    mix(re.data(), 513);
    mix(im.data(), 513);
    fixed.realInverse(re.data(), im.data(), back.data());
    mix(back.data(), 1024);
    dynamic.realForward(b.data(), re.data(), im.data());
    mix(re.data(), 1025);
    mix(im.data(), 1025);
    dynamic.realInverse(re.data(), im.data(), back.data());
    mix(back.data(), 2048);
  }

  std::printf("wide %s\n", sonore::simd::wideBackend());
  std::printf("HASH %016llx\n", (unsigned long long) g_hash);
  return 0;
}
