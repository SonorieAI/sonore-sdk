// SPDX-License-Identifier: Apache-2.0
//
// ASIO sample formats, and the conversion to and from them.
//
// ── Why this is its own header ─────────────────────────────────────────────
//
// audio_asio.h is entirely inside `#if defined(_WIN32)`, because a driver
// vtable reached through CoCreateInstance cannot mean anything anywhere else.
// The FORMAT CONVERSION is not like that. It is integer arithmetic, it is the
// part most likely to be subtly wrong, and it is the part that produces a
// full-scale buzz rather than an error message when it is. Leaving it inside
// the guard would mean the only machine able to test it is the only machine
// with the hardware -- which is the same as saying it is never tested.
//
// So it lives here, compiled everywhere, exercised by the round-trip test on
// both legs of the gate. audio_asio.h includes this and calls into it; there
// is one copy of each conversion, and the copy is the one under test.
//
// No Steinberg code. The type CODES are numbers stated in the published ASIO
// documentation -- the same category of fact as an HTTP status code -- and
// everything else here is written from the format names.
#pragma once

#include <cstdint>
#include <cstddef>

namespace sonore {
namespace asio {

/**
 * The sample formats a driver may hand back.
 *
 * Only the little-endian ones that exist on real hardware are named. A big
 * endian ASIO driver is a PowerPC-era Mac artefact, and float64 is in the
 * specification but not on any interface you can buy; both are refused BY
 * NAME in formatIsSupported rather than converted wrongly, because a wrong
 * conversion is audible and an honest refusal is not.
 */
enum : long {
  kAsioInt16Lsb = 16,
  kAsioInt24Lsb = 17,
  kAsioInt32Lsb = 18,
  kAsioFloat32Lsb = 19,
  kAsioFloat64Lsb = 20,
  kAsioInt32Lsb16 = 21,
  kAsioInt32Lsb18 = 22,
  kAsioInt32Lsb20 = 23,
  kAsioInt32Lsb24 = 24,
};

/** Whether this build converts that format at all. */
inline bool formatIsSupported(long type) {
  return type == kAsioInt16Lsb || type == kAsioInt24Lsb || type == kAsioInt32Lsb ||
         type == kAsioFloat32Lsb || type == kAsioInt32Lsb16 || type == kAsioInt32Lsb18 ||
         type == kAsioInt32Lsb20 || type == kAsioInt32Lsb24;
}

/** How many bytes one sample of that format occupies in a driver buffer. */
inline size_t formatBytes(long type) {
  switch (type) {
    case kAsioInt16Lsb: return 2;
    case kAsioInt24Lsb: return 3;
    case kAsioFloat64Lsb: return 8;
    default: return 4;
  }
}

/**
 * The full-scale magnitude of the integer formats.
 *
 * The Lsb16/18/20/24 variants are the awkward ones: a 32-bit container
 * holding a smaller word, RIGHT-aligned. An interface that reports
 * kAsioInt32Lsb24 wants 24 bits of range in a 32-bit slot, and writing
 * 32 bits into it is 256x too loud -- which is not distortion, it is a
 * number that wraps every few samples.
 */
inline float formatScale(long type) {
  switch (type) {
    case kAsioInt16Lsb: return 32768.0f;
    case kAsioInt24Lsb: return 8388608.0f;
    case kAsioInt32Lsb: return 2147483648.0f;
    case kAsioInt32Lsb16: return 32768.0f;
    case kAsioInt32Lsb18: return 131072.0f;
    case kAsioInt32Lsb20: return 524288.0f;
    case kAsioInt32Lsb24: return 8388608.0f;
    default: return 1.0f;
  }
}

/**
 * One float sample into a driver buffer, at frame `i`.
 *
 * Clamped before conversion, not after. A sample above full scale wraps to
 * the opposite sign when it is truncated into an integer, and a wrap is a
 * full-scale click rather than the clipping a listener would forgive.
 *
 * The scale is (max + 1) and the written value uses (max), so +1.0 lands on
 * the largest representable positive rather than one past it. The asymmetry
 * is real and is in every fixed-point audio format: there is one more
 * negative code than positive.
 */
inline void writeSample(void* destination, size_t i, long type, float v) {
  v = v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v);
  switch (type) {
    case kAsioFloat32Lsb:
      ((float*) destination)[i] = v;
      return;
    case kAsioInt16Lsb:
      ((int16_t*) destination)[i] = (int16_t) (v * 32767.0f);
      return;
    case kAsioInt24Lsb: {
      const int32_t s = (int32_t) (v * 8388607.0f);
      uint8_t* p = (uint8_t*) destination + i * 3;
      p[0] = (uint8_t) (s & 0xff);
      p[1] = (uint8_t) ((s >> 8) & 0xff);
      p[2] = (uint8_t) ((s >> 16) & 0xff);
      return;
    }
    case kAsioInt32Lsb:
      ((int32_t*) destination)[i] = (int32_t) ((double) v * 2147483647.0);
      return;
    case kAsioInt32Lsb16:
    case kAsioInt32Lsb18:
    case kAsioInt32Lsb20:
    case kAsioInt32Lsb24:
      ((int32_t*) destination)[i] = (int32_t) (v * (formatScale(type) - 1.0f));
      return;
    default:
      return;
  }
}

/** The exact inverse: one sample of a driver buffer back out as a float. */
inline float readSample(const void* source, size_t i, long type) {
  switch (type) {
    case kAsioFloat32Lsb:
      return ((const float*) source)[i];
    case kAsioInt16Lsb:
      return (float) ((const int16_t*) source)[i] / 32768.0f;
    case kAsioInt24Lsb: {
      const uint8_t* p = (const uint8_t*) source + i * 3;
      int32_t s = (int32_t) ((uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16));
      // Sign-extend from 24 bits. Without this every negative sample reads as
      // a large positive one, which is not quiet distortion -- it is a
      // full-scale buzz on half the waveform.
      if (s & 0x800000) s |= (int32_t) 0xFF000000;
      return (float) s / 8388608.0f;
    }
    case kAsioInt32Lsb:
      return (float) ((const int32_t*) source)[i] / 2147483648.0f;
    case kAsioInt32Lsb16:
    case kAsioInt32Lsb18:
    case kAsioInt32Lsb20:
    case kAsioInt32Lsb24:
      return (float) ((const int32_t*) source)[i] / formatScale(type);
    default:
      return 0.0f;
  }
}

} // namespace asio
} // namespace sonore
