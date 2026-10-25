# MIDI and MPE

Notes in, expression per note, and MIDI out.

## An instrument

Setting `isInstrument` in the descriptor gives the plugin a note input, drops
its audio input bus and files it with the instruments in CLAP, VST3 and LV2.
An Audio Unit is filed by its CMake call instead: a project generated with
`-DTYPE=instrument` passes `AU_TYPE aumu` to `sonore_add_plugin`, and one
written by hand must too, or the AU registers as an effect. The DSP takes the
notes in a third argument:

```
void process(sonore::AudioBlock<float>& io, const float* p, sonore::MidiBuffer& midi);
```

or, for everything at once -- MIDI in and out, per-note expression, aux
outputs, the sidechain -- the `sonore::ProcessContext` form,
`void process(sonore::ProcessContext& ctx, const float* p)`.

Each event in a `sonore::MidiBuffer` carries its frame (`samplePosition`)
within the block; applying it at that frame rather than at the block's start is
what keeps an instrument tight. `sonore::MidiMessage::getChannel()` answers 1
to 16, as MIDI is written, while `noteOn(channel, ...)` and the other
constructors take 0 to 15: `noteOn(m.getChannel() - 1, ...)` is the round trip.

`sonore::VoiceManager` does the bookkeeping for a polyphonic instrument: which
voice a note gets, which one to steal (a released voice first, then a repeat of
the same key, then the oldest that is neither the lowest nor the highest note
held), and the sustain and sostenuto pedals, through `sonore::applyPedals`. A voice type needs four methods: `noteOn(int note, float
velocity)`, `noteOff()`, `bool isActive()` and `float render()`.

## MPE

An MPE controller bends, presses and brightens each finger on its own.
`supportsMpe = true` in the descriptor declares MPE to CLAP and VST3 hosts (an
Audio Unit does not report `kAudioUnitProperty_SupportsMPE`), decodes MPE's
channel-per-note MIDI into expression events in every format, and fills
`ctx.expression`. It needs the `ProcessContext` form: the simpler signatures
have nowhere to put the expression. Match each event to its voice by key AND
channel -- under MPE two fingers on the same key are two notes:

```cpp
// test-run
#define SONORE_NUM_PARAMS 2
#include <sonore/dsp.h>

#include <cmath>

struct Voice {
  sonore::Oscillator osc;
  sonore::ADSR env;
  int key = 60;
  float velocity = 1.0f, bend = 0.0f, pressure = 0.0f;

  void setSampleRate(float sr) {
    osc.setSampleRate(sr);
    env.setSampleRate(sr);
    sonore::ADSR::Parameters a;
    a.attack = 0.005f;
    a.decay = 0.2f;
    a.sustain = 0.7f;
    a.release = 0.3f;
    env.setParameters(a);
  }
  // The four methods VoiceManager calls.
  void noteOn(int note, float vel) {
    key = note;
    velocity = vel;
    bend = pressure = 0.0f;
    retune();
    osc.reset();
    env.noteOn();
  }
  void noteOff() { env.noteOff(); }
  bool isActive() const { return env.isActive(); }
  float render() { return osc.saw() * env.getNextSample() * velocity * (0.6f + 0.4f * pressure); }

  void retune() {
    osc.setFreq((float) sonore::MidiMessage::getMidiNoteInHertz(key) * std::pow(2.0f, bend / 12.0f));
  }
};

struct SonoreDsp {
  sonore::VoiceManager<Voice, 8> voices;
  sonore::Smooth levelSm;

  void prepare(const sonore::ProcessSpec& spec) {
    for (int i = 0; i < voices.kNumVoices; ++i) voices.voice(i).setSampleRate((float) spec.sampleRate);
    levelSm.setup((float) spec.sampleRate, 15.0f);
    levelSm.snap(0.25f);
  }

  void process(sonore::ProcessContext& ctx, const float* p) {
    sonore::AudioBlock<float>& io = ctx.main;
    auto event = ctx.midi.begin();
    const auto end = ctx.midi.end();
    const sonore::NoteExpressionBuffer::Entry* expr = ctx.expression ? ctx.expression->begin() : nullptr;
    const sonore::NoteExpressionBuffer::Entry* exprEnd = ctx.expression ? ctx.expression->end() : nullptr;
    const float level = sonore::dbToGain(p[0]);

    for (size_t i = 0; i < io.getNumSamples(); ++i) {
      // Notes at their own frame. The channel travels with the note.
      for (; event != end && (size_t) event->samplePosition <= i; ++event) {
        const sonore::MidiMessage& m = event->getMessage();
        if (sonore::applyPedals(voices, m)) continue; // sustain and sostenuto
        if (m.isNoteOn())
          voices.noteOn(m.getNoteNumber(), m.getFloatVelocity(), m.getChannel() - 1);
        else if (m.isNoteOff())
          voices.noteOff(m.getNoteNumber(), m.getChannel() - 1);
      }
      // Per-note expression, also at its own frame.
      for (; expr != exprEnd && (size_t) expr->samplePosition <= i; ++expr) {
        const int v = voices.voiceForNote(expr->key, expr->channel);
        if (v < 0) continue;
        Voice& voice = voices.voice(v);
        if (expr->expression == sonore::kExprTuning) { // semitones from the key
          voice.bend = expr->value * p[1];
          voice.retune();
        } else if (expr->expression == sonore::kExprPressure) { // 0..1
          voice.pressure = expr->value;
        }
      }
      const float s = voices.render() * levelSm.next(level);
      for (size_t c = 0; c < io.getNumChannels(); ++c) io.getChannelPointer(c)[i] = s;
    }
  }
};

#include <sonore/plugin.h>

static const sonore::ParamInfo kParamTable[SONORE_NUM_PARAMS] = {
    {"level", "Level", "dB", -40.0f, 6.0f, -12.0f},
    {"bendAmount", "Bend Amount", "", 0.0f, 1.0f, 1.0f},
};

static const sonore::PluginDescriptor kDesc = [] {
  sonore::PluginDescriptor d;
  d.id = "com.mycompany.mpesaw";
  d.name = "MPE Saw";
  d.vendor = "My Company";
  d.version = "1.0.0";
  d.isInstrument = true; // note input, no audio input, filed as an instrument
  d.params = kParamTable;
  d.numParams = SONORE_NUM_PARAMS;
  d.category = "synth";
  d.supportsMpe = true; // fills ctx.expression; needs the ProcessContext form
  return d;
}();

#include <sonore/formats.h>
```

The descriptor here is built field by field in a lambda, as the showcase
example builds its own: easier to read than a positional list once it sets
fields far down the struct.

## MIDI out

A plugin whose output is notes -- an arpeggiator, a chord generator -- sets
`producesMidi` and takes the `ProcessContext` form; it adds events to
`ctx.midiOut` with their frames, as they arrive on the input side:

```
ctx.midiOut.addEvent(sonore::MidiMessage::noteOn(0, note, 100), (int) i);
```

Pass on what the plugin does not understand -- the sustain pedal, the mod
wheel, a clock -- or everything downstream of it stops working;
`sonore::forwardSysex(ctx.midi, ctx.midiOut)` does it for SysEx. The arp example
(`sdk/examples/arp/plugin.cpp`) is the whole pattern.

## Beyond a plugin's notes

The *MIDI and OSC* area of the [API reference](../index.md) has the rest:
Standard MIDI Files (`midi_file.h`), MIDI 2.0's Universal MIDI Packets
(`ump.h`, `ump128.h`) and MIDI-CI (`midi_ci.h`), MIDI clock, live MIDI devices
for the standalone, and Open Sound Control (`osc.h`).
