// SPDX-License-Identifier: Apache-2.0
// Sonore SDK example: a polyphonic subtractive synth. The instrument shape:
// process() takes the whole ProcessContext, whose MIDI is what makes this an
// instrument -- note ports, no audio input bus, filed as an instrument in
// every format.
//
// What a synth needs past an oscillator, and where each part lives:
//   VoiceManager        which voice plays which note, stealing without a
//                       click, the pedals, Poly / Mono / Legato, polyphony
//   renderVoices()      the block's MIDI and expression applied at their exact
//                       frames, the voices rendered between them
//   Portamento          the glide a mono line makes between notes
//   pitchBendRange()    the wheel's range for a keyboard that is not MPE
//   setMpe(true)        bend and pressure per finger, from an MPE controller
//   setParamMod()       a CLAP host's modulation of the filter, per note
//   voiceCount()        the Voices control, as a CLAP host reads it
#define SONORE_NUM_PARAMS 10
// Tall enough for the generic editor to show all four groups of controls and
// the keyboard under them without scrolling: a keyboard below the fold is one
// a player does not know is there.
#define SONORE_UI_WIDTH 700
#define SONORE_UI_HEIGHT 640

#include <sonore/dsp.h>

namespace {

// The table's rows by name, in the order the table has them -- which is their
// identity in every saved session, so a new control is only ever appended.
enum Param { kCutoff, kResonance, kAttack, kRelease, kLevel, kVoices, kMode, kGlide, kBendRange, kSpread };

struct Voice {
  sonore::Oscillator osc;
  sonore::ADSR env;
  sonore::LadderFilter filter;
  sonore::Portamento glide;
  float velocity = 1.0f;
  float cutoff = 4000.0f;
  float resonance = 0.2f;
  float sampleRate = 48000.0f;
  /** Glide only where a line is played: a poly chord gliding each note from
   *  whatever its voice last played would smear every chord change. */
  bool glides = false;

  void setSampleRate(float sr) {
    sampleRate = sr;
    osc.setSampleRate(sr);
    env.setSampleRate(sr);
    filter.setSampleRate(sr);
    filter.reset();
    glide.setSampleRate(sr);
  }
  void setTone(float cutoffHz, float res) {
    cutoff = cutoffHz;
    resonance = res;
    // Leant open by the note's pressure, as setPressure() leaves it. Set
    // plain, a cutoff knob nudged by a hertz -- or automated, every block --
    // closed the filter under a finger still pressing, until it moved again.
    applyFilter();
  }
  /** A CLAP host's modulation of this note alone (the table marks Cutoff and
   *  Resonance modulatablePerNote): an offset in the control's own units,
   *  added to the knob for this voice and kept until the host moves it. */
  void setParamMod(int param, float amount) {
    if (param == kCutoff) cutoffMod = amount;
    else if (param == kResonance) resonanceMod = amount;
    else return;
    applyFilter();
  }
  void setEnvelope(float attack, float release) {
    sonore::ADSR::Parameters p;
    p.attack = attack;
    p.decay = 0.15f;
    p.sustain = 0.75f;
    p.release = release;
    env.setParameters(p);
  }
  void setGlide(float seconds, bool on) {
    glide.setTime(seconds);
    glides = on && seconds > 0.0f;
  }
  /** Where in the stereo field this voice sits, -1 left to 1 right: equal
   *  power, so a note panned to one side is as loud as one in the middle. */
  void setPan(float pan) {
    const float a = (pan + 1.0f) * 0.25f * 3.14159265f;
    gainL = std::cos(a);
    gainR = std::sin(a);
  }

  void noteOn(int note, float vel) {
    velocity = vel;
    // A voice still sounding glides to the new note (mono: "glide always");
    // a silent one has no pitch to come from.
    if (glides && env.isActive()) glide.glideTo((float) note);
    else glide.jumpTo((float) note);
    bendSemis = 0.0f;
    // The filter goes back with the pressure. Zeroing only the number left
    // the ladder where the voice's last note had leant it -- four times the
    // cutoff after a full press -- and the next note's own "no pressure" was
    // no change by that number, so it played the whole note that bright.
    // A new note's modulation is its own, which the host sends after this.
    pressure = 0.0f;
    cutoffMod = resonanceMod = 0.0f;
    applyFilter();
    retune(glide.current());
    osc.reset();
    filter.reset();
    env.noteOn();
  }
  /** Legato: the next note without a new attack, gliding if glide is on. */
  void legatoTo(int note, float vel) {
    velocity = vel;
    if (glides) glide.glideTo((float) note);
    else glide.jumpTo((float) note);
  }
  void noteOff() { env.noteOff(); }
  /** Silent now, with no release: what a reset and All Sound Off mean. */
  void kill() {
    env.reset();
    filter.reset();
  }
  bool isActive() const { return env.isActive(); }
  /** Per-note expression, the whole point of MPE: this finger's bend and
   *  pressure, not the instrument's. A plain keyboard's wheel arrives the
   *  same way, at the range pitchBendRange() gives. */
  void setTuning(float semitones) { bendSemis = semitones; }
  void setPressure(float p) {
    const float clamped = p < 0.0f ? 0.0f : (p > 1.0f ? 1.0f : p);
    if (clamped == pressure) return;
    pressure = clamped;
    // Pressure opens this voice's filter -- what a player feels as leaning
    // in. Coefficients are rebuilt only when it MOVES: recomputing a ladder
    // every sample is both wasteful and audible as a change in tone.
    applyFilter();
  }
  /** The mod wheel: vibrato, up to half a semitone either way at 5.5 Hz. */
  void setModWheel(float value) { vibratoDepth = 0.5f * value; }

  void render(float& left, float& right) {
    float note = glide.next() + bendSemis;
    if (vibratoDepth > 0.0f) {
      vibratoPhase += 5.5f / sampleRate;
      if (vibratoPhase >= 1.0f) vibratoPhase -= 1.0f;
      note += vibratoDepth * std::sin(6.2831853f * vibratoPhase);
    }
    // The frequency only when the pitch moved: a steady note costs nothing.
    if (note != lastNote) retune(note);
    const float s = filter.process(osc.saw()) * env.getNextSample() * velocity;
    left = s * gainL;
    right = s * gainR;
  }

private:
  void retune(float note) {
    lastNote = note;
    osc.setFreq(440.0f * std::exp2((note - 69.0f) / 12.0f));
  }
  /** The knob, this note's modulation and its pressure, in that order: the
   *  modulated value held to the control's range before pressure leans it. */
  void applyFilter() {
    float c = cutoff + cutoffMod;
    c = c < 80.0f ? 80.0f : (c > 16000.0f ? 16000.0f : c);
    float r = resonance + resonanceMod;
    r = r < 0.0f ? 0.0f : (r > 0.95f ? 0.95f : r);
    filter.set(c * (1.0f + 3.0f * pressure), r);
  }
  float cutoffMod = 0.0f, resonanceMod = 0.0f;
  float gainL = 0.70710678f, gainR = 0.70710678f;
  float vibratoPhase = 0.0f, vibratoDepth = 0.0f;
  float lastNote = -1000.0f;

public:
  float bendSemis = 0.0f, pressure = 0.0f;
};

} // namespace

struct SonoreDsp {
  sonore::VoiceManager<Voice, 8> voices;
  sonore::Smooth levelSm;
  bool levelArrived = false; // the smoother has been put on the control
  float lastCutoff = -1.0f, lastRes = -1.0f, lastAttack = -1.0f, lastRelease = -1.0f;
  float lastGlide = -1.0f, lastSpread = -1.0f;
  int lastVoices = -1, lastMode = -1;
  float bendRange = 2.0f;

  /** What a host needs to draw a voice meter and to decide how many MPE
   *  member channels are worth allocating. Answering nothing makes a host
   *  guess, and a guess that is too high sounds like voices stolen for no
   *  reason the player can see. */
  int voiceCapacity() const { return voices.kNumVoices; }
  /** The voices the patch may use: the Voices control, which a CLAP host
   *  hears about when it moves. */
  int voiceCount() const { return voices.polyphony(); }
  int activeVoices() const { return voices.activeVoices(); }
  /** The wheel's range for a keyboard that is not MPE, from the Bend Range
   *  control: the wrapper's decoder reads it before every block. */
  float pitchBendRange() const { return bendRange; }

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
    voices.allNotesOff();
    voices.setSampleRate(spec.sampleRate); // the steal fade's 2 ms
    // Bend and pressure arrive per note, decoded by the wrapper (the
    // descriptor says supportsMpe): the voice manager leaves the raw wheel
    // alone rather than bend every note twice.
    voices.setMpe(true);
    levelSm.setup(sr, 15.0f);
    levelArrived = false; // it lands on the control in the next block
    lastCutoff = lastRes = lastAttack = lastRelease = lastGlide = lastSpread = -1.0f;
    lastVoices = lastMode = -1;
  }

  void process(sonore::ProcessContext& ctx, const float* p) {
    // Voice settings only touch the voices when a control actually moved.
    if (p[kCutoff] != lastCutoff || p[kResonance] != lastRes) {
      for (int i = 0; i < voices.kNumVoices; ++i) voices.voice(i).setTone(p[kCutoff], p[kResonance]);
      lastCutoff = p[kCutoff];
      lastRes = p[kResonance];
    }
    if (p[kAttack] != lastAttack || p[kRelease] != lastRelease) {
      for (int i = 0; i < voices.kNumVoices; ++i) voices.voice(i).setEnvelope(p[kAttack], p[kRelease]);
      lastAttack = p[kAttack];
      lastRelease = p[kRelease];
    }
    const int mode = (int) (p[kMode] + 0.5f);
    if (mode != lastMode) {
      using Voices = sonore::VoiceManager<Voice, 8>;
      voices.setMode(mode == 1 ? Voices::Mode::Mono : mode == 2 ? Voices::Mode::Legato : Voices::Mode::Poly);
      lastMode = mode;
      lastGlide = -1.0f; // glide is on in Mono and Legato only
    }
    if (p[kGlide] != lastGlide) {
      for (int i = 0; i < voices.kNumVoices; ++i) voices.voice(i).setGlide(p[kGlide], mode != 0);
      lastGlide = p[kGlide];
    }
    const int polyphony = (int) (p[kVoices] + 0.5f);
    if (polyphony != lastVoices) {
      voices.setPolyphony(polyphony);
      lastVoices = polyphony;
    }
    if (p[kSpread] != lastSpread) {
      // The voices fan out from the middle, alternating sides, so the first
      // notes of a chord are already apart: voice 0 left, 1 right, 2 a
      // little less left...
      const float spread = p[kSpread] / 100.0f;
      for (int i = 0; i < voices.kNumVoices; ++i) {
        const float side = (i % 2 == 0) ? -1.0f : 1.0f;
        const float reach = 1.0f - (float) (i / 2) / (float) (voices.kNumVoices / 2);
        voices.voice(i).setPan(spread * side * reach);
      }
      lastSpread = p[kSpread];
    }
    bendRange = p[kBendRange];

    // Notes, pedals, wheels and per-note expression, each at its own frame.
    sonore::renderVoices(voices, ctx);

    // The level, smoothed. Once per block for the target: the control is
    // constant across it, and the smoother does the ramp, so the dB-to-gain
    // power need not run per sample.
    sonore::AudioBlock<float>& io = ctx.main;
    const size_t n = io.getNumSamples();
    float* L = io.getChannelPointer(0);
    float* R = io.getNumChannels() > 1 ? io.getChannelPointer(1) : nullptr;
    const float levelTarget = sonore::dbToGain(p[kLevel]);
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
      const float g = levelSm.next(levelTarget);
      L[i] *= g;
      if (R) R[i] *= g;
    }
  }
};

#include <sonore/plugin.h>

static const char* const kModeNames[] = {"Poly", "Mono", "Legato"};

static const sonore::ParamInfo kParamTable[SONORE_NUM_PARAMS] = {
    // The trailing name is the display GROUP: a host's generic editor folds
    // these into "Filter", "Envelope", "Output" and "Voices" rather than one
    // flat list.
    // SKEWED, and this is the control that makes the case. Linear from 80 Hz
    // to 16 kHz puts 1 kHz at six per cent of the knob: everything a player
    // reaches for is crushed into the bottom and the top half is all hiss.
    // With 1 kHz at the middle the sweep is usable end to end.
    //
    // Spelled as "where the centre should be" rather than as an exponent,
    // because 0.3 is not a number anybody can check and 1000 Hz is.
    //
    // The two a host may modulate, per note as well: the last two fields,
    // modulatable and modulatablePerNote. A CLAP host's LFO or envelope then
    // moves the filter of each voice on its own (Voice::setParamMod).
    {"cutoff", "Cutoff", "Hz", 80.0f, 16000.0f, 4000.0f, 0, "Filter", nullptr, 0, true, false,
     false, sonore::skewForCentre(80.0f, 16000.0f, 1000.0f), true, true},
    {"resonance", "Resonance", "", 0.0f, 0.95f, 0.2f, 0, "Filter", nullptr, 0, true, false, false,
     1.0f, true, true},
    {"attack", "Attack", "s", 0.001f, 2.0f, 0.01f, 0, "Envelope"},
    {"release", "Release", "s", 0.01f, 4.0f, 0.3f, 0, "Envelope"},
    {"level", "Level", "dB", -40.0f, 6.0f, -12.0f, 0, "Output"},
    // Appended after the first five, which sessions already know: their
    // places are their identities, so every new control goes at the end.
    {"voices", "Voices", "", 1.0f, 8.0f, 8.0f, 8, "Voices"},
    {"mode", "Mode", "", 0.0f, 2.0f, 0.0f, 3, "Voices", kModeNames, 3},
    // Most of a glide's useful range is under a quarter of a second.
    {"glide", "Glide", "s", 0.0f, 1.0f, 0.0f, 0, "Voices", nullptr, 0, true, false, false,
     sonore::skewForCentre(0.0f, 1.0f, 0.15f)},
    {"bendrange", "Bend Range", "st", 1.0f, 24.0f, 2.0f, 24, "Voices"},
    {"spread", "Spread", "%", 0.0f, 100.0f, 30.0f, 0, "Output"},
};

static const sonore::PluginDescriptor kDesc = {
    "com.sonorie.example.synth",
    "Sonore Synth",
    "Sonorie",
    "1.0.0",
    "Eight-voice saw synth with a ladder filter: poly, mono and legato with glide, stereo spread, MPE.",
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
