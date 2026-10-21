// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: the lossy decoders, vendored.
//
// WAV, AIFF and FLAC we decode ourselves: their formats are simple enough that
// our own code is verifiable SAMPLE-EXACT against reference material, and
// owning them is worth something. MP3 and Vorbis are the opposite case
// (thousands of lines of Huffman tables, IMDCT and codebooks with zero
// differentiation in them), so they are vendored from the two public-domain
// implementations everyone uses:
//
//   minimp3    (lieff):        CC0, public domain
//   stb_vorbis (Sean Barrett): public domain / MIT, dual
//
// Both are permissive in the way this SDK requires: nothing propagates to a
// customer's product, which is the whole reason this SDK exists at all.
//
// The implementations are pulled into an ANONYMOUS NAMESPACE so every
// translation unit gets its own internal-linkage copy. A header-only SDK that
// emitted external symbols from a vendored .c would collide the moment a
// plugin was built from two source files.
//
// Which needs their extern "C" blocks switched off (SONORE_CODEC_CPP_LINKAGE,
// a local change in both files). A function with C language linkage is one
// function by its bare name whatever namespace declares it, and MSVC gave
// these external linkage inside the anonymous namespace all the same: two
// source files including audiofile.h failed to link, 18 times over
// (mp3dec_init already defined...), and so did a standalone with a second
// file, and a plugin beside its own copy of either codec. With C++ linkage
// the anonymous namespace does what this paragraph says.
//
// And what the two files define for themselves -- TRUE, FALSE, a float M_PI,
// their configuration and include guards -- does not outlive them: a user's
// own M_PI or minimp3.h after this header is theirs, not ours.

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

// The two codecs are someone else's code compiled inside the USER's
// translation unit, under the user's warning flags. At /W4 they produced 105
// warnings (conversions, shadowed locals) in every file that reached
// audiofile.h -- the standalone always does -- and a project built with /WX
// or -Werror could not compile its standalone at all. Their warnings are not
// the user's to fix, so they are silenced around the two includes and
// nowhere else.
#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(push, 0)
// Level 0 does not reach the warnings the OPTIMISER raises -- "possibly
// uninitialised" is decided after parsing, and an optimised build reported it
// from stb_vorbis with the level set to nothing. Named, so they are covered too.
#pragma warning(disable : 4701 4703)
#elif defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wfloat-conversion"
#pragma GCC diagnostic ignored "-Wdouble-promotion"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#pragma GCC diagnostic ignored "-Wtype-limits"
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wcast-qual"
#endif

// Every macro the two codecs define, on any branch they have -- SSE, NEON,
// the generic path of a 32-bit or wasm build, MinGW -- set aside before they
// are included and put back after, exactly as the user had them.
//
// They outlived this header, and the first fix #undef'd them, which was
// worse in its own way: a user's HAVE_SSE from config.h or CHECK from a test
// framework, defined BEFORE this header, was deleted along with the codec's.
// Set aside, a user's macro survives; undefined first, the codecs compile on
// a clean slate, and a user's own minimp3 or stb_vorbis -- before this header
// or after it -- keeps its include guards to itself. The list is every
// #define in the two files, generated from them, less the C library's NULL,
// alloca, malloc, free and realloc, which they only stand in for. It counts
// 129 names; a codec update that adds one must add it here.
#pragma push_macro("ADDEND")
#pragma push_macro("BITS_DEQUANTIZER_OUT")
#pragma push_macro("BSPOS")
#pragma push_macro("C")
#pragma push_macro("CHECK")
#pragma push_macro("CHECK_BITS")
#pragma push_macro("CODEBOOK_ELEMENT")
#pragma push_macro("CODEBOOK_ELEMENT_BASE")
#pragma push_macro("CODEBOOK_ELEMENT_FAST")
#pragma push_macro("CRC32_POLY")
#pragma push_macro("DECODE")
#pragma push_macro("DECODE_RAW")
#pragma push_macro("DECODE_VQ")
#pragma push_macro("DEQ_COUNT1")
#pragma push_macro("DIVTAB_DENOM")
#pragma push_macro("DIVTAB_NUMER")
#pragma push_macro("DQ")
#pragma push_macro("EOP")
#pragma push_macro("FALSE")
#pragma push_macro("FASTDEF")
#pragma push_macro("FAST_HUFFMAN_TABLE_MASK")
#pragma push_macro("FAST_HUFFMAN_TABLE_SIZE")
#pragma push_macro("FAST_SCALED_FLOAT_TO_INT")
#pragma push_macro("FLUSH_BITS")
#pragma push_macro("HAVE_ARMV6")
#pragma push_macro("HAVE_SIMD")
#pragma push_macro("HAVE_SSE")
#pragma push_macro("HDR_GET_BITRATE")
#pragma push_macro("HDR_GET_LAYER")
#pragma push_macro("HDR_GET_MY_SAMPLE_RATE")
#pragma push_macro("HDR_GET_SAMPLE_RATE")
#pragma push_macro("HDR_GET_STEREO_MODE")
#pragma push_macro("HDR_GET_STEREO_MODE_EXT")
#pragma push_macro("HDR_IS_CRC")
#pragma push_macro("HDR_IS_FRAME_576")
#pragma push_macro("HDR_IS_FREE_FORMAT")
#pragma push_macro("HDR_IS_LAYER_1")
#pragma push_macro("HDR_IS_MONO")
#pragma push_macro("HDR_IS_MS_STEREO")
#pragma push_macro("HDR_SIZE")
#pragma push_macro("HDR_TEST_I_STEREO")
#pragma push_macro("HDR_TEST_MPEG1")
#pragma push_macro("HDR_TEST_MS_STEREO")
#pragma push_macro("HDR_TEST_NOT_MPEG25")
#pragma push_macro("HDR_TEST_PADDING")
#pragma push_macro("INVALID_BITS")
#pragma push_macro("IS_PUSH_MODE")
#pragma push_macro("L")
#pragma push_macro("LIBVORBIS_MDCT")
#pragma push_macro("LINE_OP")
#pragma push_macro("LOAD")
#pragma push_macro("MAGIC")
#pragma push_macro("MAX_BITRESERVOIR_BYTES")
#pragma push_macro("MAX_BLOCKSIZE")
#pragma push_macro("MAX_BLOCKSIZE_LOG")
#pragma push_macro("MAX_FRAME_SYNC_MATCHES")
#pragma push_macro("MAX_FREE_FORMAT_FRAME_SIZE")
#pragma push_macro("MAX_L3_FRAME_PAYLOAD_BYTES")
#pragma push_macro("MAX_SCF")
#pragma push_macro("MAX_SCFI")
#pragma push_macro("MINIMP3_FLOAT_OUTPUT")
#pragma push_macro("MINIMP3_H")
#pragma push_macro("MINIMP3_IMPLEMENTATION")
#pragma push_macro("MINIMP3_MAX")
#pragma push_macro("MINIMP3_MAX_SAMPLES_PER_FRAME")
#pragma push_macro("MINIMP3_MIN")
#pragma push_macro("MINIMP3_NO_STDIO")
#pragma push_macro("MINIMP3_ONLY_MP3")
#pragma push_macro("MINIMP3_ONLY_SIMD")
#pragma push_macro("MODE_JOINT_STEREO")
#pragma push_macro("MODE_MONO")
#pragma push_macro("M_PI")
#pragma push_macro("NO_CODE")
#pragma push_macro("PAGEFLAG_continued_packet")
#pragma push_macro("PAGEFLAG_first_page")
#pragma push_macro("PAGEFLAG_last_page")
#pragma push_macro("PEEK_BITS")
#pragma push_macro("PLAYBACK_LEFT")
#pragma push_macro("PLAYBACK_MONO")
#pragma push_macro("PLAYBACK_RIGHT")
#pragma push_macro("R")
#pragma push_macro("RELOAD_SCALEFACTOR")
#pragma push_macro("S0")
#pragma push_macro("S1")
#pragma push_macro("S2")
#pragma push_macro("SAMPLE_unknown")
#pragma push_macro("SHORT_BLOCK_TYPE")
#pragma push_macro("SONORE_CODEC_CPP_LINKAGE")
#pragma push_macro("STBV_CDECL")
#pragma push_macro("STBV_NOTUSED")
#pragma push_macro("STB_BUFFER_SIZE")
#pragma push_macro("STB_VORBIS_ENDIAN")
#pragma push_macro("STB_VORBIS_FAST_HUFFMAN_LENGTH")
#pragma push_macro("STB_VORBIS_FAST_HUFFMAN_SHORT")
#pragma push_macro("STB_VORBIS_INCLUDE_STB_VORBIS_H")
#pragma push_macro("STB_VORBIS_MAX_CHANNELS")
#pragma push_macro("STB_VORBIS_NO_INTEGER_CONVERSION")
#pragma push_macro("STB_VORBIS_NO_PUSHDATA_API")
#pragma push_macro("STB_VORBIS_NO_STDIO")
#pragma push_macro("STB_VORBIS_PUSHDATA_CRC_COUNT")
#pragma push_macro("STOP_BLOCK_TYPE")
#pragma push_macro("TRUE")
#pragma push_macro("USE_MEMORY")
#pragma push_macro("V0")
#pragma push_macro("V1")
#pragma push_macro("V2")
#pragma push_macro("VADD")
#pragma push_macro("VLD")
#pragma push_macro("VLOAD")
#pragma push_macro("VMAC")
#pragma push_macro("VMSB")
#pragma push_macro("VMUL")
#pragma push_macro("VMUL_S")
#pragma push_macro("VREV")
#pragma push_macro("VSAVE2")
#pragma push_macro("VSAVE4")
#pragma push_macro("VSET")
#pragma push_macro("VSTORE")
#pragma push_macro("VSUB")
#pragma push_macro("_MINIMP3_IMPLEMENTATION_GUARD")
#pragma push_macro("__forceinline")
#pragma push_macro("array_size_required")
#pragma push_macro("check_endianness")
#pragma push_macro("minimp3_cpuid")
#pragma push_macro("temp_alloc")
#pragma push_macro("temp_alloc_restore")
#pragma push_macro("temp_alloc_save")
#pragma push_macro("temp_block_array")
#pragma push_macro("temp_free")
#undef ADDEND
#undef BITS_DEQUANTIZER_OUT
#undef BSPOS
#undef C
#undef CHECK
#undef CHECK_BITS
#undef CODEBOOK_ELEMENT
#undef CODEBOOK_ELEMENT_BASE
#undef CODEBOOK_ELEMENT_FAST
#undef CRC32_POLY
#undef DECODE
#undef DECODE_RAW
#undef DECODE_VQ
#undef DEQ_COUNT1
#undef DIVTAB_DENOM
#undef DIVTAB_NUMER
#undef DQ
#undef EOP
#undef FALSE
#undef FASTDEF
#undef FAST_HUFFMAN_TABLE_MASK
#undef FAST_HUFFMAN_TABLE_SIZE
#undef FAST_SCALED_FLOAT_TO_INT
#undef FLUSH_BITS
#undef HAVE_ARMV6
#undef HAVE_SIMD
#undef HAVE_SSE
#undef HDR_GET_BITRATE
#undef HDR_GET_LAYER
#undef HDR_GET_MY_SAMPLE_RATE
#undef HDR_GET_SAMPLE_RATE
#undef HDR_GET_STEREO_MODE
#undef HDR_GET_STEREO_MODE_EXT
#undef HDR_IS_CRC
#undef HDR_IS_FRAME_576
#undef HDR_IS_FREE_FORMAT
#undef HDR_IS_LAYER_1
#undef HDR_IS_MONO
#undef HDR_IS_MS_STEREO
#undef HDR_SIZE
#undef HDR_TEST_I_STEREO
#undef HDR_TEST_MPEG1
#undef HDR_TEST_MS_STEREO
#undef HDR_TEST_NOT_MPEG25
#undef HDR_TEST_PADDING
#undef INVALID_BITS
#undef IS_PUSH_MODE
#undef L
#undef LIBVORBIS_MDCT
#undef LINE_OP
#undef LOAD
#undef MAGIC
#undef MAX_BITRESERVOIR_BYTES
#undef MAX_BLOCKSIZE
#undef MAX_BLOCKSIZE_LOG
#undef MAX_FRAME_SYNC_MATCHES
#undef MAX_FREE_FORMAT_FRAME_SIZE
#undef MAX_L3_FRAME_PAYLOAD_BYTES
#undef MAX_SCF
#undef MAX_SCFI
#undef MINIMP3_FLOAT_OUTPUT
#undef MINIMP3_H
#undef MINIMP3_IMPLEMENTATION
#undef MINIMP3_MAX
#undef MINIMP3_MAX_SAMPLES_PER_FRAME
#undef MINIMP3_MIN
#undef MINIMP3_NO_STDIO
#undef MINIMP3_ONLY_MP3
#undef MINIMP3_ONLY_SIMD
#undef MODE_JOINT_STEREO
#undef MODE_MONO
#undef M_PI
#undef NO_CODE
#undef PAGEFLAG_continued_packet
#undef PAGEFLAG_first_page
#undef PAGEFLAG_last_page
#undef PEEK_BITS
#undef PLAYBACK_LEFT
#undef PLAYBACK_MONO
#undef PLAYBACK_RIGHT
#undef R
#undef RELOAD_SCALEFACTOR
#undef S0
#undef S1
#undef S2
#undef SAMPLE_unknown
#undef SHORT_BLOCK_TYPE
#undef SONORE_CODEC_CPP_LINKAGE
#undef STBV_CDECL
#undef STBV_NOTUSED
#undef STB_BUFFER_SIZE
#undef STB_VORBIS_ENDIAN
#undef STB_VORBIS_FAST_HUFFMAN_LENGTH
#undef STB_VORBIS_FAST_HUFFMAN_SHORT
#undef STB_VORBIS_INCLUDE_STB_VORBIS_H
#undef STB_VORBIS_MAX_CHANNELS
#undef STB_VORBIS_NO_INTEGER_CONVERSION
#undef STB_VORBIS_NO_PUSHDATA_API
#undef STB_VORBIS_NO_STDIO
#undef STB_VORBIS_PUSHDATA_CRC_COUNT
#undef STOP_BLOCK_TYPE
#undef TRUE
#undef USE_MEMORY
#undef V0
#undef V1
#undef V2
#undef VADD
#undef VLD
#undef VLOAD
#undef VMAC
#undef VMSB
#undef VMUL
#undef VMUL_S
#undef VREV
#undef VSAVE2
#undef VSAVE4
#undef VSET
#undef VSTORE
#undef VSUB
#undef _MINIMP3_IMPLEMENTATION_GUARD
#undef __forceinline
#undef array_size_required
#undef check_endianness
#undef minimp3_cpuid
#undef temp_alloc
#undef temp_alloc_restore
#undef temp_alloc_save
#undef temp_block_array
#undef temp_free
// And the configuration stb_vorbis READS but never defines: a user's choice
// for their own copy, which must not reach ours. HEADER_ONLY and
// NO_PULLDATA_API take away the very functions this SDK calls (a user's
// header-only include before this one left ours unimplemented and failed
// to link); NO_CRT swaps in allocators of its own.
#pragma push_macro("STB_VORBIS_HEADER_ONLY")
#undef STB_VORBIS_HEADER_ONLY
#pragma push_macro("STB_VORBIS_NO_CRT")
#undef STB_VORBIS_NO_CRT
#pragma push_macro("STB_VORBIS_NO_PULLDATA_API")
#undef STB_VORBIS_NO_PULLDATA_API
#define SONORE_CODEC_CPP_LINKAGE 1

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
// TWO places, because the two ways this SDK arrives disagree about where the
// codecs are. A checkout has them in sdk/third_party, two levels up. An
// INSTALLED prefix has no third_party/ at its root to climb to -- the install
// rules put them beside these headers, in include/sonore/third_party. Only
// the first was ever written, so every find_package() consumer that reached
// this file (the standalone does, through audiofile.h) failed to compile.
#if __has_include("../../third_party/minimp3.h")
#include "../../third_party/minimp3.h"
#else
#include "third_party/minimp3.h"
#endif

// stb_vorbis: memory decoding only, no stdio path, no pushdata API.
#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#define STB_VORBIS_NO_INTEGER_CONVERSION
// Our own stb_vorbis type, declared before the file names it. Its first
// mention is `typedef struct stb_vorbis stb_vorbis;`, and a user who had
// included stb_vorbis BEFORE this header had a global struct of that name
// for the lookup to find: the typedef named theirs, the definition
// further down made ours, and the two collided (C2371).
struct stb_vorbis;
#if __has_include("../../third_party/stb_vorbis.c")
#include "../../third_party/stb_vorbis.c"
#else
#include "third_party/stb_vorbis.c"
#endif

/** The one codec macro the SDK uses after this point, kept as a constant so
 *  the macro itself can go with the rest. */
constexpr int kMp3MaxSamplesPerFrame = MINIMP3_MAX_SAMPLES_PER_FRAME;

} // namespace
} // namespace codec
} // namespace sonore

// And put back: the codecs' own gone, the user's as they were.
#undef STB_VORBIS_HEADER_ONLY
#pragma pop_macro("STB_VORBIS_HEADER_ONLY")
#undef STB_VORBIS_NO_CRT
#pragma pop_macro("STB_VORBIS_NO_CRT")
#undef STB_VORBIS_NO_PULLDATA_API
#pragma pop_macro("STB_VORBIS_NO_PULLDATA_API")
#undef ADDEND
#undef BITS_DEQUANTIZER_OUT
#undef BSPOS
#undef C
#undef CHECK
#undef CHECK_BITS
#undef CODEBOOK_ELEMENT
#undef CODEBOOK_ELEMENT_BASE
#undef CODEBOOK_ELEMENT_FAST
#undef CRC32_POLY
#undef DECODE
#undef DECODE_RAW
#undef DECODE_VQ
#undef DEQ_COUNT1
#undef DIVTAB_DENOM
#undef DIVTAB_NUMER
#undef DQ
#undef EOP
#undef FALSE
#undef FASTDEF
#undef FAST_HUFFMAN_TABLE_MASK
#undef FAST_HUFFMAN_TABLE_SIZE
#undef FAST_SCALED_FLOAT_TO_INT
#undef FLUSH_BITS
#undef HAVE_ARMV6
#undef HAVE_SIMD
#undef HAVE_SSE
#undef HDR_GET_BITRATE
#undef HDR_GET_LAYER
#undef HDR_GET_MY_SAMPLE_RATE
#undef HDR_GET_SAMPLE_RATE
#undef HDR_GET_STEREO_MODE
#undef HDR_GET_STEREO_MODE_EXT
#undef HDR_IS_CRC
#undef HDR_IS_FRAME_576
#undef HDR_IS_FREE_FORMAT
#undef HDR_IS_LAYER_1
#undef HDR_IS_MONO
#undef HDR_IS_MS_STEREO
#undef HDR_SIZE
#undef HDR_TEST_I_STEREO
#undef HDR_TEST_MPEG1
#undef HDR_TEST_MS_STEREO
#undef HDR_TEST_NOT_MPEG25
#undef HDR_TEST_PADDING
#undef INVALID_BITS
#undef IS_PUSH_MODE
#undef L
#undef LIBVORBIS_MDCT
#undef LINE_OP
#undef LOAD
#undef MAGIC
#undef MAX_BITRESERVOIR_BYTES
#undef MAX_BLOCKSIZE
#undef MAX_BLOCKSIZE_LOG
#undef MAX_FRAME_SYNC_MATCHES
#undef MAX_FREE_FORMAT_FRAME_SIZE
#undef MAX_L3_FRAME_PAYLOAD_BYTES
#undef MAX_SCF
#undef MAX_SCFI
#undef MINIMP3_FLOAT_OUTPUT
#undef MINIMP3_H
#undef MINIMP3_IMPLEMENTATION
#undef MINIMP3_MAX
#undef MINIMP3_MAX_SAMPLES_PER_FRAME
#undef MINIMP3_MIN
#undef MINIMP3_NO_STDIO
#undef MINIMP3_ONLY_MP3
#undef MINIMP3_ONLY_SIMD
#undef MODE_JOINT_STEREO
#undef MODE_MONO
#undef M_PI
#undef NO_CODE
#undef PAGEFLAG_continued_packet
#undef PAGEFLAG_first_page
#undef PAGEFLAG_last_page
#undef PEEK_BITS
#undef PLAYBACK_LEFT
#undef PLAYBACK_MONO
#undef PLAYBACK_RIGHT
#undef R
#undef RELOAD_SCALEFACTOR
#undef S0
#undef S1
#undef S2
#undef SAMPLE_unknown
#undef SHORT_BLOCK_TYPE
#undef SONORE_CODEC_CPP_LINKAGE
#undef STBV_CDECL
#undef STBV_NOTUSED
#undef STB_BUFFER_SIZE
#undef STB_VORBIS_ENDIAN
#undef STB_VORBIS_FAST_HUFFMAN_LENGTH
#undef STB_VORBIS_FAST_HUFFMAN_SHORT
#undef STB_VORBIS_INCLUDE_STB_VORBIS_H
#undef STB_VORBIS_MAX_CHANNELS
#undef STB_VORBIS_NO_INTEGER_CONVERSION
#undef STB_VORBIS_NO_PUSHDATA_API
#undef STB_VORBIS_NO_STDIO
#undef STB_VORBIS_PUSHDATA_CRC_COUNT
#undef STOP_BLOCK_TYPE
#undef TRUE
#undef USE_MEMORY
#undef V0
#undef V1
#undef V2
#undef VADD
#undef VLD
#undef VLOAD
#undef VMAC
#undef VMSB
#undef VMUL
#undef VMUL_S
#undef VREV
#undef VSAVE2
#undef VSAVE4
#undef VSET
#undef VSTORE
#undef VSUB
#undef _MINIMP3_IMPLEMENTATION_GUARD
#undef __forceinline
#undef array_size_required
#undef check_endianness
#undef minimp3_cpuid
#undef temp_alloc
#undef temp_alloc_restore
#undef temp_alloc_save
#undef temp_block_array
#undef temp_free
#pragma pop_macro("temp_free")
#pragma pop_macro("temp_block_array")
#pragma pop_macro("temp_alloc_save")
#pragma pop_macro("temp_alloc_restore")
#pragma pop_macro("temp_alloc")
#pragma pop_macro("minimp3_cpuid")
#pragma pop_macro("check_endianness")
#pragma pop_macro("array_size_required")
#pragma pop_macro("__forceinline")
#pragma pop_macro("_MINIMP3_IMPLEMENTATION_GUARD")
#pragma pop_macro("VSUB")
#pragma pop_macro("VSTORE")
#pragma pop_macro("VSET")
#pragma pop_macro("VSAVE4")
#pragma pop_macro("VSAVE2")
#pragma pop_macro("VREV")
#pragma pop_macro("VMUL_S")
#pragma pop_macro("VMUL")
#pragma pop_macro("VMSB")
#pragma pop_macro("VMAC")
#pragma pop_macro("VLOAD")
#pragma pop_macro("VLD")
#pragma pop_macro("VADD")
#pragma pop_macro("V2")
#pragma pop_macro("V1")
#pragma pop_macro("V0")
#pragma pop_macro("USE_MEMORY")
#pragma pop_macro("TRUE")
#pragma pop_macro("STOP_BLOCK_TYPE")
#pragma pop_macro("STB_VORBIS_PUSHDATA_CRC_COUNT")
#pragma pop_macro("STB_VORBIS_NO_STDIO")
#pragma pop_macro("STB_VORBIS_NO_PUSHDATA_API")
#pragma pop_macro("STB_VORBIS_NO_INTEGER_CONVERSION")
#pragma pop_macro("STB_VORBIS_MAX_CHANNELS")
#pragma pop_macro("STB_VORBIS_INCLUDE_STB_VORBIS_H")
#pragma pop_macro("STB_VORBIS_FAST_HUFFMAN_SHORT")
#pragma pop_macro("STB_VORBIS_FAST_HUFFMAN_LENGTH")
#pragma pop_macro("STB_VORBIS_ENDIAN")
#pragma pop_macro("STB_BUFFER_SIZE")
#pragma pop_macro("STBV_NOTUSED")
#pragma pop_macro("STBV_CDECL")
#pragma pop_macro("SONORE_CODEC_CPP_LINKAGE")
#pragma pop_macro("SHORT_BLOCK_TYPE")
#pragma pop_macro("SAMPLE_unknown")
#pragma pop_macro("S2")
#pragma pop_macro("S1")
#pragma pop_macro("S0")
#pragma pop_macro("RELOAD_SCALEFACTOR")
#pragma pop_macro("R")
#pragma pop_macro("PLAYBACK_RIGHT")
#pragma pop_macro("PLAYBACK_MONO")
#pragma pop_macro("PLAYBACK_LEFT")
#pragma pop_macro("PEEK_BITS")
#pragma pop_macro("PAGEFLAG_last_page")
#pragma pop_macro("PAGEFLAG_first_page")
#pragma pop_macro("PAGEFLAG_continued_packet")
#pragma pop_macro("NO_CODE")
#pragma pop_macro("M_PI")
#pragma pop_macro("MODE_MONO")
#pragma pop_macro("MODE_JOINT_STEREO")
#pragma pop_macro("MINIMP3_ONLY_SIMD")
#pragma pop_macro("MINIMP3_ONLY_MP3")
#pragma pop_macro("MINIMP3_NO_STDIO")
#pragma pop_macro("MINIMP3_MIN")
#pragma pop_macro("MINIMP3_MAX_SAMPLES_PER_FRAME")
#pragma pop_macro("MINIMP3_MAX")
#pragma pop_macro("MINIMP3_IMPLEMENTATION")
#pragma pop_macro("MINIMP3_H")
#pragma pop_macro("MINIMP3_FLOAT_OUTPUT")
#pragma pop_macro("MAX_SCFI")
#pragma pop_macro("MAX_SCF")
#pragma pop_macro("MAX_L3_FRAME_PAYLOAD_BYTES")
#pragma pop_macro("MAX_FREE_FORMAT_FRAME_SIZE")
#pragma pop_macro("MAX_FRAME_SYNC_MATCHES")
#pragma pop_macro("MAX_BLOCKSIZE_LOG")
#pragma pop_macro("MAX_BLOCKSIZE")
#pragma pop_macro("MAX_BITRESERVOIR_BYTES")
#pragma pop_macro("MAGIC")
#pragma pop_macro("LOAD")
#pragma pop_macro("LINE_OP")
#pragma pop_macro("LIBVORBIS_MDCT")
#pragma pop_macro("L")
#pragma pop_macro("IS_PUSH_MODE")
#pragma pop_macro("INVALID_BITS")
#pragma pop_macro("HDR_TEST_PADDING")
#pragma pop_macro("HDR_TEST_NOT_MPEG25")
#pragma pop_macro("HDR_TEST_MS_STEREO")
#pragma pop_macro("HDR_TEST_MPEG1")
#pragma pop_macro("HDR_TEST_I_STEREO")
#pragma pop_macro("HDR_SIZE")
#pragma pop_macro("HDR_IS_MS_STEREO")
#pragma pop_macro("HDR_IS_MONO")
#pragma pop_macro("HDR_IS_LAYER_1")
#pragma pop_macro("HDR_IS_FREE_FORMAT")
#pragma pop_macro("HDR_IS_FRAME_576")
#pragma pop_macro("HDR_IS_CRC")
#pragma pop_macro("HDR_GET_STEREO_MODE_EXT")
#pragma pop_macro("HDR_GET_STEREO_MODE")
#pragma pop_macro("HDR_GET_SAMPLE_RATE")
#pragma pop_macro("HDR_GET_MY_SAMPLE_RATE")
#pragma pop_macro("HDR_GET_LAYER")
#pragma pop_macro("HDR_GET_BITRATE")
#pragma pop_macro("HAVE_SSE")
#pragma pop_macro("HAVE_SIMD")
#pragma pop_macro("HAVE_ARMV6")
#pragma pop_macro("FLUSH_BITS")
#pragma pop_macro("FAST_SCALED_FLOAT_TO_INT")
#pragma pop_macro("FAST_HUFFMAN_TABLE_SIZE")
#pragma pop_macro("FAST_HUFFMAN_TABLE_MASK")
#pragma pop_macro("FASTDEF")
#pragma pop_macro("FALSE")
#pragma pop_macro("EOP")
#pragma pop_macro("DQ")
#pragma pop_macro("DIVTAB_NUMER")
#pragma pop_macro("DIVTAB_DENOM")
#pragma pop_macro("DEQ_COUNT1")
#pragma pop_macro("DECODE_VQ")
#pragma pop_macro("DECODE_RAW")
#pragma pop_macro("DECODE")
#pragma pop_macro("CRC32_POLY")
#pragma pop_macro("CODEBOOK_ELEMENT_FAST")
#pragma pop_macro("CODEBOOK_ELEMENT_BASE")
#pragma pop_macro("CODEBOOK_ELEMENT")
#pragma pop_macro("CHECK_BITS")
#pragma pop_macro("CHECK")
#pragma pop_macro("C")
#pragma pop_macro("BSPOS")
#pragma pop_macro("BITS_DEQUANTIZER_OUT")
#pragma pop_macro("ADDEND")

#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(pop)
#elif defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
