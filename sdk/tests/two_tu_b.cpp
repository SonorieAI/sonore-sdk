// SPDX-License-Identifier: Apache-2.0
// The second translation unit of two_tu_a.cpp's program: the same headers.
// A user's own macros, defined before the SDK -- a config.h's HAVE_SSE, a
// test framework's CHECK -- survive it. The codecs define both names, and
// the first fix for their leaking deleted the user's along with theirs.
#define HAVE_SSE 7
#define CHECK(x) (x)
#include <sonore/audiofile.h>
#if !defined(HAVE_SSE) || HAVE_SSE != 7 || !defined(CHECK)
#error "audiofile.h deleted or changed a macro the user defined before it"
#endif
#undef HAVE_SSE
#undef CHECK

// The codecs' own macros end with audiofile.h. A user's CHECK() or V0 met
// minimp3's and stb_vorbis's, and a user's own copy of either met their
// guards -- a minimp3 implementation after this header was skipped and did
// not link.
#if defined(CHECK) || defined(DECODE) || defined(V0) || defined(HDR_SIZE) ||                      \
    defined(USE_MEMORY) || defined(IS_PUSH_MODE) || defined(_MINIMP3_IMPLEMENTATION_GUARD) ||     \
    defined(MINIMP3_MAX_SAMPLES_PER_FRAME) || defined(STBV_CDECL) || defined(temp_alloc) ||       \
    defined(MINIMP3_H) || defined(STB_VORBIS_INCLUDE_STB_VORBIS_H) || defined(LOAD) ||           \
    defined(S0) || defined(S2) || defined(MINIMP3_ONLY_SIMD)
#error "a codec macro outlived audiofile.h"
#endif
// ...so the user's own minimp3, implementation and all, compiles and links
// beside the SDK's private copy.
#include "two_tu_user_mp3.h"

#include <sonore/dsp.h>
#include <sonore/impulse.h>
#include <sonore/sampler.h>

int secondUnitFrames() {
  mp3dec_t own;
  mp3dec_init(&own); // the user's, not the SDK's: a link error when it was skipped
  sonore::WavData empty;
  return sonore::readAudioFile("sonore-two-tu-missing.wav", &empty) ? -1 : 7;
}
