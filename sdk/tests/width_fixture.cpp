// SPDX-License-Identifier: Apache-2.0
// Sonore SDK test fixture: an effect that is ONLY one width.
//
// Every example is stereo-capable, so every LV2 test ran two channels, and
// the port wiring had only ever been exercised at width 2. At width 1 the
// right-input slot in connectPort WAS the output port, so a mono-only effect
// never got its output connected and run() returned on its first line; at
// width 4 the wrapper's two-wide pointer arrays were indexed at 2 and 3.
//
// Built at SONORE_FIXTURE_WIDTH channels, minimum and maximum both, so the
// wrapper has no stereo to fall back on. The DSP passes each channel through
// untouched: lv2_host_test --expect-width feeds every input a different level
// and asserts each output carries its OWN input's, which is what proves each
// port reached the right channel.
#define SONORE_NUM_PARAMS 1

#ifndef SONORE_FIXTURE_WIDTH
#define SONORE_FIXTURE_WIDTH 1
#endif

#include <sonore/dsp.h>

struct SonoreDsp {
  void prepare(const sonore::ProcessSpec&) {}
  void process(sonore::AudioBlock<float>&, const float*) {}
};

#include <sonore/plugin.h>

static const sonore::ParamInfo kParamTable[SONORE_NUM_PARAMS] = {
    {"unused", "Unused", "", 0.0f, 1.0f, 0.0f, 0},
};

static const sonore::PluginDescriptor kDesc = {
#if SONORE_FIXTURE_WIDTH == 1
    "com.sonorie.test.width1",
    "Sonore Width Fixture (mono)",
#else
    "com.sonorie.test.width4",
    "Sonore Width Fixture (quad)",
#endif
    "Sonorie",
    "1.0.0",
    "A passthrough that is only one width, for the port-wiring tests.",
    "https://sonorie.com",
    false, // effect
    kParamTable,
    SONORE_NUM_PARAMS,
    nullptr,
    0,
    "utility",
    nullptr,
    nullptr,
    SONORE_FIXTURE_WIDTH, // minChannels
    SONORE_FIXTURE_WIDTH, // maxChannels: no stereo to fall back on
};

#include <sonore/clap_wrapper.h>

#if defined(SONORE_BUILD_LV2)
#include <sonore/lv2_wrapper.h>
#endif
