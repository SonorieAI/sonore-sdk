// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: the lossy decoders, vendored.
//
// WAV, AIFF and FLAC we decode ourselves: their formats are simple enough that
// our own code is verifiable SAMPLE-EXACT against reference material, and
// owning them is worth something. MP3 and Vorbis are the opposite case:
// thousands of lines of Huffman tables, IMDCT and codebooks with zero
// differentiation in them, so they are vendored from the two public-domain
// implementations everyone uses:
//
//   minimp3    (lieff): CC0, public domain
//   stb_vorbis (Sean Barrett): public domain / MIT, dual
//
// Both are permissive in the way this SDK requires: nothing propagates to a
// customer's product, which is the whole reason this SDK exists at all.
//
// The implementations are pulled into an ANONYMOUS NAMESPACE so every
// translation unit gets its own internal-linkage copy. A header-only SDK that
// emitted external symbols from a vendored .c would collide the moment a
// plugin was built from two source files.

#pragma once

// System headers FIRST, at global scope: their include guards then make the
// copies inside the vendored sources no-ops, so nothing from libc ends up
// declared inside our namespace.
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The INTRINSICS headers belong on that list too, and were not on it.
//
// minimp3 includes <immintrin.h> (or <arm_neon.h>) itself, from inside the
// anonymous namespace below. Its include guard then fires, so a later
// #include <emmintrin.h> at global scope -- which is what simd.h does -- is a
// no-op, and __m128 exists only as sonore::codec::{anonymous}::__m128. Every
// SIMD type in this SDK stops resolving, with errors that point at simd.h and
// have nothing to do with it.
//
// It never showed on MSVC, where the CRT drags <intrin.h> in early enough by
// itself. On GCC and Clang any plugin that decodes an audio file AND uses
// SIMD simply failed to compile -- which is most of them, and which nothing
// caught because the two examples that do both were only ever built on
// Windows.
//
// The conditions are minimp3's own, copied deliberately: guessing a broader
// one would pull an intrinsics header into a build that cannot compile it.
#if (defined(_MSC_VER) && (defined(_M_IX86) || defined(_M_X64))) ||     ((defined(__i386__) || defined(__x86_64__)) && defined(__SSE2__))
#if defined(_MSC_VER)
#include <intrin.h>
#endif
#include <immintrin.h>
#elif defined(__ARM_NEON) || defined(__aarch64__) || defined(_M_ARM64)
#include <arm_neon.h>
#endif

namespace sonore {
namespace codec {
namespace {

// minimp3: frame decoder only. We drive the frame loop ourselves so file
// handling matches wav.h rather than pulling in its own IO layer.
#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_STDIO
// Decode straight to float: everything downstream in the SDK is float, and a
// round trip through int16 would throw away resolution the decoder had.
#define MINIMP3_FLOAT_OUTPUT
#define MINIMP3_IMPLEMENTATION
#include "../../third_party/minimp3.h"

// stb_vorbis: memory decoding only, no stdio path, no pushdata API.
#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#define STB_VORBIS_NO_INTEGER_CONVERSION
#include "../../third_party/stb_vorbis.c"

} // namespace
} // namespace codec
} // namespace sonore
