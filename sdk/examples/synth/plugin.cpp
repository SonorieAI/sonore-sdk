// SPDX-License-Identifier: Apache-2.0
// Sonore SDK example: a polyphonic subtractive synth. The instrument shape:
// process() takes a third MidiBuffer argument, which is what flips note ports,
// drops the audio input bus and marks the plugin as an instrument everywhere.
#define SONORE_NUM_PARAMS 5

#include <sonore/dsp.h>

namespace {

struct Voice {
  sonore::Oscillator osc;
  sonore::ADSR env;
  sonore::LadderFilter filter;
  float velocity = 1.0f;
  float cutoff = 4000.0f;
  float resonance = 0.2f;
  float sampleRate = 48000.0f;

  void setSampleRate(float sr) {
    sampleRate = sr;
    osc.setSampleRate(sr);
    env.setSampleRate(sr);
    filter.setSampleRate(sr);
    filter.reset();
  }
  void setTone(float cutoffHz, float res) {
    cutoff = cutoffHz;
    resonance = res;
    // Leant open by the note's pressure, as setPressure() leaves it. Set
    // plain, a cutoff knob nudged by a hertz -- or automated, every block --
    // closed the filter under a finger still pressing, until it moved again.
    filter.set(cutoff * (1.0f + 3.0f * pressure), resonance);
  }
  void setEnvelope(float attack, float release) {
    sonore::ADSR::Parameters p;
    p.attack = attack;
    p.decay = 0.15f;
    p.sustain = 0.75f;
    p.release = release;
    env.setParameters(p);
  }
  void noteOn(int note, float vel) {
    velocity = vel;
    key = note;
    bendSemis = 0.0f;
    // The filter goes back with the pressure. Zeroing only the number left
    // the ladder where the voice's last note had leant it -- four times the
    // cutoff after a full press -- and the next note's own "no pressure" was
    // no change by that number, so it played the whole note that bright.
    pressure = 0.0f;
    filter.set(cutoff, resonance);
    retune();
    osc.reset();
    filter.reset();
    env.noteOn();
  }
  void noteOff() { env.noteOff(); }
  /** Silent now, with no release: what a reset means. */
  void kill() {
    env.reset();
    filter.reset();
  }
  bool isActive() const { return env.isActive(); }

  /** Per-note expression, the whole point of MPE: this finger's bend and
   *  pressure, not the instrument's. */
  void setTuning(float semitones) {
    bendSemis = semitones;
    retune();
  }
  void setPressure(float p) {
    const float clamped = p < 0.0f ? 0.0f : (p > 1.0f ? 1.0f : p);
    if (clamped == pressure) return;
    pressure = clamped;
    // Pressure opens this voice's filter -- what a player feels as leaning
    // in. Coefficients are rebuilt only when it MOVES: recomputing a ladder
    // every sample is both wasteful and audible as a change in tone.
    filter.set(cutoff * (1.0f + 3.0f * pressure), resonance);
  }

  float render() { return filter.process(osc.saw()) * env.getNextSample() * velocity; }

private:
  void retune() {
    osc.setFreq((float) sonore::MidiMessage::getMidiNoteInHertz(key) *
                std::pow(2.0f, bendSemis / 12.0f));
  }

public:
  int key = 60;
  float bendSemis = 0.0f, pressure = 0.0f;
};

} // namespace

struct SonoreDsp {
  sonore::VoiceManager<Voice, 8> voices;
  sonore::Smooth levelSm;
  bool levelArrived = false; // the smoother has been put on the control
  float lastCutoff = -1.0f, lastRes = -1.0f, lastAttack = -1.0f, lastRelease = -1.0f;

  /** What a host needs to draw a voice meter and to decide how many MPE
   *  member channels are worth allocating. Answering nothing makes a host
   *  guess, and a guess that is too high sounds like voices stolen for no
   *  reason the player can see. */
  int voiceCapacity() const { return voices.kNumVoices; }
  int activeVoices() const {
    int n = 0;
    for (int i = 0; i < voices.kNumVoices; ++i)
      if (voices.voice(i).isActive()) ++n;
    return n;
  }

  void prepare(const sonore::ProcessSpec& spec) {
    const float sr = (float) spec.sampleRate;
    // Every voice is silenced, not left to finish. The wrapper prepares a DSP
    // with no reset() of its own in place of one -- CLAP's reset(), an AU's
    // Reset -- and both promise the host that every voice is gone: it sends
    // no note-off for a note it started before, and the wrapper forgets the
    // keys it had down. Kept, a held note sounded for ever; across a change
    // of rate it sounded at the wrong pitch as well, its oscillator still
    // stepping at the old rate's increment.
    for (int i = 0; i < voices.kNumVoices; ++i) {
      voices.voice(i).kill();
      voices.voice(i).setSampleRate(sr);
    }
    levelSm.setup(sr, 15.0f);
    levelArrived = false; // it lands on the control in the next block
    lastCutoff = lastRes = lastAttack = lastRelease = -1.0f;
  }

  void process(sonore::ProcessContext& ctx, const float* p) {
    sonore::AudioBlock<float>& io = ctx.main;
    sonore::MidiBuffer& midi = ctx.midi;
    const size_t n = io.getNumSamples();
    float* L = io.getChannelPointer(0);
    float* R = io.getNumChannels() > 1 ? io.getChannelPointer(1) : L;

    // Voice settings only touch the filters when a control actually moved.
    if (p[0] != lastCutoff || p[1] != lastRes) {
      for (int i = 0; i < voices.kNumVoices; ++i) voices.voice(i).setTone(p[0], p[1]);
      lastCutoff = p[0];
      lastRes = p[1];
    }
    if (p[2] != lastAttack || p[3] != lastRelease) {
      for (int i = 0; i < voices.kNumVoices; ++i) voices.voice(i).setEnvelope(p[2], p[3]);
      lastAttack = p[2];
      lastRelease = p[3];
    }

    auto event = midi.begin();
    const auto end = midi.end();
    // Expression events are interleaved with the notes by frame, so a bend
    // that arrives mid-block bends from that frame rather than the next one.
    const sonore::NoteExpressionBuffer::Entry* expr =
        ctx.expression ? ctx.expression->begin() : nullptr;
    const sonore::NoteExpressionBuffer::Entry* exprEnd =
        ctx.expression ? ctx.expression->end() : nullptr;

    // Once per block: the control is constant across it, and the smoother
    // does the ramp, so the dB-to-gain power need not run per sample.
    const float levelTarget = sonore::dbToGain(p[4]);
    // The first block after prepare() LANDS on the control rather than
    // gliding to it: there is no earlier level to come from. It was snapped
    // to a fixed 0.3 in prepare() -- which also answers a reset and the
    // switch to offline rendering -- and a note at the top of a bounce
    // played its first 50 ms from there: 30 times too loud at -40 dB.
    if (!levelArrived) {
      levelSm.snap(levelTarget);
      levelArrived = true;
    }
    for (size_t i = 0; i < n; ++i) {
      // Notes apply at their exact frame: that is what keeps a synth tight.
      while (event != end && (size_t) event->samplePosition <= i) {
        const sonore::MidiMessage& m = event->getMessage();
        // The CHANNEL travels with the note. Under MPE every finger has its
        // own, so two fingers on the same key are two notes -- and without
        // this, lifting one released both.
        // The pedals first: a damper held while the hands lift is the
        // difference between a playable instrument and one that stops dead.
        if (sonore::applyPedals(voices, m)) {
          ++event;
          continue;
        }
        if (m.isNoteOn())
          voices.noteOn(m.getNoteNumber(), m.getFloatVelocity(), m.getChannel() - 1);
        else if (m.isNoteOff())
          voices.noteOff(m.getNoteNumber(), m.getChannel() - 1);
        ++event;
      }
      while (expr != exprEnd && (size_t) expr->samplePosition <= i) {
        const int v = voices.voiceForNote(expr->key, expr->channel);
        if (v >= 0) {
          if (expr->expression == sonore::kExprTuning) voices.voice(v).setTuning(expr->value);
          else if (expr->expression == sonore::kExprPressure)
            voices.voice(v).setPressure(expr->value);
        }
        ++expr;
      }
      const float s = voices.render() * levelSm.next(levelTarget);
      L[i] = s;
      R[i] = s;
    }
  }
};

#include <sonore/plugin.h>

static const sonore::ParamInfo kParamTable[SONORE_NUM_PARAMS] = {
    // The trailing name is the display GROUP: a host's generic editor folds
    // these into "Filter", "Envelope" and "Output" rather than one flat list.
    // SKEWED, and this is the control that makes the case. Linear from 80 Hz
    // to 16 kHz puts 1 kHz at six per cent of the knob: everything a player
    // reaches for is crushed into the bottom and the top half is all hiss.
    // With 1 kHz at the middle the sweep is usable end to end.
    //
    // Spelled as "where the centre should be" rather than as an exponent,
    // because 0.3 is not a number anybody can check and 1000 Hz is.
    {"cutoff", "Cutoff", "Hz", 80.0f, 16000.0f, 4000.0f, 0, "Filter", nullptr, 0, true, false,
     false, sonore::skewForCentre(80.0f, 16000.0f, 1000.0f)},
    {"resonance", "Resonance", "", 0.0f, 0.95f, 0.2f, 0, "Filter"},
    {"attack", "Attack", "s", 0.001f, 2.0f, 0.01f, 0, "Envelope"},
    {"release", "Release", "s", 0.01f, 4.0f, 0.3f, 0, "Envelope"},
    {"level", "Level", "dB", -40.0f, 6.0f, -12.0f, 0, "Output"},
};

static const sonore::PluginDescriptor kDesc = {
    "com.sonorie.example.synth",
    "Sonore Synth",
    "Sonorie",
    "1.0.0",
    "Eight-voice polyphonic saw synth with a ladder filter.",
    "https://sonorie.com",
    true, // instrument
    kParamTable,
    SONORE_NUM_PARAMS,
    nullptr, // no factory presets
    0,
    "synth",  // -> lv2:InstrumentPlugin and friends per format
    nullptr,  // licence falls back to the vendor URL
    nullptr,  // no maintainer email
    2, 2,     // fixed stereo
    nullptr,  // no aux buses
    0,
    false,    // emits no MIDI
    true,     // …but it DOES play expressively: per-note bend and pressure
};

#include <sonore/formats.h> // CLAP, and every format this target is built as
