// SPDX-License-Identifier: Apache-2.0
// Sonore SDK test fixture: a 64-bit DSP that does everything a process()
// call can have consequences for.
//
// The CLAP wrapper's double-precision path returned straight after the DSP,
// skipping what the float path does next: MIDI and SysEx out, the restart a
// latency change needs, the tail-changed announcement and the dirty-state
// callback. Only Trim speaks doubles among the examples, and Trim does none of
// those things -- so nothing could see them missing. This one does all four,
// in the 64-bit signature, so clap_host_test --expect-64bit-parity can.
#define SONORE_NUM_PARAMS 1

#include <sonore/dsp.h>

struct SonoreDsp {
  float lastSwitch = 0.0f, current = 0.0f;
  bool dirty = false;

  void prepare(const sonore::ProcessSpec&) {}

  // One template for both precisions: the float path is what every other
  // test drives, the double one is what this fixture exists for.
  template <typename Sample>
  void process(sonore::ProcessContextT<Sample>& ctx, const float* p) {
    current = p[0];
    if (p[0] != lastSwitch) {
      lastSwitch = p[0];
      dirty = true;
    }
    // A note in every block -- on, then off a frame later, so every note it
    // starts it also releases -- so a host can see MIDI leave whichever path
    // ran; and SysEx passed along, as any plugin with MIDI in and out must.
    // Graded by clap_host_test as a NOTE EFFECT, the arpeggiator's category.
    ctx.midiOut.addEvent(sonore::MidiMessage::noteOn(0, 60, 100), 0);
    ctx.midiOut.addEvent(sonore::MidiMessage::noteOff(0, 60), 1);
    sonore::forwardSysex(ctx.midi, ctx.midiOut);
  }

  int latencySamples() const { return current > 0.5f ? 64 : 0; }
  int tailSamples() const { return current > 0.5f ? 9999 : 1234; }
  bool consumeStateDirty() {
    const bool was = dirty;
    dirty = false;
    return was;
  }
};

#include <sonore/plugin.h>

static const sonore::ParamInfo kParamTable[SONORE_NUM_PARAMS] = {
    {"switch", "Switch", "", 0.0f, 1.0f, 0.0f, 0},
};

static const sonore::PluginDescriptor kDesc = {
    "com.sonorie.test.double",
    "Sonore Double Fixture",
    "Sonorie",
    "1.0.0",
    "A 64-bit DSP that emits MIDI and moves its latency, tail and state.",
    "https://sonorie.com",
    true, // a note effect: notes in, notes out, no audio input
    kParamTable,
    SONORE_NUM_PARAMS,
    nullptr,
    0,
    "utility",
    nullptr,
    nullptr,
    2,
    2,
    nullptr, // no aux outputs
    0,
    true, // producesMidi
};

#include <sonore/clap_wrapper.h>
