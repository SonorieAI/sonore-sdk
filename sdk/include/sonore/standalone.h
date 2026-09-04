// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: the standalone application.
//
// The standalone stays on the webview deliberately, where the plugin formats
// now default to the native editor. Two reasons, both specific to this
// program: it is ONE instance in ONE process, so the browser's 100-300 MB is
// paid once rather than thirty times; and its page carries the audio and MIDI
// device pickers, which are not plugin parameters and have no native widget
// behind them. A native standalone shell is worth building, and building it
// means building those pickers first.
//
// The same plugin, runnable without a DAW: a window showing the SAME webview
// faceplate, audio through the OS, and the same Instance/bridge machinery the
// plugin formats use, so what you audition standalone IS what loads in a host.
//
// Include it AFTER clap_wrapper.h (it reuses Instance, the bridge wiring and
// the UI queue) in a translation unit compiled with SONORE_BUILD_STANDALONE.
// It defines main().
//
// Modes, because a standalone that can only be judged by ear cannot be tested:
//
//   (no flags)            window + live audio: the actual product
//   --verify              render offline, assert basic health, exit code = verdict
//   --render out.wav [s]  render `s` seconds offline to a WAV (no device needed)
//   --play [s]            live audio without a window, exits after `s` seconds
//   --shot out.bmp [s]    (Windows) open the window, wait, screenshot, exit
//   --test-signal         feed an effect the built-in arpeggio (see below)
//
// The offline modes need no audio device and no display, which is what makes
// the standalone testable in CI on all three platforms.
//
// Effects have an internal test source (a slow saw arpeggio with an envelope):
// harmonically rich enough that filters, saturation and reverbs are audible on
// it. It is a TEST signal and it plays when a test asks for it -- the offline
// modes, --play, or --test-signal. It used to play whenever nothing else fed
// the effect, which meant somebody who double-clicked their delay heard a
// looping arpeggio coming out of a plugin they had just downloaded, with no
// way to reach a device picker (the window shows the PLUGIN's faceplate, not
// a host's chrome) and nothing on screen saying where the sound came from.
// An app that opens on silence and says how to feed it is the lesser surprise.
// Instruments start silent and are played from the on-screen keyboard, or
// programmatically in the offline modes.
#pragma once

#if !defined(SONORE_BUILD_STANDALONE)
#error "standalone.h is only meant for SONORE_BUILD_STANDALONE builds"
#endif

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <unistd.h>
#endif

#include <memory>

#include "denormals.h"
#include "midi_input.h"
#include <chrono>

#include "audio_input.h"
#include "audiofile.h"
#include "midi_output.h"
#include "midi_sequence.h"
#include "user_settings.h"
#include "record.h"
#if defined(_WIN32)
#include "audio_asio.h"
#endif
#include "wav.h"

#if defined(_WIN32)
#include "audio_wasapi.h"
#elif defined(__APPLE__)
#include "audio_coreaudio.h"
#elif defined(__linux__)
#include "audio_alsa.h"
#include "audio_linux.h"
#endif

namespace sonore {
namespace standalone {

using clapwrap::Instance;

/** The standalone has no host to route a key signal, so a sidechain DSP
 *  simply hears silence. */
template <typename T>
inline void runDspStandalone(T& dsp, AudioBlock<float>& block, const float* params,
                             MidiBuffer& midi, midiout::Sink* sink = nullptr,
                             double timeSeconds = 0.0, double sampleRate = 48000.0,
                             MpeDecoder* mpe = nullptr) {
  // Flush-to-zero for this block. Measured at 2.82x on a filter cascade
  // decaying through silence -- see denormals.h. Every wrapper reaches the
  // DSP through one of these functions, so this is the whole surface.
  const ScopedNoDenormals noDenormals;
  (void) mpe;
  (void) sink;
  (void) timeSeconds;
  (void) sampleRate;
  if constexpr (clapwrap::TakesContext<T>::value) {
    // No host to route aux buses to: they get real storage so the DSP can
    // write to them safely, and the audio device only ever hears the main bus.
    static thread_local std::vector<float> silence, auxStore;
    const size_t n = block.getNumSamples();
    const uint32_t nAux = clapwrap::numAuxOutputs();
    if (silence.size() < n) silence.assign(n, 0.0f);
    AudioBlock<float> auxBlocks[clapwrap::kMaxAuxOutputs] = {};
    float* auxPtrs[clapwrap::kMaxAuxOutputs][clapwrap::kMaxAudioChannels] = {};
    size_t flatCount = 0;
    for (uint32_t b = 0; b < nAux; ++b) flatCount += clapwrap::auxBusChannels(b);
    if (auxStore.size() < flatCount * n) auxStore.assign(flatCount * n, 0.0f);
    size_t flat = 0;
    for (uint32_t b = 0; b < nAux; ++b) {
      const uint32_t width = clapwrap::auxBusChannels(b);
      for (uint32_t c = 0; c < width; ++c, ++flat) auxPtrs[b][c] = auxStore.data() + flat * n;
      auxBlocks[b] = AudioBlock<float>(auxPtrs[b], width, n);
    }
    float* sc[2] = {silence.data(), silence.data()};
    AudioBlock<float> scBlock(sc, 2, n);
    // What the plugin PLAYS. This used to be dropped on the floor after the
    // call, which for an arpeggiator or a note splitter meant the entire
    // output of the plugin went nowhere -- the one thing it exists to do.
    static thread_local MidiBuffer midiOut;
    midiOut.clear();
    uint8_t roles[clapwrap::kMaxAudioChannels];
    const uint32_t numRoles = rolesFromMask(defaultChannelMask(
                                                (uint32_t) block.getNumChannels()),
                                            roles, clapwrap::kMaxAudioChannels);
    static thread_local NoteExpressionBuffer expression;
    expression.clear();
    // The same translation every plugin format does, and the one place that
    // did not. CLAP, VST3, LV2 and AU all run incoming MIDI through the MPE
    // zone so a controller's per-finger bend and pressure become per-note
    // expression; the standalone built an empty buffer and handed it over.
    //
    // Which is the one host where somebody is most likely to plug a Seaboard
    // in and find out whether the instrument they just generated responds to
    // it. It did not, and nothing said why: the synth reads pitch only from
    // expression, so a bend on a member channel simply went nowhere.
    if (mpe) mpe->process(midi, expression);
    ProcessContext ctx{block, auxBlocks, nAux,
                       scBlock, midi,    midiOut,
                       numRoles ? roles : nullptr, &expression};
    dsp.process(ctx, params);
    if (sink) {
      for (const auto& entry : midiOut) {
        const int at = entry.samplePosition < 0 ? 0 : entry.samplePosition;
        sink->send(entry.getMessage(), timeSeconds + (double) at / sampleRate);
      }
    }
  } else if constexpr (clapwrap::TakesSidechain<T>::value) {
    static thread_local std::vector<float> silence;
    if (silence.size() < block.getNumSamples()) silence.assign(block.getNumSamples(), 0.0f);
    float* sc[2] = {silence.data(), silence.data()};
    AudioBlock<float> scBlock(sc, 2, block.getNumSamples());
    (void) midi;
    dsp.process(block, scBlock, params);
  } else {
    clapwrap::runDsp(dsp, block, params, midi);
  }
}
// `kDesc` is the plugin's own descriptor at global scope: the same one every
// wrapper reads, found by ordinary lookup.

/**
 * The internal source an effect processes when nothing else is playing: a slow
 * two-octave saw arpeggio with a soft envelope. Chosen over a sine because a
 * sine through a filter is inaudible as a demo, and over noise because noise
 * through a reverb is just more noise.
 */
/** A file played into an effect, instead of the built-in test tone.
 *
 *  "An effect gets its input from the standalone's internal source (or a WAV,
 *  once the user drops one)" is what the audio backend's own header said, and
 *  the parenthesis never happened. A distortion you can only feed a sine to
 *  is a demo of a sine, and rendering a plugin over real material is the
 *  first thing anyone wants from a standalone build.
 *
 *  Whatever readAudioFile() accepts works, so this is WAV, AIFF, FLAC, MP3
 *  and Ogg without a line of format code here.
 *
 *  Resampling is NOT done. A 44.1k file played at 48k would come out a
 *  semitone-ish sharp and slightly short, and silently doing that to
 *  somebody's material is worse than saying the rate does not match --
 *  especially when the answer is to pass the file the plugin will actually
 *  run at. */
class FileSource {
public:
  bool load(const char* path, double expectedRate) {
    if (!readAudioFile(path, &data_)) {
      error_ = std::string("could not read ") + path;
      return false;
    }
    if (data_.numChannels == 0 || data_.numFrames() == 0) {
      error_ = std::string("there is no audio in ") + path;
      return false;
    }
    if (expectedRate > 0.0 && data_.sampleRate > 0 &&
        std::fabs((double) data_.sampleRate - expectedRate) > 1.0) {
      char buffer[160];
      std::snprintf(buffer, sizeof(buffer),
                    "%s is %u Hz and this run is %.0f Hz; no resampling is done",
                    path, (unsigned) data_.sampleRate, expectedRate);
      error_ = buffer;
      return false;
    }
    position_ = 0;
    return true;
  }

  bool loaded() const { return data_.numFrames() > 0; }
  size_t numFrames() const { return data_.numFrames(); }
  /** True once every frame has been handed out, so a render knows when the
   *  material ends rather than looping it or padding it forever. */
  bool finished() const { return position_ >= data_.numFrames(); }
  const std::string& error() const { return error_; }

  /** One block, up-mixed or down-mixed to stereo. Past the end is silence,
   *  not a wrap: an effect's release tail is worth hearing and a loop point
   *  in the middle of it is not. */
  void read(float* left, float* right, uint32_t frames) {
    const size_t total = data_.numFrames();
    const uint16_t channels = data_.numChannels;
    for (uint32_t i = 0; i < frames; ++i) {
      if (position_ >= total) {
        left[i] = right[i] = 0.0f;
        continue;
      }
      const float* frame = data_.samples.data() + position_ * channels;
      left[i] = frame[0];
      right[i] = channels > 1 ? frame[1] : frame[0];
      ++position_;
    }
  }

  void rewind() { position_ = 0; }

private:
  WavData data_;
  size_t position_ = 0;
  std::string error_;
};

class TestSource {
public:
  void prepare(double sampleRate) {
    sampleRate_ = sampleRate > 1.0 ? sampleRate : 48000.0;
    phase_ = 0.0;
    position_ = 0;
  }

  inline float next() {
    static constexpr double kNotes[] = {110.0, 164.81, 220.0, 261.63, 329.63, 261.63, 220.0, 164.81};
    const double noteLength = sampleRate_ * 0.4; // 150 BPM eighths, roughly
    const size_t note = (size_t) ((double) position_ / noteLength) % 8;
    const double t = std::fmod((double) position_, noteLength) / noteLength;
    ++position_;

    phase_ += kNotes[note] / sampleRate_;
    if (phase_ >= 1.0) phase_ -= 1.0;
    const float saw = (float) (2.0 * phase_ - 1.0);

    // Soft attack and release per note, so the source has transients without
    // clicking, and a level that leaves headroom for drive-type effects.
    const double env = t < 0.05 ? t / 0.05 : (t > 0.8 ? (1.0 - t) / 0.2 : 1.0);
    return saw * (float) env * 0.22f;
  }

private:
  double sampleRate_ = 48000.0;
  double phase_ = 0.0;
  uint64_t position_ = 0;
};

/** Everything a running standalone owns. */
struct App {
  Instance instance;
  TestSource source;
  /** Used INSTEAD of `source` when a file was given. Not merged with it: an
   *  effect fed both would be an effect fed neither. */
  FileSource fileSource;
  /** Used instead of BOTH when a capture device was opened. A live input
   *  outranks a file for the same reason a file outranks the test tone: it is
   *  the more specific thing the user asked for. */
  PlatformAudioInput audioInput;
  bool inputOpen = false;
  /** Whether the built-in arpeggio feeds an effect when nothing else does.
   *  Off unless asked for: see the banner. The offline modes turn it on
   *  because a render of silence measures nothing. */
  bool testSignal = false;
  /** How many times the keyboard has been opened this run. Should be one, and
   *  --switch-devices fails if it is not: this was six for a six-device sweep
   *  until MIDI stopped being a property of the soundcard, and nothing on the
   *  machine that found it would have noticed. */
  int midiOpens = 0;
  /** Which MIDI input is open, or -1. The picker shows it. */
  int midiIndex = -1;
  double sampleRate = 48000.0;
  /** Live MIDI, when a device was found. The queue is written by the device
   *  thread and drained by the audio callback -- never the other way round. */
  midiin::MessageQueue midiQueue;
  midiin::Device midiDevice;
  bool midiOpen = false;

  /** Where the plugin's OWN notes go, if anywhere. Null is the normal case
   *  and means the same thing it always did -- a synth has nothing to send
   *  and an effect that does has nowhere asked for. */
  midiout::Sink* midiSink = nullptr;
  /** A performance to play into the plugin, instead of the one note the
   *  offline render invents. What turns an instrument standalone into
   *  something that can render a demo of itself. */
  MidiSequence midiIn;
  bool midiFileLoaded = false;
  /** Samples since the run began, so a file sink writes ticks that mean
   *  something. Not a wall clock: two renders of the same thing have to
   *  produce the same file. */
  uint64_t framesElapsed = 0;
};

/** One block of the plugin, exactly as the formats run it: drain UI events,
 *  synthesize or pass input, run the DSP, publish the meters. */
/**
 * One block.
 *
 * `externalIn` is audio the DEVICE already has, interleaved: the ASIO path
 * gets input and output from one driver in one callback, so there is nothing
 * to open separately and nothing to resample. Null means the usual chain --
 * a capture device, a loaded file, or the test source -- applies instead.
 */
inline void processBlock(App& app, float* left, float* right, uint32_t frames,
                         const float* externalIn = nullptr, uint32_t externalChannels = 0) {
  Instance* inst = &app.instance;
  inst->midi.clear();
  // Whatever a keyboard sent since the last block, ahead of the UI events so
  // a note and a knob move in the same block keep the order they happened in.
  if (app.midiOpen) app.midiQueue.drain(inst->midi);
  // A file being played, at the position the run has reached. Before the UI
  // events for the same reason a keyboard is: what happened first goes first.
  if (app.midiFileLoaded)
    app.midiIn.fillBuffer(inst->midi, (double) app.framesElapsed / app.sampleRate, frames,
                          app.sampleRate);
  clapwrap::drainUiEvents(inst, nullptr);

  if (kDesc.isInstrument) {
    std::memset(left, 0, frames * sizeof(float));
    std::memset(right, 0, frames * sizeof(float));
  } else if (externalIn && externalChannels > 0) {
    // Ahead of every other source, because the device that is about to play
    // the output is the device this came from. One clock, no drift, no ring.
    for (uint32_t i = 0; i < frames; ++i) {
      const float* frame = externalIn + (size_t) i * externalChannels;
      left[i] = frame[0];
      right[i] = externalChannels > 1 ? frame[1] : frame[0];
    }
  } else if (app.inputOpen) {
    app.audioInput.read(left, right, frames);
  } else if (app.fileSource.loaded()) {
    app.fileSource.read(left, right, frames);
  } else if (app.testSignal) {
    for (uint32_t i = 0; i < frames; ++i) left[i] = right[i] = app.source.next();
  } else {
    // Nothing is feeding this effect. Silence, not a tune nobody asked for.
    std::memset(left, 0, frames * sizeof(float));
    std::memset(right, 0, frames * sizeof(float));
  }

  float* chans[2] = {left, right};
  clapwrap::sendTransport(inst->dsp, TransportInfo{});
  // The device is stereo; a mono-only DSP processes the left channel and the
  // result is mirrored so both speakers play.
  const bool monoDsp = clapwrap::maxMainChannels() < 2;
  AudioBlock<float> block(chans, monoDsp ? 1 : 2, frames);
  clapwrap::snapshotParams(inst);
  runDspStandalone(inst->dsp, block, inst->paramsBlock, inst->midi, app.midiSink,
                   (double) app.framesElapsed / app.sampleRate, app.sampleRate,
                   kDesc.supportsMpe ? &inst->mpe : nullptr);
  app.framesElapsed += frames;
  if (monoDsp && right != left) std::memcpy(right, left, sizeof(float) * frames);

  inst->meter.push(measureBlock(left, (size_t) frames));
}

/** The standalone names itself, because it IS the host. A DSP that adapts to
 *  its surroundings should be told it is not in a DAW at all rather than
 *  seeing an empty name and guessing. */
inline void tellHostInfo(App& app) {
  app.instance.hostInfo.name = "Sonore Standalone";
  app.instance.hostInfo.vendor = "Sonorie";
  app.instance.hostInfo.version = "1.0.0";
  clapwrap::sendHostInfo(app.instance.dsp, app.instance.hostInfo);
}

inline void prepare(App& app, double sampleRate, uint32_t maxBlock) {
  tellHostInfo(app);
  app.sampleRate = sampleRate;
  ProcessSpec spec;
  spec.sampleRate = sampleRate;
  spec.maximumBlockSize = maxBlock;
  spec.numChannels = 2;
  app.instance.dsp.prepare(spec);
  app.source.prepare(sampleRate);
  for (int i = 0; i < SONORE_NUM_PARAMS && i < kDesc.numParams; ++i)
    app.instance.params[i] = kDesc.params[i].defaultValue;
}

// ── Offline rendering ────────────────────────────────────────────────────────

/** Render `seconds` offline at 48 kHz. For instruments, a short phrase is
 *  played programmatically so the render contains sound to judge. */
inline std::vector<float> renderOffline(App& app, double seconds) {
  constexpr double kRate = 48000.0;
  constexpr uint32_t kBlock = 128;
  prepare(app, kRate, kBlock);
  // A test asking for the test signal. Offline is where the arpeggio earns its
  // keep -- a render of silence measures nothing and --verify would be grading
  // an empty file -- and it is off everywhere else, so an app somebody opens
  // does not play a tune at them. A loaded file still wins: the branch order in
  // processBlock is external input, then capture, then file, then this.
  app.testSignal = true;

  uint64_t total = (uint64_t) (seconds * kRate);
  if (app.midiFileLoaded) {
    // The performance decides, plus the plugin's own tail so the last note
    // gets to finish rather than being cut off at its note-off.
    app.framesElapsed = 0;
    total = (uint64_t) (app.midiIn.duration() * kRate) +
            (uint64_t) clapwrap::dspTail(app.instance.dsp) + (uint64_t) kRate;
  }
  if (app.fileSource.loaded()) {
    // The material decides, plus the plugin's own tail so a reverb is not
    // cut off at the last input sample. Asking a user to also pass --seconds
    // matching the file they just passed would be asking them to do a
    // subtraction the program can do.
    app.fileSource.rewind();
    total = (uint64_t) app.fileSource.numFrames() +
            (uint64_t) clapwrap::dspTail(app.instance.dsp);
  }
  std::vector<float> interleaved;
  interleaved.reserve((size_t) total * 2);

  float left[kBlock], right[kBlock];
  uint64_t done = 0;
  // Explicit one-shot flags. A first version encoded the phrase state in a
  // note number's sign, and after note-off the "no note" test matched again:
  // the note retriggered every block and the release-tail check caught it.
  bool noteOnSent = false;
  bool noteOffSent = false;
  while (done < total) {
    // The instrument phrase: note on at 5%, off at 60% of the render, so the
    // file also shows the release tail.
    if (kDesc.isInstrument && !app.midiFileLoaded) {
      const double at = (double) done / (double) total;
      if (at >= 0.05 && !noteOnSent) {
        noteOnSent = true;
        UiEventQueue::Event e;
        e.kind = UiEventQueue::Event::Kind::NoteOn;
        e.index = 69;
        e.value = 100.0f;
        app.instance.uiEvents.push(e);
      } else if (at >= 0.6 && noteOnSent && !noteOffSent) {
        noteOffSent = true;
        UiEventQueue::Event e;
        e.kind = UiEventQueue::Event::Kind::NoteOff;
        e.index = 69;
        app.instance.uiEvents.push(e);
      }
    }
    const uint32_t frames = (uint32_t) ((total - done) < kBlock ? (total - done) : kBlock);
    processBlock(app, left, right, frames);
    for (uint32_t i = 0; i < frames; ++i) {
      interleaved.push_back(left[i]);
      interleaved.push_back(right[i]);
    }
    done += frames;
  }
  return interleaved;
}

/** Run the plugin on a LIVE input for a fixed time and keep the result.
 *
 *  Not renderOffline with a different source: offline runs as fast as the CPU
 *  allows, and a capture device delivers at exactly one speed. This has to
 *  wait, which is the whole difference, and waiting is why the pacing below
 *  is against the CLOCK rather than a fixed sleep per block -- sleeping 2.6 ms
 *  per 128-frame block drifts, and drift against a device that does not is a
 *  ring that empties or overflows depending on which way you rounded.
 *
 *  Returns interleaved stereo at the input's rate. Empty means the input was
 *  never opened, which the caller has already been told about. */
/**
 * Record the live input through the plugin, straight to disk.
 *
 * This used to grow a std::vector for the whole take and write it at the end,
 * which is fine for the twenty seconds a test renders and turns into
 * gigabytes for an actual recording -- and puts a reallocation in the middle
 * of a capture loop. WavRecorder hands each block to a writer thread instead,
 * so memory is one ring however long the take runs.
 *
 * Returns the number of frames that reached the file, or 0 if it could not be
 * opened.
 */
inline uint64_t captureLive(App& app, double seconds, const char* path) {
  if (!app.inputOpen || !path || !*path) return 0;

  const double rate = app.audioInput.sampleRate() > 0.0 ? app.audioInput.sampleRate() : 48000.0;
  constexpr uint32_t kBlock = 128;
  prepare(app, rate, kBlock);

  WavRecorder recorder;
  if (!recorder.start(path, rate, 2)) return 0;

  const uint64_t total = (uint64_t) (seconds * rate);
  float left[kBlock], right[kBlock];
  float block[kBlock * 2];
  uint64_t done = 0;
  const auto started = std::chrono::steady_clock::now();
  while (done < total) {
    // Where the DEVICE should be by now. Blocks are taken only as fast as it
    // could have produced them; running ahead would drain the ring and record
    // the silence that a starved read correctly returns.
    const double elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    const uint64_t shouldHave = (uint64_t) (elapsed * rate);
    if (done + kBlock > shouldHave) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      continue;
    }
    const uint32_t frames = (uint32_t) ((total - done) < kBlock ? (total - done) : kBlock);
    processBlock(app, left, right, frames);
    for (uint32_t i = 0; i < frames; ++i) {
      block[i * 2] = left[i];
      block[i * 2 + 1] = right[i];
    }
    recorder.write(block, frames);
    done += frames;
  }
  recorder.stop();
  if (recorder.framesDropped() > 0)
    std::printf("[sonore] the disk fell behind: %llu frames are missing from the recording\n",
                (unsigned long long) recorder.framesDropped());
  return recorder.framesWritten();
}

/** The health assertions --verify runs. The same physics the plugin gates use:
 *  finite always; audible for an effect (it processes the source) and for an
 *  instrument's held note; silent again after an instrument's release. */
inline int verify(App& app) {
  std::printf("standalone verify: %s (%s)\n", kDesc.name,
              kDesc.isInstrument ? "instrument" : "effect");

  // A plugin whose output is NOTES gets rendered into a file, because that is
  // the only place its output exists. Set up before the render, not after:
  // there is nowhere to recover the notes from once the blocks are gone.
  const char* kMidiPath = "sonore-verify.mid";
  std::unique_ptr<midiout::FileSink> sink;
  if (kDesc.producesMidi) {
    sink.reset(new midiout::FileSink(kMidiPath));
    app.midiSink = sink.get();
  }

  const std::vector<float> audio = renderOffline(app, 2.0);
  app.midiSink = nullptr;

  bool finite = true;
  double energy = 0.0;
  float peak = 0.0f;
  for (float v : audio) {
    if (!std::isfinite(v)) finite = false;
    const float a = v < 0.0f ? -v : v;
    if (a > peak) peak = a;
    energy += (double) v * v;
  }

  int failures = 0;
  auto check = [&failures](bool ok, const char* what) {
    std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
  };

  check(finite, "the render is finite (no NaN/Inf)");
  if (!kDesc.producesMidi) {
    // Asked only of plugins whose output is audio. An arpeggiator's stereo
    // bus is silent BY DESIGN, and demanding sound from it would be demanding
    // that it stop being an arpeggiator.
    check(energy > 0.0, "the render contains sound");
  }
  check(peak < 4.0f, "the render does not blow up");

  if (kDesc.producesMidi) {
    const size_t sent = sink->numEvents();
    check(sink->close(), "the MIDI the plugin played was written to a file");

    // Read BACK, not trusted. The sink counting its own events proves the
    // buffer was not dropped; only reading the file proves a host or a DAW
    // could open what came out of it.
    MidiFileData file;
    const bool read = readMidiFile(kMidiPath, &file);
    check(read, "…and the file reads back as a Standard MIDI File");

    size_t notes = 0;
    double lastSecond = 0.0;
    if (read && !file.tracks.empty()) {
      for (const auto& e : file.tracks[0].events) {
        if (e.message.isNoteOn() || e.message.isNoteOff()) ++notes;
        lastSecond = file.tickToSeconds(e.tick);
      }
    }
    std::printf("  ---- %zu events sent, %zu notes in the file, last at %.3fs ----\n", sent,
                notes, lastSecond);
    check(notes > 0, "…containing the notes the plugin played");
    // Inside the render it was made from. A tempo or tick conversion that is
    // off by a factor still produces a valid file, and this is what catches
    // it: two seconds of arpeggio cannot end at twenty.
    check(lastSecond > 0.0 && lastSecond <= 2.05,
          "…at times that fall inside the two seconds rendered");
    std::remove(kMidiPath);
  }

  // ── Material in, material out ─────────────────────────────────
  //
  // An effect that can only be fed its own test tone is a demo of a test
  // tone. The file path is checked here rather than trusted, and with a file
  // this test writes and reads itself, so there is no fixture to go missing
  // and nothing to believe on faith.
  if (!kDesc.isInstrument) {
    const char* kInputPath = "sonore-verify-input.wav";
    constexpr uint32_t kFrames = 24000; // half a second at 48k
    {
      std::vector<float> stereo((size_t) kFrames * 2);
      for (uint32_t i = 0; i < kFrames; ++i) {
        const float v =
            0.3f * (float) std::sin(2.0 * 3.14159265358979 * 220.0 * (double) i / 48000.0);
        stereo[(size_t) i * 2] = v;
        stereo[(size_t) i * 2 + 1] = v;
      }
      check(sonore::writeWav(kInputPath, stereo.data(), kFrames, 2, 48000),
            "a file can be written to feed the effect");
    }

    App fileApp;
    const bool loaded = fileApp.fileSource.load(kInputPath, 48000.0);
    check(loaded, "…and read back as material");

    // A rate that does not match is REFUSED, not resampled behind the user's
    // back. Playing a 44.1k file at 48k comes out sharp and short, and doing
    // that silently to somebody's material is worse than declining it.
    App wrongRate;
    check(!wrongRate.fileSource.load(kInputPath, 44100.0),
          "…and a rate that does not match is refused rather than resampled quietly");
    check(!wrongRate.fileSource.error().empty(), "…with the two rates named");
    check(!fileApp.fileSource.load("no-such-file-anywhere.wav", 48000.0),
          "a file that is not there is refused");

    if (loaded) {
      const std::vector<float> rendered = renderOffline(fileApp, 0.0);
      const size_t frames = rendered.size() / 2;

      // Long enough to hold the material AND the plugin's own tail. A render
      // that stops at the last input sample cuts a reverb off mid-decay.
      check(frames >= kFrames, "the render is at least as long as the material");

      double energy = 0.0;
      bool finite = true;
      for (float v : rendered) {
        if (!std::isfinite(v)) finite = false;
        energy += (double) v * v;
      }
      check(finite, "…and finite");
      check(energy > 0.0, "…and not silence");

      // The point of the whole thing: the FILE was processed, not the test
      // tone. Rendering the same plugin without a file must not produce the
      // same audio, or --input changed nothing and this check passed for no
      // reason.
      App toneApp;
      const std::vector<float> tone = renderOffline(toneApp, (double) kFrames / 48000.0);
      const size_t compare = frames < tone.size() / 2 ? frames : tone.size() / 2;
      double difference = 0.0;
      for (size_t i = 0; i < compare * 2; ++i)
        difference += std::fabs((double) rendered[i] - (double) tone[i]);
      const double perSample = compare ? difference / (double) (compare * 2) : 0.0;
      std::printf("  ---- %zu frames in, %zu out; mean difference from the test tone %.4f ----\n",
                  (size_t) kFrames, frames, perSample);
      check(perSample > 0.01, "…and it is the FILE that was processed, not the test tone");

      // Silence in, silence out. A source that was ignored would give the
      // test tone here and this is what would catch it.
      {
        std::vector<float> quiet((size_t) kFrames * 2, 0.0f);
        const char* kQuietPath = "sonore-verify-silence.wav";
        if (sonore::writeWav(kQuietPath, quiet.data(), kFrames, 2, 48000)) {
          App silentApp;
          if (silentApp.fileSource.load(kQuietPath, 48000.0)) {
            const std::vector<float> out = renderOffline(silentApp, 0.0);
            float loudest = 0.0f;
            for (float v : out) {
              const float a = v < 0.0f ? -v : v;
              if (a > loudest) loudest = a;
            }
            std::printf("  ---- silence in: loudest sample out %.6g ----\n", (double) loudest);
            check(loudest < 1e-4f, "silence in gives silence out, not the test tone");
          }
          std::remove(kQuietPath);
        }
      }
    }
    std::remove(kInputPath);
  }

  if (kDesc.isInstrument && !kDesc.producesMidi) {
    // The last 5% is deep in the release; it must be quiet again.
    double tail = 0.0;
    const size_t start = audio.size() * 95 / 100;
    for (size_t i = start; i < audio.size(); ++i) {
      const float a = audio[i] < 0.0f ? -audio[i] : audio[i];
      if ((double) a > tail) tail = a;
    }
    check(tail < 0.05, "the instrument decays after note-off");
  }

  std::printf("%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
}

// ── Live audio ───────────────────────────────────────────────────────────────

#if defined(_WIN32)
using PlatformAudio = WasapiOutput;
#elif defined(__APPLE__)
using PlatformAudio = CoreAudioOutput;
#elif defined(__linux__)
// Both backends behind one type. Whether JACK is worth using is a fact about
// the machine the plugin RUNS on, not the one it was built on, so the choice is
// a device in the list rather than a build flag.
using PlatformAudio = LinuxOutput;
#endif

/** Open the device, prepare the DSP at ITS rate, then start the stream: in
 *  that order, or the first callback races an unprepared processor. */
// ── Remembering the device ───────────────────────────────────────────────────
//
// One line in a file beside the user's other settings. A standalone that
// forgets which interface to use every launch is a standalone nobody uses
// twice, and a full config format would be more machinery than one integer
// deserves.

/**
 * Which output device the user last chose.
 *
 * This had its own path logic and its own one-number file format, in this
 * file, next to a comment about where %APPDATA% lives. It is now the SDK's
 * per-user settings store -- the same one a plugin uses -- so there is one
 * place that knows where user data goes and one format it is written in.
 *
 * The old `.device` files are simply ignored. Losing a remembered device is a
 * user picking their soundcard once; carrying a bespoke reader for a
 * single-integer format forever is worse.
 */
inline int loadRememberedDevice() {
  UserSettings settings(kDesc.id);
  if (!settings.load()) return -1;
  const int64_t v = settings.values().getInt("audioDevice", -1);
  return (v >= 0 && v < 256) ? (int) v : -1;
}

inline void rememberDevice(int index) {
  if (index < 0) return;
  UserSettings settings(kDesc.id);
  // Read first: this file belongs to the whole plugin, and writing only the
  // key we care about would delete every other preference in it.
  settings.load();
  settings.values().setInt("audioDevice", index);
  // Choosing a system device is also choosing NOT to use ASIO. Leaving the
  // driver behind would mean the next launch quietly ignored the choice that
  // was just made.
  settings.values().setString("asioDriver", "");
  settings.save();
}

/**
 * The ASIO driver last used, by NAME.
 *
 * A name and not an index, for the same reason --asio takes one: the registry
 * order is not stable, an installer can renumber it, and a remembered number
 * would then point at somebody else's hardware. A name that no longer exists
 * simply fails to open and says so, which is a recoverable Tuesday.
 */
inline std::string loadRememberedAsio() {
  UserSettings settings(kDesc.id);
  if (!settings.load()) return std::string();
  return settings.values().getString("asioDriver", "");
}

inline void rememberAsio(const std::string& driver) {
  if (driver.empty()) return;
  UserSettings settings(kDesc.id);
  settings.load();
  settings.values().setString("asioDriver", driver);
  settings.save();
}

#if defined(_WIN32)
/**
 * Run on an ASIO driver instead of the system's own output.
 *
 * The reason a Windows musician cares: WASAPI in shared mode adds a buffer
 * nobody asked for, and an interface driver talking to its own hardware does
 * not. This is what every serious Windows DAW offers first.
 *
 * The driver's PREFERRED buffer size is used, whatever it is -- one machine
 * here reports 512 and another 528, and a host that rounds that to something
 * tidier is the host that gets blamed for the crackle.
 */
inline bool startAsio(App& app, asio::AsioOutput& out, const char* driverName) {
  // Input ONLY for an effect, and the reason is measured rather than tidy.
  // Realtek's driver, asked for input channels it is then not fed from, drops
  // its callback rate by a factor of nine -- 27 blocks in a quarter second
  // becomes 3, and that starvation lands on the OUTPUT, which had been fine.
  // A synth that never reads input must not pay that price to have it open.
  const bool wantInput = !kDesc.isInstrument;
  const bool ok = out.open(driverName, [&app](const float* in, float* interleaved,
                                               uint32_t frames, uint32_t channels) {
    constexpr uint32_t kBlock = 256;
    static thread_local float left[kBlock], right[kBlock];
    uint32_t done = 0;
    while (done < frames) {
      const uint32_t n = (frames - done) < kBlock ? (frames - done) : kBlock;
      // The same slice of the input the output slice is about to replace.
      processBlock(app, left, right, n, in ? in + (size_t) done * channels : nullptr,
                   in ? channels : 0);
      for (uint32_t i = 0; i < n; ++i) {
        float* frame = interleaved + (size_t) (done + i) * channels;
        frame[0] = left[i];
        if (channels > 1) frame[1] = right[i];
      }
      done += n;
    }
  }, wantInput);
  if (!ok) {
    std::printf("[sonore] ASIO: %s\n", out.error().c_str());
    return false;
  }

  // AFTER the device is open and BEFORE a single callback: the driver decides
  // the rate and the block size, and a DSP prepared at the wrong rate is a DSP
  // whose filters are all in the wrong place -- while a DSP not yet prepared
  // at all is a crash.
  prepare(app, out.sampleRate(), out.bufferFrames());
  // Only now, and only on success. Remembering a driver that refused to open
  // would make the failure permanent across every future launch.
  rememberAsio(out.driverName());

  // Only now does the driver's clock turn. Everything above this line -- the
  // DSP prepare, the settings write -- happens with the audio thread idle.
  if (!out.run()) {
    std::printf("[sonore] ASIO: %s\n", out.error().c_str());
    return false;
  }
  std::printf("  Audio output: %s (ASIO, %.0f Hz, %u frames)\n", out.driverName().c_str(),
              out.sampleRate(), out.bufferFrames());
  // Named either way. An effect that opened a driver with no usable inputs is
  // an effect processing silence, and the user is owed the reason rather than
  // left to wonder why nothing comes out.
  if (wantInput)
    std::printf("  Audio input:  %s\n",
                out.hasInput() ? "the same driver, on the same clock"
                               : "none -- this driver offers fewer than two input channels");
  return true;
}
#endif

/**
 * The MIDI keyboard. Once per run, whichever backend is playing.
 *
 * This used to live inside startAudio, and being there cost two things:
 *
 *   - The ASIO path never called startAudio, so a synth on an audio interface
 *     had no MIDI input AT ALL. The one setup people buy an interface for.
 *   - Every device switch opened the keyboard again without closing it, which
 *     overwrote the handle and left the old one still delivering. After three
 *     switches every note arrived four times.
 *
 * Neither was a hard bug to write. Both come from the same mistake: a keyboard
 * is a property of the PROGRAM, and it was made a property of the soundcard.
 *
 * Live MIDI is a bonus, never a requirement -- an effect does not need it and
 * a machine without a keyboard must still make sound. Either way it says
 * which, because "silent when played" with no explanation is the worst
 * possible outcome for someone holding a controller.
 */
inline int loadRememberedMidi() {
  UserSettings settings(kDesc.id);
  if (!settings.load()) return -1;
  const int64_t v = settings.values().getInt("midiDevice", -1);
  return (v >= 0 && v < 256) ? (int) v : -1;
}

inline void rememberMidi(int index) {
  if (index < 0) return;
  UserSettings settings(kDesc.id);
  settings.load();
  settings.values().setInt("midiDevice", index);
  settings.save();
}

inline void startMidi(App& app, int deviceIndex = -1) {
  ++app.midiOpens;
  const int chosen = deviceIndex >= 0 ? deviceIndex : loadRememberedMidi();
  app.midiOpen = app.midiDevice.open(&app.midiQueue, chosen);
  if (app.midiOpen) {
    app.midiIndex = chosen >= 0 ? chosen : 0;
    if (deviceIndex >= 0) rememberMidi(deviceIndex);
    const std::string& name = app.midiDevice.deviceName();
    std::printf("  MIDI input: %s\n", name.empty() ? "connected" : name.c_str());
  } else {
    app.midiIndex = -1;
    std::printf("  MIDI input: %s\n", app.midiDevice.error().c_str());
  }
}

inline bool startAudio(App& app, PlatformAudio& audio, int deviceIndex = -1) {
  // A device the user picked, otherwise the one they picked last time,
  // otherwise the system default.
  const int chosen = deviceIndex >= 0 ? deviceIndex : loadRememberedDevice();
  if (chosen >= 0) audio.setDeviceIndex(chosen);
  const bool ok = audio.open([&app](float* interleaved, uint32_t frames, uint32_t channels) {
    constexpr uint32_t kBlock = 256;
    static thread_local float left[kBlock], right[kBlock];
    uint32_t done = 0;
    while (done < frames) {
      const uint32_t n = (frames - done) < kBlock ? (frames - done) : kBlock;
      processBlock(app, left, right, n);
      for (uint32_t i = 0; i < n; ++i) {
        float* frame = interleaved + (size_t) (done + i) * channels;
        frame[0] = left[i];
        if (channels > 1) frame[1] = right[i];
        for (uint32_t c = 2; c < channels; ++c) frame[c] = 0.0f;
      }
      done += n;
    }
  });
  if (!ok) return false;
  prepare(app, audio.sampleRate(), 256);

  std::printf("  Audio output: %s\n",
              audio.deviceName().empty() ? "(unnamed device)" : audio.deviceName().c_str());
  if (deviceIndex >= 0) rememberDevice(deviceIndex);

  // MIDI is deliberately NOT opened here. See startMidi() -- a keyboard has
  // nothing to do with which soundcard is playing, and tying the two together
  // is what cost the ASIO path its MIDI input entirely.
  return audio.run();
}

} // namespace standalone
} // namespace sonore

// ── The application entry point ──────────────────────────────────────────────

namespace sonore {
namespace standalone {

struct Options {
  bool verifyMode = false;
  /** --test-signal: play the built-in arpeggio through an effect. */
  bool testSignal = false;
  bool listDevices = false;
  bool listMidiIns = false;
  int midiDeviceIndex = -1;
  /** Cycle every audio device live, reporting each. The picker's mechanism
   *  without the picker: a page clicking through the list is exactly this, and
   *  a switch that works from here works from there. */
  bool switchTest = false;
  /** Write the page this program would show, and the bridge script that runs
   *  in it, to a file. The interface is HTML; being able to READ it without a
   *  window is worth one flag. */
  const char* dumpUi = nullptr;
  int deviceIndex = -1; // <0 = whatever was remembered, or the system default
  const char* renderPath = nullptr;
  const char* shotPath = nullptr;
  bool playOnly = false;
  double seconds = 2.0;
  /** Where the plugin's own MIDI goes. A port, a file, or nowhere. */
  /** Material to feed an effect, instead of the built-in test tone. */
  const char* inputPath = nullptr;
  /** A MIDI file to play into an instrument, instead of the single note the
   *  offline render invents. */
  const char* midiInPath = nullptr;
  /** A live capture device, which outranks both. <0 means none. */
  bool listInputs = false;
  int inputDeviceIndex = -1;
  /** Record the live input, through the plugin, to this file. */
  const char* capturePath = nullptr;
  bool listMidiOuts = false;
  int midiOutIndex = -1;
  const char* midiOutPath = nullptr;
  /** What track to CLAIM to be on. There is no track here, so none of this is
   *  true unless a developer says it is -- which is the point: a plugin that
   *  reacts to its track is otherwise only testable inside a DAW. */
  /** An ASIO driver BY NAME. Not by index: the registry order is not stable,
   *  an installer can renumber it, and a number remembered in a settings file
   *  would then point at somebody else's hardware. */
  const char* asioDriver = nullptr;
  bool listAsio = false;
  const char* trackName = nullptr;
  const char* trackColour = nullptr;
  bool trackIsReturn = false;
  bool trackIsBus = false;
  bool trackIsMaster = false;
};

inline Options parseOptions(int argc, char** argv) {
  Options opt;
  for (int i = 1; i < argc; ++i) {
    const char* arg = argv[i];
    auto number = [&](double fallback) {
      if (i + 1 < argc) {
        char* end = nullptr;
        const double v = std::strtod(argv[i + 1], &end);
        if (end != argv[i + 1]) {
          ++i;
          return v;
        }
      }
      return fallback;
    };
    if (std::strcmp(arg, "--switch-devices") == 0) opt.switchTest = true;
    if (std::strcmp(arg, "--dump-ui") == 0 && i + 1 < argc) opt.dumpUi = argv[++i];
    if (std::strcmp(arg, "--devices") == 0) opt.listDevices = true;
    if (std::strcmp(arg, "--midi-inputs") == 0) opt.listMidiIns = true;
    if (std::strcmp(arg, "--midi-input") == 0 && i + 1 < argc)
      opt.midiDeviceIndex = std::atoi(argv[++i]);
    else if (std::strcmp(arg, "--device") == 0) opt.deviceIndex = (int) number(-1.0);
    else if (std::strcmp(arg, "--verify") == 0) opt.verifyMode = true;
    else if (std::strcmp(arg, "--test-signal") == 0) opt.testSignal = true;
    else if (std::strcmp(arg, "--render") == 0 && i + 1 < argc) opt.renderPath = argv[++i];
    else if (std::strcmp(arg, "--shot") == 0 && i + 1 < argc) opt.shotPath = argv[++i];
    else if (std::strcmp(arg, "--play") == 0) { opt.playOnly = true; opt.seconds = number(2.0); }
    else if (std::strcmp(arg, "--seconds") == 0) opt.seconds = number(opt.seconds);
    else if (std::strcmp(arg, "--input") == 0 && i + 1 < argc) opt.inputPath = argv[++i];
    else if (std::strcmp(arg, "--midi-in-file") == 0 && i + 1 < argc)
      opt.midiInPath = argv[++i];
    else if (std::strcmp(arg, "--inputs") == 0) opt.listInputs = true;
    else if (std::strcmp(arg, "--capture") == 0 && i + 1 < argc) opt.capturePath = argv[++i];
    else if (std::strcmp(arg, "--input-device") == 0) opt.inputDeviceIndex = (int) number(-1.0);
    else if (std::strcmp(arg, "--midi-outs") == 0) opt.listMidiOuts = true;
    else if (std::strcmp(arg, "--midi-out") == 0) opt.midiOutIndex = (int) number(-1.0);
    else if (std::strcmp(arg, "--midi-out-file") == 0 && i + 1 < argc)
      opt.midiOutPath = argv[++i];
    else if (std::strcmp(arg, "--asios") == 0) opt.listAsio = true;
    else if (std::strcmp(arg, "--asio") == 0 && i + 1 < argc) opt.asioDriver = argv[++i];
    else if (std::strcmp(arg, "--track-name") == 0 && i + 1 < argc) opt.trackName = argv[++i];
    else if (std::strcmp(arg, "--track-colour") == 0 && i + 1 < argc) opt.trackColour = argv[++i];
    else if (std::strcmp(arg, "--track-color") == 0 && i + 1 < argc) opt.trackColour = argv[++i];
    else if (std::strcmp(arg, "--track-return") == 0) opt.trackIsReturn = true;
    else if (std::strcmp(arg, "--track-bus") == 0) opt.trackIsBus = true;
    else if (std::strcmp(arg, "--track-master") == 0) opt.trackIsMaster = true;
  }
  return opt;
}

/**
 * Pretend to be a track, if asked.
 *
 * There is no track here -- the plugin IS the signal chain -- so by default
 * nothing is sent and a DSP sees exactly what an LV2 build sees: silence on
 * the subject. Claiming to be the master would be worse than saying nothing,
 * because "on the master" changes what a sensible plugin defaults to.
 *
 * But a plugin whose UI tints itself to the track colour, or whose reverb
 * starts wet on a return, is otherwise only testable inside a DAW. So the
 * flags exist to say it deliberately: this is the one host in the set whose
 * answers a developer can dictate.
 */
inline void tellTrackInfo(App& app, const Options& opt) {
  TrackInfo info;
  if (opt.trackName) {
    info.hasName = true;
    info.name = opt.trackName;
  }
  if (opt.trackColour) {
    // #rgb and #rrggbb, with or without the hash, because a colour typed on a
    // command line is typed the way CSS spells it.
    const char* text = opt.trackColour;
    if (*text == '#') ++text;
    const size_t n = std::strlen(text);
    auto nibble = [](char c) -> int {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      if (c >= 'A' && c <= 'F') return c - 'A' + 10;
      return -1;
    };
    int v[6] = {0, 0, 0, 0, 0, 0};
    bool ok = (n == 3 || n == 6);
    for (size_t i = 0; ok && i < n; ++i) {
      const int d = nibble(text[i]);
      if (d < 0) ok = false;
      else v[i] = d;
    }
    if (ok && n == 3) {
      // #abc means #aabbcc, which is the whole point of the short form.
      v[5] = v[4] = v[2];
      v[3] = v[2] = v[1];
      v[1] = v[0];
    }
    if (ok) {
      info.hasColour = true;
      info.red = (unsigned char) (v[0] * 16 + v[1]);
      info.green = (unsigned char) (v[2] * 16 + v[3]);
      info.blue = (unsigned char) (v[4] * 16 + v[5]);
    } else {
      std::printf("ignoring --track-colour '%s': expected #rgb or #rrggbb\n", opt.trackColour);
    }
  }
  info.isReturnTrack = opt.trackIsReturn;
  info.isBus = opt.trackIsBus;
  info.isMaster = opt.trackIsMaster;
  if (!info.hasName && !info.hasColour && !info.isReturnTrack && !info.isBus && !info.isMaster)
    return; // nothing was asked for, so nothing is claimed
  app.instance.trackInfo = info;
  clapwrap::sendTrackInfo(app.instance.dsp, app.instance.trackInfo);
}


/** The live modes share this: device up, DSP prepared at its rate, stream on.
 *  Returns false with the reason already printed. */
inline bool startLiveAudio(App& app, PlatformAudio& audio, int deviceIndex = -1) {
  if (!startAudio(app, audio, deviceIndex)) {
    std::fprintf(stderr, "[sonore] audio unavailable: %s\n", audio.error().c_str());
    return false;
  }
  std::printf("[sonore] audio: %.0f Hz, %u channel(s)\n", audio.sampleRate(), audio.channels());
  return true;
}

/** Open a capture device, at the rate the OUTPUT already settled on.
 *
 *  Order matters and is not arbitrary: shared mode lets the output device
 *  pick the rate, the DSP is prepared at that rate, and only then can capture
 *  be asked for the same one. Opening capture first would mean either
 *  resampling the output or preparing the DSP twice.
 *
 *  A failure here is NOT fatal. Someone who asked for a microphone and does
 *  not have one still wants their plugin, so this reports the reason and
 *  leaves the test tone playing. */

// ── The device hub: what a page can see, and what it may change ─────────────
//
// A device selector, which a framework would also DRAW. This does not
// draw anything -- the interface is a web page and drawing is its job -- so
// what is here is the half a page cannot do for itself: enumerate the
// backends, switch between them without stopping the program, and remember
// the answer.
//
// A PLUGIN never gets any of this. The host owns the device there, and a
// plugin offering a device picker offers a control that cannot work. This
// lives in standalone.h for that reason and not by accident.

/** Everything a page needs to draw a picker, as the JS object literal that
 *  window.sonore.audioDevices becomes. */
inline std::string audioDevicesJson(const std::string& currentBackend, int currentIndex,
                                    const std::string& currentName, double rate,
                                    uint32_t bufferFrames, int currentMidi) {
  std::string json = "{system:[";
  const std::vector<std::string> system = PlatformAudio::listDevices();
  for (size_t i = 0; i < system.size(); ++i) {
    if (i) json += ",";
    json += "'" + escapeForJs(system[i].c_str()) + "'";
  }
  json += "],asio:[";
#if defined(_WIN32)
  // Names only, and deliberately not opened to report their rates. Opening a
  // driver runs its code in this process, and enumerating a list must not be
  // able to take the program down.
  const auto drivers = sonore::asio::listDrivers();
  for (size_t i = 0; i < drivers.size(); ++i) {
    if (i) json += ",";
    json += "'" + escapeForJs(drivers[i].name.c_str()) + "'";
  }
#endif
  json += "],midi:[";
  const std::vector<std::string> midiIns = sonore::midiin::Device::listDevices();
  for (size_t i = 0; i < midiIns.size(); ++i) {
    if (i) json += ",";
    json += "'" + escapeForJs(midiIns[i].c_str()) + "'";
  }
  json += "],currentMidi:" + std::to_string(currentMidi);
  json += ",current:{backend:'" + escapeForJs(currentBackend.c_str()) + "',index:" +
          std::to_string(currentIndex) + ",name:'" + escapeForJs(currentName.c_str()) + "'},";
  json += "sampleRate:" + std::to_string((long) (rate + 0.5)) + ",";
  json += "bufferFrames:" + std::to_string((unsigned long) bufferFrames) + "}";
  return json;
}

/**
 * The audio device, and the ability to change it while the program runs.
 *
 * Holds REFERENCES to the two outputs the run path already owns rather than
 * owning them itself: an interface that is open has a window sitting on it,
 * and a hub that owned the device would have to outlive or be outlived by
 * that window in some order nobody would remember.
 */
struct DeviceHub {
  App& app;
  PlatformAudio& system;
#if defined(_WIN32)
  sonore::asio::AsioOutput& asioOut;
#endif
  /** "system" or "asio" -- which of the two is currently making sound. */
  std::string backend = "system";
  int index = -1;
  std::string name;
  bool open = false;
  /** The last list handed to the page, so a poll can tell new from same. */
  std::string lastPushed_;

  double sampleRate() const {
#if defined(_WIN32)
    if (backend == "asio") return asioOut.sampleRate();
#endif
    return system.sampleRate();
  }

  uint32_t bufferFrames() const {
#if defined(_WIN32)
    if (backend == "asio") return asioOut.bufferFrames();
#endif
    // WASAPI and ALSA are driven in 256-frame blocks by startAudio; the device
    // period is the driver's business and not something we chose.
    return 256;
  }

  /**
   * Re-enumerate, and say whether anything changed.
   *
   * ── Why polling, and not a notification ──
   *
   * Every platform announces device changes differently -- CoreMIDI has a
   * notify proc, ALSA has an announce port, and winmm has NOTHING at all;
   * on Windows the only way to learn that a keyboard was plugged in is to
   * count the devices again.
   *
   * Three notification paths plus a polling fallback is four things to keep
   * in step, and this project has now found the same rule diverging across
   * platforms often enough to stop writing four of anything. One poll,
   * compared against what was last sent, is a rule that cannot drift.
   *
   * The cost is real and bounded: enumerating audio devices is a COM call on
   * Windows, so this runs on a timer measured in seconds rather than on every
   * UI frame. A keyboard noticed two seconds after it was plugged in is a
   * keyboard noticed.
   */
  bool changedSinceLastPush() {
    const std::string now = json();
    if (now == lastPushed_) return false;
    lastPushed_ = now;
    return true;
  }

  std::string json() const {
    return audioDevicesJson(backend, index, name, sampleRate(), bufferFrames(), app.midiIndex);
  }

  /** Stop whatever is playing. Both are asked, because only one is open and
   *  stopping the other is free. */
  void stopAll() {
#if defined(_WIN32)
    asioOut.stop();
#endif
    system.stop();
    open = false;
  }

  /**
   * Switch to another device, live.
   *
   * The order is the whole of the correctness here: stop first, so no callback
   * is running while anything below changes; then open, which settles the new
   * rate; then prepare the DSP at that rate; then start the clock. A DSP
   * prepared at the old rate has every filter in the wrong place, and a DSP
   * not prepared at all is a crash.
   *
   * A failed switch leaves the program SILENT rather than half-connected, and
   * says so. Trying to reopen what was just closed is a second thing that can
   * fail while the first failure is still being reported, and the user can
   * pick again from a picker that is still on screen.
   */
  bool select(const std::string& wantBackend, int wantIndex, const std::string& wantName) {
    // MIDI is not an OUTPUT: switching it must not stop the audio device, and
    // treating it like one would silence the plugin every time somebody
    // changed keyboard.
    if (wantBackend == "midi") {
      startMidi(app, wantIndex);
      return app.midiOpen;
    }
    stopAll();

#if defined(_WIN32)
    if (wantBackend == "asio") {
      // A capture device opened for the OLD output has no business staying
      // open: ASIO brings its own input on its own clock, processBlock prefers
      // it, and what is left is a second soundcard being read by nobody.
      if (app.inputOpen) {
        app.audioInput.stop();
        app.inputOpen = false;
      }
      if (!startAsio(app, asioOut, wantName.c_str())) return false;
      backend = "asio";
      index = -1;
      name = asioOut.driverName();
      open = true;
      return true;
    }
#else
    (void) wantName;
#endif
    if (!startLiveAudio(app, system, wantIndex)) return false;
    backend = "system";
    index = wantIndex;
    name = system.deviceName();
    open = true;
    return true;
  }
};

/** Push the current device list into the page. Called once the interface is
 *  up, and again after every switch -- a picker showing the device that was
 *  selected two switches ago is worse than one showing nothing. */
inline void pushDevices(Instance* inst, const DeviceHub& hub) {
#if defined(SONORE_HAS_WEBVIEW_BACKEND)
  inst->webview.eval("if(window.sonore&&window.sonore.__devices)window.sonore.__devices(" +
                     hub.json() + ");");
#else
  (void) inst;
  (void) hub;
#endif
}

/**
 * Re-enumerate on a timer and tell the page only when something moved.
 *
 * Called from the UI tick, which runs at frame rate -- so the interval is
 * enforced here rather than by trusting every caller to remember it.
 */
inline void pollDevices(Instance* inst, DeviceHub& hub, uint64_t& nextPollTick,
                        uint64_t tick, uint64_t everyTicks) {
  if (tick < nextPollTick) return;
  nextPollTick = tick + everyTicks;
  // Only when it actually differs. Pushing an identical list every two
  // seconds would rebuild the page's <select> under whatever the user was
  // in the middle of choosing.
  if (hub.changedSinceLastPush()) pushDevices(inst, hub);
}

/**
 * The standalone's bridge: device messages here, everything else onward.
 *
 * Wrapping rather than extending clapwrap::guiOnMessage on purpose. That
 * function is what a PLUGIN runs, and teaching it about audio devices would
 * put a code path in every shipped plugin that only a standalone can ever
 * reach.
 */
inline void installDeviceBridge(Instance* inst, DeviceHub& hub) {
#if defined(SONORE_HAS_WEBVIEW_BACKEND)
  inst->webview.onMessage = [inst, &hub](const BridgeMessage& m) {
    if (m.kind == BridgeMessage::Kind::ListDevices) {
      pushDevices(inst, hub);
      return;
    }
    if (m.kind != BridgeMessage::Kind::SelectDevice) {
      clapwrap::guiOnMessage(inst, m);
      return;
    }
    const bool ok = hub.select(m.backend, m.index, m.deviceName);
    if (!ok)
      std::fprintf(stderr, "[sonore] could not switch to that device; audio is stopped\n");
    // Either way. A page that asked for a device it did not get must be told,
    // and the surest way is to hand it the truth about what is open now.
    pushDevices(inst, hub);
  };
#else
  (void) inst;
  (void) hub;
#endif
}

inline bool startLiveInput(App& app, int deviceIndex, double rate) {
  if (!app.audioInput.open(deviceIndex, rate)) {
    std::fprintf(stderr, "[sonore] audio input unavailable: %s\n",
                 app.audioInput.error().c_str());
    return false;
  }
  if (!app.audioInput.run()) {
    std::fprintf(stderr, "[sonore] audio input would not start: %s\n",
                 app.audioInput.error().c_str());
    return false;
  }
  app.inputOpen = true;
  std::printf("[sonore] input: %s at %.0f Hz\n", app.audioInput.deviceName().c_str(),
              app.audioInput.sampleRate());
  return true;
}

} // namespace standalone
} // namespace sonore

// ── Windows ──────────────────────────────────────────────────────────────────
#if defined(_WIN32)

namespace sonore {
namespace standalone {

inline LRESULT CALLBACK appWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_DESTROY) {
    PostQuitMessage(0);
    return 0;
  }
  if (msg == WM_GETMINMAXINFO) {
    // Windows will happily let a WS_OVERLAPPEDWINDOW be dragged down to its
    // title bar, and this window had nothing stopping it. Every hosted format
    // asks the plugin for a minimum before resizing; the standalone was the
    // one place where the user could simply make the interface disappear.
    //
    // WM_GETMINMAXINFO arrives BEFORE the first WM_SIZE, so this bounds the
    // window from its creation rather than from the first drag.
    const EditorConstraints& limits = kDesc.editorLimits;
    uint32_t minW = limits.minWidth, minH = limits.minHeight;
    uint32_t maxW = limits.maxWidth, maxH = limits.maxHeight;
    // Through the same function, so the floor and ceiling a bad descriptor
    // gets corrected against are the ones every other format applies.
    applyEditorConstraints(limits, SONORE_UI_WIDTH, SONORE_UI_HEIGHT, &minW, &minH);
    applyEditorConstraints(limits, SONORE_UI_WIDTH, SONORE_UI_HEIGHT, &maxW, &maxH);

    // In WINDOW pixels, not client pixels: the constraint is on the interface,
    // and the frame and title bar are around it. AdjustWindowRect converts.
    const DWORD style = (DWORD) GetWindowLongPtrW(hwnd, GWL_STYLE);
    RECT frame{0, 0, (LONG) minW, (LONG) minH};
    AdjustWindowRect(&frame, style, FALSE);
    auto* info = (MINMAXINFO*) lp;
    info->ptMinTrackSize.x = frame.right - frame.left;
    info->ptMinTrackSize.y = frame.bottom - frame.top;

    RECT big{0, 0, (LONG) maxW, (LONG) maxH};
    AdjustWindowRect(&big, style, FALSE);
    info->ptMaxTrackSize.x = big.right - big.left;
    info->ptMaxTrackSize.y = big.bottom - big.top;
    return 0;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

/** Save the window as a BMP: how a machine (or a human, later) looks at the
 *  standalone without running it by hand.
 *
 *  The WINDOW rect, not the client rect, and that is a correction: PrintWindow
 *  draws the whole window, frame and title bar included, starting at the
 *  bitmap's origin. Sized to the CLIENT rect it therefore painted the title bar
 *  into the top of the picture and ran off the bottom by exactly the frame's
 *  height, so every screenshot showed a plugin whose lower edge was missing
 *  and a window that looked too short. Two rounds of window-size work were
 *  read off those pictures before the numbers said the layout had been right
 *  all along. A tool that photographs the thing you are debugging has to be
 *  the one thing you can trust. */
inline bool captureWindow(HWND hwnd, const char* path) {
  RECT rc{};
  if (!GetWindowRect(hwnd, &rc)) return false;
  const int w = rc.right - rc.left, h = rc.bottom - rc.top;
  if (w <= 0 || h <= 0) return false;
  HDC screen = GetDC(nullptr);
  HDC mem = CreateCompatibleDC(screen);
  HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
  HGDIOBJ old = SelectObject(mem, bmp);
  if (!PrintWindow(hwnd, mem, 2 /* PW_RENDERFULLCONTENT */))
    BitBlt(mem, 0, 0, w, h, screen, 0, 0, SRCCOPY);
  BITMAPINFOHEADER bi{};
  bi.biSize = sizeof(bi);
  bi.biWidth = w;
  bi.biHeight = -h;
  bi.biPlanes = 1;
  bi.biBitCount = 32;
  bi.biCompression = BI_RGB;
  std::vector<unsigned char> pixels((size_t) w * h * 4);
  GetDIBits(mem, bmp, 0, (UINT) h, pixels.data(), (BITMAPINFO*) &bi, DIB_RGB_COLORS);
  BITMAPFILEHEADER fh{};
  fh.bfType = 0x4D42;
  fh.bfOffBits = sizeof(fh) + sizeof(bi);
  fh.bfSize = fh.bfOffBits + (DWORD) pixels.size();
  std::FILE* f = std::fopen(path, "wb");
  bool ok = false;
  if (f) {
    std::fwrite(&fh, sizeof(fh), 1, f);
    std::fwrite(&bi, sizeof(bi), 1, f);
    std::fwrite(pixels.data(), 1, pixels.size(), f);
    std::fclose(f);
    ok = true;
  }
  SelectObject(mem, old);
  DeleteObject(bmp);
  DeleteDC(mem);
  ReleaseDC(nullptr, screen);
  return ok;
}

inline int runInteractive(App& app, const Options& opt) {
  PlatformAudio audio;
  // ASIO instead of the system output, when a driver was named -- or when one
  // was named LAST time and nothing has said otherwise since. Choosing a
  // system device with --device clears the remembered driver, so this only
  // fires for someone whose most recent choice really was an interface.
  //
  // Kept in the same scope as the window loop below so it lives exactly as
  // long as the interface does.
  asio::AsioOutput asioOut;
  const std::string rememberedAsio =
      (opt.asioDriver || opt.deviceIndex >= 0) ? std::string() : loadRememberedAsio();
  const char* asioName = opt.asioDriver ? opt.asioDriver
                                        : (rememberedAsio.empty() ? nullptr
                                                                  : rememberedAsio.c_str());
  bool useAsio = asioName != nullptr;
  bool audioOk = useAsio ? startAsio(app, asioOut, asioName)
                         : startLiveAudio(app, audio, opt.deviceIndex);
  // A remembered driver that no longer opens must not take the standalone down
  // with it. An interface gets unplugged, and the right answer is the built-in
  // output and a sentence saying why -- not a program that will not start.
  //
  // Only for the REMEMBERED one. Someone who typed --asio asked for that
  // driver specifically, and silently playing through something else would be
  // answering a different question.
  if (!audioOk && useAsio && !opt.asioDriver) {
    std::fprintf(stderr, "[sonore] the remembered ASIO driver \"%s\" did not open; "
                         "falling back to the system device\n", asioName);
    useAsio = false;
    audioOk = startLiveAudio(app, audio, opt.deviceIndex);
  }
  // Once, whichever backend just opened. A keyboard belongs to the program,
  // not to the soundcard.
  startMidi(app, opt.midiDeviceIndex);

  // After the output, never before: capture is asked for the rate the output
  // device chose, and that rate does not exist until the output is open.
  // Not when ASIO is driving: that path takes its input from the SAME driver,
  // and `audio` was never opened, so the rate this would be handed belongs to
  // a device that does not exist.
  if (audioOk && !useAsio && opt.inputDeviceIndex >= 0 && !kDesc.isInstrument)
    startLiveInput(app, opt.inputDeviceIndex, audio.sampleRate());
  if (opt.switchTest) {
    if (!audioOk) return 1;
    // The same hub the interface drives, exercised without an interface. Every
    // device in turn, each actually opened and actually played through -- a
    // switch that only reported success would be a test of the reporting.
    DeviceHub hub{app, audio, asioOut};
    hub.backend = useAsio ? "asio" : "system";
    hub.index = useAsio ? -1 : opt.deviceIndex;
    hub.name = useAsio ? asioOut.driverName() : audio.deviceName();
    hub.open = audioOk;

    int attempted = 0, succeeded = 0;
    const std::vector<std::string> system = PlatformAudio::listDevices();
    for (size_t i = 0; i < system.size(); ++i) {
      ++attempted;
      std::printf("[switch] system %zu: %s\n", i, system[i].c_str());
      if (hub.select("system", (int) i, std::string())) {
        ++succeeded;
        Sleep(250);
        std::printf("         playing at %.0f Hz\n", hub.sampleRate());
      } else {
        std::printf("         did not open\n");
      }
    }
    for (const auto& d : sonore::asio::listDrivers()) {
      ++attempted;
      std::printf("[switch] asio: %s\n", d.name.c_str());
      if (hub.select("asio", -1, d.name)) {
        ++succeeded;
        Sleep(250);
        std::printf("         playing at %.0f Hz, %u frames\n", hub.sampleRate(),
                    hub.bufferFrames());
      } else {
        // Expected for most of them. A driver whose hardware is absent
        // refusing to open is the correct answer, not a failure of the hub.
        std::printf("         did not open\n");
      }
    }
    hub.stopAll();
    std::printf("[switch] %d of %d device(s) opened and played\n", succeeded, attempted);

    // The keyboard, which has nothing to do with any of them. One open for the
    // whole run or this is the old bug back: every switch used to open it
    // again without closing, and the old handle kept delivering, so the same
    // note arrived once per soundcard the user had ever selected.
    std::printf("[switch] MIDI opened %d time(s)\n", app.midiOpens);

    // The hot-plug detector, on the hub that has just been driven through
    // every device on the machine.
    //
    // Two things have to hold and they pull against each other: it must
    // report a change when one happened, and it must NOT report one when
    // nothing did. A detector that always says yes rebuilds the page's
    // <select> twice a second under whatever the user is choosing from.
    const bool firstSaysChanged = hub.changedSinceLastPush();
    const bool secondSaysChanged = hub.changedSinceLastPush();
    hub.select("system", 0, std::string());
    const bool afterSwitch = hub.changedSinceLastPush();
    hub.stopAll();
    std::printf("[switch] hot-plug detector: first=%d unchanged=%d after-a-switch=%d\n",
                (int) firstSaysChanged, (int) secondSaysChanged, (int) afterSwitch);
    if (!firstSaysChanged || secondSaysChanged || !afterSwitch) {
      std::fprintf(stderr,
                   "[switch] the device-change detector is wrong: it must report the first "
                   "list, stay quiet when nothing moved, and notice a switch\n");
      return 1;
    }
    if (app.midiOpens != 1) {
      std::fprintf(stderr, "[switch] the keyboard was opened %d times; it belongs to the "
                           "program, not the device\n", app.midiOpens);
      return 1;
    }
    // The hub is what is under test, not the hardware. One device that never
    // opened is a Tuesday; a hub that opened NOTHING after starting fine is a
    // broken switch.
    return succeeded > 0 ? 0 : 1;
  }

  if (opt.playOnly) {
    if (!audioOk) return 1;
    Sleep((DWORD) (opt.seconds * 1000.0));
    // Whichever one was actually opened.
    if (useAsio) asioOut.stop();
    else audio.stop();
    std::printf("[sonore] played %.1fs cleanly\n", opt.seconds);
    return 0;
  }
  // Without audio the window still opens: seeing the UI beats a refusal.

  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = appWndProc;
  wc.hInstance = (HINSTANCE) GetModuleHandleW(nullptr);
  wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
  wc.lpszClassName = L"SonoreStandalone";
  RegisterClassExW(&wc);

  // A fixed-size interface gets a window that cannot be dragged, rather than
  // one that can be dragged with nothing following. Dropping the maximise box
  // with it: a maximise button on a window that will not change size is a
  // control that does nothing, which is worse than an absent one.
  const DWORD kStyle =
      (kDesc.editorLimits.resizableHorizontally || kDesc.editorLimits.resizableVertically)
          ? WS_OVERLAPPEDWINDOW
          : (WS_OVERLAPPEDWINDOW & ~(DWORD) (WS_THICKFRAME | WS_MAXIMIZEBOX));

  RECT want{0, 0, (LONG) app.instance.guiWidth, (LONG) app.instance.guiHeight};
  AdjustWindowRect(&want, kStyle, FALSE);
  const std::wstring title = win32::widen(kDesc.name);
  HWND hwnd = CreateWindowExW(0, L"SonoreStandalone", title.c_str(), kStyle,
                              CW_USEDEFAULT, CW_USEDEFAULT, want.right - want.left,
                              want.bottom - want.top, nullptr, nullptr, wc.hInstance, nullptr);
  if (!hwnd) {
    std::fprintf(stderr, "[sonore] no window\n");
    return 1;
  }
  ShowWindow(hwnd, SW_SHOW);

  Instance* inst = &app.instance;
  // The device picker. Constructed HERE and not earlier because it holds
  // references to the two outputs this scope owns, and a hub that outlived
  // them would be a picker pointing at closed devices.
  DeviceHub hub{app, audio, asioOut};
  hub.backend = useAsio ? "asio" : "system";
  hub.index = useAsio ? -1 : opt.deviceIndex;
  hub.name = useAsio ? asioOut.driverName() : audio.deviceName();
  hub.open = audioOk;
  installDeviceBridge(inst, hub);
  // Devices, re-checked about twice a second. A keyboard plugged in while the
  // window is open should appear in the picker without reopening it.
  uint64_t uiTick = 0, nextDevicePoll = 0;
  inst->webview.onTick = [inst, &hub, &uiTick, &nextDevicePoll]() {
    clapwrap::guiTick(inst);
    pollDevices(inst, hub, nextDevicePoll, ++uiTick, 30);
  };
  inst->webview.create(hwnd, inst->guiWidth, inst->guiHeight, clapwrap::uiHtml(),
                       bridgeScript(kDesc), kDesc.id);
  inst->webview.setVisible(true);

  const DWORD deadline =
      opt.shotPath ? GetTickCount() + (DWORD) (opt.seconds * 1000.0) : 0;
  MSG msg;
  int exitCode = 0;
  for (;;) {
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT) goto done;
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    if (opt.shotPath && GetTickCount() >= deadline) {
      exitCode = captureWindow(hwnd, opt.shotPath) ? 0 : 1;
      std::printf(exitCode == 0 ? "[sonore] wrote %s\n" : "[sonore] capture failed\n",
                  opt.shotPath);
      break;
    }
    Sleep(5);
  }
done:
  inst->webview.destroy();
  audio.stop();
  DestroyWindow(hwnd);
  return exitCode;
}

} // namespace standalone
} // namespace sonore

// ── Linux ────────────────────────────────────────────────────────────────────
#elif defined(__linux__)

namespace sonore {
namespace standalone {

inline int runInteractive(App& app, const Options& opt) {
  PlatformAudio audio;
  const bool audioOk = startLiveAudio(app, audio, opt.deviceIndex);
  // Once, whichever backend just opened. A keyboard belongs to the program,
  // not to the soundcard.
  startMidi(app, opt.midiDeviceIndex);

  // After the output, never before: capture is asked for the rate the output
  // device chose, and that rate does not exist until the output is open.
  if (audioOk && opt.inputDeviceIndex >= 0 && !kDesc.isInstrument)
    startLiveInput(app, opt.inputDeviceIndex, audio.sampleRate());
  if (opt.playOnly) {
    if (!audioOk) return 1;
    usleep((useconds_t) (opt.seconds * 1e6));
    audio.stop();
    std::printf("[sonore] played %.1fs cleanly\n", opt.seconds);
    return 0;
  }

  gtk::Api& a = gtk::api();
  if (!a.ok || !a.gtk_window_new) {
    std::fprintf(stderr, "[sonore] no GTK: %s\n", a.error.c_str());
    return 1;
  }
  if (!a.gtk_init_check(nullptr, nullptr)) {
    std::fprintf(stderr, "[sonore] no display\n");
    return 1;
  }

  gtk::GtkWidgetPtr window = a.gtk_window_new(0 /* GTK_WINDOW_TOPLEVEL */);
  a.gtk_window_set_title(window, kDesc.name);
  a.gtk_window_set_default_size(window, (int) app.instance.guiWidth,
                                (int) app.instance.guiHeight);
  // Closing the window ends the app; gtk_main_quit is the canonical handler.
  a.g_signal_connect_data(window, "destroy", (void (*)()) a.gtk_main_quit, nullptr, nullptr, 0);

  Instance* inst = &app.instance;
  // The device picker. Constructed HERE and not earlier because it holds
  // references to the two outputs this scope owns, and a hub that outlived
  // them would be a picker pointing at closed devices.
  DeviceHub hub{app, audio};
  hub.index = opt.deviceIndex;
  hub.name = audio.deviceName();
  hub.open = audioOk;
  installDeviceBridge(inst, hub);
  // Devices, re-checked about twice a second. A keyboard plugged in while the
  // window is open should appear in the picker without reopening it.
  uint64_t uiTick = 0, nextDevicePoll = 0;
  inst->webview.onTick = [inst, &hub, &uiTick, &nextDevicePoll]() {
    clapwrap::guiTick(inst);
    pollDevices(inst, hub, nextDevicePoll, ++uiTick, 30);
  };
  inst->webview.createInContainer(window, inst->guiWidth, inst->guiHeight, clapwrap::uiHtml(),
                                  bridgeScript(kDesc), kDesc.id);
  a.gtk_widget_show_all(window);

  a.gtk_main();
  inst->webview.destroy();
  audio.stop();
  return 0;
}

} // namespace standalone
} // namespace sonore

// ── macOS ────────────────────────────────────────────────────────────────────
#elif defined(__APPLE__)

namespace sonore {
namespace standalone {

// Compiled on macOS since 2026-09-01, and the offline modes run there under
// ctest; this interactive path, a real device, a real window, has not, because
// no macOS test opens either.
inline int runInteractive(App& app, const Options& opt) {
  PlatformAudio audio;
  const bool audioOk = startLiveAudio(app, audio, opt.deviceIndex);
  // Once, whichever backend just opened. A keyboard belongs to the program,
  // not to the soundcard.
  startMidi(app, opt.midiDeviceIndex);

  // After the output, never before: capture is asked for the rate the output
  // device chose, and that rate does not exist until the output is open.
  if (audioOk && opt.inputDeviceIndex >= 0 && !kDesc.isInstrument)
    startLiveInput(app, opt.inputDeviceIndex, audio.sampleRate());
  if (opt.playOnly) {
    if (!audioOk) return 1;
    usleep((useconds_t) (opt.seconds * 1e6));
    audio.stop();
    std::printf("[sonore] played %.1fs cleanly\n", opt.seconds);
    return 0;
  }

  using cocoa::cls;
  using cocoa::msg;
  id nsapp = msg<id>(cls("NSApplication"), sel_registerName("sharedApplication"));
  // NSApplicationActivationPolicyRegular = 0: a normal app with a Dock icon.
  msg<void>(nsapp, sel_registerName("setActivationPolicy:"), (long) 0);

  const cocoa::CGRectStruct frame{{100.0, 100.0},
                                  {(double) app.instance.guiWidth,
                                   (double) app.instance.guiHeight}};
  // Titled | closable | miniaturizable | resizable = 15.
  using NewWindow = id (*)(id, SEL, cocoa::CGRectStruct, unsigned long, long, BOOL);
  id window = reinterpret_cast<NewWindow>(objc_msgSend)(
      msg<id>(cls("NSWindow"), sel_registerName("alloc")),
      sel_registerName("initWithContentRect:styleMask:backing:defer:"), frame, 15ul, 2l, NO);
  msg<void>(window, sel_registerName("setTitle:"), cocoa::nsString(kDesc.name));

  id content = msg<id>(window, sel_registerName("contentView"));
  Instance* inst = &app.instance;
  // The device picker. Constructed HERE and not earlier because it holds
  // references to the two outputs this scope owns, and a hub that outlived
  // them would be a picker pointing at closed devices.
  DeviceHub hub{app, audio};
  hub.index = opt.deviceIndex;
  hub.name = audio.deviceName();
  hub.open = audioOk;
  installDeviceBridge(inst, hub);
  // Devices, re-checked about twice a second. A keyboard plugged in while the
  // window is open should appear in the picker without reopening it.
  uint64_t uiTick = 0, nextDevicePoll = 0;
  inst->webview.onTick = [inst, &hub, &uiTick, &nextDevicePoll]() {
    clapwrap::guiTick(inst);
    pollDevices(inst, hub, nextDevicePoll, ++uiTick, 30);
  };
  inst->webview.create(content, inst->guiWidth, inst->guiHeight, clapwrap::uiHtml(),
                       bridgeScript(kDesc), kDesc.id);

  msg<void>(window, sel_registerName("makeKeyAndOrderFront:"), (id) nullptr);
  msg<void>(nsapp, sel_registerName("activateIgnoringOtherApps:"), (BOOL) YES);
  msg<void>(nsapp, sel_registerName("run"));

  inst->webview.destroy();
  audio.stop();
  return 0;
}

} // namespace standalone
} // namespace sonore

#endif

// ── main ─────────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
  using namespace sonore::standalone;
  Options opt = parseOptions(argc, argv);
  static App app; // static: the Instance carries large inline DSP state

  // Before any run mode, because a plugin that reacts to its track reacts
  // when it is TOLD, and every mode below ends up preparing the DSP.
  tellTrackInfo(app, opt);

#if defined(_WIN32)
  if (opt.listAsio) {
    // Named, not numbered, because --asio takes a name. The registry order is
    // not stable across installs and a number would go stale.
    const auto drivers = sonore::asio::listDrivers();
    if (drivers.empty()) {
      std::printf("[sonore] no ASIO drivers are installed\n");
      return 0;
    }
    std::printf("[sonore] ASIO drivers:\n");
    for (const auto& d : drivers) std::printf("  %s\n", d.name.c_str());
    std::printf("  (choose with --asio \"<name>\")\n");
    // Deliberately NOT opened to report their rates. Opening a driver runs
    // its code in this process and two of the five on the machine this was
    // written on crash when their hardware is absent -- so a list is a list,
    // and finding out more costs a process.
    return 0;
  }
#endif

  if (opt.dumpUi) {
    // The bridge script FIRST, as the webview injects it, then the page. What
    // comes out is what actually runs -- not a reconstruction of it.
    std::string page = "<script>" + sonore::bridgeScript(kDesc) + "</script>";
    page += sonore::clapwrap::uiHtml();
    FILE* f = std::fopen(opt.dumpUi, "wb");
    if (!f) {
      std::fprintf(stderr, "[sonore] could not write %s\n", opt.dumpUi);
      return 1;
    }
    std::fwrite(page.data(), 1, page.size(), f);
    std::fclose(f);
    std::printf("[sonore] wrote %s (%zu bytes)\n", opt.dumpUi, page.size());
    return 0;
  }

  if (opt.listMidiIns) {
    const std::vector<std::string> ins = sonore::midiin::Device::listDevices();
    if (ins.empty()) {
      // Not an error. Most machines have no MIDI hardware, and a program that
      // exited non-zero for that would be calling a normal Tuesday a failure.
      std::printf("[sonore] no MIDI input devices are connected\n");
      return 0;
    }
    const int remembered = loadRememberedMidi();
    std::printf("[sonore] MIDI input devices:\n");
    for (size_t i = 0; i < ins.size(); ++i)
      std::printf("  %zu%s %s\n", i,
                  ((int) i == remembered) ? " *" : ((remembered < 0 && i == 0) ? " *" : "  "),
                  ins[i].c_str());
    std::printf("  (* = current; choose with --midi-input <n>)\n");
    return 0;
  }

  if (opt.listDevices) {
    // A plain numbered list: the number is what --device takes, so a user can
    // read one line and act on it without a manual.
    const std::vector<std::string> devices = PlatformAudio::listDevices();
    if (devices.empty()) {
      std::printf("[sonore] no audio output devices found\n");
      return 1;
    }
    const int remembered = loadRememberedDevice();
    std::printf("[sonore] audio output devices:\n");
    for (size_t i = 0; i < devices.size(); ++i)
      std::printf("  %zu%s %s\n", i,
                  ((int) i == remembered) ? " *" : ((remembered < 0 && i == 0) ? " *" : "  "),
                  devices[i].c_str());
    std::printf("  (* = current; choose with --device <n>)\n");
    return 0;
  }

  if (opt.listInputs) {
    const std::vector<std::string> devices = sonore::standalone::PlatformAudioInput::listDevices();
    if (devices.empty()) {
      std::printf("[sonore] no audio input devices found\n");
      return 1;
    }
    std::printf("[sonore] audio input devices:\n");
    for (size_t i = 0; i < devices.size(); ++i)
      std::printf("  %zu  %s\n", i, devices[i].c_str());
    std::printf("  (choose with --input-device <n>)\n");
    return 0;
  }

  if (opt.listMidiOuts) {
    const std::vector<std::string> ports = sonore::midiout::Device::enumerate();
    if (ports.empty()) {
      std::printf("[sonore] no MIDI output ports found\n");
      return 1;
    }
    std::printf("[sonore] MIDI output ports:\n");
    for (size_t i = 0; i < ports.size(); ++i)
      std::printf("  %zu  %s\n", i, ports[i].c_str());
    std::printf("  (choose with --midi-out <n>, or write a file with --midi-out-file <path>)\n");
    return 0;
  }

  // The sink outlives every use below, which is why it is here and not inside
  // a branch. A plugin that produces no MIDI is given one anyway when asked:
  // an empty file is a real answer and beats a silent refusal that leaves a
  // user wondering which half went wrong.
  if (opt.midiInPath) {
    sonore::MidiFileData file;
    if (!sonore::readMidiFile(opt.midiInPath, &file)) {
      std::printf("[sonore] could not read %s as a MIDI file\n", opt.midiInPath);
      return 1;
    }
    app.midiIn = sonore::MidiSequence::fromMidiFile(file);
    app.midiFileLoaded = !app.midiIn.empty();
    if (!app.midiFileLoaded) {
      std::printf("[sonore] %s has no events in it\n", opt.midiInPath);
      return 1;
    }
    std::printf("[sonore] playing %s: %zu events over %.1fs\n", opt.midiInPath,
                app.midiIn.size(), app.midiIn.duration());
  }

  if (opt.inputPath) {
    if (kDesc.isInstrument) {
      std::printf("[sonore] --input is for effects; %s is an instrument\n", kDesc.name);
      return 1;
    }
    if (!app.fileSource.load(opt.inputPath, 48000.0)) {
      std::printf("[sonore] %s\n", app.fileSource.error().c_str());
      return 1;
    }
    std::printf("[sonore] input: %s (%zu frames)\n", opt.inputPath,
                app.fileSource.numFrames());
  }

  std::unique_ptr<sonore::midiout::FileSink> fileSink;
  std::unique_ptr<sonore::midiout::Device> portSink;
  if (opt.midiOutPath) {
    fileSink.reset(new sonore::midiout::FileSink(opt.midiOutPath));
    app.midiSink = fileSink.get();
  } else if (opt.midiOutIndex >= 0) {
    portSink.reset(new sonore::midiout::Device());
    if (!portSink->open(opt.midiOutIndex)) {
      std::printf("[sonore] MIDI output: %s\n", portSink->error().c_str());
      return 1;
    }
    app.midiSink = portSink.get();
  }
  if (app.midiSink && !kDesc.producesMidi)
    std::printf("[sonore] note: this plugin does not produce MIDI, so nothing will be sent\n");

  if (opt.capturePath) {
    if (opt.inputDeviceIndex < 0) {
      std::printf("[sonore] --capture needs --input-device <n>; see --inputs\n");
      return 1;
    }
    // No output device is opened. Recording through a plugin is a job you do
    // with headphones off, and demanding a working output to use an input
    // would rule out every machine that has only one of the two.
    if (!sonore::standalone::startLiveInput(app, opt.inputDeviceIndex, 48000.0)) return 1;
    const uint64_t frames = sonore::standalone::captureLive(app, opt.seconds, opt.capturePath);
    const double rate = app.audioInput.sampleRate();
    const uint64_t starved = app.audioInput.starvedFrames();
    const uint64_t dropped = app.audioInput.droppedFrames();
    app.audioInput.stop();
    app.inputOpen = false;

    const bool ok = frames > 0;
    std::printf(ok ? "[sonore] wrote %s (%.1fs)\n" : "[sonore] could not write %s\n",
                opt.capturePath, (double) frames / (rate > 0 ? rate : 48000.0));
    // Reported every time, not only when bad. A recording with 4000 starved
    // frames in it is a recording with gaps, and the user should find that
    // out here rather than by listening for it.
    std::printf("[sonore] input: %llu frame(s) starved, %llu dropped\n",
                (unsigned long long) starved, (unsigned long long) dropped);
    return ok ? 0 : 1;
  }

  if (opt.verifyMode) return verify(app);

  if (opt.renderPath) {
    const std::vector<float> audio = renderOffline(app, opt.seconds);
    const bool ok = sonore::writeWav(opt.renderPath, audio.data(), audio.size() / 2, 2, 48000);
    // The length ACTUALLY written, not the one asked for. With --input the
    // material and the plugin tail decide it, and printing the --seconds
    // default beside a file of a different length is a small lie that costs
    // somebody an afternoon.
    std::printf(ok ? "[sonore] wrote %s (%.1fs)\n" : "[sonore] could not write %s\n",
                opt.renderPath, (double) (audio.size() / 2) / 48000.0);
    if (fileSink) {
      const size_t n = fileSink->numEvents();
      const bool wrote = fileSink->close();
      std::printf(wrote ? "[sonore] wrote %s (%zu events)\n" : "[sonore] could not write %s\n",
                  opt.midiOutPath, n);
      app.midiSink = nullptr;
      if (!wrote) return 1;
    }
    return ok ? 0 : 1;
  }

  // --play is a listening check, so it keeps the tone; the window does not.
  app.testSignal = opt.testSignal || opt.playOnly;
  if (!kDesc.isInstrument && !app.testSignal && opt.inputDeviceIndex < 0 &&
      !opt.inputPath && !opt.capturePath) {
    // Said out loud, because "I opened it and heard nothing" and "I opened it
    // and heard a tune" are both bug reports, and only one of them is cheap
    // to answer.
    std::printf(
        "[sonore] no input: this effect is processing silence. Feed it with"
        " --input-device N (list them with --inputs), --input song.wav, or hear"
        " the built-in test tone with --test-signal.\n");
  }
  return runInteractive(app, opt);
}
