// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: every operation of every vector type, lane by lane, against
// the plain scalar arithmetic it stands for.
//
// simd_identity proves the vector build and the scalar build agree with EACH
// OTHER; this proves both are right -- that Vec8f's sum is the order the
// header promises, that an SSE2 multiply-low built from widening multiplies
// is the low 32 bits of the product, that a shift by 33 means a shift by 1
// on every unit. Numbers are compared bit for bit. A NaN result is checked
// for being NaN and nothing more: its sign and payload belong to the CPU (an
// x86 0/0 is negative, an ARM one positive, and wasm leaves it open), in
// scalar code exactly as in vector code.
#include <sonore/simd.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <complex>
#include <initializer_list>
#include <limits>
#include <type_traits>

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const char* what) {
  ++g_checks;
  if (!ok) ++g_failures;
  std::printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
}

uint32_t bitsOf(float f) {
  uint32_t u;
  std::memcpy(&u, &f, 4);
  return u;
}
uint64_t bitsOf(double d) {
  uint64_t u;
  std::memcpy(&u, &d, 8);
  return u;
}
template <typename T>
bool same(T got, T want) {
  if (std::isnan(want)) return std::isnan(got);
  return bitsOf(got) == bitsOf(want);
}
bool sameMask(float got, bool want) { return bitsOf(got) == (want ? 0xFFFFFFFFu : 0u); }
bool sameMask(double got, bool want) { return bitsOf(got) == (want ? ~uint64_t(0) : uint64_t(0)); }

// Material: ordinary values of every size and sign, plus the edges -- signed
// zeros, infinities, NaN, the largest and smallest normals, a denormal.
constexpr int kValues = 64;
float g_f[kValues];
double g_d[kValues];
int32_t g_i[kValues];

void fillMaterial() {
  uint32_t seed = 0x9E3779B9u;
  for (int i = 0; i < kValues; ++i) {
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    const double r = (double) (int32_t) seed / 2147483648.0;
    g_d[i] = r * std::pow(10.0, (double) ((i % 13) - 6));
    g_f[i] = (float) g_d[i];
    g_i[i] = (int32_t) seed;
  }
  const float edgesF[] = {0.0f, -0.0f, 1.0f, -1.0f, std::numeric_limits<float>::infinity(),
                          -std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN(),
                          std::numeric_limits<float>::max(), std::numeric_limits<float>::min(),
                          std::numeric_limits<float>::denorm_min(), 3.0f, 3.0f};
  const double edgesD[] = {0.0, -0.0, 1.0, -1.0, std::numeric_limits<double>::infinity(),
                           -std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN(),
                           std::numeric_limits<double>::max(), std::numeric_limits<double>::min(),
                           std::numeric_limits<double>::denorm_min(), 3.0, 3.0};
  const int32_t edgesI[] = {0, -1, 1, INT32_MAX, INT32_MIN, 65536, -65536, 16777217, 3, 3, 7, -7};
  for (int i = 0; i < 12; ++i) {
    g_f[i * 5 % kValues] = edgesF[i];
    g_d[i * 5 % kValues] = edgesD[i];
    g_i[i * 5 % kValues] = edgesI[i];
  }
}

// ── Float types: Vec4f and Vec8f ─────────────────────────────────────────────

template <typename V>
void testFloatType(const char* name) {
  using namespace sonore::simd;
  constexpr int W = V::width();
  bool add = true, sub = true, mul = true, div = true, sqr = true, mn = true, mx = true, ab = true;
  bool lt = true, gt = true, eq = true, sel = true, sm = true, ln = true, ldst = true;
  for (int start = 0; start + 2 * W <= kValues; ++start) {
    const float* a = g_f + start;
    const float* b = g_f + kValues - start - W;
    const V va = V::load(a), vb = V::load(b);
    float r[8];
    (va + vb).store(r);
    for (int i = 0; i < W; ++i) add = add && same(r[i], a[i] + b[i]);
    (va - vb).store(r);
    for (int i = 0; i < W; ++i) sub = sub && same(r[i], a[i] - b[i]);
    (va * vb).store(r);
    for (int i = 0; i < W; ++i) mul = mul && same(r[i], a[i] * b[i]);
    (va / vb).store(r);
    for (int i = 0; i < W; ++i) div = div && same(r[i], a[i] / b[i]);
    va.sqrt().store(r);
    for (int i = 0; i < W; ++i) sqr = sqr && same(r[i], std::sqrt(a[i]));
    (va.min)(vb).store(r);
    for (int i = 0; i < W; ++i) mn = mn && same(r[i], a[i] < b[i] ? a[i] : b[i]);
    (va.max)(vb).store(r);
    for (int i = 0; i < W; ++i) mx = mx && same(r[i], a[i] > b[i] ? a[i] : b[i]);
    va.abs().store(r);
    for (int i = 0; i < W; ++i) ab = ab && (bitsOf(r[i]) == (bitsOf(a[i]) & 0x7FFFFFFFu));
    va.lessThan(vb).store(r);
    for (int i = 0; i < W; ++i) lt = lt && sameMask(r[i], a[i] < b[i]);
    va.greaterThan(vb).store(r);
    for (int i = 0; i < W; ++i) gt = gt && sameMask(r[i], a[i] > b[i]);
    va.equalTo(vb).store(r);
    for (int i = 0; i < W; ++i) eq = eq && sameMask(r[i], a[i] == b[i]);
    V::select(va.lessThan(vb), va, vb).store(r);
    for (int i = 0; i < W; ++i) sel = sel && (bitsOf(r[i]) == bitsOf(a[i] < b[i] ? a[i] : b[i]));
    // sum: the documented order -- each four-lane group ((l0+l1)+(l2+l3)),
    // the groups added in turn.
    float want = 0.0f;
    for (int g = 0; g < W; g += 4) {
      const float group = (a[g] + a[g + 1]) + (a[g + 2] + a[g + 3]);
      want = g == 0 ? group : want + group;
    }
    sm = sm && same(va.sum(), want);
    for (int i = 0; i < W; ++i) ln = ln && (bitsOf(va.lane(i)) == bitsOf(a[i]));
    float back[8];
    va.store(back);
    for (int i = 0; i < W; ++i) ldst = ldst && (bitsOf(back[i]) == bitsOf(a[i]));
  }
  if constexpr (W == 4) {
    // The data movers: exact by construction, checked anyway.
    float p8[8], ev[4], od[4], back8[8], rev[4];
    for (int i = 0; i < 8; ++i) p8[i] = g_f[i * 7 % kValues];
    V even, odd;
    V::deinterleave(p8, even, odd);
    even.store(ev);
    odd.store(od);
    V::interleave(even, odd, back8);
    V::load(p8).reversed().store(rev);
    bool moved = true;
    for (int i = 0; i < 4; ++i)
      moved = moved && bitsOf(ev[i]) == bitsOf(p8[2 * i]) && bitsOf(od[i]) == bitsOf(p8[2 * i + 1]) &&
              bitsOf(rev[i]) == bitsOf(p8[3 - i]);
    for (int i = 0; i < 8; ++i) moved = moved && bitsOf(back8[i]) == bitsOf(p8[i]);
    char moveWhat[128];
    std::snprintf(moveWhat, sizeof moveWhat, "%s: reversed, deinterleave and interleave move every bit and change none", name);
    check(moved, moveWhat);
  }
  const V s = V::splat(-2.5f), z = V::zero();
  bool splat = true;
  for (int i = 0; i < W; ++i) splat = splat && s.lane(i) == -2.5f && bitsOf(z.lane(i)) == 0u;

  char what[160];
  std::snprintf(what, sizeof what, "%s: + - * / are IEEE, lane by lane, edges included", name);
  check(add && sub && mul && div, what);
  std::snprintf(what, sizeof what, "%s: sqrt is std::sqrt, abs clears exactly the sign bit", name);
  check(sqr && ab, what);
  std::snprintf(what, sizeof what, "%s: min and max are a<b?a:b and a>b?a:b, NaN included", name);
  check(mn && mx, what);
  std::snprintf(what, sizeof what, "%s: lessThan, greaterThan, equalTo give all-ones or all-zero lanes", name);
  check(lt && gt && eq, what);
  std::snprintf(what, sizeof what, "%s: select picks bits, not values", name);
  check(sel, what);
  std::snprintf(what, sizeof what, "%s: sum is ((l0+l1)+(l2+l3)) per four lanes, the groups added in order", name);
  check(sm, what);
  std::snprintf(what, sizeof what, "%s: load, store, lane, splat and zero round-trip every bit", name);
  check(ln && ldst && splat, what);
}

// ── Double types: Vec2d and Vec4d ────────────────────────────────────────────

template <typename V>
void testDoubleType(const char* name) {
  using namespace sonore::simd;
  constexpr int W = V::width();
  bool arith = true, sqr = true, mnmx = true, ab = true, cmp = true, sel = true, sm = true, ldst = true;
  for (int start = 0; start + 2 * W <= kValues; ++start) {
    const double* a = g_d + start;
    const double* b = g_d + kValues - start - W;
    const V va = V::load(a), vb = V::load(b);
    double r[4];
    (va + vb).store(r);
    for (int i = 0; i < W; ++i) arith = arith && same(r[i], a[i] + b[i]);
    (va - vb).store(r);
    for (int i = 0; i < W; ++i) arith = arith && same(r[i], a[i] - b[i]);
    (va * vb).store(r);
    for (int i = 0; i < W; ++i) arith = arith && same(r[i], a[i] * b[i]);
    (va / vb).store(r);
    for (int i = 0; i < W; ++i) arith = arith && same(r[i], a[i] / b[i]);
    va.sqrt().store(r);
    for (int i = 0; i < W; ++i) sqr = sqr && same(r[i], std::sqrt(a[i]));
    (va.min)(vb).store(r);
    for (int i = 0; i < W; ++i) mnmx = mnmx && same(r[i], a[i] < b[i] ? a[i] : b[i]);
    (va.max)(vb).store(r);
    for (int i = 0; i < W; ++i) mnmx = mnmx && same(r[i], a[i] > b[i] ? a[i] : b[i]);
    va.abs().store(r);
    for (int i = 0; i < W; ++i) ab = ab && (bitsOf(r[i]) == (bitsOf(a[i]) & 0x7FFFFFFFFFFFFFFFull));
    va.lessThan(vb).store(r);
    for (int i = 0; i < W; ++i) cmp = cmp && sameMask(r[i], a[i] < b[i]);
    va.greaterThan(vb).store(r);
    for (int i = 0; i < W; ++i) cmp = cmp && sameMask(r[i], a[i] > b[i]);
    va.equalTo(vb).store(r);
    for (int i = 0; i < W; ++i) cmp = cmp && sameMask(r[i], a[i] == b[i]);
    V::select(va.greaterThan(vb), va, vb).store(r);
    for (int i = 0; i < W; ++i) sel = sel && (bitsOf(r[i]) == bitsOf(a[i] > b[i] ? a[i] : b[i]));
    const double want = W == 2 ? a[0] + a[1] : (a[0] + a[1]) + (a[2] + a[3]);
    sm = sm && same(va.sum(), want);
    double back[4];
    va.store(back);
    for (int i = 0; i < W; ++i) ldst = ldst && bitsOf(back[i]) == bitsOf(a[i]) && bitsOf(va.lane(i)) == bitsOf(a[i]);
  }
  char what[160];
  std::snprintf(what, sizeof what, "%s: + - * / IEEE, sqrt std::sqrt, abs the sign bit", name);
  check(arith && sqr && ab, what);
  std::snprintf(what, sizeof what, "%s: min, max, the three compares and select, as the float types", name);
  check(mnmx && cmp && sel, what);
  std::snprintf(what, sizeof what, "%s: sum in its documented order; load, store and lane exact", name);
  check(sm && ldst, what);
}

// ── Vec4i ────────────────────────────────────────────────────────────────────

void testVec4i() {
  using namespace sonore::simd;
  bool arith = true, bits = true, shl = true, shr = true, shrl = true, cmp = true, mnmx = true;
  bool conv = true, sm = true;
  for (int start = 0; start + 8 <= kValues; ++start) {
    const int32_t* a = g_i + start;
    const int32_t* b = g_i + kValues - start - 4;
    const Vec4i va = Vec4i::load(a), vb = Vec4i::load(b);
    int32_t r[4];
    auto u = [](int32_t x) { return (uint32_t) x; };
    (va + vb).store(r);
    for (int i = 0; i < 4; ++i) arith = arith && u(r[i]) == u(a[i]) + u(b[i]);
    (va - vb).store(r);
    for (int i = 0; i < 4; ++i) arith = arith && u(r[i]) == u(a[i]) - u(b[i]);
    (va * vb).store(r);
    for (int i = 0; i < 4; ++i) arith = arith && u(r[i]) == u(a[i]) * u(b[i]);
    (va & vb).store(r);
    for (int i = 0; i < 4; ++i) bits = bits && u(r[i]) == (u(a[i]) & u(b[i]));
    (va | vb).store(r);
    for (int i = 0; i < 4; ++i) bits = bits && u(r[i]) == (u(a[i]) | u(b[i]));
    (va ^ vb).store(r);
    for (int i = 0; i < 4; ++i) bits = bits && u(r[i]) == (u(a[i]) ^ u(b[i]));
    for (int n : {0, 1, 5, 31, 32, 33, 63, -1}) {
      const int m = n & 31;
      va.shiftLeft(n).store(r);
      for (int i = 0; i < 4; ++i) shl = shl && u(r[i]) == (u(a[i]) << m);
      va.shiftRight(n).store(r);
      for (int i = 0; i < 4; ++i) {
        // Arithmetic: floor division by 2^m, which is what copying the sign
        // bit in amounts to.
        const int64_t want = (int64_t) std::floor((double) a[i] / std::ldexp(1.0, m));
        shr = shr && (int64_t) r[i] == want;
      }
      va.shiftRightLogical(n).store(r);
      for (int i = 0; i < 4; ++i) shrl = shrl && u(r[i]) == (u(a[i]) >> m);
    }
    va.lessThan(vb).store(r);
    for (int i = 0; i < 4; ++i) cmp = cmp && r[i] == (a[i] < b[i] ? -1 : 0);
    va.greaterThan(vb).store(r);
    for (int i = 0; i < 4; ++i) cmp = cmp && r[i] == (a[i] > b[i] ? -1 : 0);
    va.equalTo(vb).store(r);
    for (int i = 0; i < 4; ++i) cmp = cmp && r[i] == (a[i] == b[i] ? -1 : 0);
    Vec4i::select(va.lessThan(vb), vb, va).store(r);
    for (int i = 0; i < 4; ++i) cmp = cmp && r[i] == (a[i] < b[i] ? b[i] : a[i]);
    (va.min)(vb).store(r);
    for (int i = 0; i < 4; ++i) mnmx = mnmx && r[i] == (a[i] < b[i] ? a[i] : b[i]);
    (va.max)(vb).store(r);
    for (int i = 0; i < 4; ++i) mnmx = mnmx && r[i] == (a[i] > b[i] ? a[i] : b[i]);
    float f[4];
    va.toFloat().store(f);
    for (int i = 0; i < 4; ++i) conv = conv && bitsOf(f[i]) == bitsOf((float) a[i]);
    sm = sm && u(va.sum()) == u(a[0]) + u(a[1]) + u(a[2]) + u(a[3]);
  }
  check(arith, "Vec4i: + - * wrap modulo 2^32, the product's low 32 bits (SSE2's built from widening multiplies)");
  check(bits, "Vec4i: & | ^ bit for bit");
  check(shl && shr && shrl, "Vec4i: shifts count modulo 32 on every unit; >> copies the sign, the logical one does not");
  check(cmp && mnmx, "Vec4i: signed compares, select, min and max");
  check(conv && sm, "Vec4i: toFloat rounds as a cast does, past 2^24 too; sum wraps");
}


// ── Vec4i's unsigned forms ───────────────────────────────────────────────────

void testVec4iUnsigned() {
  using namespace sonore::simd;
  bool ok = true;
  for (int start = 0; start + 8 <= kValues; ++start) {
    const int32_t* a = g_i + start;
    const int32_t* b = g_i + kValues - start - 4;
    const Vec4i va = Vec4i::load(a), vb = Vec4i::load(b);
    int32_t lt[4], gt[4], mn[4], mx[4];
    va.lessThanUnsigned(vb).store(lt);
    va.greaterThanUnsigned(vb).store(gt);
    va.minUnsigned(vb).store(mn);
    va.maxUnsigned(vb).store(mx);
    for (int i = 0; i < 4; ++i) {
      const uint32_t x = (uint32_t) a[i], y = (uint32_t) b[i];
      ok = ok && lt[i] == (x < y ? -1 : 0) && gt[i] == (x > y ? -1 : 0) && (uint32_t) mn[i] == (x < y ? x : y) &&
           (uint32_t) mx[i] == (x > y ? x : y);
    }
  }
  check(ok, "Vec4i: unsigned compares, min and max (0xFFFFFFFF the largest, not -1)");
}

// ── Vec16i8, Vec8i16, Vec2i64 ────────────────────────────────────────────────

int8_t g_i8[kValues];
int16_t g_i16[kValues];
int64_t g_i64[kValues];

void fillIntMaterial() {
  uint64_t seed = 0x9E3779B97F4A7C15ull;
  for (int i = 0; i < kValues; ++i) {
    seed ^= seed << 13;
    seed ^= seed >> 7;
    seed ^= seed << 17;
    g_i8[i] = (int8_t) (uint8_t) seed;
    g_i16[i] = (int16_t) (uint16_t) (seed >> 8);
    g_i64[i] = (int64_t) seed;
  }
  const int8_t e8[] = {0, -1, 1, 127, -128, -127, 64, -64, 3, 3, 100, -100};
  const int16_t e16[] = {0, -1, 1, 32767, -32768, -32767, 256, -256, 3, 3, 30000, -30000};
  const int64_t e64[] = {0, -1, 1, INT64_MAX, INT64_MIN, INT64_MIN + 1, (int64_t) 1 << 32, -((int64_t) 1 << 32),
                         3, 3, 0xFFFFFFFFll, -0xFFFFFFFFll};
  for (int i = 0; i < 12; ++i) {
    g_i8[i * 5 % kValues] = e8[i];
    g_i16[i * 5 % kValues] = e16[i];
    g_i64[i * 5 % kValues] = e64[i];
  }
}
const int8_t* material(int8_t) { return g_i8; }
const int16_t* material(int16_t) { return g_i16; }
const int64_t* material(int64_t) { return g_i64; }

template <typename V>
void testIntType(const char* name) {
  using L = typename V::Lane;
  using U = typename std::make_unsigned<L>::type;
  constexpr int W = V::width();
  constexpr int B = 8 * (int) sizeof(L);
  const L* mat = material(L());
  // The reference, each written from the definition rather than as the
  // header writes it: wrapping is modulo 2^B, so do it in 64 bits and cut.
  auto cut = [](uint64_t v) { return (L) (U) v; };
  bool arith = true, bits = true, shl = true, shr = true, shrl = true, cmp = true, ucmp = true, mnmx = true;
  bool sat = true, sm = true, ldst = true;
  for (int start = 0; start + 2 * W <= kValues; ++start) {
    const L* a = mat + start;
    const L* b = mat + kValues - start - W;
    const V va = V::load(a), vb = V::load(b);
    L r[16];
    auto u64 = [](L x) { return (uint64_t) (U) x; };
    (va + vb).store(r);
    for (int i = 0; i < W; ++i) arith = arith && r[i] == cut(u64(a[i]) + u64(b[i]));
    (va - vb).store(r);
    for (int i = 0; i < W; ++i) arith = arith && r[i] == cut(u64(a[i]) - u64(b[i]));
    (va * vb).store(r);
    for (int i = 0; i < W; ++i) arith = arith && r[i] == cut(u64(a[i]) * u64(b[i]));
    (va & vb).store(r);
    for (int i = 0; i < W; ++i) bits = bits && r[i] == cut(u64(a[i]) & u64(b[i]));
    (va | vb).store(r);
    for (int i = 0; i < W; ++i) bits = bits && r[i] == cut(u64(a[i]) | u64(b[i]));
    (va ^ vb).store(r);
    for (int i = 0; i < W; ++i) bits = bits && r[i] == cut(u64(a[i]) ^ u64(b[i]));
    for (int n : {0, 1, 3, B - 1, B, B + 1, 2 * B - 1, -1, 37}) {
      const int m = n & (B - 1);
      va.shiftLeft(n).store(r);
      for (int i = 0; i < W; ++i) shl = shl && r[i] == cut(u64(a[i]) << m);
      va.shiftRightLogical(n).store(r);
      for (int i = 0; i < W; ++i) shrl = shrl && r[i] == cut(u64(a[i]) >> m);
      va.shiftRight(n).store(r);
      for (int i = 0; i < W; ++i) {
        // Floor division by 2^m; for a negative a that is ~(~a >> m), with ~a
        // non-negative so the shift is an ordinary one.
        const L want = a[i] >= 0 ? cut(u64(a[i]) >> m) : cut(~(u64((L) ~a[i]) >> m));
        shr = shr && r[i] == want;
      }
    }
    va.lessThan(vb).store(r);
    for (int i = 0; i < W; ++i) cmp = cmp && r[i] == (a[i] < b[i] ? (L) -1 : (L) 0);
    va.greaterThan(vb).store(r);
    for (int i = 0; i < W; ++i) cmp = cmp && r[i] == (a[i] > b[i] ? (L) -1 : (L) 0);
    va.equalTo(vb).store(r);
    for (int i = 0; i < W; ++i) cmp = cmp && r[i] == (a[i] == b[i] ? (L) -1 : (L) 0);
    va.equalTo(va).store(r);
    for (int i = 0; i < W; ++i) cmp = cmp && r[i] == (L) -1;
    V::select(va.lessThan(vb), vb, va).store(r);
    for (int i = 0; i < W; ++i) cmp = cmp && r[i] == (a[i] < b[i] ? b[i] : a[i]);
    va.lessThanUnsigned(vb).store(r);
    for (int i = 0; i < W; ++i) ucmp = ucmp && r[i] == ((U) a[i] < (U) b[i] ? (L) -1 : (L) 0);
    va.greaterThanUnsigned(vb).store(r);
    for (int i = 0; i < W; ++i) ucmp = ucmp && r[i] == ((U) a[i] > (U) b[i] ? (L) -1 : (L) 0);
    (va.min)(vb).store(r);
    for (int i = 0; i < W; ++i) mnmx = mnmx && r[i] == (a[i] < b[i] ? a[i] : b[i]);
    (va.max)(vb).store(r);
    for (int i = 0; i < W; ++i) mnmx = mnmx && r[i] == (a[i] > b[i] ? a[i] : b[i]);
    va.minUnsigned(vb).store(r);
    for (int i = 0; i < W; ++i) mnmx = mnmx && (U) r[i] == ((U) a[i] < (U) b[i] ? (U) a[i] : (U) b[i]);
    va.maxUnsigned(vb).store(r);
    for (int i = 0; i < W; ++i) mnmx = mnmx && (U) r[i] == ((U) a[i] > (U) b[i] ? (U) a[i] : (U) b[i]);
    if constexpr (sizeof(L) <= 2) {
      const int64_t lo = -((int64_t) 1 << (B - 1)), hi = ((int64_t) 1 << (B - 1)) - 1, uhi = ((int64_t) 1 << B) - 1;
      auto clamp = [](int64_t v, int64_t l, int64_t h) { return v < l ? l : (v > h ? h : v); };
      va.addSaturate(vb).store(r);
      for (int i = 0; i < W; ++i) sat = sat && r[i] == (L) clamp((int64_t) a[i] + b[i], lo, hi);
      va.subSaturate(vb).store(r);
      for (int i = 0; i < W; ++i) sat = sat && r[i] == (L) clamp((int64_t) a[i] - b[i], lo, hi);
      va.addSaturateUnsigned(vb).store(r);
      for (int i = 0; i < W; ++i) sat = sat && (U) r[i] == (U) clamp((int64_t) (U) a[i] + (U) b[i], 0, uhi);
      va.subSaturateUnsigned(vb).store(r);
      for (int i = 0; i < W; ++i) sat = sat && (U) r[i] == (U) clamp((int64_t) (U) a[i] - (U) b[i], 0, uhi);
    }
    uint64_t s = 0;
    for (int i = 0; i < W; ++i) s += u64(a[i]);
    sm = sm && va.sum() == cut(s);
    va.store(r);
    for (int i = 0; i < W; ++i) ldst = ldst && r[i] == a[i] && va.lane(i) == a[i];
    ldst = ldst && V::splat(a[0]).lane(W - 1) == a[0] && V::zero().sum() == 0;
  }
  char what[200];
  std::snprintf(what, sizeof what, "%s: + - * wrap modulo 2^%d (the product's low bits), & | ^ bit for bit", name, B);
  check(arith && bits, what);
  std::snprintf(what, sizeof what, "%s: shifts count modulo %d; >> copies the sign, the logical one does not", name, B);
  check(shl && shr && shrl, what);
  std::snprintf(what, sizeof what, "%s: signed and unsigned compares, select, min and max in both readings", name);
  check(cmp && ucmp && mnmx, what);
  if constexpr (sizeof(L) <= 2) {
    std::snprintf(what, sizeof what, "%s: saturating + and -, signed and unsigned, held to the lane's range", name);
    check(sat, what);
  }
  std::snprintf(what, sizeof what, "%s: sum wraps; load, store, splat, zero and lane exact", name);
  check(sm && ldst, what);
}

void testIntConversions() {
  using namespace sonore::simd;
  bool w16 = true, n16 = true, w8 = true, n8 = true, w64 = true, dbl = true;
  for (int start = 0; start + 16 <= kValues; ++start) {
    Vec4i lo, hi;
    widen(Vec8i16::load(g_i16 + start), lo, hi);
    for (int i = 0; i < 4; ++i) w16 = w16 && lo.lane(i) == g_i16[start + i] && hi.lane(i) == g_i16[start + 4 + i];
    const Vec8i16 n = narrowSaturate(Vec4i::load(g_i + start), Vec4i::load(g_i + start + 4));
    for (int i = 0; i < 8; ++i) {
      const int32_t x = g_i[start + i];
      n16 = n16 && n.lane(i) == (int16_t) (x < -32768 ? -32768 : (x > 32767 ? 32767 : x));
    }
    Vec8i16 b0, b1;
    widen(Vec16i8::load(g_i8 + start), b0, b1);
    for (int i = 0; i < 8; ++i) w8 = w8 && b0.lane(i) == g_i8[start + i] && b1.lane(i) == g_i8[start + 8 + i];
    const Vec16i8 m = narrowSaturate(Vec8i16::load(g_i16 + start), Vec8i16::load(g_i16 + start + 8));
    for (int i = 0; i < 16; ++i) {
      const int16_t x = g_i16[start + i];
      n8 = n8 && m.lane(i) == (int8_t) (x < -128 ? -128 : (x > 127 ? 127 : x));
    }
    const Vec4i v = Vec4i::load(g_i + start);
    const Vec2i64 l64 = widenLow(v), h64 = widenHigh(v);
    w64 = w64 && l64.lane(0) == g_i[start] && l64.lane(1) == g_i[start + 1] && h64.lane(0) == g_i[start + 2] &&
          h64.lane(1) == g_i[start + 3];
    const Vec2d d = toDouble(Vec2i64::load(g_i64 + start));
    dbl = dbl && bitsOf(d.lane(0)) == bitsOf((double) g_i64[start]) &&
          bitsOf(d.lane(1)) == bitsOf((double) g_i64[start + 1]);
  }
  check(w16 && w8 && w64, "widen / widenLow / widenHigh: int8 -> int16 -> int32 -> int64 sign-extended exactly");
  check(n16 && n8, "narrowSaturate: int32 -> int16 and int16 -> int8 held to the smaller range, never wrapped");
  check(dbl, "toDouble(Vec2i64): rounded as a cast rounds, past 2^53 too");
  // 16-bit PCM to float, exactly, through the integer widths.
  const int16_t pcm[8] = {0, 1, -1, 32767, -32768, 12345, -54, 256};
  Vec4i lo, hi;
  widen(Vec8i16::load(pcm), lo, hi);
  float f[8];
  (lo.toFloat() * Vec4f::splat(1.0f / 32768.0f)).store(f);
  (hi.toFloat() * Vec4f::splat(1.0f / 32768.0f)).store(f + 4);
  bool pcmOk = true;
  for (int i = 0; i < 8; ++i) pcmOk = pcmOk && bitsOf(f[i]) == bitsOf((float) pcm[i] / 32768.0f);
  check(pcmOk, "16-bit PCM to float through widen() and toFloat(): the bits of the scalar conversion");
}

// ── Vec4cf, Vec4cd ───────────────────────────────────────────────────────────

template <typename T>
void testComplexType(const char* name) {
  using namespace sonore::simd;
  using C = ComplexVec4T<T>;
  const T* mat = nullptr;
  if constexpr (std::is_same<T, float>::value) mat = g_f;
  else mat = g_d;
  bool arith = true, mul = true, misc = true, ldst = true;
  for (int start = 0; start + 16 <= kValues; ++start) {
    std::complex<T> a[4], b[4], r[4];
    for (int i = 0; i < 4; ++i) {
      a[i] = std::complex<T>(mat[start + 2 * i], mat[start + 2 * i + 1]);
      b[i] = std::complex<T>(mat[kValues - 1 - start - 2 * i], mat[kValues - 2 - start - 2 * i]);
    }
    const C va = C::load(a), vb = C::load(b);
    (va + vb).store(r);
    for (int i = 0; i < 4; ++i)
      arith = arith && same(r[i].real(), a[i].real() + b[i].real()) && same(r[i].imag(), a[i].imag() + b[i].imag());
    (va - vb).store(r);
    for (int i = 0; i < 4; ++i)
      arith = arith && same(r[i].real(), a[i].real() - b[i].real()) && same(r[i].imag(), a[i].imag() - b[i].imag());
    (va * vb).store(r);
    for (int i = 0; i < 4; ++i) {
      const T ar = a[i].real(), ai = a[i].imag(), br = b[i].real(), bi = b[i].imag();
      const T pr = ar * br, qr = ai * bi, pi = ar * bi, qi = ai * br;
      mul = mul && same(r[i].real(), pr - qr) && same(r[i].imag(), pi + qi);
    }
    T g[4], re[4], im[4];
    for (int i = 0; i < 4; ++i) g[i] = mat[(start * 3 + i) % kValues];
    va.scale(C::Half::load(g)).store(r);
    for (int i = 0; i < 4; ++i)
      misc = misc && same(r[i].real(), a[i].real() * g[i]) && same(r[i].imag(), a[i].imag() * g[i]);
    va.conj().store(r);
    for (int i = 0; i < 4; ++i) misc = misc && same(r[i].real(), a[i].real()) && same(r[i].imag(), a[i].imag() * T(-1.0));
    va.normSquared().store(g);
    for (int i = 0; i < 4; ++i) {
      const T x = a[i].real() * a[i].real(), y = a[i].imag() * a[i].imag();
      misc = misc && same(g[i], x + y);
    }
    const std::complex<T> s = va.sum();
    misc = misc && same(s.real(), (a[0].real() + a[1].real()) + (a[2].real() + a[3].real())) &&
           same(s.imag(), (a[0].imag() + a[1].imag()) + (a[2].imag() + a[3].imag()));
    va.storeSplit(re, im);
    const C back = C::loadSplit(re, im);
    back.store(r);
    for (int i = 0; i < 4; ++i)
      ldst = ldst && bitsOf(re[i]) == bitsOf(a[i].real()) && bitsOf(im[i]) == bitsOf(a[i].imag()) &&
             bitsOf(r[i].real()) == bitsOf(a[i].real()) && bitsOf(r[i].imag()) == bitsOf(a[i].imag()) &&
             bitsOf(va.lane(i).real()) == bitsOf(a[i].real()) && bitsOf(va.real().lane(i)) == bitsOf(a[i].real()) &&
             bitsOf(va.imag().lane(i)) == bitsOf(a[i].imag());
  }
  const C z = C::splat(std::complex<T>(T(0.0), T(0.0))).conj();
  misc = misc && std::signbit(z.lane(2).imag()) && C::zero().sum() == std::complex<T>(0, 0);
  char what[200];
  std::snprintf(what, sizeof what, "%s: + and - lane by lane, IEEE in each part", name);
  check(arith, what);
  std::snprintf(what, sizeof what, "%s: the product is (ac - bd) + (ad + bc)i in that order, bit for bit", name);
  check(mul, what);
  std::snprintf(what, sizeof what, "%s: scale, conj (+0 to -0, as std::conj), normSquared, sum in Half's order", name);
  check(misc, what);
  std::snprintf(what, sizeof what, "%s: load/store interleaved, loadSplit/storeSplit, lane, real, imag exact", name);
  check(ldst, what);
}

} // namespace

int main() {
  std::printf("backend %s, wide %s\n", sonore::simd::backend(), sonore::simd::wideBackend());
  fillMaterial();
  testFloatType<sonore::simd::Vec4f>("Vec4f");
  testFloatType<sonore::simd::Vec8f>("Vec8f");
  testDoubleType<sonore::simd::Vec2d>("Vec2d");
  testDoubleType<sonore::simd::Vec4d>("Vec4d");
  testVec4i();
  testVec4iUnsigned();
  fillIntMaterial();
  testIntType<sonore::simd::Vec16i8>("Vec16i8");
  testIntType<sonore::simd::Vec8i16>("Vec8i16");
  testIntType<sonore::simd::Vec2i64>("Vec2i64");
  testIntConversions();
  testComplexType<float>("Vec4cf");
  testComplexType<double>("Vec4cd");
  std::printf("\n%d checks, %d failed\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
