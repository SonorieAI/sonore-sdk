// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: every plugin format a build asks for, from one include.
//
// The last line of a plugin source, after the DSP, the parameter table and
// the descriptor:
//
//   #include <sonore/formats.h>
//
// The CLAP wrapper always: it owns the machinery -- the DSP instance,
// parameters, state, the editor -- and every other format adapts it, so the
// formats cannot drift. Then each format the CMake helpers ask for, by the
// macro they define on the target: sonore_add_vst3 SONORE_BUILD_VST3,
// sonore_add_au SONORE_BUILD_AU, sonore_add_lv2 SONORE_BUILD_LV2 (and, for
// its TTL generator, SONORE_LV2_TTLGEN), sonore_add_aax SONORE_BUILD_AAX,
// sonore_add_standalone SONORE_BUILD_STANDALONE, and sonore_add_plugin each
// one it builds.
//
// A source that includes clap_wrapper.h alone builds under every helper, as a
// CLAP and nothing else: a VST3 target linked and exported only clap_entry, a
// standalone had no main(), an LV2 generator no entry point, an AU bundle no
// factory -- and no error named the missing include. The README's template
// was such a source.
#pragma once

#include "clap_wrapper.h"

#if defined(SONORE_BUILD_VST3)
#include "vst3_wrapper.h"
#endif

#if defined(SONORE_BUILD_AU)
#include "au_wrapper.h"
#include "au_view.h"
#endif

#if defined(SONORE_BUILD_LV2)
#include "lv2_wrapper.h"
#endif

// Only from sonore_add_aax, which compiles against the developer's own copy of
// Avid's SDK: never part of the shared object sonore_add_plugin builds, since
// every other bundle would then need the AAX library to link.
#if defined(SONORE_BUILD_AAX)
#include "aax_wrapper.h"
#endif

// standalone.h defines main().
#if defined(SONORE_BUILD_STANDALONE)
#include "standalone.h"
#endif
