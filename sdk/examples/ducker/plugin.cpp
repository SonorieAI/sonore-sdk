// SPDX-License-Identifier: Apache-2.0
// Sonore SDK example: a sidechain ducker, the classic "kick pushes the bass
// down" tool. Its whole reason to exist is the THIRD process() argument: the
// key input arrives on a second stereo bus that every format wrapper grows
// automatically when the DSP declares the sidechain signature. No custom UI on
// purpose: this example also proves the generated fallback faceplate.
#define SONORE_NUM_PARAMS 4

#include <string>

#include <sonore/dsp.h>
#include <sonore/state_bag.h>

struct SonoreDsp {
  sonore::EnvFollower env;
  sonore::Smooth duckSm, outSm;
  float lastAttack = -1.0f, lastRelease = -1.0f;

  void prepare(const sonore::ProcessSpec& spec) {
    const float sr = (float) spec.sampleRate;
    env.setSampleRate(sr);
    env.setTimes(5.0f, 120.0f);
    env.reset();
    duckSm.setup(sr, 12.0f);
    duckSm.snap(1.0f);
    outSm.setup(sr, 12.0f);
    outSm.snap(1.0f);
    lastAttack = lastRelease = -1.0f;
  }

  /** State this DSP owns beyond its parameters. A ducker barely needs one,
   *  but a SAMPLER does, and this is where the wrappers prove the path works
   *  end to end: what goes in here has to come back out of a session saved
   *  by any format. */
  std::string keySource = "sidechain";
  int keyChannel = 0;

  void saveState(sonore::StateBag& bag) const {
    bag.setString("keySource", keySource);
    bag.setInt("keyChannel", keyChannel);
  }

  void loadState(const sonore::StateBag& bag) {
    keySource = bag.getString("keySource", "sidechain");
    keyChannel = (int) bag.getInt("keyChannel", 0);
  }

  void process(sonore::AudioBlock<float>& io, sonore::AudioBlock<float>& sidechain,
               const float* p) {
    const size_t n = io.getNumSamples();
    const size_t nch = io.getNumChannels();
    const bool haveKey = sidechain.getNumSamples() >= n && sidechain.getNumChannels() > 0;
    const float* scL = haveKey ? sidechain.getChannelPointer(0) : nullptr;
    const float* scR = haveKey && sidechain.getNumChannels() > 1
                           ? sidechain.getChannelPointer(1)
                           : scL;

    // Follower times are controls; rebuild only on movement.
    if (p[1] != lastAttack || p[2] != lastRelease) {
      env.setTimes(p[1], p[2]);
      lastAttack = p[1];
      lastRelease = p[2];
    }

    for (size_t i = 0; i < n; ++i) {
      const float amount = duckSm.next(p[0] * 0.01f); // percentage control
      const float outGain = outSm.next(sonore::dbToGain(p[3]));
      const float key =
          scL ? (std::fabs(scL[i]) > std::fabs(scR[i]) ? scL[i] : scR[i]) : 0.0f;
      // The louder the key, the further the main signal ducks. env is a peak
      // follower in 0..~1 for sane material; clamp so a hot key cannot invert.
      float duck = 1.0f - amount * env.process(key) * 4.0f;
      if (duck < 0.0f) duck = 0.0f;
      const float g = duck * outGain;
      for (size_t c = 0; c < nch; ++c) io.getChannelPointer(c)[i] *= g;
    }
  }
};

#include <sonore/plugin.h>

static const sonore::ParamInfo kParamTable[SONORE_NUM_PARAMS] = {
    {"duck", "Duck", "%", 0.0f, 100.0f, 60.0f, 0},
    {"attack", "Attack", "ms", 0.1f, 100.0f, 5.0f, 0},
    {"release", "Release", "ms", 10.0f, 1000.0f, 120.0f, 0},
    {"output", "Output", "dB", -24.0f, 12.0f, 0.0f, 0},
};

static const sonore::PluginDescriptor kDesc = {
    "com.sonorie.example.ducker",
    "Sonore Ducker",
    "Sonorie",
    "1.0.0",
    "Sidechain ducker: the key input pushes the main signal down.",
    "https://sonorie.com",
    false, // effect
    kParamTable,
    SONORE_NUM_PARAMS,
    nullptr, // no factory presets
    0,
    "dynamics", // -> lv2:DynamicsPlugin and friends per format
};

#include <sonore/clap_wrapper.h>

#if defined(SONORE_BUILD_VST3)
#include <sonore/vst3_wrapper.h>
#endif

#if defined(SONORE_BUILD_AU)
#include <sonore/au_wrapper.h>
#include <sonore/au_view.h>
#endif

#if defined(SONORE_BUILD_LV2)
#include <sonore/lv2_wrapper.h>
#endif

#if defined(SONORE_BUILD_STANDALONE)
#include <sonore/standalone.h>
#endif
