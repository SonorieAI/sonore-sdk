// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: SIMD.
//
// `Vec4f`: four floats processed together, the type the toolkit is written in.
// 128-bit / 4-wide is the common denominator of every target we ship to: SSE2
// on x86-64, NEON on Apple Silicon and ARM Windows, and simd128 in
// WebAssembly. Picking the widest available instead (AVX, AVX-512) for it
// would mean a different vector width per platform, and the browser preview
// would stop matching the shipped plugin in the one dimension this SDK exists
// to keep identical.
//
// Beside it, for code that wants more: `Vec8f` (eight floats -- one AVX
// register where AVX is targeted, two 128-bit ones elsewhere), `Vec2d` and
// `Vec4d` (doubles), the integer lanes `Vec16i8`, `Vec8i16`, `Vec4i` and
// `Vec2i64` (every width a 128-bit register holds, each readable as signed or
// unsigned, with saturating forms for the narrow ones and conversions
// between the widths), and `Vec4cf` / `Vec4cd`, four complex numbers in
// split form. They keep the same promise by construction rather than by
// width: every operation is lane by lane and correctly rounded, and the one
// that crosses lanes, sum(), has one order for every width, so AVX and two
// SSE registers give the same bits.
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
#include <complex>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

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

// AVX widens Vec8f and Vec4d into one register where the compiler was told
// it may use it (-mavx, /arch:AVX); it changes the speed of those two types
// and nothing else -- see "Wider registers" below for why not the numbers.
#if defined(SONORE_SIMD_SSE) && defined(__AVX__)
#define SONORE_SIMD_AVX 1
#include <immintrin.h>
#endif
// 64-bit ARM has double lanes, a divide and a square root in NEON; 32-bit ARM
// has none of the three, and those go a lane at a time there.
#if defined(SONORE_SIMD_NEON) && (defined(__aarch64__) || defined(_M_ARM64))
#define SONORE_SIMD_NEON64 1
#endif

namespace sonore {
namespace simd {

namespace simddetail {
// Masks on the scalar build: a lane of all ones or all zeros, as the vector
// compares produce, held in the lane's own type so select() is bitwise there
// too.
inline float maskF(bool b) {
  const uint32_t u = b ? 0xFFFFFFFFu : 0u;
  float f;
  std::memcpy(&f, &u, sizeof f);
  return f;
}
inline float selectBits(float mask, float a, float b) {
  uint32_t m, x, y;
  std::memcpy(&m, &mask, 4);
  std::memcpy(&x, &a, 4);
  std::memcpy(&y, &b, 4);
  const uint32_t r = (m & x) | (~m & y);
  float out;
  std::memcpy(&out, &r, 4);
  return out;
}
inline double maskD(bool b) {
  const uint64_t u = b ? ~uint64_t(0) : uint64_t(0);
  double d;
  std::memcpy(&d, &u, sizeof d);
  return d;
}
inline double selectBitsD(double mask, double a, double b) {
  uint64_t m, x, y;
  std::memcpy(&m, &mask, 8);
  std::memcpy(&x, &a, 8);
  std::memcpy(&y, &b, 8);
  const uint64_t r = (m & x) | (~m & y);
  double out;
  std::memcpy(&out, &r, 8);
  return out;
}
} // namespace simddetail

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

/** True when a vector unit (SSE2, NEON or wasm simd128) was compiled in,
 *  false on the scalar fallback. constexpr, so `if constexpr` can branch on it. */
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
  /** Lanes per vector: 4 on every backend, scalar included. */
  static constexpr int width() { return 4; }

#if defined(SONORE_SIMD_SSE)
  using Native = __m128;
#elif defined(SONORE_SIMD_NEON)
  using Native = float32x4_t;
#elif defined(SONORE_SIMD_WASM)
  using Native = v128_t;
#else
  /** The register on a scalar build (SONORE_NO_SIMD defined, or a target
   *  none of the three backends covers): four plain floats, which every
   *  operation loops over. With SSE, NEON or wasm SIMD, Native is that
   *  platform's own vector type instead. */
  struct Native {
    float v[4]; ///< the four lanes, lane 0 first
  };
#endif

  /** All four lanes 0: the register is value-initialised. */
  Vec4f() = default;
  /** Wraps the platform's own vector, for code that mixes intrinsics with this
   *  type. */
  explicit Vec4f(Native v) : v_(v) {}
  /** The platform's register, for code that mixes intrinsics with this
   *  type (the struct of plain lanes on the scalar build). */
  Native native() const { return v_; }

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

  /** Every lane 0.0f; the start of an accumulator. */
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

  /** Writes the four lanes to `p[0..3]`, lane 0 first. Unaligned, like load();
   *  `p` must have room for four floats. */
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

  /** Lane-by-lane sum. */
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

  /** Lane-by-lane difference, this minus `o`. */
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

  /** Lane-by-lane product. */
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

  /** In-place lane-by-lane sum. */
  inline Vec4f& operator+=(Vec4f o) { *this = *this + o; return *this; }
  /** In-place lane-by-lane difference. */
  inline Vec4f& operator-=(Vec4f o) { *this = *this - o; return *this; }
  /** In-place lane-by-lane product. */
  inline Vec4f& operator*=(Vec4f o) { *this = *this * o; return *this; }

  /** a < b ? a : b, lane by lane -- on EVERY backend, NaN included. SSE's
   *  minps is exactly that (a NaN in either lane gives the second operand),
   *  and so is the scalar fallback; NEON's vminq and wasm's f32x4.min instead
   *  return NaN when either is NaN. A NaN then stuck in its lane for the
   *  rest of a reduction -- peakAbs() hid every later peak in that lane on
   *  ARM and in the browser, and only there. Compare-and-select agrees. */
  // (min) and (max), in parentheses here and where they are called: a
  // windows.h included without NOMINMAX before this header defines both as
  // macros, and `min(` is then the macro, not this. Including <windows.h>
  // first broke dsp.h and every header that reaches this one, 17 errors deep
  // in a file the user never touched.
  inline Vec4f (min)(Vec4f o) const {
#if defined(SONORE_SIMD_SSE)
    return Vec4f(_mm_min_ps(v_, o.v_));
#elif defined(SONORE_SIMD_NEON)
    return Vec4f(vbslq_f32(vcltq_f32(v_, o.v_), v_, o.v_));
#elif defined(SONORE_SIMD_WASM)
    return Vec4f(wasm_v128_bitselect(v_, o.v_, wasm_f32x4_lt(v_, o.v_)));
#else
    Native n{};
    for (int i = 0; i < 4; ++i) n.v[i] = v_.v[i] < o.v_.v[i] ? v_.v[i] : o.v_.v[i];
    return Vec4f(n);
#endif
  }

  /** a > b ? a : b, lane by lane, on every backend -- see min(). */
  inline Vec4f (max)(Vec4f o) const {
#if defined(SONORE_SIMD_SSE)
    return Vec4f(_mm_max_ps(v_, o.v_));
#elif defined(SONORE_SIMD_NEON)
    return Vec4f(vbslq_f32(vcgtq_f32(v_, o.v_), v_, o.v_));
#elif defined(SONORE_SIMD_WASM)
    return Vec4f(wasm_v128_bitselect(v_, o.v_, wasm_f32x4_gt(v_, o.v_)));
#else
    Native n{};
    for (int i = 0; i < 4; ++i) n.v[i] = v_.v[i] > o.v_.v[i] ? v_.v[i] : o.v_.v[i];
    return Vec4f(n);
#endif
  }

  /** The magnitude of each lane. */
  inline Vec4f abs() const {
#if defined(SONORE_SIMD_SSE)
    return Vec4f(_mm_andnot_ps(_mm_set1_ps(-0.0f), v_));
#elif defined(SONORE_SIMD_NEON)
    return Vec4f(vabsq_f32(v_));
#elif defined(SONORE_SIMD_WASM)
    return Vec4f(wasm_f32x4_abs(v_));
#else
    // The sign bit cleared, as andnot, vabs and f32x4.abs all do. This was
    // `v < 0 ? -v : v`, which leaves -0.0 negative (and a negative NaN
    // negative): the scalar build, which is the preview without -msimd128,
    // disagreed with every vector unit in that bit.
    Native n{};
    for (int i = 0; i < 4; ++i) n.v[i] = std::fabs(v_.v[i]);
    return Vec4f(n);
#endif
  }

  /** Sum of the four lanes. Used to close a dot product, so it happens once per
   *  loop rather than once per element: a horizontal op inside the loop would
   *  give most of the speedup straight back.
   *
   *  (l0 + l1) + (l2 + l3) on EVERY backend. Float addition does not
   *  associate, so the order is part of the answer: SSE and AArch64's
   *  vaddvq pair the lanes this way, and the scalar and wasm paths summed
   *  left to right (32-bit NEON paired l0 with l2) -- every dot product
   *  differed in its last bits between the preview and the plugin. */
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
    const float32x2_t pairs = vpadd_f32(vget_low_f32(v_), vget_high_f32(v_));
    return vget_lane_f32(pairs, 0) + vget_lane_f32(pairs, 1);
#endif
#elif defined(SONORE_SIMD_WASM)
    return (wasm_f32x4_extract_lane(v_, 0) + wasm_f32x4_extract_lane(v_, 1)) +
           (wasm_f32x4_extract_lane(v_, 2) + wasm_f32x4_extract_lane(v_, 3));
#else
    return (v_.v[0] + v_.v[1]) + (v_.v[2] + v_.v[3]);
#endif
  }

  /** Lane `i`, 0 to 3; not bounds-checked. It goes through memory, so it is
   *  for reading a result after a loop, not for use inside one. */
  inline float lane(int i) const {
    float tmp[4];
    store(tmp);
    return tmp[i];
  }

  /** Lane-by-lane quotient, this over `o`. IEEE division is correctly
   *  rounded on every unit, so it agrees with the scalar build bit for bit;
   *  32-bit ARM, whose NEON has no divide, does it a lane at a time. */
  inline Vec4f operator/(Vec4f o) const {
#if defined(SONORE_SIMD_SSE)
    return Vec4f(_mm_div_ps(v_, o.v_));
#elif defined(SONORE_SIMD_NEON64)
    return Vec4f(vdivq_f32(v_, o.v_));
#elif defined(SONORE_SIMD_WASM)
    return Vec4f(wasm_f32x4_div(v_, o.v_));
#else
    return perLane(o, [](float a, float b) { return a / b; });
#endif
  }
  /** In-place lane-by-lane quotient. */
  inline Vec4f& operator/=(Vec4f o) { *this = *this / o; return *this; }

  /** The square root of each lane: correctly rounded everywhere, so the same
   *  bits as std::sqrt on a float. */
  inline Vec4f sqrt() const {
#if defined(SONORE_SIMD_SSE)
    return Vec4f(_mm_sqrt_ps(v_));
#elif defined(SONORE_SIMD_NEON64)
    return Vec4f(vsqrtq_f32(v_));
#elif defined(SONORE_SIMD_WASM)
    return Vec4f(wasm_f32x4_sqrt(v_));
#else
    return perLane(*this, [](float a, float) { return std::sqrt(a); });
#endif
  }

  /** A MASK: every bit of a lane set where this < o, clear where not (NaN
   *  compares false). Feed it to select(); its lanes are not numbers. */
  inline Vec4f lessThan(Vec4f o) const {
#if defined(SONORE_SIMD_SSE)
    return Vec4f(_mm_cmplt_ps(v_, o.v_));
#elif defined(SONORE_SIMD_NEON)
    return Vec4f(vreinterpretq_f32_u32(vcltq_f32(v_, o.v_)));
#elif defined(SONORE_SIMD_WASM)
    return Vec4f(wasm_f32x4_lt(v_, o.v_));
#else
    return perLane(o, [](float a, float b) { return simddetail::maskF(a < b); });
#endif
  }
  /** The mask of this > o, as lessThan(). */
  inline Vec4f greaterThan(Vec4f o) const { return o.lessThan(*this); }
  /** The mask of this == o (NaN is never equal), as lessThan(). */
  inline Vec4f equalTo(Vec4f o) const {
#if defined(SONORE_SIMD_SSE)
    return Vec4f(_mm_cmpeq_ps(v_, o.v_));
#elif defined(SONORE_SIMD_NEON)
    return Vec4f(vreinterpretq_f32_u32(vceqq_f32(v_, o.v_)));
#elif defined(SONORE_SIMD_WASM)
    return Vec4f(wasm_f32x4_eq(v_, o.v_));
#else
    return perLane(o, [](float a, float b) { return simddetail::maskF(a == b); });
#endif
  }

  /** mask ? a : b, lane by lane, bit for bit: `mask` from lessThan() and
   *  friends. The branch-free form of a per-sample `if`. */
  static inline Vec4f select(Vec4f mask, Vec4f a, Vec4f b) {
#if defined(SONORE_SIMD_SSE)
    return Vec4f(_mm_or_ps(_mm_and_ps(mask.v_, a.v_), _mm_andnot_ps(mask.v_, b.v_)));
#elif defined(SONORE_SIMD_NEON)
    return Vec4f(vbslq_f32(vreinterpretq_u32_f32(mask.v_), a.v_, b.v_));
#elif defined(SONORE_SIMD_WASM)
    return Vec4f(wasm_v128_bitselect(a.v_, b.v_, mask.v_));
#else
    Native n{};
    for (int i = 0; i < 4; ++i)
      n.v[i] = simddetail::selectBits(mask.v_.v[i], a.v_.v[i], b.v_.v[i]);
    return Vec4f(n);
#endif
  }

  // ── Moving lanes, never changing them ──
  // Pure data movement, so the same bits on every backend by construction:
  // what an FFT's packing passes and a mirrored loop need.

  /** The lanes in reverse order: {l3, l2, l1, l0}. */
  inline Vec4f reversed() const {
#if defined(SONORE_SIMD_SSE)
    return Vec4f(_mm_shuffle_ps(v_, v_, _MM_SHUFFLE(0, 1, 2, 3)));
#elif defined(SONORE_SIMD_NEON)
    const float32x4_t r = vrev64q_f32(v_);
    return Vec4f(vcombine_f32(vget_high_f32(r), vget_low_f32(r)));
#elif defined(SONORE_SIMD_WASM)
    return Vec4f(wasm_i32x4_shuffle(v_, v_, 3, 2, 1, 0));
#else
    Native n{{v_.v[3], v_.v[2], v_.v[1], v_.v[0]}};
    return Vec4f(n);
#endif
  }

  /** Eight interleaved floats p[0..7] split into the even ones and the odd
   *  ones: even = {p0, p2, p4, p6}, odd = {p1, p3, p5, p7}. */
  static inline void deinterleave(const float* p, Vec4f& even, Vec4f& odd) {
#if defined(SONORE_SIMD_SSE)
    const __m128 a = _mm_loadu_ps(p), b = _mm_loadu_ps(p + 4);
    even = Vec4f(_mm_shuffle_ps(a, b, _MM_SHUFFLE(2, 0, 2, 0)));
    odd = Vec4f(_mm_shuffle_ps(a, b, _MM_SHUFFLE(3, 1, 3, 1)));
#elif defined(SONORE_SIMD_NEON)
    const float32x4x2_t v = vld2q_f32(p);
    even = Vec4f(v.val[0]);
    odd = Vec4f(v.val[1]);
#elif defined(SONORE_SIMD_WASM)
    const v128_t a = wasm_v128_load(p), b = wasm_v128_load(p + 4);
    even = Vec4f(wasm_i32x4_shuffle(a, b, 0, 2, 4, 6));
    odd = Vec4f(wasm_i32x4_shuffle(a, b, 1, 3, 5, 7));
#else
    even = Vec4f(Native{{p[0], p[2], p[4], p[6]}});
    odd = Vec4f(Native{{p[1], p[3], p[5], p[7]}});
#endif
  }

  /** The inverse: a's lanes and b's interleaved into out[0..7] as
   *  {a0, b0, a1, b1, a2, b2, a3, b3}. */
  static inline void interleave(Vec4f a, Vec4f b, float* out) {
#if defined(SONORE_SIMD_SSE)
    _mm_storeu_ps(out, _mm_unpacklo_ps(a.v_, b.v_));
    _mm_storeu_ps(out + 4, _mm_unpackhi_ps(a.v_, b.v_));
#elif defined(SONORE_SIMD_NEON)
    float32x4x2_t v;
    v.val[0] = a.v_;
    v.val[1] = b.v_;
    vst2q_f32(out, v);
#elif defined(SONORE_SIMD_WASM)
    wasm_v128_store(out, wasm_i32x4_shuffle(a.v_, b.v_, 0, 4, 1, 5));
    wasm_v128_store(out + 4, wasm_i32x4_shuffle(a.v_, b.v_, 2, 6, 3, 7));
#else
    for (int i = 0; i < 4; ++i) {
      out[2 * i] = a.v_.v[i];
      out[2 * i + 1] = b.v_.v[i];
    }
#endif
  }

private:
  // A lane at a time through memory: the scalar build, and the two
  // operations 32-bit NEON lacks.
  template <typename F>
  inline Vec4f perLane(Vec4f o, F&& f) const {
    float a[4], b[4], r[4];
    store(a);
    o.store(b);
    for (int i = 0; i < 4; ++i) r[i] = f(a[i], b[i]);
    return load(r);
  }

  Native v_{};
};

// ── Wider registers, doubles, integers ───────────────────────────────────────
//
// Vec4f stays the type the toolkit is written in. These are for a plugin
// (or a part of the toolkit) that wants more than four floats at once, double
// precision, or integer lanes -- the rest of what a SIMD layer is asked for.
//
// They keep Vec4f's one promise: the SAME BITS on every backend. A wider
// register is not a different computation, only more lanes of the same one:
// every operation here is lane by lane and correctly rounded (+ - * / sqrt are
// IEEE everywhere, min and max are the compare-and-select Vec4f defines), so
// eight floats in one AVX register and eight floats in two SSE, NEON or wasm
// registers produce identical lanes. The one operation that crosses lanes,
// sum(), is defined in ONE order for every width -- Vec8f's is the two
// halves' Vec4f sums, added -- so a dot product over Vec8f gives the same
// answer whether the machine has AVX or not, and whether the code runs in the
// shipped plugin or in the browser. simd_identity checks all of it.

/**
 * Eight floats, one operation: AVX where the compiler targets it (-mavx,
 * /arch:AVX), two Vec4f everywhere else -- SSE2, NEON, wasm simd128 and the
 * scalar build alike. The interface is Vec4f's, lane for lane.
 */
class Vec8f {
public:
  /** Lanes per vector: 8 on every backend. */
  static constexpr int width() { return 8; }

  Vec8f() = default;

#if defined(SONORE_SIMD_AVX)
  /** Wraps an AVX register. */
  explicit Vec8f(__m256 v) : v_(v) {}
#else
  /** Two Vec4f halves, lanes 0-3 and 4-7. */
  Vec8f(Vec4f lo, Vec4f hi) : lo_(lo), hi_(hi) {}
#endif

  /** Every lane set to `x`. */
  static inline Vec8f splat(float x) {
#if defined(SONORE_SIMD_AVX)
    return Vec8f(_mm256_set1_ps(x));
#else
    return Vec8f(Vec4f::splat(x), Vec4f::splat(x));
#endif
  }
  /** Every lane 0. */
  static inline Vec8f zero() { return splat(0.0f); }
  /** Unaligned load of p[0..7]. */
  static inline Vec8f load(const float* p) {
#if defined(SONORE_SIMD_AVX)
    return Vec8f(_mm256_loadu_ps(p));
#else
    return Vec8f(Vec4f::load(p), Vec4f::load(p + 4));
#endif
  }
  /** Unaligned store to p[0..7], lane 0 first. */
  inline void store(float* p) const {
#if defined(SONORE_SIMD_AVX)
    _mm256_storeu_ps(p, v_);
#else
    lo_.store(p);
    hi_.store(p + 4);
#endif
  }

#if defined(SONORE_SIMD_AVX)
#define SONORE_V8_BINARY(op, avx, half)                                         \
  inline Vec8f operator op(Vec8f o) const { return Vec8f(avx(v_, o.v_)); }
#else
#define SONORE_V8_BINARY(op, avx, half)                                         \
  inline Vec8f operator op(Vec8f o) const { return Vec8f(lo_ op o.lo_, hi_ op o.hi_); }
#endif
  /** Lane-by-lane sum. */
  SONORE_V8_BINARY(+, _mm256_add_ps, +)
  /** Lane-by-lane difference. */
  SONORE_V8_BINARY(-, _mm256_sub_ps, -)
  /** Lane-by-lane product. */
  SONORE_V8_BINARY(*, _mm256_mul_ps, *)
  /** Lane-by-lane quotient (IEEE, as Vec4f's). */
  SONORE_V8_BINARY(/, _mm256_div_ps, /)
#undef SONORE_V8_BINARY

  /** In-place sum. */
  inline Vec8f& operator+=(Vec8f o) { *this = *this + o; return *this; }
  /** In-place difference. */
  inline Vec8f& operator-=(Vec8f o) { *this = *this - o; return *this; }
  /** In-place product. */
  inline Vec8f& operator*=(Vec8f o) { *this = *this * o; return *this; }
  /** In-place quotient. */
  inline Vec8f& operator/=(Vec8f o) { *this = *this / o; return *this; }

  /** a < b ? a : b per lane, NaN as Vec4f's min() has it. */
  inline Vec8f (min)(Vec8f o) const {
#if defined(SONORE_SIMD_AVX)
    return Vec8f(_mm256_min_ps(v_, o.v_));
#else
    return Vec8f((lo_.min)(o.lo_), (hi_.min)(o.hi_));
#endif
  }
  /** a > b ? a : b per lane, as max() on Vec4f. */
  inline Vec8f (max)(Vec8f o) const {
#if defined(SONORE_SIMD_AVX)
    return Vec8f(_mm256_max_ps(v_, o.v_));
#else
    return Vec8f((lo_.max)(o.lo_), (hi_.max)(o.hi_));
#endif
  }
  /** |lane|. */
  inline Vec8f abs() const {
#if defined(SONORE_SIMD_AVX)
    return Vec8f(_mm256_andnot_ps(_mm256_set1_ps(-0.0f), v_));
#else
    return Vec8f(lo_.abs(), hi_.abs());
#endif
  }
  /** sqrt(lane), correctly rounded. */
  inline Vec8f sqrt() const {
#if defined(SONORE_SIMD_AVX)
    return Vec8f(_mm256_sqrt_ps(v_));
#else
    return Vec8f(lo_.sqrt(), hi_.sqrt());
#endif
  }
  /** The mask of this < o, as Vec4f's. */
  inline Vec8f lessThan(Vec8f o) const {
#if defined(SONORE_SIMD_AVX)
    return Vec8f(_mm256_cmp_ps(v_, o.v_, _CMP_LT_OQ));
#else
    return Vec8f(lo_.lessThan(o.lo_), hi_.lessThan(o.hi_));
#endif
  }
  /** The mask of this > o. */
  inline Vec8f greaterThan(Vec8f o) const { return o.lessThan(*this); }
  /** The mask of this == o. */
  inline Vec8f equalTo(Vec8f o) const {
#if defined(SONORE_SIMD_AVX)
    return Vec8f(_mm256_cmp_ps(v_, o.v_, _CMP_EQ_OQ));
#else
    return Vec8f(lo_.equalTo(o.lo_), hi_.equalTo(o.hi_));
#endif
  }
  /** mask ? a : b, lane by lane. */
  static inline Vec8f select(Vec8f mask, Vec8f a, Vec8f b) {
#if defined(SONORE_SIMD_AVX)
    return Vec8f(_mm256_or_ps(_mm256_and_ps(mask.v_, a.v_), _mm256_andnot_ps(mask.v_, b.v_)));
#else
    return Vec8f(Vec4f::select(mask.lo_, a.lo_, b.lo_), Vec4f::select(mask.hi_, a.hi_, b.hi_));
#endif
  }

  /** The sum of the eight lanes, in ONE order on every backend: the low
   *  half's Vec4f sum plus the high half's, each ((l0 + l1) + (l2 + l3)). */
  inline float sum() const {
#if defined(SONORE_SIMD_AVX)
    return Vec4f(_mm256_castps256_ps128(v_)).sum() + Vec4f(_mm256_extractf128_ps(v_, 1)).sum();
#else
    return lo_.sum() + hi_.sum();
#endif
  }
  /** Lane `i`, 0 to 7; through memory, for after a loop. */
  inline float lane(int i) const {
    float tmp[8];
    store(tmp);
    return tmp[i];
  }

private:
#if defined(SONORE_SIMD_AVX)
  __m256 v_{};
#else
  Vec4f lo_, hi_;
#endif
};

/**
 * Two doubles, one operation: SSE2, AArch64 NEON and wasm simd128 all have
 * a double register this size; 32-bit ARM and the scalar build use two plain
 * doubles. For the work that needs double's 53 bits -- long accumulations, a
 * low corner at a high rate, a phase that must run for hours -- at vector
 * speed. Lane by lane IEEE, sum() is l0 + l1.
 */
class Vec2d {
public:
  /** Lanes per vector: 2. */
  static constexpr int width() { return 2; }

#if defined(SONORE_SIMD_SSE)
  using Native = __m128d;
#elif defined(SONORE_SIMD_NEON64)
  using Native = float64x2_t;
#elif defined(SONORE_SIMD_WASM)
  using Native = v128_t;
#else
  /** Two plain doubles where no double register exists. */
  struct Native {
    double v[2]; ///< lane 0, lane 1
  };
#endif

  Vec2d() = default;
  /** Wraps the platform's register. */
  explicit Vec2d(Native v) : v_(v) {}
  /** The platform's register, for code that mixes intrinsics with this
   *  type (the struct of plain lanes on the scalar build). */
  Native native() const { return v_; }

  /** Both lanes `x`. */
  static inline Vec2d splat(double x) {
#if defined(SONORE_SIMD_SSE)
    return Vec2d(_mm_set1_pd(x));
#elif defined(SONORE_SIMD_NEON64)
    return Vec2d(vdupq_n_f64(x));
#elif defined(SONORE_SIMD_WASM)
    return Vec2d(wasm_f64x2_splat(x));
#else
    return Vec2d(Native{{x, x}});
#endif
  }
  /** Both lanes 0. */
  static inline Vec2d zero() { return splat(0.0); }
  /** Unaligned load of p[0..1]. */
  static inline Vec2d load(const double* p) {
#if defined(SONORE_SIMD_SSE)
    return Vec2d(_mm_loadu_pd(p));
#elif defined(SONORE_SIMD_NEON64)
    return Vec2d(vld1q_f64(p));
#elif defined(SONORE_SIMD_WASM)
    return Vec2d(wasm_v128_load(p));
#else
    return Vec2d(Native{{p[0], p[1]}});
#endif
  }
  /** Unaligned store to p[0..1]. */
  inline void store(double* p) const {
#if defined(SONORE_SIMD_SSE)
    _mm_storeu_pd(p, v_);
#elif defined(SONORE_SIMD_NEON64)
    vst1q_f64(p, v_);
#elif defined(SONORE_SIMD_WASM)
    wasm_v128_store(p, v_);
#else
    p[0] = v_.v[0];
    p[1] = v_.v[1];
#endif
  }

#if defined(SONORE_SIMD_SSE)
#define SONORE_V2D_BINARY(op, sse, neon, wasm)                                  \
  inline Vec2d operator op(Vec2d o) const { return Vec2d(sse(v_, o.v_)); }
#elif defined(SONORE_SIMD_NEON64)
#define SONORE_V2D_BINARY(op, sse, neon, wasm)                                  \
  inline Vec2d operator op(Vec2d o) const { return Vec2d(neon(v_, o.v_)); }
#elif defined(SONORE_SIMD_WASM)
#define SONORE_V2D_BINARY(op, sse, neon, wasm)                                  \
  inline Vec2d operator op(Vec2d o) const { return Vec2d(wasm(v_, o.v_)); }
#else
#define SONORE_V2D_BINARY(op, sse, neon, wasm)                                  \
  inline Vec2d operator op(Vec2d o) const {                                     \
    return Vec2d(Native{{v_.v[0] op o.v_.v[0], v_.v[1] op o.v_.v[1]}});         \
  }
#endif
  /** Lane-by-lane sum. */
  SONORE_V2D_BINARY(+, _mm_add_pd, vaddq_f64, wasm_f64x2_add)
  /** Lane-by-lane difference. */
  SONORE_V2D_BINARY(-, _mm_sub_pd, vsubq_f64, wasm_f64x2_sub)
  /** Lane-by-lane product. */
  SONORE_V2D_BINARY(*, _mm_mul_pd, vmulq_f64, wasm_f64x2_mul)
  /** Lane-by-lane quotient. */
  SONORE_V2D_BINARY(/, _mm_div_pd, vdivq_f64, wasm_f64x2_div)
#undef SONORE_V2D_BINARY

  /** In-place sum. */
  inline Vec2d& operator+=(Vec2d o) { *this = *this + o; return *this; }
  /** In-place difference. */
  inline Vec2d& operator-=(Vec2d o) { *this = *this - o; return *this; }
  /** In-place product. */
  inline Vec2d& operator*=(Vec2d o) { *this = *this * o; return *this; }
  /** In-place quotient. */
  inline Vec2d& operator/=(Vec2d o) { *this = *this / o; return *this; }

  /** The mask of this < o: every bit of a lane set, or none. */
  inline Vec2d lessThan(Vec2d o) const {
#if defined(SONORE_SIMD_SSE)
    return Vec2d(_mm_cmplt_pd(v_, o.v_));
#elif defined(SONORE_SIMD_NEON64)
    return Vec2d(vreinterpretq_f64_u64(vcltq_f64(v_, o.v_)));
#elif defined(SONORE_SIMD_WASM)
    return Vec2d(wasm_f64x2_lt(v_, o.v_));
#else
    return Vec2d(Native{{simddetail::maskD(v_.v[0] < o.v_.v[0]), simddetail::maskD(v_.v[1] < o.v_.v[1])}});
#endif
  }
  /** The mask of this > o. */
  inline Vec2d greaterThan(Vec2d o) const { return o.lessThan(*this); }
  /** The mask of this == o. */
  inline Vec2d equalTo(Vec2d o) const {
#if defined(SONORE_SIMD_SSE)
    return Vec2d(_mm_cmpeq_pd(v_, o.v_));
#elif defined(SONORE_SIMD_NEON64)
    return Vec2d(vreinterpretq_f64_u64(vceqq_f64(v_, o.v_)));
#elif defined(SONORE_SIMD_WASM)
    return Vec2d(wasm_f64x2_eq(v_, o.v_));
#else
    return Vec2d(Native{{simddetail::maskD(v_.v[0] == o.v_.v[0]), simddetail::maskD(v_.v[1] == o.v_.v[1])}});
#endif
  }
  /** mask ? a : b, lane by lane. */
  static inline Vec2d select(Vec2d mask, Vec2d a, Vec2d b) {
#if defined(SONORE_SIMD_SSE)
    return Vec2d(_mm_or_pd(_mm_and_pd(mask.v_, a.v_), _mm_andnot_pd(mask.v_, b.v_)));
#elif defined(SONORE_SIMD_NEON64)
    return Vec2d(vbslq_f64(vreinterpretq_u64_f64(mask.v_), a.v_, b.v_));
#elif defined(SONORE_SIMD_WASM)
    return Vec2d(wasm_v128_bitselect(a.v_, b.v_, mask.v_));
#else
    return Vec2d(Native{{simddetail::selectBitsD(mask.v_.v[0], a.v_.v[0], b.v_.v[0]),
                         simddetail::selectBitsD(mask.v_.v[1], a.v_.v[1], b.v_.v[1])}});
#endif
  }
  /** a < b ? a : b per lane, NaN included, as Vec4f's. */
  inline Vec2d (min)(Vec2d o) const { return select(lessThan(o), *this, o); }
  /** a > b ? a : b per lane. */
  inline Vec2d (max)(Vec2d o) const { return select(greaterThan(o), *this, o); }
  /** |lane|. */
  inline Vec2d abs() const {
#if defined(SONORE_SIMD_SSE)
    return Vec2d(_mm_andnot_pd(_mm_set1_pd(-0.0), v_));
#elif defined(SONORE_SIMD_NEON64)
    return Vec2d(vabsq_f64(v_));
#elif defined(SONORE_SIMD_WASM)
    return Vec2d(wasm_f64x2_abs(v_));
#else
    return Vec2d(Native{{std::fabs(v_.v[0]), std::fabs(v_.v[1])}});
#endif
  }
  /** sqrt(lane), correctly rounded. */
  inline Vec2d sqrt() const {
#if defined(SONORE_SIMD_SSE)
    return Vec2d(_mm_sqrt_pd(v_));
#elif defined(SONORE_SIMD_NEON64)
    return Vec2d(vsqrtq_f64(v_));
#elif defined(SONORE_SIMD_WASM)
    return Vec2d(wasm_f64x2_sqrt(v_));
#else
    return Vec2d(Native{{std::sqrt(v_.v[0]), std::sqrt(v_.v[1])}});
#endif
  }
  /** l0 + l1. */
  inline double sum() const { return lane(0) + lane(1); }
  /** Lane `i`, 0 or 1. */
  inline double lane(int i) const {
    double tmp[2];
    store(tmp);
    return tmp[i];
  }

private:
  Native v_{};
};

/**
 * Four doubles: AVX where it is targeted, two Vec2d everywhere else. sum()
 * is the halves' sums added, (l0 + l1) + (l2 + l3), on every backend.
 */
class Vec4d {
public:
  /** Lanes per vector: 4. */
  static constexpr int width() { return 4; }

  Vec4d() = default;
#if defined(SONORE_SIMD_AVX)
  /** Wraps an AVX register. */
  explicit Vec4d(__m256d v) : v_(v) {}
#else
  /** Two Vec2d halves, lanes 0-1 and 2-3. */
  Vec4d(Vec2d lo, Vec2d hi) : lo_(lo), hi_(hi) {}
#endif

  /** Every lane `x`. */
  static inline Vec4d splat(double x) {
#if defined(SONORE_SIMD_AVX)
    return Vec4d(_mm256_set1_pd(x));
#else
    return Vec4d(Vec2d::splat(x), Vec2d::splat(x));
#endif
  }
  /** Every lane 0. */
  static inline Vec4d zero() { return splat(0.0); }
  /** Unaligned load of p[0..3]. */
  static inline Vec4d load(const double* p) {
#if defined(SONORE_SIMD_AVX)
    return Vec4d(_mm256_loadu_pd(p));
#else
    return Vec4d(Vec2d::load(p), Vec2d::load(p + 2));
#endif
  }
  /** Unaligned store to p[0..3]. */
  inline void store(double* p) const {
#if defined(SONORE_SIMD_AVX)
    _mm256_storeu_pd(p, v_);
#else
    lo_.store(p);
    hi_.store(p + 2);
#endif
  }

#if defined(SONORE_SIMD_AVX)
#define SONORE_V4D_BINARY(op, avx)                                              \
  inline Vec4d operator op(Vec4d o) const { return Vec4d(avx(v_, o.v_)); }
#else
#define SONORE_V4D_BINARY(op, avx)                                              \
  inline Vec4d operator op(Vec4d o) const { return Vec4d(lo_ op o.lo_, hi_ op o.hi_); }
#endif
  /** Lane-by-lane sum. */
  SONORE_V4D_BINARY(+, _mm256_add_pd)
  /** Lane-by-lane difference. */
  SONORE_V4D_BINARY(-, _mm256_sub_pd)
  /** Lane-by-lane product. */
  SONORE_V4D_BINARY(*, _mm256_mul_pd)
  /** Lane-by-lane quotient. */
  SONORE_V4D_BINARY(/, _mm256_div_pd)
#undef SONORE_V4D_BINARY

  /** In-place sum. */
  inline Vec4d& operator+=(Vec4d o) { *this = *this + o; return *this; }
  /** In-place difference. */
  inline Vec4d& operator-=(Vec4d o) { *this = *this - o; return *this; }
  /** In-place product. */
  inline Vec4d& operator*=(Vec4d o) { *this = *this * o; return *this; }
  /** In-place quotient. */
  inline Vec4d& operator/=(Vec4d o) { *this = *this / o; return *this; }

  /** The mask of this < o. */
  inline Vec4d lessThan(Vec4d o) const {
#if defined(SONORE_SIMD_AVX)
    return Vec4d(_mm256_cmp_pd(v_, o.v_, _CMP_LT_OQ));
#else
    return Vec4d(lo_.lessThan(o.lo_), hi_.lessThan(o.hi_));
#endif
  }
  /** The mask of this > o. */
  inline Vec4d greaterThan(Vec4d o) const { return o.lessThan(*this); }
  /** The mask of this == o. */
  inline Vec4d equalTo(Vec4d o) const {
#if defined(SONORE_SIMD_AVX)
    return Vec4d(_mm256_cmp_pd(v_, o.v_, _CMP_EQ_OQ));
#else
    return Vec4d(lo_.equalTo(o.lo_), hi_.equalTo(o.hi_));
#endif
  }
  /** mask ? a : b, lane by lane. */
  static inline Vec4d select(Vec4d mask, Vec4d a, Vec4d b) {
#if defined(SONORE_SIMD_AVX)
    return Vec4d(_mm256_or_pd(_mm256_and_pd(mask.v_, a.v_), _mm256_andnot_pd(mask.v_, b.v_)));
#else
    return Vec4d(Vec2d::select(mask.lo_, a.lo_, b.lo_), Vec2d::select(mask.hi_, a.hi_, b.hi_));
#endif
  }
  /** a < b ? a : b per lane. */
  inline Vec4d (min)(Vec4d o) const { return select(lessThan(o), *this, o); }
  /** a > b ? a : b per lane. */
  inline Vec4d (max)(Vec4d o) const { return select(greaterThan(o), *this, o); }
  /** |lane|. */
  inline Vec4d abs() const {
#if defined(SONORE_SIMD_AVX)
    return Vec4d(_mm256_andnot_pd(_mm256_set1_pd(-0.0), v_));
#else
    return Vec4d(lo_.abs(), hi_.abs());
#endif
  }
  /** sqrt(lane), correctly rounded. */
  inline Vec4d sqrt() const {
#if defined(SONORE_SIMD_AVX)
    return Vec4d(_mm256_sqrt_pd(v_));
#else
    return Vec4d(lo_.sqrt(), hi_.sqrt());
#endif
  }
  /** (l0 + l1) + (l2 + l3), on every backend. */
  inline double sum() const {
    double tmp[4];
    store(tmp);
    return (tmp[0] + tmp[1]) + (tmp[2] + tmp[3]);
  }
  /** Lane `i`, 0 to 3. */
  inline double lane(int i) const {
    double tmp[4];
    store(tmp);
    return tmp[i];
  }

private:
#if defined(SONORE_SIMD_AVX)
  __m256d v_{};
#else
  Vec2d lo_, hi_;
#endif
};

/**
 * Four 32-bit integers, one operation: for bit work, counters, table indices,
 * noise generators and anything else a float lane cannot hold exactly.
 *
 * Arithmetic WRAPS (two's complement, modulo 2^32) on every backend,
 * including the scalar one, where it is done in unsigned arithmetic because
 * signed overflow in C++ is undefined rather than wrapping. Shift counts are
 * taken modulo 32, which is what wasm does natively and the others are made
 * to match: an x86 shift by 32 clears the lane, an ARM one by a negative
 * count shifts the other way, and a count that meant different things on
 * different machines would be exactly the divergence this header exists to
 * prevent. SSE2 has no 32-bit multiply-low and no 32-bit min or max (those
 * arrived with SSE4.1); both are built from what SSE2 has.
 */
class Vec4i {
public:
  /** Lanes per vector: 4. */
  static constexpr int width() { return 4; }

#if defined(SONORE_SIMD_SSE)
  using Native = __m128i;
#elif defined(SONORE_SIMD_NEON)
  using Native = int32x4_t;
#elif defined(SONORE_SIMD_WASM)
  using Native = v128_t;
#else
  /** Four plain int32 on the scalar build. */
  struct Native {
    int32_t v[4]; ///< lane 0 first
  };
#endif

  Vec4i() = default;
  /** Wraps the platform's register. */
  explicit Vec4i(Native v) : v_(v) {}
  /** The platform's register, for code that mixes intrinsics with this
   *  type (the struct of plain lanes on the scalar build). */
  Native native() const { return v_; }

  /** Every lane `x`. */
  static inline Vec4i splat(int32_t x) {
#if defined(SONORE_SIMD_SSE)
    return Vec4i(_mm_set1_epi32(x));
#elif defined(SONORE_SIMD_NEON)
    return Vec4i(vdupq_n_s32(x));
#elif defined(SONORE_SIMD_WASM)
    return Vec4i(wasm_i32x4_splat(x));
#else
    return Vec4i(Native{{x, x, x, x}});
#endif
  }
  /** Every lane 0. */
  static inline Vec4i zero() { return splat(0); }
  /** Unaligned load of p[0..3]. */
  static inline Vec4i load(const int32_t* p) {
#if defined(SONORE_SIMD_SSE)
    return Vec4i(_mm_loadu_si128(reinterpret_cast<const __m128i*>(p)));
#elif defined(SONORE_SIMD_NEON)
    return Vec4i(vld1q_s32(p));
#elif defined(SONORE_SIMD_WASM)
    return Vec4i(wasm_v128_load(p));
#else
    return Vec4i(Native{{p[0], p[1], p[2], p[3]}});
#endif
  }
  /** Unaligned store to p[0..3]. */
  inline void store(int32_t* p) const {
#if defined(SONORE_SIMD_SSE)
    _mm_storeu_si128(reinterpret_cast<__m128i*>(p), v_);
#elif defined(SONORE_SIMD_NEON)
    vst1q_s32(p, v_);
#elif defined(SONORE_SIMD_WASM)
    wasm_v128_store(p, v_);
#else
    for (int i = 0; i < 4; ++i) p[i] = v_.v[i];
#endif
  }

  /** Lane-by-lane sum, wrapping. */
  inline Vec4i operator+(Vec4i o) const {
#if defined(SONORE_SIMD_SSE)
    return Vec4i(_mm_add_epi32(v_, o.v_));
#elif defined(SONORE_SIMD_NEON)
    return Vec4i(vaddq_s32(v_, o.v_));
#elif defined(SONORE_SIMD_WASM)
    return Vec4i(wasm_i32x4_add(v_, o.v_));
#else
    return perLane(o, [](uint32_t a, uint32_t b) { return a + b; });
#endif
  }
  /** Lane-by-lane difference, wrapping. */
  inline Vec4i operator-(Vec4i o) const {
#if defined(SONORE_SIMD_SSE)
    return Vec4i(_mm_sub_epi32(v_, o.v_));
#elif defined(SONORE_SIMD_NEON)
    return Vec4i(vsubq_s32(v_, o.v_));
#elif defined(SONORE_SIMD_WASM)
    return Vec4i(wasm_i32x4_sub(v_, o.v_));
#else
    return perLane(o, [](uint32_t a, uint32_t b) { return a - b; });
#endif
  }
  /** Lane-by-lane product, the low 32 bits (wrapping). */
  inline Vec4i operator*(Vec4i o) const {
#if defined(SONORE_SIMD_SSE)
    // Lanes 0 and 2, then 1 and 3, as 64-bit products, whose low halves are
    // the answer: SSE2's only 32-bit multiply is the widening one.
    const __m128i even = _mm_mul_epu32(v_, o.v_);
    const __m128i odd = _mm_mul_epu32(_mm_srli_si128(v_, 4), _mm_srli_si128(o.v_, 4));
    return Vec4i(_mm_unpacklo_epi32(_mm_shuffle_epi32(even, _MM_SHUFFLE(0, 0, 2, 0)),
                                    _mm_shuffle_epi32(odd, _MM_SHUFFLE(0, 0, 2, 0))));
#elif defined(SONORE_SIMD_NEON)
    return Vec4i(vmulq_s32(v_, o.v_));
#elif defined(SONORE_SIMD_WASM)
    return Vec4i(wasm_i32x4_mul(v_, o.v_));
#else
    return perLane(o, [](uint32_t a, uint32_t b) { return a * b; });
#endif
  }
  /** In-place sum. */
  inline Vec4i& operator+=(Vec4i o) { *this = *this + o; return *this; }
  /** In-place difference. */
  inline Vec4i& operator-=(Vec4i o) { *this = *this - o; return *this; }
  /** In-place product. */
  inline Vec4i& operator*=(Vec4i o) { *this = *this * o; return *this; }

  /** Bitwise and. */
  inline Vec4i operator&(Vec4i o) const {
#if defined(SONORE_SIMD_SSE)
    return Vec4i(_mm_and_si128(v_, o.v_));
#elif defined(SONORE_SIMD_NEON)
    return Vec4i(vandq_s32(v_, o.v_));
#elif defined(SONORE_SIMD_WASM)
    return Vec4i(wasm_v128_and(v_, o.v_));
#else
    return perLane(o, [](uint32_t a, uint32_t b) { return a & b; });
#endif
  }
  /** Bitwise or. */
  inline Vec4i operator|(Vec4i o) const {
#if defined(SONORE_SIMD_SSE)
    return Vec4i(_mm_or_si128(v_, o.v_));
#elif defined(SONORE_SIMD_NEON)
    return Vec4i(vorrq_s32(v_, o.v_));
#elif defined(SONORE_SIMD_WASM)
    return Vec4i(wasm_v128_or(v_, o.v_));
#else
    return perLane(o, [](uint32_t a, uint32_t b) { return a | b; });
#endif
  }
  /** Bitwise exclusive or. */
  inline Vec4i operator^(Vec4i o) const {
#if defined(SONORE_SIMD_SSE)
    return Vec4i(_mm_xor_si128(v_, o.v_));
#elif defined(SONORE_SIMD_NEON)
    return Vec4i(veorq_s32(v_, o.v_));
#elif defined(SONORE_SIMD_WASM)
    return Vec4i(wasm_v128_xor(v_, o.v_));
#else
    return perLane(o, [](uint32_t a, uint32_t b) { return a ^ b; });
#endif
  }

  /** Every lane shifted left by `n` modulo 32. */
  inline Vec4i shiftLeft(int n) const {
    n &= 31;
#if defined(SONORE_SIMD_SSE)
    return Vec4i(_mm_sll_epi32(v_, _mm_cvtsi32_si128(n)));
#elif defined(SONORE_SIMD_NEON)
    return Vec4i(vshlq_s32(v_, vdupq_n_s32(n)));
#elif defined(SONORE_SIMD_WASM)
    return Vec4i(wasm_i32x4_shl(v_, (uint32_t) n));
#else
    return perLane(*this, [n](uint32_t a, uint32_t) { return a << n; });
#endif
  }
  /** Every lane shifted right by `n` modulo 32, ARITHMETICALLY: the sign bit
   *  is copied in, so -8 >> 1 is -4. */
  inline Vec4i shiftRight(int n) const {
    n &= 31;
#if defined(SONORE_SIMD_SSE)
    return Vec4i(_mm_sra_epi32(v_, _mm_cvtsi32_si128(n)));
#elif defined(SONORE_SIMD_NEON)
    return Vec4i(vshlq_s32(v_, vdupq_n_s32(-n)));
#elif defined(SONORE_SIMD_WASM)
    return Vec4i(wasm_i32x4_shr(v_, (uint32_t) n));
#else
    // Arithmetic shift spelled out, since >> on a negative int was only
    // pinned down by C++20: the low bits shifted, the sign bits filled in.
    return perLane(*this, [n](uint32_t a, uint32_t) {
      const uint32_t fill = (a & 0x80000000u) && n ? ~(0xFFFFFFFFu >> n) : 0u;
      return (a >> n) | fill;
    });
#endif
  }
  /** Every lane shifted right by `n` modulo 32, LOGICALLY: zeros in. */
  inline Vec4i shiftRightLogical(int n) const {
    n &= 31;
#if defined(SONORE_SIMD_SSE)
    return Vec4i(_mm_srl_epi32(v_, _mm_cvtsi32_si128(n)));
#elif defined(SONORE_SIMD_NEON)
    return Vec4i(vreinterpretq_s32_u32(vshlq_u32(vreinterpretq_u32_s32(v_), vdupq_n_s32(-n))));
#elif defined(SONORE_SIMD_WASM)
    return Vec4i(wasm_u32x4_shr(v_, (uint32_t) n));
#else
    return perLane(*this, [n](uint32_t a, uint32_t) { return a >> n; });
#endif
  }

  /** The mask of this < o, signed: all bits of a lane set, or none. */
  inline Vec4i lessThan(Vec4i o) const {
#if defined(SONORE_SIMD_SSE)
    return Vec4i(_mm_cmplt_epi32(v_, o.v_));
#elif defined(SONORE_SIMD_NEON)
    return Vec4i(vreinterpretq_s32_u32(vcltq_s32(v_, o.v_)));
#elif defined(SONORE_SIMD_WASM)
    return Vec4i(wasm_i32x4_lt(v_, o.v_));
#else
    Native n{};
    for (int i = 0; i < 4; ++i) n.v[i] = v_.v[i] < o.v_.v[i] ? -1 : 0;
    return Vec4i(n);
#endif
  }
  /** The mask of this > o, signed. */
  inline Vec4i greaterThan(Vec4i o) const { return o.lessThan(*this); }
  /** The mask of this == o. */
  inline Vec4i equalTo(Vec4i o) const {
#if defined(SONORE_SIMD_SSE)
    return Vec4i(_mm_cmpeq_epi32(v_, o.v_));
#elif defined(SONORE_SIMD_NEON)
    return Vec4i(vreinterpretq_s32_u32(vceqq_s32(v_, o.v_)));
#elif defined(SONORE_SIMD_WASM)
    return Vec4i(wasm_i32x4_eq(v_, o.v_));
#else
    Native n{};
    for (int i = 0; i < 4; ++i) n.v[i] = v_.v[i] == o.v_.v[i] ? -1 : 0;
    return Vec4i(n);
#endif
  }
  /** mask ? a : b, bit by bit. */
  static inline Vec4i select(Vec4i mask, Vec4i a, Vec4i b) { return (mask & a) | andNot(mask, b); }
  /** a < b ? a : b per lane, signed. */
  inline Vec4i (min)(Vec4i o) const {
#if defined(SONORE_SIMD_NEON)
    return Vec4i(vminq_s32(v_, o.v_));
#elif defined(SONORE_SIMD_WASM)
    return Vec4i(wasm_i32x4_min(v_, o.v_));
#else
    return select(lessThan(o), *this, o);
#endif
  }
  /** a > b ? a : b per lane, signed. */
  inline Vec4i (max)(Vec4i o) const {
#if defined(SONORE_SIMD_NEON)
    return Vec4i(vmaxq_s32(v_, o.v_));
#elif defined(SONORE_SIMD_WASM)
    return Vec4i(wasm_i32x4_max(v_, o.v_));
#else
    return select(greaterThan(o), *this, o);
#endif
  }

  /** The mask of this < o with both read as UNSIGNED: 0xFFFFFFFF is the
   *  largest, not -1. The register does not know which it holds; only the
   *  comparisons, min/max and the right shift care, and each has both. */
  inline Vec4i lessThanUnsigned(Vec4i o) const {
#if defined(SONORE_SIMD_SSE)
    // SSE2 compares signed only: flipping the sign bit of both maps the
    // unsigned order onto the signed one.
    const __m128i flip = _mm_set1_epi32((int) 0x80000000u);
    return Vec4i(_mm_cmplt_epi32(_mm_xor_si128(v_, flip), _mm_xor_si128(o.v_, flip)));
#elif defined(SONORE_SIMD_NEON)
    return Vec4i(vreinterpretq_s32_u32(vcltq_u32(vreinterpretq_u32_s32(v_), vreinterpretq_u32_s32(o.v_))));
#elif defined(SONORE_SIMD_WASM)
    return Vec4i(wasm_u32x4_lt(v_, o.v_));
#else
    Native n{};
    for (int i = 0; i < 4; ++i) n.v[i] = (uint32_t) v_.v[i] < (uint32_t) o.v_.v[i] ? -1 : 0;
    return Vec4i(n);
#endif
  }
  /** The mask of this > o, unsigned. */
  inline Vec4i greaterThanUnsigned(Vec4i o) const { return o.lessThanUnsigned(*this); }
  /** a < b ? a : b per lane, unsigned. */
  inline Vec4i minUnsigned(Vec4i o) const {
#if defined(SONORE_SIMD_NEON)
    return Vec4i(vreinterpretq_s32_u32(vminq_u32(vreinterpretq_u32_s32(v_), vreinterpretq_u32_s32(o.v_))));
#elif defined(SONORE_SIMD_WASM)
    return Vec4i(wasm_u32x4_min(v_, o.v_));
#else
    return select(lessThanUnsigned(o), *this, o);
#endif
  }
  /** a > b ? a : b per lane, unsigned. */
  inline Vec4i maxUnsigned(Vec4i o) const {
#if defined(SONORE_SIMD_NEON)
    return Vec4i(vreinterpretq_s32_u32(vmaxq_u32(vreinterpretq_u32_s32(v_), vreinterpretq_u32_s32(o.v_))));
#elif defined(SONORE_SIMD_WASM)
    return Vec4i(wasm_u32x4_max(v_, o.v_));
#else
    return select(greaterThanUnsigned(o), *this, o);
#endif
  }

  /** Each lane converted to float, rounded to nearest: exact below 2^24,
   *  and the same rounding above it on every unit. */
  inline Vec4f toFloat() const {
#if defined(SONORE_SIMD_SSE)
    return Vec4f(_mm_cvtepi32_ps(v_));
#elif defined(SONORE_SIMD_NEON)
    return Vec4f(vcvtq_f32_s32(v_));
#elif defined(SONORE_SIMD_WASM)
    return Vec4f(wasm_f32x4_convert_i32x4(v_));
#else
    float tmp[4];
    for (int i = 0; i < 4; ++i) tmp[i] = (float) v_.v[i];
    return Vec4f::load(tmp);
#endif
  }

  /** The four lanes summed, wrapping: integer addition associates, so there
   *  is no order to define. */
  inline int32_t sum() const {
    int32_t tmp[4];
    store(tmp);
    const uint32_t s = (uint32_t) tmp[0] + (uint32_t) tmp[1] + (uint32_t) tmp[2] + (uint32_t) tmp[3];
    return (int32_t) s;
  }
  /** Lane `i`, 0 to 3. */
  inline int32_t lane(int i) const {
    int32_t tmp[4];
    store(tmp);
    return tmp[i];
  }

private:
  static inline Vec4i andNot(Vec4i mask, Vec4i b) {
#if defined(SONORE_SIMD_SSE)
    return Vec4i(_mm_andnot_si128(mask.v_, b.v_));
#elif defined(SONORE_SIMD_NEON)
    return Vec4i(vbicq_s32(b.v_, mask.v_));
#elif defined(SONORE_SIMD_WASM)
    return Vec4i(wasm_v128_andnot(b.v_, mask.v_));
#else
    return mask.perLane(b, [](uint32_t m, uint32_t x) { return ~m & x; });
#endif
  }
#if defined(SONORE_SIMD_SCALAR)
  template <typename F>
  inline Vec4i perLane(Vec4i o, F&& f) const {
    Native n{};
    for (int i = 0; i < 4; ++i) n.v[i] = (int32_t) f((uint32_t) v_.v[i], (uint32_t) o.v_.v[i]);
    return Vec4i(n);
  }
#endif

  Native v_{};
};

/** Four lanes of `T`: Vec4f for float, Vec4d for double. What a template
 *  over its sample type writes, so its float form is exactly the Vec4f code
 *  it always was. */
template <typename T>
struct Vec4Of;
template <>
struct Vec4Of<float> {
  using type = Vec4f;
};
template <>
struct Vec4Of<double> {
  using type = Vec4d;
};
/** See Vec4Of. */
template <typename T>
using Vec4 = typename Vec4Of<T>::type;

// ── Narrower and wider integer lanes ─────────────────────────────────────────

namespace simddetail {
#if defined(SONORE_SIMD_NEON)
// NEON names its integer registers by lane type; SSE and wasm have one.
template <typename Int>
struct NeonInt;
template <>
struct NeonInt<int8_t> {
  using type = int8x16_t;
};
template <>
struct NeonInt<int16_t> {
  using type = int16x8_t;
};
template <>
struct NeonInt<int64_t> {
  using type = int64x2_t;
};
#endif
} // namespace simddetail

/**
 * Sixteen int8, eight int16 or two int64 lanes in one 128-bit register:
 * Vec16i8, Vec8i16 and Vec2i64. With Vec4i (int32) they are every integer
 * width a 128-bit register holds. The 16-bit one is the one audio meets most:
 * PCM samples, which widen() turns into Vec4i and Vec4i::toFloat() into
 * floats, exactly.
 *
 * Unsigned lanes are the same register read differently. Addition,
 * subtraction, multiplication, the bitwise operations and the left shift
 * give the same bits either way; what differs -- comparison, min/max,
 * saturation and the right shift -- comes in both forms
 * (lessThanUnsigned(), minUnsigned(), addSaturateUnsigned(),
 * shiftRightLogical()).
 *
 * The rules are Vec4i's. Arithmetic WRAPS, on every backend and in the scalar
 * reference. Shift counts are taken modulo the lane width. And where a unit
 * lacks an operation it is built from what the unit has, giving the same
 * bits: SSE2 has no 8-bit multiply or shift (made from 16-bit ones), no
 * 64-bit multiply (made from 32-bit pieces), no 64-bit arithmetic right
 * shift (a logical one plus the sign), and no 64-bit comparison, which it
 * and 32-bit ARM do a lane at a time; NEON has no 64-bit multiply and
 * WebAssembly no 8-bit one. The tests run every operation against the
 * scalar loop on whichever unit they are built for.
 */
// `Int`, not `L`: stb_vorbis #defines L, C and R, and a user's own copy of it
// can come before this header (two_tu_test includes one first on purpose).
template <typename Int>
class VecIntT {
  static_assert(std::is_same<Int, int8_t>::value || std::is_same<Int, int16_t>::value ||
                    std::is_same<Int, int64_t>::value,
                "VecIntT holds int8_t, int16_t or int64_t lanes; Vec4i is the int32 one");
  using U = typename std::make_unsigned<Int>::type;
  static constexpr int kBits = 8 * (int) sizeof(Int);
  static constexpr int kLanes = 16 / (int) sizeof(Int);

public:
  /** The lane type. */
  using Lane = Int;
  /** Lanes per vector: 16, 8 or 2. */
  static constexpr int width() { return kLanes; }

#if defined(SONORE_SIMD_SSE)
  using Native = __m128i;
#elif defined(SONORE_SIMD_NEON)
  using Native = typename simddetail::NeonInt<Int>::type;
#elif defined(SONORE_SIMD_WASM)
  using Native = v128_t;
#else
  /** The lanes as plain integers on the scalar build. */
  struct Native {
    Int v[16 / sizeof(Int)]; ///< lane 0 first
  };
#endif

  VecIntT() = default;
  /** Wraps the platform's register. */
  explicit VecIntT(Native v) : v_(v) {}
  /** The platform's register (the struct of plain lanes on the scalar
   *  build). */
  Native native() const { return v_; }

  /** Every lane `x`. */
  static inline VecIntT splat(Int x) {
#if defined(SONORE_SIMD_SSE)
    if constexpr (sizeof(Int) == 1) return VecIntT(_mm_set1_epi8((char) x));
    else if constexpr (sizeof(Int) == 2) return VecIntT(_mm_set1_epi16(x));
    else return VecIntT(_mm_set1_epi64x(x));
#elif defined(SONORE_SIMD_NEON)
    if constexpr (sizeof(Int) == 1) return VecIntT(vdupq_n_s8(x));
    else if constexpr (sizeof(Int) == 2) return VecIntT(vdupq_n_s16(x));
    else return VecIntT(vdupq_n_s64(x));
#elif defined(SONORE_SIMD_WASM)
    if constexpr (sizeof(Int) == 1) return VecIntT(wasm_i8x16_splat(x));
    else if constexpr (sizeof(Int) == 2) return VecIntT(wasm_i16x8_splat(x));
    else return VecIntT(wasm_i64x2_splat(x));
#else
    Native n{};
    for (int i = 0; i < kLanes; ++i) n.v[i] = x;
    return VecIntT(n);
#endif
  }
  /** Every lane 0. */
  static inline VecIntT zero() { return splat(0); }
  /** Unaligned load of p[0 .. width() - 1]. */
  static inline VecIntT load(const Int* p) {
#if defined(SONORE_SIMD_SSE)
    return VecIntT(_mm_loadu_si128(reinterpret_cast<const __m128i*>(p)));
#elif defined(SONORE_SIMD_NEON)
    if constexpr (sizeof(Int) == 1) return VecIntT(vld1q_s8(p));
    else if constexpr (sizeof(Int) == 2) return VecIntT(vld1q_s16(p));
    else return VecIntT(vld1q_s64(p));
#elif defined(SONORE_SIMD_WASM)
    return VecIntT(wasm_v128_load(p));
#else
    Native n{};
    for (int i = 0; i < kLanes; ++i) n.v[i] = p[i];
    return VecIntT(n);
#endif
  }
  /** Unaligned store to p[0 .. width() - 1]. */
  inline void store(Int* p) const {
#if defined(SONORE_SIMD_SSE)
    _mm_storeu_si128(reinterpret_cast<__m128i*>(p), v_);
#elif defined(SONORE_SIMD_NEON)
    if constexpr (sizeof(Int) == 1) vst1q_s8(p, v_);
    else if constexpr (sizeof(Int) == 2) vst1q_s16(p, v_);
    else vst1q_s64(p, v_);
#elif defined(SONORE_SIMD_WASM)
    wasm_v128_store(p, v_);
#else
    for (int i = 0; i < kLanes; ++i) p[i] = v_.v[i];
#endif
  }

  /** Lane-by-lane sum, wrapping. */
  inline VecIntT operator+(VecIntT o) const {
#if defined(SONORE_SIMD_SSE)
    if constexpr (sizeof(Int) == 1) return VecIntT(_mm_add_epi8(v_, o.v_));
    else if constexpr (sizeof(Int) == 2) return VecIntT(_mm_add_epi16(v_, o.v_));
    else return VecIntT(_mm_add_epi64(v_, o.v_));
#elif defined(SONORE_SIMD_NEON)
    if constexpr (sizeof(Int) == 1) return VecIntT(vaddq_s8(v_, o.v_));
    else if constexpr (sizeof(Int) == 2) return VecIntT(vaddq_s16(v_, o.v_));
    else return VecIntT(vaddq_s64(v_, o.v_));
#elif defined(SONORE_SIMD_WASM)
    if constexpr (sizeof(Int) == 1) return VecIntT(wasm_i8x16_add(v_, o.v_));
    else if constexpr (sizeof(Int) == 2) return VecIntT(wasm_i16x8_add(v_, o.v_));
    else return VecIntT(wasm_i64x2_add(v_, o.v_));
#else
    return lanes(o, [](Int a, Int b) { return (Int) (U) ((U) a + (U) b); });
#endif
  }
  /** Lane-by-lane difference, wrapping. */
  inline VecIntT operator-(VecIntT o) const {
#if defined(SONORE_SIMD_SSE)
    if constexpr (sizeof(Int) == 1) return VecIntT(_mm_sub_epi8(v_, o.v_));
    else if constexpr (sizeof(Int) == 2) return VecIntT(_mm_sub_epi16(v_, o.v_));
    else return VecIntT(_mm_sub_epi64(v_, o.v_));
#elif defined(SONORE_SIMD_NEON)
    if constexpr (sizeof(Int) == 1) return VecIntT(vsubq_s8(v_, o.v_));
    else if constexpr (sizeof(Int) == 2) return VecIntT(vsubq_s16(v_, o.v_));
    else return VecIntT(vsubq_s64(v_, o.v_));
#elif defined(SONORE_SIMD_WASM)
    if constexpr (sizeof(Int) == 1) return VecIntT(wasm_i8x16_sub(v_, o.v_));
    else if constexpr (sizeof(Int) == 2) return VecIntT(wasm_i16x8_sub(v_, o.v_));
    else return VecIntT(wasm_i64x2_sub(v_, o.v_));
#else
    return lanes(o, [](Int a, Int b) { return (Int) (U) ((U) a - (U) b); });
#endif
  }
  /** Lane-by-lane product, the low bits (wrapping). */
  inline VecIntT operator*(VecIntT o) const {
#if defined(SONORE_SIMD_SSE)
    if constexpr (sizeof(Int) == 2) {
      return VecIntT(_mm_mullo_epi16(v_, o.v_));
    } else if constexpr (sizeof(Int) == 1) {
      // A 16-bit product's low byte is the product of the low bytes; the high
      // bytes are multiplied after shifting them down, and put back.
      const __m128i even = _mm_mullo_epi16(v_, o.v_);
      const __m128i odd = _mm_mullo_epi16(_mm_srli_epi16(v_, 8), _mm_srli_epi16(o.v_, 8));
      return VecIntT(_mm_or_si128(_mm_and_si128(even, _mm_set1_epi16(0x00FF)), _mm_slli_epi16(odd, 8)));
    } else {
      // Modulo 2^64, a b = al bl + ((al bh + ah bl) << 32), and the 32 x 32
      // -> 64 multiply is the one SSE2 has.
      const __m128i lolo = _mm_mul_epu32(v_, o.v_);
      const __m128i lohi = _mm_mul_epu32(v_, _mm_srli_epi64(o.v_, 32));
      const __m128i hilo = _mm_mul_epu32(_mm_srli_epi64(v_, 32), o.v_);
      return VecIntT(_mm_add_epi64(lolo, _mm_slli_epi64(_mm_add_epi64(lohi, hilo), 32)));
    }
#elif defined(SONORE_SIMD_NEON)
    if constexpr (sizeof(Int) == 1) return VecIntT(vmulq_s8(v_, o.v_));
    else if constexpr (sizeof(Int) == 2) return VecIntT(vmulq_s16(v_, o.v_));
    else return lanes(o, mulLane);
#elif defined(SONORE_SIMD_WASM)
    if constexpr (sizeof(Int) == 2) {
      return VecIntT(wasm_i16x8_mul(v_, o.v_));
    } else if constexpr (sizeof(Int) == 8) {
      return VecIntT(wasm_i64x2_mul(v_, o.v_));
    } else {
      const v128_t even = wasm_i16x8_mul(v_, o.v_);
      const v128_t odd = wasm_i16x8_mul(wasm_u16x8_shr(v_, 8), wasm_u16x8_shr(o.v_, 8));
      return VecIntT(wasm_v128_or(wasm_v128_and(even, wasm_i16x8_splat(0x00FF)), wasm_i16x8_shl(odd, 8)));
    }
#else
    return lanes(o, mulLane);
#endif
  }
  /** In-place sum. */
  inline VecIntT& operator+=(VecIntT o) { *this = *this + o; return *this; }
  /** In-place difference. */
  inline VecIntT& operator-=(VecIntT o) { *this = *this - o; return *this; }
  /** In-place product. */
  inline VecIntT& operator*=(VecIntT o) { *this = *this * o; return *this; }

  /** Lane-by-lane sum that SATURATES at the lane's signed range instead of
   *  wrapping: what mixing two PCM signals wants. 8- and 16-bit lanes. */
  inline VecIntT addSaturate(VecIntT o) const {
    static_assert(sizeof(Int) <= 2, "saturating arithmetic is for 8- and 16-bit lanes");
#if defined(SONORE_SIMD_SSE)
    if constexpr (sizeof(Int) == 1) return VecIntT(_mm_adds_epi8(v_, o.v_));
    else return VecIntT(_mm_adds_epi16(v_, o.v_));
#elif defined(SONORE_SIMD_NEON)
    if constexpr (sizeof(Int) == 1) return VecIntT(vqaddq_s8(v_, o.v_));
    else return VecIntT(vqaddq_s16(v_, o.v_));
#elif defined(SONORE_SIMD_WASM)
    if constexpr (sizeof(Int) == 1) return VecIntT(wasm_i8x16_add_sat(v_, o.v_));
    else return VecIntT(wasm_i16x8_add_sat(v_, o.v_));
#else
    return lanes(o, [](Int a, Int b) { return clampSigned((int32_t) a + (int32_t) b); });
#endif
  }
  /** Lane-by-lane difference, saturating at the signed range. */
  inline VecIntT subSaturate(VecIntT o) const {
    static_assert(sizeof(Int) <= 2, "saturating arithmetic is for 8- and 16-bit lanes");
#if defined(SONORE_SIMD_SSE)
    if constexpr (sizeof(Int) == 1) return VecIntT(_mm_subs_epi8(v_, o.v_));
    else return VecIntT(_mm_subs_epi16(v_, o.v_));
#elif defined(SONORE_SIMD_NEON)
    if constexpr (sizeof(Int) == 1) return VecIntT(vqsubq_s8(v_, o.v_));
    else return VecIntT(vqsubq_s16(v_, o.v_));
#elif defined(SONORE_SIMD_WASM)
    if constexpr (sizeof(Int) == 1) return VecIntT(wasm_i8x16_sub_sat(v_, o.v_));
    else return VecIntT(wasm_i16x8_sub_sat(v_, o.v_));
#else
    return lanes(o, [](Int a, Int b) { return clampSigned((int32_t) a - (int32_t) b); });
#endif
  }
  /** Lane-by-lane sum with both read as UNSIGNED, saturating at the
   *  unsigned range. */
  inline VecIntT addSaturateUnsigned(VecIntT o) const {
    static_assert(sizeof(Int) <= 2, "saturating arithmetic is for 8- and 16-bit lanes");
#if defined(SONORE_SIMD_SSE)
    if constexpr (sizeof(Int) == 1) return VecIntT(_mm_adds_epu8(v_, o.v_));
    else return VecIntT(_mm_adds_epu16(v_, o.v_));
#elif defined(SONORE_SIMD_NEON)
    if constexpr (sizeof(Int) == 1)
      return VecIntT(vreinterpretq_s8_u8(vqaddq_u8(vreinterpretq_u8_s8(v_), vreinterpretq_u8_s8(o.v_))));
    else
      return VecIntT(vreinterpretq_s16_u16(vqaddq_u16(vreinterpretq_u16_s16(v_), vreinterpretq_u16_s16(o.v_))));
#elif defined(SONORE_SIMD_WASM)
    if constexpr (sizeof(Int) == 1) return VecIntT(wasm_u8x16_add_sat(v_, o.v_));
    else return VecIntT(wasm_u16x8_add_sat(v_, o.v_));
#else
    return lanes(o, [](Int a, Int b) { return clampUnsigned((int32_t) (U) a + (int32_t) (U) b); });
#endif
  }
  /** Lane-by-lane difference read as UNSIGNED, saturating at 0. */
  inline VecIntT subSaturateUnsigned(VecIntT o) const {
    static_assert(sizeof(Int) <= 2, "saturating arithmetic is for 8- and 16-bit lanes");
#if defined(SONORE_SIMD_SSE)
    if constexpr (sizeof(Int) == 1) return VecIntT(_mm_subs_epu8(v_, o.v_));
    else return VecIntT(_mm_subs_epu16(v_, o.v_));
#elif defined(SONORE_SIMD_NEON)
    if constexpr (sizeof(Int) == 1)
      return VecIntT(vreinterpretq_s8_u8(vqsubq_u8(vreinterpretq_u8_s8(v_), vreinterpretq_u8_s8(o.v_))));
    else
      return VecIntT(vreinterpretq_s16_u16(vqsubq_u16(vreinterpretq_u16_s16(v_), vreinterpretq_u16_s16(o.v_))));
#elif defined(SONORE_SIMD_WASM)
    if constexpr (sizeof(Int) == 1) return VecIntT(wasm_u8x16_sub_sat(v_, o.v_));
    else return VecIntT(wasm_u16x8_sub_sat(v_, o.v_));
#else
    return lanes(o, [](Int a, Int b) { return clampUnsigned((int32_t) (U) a - (int32_t) (U) b); });
#endif
  }

  /** Bitwise and. */
  inline VecIntT operator&(VecIntT o) const {
#if defined(SONORE_SIMD_SSE)
    return VecIntT(_mm_and_si128(v_, o.v_));
#elif defined(SONORE_SIMD_NEON)
    if constexpr (sizeof(Int) == 1) return VecIntT(vandq_s8(v_, o.v_));
    else if constexpr (sizeof(Int) == 2) return VecIntT(vandq_s16(v_, o.v_));
    else return VecIntT(vandq_s64(v_, o.v_));
#elif defined(SONORE_SIMD_WASM)
    return VecIntT(wasm_v128_and(v_, o.v_));
#else
    return lanes(o, [](Int a, Int b) { return (Int) (U) ((U) a & (U) b); });
#endif
  }
  /** Bitwise or. */
  inline VecIntT operator|(VecIntT o) const {
#if defined(SONORE_SIMD_SSE)
    return VecIntT(_mm_or_si128(v_, o.v_));
#elif defined(SONORE_SIMD_NEON)
    if constexpr (sizeof(Int) == 1) return VecIntT(vorrq_s8(v_, o.v_));
    else if constexpr (sizeof(Int) == 2) return VecIntT(vorrq_s16(v_, o.v_));
    else return VecIntT(vorrq_s64(v_, o.v_));
#elif defined(SONORE_SIMD_WASM)
    return VecIntT(wasm_v128_or(v_, o.v_));
#else
    return lanes(o, [](Int a, Int b) { return (Int) (U) ((U) a | (U) b); });
#endif
  }
  /** Bitwise exclusive or. */
  inline VecIntT operator^(VecIntT o) const {
#if defined(SONORE_SIMD_SSE)
    return VecIntT(_mm_xor_si128(v_, o.v_));
#elif defined(SONORE_SIMD_NEON)
    if constexpr (sizeof(Int) == 1) return VecIntT(veorq_s8(v_, o.v_));
    else if constexpr (sizeof(Int) == 2) return VecIntT(veorq_s16(v_, o.v_));
    else return VecIntT(veorq_s64(v_, o.v_));
#elif defined(SONORE_SIMD_WASM)
    return VecIntT(wasm_v128_xor(v_, o.v_));
#else
    return lanes(o, [](Int a, Int b) { return (Int) (U) ((U) a ^ (U) b); });
#endif
  }

  /** Every lane shifted left by `n` modulo the lane width. */
  inline VecIntT shiftLeft(int n) const {
    n &= kBits - 1;
#if defined(SONORE_SIMD_SSE)
    if constexpr (sizeof(Int) == 2) {
      return VecIntT(_mm_sll_epi16(v_, _mm_cvtsi32_si128(n)));
    } else if constexpr (sizeof(Int) == 8) {
      return VecIntT(_mm_sll_epi64(v_, _mm_cvtsi32_si128(n)));
    } else {
      // As 16-bit lanes, then the bits that crossed up from the byte below
      // are cleared.
      const __m128i shifted = _mm_sll_epi16(v_, _mm_cvtsi32_si128(n));
      return VecIntT(_mm_and_si128(shifted, _mm_set1_epi8((char) (uint8_t) (0xFFu << n))));
    }
#elif defined(SONORE_SIMD_NEON)
    if constexpr (sizeof(Int) == 1) return VecIntT(vshlq_s8(v_, vdupq_n_s8((int8_t) n)));
    else if constexpr (sizeof(Int) == 2) return VecIntT(vshlq_s16(v_, vdupq_n_s16((int16_t) n)));
    else return VecIntT(vshlq_s64(v_, vdupq_n_s64(n)));
#elif defined(SONORE_SIMD_WASM)
    if constexpr (sizeof(Int) == 1) return VecIntT(wasm_i8x16_shl(v_, (uint32_t) n));
    else if constexpr (sizeof(Int) == 2) return VecIntT(wasm_i16x8_shl(v_, (uint32_t) n));
    else return VecIntT(wasm_i64x2_shl(v_, (uint32_t) n));
#else
    return lanes(*this, [n](Int a, Int) { return (Int) (U) ((uint64_t) (U) a << n); });
#endif
  }
  /** Every lane shifted right by `n` modulo the lane width, LOGICALLY: zeros
   *  in. The unsigned right shift. */
  inline VecIntT shiftRightLogical(int n) const {
    n &= kBits - 1;
#if defined(SONORE_SIMD_SSE)
    if constexpr (sizeof(Int) == 2) {
      return VecIntT(_mm_srl_epi16(v_, _mm_cvtsi32_si128(n)));
    } else if constexpr (sizeof(Int) == 8) {
      return VecIntT(_mm_srl_epi64(v_, _mm_cvtsi32_si128(n)));
    } else {
      const __m128i shifted = _mm_srl_epi16(v_, _mm_cvtsi32_si128(n));
      return VecIntT(_mm_and_si128(shifted, _mm_set1_epi8((char) (uint8_t) (0xFFu >> n))));
    }
#elif defined(SONORE_SIMD_NEON)
    if constexpr (sizeof(Int) == 1)
      return VecIntT(vreinterpretq_s8_u8(vshlq_u8(vreinterpretq_u8_s8(v_), vdupq_n_s8((int8_t) -n))));
    else if constexpr (sizeof(Int) == 2)
      return VecIntT(vreinterpretq_s16_u16(vshlq_u16(vreinterpretq_u16_s16(v_), vdupq_n_s16((int16_t) -n))));
    else
      return VecIntT(vreinterpretq_s64_u64(vshlq_u64(vreinterpretq_u64_s64(v_), vdupq_n_s64(-n))));
#elif defined(SONORE_SIMD_WASM)
    if constexpr (sizeof(Int) == 1) return VecIntT(wasm_u8x16_shr(v_, (uint32_t) n));
    else if constexpr (sizeof(Int) == 2) return VecIntT(wasm_u16x8_shr(v_, (uint32_t) n));
    else return VecIntT(wasm_u64x2_shr(v_, (uint32_t) n));
#else
    return lanes(*this, [n](Int a, Int) { return (Int) (U) ((U) a >> n); });
#endif
  }
  /** Every lane shifted right by `n` modulo the lane width, ARITHMETICALLY:
   *  the sign bit copied in, so -8 >> 1 is -4. */
  inline VecIntT shiftRight(int n) const {
    n &= kBits - 1;
#if defined(SONORE_SIMD_SSE)
    if constexpr (sizeof(Int) == 2) {
      return VecIntT(_mm_sra_epi16(v_, _mm_cvtsi32_si128(n)));
    } else if constexpr (sizeof(Int) == 1) {
      // The logical shift, then the sign filled into the n bits it emptied.
      const __m128i sign = _mm_cmplt_epi8(v_, _mm_setzero_si128());
      const __m128i kept = _mm_set1_epi8((char) (uint8_t) (0xFFu >> n));
      return VecIntT(_mm_or_si128(shiftRightLogical(n).v_, _mm_andnot_si128(kept, sign)));
    } else {
      // Each 64-bit lane's sign, spread across it, shifted up into the n
      // emptied bits; a shift by 64 (n = 0) clears it, as SSE defines.
      const __m128i sign = _mm_srai_epi32(_mm_shuffle_epi32(v_, _MM_SHUFFLE(3, 3, 1, 1)), 31);
      return VecIntT(_mm_or_si128(_mm_srl_epi64(v_, _mm_cvtsi32_si128(n)),
                                  _mm_sll_epi64(sign, _mm_cvtsi32_si128(64 - n))));
    }
#elif defined(SONORE_SIMD_NEON)
    if constexpr (sizeof(Int) == 1) return VecIntT(vshlq_s8(v_, vdupq_n_s8((int8_t) -n)));
    else if constexpr (sizeof(Int) == 2) return VecIntT(vshlq_s16(v_, vdupq_n_s16((int16_t) -n)));
    else return VecIntT(vshlq_s64(v_, vdupq_n_s64(-n)));
#elif defined(SONORE_SIMD_WASM)
    if constexpr (sizeof(Int) == 1) return VecIntT(wasm_i8x16_shr(v_, (uint32_t) n));
    else if constexpr (sizeof(Int) == 2) return VecIntT(wasm_i16x8_shr(v_, (uint32_t) n));
    else return VecIntT(wasm_i64x2_shr(v_, (uint32_t) n));
#else
    // Spelled out, since >> on a negative value was only pinned down by C++20.
    return lanes(*this, [n](Int a, Int) {
      U r = (U) ((U) a >> n);
      if (a < 0 && n > 0) r = (U) (r | (U) ~(U) ((U) ~(U) 0 >> n));
      return (Int) r;
    });
#endif
  }

  /** The mask of this < o, signed: all bits of a lane set, or none. */
  inline VecIntT lessThan(VecIntT o) const {
#if defined(SONORE_SIMD_SSE)
    if constexpr (sizeof(Int) == 1) return VecIntT(_mm_cmplt_epi8(v_, o.v_));
    else if constexpr (sizeof(Int) == 2) return VecIntT(_mm_cmplt_epi16(v_, o.v_));
    else return lanes(o, [](Int a, Int b) { return a < b ? (Int) -1 : (Int) 0; });
#elif defined(SONORE_SIMD_NEON)
    if constexpr (sizeof(Int) == 1) return VecIntT(vreinterpretq_s8_u8(vcltq_s8(v_, o.v_)));
    else if constexpr (sizeof(Int) == 2) return VecIntT(vreinterpretq_s16_u16(vcltq_s16(v_, o.v_)));
#if defined(SONORE_SIMD_NEON64)
    else return VecIntT(vreinterpretq_s64_u64(vcltq_s64(v_, o.v_)));
#else
    else return lanes(o, [](Int a, Int b) { return a < b ? (Int) -1 : (Int) 0; });
#endif
#elif defined(SONORE_SIMD_WASM)
    if constexpr (sizeof(Int) == 1) return VecIntT(wasm_i8x16_lt(v_, o.v_));
    else if constexpr (sizeof(Int) == 2) return VecIntT(wasm_i16x8_lt(v_, o.v_));
    else return VecIntT(wasm_i64x2_lt(v_, o.v_));
#else
    return lanes(o, [](Int a, Int b) { return a < b ? (Int) -1 : (Int) 0; });
#endif
  }
  /** The mask of this > o, signed. */
  inline VecIntT greaterThan(VecIntT o) const { return o.lessThan(*this); }
  /** The mask of this < o with both read as UNSIGNED. */
  inline VecIntT lessThanUnsigned(VecIntT o) const {
#if defined(SONORE_SIMD_SSE)
    if constexpr (sizeof(Int) <= 2) {
      // SSE2 compares signed only; flipping both sign bits maps the unsigned
      // order onto the signed one.
      const VecIntT flip = splat((Int) ((U) 1 << (kBits - 1)));
      return (*this ^ flip).lessThan(o ^ flip);
    } else {
      return lanes(o, [](Int a, Int b) { return (U) a < (U) b ? (Int) -1 : (Int) 0; });
    }
#elif defined(SONORE_SIMD_NEON)
    if constexpr (sizeof(Int) == 1)
      return VecIntT(vreinterpretq_s8_u8(vcltq_u8(vreinterpretq_u8_s8(v_), vreinterpretq_u8_s8(o.v_))));
    else if constexpr (sizeof(Int) == 2)
      return VecIntT(vreinterpretq_s16_u16(vcltq_u16(vreinterpretq_u16_s16(v_), vreinterpretq_u16_s16(o.v_))));
#if defined(SONORE_SIMD_NEON64)
    else
      return VecIntT(vreinterpretq_s64_u64(vcltq_u64(vreinterpretq_u64_s64(v_), vreinterpretq_u64_s64(o.v_))));
#else
    else return lanes(o, [](Int a, Int b) { return (U) a < (U) b ? (Int) -1 : (Int) 0; });
#endif
#elif defined(SONORE_SIMD_WASM)
    if constexpr (sizeof(Int) == 1) {
      return VecIntT(wasm_u8x16_lt(v_, o.v_));
    } else if constexpr (sizeof(Int) == 2) {
      return VecIntT(wasm_u16x8_lt(v_, o.v_));
    } else {
      // simd128 has no unsigned 64-bit compare: the sign-flip again.
      const VecIntT flip = splat((Int) ((U) 1 << 63));
      return (*this ^ flip).lessThan(o ^ flip);
    }
#else
    return lanes(o, [](Int a, Int b) { return (U) a < (U) b ? (Int) -1 : (Int) 0; });
#endif
  }
  /** The mask of this > o, unsigned. */
  inline VecIntT greaterThanUnsigned(VecIntT o) const { return o.lessThanUnsigned(*this); }
  /** The mask of this == o. */
  inline VecIntT equalTo(VecIntT o) const {
#if defined(SONORE_SIMD_SSE)
    if constexpr (sizeof(Int) == 1) {
      return VecIntT(_mm_cmpeq_epi8(v_, o.v_));
    } else if constexpr (sizeof(Int) == 2) {
      return VecIntT(_mm_cmpeq_epi16(v_, o.v_));
    } else {
      // Equal as 64 bits when both 32-bit halves are.
      const __m128i halves = _mm_cmpeq_epi32(v_, o.v_);
      return VecIntT(_mm_and_si128(halves, _mm_shuffle_epi32(halves, _MM_SHUFFLE(2, 3, 0, 1))));
    }
#elif defined(SONORE_SIMD_NEON)
    if constexpr (sizeof(Int) == 1) return VecIntT(vreinterpretq_s8_u8(vceqq_s8(v_, o.v_)));
    else if constexpr (sizeof(Int) == 2) return VecIntT(vreinterpretq_s16_u16(vceqq_s16(v_, o.v_)));
#if defined(SONORE_SIMD_NEON64)
    else return VecIntT(vreinterpretq_s64_u64(vceqq_s64(v_, o.v_)));
#else
    else return lanes(o, [](Int a, Int b) { return a == b ? (Int) -1 : (Int) 0; });
#endif
#elif defined(SONORE_SIMD_WASM)
    if constexpr (sizeof(Int) == 1) return VecIntT(wasm_i8x16_eq(v_, o.v_));
    else if constexpr (sizeof(Int) == 2) return VecIntT(wasm_i16x8_eq(v_, o.v_));
    else return VecIntT(wasm_i64x2_eq(v_, o.v_));
#else
    return lanes(o, [](Int a, Int b) { return a == b ? (Int) -1 : (Int) 0; });
#endif
  }
  /** mask ? a : b, bit by bit. */
  static inline VecIntT select(VecIntT mask, VecIntT a, VecIntT b) { return (mask & a) | andNot(mask, b); }
  /** a < b ? a : b per lane, signed. */
  inline VecIntT (min)(VecIntT o) const {
#if defined(SONORE_SIMD_SSE)
    if constexpr (sizeof(Int) == 2) return VecIntT(_mm_min_epi16(v_, o.v_));
    else return select(lessThan(o), *this, o);
#elif defined(SONORE_SIMD_NEON)
    if constexpr (sizeof(Int) == 1) return VecIntT(vminq_s8(v_, o.v_));
    else if constexpr (sizeof(Int) == 2) return VecIntT(vminq_s16(v_, o.v_));
    else return select(lessThan(o), *this, o);
#elif defined(SONORE_SIMD_WASM)
    if constexpr (sizeof(Int) == 1) return VecIntT(wasm_i8x16_min(v_, o.v_));
    else if constexpr (sizeof(Int) == 2) return VecIntT(wasm_i16x8_min(v_, o.v_));
    else return select(lessThan(o), *this, o);
#else
    return select(lessThan(o), *this, o);
#endif
  }
  /** a > b ? a : b per lane, signed. */
  inline VecIntT (max)(VecIntT o) const {
#if defined(SONORE_SIMD_SSE)
    if constexpr (sizeof(Int) == 2) return VecIntT(_mm_max_epi16(v_, o.v_));
    else return select(greaterThan(o), *this, o);
#elif defined(SONORE_SIMD_NEON)
    if constexpr (sizeof(Int) == 1) return VecIntT(vmaxq_s8(v_, o.v_));
    else if constexpr (sizeof(Int) == 2) return VecIntT(vmaxq_s16(v_, o.v_));
    else return select(greaterThan(o), *this, o);
#elif defined(SONORE_SIMD_WASM)
    if constexpr (sizeof(Int) == 1) return VecIntT(wasm_i8x16_max(v_, o.v_));
    else if constexpr (sizeof(Int) == 2) return VecIntT(wasm_i16x8_max(v_, o.v_));
    else return select(greaterThan(o), *this, o);
#else
    return select(greaterThan(o), *this, o);
#endif
  }
  /** a < b ? a : b per lane, unsigned. */
  inline VecIntT minUnsigned(VecIntT o) const {
#if defined(SONORE_SIMD_SSE)
    if constexpr (sizeof(Int) == 1) return VecIntT(_mm_min_epu8(v_, o.v_));
    else return select(lessThanUnsigned(o), *this, o);
#elif defined(SONORE_SIMD_NEON)
    if constexpr (sizeof(Int) == 1)
      return VecIntT(vreinterpretq_s8_u8(vminq_u8(vreinterpretq_u8_s8(v_), vreinterpretq_u8_s8(o.v_))));
    else if constexpr (sizeof(Int) == 2)
      return VecIntT(vreinterpretq_s16_u16(vminq_u16(vreinterpretq_u16_s16(v_), vreinterpretq_u16_s16(o.v_))));
    else return select(lessThanUnsigned(o), *this, o);
#elif defined(SONORE_SIMD_WASM)
    if constexpr (sizeof(Int) == 1) return VecIntT(wasm_u8x16_min(v_, o.v_));
    else if constexpr (sizeof(Int) == 2) return VecIntT(wasm_u16x8_min(v_, o.v_));
    else return select(lessThanUnsigned(o), *this, o);
#else
    return select(lessThanUnsigned(o), *this, o);
#endif
  }
  /** a > b ? a : b per lane, unsigned. */
  inline VecIntT maxUnsigned(VecIntT o) const {
#if defined(SONORE_SIMD_SSE)
    if constexpr (sizeof(Int) == 1) return VecIntT(_mm_max_epu8(v_, o.v_));
    else return select(greaterThanUnsigned(o), *this, o);
#elif defined(SONORE_SIMD_NEON)
    if constexpr (sizeof(Int) == 1)
      return VecIntT(vreinterpretq_s8_u8(vmaxq_u8(vreinterpretq_u8_s8(v_), vreinterpretq_u8_s8(o.v_))));
    else if constexpr (sizeof(Int) == 2)
      return VecIntT(vreinterpretq_s16_u16(vmaxq_u16(vreinterpretq_u16_s16(v_), vreinterpretq_u16_s16(o.v_))));
    else return select(greaterThanUnsigned(o), *this, o);
#elif defined(SONORE_SIMD_WASM)
    if constexpr (sizeof(Int) == 1) return VecIntT(wasm_u8x16_max(v_, o.v_));
    else if constexpr (sizeof(Int) == 2) return VecIntT(wasm_u16x8_max(v_, o.v_));
    else return select(greaterThanUnsigned(o), *this, o);
#else
    return select(greaterThanUnsigned(o), *this, o);
#endif
  }

  /** The lanes summed, wrapping: integer addition associates, so there is
   *  no order to define. */
  inline Int sum() const {
    Int tmp[kLanes];
    store(tmp);
    U s = 0;
    for (int i = 0; i < kLanes; ++i) s = (U) (s + (U) tmp[i]);
    return (Int) s;
  }
  /** Lane `i`, 0 to width() - 1. */
  inline Int lane(int i) const {
    Int tmp[kLanes];
    store(tmp);
    return tmp[i];
  }

private:
  static inline VecIntT andNot(VecIntT mask, VecIntT b) {
#if defined(SONORE_SIMD_SSE)
    return VecIntT(_mm_andnot_si128(mask.v_, b.v_));
#elif defined(SONORE_SIMD_NEON)
    if constexpr (sizeof(Int) == 1) return VecIntT(vbicq_s8(b.v_, mask.v_));
    else if constexpr (sizeof(Int) == 2) return VecIntT(vbicq_s16(b.v_, mask.v_));
    else return VecIntT(vbicq_s64(b.v_, mask.v_));
#elif defined(SONORE_SIMD_WASM)
    return VecIntT(wasm_v128_andnot(b.v_, mask.v_));
#else
    return mask.lanes(b, [](Int m, Int x) { return (Int) (U) (~(U) m & (U) x); });
#endif
  }
  // The low bits of a product. 8- and 16-bit lanes multiply as uint32_t:
  // promoted to int, 65535 * 65535 would be a signed overflow.
  static inline Int mulLane(Int a, Int b) {
    if constexpr (sizeof(Int) < 4) return (Int) (U) ((uint32_t) (U) a * (uint32_t) (U) b);
    else return (Int) ((U) a * (U) b);
  }
  static inline Int clampSigned(int32_t v) {
    constexpr int32_t lo = sizeof(Int) == 1 ? -128 : -32768, hi = sizeof(Int) == 1 ? 127 : 32767;
    return (Int) (v < lo ? lo : (v > hi ? hi : v));
  }
  static inline Int clampUnsigned(int32_t v) {
    constexpr int32_t hi = sizeof(Int) == 1 ? 255 : 65535;
    return (Int) (U) (v < 0 ? 0 : (v > hi ? hi : v));
  }
  // A lane at a time through memory: the scalar build, and each operation a
  // unit lacks.
  template <typename F>
  inline VecIntT lanes(VecIntT o, F&& f) const {
    Int a[kLanes], b[kLanes], r[kLanes];
    store(a);
    o.store(b);
    for (int i = 0; i < kLanes; ++i) r[i] = f(a[i], b[i]);
    return load(r);
  }

  Native v_{};
};

/** Sixteen int8 lanes. */
using Vec16i8 = VecIntT<int8_t>;
/** Eight int16 lanes: 16-bit PCM, eight samples at a time. */
using Vec8i16 = VecIntT<int16_t>;
/** Two int64 lanes. */
using Vec2i64 = VecIntT<int64_t>;

// ── Between the integer widths ───────────────────────────────────────────────
//
// Widening is exact (sign extension); narrowing SATURATES, which is what a
// conversion to a smaller PCM format must do -- wrapping turns an overload
// into a full-scale step of the opposite sign.

/** The eight int16 lanes of `v` sign-extended to int32: lanes 0..3 into `lo`,
 *  4..7 into `hi`. With Vec4i::toFloat(), 16-bit PCM to float exactly. */
inline void widen(Vec8i16 v, Vec4i& lo, Vec4i& hi) {
#if defined(SONORE_SIMD_SSE)
  lo = Vec4i(_mm_srai_epi32(_mm_unpacklo_epi16(v.native(), v.native()), 16));
  hi = Vec4i(_mm_srai_epi32(_mm_unpackhi_epi16(v.native(), v.native()), 16));
#elif defined(SONORE_SIMD_NEON)
  lo = Vec4i(vmovl_s16(vget_low_s16(v.native())));
  hi = Vec4i(vmovl_s16(vget_high_s16(v.native())));
#elif defined(SONORE_SIMD_WASM)
  lo = Vec4i(wasm_i32x4_extend_low_i16x8(v.native()));
  hi = Vec4i(wasm_i32x4_extend_high_i16x8(v.native()));
#else
  int16_t in[8];
  int32_t a[4], b[4];
  v.store(in);
  for (int i = 0; i < 4; ++i) {
    a[i] = in[i];
    b[i] = in[i + 4];
  }
  lo = Vec4i::load(a);
  hi = Vec4i::load(b);
#endif
}

/** Eight int32 lanes -- `lo`'s then `hi`'s -- narrowed to int16, each held to
 *  -32768 .. 32767. */
inline Vec8i16 narrowSaturate(Vec4i lo, Vec4i hi) {
#if defined(SONORE_SIMD_SSE)
  return Vec8i16(_mm_packs_epi32(lo.native(), hi.native()));
#elif defined(SONORE_SIMD_NEON)
  return Vec8i16(vcombine_s16(vqmovn_s32(lo.native()), vqmovn_s32(hi.native())));
#elif defined(SONORE_SIMD_WASM)
  return Vec8i16(wasm_i16x8_narrow_i32x4(lo.native(), hi.native()));
#else
  int32_t a[4], b[4];
  int16_t out[8];
  lo.store(a);
  hi.store(b);
  for (int i = 0; i < 8; ++i) {
    const int32_t x = i < 4 ? a[i] : b[i - 4];
    out[i] = (int16_t) (x < -32768 ? -32768 : (x > 32767 ? 32767 : x));
  }
  return Vec8i16::load(out);
#endif
}

/** The sixteen int8 lanes of `v` sign-extended to int16: 0..7 into `lo`,
 *  8..15 into `hi`. */
inline void widen(Vec16i8 v, Vec8i16& lo, Vec8i16& hi) {
#if defined(SONORE_SIMD_SSE)
  lo = Vec8i16(_mm_srai_epi16(_mm_unpacklo_epi8(v.native(), v.native()), 8));
  hi = Vec8i16(_mm_srai_epi16(_mm_unpackhi_epi8(v.native(), v.native()), 8));
#elif defined(SONORE_SIMD_NEON)
  lo = Vec8i16(vmovl_s8(vget_low_s8(v.native())));
  hi = Vec8i16(vmovl_s8(vget_high_s8(v.native())));
#elif defined(SONORE_SIMD_WASM)
  lo = Vec8i16(wasm_i16x8_extend_low_i8x16(v.native()));
  hi = Vec8i16(wasm_i16x8_extend_high_i8x16(v.native()));
#else
  int8_t in[16];
  int16_t a[8], b[8];
  v.store(in);
  for (int i = 0; i < 8; ++i) {
    a[i] = in[i];
    b[i] = in[i + 8];
  }
  lo = Vec8i16::load(a);
  hi = Vec8i16::load(b);
#endif
}

/** Sixteen int16 lanes -- `lo`'s then `hi`'s -- narrowed to int8, each held
 *  to -128 .. 127. */
inline Vec16i8 narrowSaturate(Vec8i16 lo, Vec8i16 hi) {
#if defined(SONORE_SIMD_SSE)
  return Vec16i8(_mm_packs_epi16(lo.native(), hi.native()));
#elif defined(SONORE_SIMD_NEON)
  return Vec16i8(vcombine_s8(vqmovn_s16(lo.native()), vqmovn_s16(hi.native())));
#elif defined(SONORE_SIMD_WASM)
  return Vec16i8(wasm_i8x16_narrow_i16x8(lo.native(), hi.native()));
#else
  int16_t a[8], b[8];
  int8_t out[16];
  lo.store(a);
  hi.store(b);
  for (int i = 0; i < 16; ++i) {
    const int16_t x = i < 8 ? a[i] : b[i - 8];
    out[i] = (int8_t) (x < -128 ? -128 : (x > 127 ? 127 : x));
  }
  return Vec16i8::load(out);
#endif
}

/** Lanes 0 and 1 of `v` sign-extended to int64. */
inline Vec2i64 widenLow(Vec4i v) {
#if defined(SONORE_SIMD_SSE)
  return Vec2i64(_mm_unpacklo_epi32(v.native(), _mm_srai_epi32(v.native(), 31)));
#elif defined(SONORE_SIMD_NEON)
  return Vec2i64(vmovl_s32(vget_low_s32(v.native())));
#elif defined(SONORE_SIMD_WASM)
  return Vec2i64(wasm_i64x2_extend_low_i32x4(v.native()));
#else
  const int64_t out[2] = {v.lane(0), v.lane(1)};
  return Vec2i64::load(out);
#endif
}

/** Lanes 2 and 3 of `v` sign-extended to int64. */
inline Vec2i64 widenHigh(Vec4i v) {
#if defined(SONORE_SIMD_SSE)
  return Vec2i64(_mm_unpackhi_epi32(v.native(), _mm_srai_epi32(v.native(), 31)));
#elif defined(SONORE_SIMD_NEON)
  return Vec2i64(vmovl_s32(vget_high_s32(v.native())));
#elif defined(SONORE_SIMD_WASM)
  return Vec2i64(wasm_i64x2_extend_high_i32x4(v.native()));
#else
  const int64_t out[2] = {v.lane(2), v.lane(3)};
  return Vec2i64::load(out);
#endif
}

/** The two int64 lanes converted to double, rounded to nearest: exact below
 *  2^53. A lane at a time -- only AArch64 has the instruction, and the
 *  conversion is correctly rounded everywhere, so the bits agree. */
inline Vec2d toDouble(Vec2i64 v) {
  const double out[2] = {(double) v.lane(0), (double) v.lane(1)};
  return Vec2d::load(out);
}

// ── Complex lanes ────────────────────────────────────────────────────────────

/**
 * Four complex numbers in SPLIT form: the four real parts in one Vec4f (or
 * Vec4d) and the four imaginary parts in another. ComplexVec4T<float> is
 * Vec4cf, ComplexVec4T<double> is Vec4cd.
 *
 * Split, not interleaved re/im pairs, because a complex multiply is then four
 * multiplies and two adds with nothing to shuffle -- the layout vDSP calls
 * DSPSplitComplex and the one FFT kernels convert to. load() and store()
 * take an ordinary interleaved std::complex array and do the conversion;
 * loadSplit()/storeSplit() skip it for data already split.
 *
 * Every operation is lane-wise on the two halves, so the bits are Vec4f's (or
 * Vec4d's): the same on every unit. The product is (ac - bd) + (ad + bc)i,
 * computed in that order; there is no fused multiply-add to change it, and
 * no Annex G recovery of infinities, which is what std::complex's operator*
 * spends its time on.
 */
template <typename T>
class ComplexVec4T {
public:
  /** The register each half is: Vec4f or Vec4d. */
  using Half = Vec4<T>;
  /** Complex values per vector: 4. */
  static constexpr int width() { return 4; }

  ComplexVec4T() = default;
  /** From the four real parts and the four imaginary parts. */
  ComplexVec4T(Half re, Half im) : re_(re), im_(im) {}

  /** Every lane `z`. */
  static inline ComplexVec4T splat(std::complex<T> z) {
    return ComplexVec4T(Half::splat(z.real()), Half::splat(z.imag()));
  }
  /** Every lane 0. */
  static inline ComplexVec4T zero() { return ComplexVec4T(Half::zero(), Half::zero()); }
  /** Four values from an interleaved std::complex array (unaligned). */
  static inline ComplexVec4T load(const std::complex<T>* p) {
    // std::complex<T> is laid out as T[2] (C++11 [complex.numbers]).
    const T* q = reinterpret_cast<const T*>(p);
    if constexpr (std::is_same<T, float>::value) {
      Vec4f re, im;
      Vec4f::deinterleave(q, re, im);
      return ComplexVec4T(re, im);
    } else {
      T re[4], im[4];
      for (int i = 0; i < 4; ++i) {
        re[i] = q[2 * i];
        im[i] = q[2 * i + 1];
      }
      return ComplexVec4T(Half::load(re), Half::load(im));
    }
  }
  /** Four values to an interleaved std::complex array. */
  inline void store(std::complex<T>* p) const {
    T* q = reinterpret_cast<T*>(p);
    if constexpr (std::is_same<T, float>::value) {
      Vec4f::interleave(re_, im_, q);
    } else {
      T re[4], im[4];
      re_.store(re);
      im_.store(im);
      for (int i = 0; i < 4; ++i) {
        q[2 * i] = re[i];
        q[2 * i + 1] = im[i];
      }
    }
  }
  /** Four values from separate real and imaginary arrays. */
  static inline ComplexVec4T loadSplit(const T* re, const T* im) {
    return ComplexVec4T(Half::load(re), Half::load(im));
  }
  /** Four values to separate real and imaginary arrays. */
  inline void storeSplit(T* re, T* im) const {
    re_.store(re);
    im_.store(im);
  }

  /** The four real parts. */
  inline Half real() const { return re_; }
  /** The four imaginary parts. */
  inline Half imag() const { return im_; }

  /** Lane-by-lane sum. */
  inline ComplexVec4T operator+(ComplexVec4T o) const { return ComplexVec4T(re_ + o.re_, im_ + o.im_); }
  /** Lane-by-lane difference. */
  inline ComplexVec4T operator-(ComplexVec4T o) const { return ComplexVec4T(re_ - o.re_, im_ - o.im_); }
  /** Lane-by-lane complex product: (ac - bd) + (ad + bc)i. */
  inline ComplexVec4T operator*(ComplexVec4T o) const {
    return ComplexVec4T(re_ * o.re_ - im_ * o.im_, re_ * o.im_ + im_ * o.re_);
  }
  /** In-place sum. */
  inline ComplexVec4T& operator+=(ComplexVec4T o) { *this = *this + o; return *this; }
  /** In-place difference. */
  inline ComplexVec4T& operator-=(ComplexVec4T o) { *this = *this - o; return *this; }
  /** In-place product. */
  inline ComplexVec4T& operator*=(ComplexVec4T o) { *this = *this * o; return *this; }
  /** Each lane times a REAL factor (a gain, a window): both parts scaled. */
  inline ComplexVec4T scale(Half g) const { return ComplexVec4T(re_ * g, im_ * g); }
  /** The complex conjugate of each lane: the imaginary part times -1, which
   *  is exact and turns +0 into -0 as std::conj does (0 - x would not). */
  inline ComplexVec4T conj() const { return ComplexVec4T(re_, im_ * Half::splat(T(-1.0))); }
  /** |z|^2 of each lane, re^2 + im^2: the power, with no square root. */
  inline Half normSquared() const { return re_ * re_ + im_ * im_; }

  /** The four lanes summed, the real parts and the imaginary parts each in
   *  Half::sum()'s one order. */
  inline std::complex<T> sum() const { return std::complex<T>(re_.sum(), im_.sum()); }
  /** Lane `i`, 0 to 3. */
  inline std::complex<T> lane(int i) const { return std::complex<T>(re_.lane(i), im_.lane(i)); }

private:
  Half re_{}, im_{};
};

/** Four std::complex<float> in split form. */
using Vec4cf = ComplexVec4T<float>;
/** Four std::complex<double> in split form. */
using Vec4cd = ComplexVec4T<double>;

/** Which wide path Vec8f and Vec4d compiled to: "avx", or "2x128" when they
 *  are two 128-bit registers (or two scalar halves). For a test or a log;
 *  the numbers are the same either way. */
inline const char* wideBackend() {
#if defined(SONORE_SIMD_AVX)
  return "avx";
#else
  return "2x128";
#endif
}


// ── Block helpers ────────────────────────────────────────────────────────────
// These are where the speedup actually lives. A per-sample recursive filter
// cannot be vectorised across time (its next input depends on its last output),
// so the wins come from the places that ARE independent per sample: dot
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

/** dot(a, b) over `n` doubles: Vec4d lanes, closed in Vec4d's order, then the
 *  tail -- the float dot()'s structure, so a template over its sample type
 *  sums the same way in both precisions. */
inline double dot(const double* a, const double* b, size_t n) {
  size_t i = 0;
  Vec4d acc = Vec4d::zero();
  for (; i + 4 <= n; i += 4) acc += Vec4d::load(a + i) * Vec4d::load(b + i);
  double total = acc.sum();
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
  for (; i + 4 <= n; i += 4) m = (m.max)(Vec4f::load(data + i).abs());
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
