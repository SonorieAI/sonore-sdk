// SPDX-License-Identifier: Apache-2.0
// Sonore SDK example: a channel-flexible trim, the utility every session
// eventually needs. Its whole reason to exist is the descriptor's channel
// RANGE: 1..8, so this one source negotiates mono in a mono host, stereo on a
// normal track and 7.1 on a film stem: CLAP via audio-ports-config, VST3 via
// speaker arrangements, AU via SupportedNumChannels. The DSP loops over
// block.getNumChannels() instead of assuming two; that is the whole contract.
#define SONORE_NUM_PARAMS 2

#include <sonore/dsp.h>

struct SonoreDsp {
  sonore::Smooth gainSm;

  void prepare(const sonore::ProcessSpec& spec) {
    gainSm.setup((float) spec.sampleRate, 10.0f);
    gainSm.snap(1.0f);
  }

  // Written over the SAMPLE TYPE, which is all it takes to earn genuine
  // 64-bit support: a host running a double pipeline gets doubles end to end
  // instead of a silent round trip through float.
  template <typename Sample>
  void process(sonore::AudioBlock<Sample>& io, const float* p) {
    const size_t n = io.getNumSamples();
    const size_t nch = io.getNumChannels();
    const Sample phase = p[1] >= 0.5f ? (Sample) -1 : (Sample) 1;
    // Once per block: the control is constant across it, and the smoother
    // does the ramp, so the dB-to-gain power need not run per sample.
    const float gainTarget = sonore::dbToGain(p[0]);
    for (size_t i = 0; i < n; ++i) {
      const Sample g = (Sample) (gainSm.next(gainTarget)) * phase;
      for (size_t c = 0; c < nch; ++c) io.getChannelPointer(c)[i] *= g;
    }
  }
};

#include <sonore/plugin.h>

static const sonore::ParamInfo kParamTable[SONORE_NUM_PARAMS] = {
    {"gain", "Gain", "dB", -60.0f, 12.0f, 0.0f, 0},
    {"phase", "Phase Flip", "", 0.0f, 1.0f, 0.0f, 2},
};

static const sonore::PluginDescriptor kDesc = {
    "com.sonorie.example.trim",
    "Sonore Trim",
    "Sonorie",
    "1.0.0",
    "Channel-flexible gain trim: mono to 7.1 from one source.",
    "https://sonorie.com",
    false, // effect
    kParamTable,
    SONORE_NUM_PARAMS,
    nullptr, // no factory presets
    0,
    "utility", // -> lv2:UtilityPlugin and friends per format
    nullptr,   // licence falls back to the vendor URL
    nullptr,   // no maintainer email
    1,         // minChannels: happily mono
    8,         // maxChannels: up to 7.1
};

#include <sonore/formats.h> // CLAP, and every format this target is built as
