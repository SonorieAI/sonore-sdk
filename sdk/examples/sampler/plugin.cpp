// SPDX-License-Identifier: Apache-2.0
// Sonore SDK example: a sampler, and the integration proof for half the
// toolkit.
//
// Every other example exercises one thing. This one is here because the unit
// tests cannot catch what happens when the pieces MEET: a sample loaded by
// audiofile.h, played by SampleVoice through VoiceManager, shaped by ADSR,
// remembered across sessions by StateBag, and drawn in the page from
// WaveformPeaks. If any of those contracts disagree, it shows up here rather
// than in a customer's session.
//
// It ships with a procedurally generated sample so it makes sound with no file
// at all: an instrument whose demo requires the user to find a WAV first is an
// instrument nobody evaluates.
#define SONORE_NUM_PARAMS 4

#include <atomic>
#include <string>
#include <vector>

#include <sonore/audiofile.h>
#include <sonore/dsp.h>
#include <sonore/sampler.h>
#include <sonore/state_bag.h>
#include <sonore/waveform.h>

namespace {

/** One voice: a sample reader with an envelope over it. The voice owns no
 *  envelope of its own by design, so this is where the two are joined. */
struct Voice {
  sonore::SampleVoice player;
  sonore::ADSR env;

  void setSampleRate(float sr) {
    player.setSampleRate((double) sr);
    env.setSampleRate(sr);
  }
  void setSample(const sonore::SampleData* data) { player.setSample(data); }
  void setEnvelope(float attack, float release) {
    sonore::ADSR::Parameters p;
    p.attack = attack;
    p.decay = 0.05f;
    p.sustain = 1.0f; // a sampler sustains at full: the recording has its own shape
    p.release = release;
    env.setParameters(p);
  }
  void setTune(float semitones) { player.setPitchOffset(semitones); }

  void noteOn(int note, float velocity) {
    velocity_ = velocity;
    player.noteOn(note);
    env.noteOn();
  }
  void noteOff() {
    player.noteOff(); // stop looping, let the recording's own tail run
    env.noteOff();
  }
  /** Silence NOW, with no release: for when the memory under the voice is
   *  about to change and a tail would keep reading it. */
  void kill() {
    player.stop();
    env.reset();
  }
  /** Active while EITHER still has something to say: the envelope may still be
   *  releasing after a one-shot ended, and a looping sample may outlast a
   *  finished envelope. Getting this wrong either cuts tails or leaks voices. */
  bool isActive() const { return env.isActive() && player.isActive(); }

  float render(size_t channel) {
    const float s = player.renderChannel(channel);
    if (channel == 0) gain_ = env.getNextSample() * velocity_;
    return s * gain_;
  }
  void advance() { player.advance(); }

private:
  float velocity_ = 1.0f, gain_ = 0.0f;
};

} // namespace

struct SonoreDsp {
  /** A sample and the memory behind it, owned as ONE object, so that handing
   *  the audio thread a different one is a single pointer swap.
   *
   *  The first version of this example kept the storage as a member vector
   *  and reassigned it in loadFile(): the vector freed the buffer every voice
   *  was reading while process() ran on the other thread. clap_wrapper.h
   *  spells out the contract for exactly this ("the DSP OWNS the handover"),
   *  and the example a generated sampler is modelled on broke it. */
  struct Bank {
    std::vector<float> storage;
    sonore::SampleData data;
    sonore::WaveformPeaks peaks;
    std::string path; // empty means the built-in demo sample
    Bank* next = nullptr; // link in the retired stack

    /** Point `data` at `storage` and rebuild the outline the interface draws.
     *  After ANY change to the storage, because a stale pointer here is a
     *  crash rather than a wrong sound. */
    void bind() {
      data.interleaved = storage.data();
      data.numFrames = data.numChannels > 0 ? storage.size() / data.numChannels : 0;
      data.looping = data.loopEnd > data.loopStart && data.loopEnd <= data.numFrames;
      peaks.build(storage.data(), data.numFrames, data.numChannels, 256);
    }
  };

  sonore::VoiceManager<Voice, 8> voices;
  sonore::Smooth levelSm;
  float lastAttack = -1.0f, lastRelease = -1.0f, lastTune = -1000.0f;
  double hostRate = 48000.0;

  // -- The handover --------------------------------------------------------
  // Main thread: builds a Bank, publishes it through `pending`, and frees
  // whatever the audio thread has pushed onto `retired`. Audio thread: takes
  // `pending` at the top of process(), points the voices at it, and pushes
  // the bank it was playing onto `retired`. Every delete happens on the main
  // thread; the audio thread only ever exchanges pointers. `latest` is the
  // main thread's own view of what it last published, for saveState.
  Bank* playing = nullptr;             // [audio thread]
  std::atomic<Bank*> pending{nullptr}; // main -> audio, at most one waiting
  std::atomic<Bank*> retired{nullptr}; // audio -> main, a lock-free stack
  Bank* latest = nullptr;              // [main thread]

  /** The demo sample is built at CONSTRUCTION, not at prepare().
   *
   *  A host may save state before it ever calls prepare(), and it may save
   *  twice and compare the bytes -- clap-validator does exactly that. Building
   *  the sample later meant the root note was 60 before prepare and 69 after,
   *  so save, reload, save produced two different blobs and the plugin looked
   *  non-deterministic. State that changes because of WHEN it was asked for
   *  is state a host cannot trust. */
  SonoreDsp() {
    latest = buildDemoBank();
    playing = latest;
    for (int i = 0; i < voices.kNumVoices; ++i) voices.voice(i).setSample(&playing->data);
  }
  ~SonoreDsp() {
    collectRetired();
    delete pending.exchange(nullptr);
    delete playing;
  }
  // Three raw owners is one too many to let the compiler copy.
  SonoreDsp(const SonoreDsp&) = delete;
  SonoreDsp& operator=(const SonoreDsp&) = delete;

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
    hostRate = spec.sampleRate;
    const float sr = (float) spec.sampleRate;
    for (int i = 0; i < voices.kNumVoices; ++i) voices.voice(i).setSampleRate(sr);
    levelSm.setup(sr, 15.0f);
    levelSm.snap(1.0f);
    lastAttack = lastRelease = -1.0f;
    lastTune = -1000.0f;
  }

  /** The built-in demo: a harmonic tone with an attack, a SUSTAIN LOOP and a
   *  release tail -- which is what an instrument sample actually is, and what
   *  makes a held note hold.
   *
   *  The frequency is 441 Hz, not 440, on purpose: at 44.1 kHz that is
   *  exactly 100 samples per cycle, so a loop spanning a whole number of
   *  cycles joins seamlessly. A loop whose length is not a whole number of
   *  cycles steps in phase at the join and clicks once per cycle, which is
   *  the single most common fault in a hand-made sample library. The four
   *  cents that 441 sits sharp of A440 are then declared as the recording's
   *  own detuning, so the sampler plays it back IN TUNE rather than
   *  inheriting the compromise. */
  static Bank* buildDemoBank() {
    const double rate = 44100.0;
    const double f0 = 441.0;
    const size_t cycle = 100;            // exactly, at this rate
    const size_t loopBegin = cycle * 10; // past the attack
    const size_t loopFinish = cycle * 30;
    const size_t frames = (size_t) (rate * 1.2);
    Bank* b = new Bank;
    b->storage.assign(frames, 0.0f);

    for (size_t i = 0; i < frames; ++i) {
      const double t = (double) i / rate;
      double v = 0.0;
      for (int h = 1; h <= 6; ++h)
        v += std::sin(2.0 * 3.14159265358979 * f0 * (double) h * t) / (double) (h * h);

      double amplitude = 1.0;
      if (i < loopBegin) amplitude = (double) i / (double) loopBegin; // attack
      else if (i > loopFinish) amplitude = std::exp(-3.0 * (t - (double) loopFinish / rate));
      b->storage[i] = (float) (v * amplitude * 0.5);
    }

    b->data.sampleRate = rate;
    b->data.numChannels = 1;
    b->data.rootNote = 69;        // it sounds A
    b->data.tuningCents = -3.93f; // ...but 441 Hz is that far sharp of A440
    b->data.loopStart = loopBegin;
    b->data.loopEnd = loopFinish;
    b->bind();
    return b;
  }

  /** [main thread] Hand a bank to the audio thread. */
  void publish(Bank* b) {
    collectRetired();
    // A bank the audio thread never got round to is superseded, and it is
    // safe to free because nobody but this thread has seen it.
    delete pending.exchange(b, std::memory_order_acq_rel);
    latest = b;
  }

  /** [main thread] Free what the audio thread has finished with. */
  void collectRetired() {
    for (Bank* b = retired.exchange(nullptr, std::memory_order_acquire); b;) {
      Bank* next = b->next;
      delete b;
      b = next;
    }
  }

  /** [audio thread] Switch to a newly published bank, if there is one. */
  void takePending() {
    Bank* b = pending.exchange(nullptr, std::memory_order_acq_rel);
    if (!b) return;
    for (int i = 0; i < voices.kNumVoices; ++i) {
      // Every voice is at a position that means nothing in the new memory:
      // stopped, not released -- a release would keep reading the old one.
      voices.voice(i).kill();
      voices.voice(i).setSample(&b->data); // in-memory sample: allocates nothing
    }
    Bank* old = playing;
    playing = b;
    // Onto the retired stack. A CAS rather than a store, because the main
    // thread may take the whole stack between the load of the head and the
    // store of it -- and a stale head would put a freed bank back on the list.
    old->next = retired.load(std::memory_order_relaxed);
    while (!retired.compare_exchange_weak(old->next, old, std::memory_order_release,
                                          std::memory_order_relaxed)) {
    }
  }

  /** [main thread] Load a file, resampled to nothing: SampleVoice corrects for
   *  the file's rate itself, so the samples are kept as recorded and only the
   *  rate is remembered. Resampling here would throw away quality for no
   *  reason. */
  bool loadFile(const char* path, int rootNote) {
    sonore::WavData file;
    if (!path || !path[0] || !sonore::readAudioFile(path, &file) || file.samples.empty())
      return false;
    Bank* b = new Bank;
    b->storage = std::move(file.samples);
    b->data.sampleRate = (double) file.sampleRate;
    b->data.numChannels = file.numChannels;
    b->data.rootNote = rootNote;
    b->data.tuningCents = 0.0f;
    b->data.loopStart = 0; // a loaded file is a one-shot until told otherwise
    b->data.loopEnd = 0;
    b->path = path;
    b->bind();
    publish(b);
    return true;
  }

  /** Beyond the parameters: which file, and what note it sounds. Both are
   *  [main-thread] by the wrapper's contract, and read the main thread's own
   *  view -- never `playing`, which belongs to the other thread. */
  void saveState(sonore::StateBag& bag) const {
    bag.setString("samplePath", latest->path);
    bag.setInt("rootNote", latest->data.rootNote);
  }
  void loadState(const sonore::StateBag& bag) {
    const std::string path = bag.getString("samplePath", "");
    // An unknown recording is assumed to sound middle C.
    if (!path.empty() && loadFile(path.c_str(), (int) bag.getInt("rootNote", 60))) return;
    // Back to the demo, but only if something else is loaded: a session that
    // saved the demo must come back as the demo, and one that saved a file
    // this machine no longer has is told so by the sound rather than by
    // silence.
    if (!latest->path.empty()) publish(buildDemoBank());
  }

  void process(sonore::ProcessContext& ctx, const float* p) {
    takePending(); // a file loaded since the last block lands here, safely
    sonore::AudioBlock<float>& io = ctx.main;
    sonore::MidiBuffer& midi = ctx.midi;
    const size_t n = io.getNumSamples();
    const size_t channels = io.getNumChannels();
    float* L = io.getChannelPointer(0);
    float* R = channels > 1 ? io.getChannelPointer(1) : L;

    if (p[0] != lastAttack || p[1] != lastRelease) {
      for (int i = 0; i < voices.kNumVoices; ++i) voices.voice(i).setEnvelope(p[0], p[1]);
      lastAttack = p[0];
      lastRelease = p[1];
    }
    if (p[2] != lastTune) {
      for (int i = 0; i < voices.kNumVoices; ++i) voices.voice(i).setTune(p[2]);
      lastTune = p[2];
    }

    auto event = midi.begin();
    const auto end = midi.end();
    // Per-note expression: this finger's bend retunes THIS voice, on top of
    // whatever the Tune control is doing. Declaring supportsMpe without
    // reading these would be a promise the plugin cannot keep.
    const sonore::NoteExpressionBuffer::Entry* expr =
        ctx.expression ? ctx.expression->begin() : nullptr;
    const sonore::NoteExpressionBuffer::Entry* exprEnd =
        ctx.expression ? ctx.expression->end() : nullptr;

    for (size_t i = 0; i < n; ++i) {
      while (expr != exprEnd && (size_t) expr->samplePosition <= i) {
        if (expr->expression == sonore::kExprTuning) {
          const int v = voices.voiceForNote(expr->key, expr->channel);
          if (v >= 0) voices.voice(v).setTune(p[2] + expr->value);
        }
        ++expr;
      }
      while (event != end && (size_t) event->samplePosition <= i) {
        const sonore::MidiMessage& m = event->getMessage();
        // The pedals first: a damper held while the hands lift is the
        // difference between a playable instrument and one that stops dead.
        if (sonore::applyPedals(voices, m)) {
          ++event;
          continue;
        }
        if (m.isNoteOn()) {
          voices.noteOn(m.getNoteNumber(), m.getFloatVelocity(), m.getChannel() - 1);
          // A REUSED voice must forget the previous note's bend.
          //
          // The synth resets bendSemis inside its own noteOn; this voice
          // cannot, because its base pitch lives in a parameter that only
          // process() can see. Without this line a voice bent by expression
          // stayed bent: play a bent note, release it, play another, and the
          // new note is out of tune, and the Tune control could not rescue
          // it either, because line 227 only pushes p[2] to the voices when
          // the control MOVES.
          //
          // Found by turning on VST3 note expression: the section that runs
          // before the pitch-bend one left a voice bent, and the next
          // measurement started at 123.4 Hz instead of 110.
          const int v = voices.voiceForNote(m.getNoteNumber(), m.getChannel() - 1);
          if (v >= 0) voices.voice(v).setTune(p[2]);
        }
        else if (m.isNoteOff()) voices.noteOff(m.getNoteNumber(), m.getChannel() - 1);
        ++event;
      }

      const float gain = levelSm.next(sonore::dbToGain(p[3]));
      float left = 0.0f, right = 0.0f;
      for (int v = 0; v < voices.kNumVoices; ++v) {
        Voice& voice = voices.voice(v);
        if (!voice.isActive()) continue;
        left += voice.render(0);
        right += voice.render(playing->data.numChannels > 1 ? 1 : 0);
        voice.advance();
      }
      L[i] = left * gain;
      R[i] = right * gain;
    }
  }
};

#include <sonore/plugin.h>

static const sonore::ParamInfo kParamTable[SONORE_NUM_PARAMS] = {
    {"attack", "Attack", "s", 0.001f, 2.0f, 0.005f, 0, "Envelope"},
    {"release", "Release", "s", 0.01f, 4.0f, 0.4f, 0, "Envelope"},
    {"tune", "Tune", "st", -24.0f, 24.0f, 0.0f, 0, "Pitch"},
    {"level", "Level", "dB", -40.0f, 12.0f, 0.0f, 0, "Output"},
};

static const sonore::PluginDescriptor kDesc = {
    "com.sonorie.example.sampler",
    "Sonore Sampler",
    "Sonorie",
    "1.0.0",
    "Sample playback with an envelope, remembering its file across sessions.",
    "https://sonorie.com",
    true, // instrument
    kParamTable,
    SONORE_NUM_PARAMS,
    nullptr, // no factory presets
    0,
    "instrument",
    nullptr,
    nullptr,
    2, 2,
    nullptr,
    0,
    false, // emits no MIDI
    true,  // plays expressively: per-note tuning reaches SampleVoice
};

// A test may want the DSP WITHOUT the wrappers -- tests/sampler_stress_test.cpp
// drives this struct from two threads to prove the sample handover -- and it
// says so with this macro. A real build never defines it.
#if !defined(SONORE_EXAMPLE_DSP_ONLY)

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

#endif // !SONORE_EXAMPLE_DSP_ONLY
