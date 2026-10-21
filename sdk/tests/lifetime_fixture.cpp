// SPDX-License-Identifier: Apache-2.0
// Sonore SDK test fixture: a plugin that says how many of it are alive.
//
// sonore/host.h took two references through queryInterface on a VST3 plugin
// and released neither: the IEditController of a plugin whose component IS its
// controller, and the IMidiMapping of any controller that has one. Every SDK
// plugin is both, so none was ever destroyed -- its DSP's destructor never ran,
// and ExitDll and FreeLibrary pulled the code out from under a live object.
//
// Nothing a host can ask a plugin through VST3 reveals that, so this DSP
// counts its own constructions and destructions in an environment variable,
// which the plugin and the host share because they share a process. Not an
// export: exports_test holds every built plugin to its format's entry points,
// and this is built outside the scanned folder for the same reason.
//
// An instrument, so it takes MIDI and exposes IMidiMapping: both leaks.
#define SONORE_NUM_PARAMS 1

#include <sonore/dsp.h>

void fixtureLiveAdd(int delta);

struct SonoreDsp {
  SonoreDsp() { fixtureLiveAdd(+1); }
  ~SonoreDsp() { fixtureLiveAdd(-1); }
  SonoreDsp(const SonoreDsp&) = delete;
  SonoreDsp& operator=(const SonoreDsp&) = delete;

  void prepare(const sonore::ProcessSpec&) {}
  void process(sonore::ProcessContext& ctx, const float*) { ctx.main.clear(); }
};

#include <sonore/plugin.h>

static const sonore::ParamInfo kParamTable[SONORE_NUM_PARAMS] = {
    // Named outside ASCII on purpose: a host reads a VST3 name as UTF-16 and
    // must hand it on as UTF-8. Escaped, so the bytes do not depend on the
    // compiler's idea of the source encoding.
    {"level", "Gr\xC3\xB6\xC3\x9F" "e", "\xC2\xB5s", 0.0f, 1.0f, 0.5f, 0},
};

static const sonore::PluginDescriptor kDesc = {
    "com.sonorie.test.lifetime",
    "Sonore Lifetime Fixture",
    "Sonorie",
    "1.0.0",
    "An instrument that counts its live instances, for the host's lifetime test.",
    "https://sonorie.com",
    true, // an instrument: MIDI in, so the controller has an IMidiMapping
    kParamTable,
    SONORE_NUM_PARAMS,
    nullptr,
    0,
    "instrument",
    nullptr,
    nullptr,
    2,
    2,
};

#include <sonore/clap_wrapper.h>

#if defined(SONORE_BUILD_VST3)
#include <sonore/vst3_wrapper.h>
#endif

#include <cstdio>
#include <cstdlib>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

// The process's own environment block, not the C runtime's copy of it: a
// plugin and its host can link different runtimes, and only the block is
// shared between them.
void fixtureLiveAdd(int delta) {
  static const char* kName = "SONORE_LIFETIME_FIXTURE_LIVE";
  char text[32] = {0};
#if defined(_WIN32)
  GetEnvironmentVariableA(kName, text, (DWORD) sizeof(text));
#else
  if (const char* v = std::getenv(kName)) std::snprintf(text, sizeof(text), "%s", v);
#endif
  std::snprintf(text, sizeof(text), "%d", std::atoi(text) + delta);
#if defined(_WIN32)
  SetEnvironmentVariableA(kName, text);
#else
  setenv(kName, text, 1);
#endif
}
