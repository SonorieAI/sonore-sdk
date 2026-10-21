// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: the output guard: a block with NaN or infinity in it leaves as
// silence.
//
// ── Why ──────────────────────────────────────────────────────────────────────
//
// A DSP bug -- a filter driven unstable, a division by zero, a logarithm of
// zero -- makes NaN or infinity, and both are sticky: every recursive filter,
// smoother and feedback path the value reaches keeps it for good. A host
// passes it on, and what happens next is the host's business and never good.
// A device converting it to an integer plays a full-scale burst or a click; a
// mix bus sums it into every other track, so one plugin takes the whole
// session with it -- the limiter on the master included, which is the plugin
// a user reaches for when something sounds wrong.
//
// So every format checks the block it is about to hand back, on every output
// port the host sees. A block with one non-finite sample anywhere becomes
// silence on EVERY port: a channel with a NaN in it holds nothing worth
// keeping, and half a stereo image is a louder surprise than none. The DSP is
// not reset. Its state is the plugin's own, a reset is the plugin's decision
// to take, and a filter that recovers plays its next finite block as normal.
// The first time, the wrapper asks for the main thread and says so there,
// once -- in the host's log where the host has one, on stderr otherwise --
// because a silent fix for a bug nobody hears about is a bug that ships.
//
// ── The check reads the bits ─────────────────────────────────────────────────
//
// The obvious test accumulates x * 0 over the block -- 0 for every finite x,
// NaN for a NaN and for either infinity -- and asks whether the sum is NaN.
// Under -ffast-math or /fp:fast that is no test at all: the compiler may fold
// x * 0 to 0 and s != s to false, and the guard compiles to nothing in
// exactly the builds an audio developer is most likely to ship. MSVC's
// /fp:fast does exactly that: the arithmetic form, fed a NaN and then an
// infinity, called both blocks clean. A float is non-finite exactly when its
// exponent bits are all ones, which is (bits & 0x7fffffff) > 0x7f7fffff, and
// no floating-point flag rewrites an integer compare. It is cheaper as well:
// an and, a compare and an or per four samples, single-cycle integer
// operations, no branch until the block ends. The arithmetic form is a chain
// of dependent float adds that a compiler may not reorder without fast-math,
// so it does not vectorise: 355 ns for 512 samples where this takes 38.
//
// ── What it costs ────────────────────────────────────────────────────────────
//
// Measured on the machine this was written on (Ryzen 7 4700U, MSVC /O2, SSE2;
// stereo, the check alone against the example's DSP alone and against the
// CLAP wrapper's whole process() call, the guard included):
//
//     frames   guard (64-bit)    trim DSP / process()    saturator DSP / process()
//         32    12 ns  (19 ns)     153 ns /   495 ns        4.0 us /  4.3 us
//         64    16 ns  (29 ns)     303 ns /   612 ns        9.2 us /  9.9 us
//        512    83 ns (177 ns)    2.55 us /  3.14 us       55.4 us / 59.7 us
//
// 2.4-2.7% of process() for trim, a smoothed gain and about the least a
// plugin can do; 0.14-0.31% for the saturator, an oversampled waveshaper.
// A plugin that wants to see its own NaN -- a test rig, an analyser of broken
// signals -- sets PluginDescriptor::guardOutput = false and no process path
// runs the check.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "audio.h"
#include "simd.h"

namespace sonore {
namespace detail {

/** [audio-thread] True when every sample in [data, data + n) is finite.
 *
 *  Branch-free over the block: the verdict is OR-ed lane by lane and read
 *  once at the end, so a clean block -- the only kind a working plugin makes
 *  -- costs the same whatever it holds. */
inline bool samplesFinite(const float* data, size_t n) {
  size_t i = 0;
  bool bad = false;
#if defined(SONORE_SIMD_SSE)
  const __m128i magnitude = _mm_set1_epi32(0x7fffffff);
  const __m128i largest = _mm_set1_epi32(0x7f7fffff); // FLT_MAX's bits
  __m128i a = _mm_setzero_si128(), b = _mm_setzero_si128();
  for (; i + 8 <= n; i += 8) {
    // Two accumulators, so the loop is bound by loads rather than by one OR
    // waiting on the last.
    const __m128i x = _mm_and_si128(_mm_castps_si128(_mm_loadu_ps(data + i)), magnitude);
    const __m128i y = _mm_and_si128(_mm_castps_si128(_mm_loadu_ps(data + i + 4)), magnitude);
    a = _mm_or_si128(a, _mm_cmpgt_epi32(x, largest));
    b = _mm_or_si128(b, _mm_cmpgt_epi32(y, largest));
  }
  bad = _mm_movemask_epi8(_mm_or_si128(a, b)) != 0;
#elif defined(SONORE_SIMD_NEON)
  const uint32x4_t magnitude = vdupq_n_u32(0x7fffffffu);
  const uint32x4_t largest = vdupq_n_u32(0x7f7fffffu);
  uint32x4_t a = vdupq_n_u32(0u), b = vdupq_n_u32(0u);
  for (; i + 8 <= n; i += 8) {
    const uint32x4_t x = vandq_u32(vreinterpretq_u32_f32(vld1q_f32(data + i)), magnitude);
    const uint32x4_t y = vandq_u32(vreinterpretq_u32_f32(vld1q_f32(data + i + 4)), magnitude);
    a = vorrq_u32(a, vcgtq_u32(x, largest));
    b = vorrq_u32(b, vcgtq_u32(y, largest));
  }
  const uint32x4_t any = vorrq_u32(a, b);
  const uint32x2_t half = vorr_u32(vget_low_u32(any), vget_high_u32(any));
  bad = (vget_lane_u32(half, 0) | vget_lane_u32(half, 1)) != 0u;
#elif defined(SONORE_SIMD_WASM)
  const v128_t magnitude = wasm_i32x4_splat(0x7fffffff);
  const v128_t largest = wasm_i32x4_splat(0x7f7fffff);
  v128_t a = wasm_i32x4_splat(0), b = wasm_i32x4_splat(0);
  for (; i + 8 <= n; i += 8) {
    const v128_t x = wasm_v128_and(wasm_v128_load(data + i), magnitude);
    const v128_t y = wasm_v128_and(wasm_v128_load(data + i + 4), magnitude);
    a = wasm_v128_or(a, wasm_i32x4_gt(x, largest));
    b = wasm_v128_or(b, wasm_i32x4_gt(y, largest));
  }
  const v128_t any = wasm_v128_or(a, b);
  bad = (wasm_i32x4_extract_lane(any, 0) | wasm_i32x4_extract_lane(any, 1) |
         wasm_i32x4_extract_lane(any, 2) | wasm_i32x4_extract_lane(any, 3)) != 0;
#endif
  // The tail, and the whole block on a build with no vector unit
  // (SONORE_NO_SIMD): the same test one sample at a time, still no branch.
  uint32_t tail = 0;
  for (; i < n; ++i) {
    uint32_t bits;
    std::memcpy(&bits, data + i, sizeof(bits));
    tail |= (uint32_t) ((bits & 0x7fffffffu) > 0x7f7fffffu);
  }
  return !bad && tail == 0;
}

/** [audio-thread] The same for doubles: non-finite exactly when the exponent
 *  is all ones, (bits & 0x7fff...) > DBL_MAX's bits. */
inline bool samplesFinite(const double* data, size_t n) {
  size_t i = 0;
  bool bad = false;
#if defined(SONORE_SIMD_SSE)
  // SSE2 has no 64-bit compare, and needs none: a double's exponent lives in
  // its HIGH 32 bits, non-finite exactly when (hi & 0x7fffffff) > 0x7fefffff.
  // The low words are masked to zero, and zero is never greater.
  const __m128i magnitude = _mm_set_epi32(0x7fffffff, 0, 0x7fffffff, 0);
  const __m128i largest = _mm_set_epi32(0x7fefffff, 0, 0x7fefffff, 0);
  __m128i a = _mm_setzero_si128(), b = _mm_setzero_si128();
  for (; i + 4 <= n; i += 4) {
    const __m128i x = _mm_and_si128(_mm_castpd_si128(_mm_loadu_pd(data + i)), magnitude);
    const __m128i y = _mm_and_si128(_mm_castpd_si128(_mm_loadu_pd(data + i + 2)), magnitude);
    a = _mm_or_si128(a, _mm_cmpgt_epi32(x, largest));
    b = _mm_or_si128(b, _mm_cmpgt_epi32(y, largest));
  }
  bad = _mm_movemask_epi8(_mm_or_si128(a, b)) != 0;
#endif
  // NEON and wasm take this loop for doubles: the 64-bit path is the rare one,
  // and both compilers vectorise a loop of integer ORs on their own.
  uint64_t tail = 0;
  for (; i < n; ++i) {
    uint64_t bits;
    std::memcpy(&bits, data + i, sizeof(bits));
    tail |= (uint64_t) ((bits & 0x7fffffffffffffffull) > 0x7fefffffffffffffull);
  }
  return !bad && tail == 0;
}

/**
 * One plugin instance's guard: the check over every port a block hands back,
 * and what is owed to the main thread after a trip.
 *
 * Two atomics and nothing else, so a wrapper's Instance stays a type nothing
 * has to construct specially; copying one copies the counts (nothing copies an
 * instance that is running).
 */
struct OutputGuard {
  /** Blocks silenced so far. Written by the audio thread, read by the main
   *  one for the report and by anything that wants to know (--verify). */
  std::atomic<uint32_t> trips{0};
  /** The report has been made. Set by whichever main-thread hook gets there
   *  first, and never cleared: once per instance is the promise. */
  std::atomic<bool> reported{false};

  OutputGuard() = default;
  OutputGuard(const OutputGuard& o)
      : trips(o.trips.load(std::memory_order_relaxed)),
        reported(o.reported.load(std::memory_order_relaxed)) {}
  OutputGuard& operator=(const OutputGuard& o) {
    trips.store(o.trips.load(std::memory_order_relaxed), std::memory_order_relaxed);
    reported.store(o.reported.load(std::memory_order_relaxed), std::memory_order_relaxed);
    return *this;
  }

  /**
   * [audio-thread] Check a block's outputs -- the main bus's channels and
   * every aux block -- and silence all of them if any sample is not finite.
   *
   * Returns true on the FIRST trip of this instance's life, which is the
   * caller's cue to ask for the main thread (request_callback in CLAP); the
   * trips after it are counted and say nothing more. Allocates nothing, takes
   * no lock, and leaves a clean block's samples exactly as they were.
   */
  template <typename Sample>
  bool check(Sample* const* main, uint32_t numMain, const AudioBlock<Sample>* aux,
             uint32_t numAux, size_t frames) {
    bool finite = true;
    for (uint32_t c = 0; c < numMain && finite; ++c)
      if (main[c]) finite = samplesFinite(main[c], frames);
    for (uint32_t b = 0; b < numAux && finite; ++b)
      for (size_t c = 0; c < aux[b].getNumChannels() && finite; ++c)
        if (const Sample* p = aux[b].getChannelPointer(c))
          finite = samplesFinite(p, aux[b].getNumSamples());
    if (finite) return false;

    for (uint32_t c = 0; c < numMain; ++c)
      if (main[c]) std::memset(main[c], 0, sizeof(Sample) * frames);
    for (uint32_t b = 0; b < numAux; ++b)
      for (size_t c = 0; c < aux[b].getNumChannels(); ++c)
        if (Sample* p = aux[b].getChannelPointer(c))
          std::memset(p, 0, sizeof(Sample) * aux[b].getNumSamples());
    return trips.fetch_add(1, std::memory_order_acq_rel) == 0;
  }

  /** [main-thread] The report, the first time it is asked for after a trip:
   *  writes it into `out` and returns true. False -- and nothing written --
   *  before any trip and once it has been made. One line, no newline: a
   *  host's log adds its own. */
  bool takeReport(const char* pluginName, char* out, size_t capacity) {
    if (!out || capacity == 0) return false;
    const uint32_t n = trips.load(std::memory_order_acquire);
    if (n == 0 || reported.exchange(true, std::memory_order_acq_rel)) return false;
    std::snprintf(out, capacity,
                  "sonore: %s produced NaN or infinity; the output guard played %u block%s of "
                  "it as silence. That is a bug in the DSP (an unstable filter, a division by "
                  "zero). Said once; PluginDescriptor::guardOutput = false turns the guard off.",
                  pluginName && pluginName[0] ? pluginName : "a plugin", (unsigned) n,
                  n == 1 ? "" : "s");
    return true;
  }
};

} // namespace detail
} // namespace sonore
