// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: SIMD.
//
// One type, `Vec4f`: four floats processed together. 128-bit / 4-wide is the
// common denominator of every target we ship to: SSE2 on x86-64, NEON on Apple
// Silicon and ARM Windows, and simd128 in WebAssembly. Picking the widest
// available instead (AVX, AVX-512) would mean a different vector width per
// platform, and the browser preview would stop matching the shipped plugin in
// the one dimension this SDK exists to keep identical.
//
// WebAssembly matters here as much as native: the same DSP runs in the studio
// preview, and a toolkit that is only fast natively makes the preview the slow
// path for no reason. emscripten needs `-msimd128`; without it this header
// falls back to scalar and everything still works.
//
// The scalar fallback is not a stub. It is compiled whenever no vector unit is
// available, and `SONORE_NO_SIMD=1` selects it deliberately, which is how the
// tests prove the vector paths produce the SAME NUMBERS as the plain ones. A
// SIMD layer that changes the sound is a bug, not an optimisation.
#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>

// ── Which unit, if any ───────────────────────────────────────────────────────
#if defined(SONORE_NO_SIMD)
#define SONORE_SIMD_SCALAR 1
#elif defined(__wasm_simd128__)
#define SONORE_SIMD_WASM 1
#include <wasm_simd128.h>
#elif defined(__ARM_NEON) || defined(__ARM_NEON__) || defined(__aarch64__) || defined(_M_ARM64)
// MSVC on Windows on Arm defines _M_ARM64 and NOT the ACLE __ARM_NEON, so
// without that last test cl.exe fell through to the scalar path: correct,
// identical numbers, and every block helper running at a quarter of its
// speed on exactly the machines that ship NEON in the base architecture.
#define SONORE_SIMD_NEON 1
#include <arm_neon.h>
#elif defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
// MSVC never defines __SSE2__; on x86-64 SSE2 is guaranteed by the ABI.
#define SONORE_SIMD_SSE 1
#include <emmintrin.h>
#else
#define SONORE_SIMD_SCALAR 1
#endif

namespace sonore {
namespace simd {

/** What actually got compiled, for a test or a log to state rather than guess. */
inline const char* backend() {
#if defined(SONORE_SIMD_WASM)
  return "wasm-simd128";
#elif defined(SONORE_SIMD_NEON)
  return "neon";
#elif defined(SONORE_SIMD_SSE)
  return "sse2";
#else
  return "scalar";
#endif
}

inline constexpr bool vectorised() {
#if defined(SONORE_SIMD_SCALAR)
  return false;
#else
  return true;
#endif
}

/**
 * Four floats, one operation.
 *
 * Deliberately a small, boring surface: load, store, arithmetic, min/max, and a
 * horizontal sum. Everything the toolkit needs is expressible in those, and a
 * bigger surface would be more per-platform code to get subtly wrong.
 */
class Vec4f {
public:
  static constexpr int width() { return 4; }

#if defined(SONORE_SIMD_SSE)
  using Native = __m128;
#elif defined(SONORE_SIMD_NEON)
  using Native = float32x4_t;
#elif defined(SONORE_SIMD_WASM)
  using Native = v128_t;
#else
  struct Native {
    float v[4];
  };
#endif

  Vec4f() = default;
  explicit Vec4f(Native v) : v_(v) {}

  /** Every lane set to the same value. */
  static inline Vec4f splat(float x) {
#if defined(SONORE_SIMD_SSE)
    return Vec4f(_mm_set1_ps(x));
#elif defined(SONORE_SIMD_NEON)
    return Vec4f(vdupq_n_f32(x));
#elif defined(SONORE_SIMD_WASM)
    return Vec4f(wasm_f32x4_splat(x));
#else
    Native n{{x, x, x, x}};
    return Vec4f(n);
#endif
  }

  static inline Vec4f zero() { return splat(0.0f); }

  /** Unaligned load: audio buffers come from the host and alignment is its
   *  business, not ours. The unaligned instruction costs nothing measurable on
   *  any CPU made this decade. */
  static inline Vec4f load(const float* p) {
#if defined(SONORE_SIMD_SSE)
    return Vec4f(_mm_loadu_ps(p));
#elif defined(SONORE_SIMD_NEON)
    return Vec4f(vld1q_f32(p));
#elif defined(SONORE_SIMD_WASM)
    return Vec4f(wasm_v128_load(p));
#else
    Native n{{p[0], p[1], p[2], p[3]}};
    return Vec4f(n);
#endif
  }

  inline void store(float* p) const {
#if defined(SONORE_SIMD_SSE)
    _mm_storeu_ps(p, v_);
#elif defined(SONORE_SIMD_NEON)
    vst1q_f32(p, v_);
#elif defined(SONORE_SIMD_WASM)
    wasm_v128_store(p, v_);
#else
    p[0] = v_.v[0]; p[1] = v_.v[1]; p[2] = v_.v[2]; p[3] = v_.v[3];
#endif
  }

  inline Vec4f operator+(Vec4f o) const {
#if defined(SONORE_SIMD_SSE)
    return Vec4f(_mm_add_ps(v_, o.v_));
#elif defined(SONORE_SIMD_NEON)
    return Vec4f(vaddq_f32(v_, o.v_));
#elif defined(SONORE_SIMD_WASM)
    return Vec4f(wasm_f32x4_add(v_, o.v_));
#else
    Native n{{v_.v[0] + o.v_.v[0], v_.v[1] + o.v_.v[1], v_.v[2] + o.v_.v[2],
              v_.v[3] + o.v_.v[3]}};
    return Vec4f(n);
#endif
  }

  inline Vec4f operator-(Vec4f o) const {
#if defined(SONORE_SIMD_SSE)
    return Vec4f(_mm_sub_ps(v_, o.v_));
#elif defined(SONORE_SIMD_NEON)
    return Vec4f(vsubq_f32(v_, o.v_));
#elif defined(SONORE_SIMD_WASM)
    return Vec4f(wasm_f32x4_sub(v_, o.v_));
#else
    Native n{{v_.v[0] - o.v_.v[0], v_.v[1] - o.v_.v[1], v_.v[2] - o.v_.v[2],
              v_.v[3] - o.v_.v[3]}};
    return Vec4f(n);
#endif
  }

  inline Vec4f operator*(Vec4f o) const {
#if defined(SONORE_SIMD_SSE)
    return Vec4f(_mm_mul_ps(v_, o.v_));
#elif defined(SONORE_SIMD_NEON)
    return Vec4f(vmulq_f32(v_, o.v_));
#elif defined(SONORE_SIMD_WASM)
    return Vec4f(wasm_f32x4_mul(v_, o.v_));
#else
    Native n{{v_.v[0] * o.v_.v[0], v_.v[1] * o.v_.v[1], v_.v[2] * o.v_.v[2],
              v_.v[3] * o.v_.v[3]}};
    return Vec4f(n);
#endif
  }

  inline Vec4f& operator+=(Vec4f o) { *this = *this + o; return *this; }
  inline Vec4f& operator-=(Vec4f o) { *this = *this - o; return *this; }
  inline Vec4f& operator*=(Vec4f o) { *this = *this * o; return *this; }

  inline Vec4f min(Vec4f o) const {
#if defined(SONORE_SIMD_SSE)
    return Vec4f(_mm_min_ps(v_, o.v_));
#elif defined(SONORE_SIMD_NEON)
    return Vec4f(vminq_f32(v_, o.v_));
#elif defined(SONORE_SIMD_WASM)
    return Vec4f(wasm_f32x4_min(v_, o.v_));
#else
    Native n{};
    for (int i = 0; i < 4; ++i) n.v[i] = v_.v[i] < o.v_.v[i] ? v_.v[i] : o.v_.v[i];
    return Vec4f(n);
#endif
  }

  inline Vec4f max(Vec4f o) const {
#if defined(SONORE_SIMD_SSE)
    return Vec4f(_mm_max_ps(v_, o.v_));
#elif defined(SONORE_SIMD_NEON)
    return Vec4f(vmaxq_f32(v_, o.v_));
#elif defined(SONORE_SIMD_WASM)
    return Vec4f(wasm_f32x4_max(v_, o.v_));
#else
    Native n{};
    for (int i = 0; i < 4; ++i) n.v[i] = v_.v[i] > o.v_.v[i] ? v_.v[i] : o.v_.v[i];
    return Vec4f(n);
#endif
  }

  inline Vec4f abs() const {
#if defined(SONORE_SIMD_SSE)
    return Vec4f(_mm_andnot_ps(_mm_set1_ps(-0.0f), v_));
#elif defined(SONORE_SIMD_NEON)
    return Vec4f(vabsq_f32(v_));
#elif defined(SONORE_SIMD_WASM)
    return Vec4f(wasm_f32x4_abs(v_));
#else
    Native n{};
    for (int i = 0; i < 4; ++i) n.v[i] = v_.v[i] < 0.0f ? -v_.v[i] : v_.v[i];
    return Vec4f(n);
#endif
  }

  /** Sum of the four lanes. Used to close a dot product, so it happens once per
   *  loop rather than once per element: a horizontal op inside the loop would
   *  give most of the speedup straight back. */
  inline float sum() const {
#if defined(SONORE_SIMD_SSE)
    __m128 shuf = _mm_shuffle_ps(v_, v_, _MM_SHUFFLE(2, 3, 0, 1));
    __m128 sums = _mm_add_ps(v_, shuf);
    shuf = _mm_movehl_ps(shuf, sums);
    sums = _mm_add_ss(sums, shuf);
    return _mm_cvtss_f32(sums);
#elif defined(SONORE_SIMD_NEON)
#if defined(__aarch64__) || defined(_M_ARM64)
    return vaddvq_f32(v_);
#else
    float32x2_t half = vadd_f32(vget_low_f32(v_), vget_high_f32(v_));
    return vget_lane_f32(vpadd_f32(half, half), 0);
#endif
#elif defined(SONORE_SIMD_WASM)
    return wasm_f32x4_extract_lane(v_, 0) + wasm_f32x4_extract_lane(v_, 1) +
           wasm_f32x4_extract_lane(v_, 2) + wasm_f32x4_extract_lane(v_, 3);
#else
    return v_.v[0] + v_.v[1] + v_.v[2] + v_.v[3];
#endif
  }

  inline float lane(int i) const {
    float tmp[4];
    store(tmp);
    return tmp[i];
  }

private:
  Native v_{};
};

// ── Block helpers ────────────────────────────────────────────────────────────
// These are where the speedup actually lives. A per-sample recursive filter
// cannot be vectorised across time: its next input depends on its last output
//, so the wins come from the places that ARE independent per sample: dot
// products, block gain, and the convolver's frequency-domain accumulation.

/** dot(a, b) over `n` floats. The oversampler's FIR taps and the convolution
 *  partitions both reduce to this. */
inline float dot(const float* a, const float* b, size_t n) {
  size_t i = 0;
  Vec4f acc = Vec4f::zero();
  for (; i + 4 <= n; i += 4) acc += Vec4f::load(a + i) * Vec4f::load(b + i);
  float total = acc.sum();
  for (; i < n; ++i) total += a[i] * b[i];
  return total;
}

/** out[i] += a[i] * b[i]: a fused multiply-accumulate over a whole block. */
inline void multiplyAccumulate(float* out, const float* a, const float* b, size_t n) {
  size_t i = 0;
  for (; i + 4 <= n; i += 4)
    (Vec4f::load(out + i) + Vec4f::load(a + i) * Vec4f::load(b + i)).store(out + i);
  for (; i < n; ++i) out[i] += a[i] * b[i];
}

/** out[i] = a[i] * scalar. */
inline void scale(float* out, const float* a, float scalar, size_t n) {
  size_t i = 0;
  const Vec4f g = Vec4f::splat(scalar);
  for (; i + 4 <= n; i += 4) (Vec4f::load(a + i) * g).store(out + i);
  for (; i < n; ++i) out[i] = a[i] * scalar;
}

/** In-place gain over a block. */
inline void applyGain(float* data, float gain, size_t n) { scale(data, data, gain, n); }

/** The largest |sample| in a block: what a peak meter needs. */
inline float peakAbs(const float* data, size_t n) {
  size_t i = 0;
  Vec4f m = Vec4f::zero();
  for (; i + 4 <= n; i += 4) m = m.max(Vec4f::load(data + i).abs());
  float peak = 0.0f;
  for (int lane = 0; lane < 4; ++lane) {
    const float v = m.lane(lane);
    if (v > peak) peak = v;
  }
  for (; i < n; ++i) {
    const float v = data[i] < 0.0f ? -data[i] : data[i];
    if (v > peak) peak = v;
  }
  return peak;
}

/** Sum of squares, for an RMS meter. */
inline double sumSquares(const float* data, size_t n) {
  size_t i = 0;
  Vec4f acc = Vec4f::zero();
  for (; i + 4 <= n; i += 4) {
    const Vec4f v = Vec4f::load(data + i);
    acc += v * v;
  }
  double total = (double) acc.sum();
  for (; i < n; ++i) total += (double) data[i] * data[i];
  return total;
}

} // namespace simd
} // namespace sonore
