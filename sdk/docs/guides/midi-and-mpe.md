# MIDI and MPE

Instruments that play notes, MPE (MIDI Polyphonic Expression, which bends and
shapes each note on its own), and plugins that send MIDI out.

## An instrument

Setting `isInstrument` in the descriptor does three things: it gives the plugin
a note input, drops its audio input bus, and files it with the instruments in
CLAP, VST3 and LV2 hosts.

An Audio Unit is filed by its CMake call instead. A project generated with
`-DTYPE=instrument` passes `AU_TYPE aumu` to `sonore_add_plugin`; a project
written by hand must pass it too, or the AU registers as an effect.

The DSP takes the notes in a third argument:

```
void process(sonore::AudioBlock<float>& io, const float* p, sonore::MidiBuffer& midi);
```

To have everything at once (MIDI in and out, per-note expression, aux outputs,
the sidechain), use the `sonore::ProcessContext` form instead:
`void process(sonore::ProcessContext& ctx, const float* p)`.

Each event in a `sonore::MidiBuffer` carries its frame within the block
(`samplePosition`). Apply it at that frame rather than at the block's start:
that is what keeps an instrument tight.

Channels are numbered two ways. `sonore::MidiMessage::getChannel()` answers 1
to 16, as MIDI is written, while `noteOn(channel, ...)` and the other
constructors take 0 to 15. So `noteOn(m.getChannel() - 1, ...)` is the round
trip.

`sonore::VoiceManager` does the bookkeeping for a polyphonic instrument:

- which voice a note gets, up to a limit you can lower at run time
  (`setPolyphony()`, a "Voices" control);
- which voice to steal when all are busy: a released voice first, then a repeat
  of the same key, then the oldest that is neither the lowest nor the highest
  note held;
- how it is stolen: the voice's next 2 ms are rendered and faded out under the
  new note, so a steal is not heard as a click (`setSampleRate()` times it);
- the sustain and sostenuto pedals;
- Poly, Mono and Legato (`setMode()`), below.

A voice type needs four methods: `noteOn(int note, float velocity)`,
`noteOff()`, `bool isActive()` and `float render()`. It may have more, and the
manager finds them at compile time:

| Method | What it gets |
| --- | --- |
| `void render(float& left, float& right)` | a stereo voice: panned, spread; it may replace `render()` |
| `void renderBlock(float* left, float* right, int numSamples)` | a stretch between two events, in the voice's own loop: added to both buffers (below) |
| `void legatoTo(int note, float velocity)` | in Legato, the next note without a new attack: where a glide starts |
| `void setPitchBend(float semitones)` | the pitch wheel of the voice's channel |
| `void setTuning(float semitones)` | this note's own bend (MPE) |
| `void setPressure(float)`, `void setBrightness(float)` | this note's pressure and timbre, 0 to 1 |
| `void setModWheel(float)` | CC 1, 0 to 1 |
| `void kill()` | silence now: All Sound Off (CC 120) stops the voice outright |
| `void setParamMod(int param, float amount)` | a host's offset to a parameter for this note alone (below) |

`sonore::renderVoices(voices, ctx)` is a whole instrument block. It applies every
MIDI event and every note expression at its frame and renders the voices
between them into `ctx.main`, stereo voices in stereo.

The stretch between two events goes through `VoiceManager::renderBlock()`. A
voice with only `render()` is run sample by sample, all the voices in turn at
each sample. That is the faster order for most voices: a filter or an envelope
is a chain of steps that each wait for the last, and eight voices' chains
interleaved keep the processor busy where one voice run to the end of the
block leaves it waiting. On the example synth's voice, one voice after another
took 1.7 times as long. A voice whose work does run better over a block, such
as a sample player or a SIMD wavetable read, takes the stretch itself through
`renderBlock()`.

The MIDI goes through `sonore::applyMidi()`, which also works on its own:

- notes, with their channel;
- the pedals, and All Notes Off and All Sound Off;
- the pitch wheel, at `setPitchBendRange()` (2 semitones by default);
- channel pressure, and the mod wheel.

`drainEndedNotes()` reports each note that has finished sounding, release tail
included: its key, its channel and, where `noteOn()` was given one, the host's
id for it. `renderVoices()` passes those on to the host itself (below).

## Mono, legato and glide

`setMode(Mode::Mono)` plays one note at a time and starts the voice again for
each one. `setMode(Mode::Legato)` does the same, except that a note played
while another key is still down moves the voice there through its `legatoTo()`,
with no new attack. Either way the manager remembers the keys held:

- lifting the note that sounds returns to a key still down, which is what a
  trill needs;
- `setNotePriority()` picks which held key sounds: the last pressed (the
  default), the lowest (a bass line) or the highest (a lead).

The glide itself is the voice's: `sonore::Portamento` moves a pitch in
semitones, over a fixed time or at a rate per octave. `jumpTo()` the note in
`noteOn()`, `glideTo()` it in `legatoTo()`, and read `next()` every sample.

## A modulation matrix

`sonore::ModRouting` holds a patch's routes: which source moves which
destination, and how far. `sonore::ModMatrix` is one voice's side of it: the
voice sets its sources, its LFO and envelope, its velocity, and reads back
each destination's offset, the sum of every route into it. The plugin numbers
the sources and destinations itself, so the matrix knows nothing of what they
mean:

```cpp
// test-compile
#include <sonore/dsp.h>

// What the matrix can read, and what it can move: the plugin's own numbering.
enum Source { kLfo, kVelocity, kNumSources };
enum Destination { kCutoff, kPitch, kNumDestinations };

struct Voice {
  sonore::Lfo lfo;
  sonore::ModMatrix<kNumSources, kNumDestinations> mod;
  const sonore::ModRouting<4>* routing = nullptr; // the patch's, shared by every voice
  float cutoffOctaves = 0.0f, pitchSemitones = 0.0f;

  void noteOn(int, float velocity) {
    mod.reset();
    mod.setSource(kVelocity, velocity);
    lfo.retrigger(); // each note's LFO from the top of its cycle
  }
  // Every few samples: the control rate a filter's coefficients can afford.
  void modulate() {
    mod.setSource(kLfo, lfo.next());
    mod.process(*routing);
    cutoffOctaves = mod.offset(kCutoff); // in the units the amounts were set in
    pitchSemitones = mod.offset(kPitch);
  }
};

// Once per block, from the controls. set() leaves an unchanged route alone.
inline void route(sonore::ModRouting<4>& routing, float vibrato) {
  routing.set(0, kLfo, kPitch, 0.5f * vibrato);  // half a semitone at full depth
  routing.set(1, kVelocity, kCutoff, 2.0f);       // two octaves brighter at full velocity
}
```

A route can also take a second source that scales it (`via`), such as the mod
wheel opening an LFO's vibrato. The example synth has three routes, from an
LFO, an envelope, velocity, key, pressure or the mod wheel to cutoff, pitch,
pan or level, worked out every 16 samples.

## Notes the host names, and modulation

A CLAP or VST3 host gives each note it starts an id. `renderVoices()` hands
it to the voice manager with the note (from `ctx.noteIds`), so the note's
expression, its modulation and its end can all be addressed by that id, even
when one key is struck twice in a block.

A CLAP host can also modulate a parameter: its own LFO or envelope adds an
offset to the value, without moving the knob and without being saved. Two
fields of `sonore::ParamInfo` allow it:

- `modulatable`: the host may modulate the parameter for the whole plugin.
  The wrapper adds the offset to the values `process()` is handed, clamped to
  the range, so the DSP needs no code for it.
- `modulatablePerNote`: the host may also modulate it one note at a time.
  `renderVoices()` hands each offset to that note's voice through
  `void setParamMod(int param, float amount)`. The voice adds the amount, in
  the parameter's own units, to the knob's value for itself alone, and clears
  its offsets in `noteOn()`: a new note starts unmodulated, and the host sends
  it its own.

A host keeps modulating a note until the plugin says the note has ended,
release tail included. `renderVoices()` reports that in `ctx.endedNotes`, and
the CLAP wrapper sends it to the host as NOTE_END. A DSP that renders its
voices by hand calls `sonore::reportEndedNotes(voices, *ctx.endedNotes, frame)`
once per block, when `ctx.endedNotes` is not null.

A DSP with a Voices control also declares `int voiceCount() const` beside
`voiceCapacity()`, and a CLAP host is told when it moves. The example synth
(`sdk/examples/synth/plugin.cpp`) does all of this: its Cutoff and Resonance
are modulatable per note.

## MPE

An MPE controller bends, presses and brightens each finger on its own. Setting
`supportsMpe = true` in the descriptor does three things:

- it declares MPE to CLAP and VST3 hosts (an Audio Unit does not report
  `kAudioUnitProperty_SupportsMPE`);
- it decodes MPE's channel-per-note MIDI into expression events, in every
  format;
- it fills `ctx.expression`.

It needs the `ProcessContext` form: the simpler signatures have nowhere to put
the expression. Each event belongs to a voice by key AND channel, since under
MPE two fingers on the same key are two notes; `renderVoices()` matches them
for you.

Call `voices.setMpe(true)`: the wrapper has already folded the pitch wheel and
channel pressure into each note's expression, and the voice manager then
leaves the raw messages alone rather than bend every note twice. A keyboard
that is not MPE still has a wheel, and it bends at two semitones, or at what the
DSP's `float pitchBendRange() const` returns (a "Bend Range" control); the
wrapper passes that on before every block.

```cpp
// test-run
#define SONORE_NUM_PARAMS 2
#include <sonore/dsp.h>

#include <cmath>

struct Voice {
  sonore::Oscillator osc;
  sonore::ADSR env;
  sonore::Portamento glide;
  float velocity = 1.0f, bend = 0.0f, pressure = 0.0f;

  void setSampleRate(float sr) {
    osc.setSampleRate(sr);
    env.setSampleRate(sr);
    glide.setSampleRate(sr);
    glide.setTime(0.08f); // 80 ms from one legato note to the next
    sonore::ADSR::Parameters a;
    a.attack = 0.005f;
    a.decay = 0.2f;
    a.sustain = 0.7f;
    a.release = 0.3f;
    env.setParameters(a);
  }
  // The four methods VoiceManager calls.
  void noteOn(int note, float vel) {
    velocity = vel;
    bend = pressure = 0.0f;
    glide.jumpTo((float) note);
    osc.reset();
    env.noteOn();
  }
  void noteOff() { env.noteOff(); }
  bool isActive() const { return env.isActive(); }
  float render() {
    osc.setFreq(440.0f * std::exp2((glide.next() + bend - 69.0f) / 12.0f));
    return osc.saw() * env.getNextSample() * velocity * (0.6f + 0.4f * pressure);
  }
  // Found at compile time, because they exist.
  void legatoTo(int note, float vel) { // Legato: the next note, no new attack
    velocity = vel;
    glide.glideTo((float) note);
  }
  void setTuning(float semitones) { bend = semitones; } // this finger's bend
  void setPressure(float p) { pressure = p; }            // and its pressure, 0..1
};

using Voices = sonore::VoiceManager<Voice, 8>;

struct SonoreDsp {
  Voices voices;
  sonore::Smooth levelSm;

  void prepare(const sonore::ProcessSpec& spec) {
    for (int i = 0; i < voices.kNumVoices; ++i) voices.voice(i).setSampleRate((float) spec.sampleRate);
    voices.setSampleRate(spec.sampleRate); // times the 2 ms steal fade
    voices.setMpe(true);                   // bend and pressure arrive per note
    levelSm.setup((float) spec.sampleRate, 15.0f);
    levelSm.snap(0.25f);
  }

  // How far a keyboard that is not MPE bends: read before every block.
  float pitchBendRange() const { return 2.0f; }

  void process(sonore::ProcessContext& ctx, const float* p) {
    voices.setMode(p[1] >= 0.5f ? Voices::Mode::Legato : Voices::Mode::Poly);
    // Notes, pedals and per-note expression, each at its own frame.
    sonore::renderVoices(voices, ctx);
    sonore::AudioBlock<float>& io = ctx.main;
    const float level = sonore::dbToGain(p[0]);
    for (size_t i = 0; i < io.getNumSamples(); ++i) {
      const float g = levelSm.next(level);
      for (size_t c = 0; c < io.getNumChannels(); ++c) io.getChannelPointer(c)[i] *= g;
    }
  }
};

#include <sonore/plugin.h>

static const char* const kModeNames[] = {"Poly", "Legato"};

static const sonore::ParamInfo kParamTable[SONORE_NUM_PARAMS] = {
    {"level", "Level", "dB", -40.0f, 6.0f, -12.0f},
    {"mode", "Mode", "", 0.0f, 1.0f, 0.0f, 2, nullptr, kModeNames, 2},
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

A plugin whose output is notes, such as an arpeggiator or a chord generator,
sets `producesMidi` and takes the `ProcessContext` form. It adds events to
`ctx.midiOut` with their frames, the same way they arrive on the input side:

```
ctx.midiOut.addEvent(sonore::MidiMessage::noteOn(0, note, 100), (int) i);
```

Pass on what the plugin does not use itself, such as the sustain pedal, the mod
wheel or a clock, or everything after it in the chain stops working.
`sonore::forwardSysex(ctx.midi, ctx.midiOut)` does that for SysEx. The arp example
(`sdk/examples/arp/plugin.cpp`) is the whole pattern.

## Beyond a plugin's notes

The *MIDI and OSC* area of the [API reference](../index.md) has the rest:
Standard MIDI Files (`midi_file.h`), MIDI 2.0's Universal MIDI Packets
(`ump.h`, `ump128.h`) and MIDI-CI (`midi_ci.h`), MIDI clock, live MIDI devices
for the standalone, and Open Sound Control (`osc.h`).
