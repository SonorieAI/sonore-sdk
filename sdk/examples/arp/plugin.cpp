// SPDX-License-Identifier: Apache-2.0
// Sonore SDK example: an arpeggiator, i.e. a plugin whose OUTPUT is MIDI.
// It holds whatever notes are down and emits them one at a time; the audio
// output stays silent, because the sound is made by whatever instrument the
// host routes this into.
//
// Two things make it work: the descriptor says producesMidi, and the DSP takes
// the ProcessContext form so it can write into ctx.midiOut. Everything else --
// the note port, the event bus, the atom sequence, the AU callback -- is the
// wrapper's problem, per format.
#define SONORE_NUM_PARAMS 3

#include <sonore/dsp.h>

struct SonoreDsp {
  // Held notes, lowest first. 16 is past any hand.
  int held[16] = {};
  int numHeld = 0;
  int step = 0;
  double phase = 0.0;   // 0..1 through the current step
  double sr = 48000.0;
  int soundingNote = -1;

  void prepare(const sonore::ProcessSpec& spec) {
    sr = spec.sampleRate > 0.0 ? spec.sampleRate : 48000.0;
    numHeld = 0;
    step = 0;
    phase = 0.0;
    soundingNote = -1;
  }

  void hold(int note) {
    for (int i = 0; i < numHeld; ++i)
      if (held[i] == note) return;
    if (numHeld >= 16) return;
    int i = numHeld++;
    while (i > 0 && held[i - 1] > note) { held[i] = held[i - 1]; --i; }
    held[i] = note;
  }

  void release(int note) {
    for (int i = 0; i < numHeld; ++i)
      if (held[i] == note) {
        for (int j = i; j + 1 < numHeld; ++j) held[j] = held[j + 1];
        --numHeld;
        return;
      }
  }

  void process(sonore::ProcessContext& ctx, const float* p) {
    const size_t n = ctx.getNumSamples();

    // The audio side is silence: an arpeggiator makes no sound of its own.
    ctx.main.clear();

    // Rate in steps per second, gate as a fraction of the step.
    const double stepSamples = sr / (double) (p[0] > 0.1f ? p[0] : 0.1f);
    const double gate = (double) (p[1] < 0.05f ? 0.05f : (p[1] > 0.95f ? 0.95f : p[1]));
    const int octaves = (int) (p[2] + 0.5f);

    // Input notes update the held set at their own offsets. A note arriving
    // mid-block takes effect from that frame, not from the next block.
    int nextEvent = 0;
    const auto* events = ctx.midi.begin();
    const int numEvents = ctx.midi.getNumEvents();

    // SysEx, forwarded for the same reason and up front rather than in the
    // sample loop: it has no musical timing to preserve within a block, and a
    // MIDI-CI negotiation passing through an arpeggiator must come out the
    // other side intact.
    sonore::forwardSysex(ctx.midi, ctx.midiOut);

    for (size_t i = 0; i < n; ++i) {
      while (nextEvent < numEvents && (size_t) events[nextEvent].samplePosition <= i) {
        const sonore::MidiMessage& m = events[nextEvent].message;
        if (m.isNoteOn()) {
          hold(m.getNoteNumber());
        } else if (m.isNoteOff()) {
          release(m.getNoteNumber());
        } else {
          // Everything this arpeggiator does NOT understand is passed on.
          //
          // A MIDI effect sits in the middle of a chain, and one that swallows
          // what it does not recognise breaks everything downstream of it: the
          // sustain pedal stops working, the mod wheel stops working, and an
          // external clock never arrives. Notes are consumed because notes are
          // what this replaces. Nothing else is.
          ctx.midiOut.addEvent(m, (int) i);
        }
        ++nextEvent;
      }

      if (numHeld == 0) {
        // Nothing held: make sure we are not leaving a note stuck on.
        if (soundingNote >= 0) {
          ctx.midiOut.addEvent(sonore::MidiMessage::noteOff(0, soundingNote), (int) i);
          soundingNote = -1;
        }
        phase = 0.0;
        step = 0;
        continue;
      }

      phase += 1.0 / stepSamples;

      // End the sounding note once the gate closes, so the next step starts clean.
      if (soundingNote >= 0 && phase >= gate) {
        ctx.midiOut.addEvent(sonore::MidiMessage::noteOff(0, soundingNote), (int) i);
        soundingNote = -1;
      }

      if (phase >= 1.0) {
        phase -= 1.0;
        const int span = numHeld * (octaves < 1 ? 1 : octaves);
        const int index = step % span;
        int note = held[index % numHeld] + 12 * (index / numHeld);
        if (note > 127) note = 127;
        // A step that starts while the previous note still sounds must end it
        // first, or the host sees two note-ons and one note-off.
        if (soundingNote >= 0)
          ctx.midiOut.addEvent(sonore::MidiMessage::noteOff(0, soundingNote), (int) i);
        ctx.midiOut.addEvent(sonore::MidiMessage::noteOn(0, note, 100), (int) i);
        soundingNote = note;
        step = (step + 1) % span;
      }
    }
  }
};

#include <sonore/plugin.h>

static const sonore::ParamInfo kParamTable[SONORE_NUM_PARAMS] = {
    {"rate", "Rate", "Hz", 0.5f, 20.0f, 8.0f, 0},
    {"gate", "Gate", "", 0.05f, 0.95f, 0.5f, 0},
    {"octaves", "Octaves", "", 1.0f, 4.0f, 1.0f, 4},
};

static const sonore::PluginDescriptor kDesc = {
    "com.sonorie.example.arp",
    "Sonore Arp",
    "Sonorie",
    "1.0.0",
    "Arpeggiator: holds a chord and emits it one note at a time.",
    "https://sonorie.com",
    true, // MIDI in, no audio input: the host files it with the instruments
    kParamTable,
    SONORE_NUM_PARAMS,
    nullptr, // no factory presets
    0,
    "utility", // a note effect, not a sound source
    nullptr,   // licence falls back to the vendor URL
    nullptr,   // no maintainer email
    2,         // fixed stereo (silent) audio output
    2,
    nullptr, // no aux buses
    0,
    true, // …but it DOES emit MIDI
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
