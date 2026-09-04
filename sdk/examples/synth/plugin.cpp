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
    filter.set(cutoff, resonance);
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
    pressure = 0.0f;
    retune();
    osc.reset();
    filter.reset();
    env.noteOn();
  }
  void noteOff() { env.noteOff(); }
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
    for (int i = 0; i < voices.kNumVoices; ++i) voices.voice(i).setSampleRate(sr);
    levelSm.setup(sr, 15.0f);
    levelSm.snap(0.3f);
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

    for (size_t i = 0; i < n; ++i) {
      // Notes apply at their exact frame, that is what keeps a synth tight.
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
      const float s = voices.render() * levelSm.next(sonore::dbToGain(p[4]));
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

#include <sonore/clap_wrapper.h>

// The same source builds a VST3 as well: the CLAP wrapper owns the shared
// machinery and the VST3 one adapts it, so the two formats cannot drift.
#if defined(SONORE_BUILD_VST3)
#include <sonore/vst3_wrapper.h>
#endif

// …and an Audio Unit on macOS, from the same sources again.
#if defined(SONORE_BUILD_AU)
#include <sonore/au_wrapper.h>
#include <sonore/au_view.h>
#endif

// …and an LV2 bundle for the Linux-native hosts (the same file also builds
// the TTL generator under SONORE_LV2_TTLGEN).
#if defined(SONORE_BUILD_LV2)
#include <sonore/lv2_wrapper.h>
#endif

// …and a runnable application, no DAW required. standalone.h defines main().
#if defined(SONORE_BUILD_STANDALONE)
#include <sonore/standalone.h>
#endif
