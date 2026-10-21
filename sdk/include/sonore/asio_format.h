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

namespace detail {
/** What a float goes through before any format sees it: clamped to [-1, 1],
 *  and a NaN made silence. A NaN passes both comparisons of the clamp
 *  untouched, and an integer format then casts it to the most negative code
 *  -- a full-scale click into the speakers from one bad sample -- so it is
 *  caught after them. That order also lets the clamp compile to a max and a
 *  min, where testing for NaN first left it as branches; what comes out is
 *  the same for every input, -0.0 included. */
inline float deviceSample(float v) {
  v = v < -1.0f ? -1.0f : v;
  v = v > 1.0f ? 1.0f : v;
  return v == v ? v : 0.0f;
}
} // namespace detail

/**
 * A run of float samples into a driver buffer: `frames` of them, read
 * `stride` floats apart from `source` (2 for one side of interleaved stereo),
 * written to frames 0..frames-1 of `destination` in the driver's format.
 *
 * Clamped before conversion, not after. A sample above full scale wraps to
 * the opposite sign when it is truncated into an integer, and a wrap is a
 * full-scale click rather than the clipping a listener would forgive.
 *
 * The scale is (max + 1) and the written value uses (max), so +1.0 lands on
 * the largest representable positive rather than one past it. The asymmetry
 * is real and is in every fixed-point audio format: there is one more
 * negative code than positive.
 *
 * A RUN, because that is what the buffer switch converts: two channels each
 * way, a block each. It went through writeSample one sample at a time, which
 * picked the format again for every sample and kept nothing in a register
 * from one to the next: 1.6 to 1.9 ns a sample, seven times what the
 * standalone spends on a whole 256-frame block around a DSP that does
 * nothing. Here the format is picked once and each loop is that format's
 * arithmetic alone. writeSample is a run of one, so this is still the only
 * copy of each conversion.
 */
inline void writeSamples(void* destination, long type, const float* source, size_t stride,
                         size_t frames) {
  switch (type) {
    case kAsioFloat32Lsb: {
      float* out = static_cast<float*>(destination);
      for (size_t i = 0; i < frames; ++i) out[i] = detail::deviceSample(source[i * stride]);
      return;
    }
    case kAsioInt16Lsb: {
      int16_t* out = static_cast<int16_t*>(destination);
      for (size_t i = 0; i < frames; ++i)
        out[i] = (int16_t) (detail::deviceSample(source[i * stride]) * 32767.0f);
      return;
    }
    case kAsioInt24Lsb: {
      uint8_t* p = static_cast<uint8_t*>(destination);
      for (size_t i = 0; i < frames; ++i, p += 3) {
        const int32_t s = (int32_t) (detail::deviceSample(source[i * stride]) * 8388607.0f);
        p[0] = (uint8_t) (s & 0xff);
        p[1] = (uint8_t) ((s >> 8) & 0xff);
        p[2] = (uint8_t) ((s >> 16) & 0xff);
      }
      return;
    }
    case kAsioInt32Lsb: {
      int32_t* out = static_cast<int32_t*>(destination);
      for (size_t i = 0; i < frames; ++i)
        out[i] = (int32_t) ((double) detail::deviceSample(source[i * stride]) * 2147483647.0);
      return;
    }
    case kAsioInt32Lsb16:
    case kAsioInt32Lsb18:
    case kAsioInt32Lsb20:
    case kAsioInt32Lsb24: {
      const float top = formatScale(type) - 1.0f;
      int32_t* out = static_cast<int32_t*>(destination);
      for (size_t i = 0; i < frames; ++i)
        out[i] = (int32_t) (detail::deviceSample(source[i * stride]) * top);
      return;
    }
    default:
      return;
  }
}

/** The exact inverse, a run at a time: frames 0..frames-1 of a driver buffer
 *  out as floats, written `stride` apart into `destination`. A format this
 *  build does not convert reads as silence, not garbage. */
inline void readSamples(const void* source, long type, float* destination, size_t stride,
                        size_t frames) {
  switch (type) {
    case kAsioFloat32Lsb: {
      const float* in = static_cast<const float*>(source);
      for (size_t i = 0; i < frames; ++i) destination[i * stride] = in[i];
      return;
    }
    case kAsioInt16Lsb: {
      const int16_t* in = static_cast<const int16_t*>(source);
      for (size_t i = 0; i < frames; ++i) destination[i * stride] = (float) in[i] / 32768.0f;
      return;
    }
    case kAsioInt24Lsb: {
      const uint8_t* p = static_cast<const uint8_t*>(source);
      for (size_t i = 0; i < frames; ++i, p += 3) {
        int32_t s =
            (int32_t) ((uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16));
        // Sign-extend from 24 bits. Without this every negative sample reads
        // as a large positive one, which is not quiet distortion -- it is a
        // full-scale buzz on half the waveform.
        if (s & 0x800000) s |= (int32_t) 0xFF000000;
        destination[i * stride] = (float) s / 8388608.0f;
      }
      return;
    }
    case kAsioInt32Lsb: {
      const int32_t* in = static_cast<const int32_t*>(source);
      for (size_t i = 0; i < frames; ++i) destination[i * stride] = (float) in[i] / 2147483648.0f;
      return;
    }
    case kAsioInt32Lsb16:
    case kAsioInt32Lsb18:
    case kAsioInt32Lsb20:
    case kAsioInt32Lsb24: {
      const float scale = formatScale(type);
      const int32_t* in = static_cast<const int32_t*>(source);
      for (size_t i = 0; i < frames; ++i) destination[i * stride] = (float) in[i] / scale;
      return;
    }
    default:
      for (size_t i = 0; i < frames; ++i) destination[i * stride] = 0.0f;
      return;
  }
}

/** One float sample into a driver buffer, at frame `i`: a run of one (see
 *  writeSamples, for the clamp and the scale). */
inline void writeSample(void* destination, size_t i, long type, float v) {
  writeSamples(static_cast<uint8_t*>(destination) + i * formatBytes(type), type, &v, 1, 1);
}

/** The exact inverse: one sample of a driver buffer back out as a float. */
inline float readSample(const void* source, size_t i, long type) {
  float v = 0.0f;
  readSamples(static_cast<const uint8_t*>(source) + i * formatBytes(type), type, &v, 1, 1);
  return v;
}

// ── The host's half of asioMessage ───────────────────────────────────────────
//
// Here rather than in audio_asio.h for the reason the conversions are: the
// answers are a table, the table is what was wrong, and outside the Windows
// guard it can be asserted on every leg of the gate.
//
// The selector numbers are the published interface's, in its order. The first
// version of this answered by a private numbering -- 4 labelled "engine
// version", which is the BUFFER-SIZE-CHANGE selector -- and replied "yes" to
// kAsioSelectorSupported whatever selector it was asked about. So it claimed
// to handle resets it ignored, and "accepted" buffer-size changes it never
// applied: the driver shrinks its buffers and we go on filling the old size.
enum : long {
  kAsioSelectorSupported = 1,
  kAsioEngineVersion = 2,
  kAsioResetRequest = 3,
  kAsioBufferSizeChange = 4,
  kAsioResyncRequest = 5,
  kAsioLatenciesChanged = 6,
  kAsioSupportsTimeInfo = 7,
  kAsioSupportsTimeCode = 8,
};

/**
 * What a host answers to the driver's asioMessage(selector, value).
 *
 * `wantsReset` is set when the driver asked for the stream to be rebuilt --
 * the caller tears down and reopens on its own thread, never inside this
 * callback, which may arrive on the driver's.
 *
 *   - ResetRequest: supported. The driver will not apply its change until we
 *     reopen it, so saying yes and doing nothing is what hangs a control panel.
 *   - BufferSizeChange: NOT supported, on purpose. Answering 0 is what the
 *     interface specifies for a host that cannot resize live, and a driver
 *     that gets it falls back to ResetRequest -- the path we do support.
 *   - ResyncRequest: acknowledged. It invalidates the driver's timestamps,
 *     and nothing here reads them.
 *   - LatenciesChanged: acknowledged; latencies are asked for when needed,
 *     never cached.
 *   - SupportsTimeInfo / TimeCode: no. We register bufferSwitch, not
 *     bufferSwitchTimeInfo, and saying yes would make the driver call the null
 *     one.
 */
inline long hostMessageReply(long selector, long value, bool* wantsReset) {
  switch (selector) {
    case kAsioSelectorSupported:
      return (value == kAsioEngineVersion || value == kAsioResetRequest ||
              value == kAsioResyncRequest || value == kAsioLatenciesChanged)
                 ? 1
                 : 0;
    case kAsioEngineVersion:
      return 2;
    case kAsioResetRequest:
      if (wantsReset) *wantsReset = true;
      return 1;
    case kAsioResyncRequest:
    case kAsioLatenciesChanged:
      return 1;
    default:
      return 0;
  }
}

} // namespace asio
} // namespace sonore
