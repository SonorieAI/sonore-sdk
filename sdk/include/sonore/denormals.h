// SPDX-License-Identifier: Apache-2.0
//
// ScopedNoDenormals: turn off denormal arithmetic for the length of a block.
//
// ── What this costs when it is missing ──────────────────────────────────────
//
// A denormal is a floating-point number too small to represent normally, and
// on x86 the hardware handles them in microcode rather than in the pipeline.
// The result is not a rounding difference; it is a stall.
//
// Measured on the machine this was written on, eight biquads decaying through
// silence:
//
//     denormals allowed :   5.80 ms
//     FTZ + DAZ set     :   2.06 ms
//     ratio             :   2.82x
//
// Nearly three times the CPU, for audio nobody can hear -- the tail of a
// filter forty decades below full scale. And it appears exactly when a track
// goes quiet, so the plugin that spikes is the one that was doing nothing.
//
// ── Why the per-sample flush is not enough ──────────────────────────────────
//
// dsp.h has flushDenormal(), and the SDK's own filters call it. That protects
// the code that remembers to call it. It does nothing for a DSP somebody
// generates tomorrow, and the whole point of this SDK is that the DSP is
// written by something other than a person who knows about denormals.
//
// A processor-wide flag protects everything in the block, including the
// arithmetic nobody thought about.
//
// ── Why it is scoped ────────────────────────────────────────────────────────
//
// The audio thread belongs to the HOST. Leaving its floating-point mode
// changed after returning would alter arithmetic in code we do not own -- and
// silently, since flush-to-zero changes results rather than failing. Saved on
// entry, restored on exit, including when the block throws.
#pragma once

#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#define SONORE_DENORMALS_SSE 1
#include <xmmintrin.h>
#elif defined(__aarch64__) || defined(_M_ARM64)
#define SONORE_DENORMALS_ARM64 1
// cl.exe has NO inline assembly on ARM64 -- the `mrs`/`msr` below is a hard
// error there, so every plugin built with Visual Studio for Windows on Arm
// failed to compile, and nothing here noticed because this box has no ARM64
// CRT to build against. The MSVC-flavoured compilers (cl and clang-cl both
// define _MSC_VER) go through the status-register intrinsics instead. The
// register number is Arm's encoding of FPCR (op0 3, op1 3, CRn 4, CRm 4,
// op2 0 -> 0x5A20), spelled out because clang's <intrin.h> declares the
// intrinsics without naming the register.
#if defined(_MSC_VER)
#define SONORE_DENORMALS_ARM64_MSVC 1
#include <intrin.h>
#ifndef ARM64_FPCR
#define ARM64_FPCR 0x5A20
#endif
#endif
#endif

#include <cstdint>

namespace sonore {

/**
 * Flush-to-zero for as long as this object lives.
 *
 * Declare one at the top of a process block. It is a handful of cycles to set
 * and a handful to restore, against a stall of hundreds of cycles per denormal
 * operation.
 */
class ScopedNoDenormals {
public:
  ScopedNoDenormals() : saved_(readState()) { writeState(flushingState(saved_)); }
  ~ScopedNoDenormals() { writeState(saved_); }

  ScopedNoDenormals(const ScopedNoDenormals&) = delete;
  ScopedNoDenormals& operator=(const ScopedNoDenormals&) = delete;

  /** What the control register held before this scope. Public so a test can
   *  assert the mode was really changed and really put back -- a guard that
   *  silently did nothing would look exactly like one that worked. */
  uintptr_t previous() const { return saved_; }

  /**
   * The MODE bits of a saved state, with the sticky exception-status flags
   * masked off.
   *
   * MXCSR carries two unrelated things in one register: the control bits this
   * guard manages (flush-to-zero, denormals-are-zero, rounding, exception
   * masks) and, in its low 6 bits, the STICKY status flags the hardware sets
   * as a side effect of arithmetic -- underflow, denormal-operand, inexact.
   * Ordinary denormal math sets those flags, so two reads of the register
   * around a single multiply differ even though nothing about the MODE moved.
   *
   * Anything asking "is the floating-point MODE the same" -- a test, or a
   * caller checking the host's mode survived -- must compare this, not the raw
   * register, or it is really asking "has any arithmetic happened", which is
   * always yes. AArch64 keeps its status flags in a separate register (FPSR),
   * so FPCR is already mode-only and this is the identity there.
   */
  static uintptr_t modeOf(uintptr_t state) {
#if defined(SONORE_DENORMALS_SSE)
    return state & ~(uintptr_t) 0x3Fu; // clear the 6 sticky exception flags
#else
    return state;
#endif
  }

  static uintptr_t readState() {
#if defined(SONORE_DENORMALS_SSE)
    return (uintptr_t) _mm_getcsr();
#elif defined(SONORE_DENORMALS_ARM64_MSVC)
    return (uintptr_t) _ReadStatusReg(ARM64_FPCR);
#elif defined(SONORE_DENORMALS_ARM64)
    uint64_t fpcr = 0;
    __asm__ __volatile__("mrs %0, fpcr" : "=r"(fpcr));
    return (uintptr_t) fpcr;
#else
    return 0;
#endif
  }

  static void writeState(uintptr_t state) {
#if defined(SONORE_DENORMALS_SSE)
    _mm_setcsr((unsigned int) state);
#elif defined(SONORE_DENORMALS_ARM64_MSVC)
    _WriteStatusReg(ARM64_FPCR, (__int64) state);
#elif defined(SONORE_DENORMALS_ARM64)
    const uint64_t fpcr = (uint64_t) state;
    __asm__ __volatile__("msr fpcr, %0" : : "r"(fpcr));
#else
    (void) state;
#endif
  }

  /** The same register with flushing turned on. Separated out so the bit
   *  arithmetic is one expression a reader can check against the manual. */
  static uintptr_t flushingState(uintptr_t state) {
#if defined(SONORE_DENORMALS_SSE)
    // MXCSR bit 15 is flush-to-zero (a denormal RESULT becomes zero) and bit 6
    // is denormals-are-zero (a denormal INPUT reads as zero). Both, because
    // either alone still leaves one side of an operation slow.
    return state | 0x8000u | 0x0040u;
#elif defined(SONORE_DENORMALS_ARM64)
    // FPCR bit 24 is FZ. AArch64 has no separate DAZ: flush-to-zero mode
    // already treats denormal inputs as zero.
    return state | (1ull << 24);
#else
    // A platform whose floating-point mode this build does not know how to
    // set. The per-sample flushDenormal() in dsp.h still applies, and saying
    // nothing would be claiming a protection that is not here.
    return state;
#endif
  }

  /** Whether this build can actually do anything. False is not a failure --
   *  it is the honest answer on a target whose control register is not one of
   *  the two this knows. */
  static bool supported() {
#if defined(SONORE_DENORMALS_SSE) || defined(SONORE_DENORMALS_ARM64)
    return true;
#else
    return false;
#endif
  }

private:
  uintptr_t saved_;
};

} // namespace sonore
