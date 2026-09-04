// SPDX-License-Identifier: Apache-2.0
// Compile au_wrapper.h. Nothing here runs -- the compiler reading the file IS
// the test.
//
// au_wrapper.h is Apple-facing, so no Windows or Linux compiler can read it.
// macOS CI builds and validates it on every push; this check answers "does
// this file still parse" on any machine, in seconds.
//
// See au_shim/README.md for exactly what a green run here does and does not
// prove. Short version: internal consistency, not ABI correctness.
#include <sonore/dsp.h>
#include <sonore/plugin.h>

struct SonoreDsp {
  void prepare(const sonore::ProcessSpec&) {}
  void process(sonore::AudioBlock<float>& io, const float*) { (void) io; }
};

#define SONORE_NUM_PARAMS 2
static const sonore::ParamInfo kParamTable[SONORE_NUM_PARAMS] = {
    {"gain", "Gain", "dB", -60.0f, 6.0f, 0.0f, 0},
    {"mode", "Mode", "", 0.0f, 2.0f, 0.0f, 3},
};

static const sonore::PluginDescriptor kDesc = {
    "com.sonorie.test.ausyntax",
    "AU Syntax",
    "Sonorie",
    "1.0.0",
    "A descriptor that exists so a compiler can read au_wrapper.h.",
    "https://sonorie.com",
    false,
    kParamTable,
    SONORE_NUM_PARAMS,
};

#include <sonore/clap_wrapper.h>
#include <sonore/au_wrapper.h>

int main() { return 0; }
