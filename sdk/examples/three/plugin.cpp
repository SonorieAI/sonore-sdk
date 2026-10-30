// SPDX-License-Identifier: Apache-2.0
// Sonore SDK example: a plugin whose face is a three.js scene.
//
// The interface is a FOLDER, not a string: examples/three/ui holds the page
// and its own module, and the build adds three.js beside them (fetched from
// npm at configure, pinned and checked against npm's SHA-512, so the library
// is not carried in this repository). sonore_add_plugin(... UI_DIR UI_ENTRY)
// compiles the folder in, and every webview serves it at the page's own
// address, so the page imports three.js by relative URL like any web page:
//
//   sonore_add_plugin(SonoreThree SOURCES plugin.cpp CLAP SonoreThree
//     UI_DIR <folder> UI_ENTRY index.html)
//
// No SONORE_UI_HTML below: with UI_ENTRY the page comes from the folder.
//
// The sound is deliberately small -- a tilt EQ and an output gain -- because
// the example is about the face. ui/app.js says how the scene is drawn
// without keeping a GPU busy in a DAW all session.
#define SONORE_NUM_PARAMS 2

#include <sonore/dsp.h>

struct SonoreDsp {
  sonore::TiltEq tilt[2];
  sonore::Smooth outSm;
  float lastTilt = 1000.0f;

  void prepare(const sonore::ProcessSpec& spec) {
    for (auto& t : tilt) {
      t.setSampleRate((float) spec.sampleRate);
      t.setPivot(700.0f);
      t.reset();
    }
    lastTilt = 1000.0f;
    outSm.setup((float) spec.sampleRate, 20.0f);
    outSm.snap(1.0f);
  }

  void process(sonore::AudioBlock<float>& io, const float* p) {
    // The shelves are redesigned only when the control moved.
    if (p[0] != lastTilt) {
      for (auto& t : tilt) t.setTilt(p[0]);
      lastTilt = p[0];
    }
    const float out = sonore::dbToGain(p[1]);
    const size_t nch = io.getNumChannels() < 2 ? io.getNumChannels() : 2;
    for (size_t i = 0; i < io.getNumSamples(); ++i) {
      const float g = outSm.next(out);
      for (size_t c = 0; c < nch; ++c) {
        float& x = io.getChannelPointer(c)[i];
        x = tilt[c].process(x) * g;
      }
    }
  }
};

#include <sonore/plugin.h>

static const sonore::ParamInfo kParamTable[SONORE_NUM_PARAMS] = {
    {"tilt", "Tilt", "dB", -6.0f, 6.0f, 0.0f, 0},
    {"output", "Output", "dB", -24.0f, 12.0f, 0.0f, 0},
};

static const sonore::PluginDescriptor kDesc = {
    "com.sonorie.example.three",
    "Sonore Orbit",
    "Sonorie",
    "1.0.0",
    "A tilt EQ whose face is a three.js scene served from a folder.",
    "https://sonorie.com",
    false,
    kParamTable,
    SONORE_NUM_PARAMS,
    nullptr,
    0,
    "eq",
};

#define SONORE_UI_WIDTH 640
#define SONORE_UI_HEIGHT 360

#include <sonore/formats.h>
