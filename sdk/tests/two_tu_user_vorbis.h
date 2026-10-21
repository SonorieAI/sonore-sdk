// SPDX-License-Identifier: Apache-2.0
// A user's own stb_vorbis, implementation and all, included the way a project
// that decodes Vorbis itself would -- BEFORE the SDK's audiofile.h. Someone
// else's code: its warnings are not what two_tu_test is testing.
#pragma once
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC system_header
#endif
#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(push, 0)
#pragma warning(disable : 4701 4703)
#endif
#include "../third_party/stb_vorbis.c"
#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(pop)
#endif
