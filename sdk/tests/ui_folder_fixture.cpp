// SPDX-License-Identifier: Apache-2.0
// Sonore SDK test fixture: a plugin whose interface is a FOLDER (sonore_add_ui).
//
// Built with sonore_add_plugin(... UI_DIR tests/data/ui_folder UI_ENTRY
// index.html): no SONORE_UI_HTML at all, so the page itself comes out of the
// folder, and the page proves the rest of it before it moves anything. It
// imports an ES module that imports another, fetches 256 bytes holding every
// byte value and checks them, and draws with WebGL2 and reads the pixel back.
// Only when all of that holds does it set the parameter to 0.777;
// clap_host_test --expect-page-sets 0=0.777 watches for it. A folder that was
// not compiled in, not registered, or not served leaves it at its default.
#define SONORE_NUM_PARAMS 1

#include <sonore/dsp.h>

// A gain the parameter turns down, so the host's generic checks -- releasing
// the bypass must change the sound -- have something to hear.
struct SonoreDsp {
  sonore::Smooth gain;
  void prepare(const sonore::ProcessSpec& spec) {
    gain.setup((float) spec.sampleRate, 10.0f);
    gain.snap(1.0f);
  }
  void process(sonore::AudioBlock<float>& io, const float* p) {
    for (size_t i = 0; i < io.getNumSamples(); ++i) {
      const float g = gain.next(1.0f - 0.5f * p[0]);
      for (size_t c = 0; c < io.getNumChannels(); ++c) io.getChannelPointer(c)[i] *= g;
    }
  }
};

#include <sonore/plugin.h>

static const sonore::ParamInfo kParamTable[SONORE_NUM_PARAMS] = {
    {"proof", "Proof", "", 0.0f, 1.0f, 0.0f, 0},
};

static const sonore::PluginDescriptor kDesc = {
    "com.sonorie.test.ui-folder",
    "Sonore UI Folder Probe",
    "Sonorie",
    "1.0.0",
    "A page served from a folder compiled into the plugin.",
    "https://sonorie.com",
    false,
    kParamTable,
    SONORE_NUM_PARAMS,
    nullptr,
    0,
    "utility",
};

#include <sonore/formats.h>
