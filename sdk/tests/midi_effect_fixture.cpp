// SPDX-License-Identifier: Apache-2.0
// Sonore SDK test fixture: an EFFECT that reads MIDI.
//
// A note gate: audio passes (at half gain) while a note is held and is silent
// otherwise.
// Its DSP takes the three-argument process(block, params, midi), which in
// CLAP and VST3 earns it a MIDI input. The LV2 wrapper gave a MIDI input only
// to instruments, so as an LV2 this effect never heard a note and never
// opened: lv2_host_test's "the effect produces sound" is the check.
#define SONORE_NUM_PARAMS 5

#include <sonore/dsp.h>

struct SonoreDsp {
  // A real 16-sample delay, reported, so the host test grades it like the
  // delaying effects it already knows (it checks the latency port and the
  // bypass alignment against it).
  static constexpr int kDelay = 16;
  float line[2][kDelay] = {};
  int at = 0;
  int held = 0;
  void prepare(const sonore::ProcessSpec&) {
    held = 0;
    at = 0;
    for (auto& ch : line)
      for (float& v : ch) v = 0.0f;
  }
  int latencySamples() const { return kDelay; }
  void process(sonore::AudioBlock<float>& io, const float*, sonore::MidiBuffer& midi) {
    for (const auto& e : midi) {
      if (e.getMessage().isNoteOn()) ++held;
      else if (e.getMessage().isNoteOff() && held > 0) --held;
    }
    const size_t n = io.getNumSamples();
    const size_t nch = io.getNumChannels() < 2 ? io.getNumChannels() : 2;
    for (size_t i = 0; i < n; ++i) {
      for (size_t c = 0; c < nch; ++c) {
        float* x = io.getChannelPointer(c);
        const float delayed = line[c][at];
        line[c][at] = x[i];
        x[i] = held > 0 ? 0.5f * delayed : 0.0f; // half gain: processing that shows
      }
      at = (at + 1) % kDelay;
    }
  }
};

#include <sonore/plugin.h>

// Ids chosen to clash in LV2: two the wrapper names its own ports, and two
// that map onto one lv2:symbol. Every port symbol must still be unique.
static const sonore::ParamInfo kParamTable[SONORE_NUM_PARAMS] = {
    {"unused", "Unused", "", 0.0f, 1.0f, 0.0f, 0},
    {"enabled", "Enabled (a parameter)", "", 0.0f, 1.0f, 0.0f, 0},
    {"midi_in", "MIDI In (a parameter)", "", 0.0f, 1.0f, 0.0f, 0},
    {"gain-db", "Gain A", "dB", -12.0f, 12.0f, 0.0f, 0},
    {"gain_db", "Gain B", "dB", -12.0f, 12.0f, 0.0f, 0},
};

static const sonore::PluginDescriptor kDesc = {
    "com.sonorie.test.midigate",
    "Sonore MIDI Gate Fixture",
    "Sonorie",
    "1.0.0",
    "Passes audio while a note is held: an effect that reads MIDI.",
    "https://sonorie.com",
    false, // an effect
    kParamTable,
    SONORE_NUM_PARAMS,
    nullptr,
    0,
    "utility",
};

#include <sonore/clap_wrapper.h>

#if defined(SONORE_BUILD_LV2)
#include <sonore/lv2_wrapper.h>
#endif
