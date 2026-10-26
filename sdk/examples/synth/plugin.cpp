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
//   pitchBendRange(p)   the wheel's range for a keyboard that is not MPE
//   setMpe(true)        bend and pressure per finger, from an MPE controller
//   setParamMod()       a CLAP host's modulation of the filter, per note
//   voiceCount()        the Voices control, as a CLAP host reads it
//   ModRouting/Matrix   three routes from an LFO, an envelope, velocity, key,
//                       pressure or the mod wheel to cutoff, pitch, pan or
//                       level, per voice
#define SONORE_NUM_PARAMS 22
// The generic editor's first groups in view and the rest a scroll away; the
// keyboard is docked along the window's foot, so it is never below the fold
// however far the controls scroll -- a keyboard out of sight is one a player
// does not know is there.
#define SONORE_UI_WIDTH 700
#define SONORE_UI_HEIGHT 640

#include <sonore/dsp.h>

namespace {

// The table's rows by name, in the order the table has them -- which is their
// identity in every saved session, so a new control is only ever appended.
enum Param {
  kCutoff, kResonance, kAttack, kRelease, kLevel, kVoices, kMode, kGlide, kBendRange, kSpread,
  kLfoRate, kLfoShape, kModDecay,
  kMod1Source, kMod1Dest, kMod1Amount, kMod2Source, kMod2Dest, kMod2Amount,
  kMod3Source, kMod3Dest, kMod3Amount
};

// What the modulation matrix can read, and what it can move. Numbered here,
// by the plugin: the matrix knows nothing of what they mean.
enum ModSource { kSrcLfo, kSrcModEnv, kSrcVelocity, kSrcKey, kSrcPressure, kSrcModWheel, kNumSources };
enum ModDest { kDstCutoff, kDstPitch, kDstPan, kDstLevel, kNumDests };
/** What a route's amount means at 100%, per destination: four octaves of
 *  cutoff, an octave of pitch, the whole stereo field, the level doubled or
 *  gone. The matrix carries offsets in these units. */
constexpr float kDestReach[kNumDests] = {4.0f, 12.0f, 1.0f, 1.0f};
constexpr int kRoutes = 3;
/** The modulation's control rate: offsets worked out every this many
 *  samples, 0.33 ms at 48 kHz, which a filter's coefficients can afford. */
constexpr int kControlRate = 16;

struct Voice {
  sonore::Oscillator osc;
  sonore::ADSR env;
  sonore::LadderFilter filter;
  sonore::Portamento glide;
  // The modulation: this voice's LFO and envelope, run at the control rate,
  // and its side of the matrix. The routes are the patch's, held by the DSP.
  sonore::Lfo lfo;
  sonore::ADSR modEnv;
  sonore::ModMatrix<kNumSources, kNumDests> mod;
  const sonore::ModRouting<kRoutes>* routing = nullptr;
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
    // Advanced once per control period, so clocked at the control rate.
    lfo.setSampleRate(sr / (float) kControlRate);
    modEnv.setSampleRate(sr / (float) kControlRate);
    modEnv.reset();
  }
  /** The modulation envelope: a pluck, up at once and down over `decay`
   *  seconds, held or not. */
  void setModDecay(float decay) {
    sonore::ADSR::Parameters p;
    p.attack = 0.002f;
    p.decay = decay;
    p.sustain = 0.0f;
    p.release = decay;
    modEnv.setParameters(p);
  }
  void setTone(float cutoffHz, float res) {
    cutoff = cutoffHz;
    resonance = res;
    // Leant open by the note's pressure, as setPressure() leaves it. Set
    // plain, a cutoff knob nudged by a hertz -- or automated, every block --
    // closed the filter under a finger still pressing, until it moved again.
    applyFilter();
  }
  /** A CLAP host's modulation (the table marks Cutoff and Resonance
   *  modulatablePerNote): this note's own amount, and the plugin-wide one
   *  every note without its own hears. Offsets in the control's own units,
   *  kept by NoteModulation, which lets a note's own replace the plugin-wide
   *  one as CLAP has it; the filter is rebuilt only when one moved. */
  void setParamMod(int param, float amount) {
    if (hostMods.setOwn(param, amount) && (param == kCutoff || param == kResonance)) applyFilter();
  }
  void setParamModWide(int param, float amount) {
    if (hostMods.setWide(param, amount) && (param == kCutoff || param == kResonance)) applyFilter();
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
    basePan = pan;
    placeInField();
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
    hostMods.noteStarted();
    // The note's own modulation sources, and its LFO and envelope started
    // with it: every voice's LFO from the top of its cycle, as a key-synced
    // LFO is.
    mod.reset();
    mod.setSource(kSrcVelocity, vel);
    mod.setSource(kSrcKey, (float) (note - 60) / 48.0f); // +-1 four octaves from middle C
    mod.setSource(kSrcModWheel, wheel);
    lfo.retrigger();
    modEnv.noteOn();
    modCutoff = modPitch = modPan = 0.0f;
    modGain = modGainTarget = 1.0f;
    controlLeft = 0; // worked out on the note's first sample
    applyFilter();
    placeInField();
    retune(glide.current());
    osc.reset();
    filter.reset();
    env.noteOn();
  }
  /** Legato: the next note without a new attack, gliding if glide is on.
   *  A new note to the host, so its own modulation, as at a note-on: the
   *  voice manager brings back what the host gave a key returned to. */
  void legatoTo(int note, float vel) {
    velocity = vel;
    if (glides) glide.glideTo((float) note);
    else glide.jumpTo((float) note);
    if (hostMods.noteStarted()) applyFilter();
  }
  void noteOff() {
    env.noteOff();
    modEnv.noteOff();
  }
  /** Silent now, with no release: what a reset and All Sound Off mean. */
  void kill() {
    env.reset();
    modEnv.reset();
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
    mod.setSource(kSrcPressure, pressure);
    // Pressure opens this voice's filter -- what a player feels as leaning
    // in. Coefficients are rebuilt only when it MOVES: recomputing a ladder
    // every sample is both wasteful and audible as a change in tone.
    applyFilter();
  }
  /** The mod wheel: vibrato, up to half a semitone either way at 5.5 Hz,
   *  and a source for the matrix. */
  void setModWheel(float value) {
    vibratoDepth = 0.5f * value;
    wheel = value;
    mod.setSource(kSrcModWheel, value);
  }

  void render(float& left, float& right) {
    if (--controlLeft <= 0) modulate();
    float note = glide.next() + bendSemis + modPitch;
    if (vibratoDepth > 0.0f) {
      vibratoPhase += 5.5f / sampleRate;
      if (vibratoPhase >= 1.0f) vibratoPhase -= 1.0f;
      note += vibratoDepth * std::sin(6.2831853f * vibratoPhase);
    }
    // The frequency only when the pitch moved: a steady note costs nothing.
    if (note != lastNote) retune(note);
    // Level and pan glide across the control period, so a fast LFO on either
    // is a curve rather than a staircase.
    modGain += modGainStep;
    gainL += gainLStep;
    gainR += gainRStep;
    const float s = filter.process(osc.saw()) * env.getNextSample() * velocity * modGain;
    left = s * gainL;
    right = s * gainR;
  }

private:
  void retune(float note) {
    lastNote = note;
    osc.setFreq(440.0f * std::exp2((note - 69.0f) / 12.0f));
  }
  /** The knob, the host's modulation of this note, the matrix's, and the
   *  note's pressure, in that order: the host's held to the control's range,
   *  the matrix's in octaves on top. */
  void applyFilter() {
    float c = cutoff + hostMods.offset(kCutoff);
    c = c < 80.0f ? 80.0f : (c > 16000.0f ? 16000.0f : c);
    if (modCutoff != 0.0f) c *= std::exp2(modCutoff);
    c = c > 20000.0f ? 20000.0f : c;
    float r = resonance + hostMods.offset(kResonance);
    r = r < 0.0f ? 0.0f : (r > 0.95f ? 0.95f : r);
    filter.set(c * (1.0f + 3.0f * pressure), r);
  }
  /** Where the voice sits, Spread's place for it moved by the matrix: the
   *  equal-power gains it glides to over the next control period. */
  void placeInField() {
    float pan = basePan + modPan;
    pan = pan < -1.0f ? -1.0f : (pan > 1.0f ? 1.0f : pan);
    const float a = (pan + 1.0f) * 0.25f * 3.14159265f;
    targetL = std::cos(a);
    targetR = std::sin(a);
    if (controlLeft <= 0) { // at a note-on: there already
      gainL = targetL;
      gainR = targetR;
      gainLStep = gainRStep = 0.0f;
    } else {
      gainLStep = (targetL - gainL) / (float) controlLeft;
      gainRStep = (targetR - gainR) / (float) controlLeft;
    }
  }
  /** The control-rate tick: the LFO and envelope step on, the matrix sums
   *  every route, and what moved is applied. No routes, and nothing moves. */
  void modulate() {
    controlLeft = kControlRate;
    // Every glide of the last period ends here, on its target, rather than
    // carrying on by its step.
    gainL = targetL;
    gainR = targetR;
    modGain = modGainTarget;
    gainLStep = gainRStep = modGainStep = 0.0f;
    if (!routing || routing->activeCount() == 0) {
      if (modCutoff != 0.0f || modPitch != 0.0f || modPan != 0.0f || modGainTarget != 1.0f) {
        modCutoff = modPitch = modPan = 0.0f;
        modGainTarget = 1.0f;
        applyFilter();
        placeInField();
        modGainStep = (modGainTarget - modGain) / (float) kControlRate;
      }
      return;
    }
    mod.setSource(kSrcLfo, lfo.next());
    mod.setSource(kSrcModEnv, modEnv.getNextSample());
    mod.process(*routing);
    modPitch = mod.offset(kDstPitch);
    const float cutoffOct = mod.offset(kDstCutoff);
    if (cutoffOct != modCutoff) {
      modCutoff = cutoffOct;
      applyFilter();
    }
    const float pan = mod.offset(kDstPan);
    if (pan != modPan) modPan = pan;
    placeInField();
    float g = 1.0f + mod.offset(kDstLevel);
    modGainTarget = g < 0.0f ? 0.0f : (g > 2.0f ? 2.0f : g);
    modGainStep = (modGainTarget - modGain) / (float) kControlRate;
  }
  sonore::NoteModulation<SONORE_NUM_PARAMS> hostMods;
  float basePan = 0.0f, targetL = 0.70710678f, targetR = 0.70710678f;
  float gainL = 0.70710678f, gainR = 0.70710678f, gainLStep = 0.0f, gainRStep = 0.0f;
  float modCutoff = 0.0f, modPitch = 0.0f, modPan = 0.0f;
  float modGain = 1.0f, modGainTarget = 1.0f, modGainStep = 0.0f;
  float wheel = 0.0f;
  int controlLeft = 0;
  float vibratoPhase = 0.0f, vibratoDepth = 0.0f;
  float lastNote = -1000.0f;

public:
  float bendSemis = 0.0f, pressure = 0.0f;
};

} // namespace

struct SonoreDsp {
  sonore::VoiceManager<Voice, 8> voices;
  /** The patch's three routes, read by every voice's matrix. */
  sonore::ModRouting<kRoutes> routing;
  sonore::Smooth levelSm;
  float lastLfoRate = -1.0f, lastModDecay = -1.0f;
  int lastLfoShape = -1;
  bool levelArrived = false; // the smoother has been put on the control
  float lastCutoff = -1.0f, lastRes = -1.0f, lastAttack = -1.0f, lastRelease = -1.0f;
  float lastGlide = -1.0f, lastSpread = -1.0f;
  int lastVoices = -1, lastMode = -1;

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
   *  control: the wrapper's decoder asks before every block, with the values
   *  that block will be handed, so a moved knob bends from that block. */
  float pitchBendRange(const float* p) const { return p[kBendRange]; }

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
      voices.voice(i).routing = &routing;
    }
    lastLfoRate = lastModDecay = -1.0f;
    lastLfoShape = -1;
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

    // The modulation: the LFO and the envelope when their controls moved,
    // and the three routes, which ModRouting::set() leaves alone when a
    // route is as it was.
    if (p[kLfoRate] != lastLfoRate) {
      for (int i = 0; i < voices.kNumVoices; ++i) voices.voice(i).lfo.setRate(p[kLfoRate]);
      lastLfoRate = p[kLfoRate];
    }
    const int shape = (int) (p[kLfoShape] + 0.5f);
    if (shape != lastLfoShape) {
      for (int i = 0; i < voices.kNumVoices; ++i) voices.voice(i).lfo.setShape((sonore::Lfo::Shape) shape);
      lastLfoShape = shape;
    }
    if (p[kModDecay] != lastModDecay) {
      for (int i = 0; i < voices.kNumVoices; ++i) voices.voice(i).setModDecay(p[kModDecay]);
      lastModDecay = p[kModDecay];
    }
    for (int r = 0; r < kRoutes; ++r) {
      // Each choice's step 0 is Off; the rest are the enums above, in order.
      const int source = (int) (p[kMod1Source + 3 * r] + 0.5f) - 1;
      const int dest = (int) (p[kMod1Dest + 3 * r] + 0.5f) - 1;
      const float amount = dest >= 0 ? p[kMod1Amount + 3 * r] / 100.0f * kDestReach[dest] : 0.0f;
      routing.set(r, source, dest, amount);
    }

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
static const char* const kShapeNames[] = {"Sine", "Triangle", "Saw", "Square", "S&H", "Drift"};
static const char* const kSourceNames[] = {"Off",      "LFO",      "Mod Env",  "Velocity",
                                           "Key",      "Pressure", "Mod Wheel"};
static const char* const kDestNames[] = {"Off", "Cutoff", "Pitch", "Pan", "Level"};

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
    // The modulation matrix: an LFO and an envelope per voice, and three
    // routes, each a source, a destination and an amount. Every route starts
    // Off, so a session saved before these existed sounds as it did. The
    // amounts are modulatable: a host's own LFO can deepen the vibrato.
    {"lforate", "LFO Rate", "Hz", 0.05f, 20.0f, 5.0f, 0, "Modulation", nullptr, 0, true, false, false,
     sonore::skewForCentre(0.05f, 20.0f, 2.0f)},
    {"lfoshape", "LFO Shape", "", 0.0f, 5.0f, 0.0f, 6, "Modulation", kShapeNames, 6},
    {"moddecay", "Mod Decay", "s", 0.01f, 4.0f, 0.5f, 0, "Modulation", nullptr, 0, true, false, false,
     sonore::skewForCentre(0.01f, 4.0f, 0.4f)},
    {"mod1src", "Mod 1 Source", "", 0.0f, 6.0f, 0.0f, 7, "Modulation", kSourceNames, 7},
    {"mod1dst", "Mod 1 Target", "", 0.0f, 4.0f, 0.0f, 5, "Modulation", kDestNames, 5},
    {"mod1amt", "Mod 1 Amount", "%", -100.0f, 100.0f, 0.0f, 0, "Modulation", nullptr, 0, true, false,
     false, 1.0f, true},
    {"mod2src", "Mod 2 Source", "", 0.0f, 6.0f, 0.0f, 7, "Modulation", kSourceNames, 7},
    {"mod2dst", "Mod 2 Target", "", 0.0f, 4.0f, 0.0f, 5, "Modulation", kDestNames, 5},
    {"mod2amt", "Mod 2 Amount", "%", -100.0f, 100.0f, 0.0f, 0, "Modulation", nullptr, 0, true, false,
     false, 1.0f, true},
    {"mod3src", "Mod 3 Source", "", 0.0f, 6.0f, 0.0f, 7, "Modulation", kSourceNames, 7},
    {"mod3dst", "Mod 3 Target", "", 0.0f, 4.0f, 0.0f, 5, "Modulation", kDestNames, 5},
    {"mod3amt", "Mod 3 Amount", "%", -100.0f, 100.0f, 0.0f, 0, "Modulation", nullptr, 0, true, false,
     false, 1.0f, true},
};

static const sonore::PluginDescriptor kDesc = {
    "com.sonorie.example.synth",
    "Sonore Synth",
    "Sonorie",
    "1.0.0",
    "Eight-voice saw synth with a ladder filter: poly, mono and legato with glide, a modulation matrix, "
    "stereo spread, MPE.",
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
    sonore::EditorKind::Auto,
    {},       // the editor's default size limits
    true,     // the output guard
    // Sample-accurate automation: a host's point inside a block lands within
    // 16 frames of where it was drawn (a third of a millisecond at 48 kHz),
    // whatever the buffer size, rather than at the top of the block.
    16,
};

#include <sonore/formats.h> // CLAP, and every format this target is built as
