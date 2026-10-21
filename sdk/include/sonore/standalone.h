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
//   --help                every option: devices, ASIO, MIDI, capture, track info
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
#include "path_io.h"
#if defined(_WIN32)
#include <shellapi.h> // CommandLineToArgvW: argv is not UTF-8 on Windows
#endif

namespace sonore {
namespace standalone {

using clapwrap::Instance;

/** How many main channels the standalone runs the DSP at: two, the device's
 *  stereo, held to what the descriptor says the DSP handles. It prepared and
 *  ran every DSP at two whatever it declared, so a DSP that handles exactly
 *  six -- which the descriptor check allows -- read four channels past the
 *  two it was given, and a mono-only one was prepared for two and handed one. */
inline uint32_t standaloneWidth() {
  uint32_t width = 2;
  if (width < clapwrap::minMainChannels()) width = clapwrap::minMainChannels();
  if (width > clapwrap::maxMainChannels()) width = clapwrap::maxMainChannels();
  return width < 1 ? 1 : (width > 8 ? 8 : width);
}

/**
 * What runDspStandalone needs beyond the DSP: silence for a key input nobody
 * routes, somewhere for aux buses to go, and the MIDI and expression buffers
 * of one block. Sized by prepare(), on the thread that starts the audio.
 *
 * These were thread_local vectors grown on first use -- so the first block on
 * every NEW audio thread allocated, which is the first block after every
 * device switch and driver reset, inside the callback that must not.
 */
struct StandaloneScratch {
  std::vector<float> silence, auxStore;
  /** The channels past the device's two, for a DSP wider than stereo: silent
   *  going in, and not played coming out. */
  std::vector<float> wide;
  MidiBuffer midiOut;
  NoteExpressionBuffer expression;
  /** The aux buses a ProcessContext DSP is handed each block. They were
   *  declared on the stack per block, kMaxAuxOutputs AudioBlocks and a
   *  kMaxAuxOutputs x kMaxAudioChannels table of pointers, all zeroed
   *  whether the plugin had an aux bus or not: 3 KB of stores, more than a
   *  quarter of what processBlock() spent on a 32-frame block around an
   *  instrument that does nothing. The wrappers keep theirs in the Instance
   *  for the same reason (clapwrap::processdetail::AuxScratch); a bus past
   *  the plugin's count is never written, so it stays the empty block it
   *  always was. */
  clapwrap::processdetail::AuxScratch<float> aux;

  void prepare(uint32_t maxFrames) {
    const size_t n = maxFrames > 0 ? maxFrames : 1;
    silence.assign(n, 0.0f);
    const uint32_t width = standaloneWidth();
    wide.assign(width > 2 ? (size_t) (width - 2) * n : 0, 0.0f);
    size_t flatCount = 0;
    for (uint32_t b = 0; b < clapwrap::numAuxOutputs(); ++b)
      flatCount += clapwrap::auxBusChannels(b);
    auxStore.assign(flatCount * n, 0.0f);
  }
};

/** This thread's own buffers, for a caller that brought none. Behind a
 *  function so the thread_local is built only when one is used: declared
 *  in runDspStandalone, it was built on the first block of every new audio
 *  thread whether or not the app had passed its own -- three empty vectors,
 *  which a Debug build's checked iterators allocate a proxy for apiece. */
inline StandaloneScratch& threadScratch() {
  static thread_local StandaloneScratch fallback;
  return fallback;
}

/** The standalone has no host to route a key signal, so a sidechain DSP
 *  simply hears silence. `scratch` comes from prepare(); without one (a
 *  caller outside the app) the buffers are this thread's own, grown on use. */
template <typename T>
inline void runDspStandalone(T& dsp, AudioBlock<float>& block, const float* params,
                             MidiBuffer& midi, midiout::Sink* sink = nullptr,
                             double timeSeconds = 0.0, double sampleRate = 48000.0,
                             MpeDecoder* mpe = nullptr, StandaloneScratch* scratch = nullptr,
                             bool offline = false) {
  StandaloneScratch& work = scratch ? *scratch : threadScratch();
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
    std::vector<float>& silence = work.silence;
    std::vector<float>& auxStore = work.auxStore;
    const size_t n = block.getNumSamples();
    const uint32_t nAux = clapwrap::numAuxOutputs();
    if (silence.size() < n) silence.assign(n, 0.0f);
    // Held in the scratch (StandaloneScratch::aux), each bus the plugin has
    // set up from a row of null pointers, as the stack table was.
    AudioBlock<float>* const auxBlocks = work.aux.blocks;
    size_t flatCount = 0;
    for (uint32_t b = 0; b < nAux; ++b) flatCount += clapwrap::auxBusChannels(b);
    if (auxStore.size() < flatCount * n) auxStore.assign(flatCount * n, 0.0f);
    size_t flat = 0;
    for (uint32_t b = 0; b < nAux; ++b) {
      const uint32_t width = clapwrap::auxBusChannels(b);
      float** const row = work.aux.row(b);
      for (uint32_t c = 0; c < width; ++c, ++flat) row[c] = auxStore.data() + flat * n;
      auxBlocks[b] = AudioBlock<float>(row, width, n);
    }
    float* sc[2] = {silence.data(), silence.data()};
    AudioBlock<float> scBlock(sc, 2, n);
    // What the plugin PLAYS. This used to be dropped on the floor after the
    // call, which for an arpeggiator or a note splitter meant the entire
    // output of the plugin went nowhere -- the one thing it exists to do.
    MidiBuffer& midiOut = work.midiOut;
    midiOut.clear();
    uint8_t roles[clapwrap::kMaxAudioChannels];
    const uint32_t numRoles = rolesFromMask(defaultChannelMask(
                                                (uint32_t) block.getNumChannels()),
                                            roles, clapwrap::kMaxAudioChannels);
    NoteExpressionBuffer& expression = work.expression;
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
    ProcessContext ctx{block,    auxBlocks, nAux,
                       scBlock,  midi,      midiOut,
                       numRoles ? roles : nullptr, &expression,
                       nullptr,  true,      offline};
    dsp.process(ctx, params);
    if (sink) {
      for (const auto& entry : midiOut) {
        const int at = entry.samplePosition < 0 ? 0 : entry.samplePosition;
        sink->send(entry.getMessage(), timeSeconds + (double) at / sampleRate);
      }
    }
  } else if constexpr (clapwrap::TakesSidechain<T>::value) {
    std::vector<float>& silence = work.silence;
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
  /** Decode the file at `path`, UTF-8, whole, and start it from its first
   *  frame. `expectedRate` above 0 refuses a file at another rate; 0 takes
   *  any, for a caller that checks once the device has chosen. False, with
   *  error() saying why, for a file that cannot be read, holds no audio, or
   *  is at the wrong rate; a refused file is not kept, and whatever was
   *  loaded before stays loaded, at its position. [main-thread]: it reads
   *  the disk. */
  bool load(const char* path, double expectedRate) {
    // Decoded aside and taken only once accepted. Decoded in place, a file
    // refused for its rate stayed held -- loaded() said yes after load()
    // said no, and the previous file was gone -- and one that failed half
    // way left whatever its decoder had filled.
    WavData decoded;
    if (!readAudioFile(path, &decoded)) {
      error_ = std::string("could not read ") + path;
      return false;
    }
    if (decoded.numChannels == 0 || decoded.numFrames() == 0) {
      error_ = std::string("there is no audio in ") + path;
      return false;
    }
    if (expectedRate > 0.0 && decoded.sampleRate > 0 &&
        std::fabs((double) decoded.sampleRate - expectedRate) > 1.0) {
      char buffer[160];
      std::snprintf(buffer, sizeof(buffer),
                    "%s is %u Hz and this run is %.0f Hz; no resampling is done",
                    path, (unsigned) decoded.sampleRate, expectedRate);
      error_ = buffer;
      return false;
    }
    data_ = std::move(decoded);
    position_ = 0;
    error_.clear();
    return true;
  }

  /** True while a file load() accepted is held. */
  bool loaded() const { return data_.numFrames() > 0; }
  /** The file's length in frames. */
  size_t numFrames() const { return data_.numFrames(); }
  /** The rate the file was recorded at; 0 when the file did not say. */
  uint32_t sampleRate() const { return data_.sampleRate; }
  /** True once every frame has been handed out, so a render knows when the
   *  material ends rather than looping it or padding it forever. */
  bool finished() const { return position_ >= data_.numFrames(); }
  /** Why the latest load() failed; empty after one that succeeded. */
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

  /** Back to the first frame. */
  void rewind() { position_ = 0; }

private:
  WavData data_;
  size_t position_ = 0;
  std::string error_;
};

/**
 * The internal source an effect processes when nothing else is playing: a slow
 * two-octave saw arpeggio with a soft envelope. Chosen over a sine because a
 * sine through a filter is inaudible as a demo, and over noise because noise
 * through a reverb is just more noise.
 *
 * prepare() first, with the run's rate; then next() once per sample, on the
 * audio thread, for a mono sample peaking about 13 dB below full scale.
 * Allocates nothing.
 */
class TestSource {
public:
  /** Start the arpeggio from its first note at `sampleRate` Hz; 48000 for a
   *  rate that is not above 1. */
  void prepare(double sampleRate) {
    sampleRate_ = sampleRate > 1.0 ? sampleRate : 48000.0;
    phase_ = 0.0;
    position_ = 0;
  }

  /** [audio-thread] The next sample, mono, peaking about 13 dB below full
   *  scale. */
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
  Instance instance; ///< The plugin itself, as the formats hold it: DSP, parameters, bridge.
  /** runDspStandalone's buffers, sized by prepare(). */
  StandaloneScratch scratch;
  /** Whether the parameters have been given their defaults yet. See prepare(). */
  bool paramsInitialised = false;
  TestSource source; ///< The built-in arpeggio, for an effect when testSignal is on.
  /** Whether the loaded file's rate is the rate the DSP is prepared at, which
   *  is the only rate it plays at. Settled by prepare(), on every open and
   *  every device switch. */
  bool fileRateMatches = true;
  /** Used INSTEAD of `source` when a file was given. Not merged with it: an
   *  effect fed both would be an effect fed neither. */
  FileSource fileSource;
  /** Used instead of BOTH when a capture device was opened. A live input
   *  outranks a file for the same reason a file outranks the test tone: it is
   *  the more specific thing the user asked for. */
  PlatformAudioInput audioInput;
  /** Atomic now that the picker can flip it while audio runs; the callback
   *  reads it every block. */
  std::atomic<bool> inputOpen{false};
  /** Blocks the audio thread has finished, so the UI thread can tell when a
   *  block that saw inputOpen before it was cleared has let go of the ring. */
  std::atomic<uint64_t> blocksDone{0};
  /** Whether the built-in arpeggio feeds an effect when nothing else does.
   *  Off unless asked for: see the banner. The offline modes turn it on
   *  because a render of silence measures nothing. */
  std::atomic<bool> testSignal{false};
  /** How many times the keyboard has been opened this run. Should be one, and
   *  --switch-devices fails if it is not: this was six for a six-device sweep
   *  until MIDI stopped being a property of the soundcard, and nothing on the
   *  machine that found it would have noticed. */
  int midiOpens = 0;
  /** Which MIDI input is open, or -1. The picker shows it. */
  int midiIndex = -1;
  /** Which entry of PlatformAudio::listDevices() the system output was
   *  opened as -- chosen, or remembered and found again by name -- or -1
   *  for the default. The picker starts from it. */
  int outputDevice = -1;
  double sampleRate = 48000.0; ///< The rate the DSP is prepared at, in Hz: the device's, or 48000 offline.
  /** Live MIDI, when a device was found. The queue is written by the device
   *  thread and drained by the audio callback -- never the other way round. */
  midiin::MessageQueue midiQueue;
  midiin::Device midiDevice; ///< The keyboard feeding midiQueue; startMidi() and stopMidi() open and close it.
  /** Written by the UI thread, read by the audio thread every block: atomic,
   *  as inputOpen is. It was a plain bool. */
  std::atomic<bool> midiOpen{false};
  /** Raised when a keyboard is closed or swapped with keys down: the next
   *  block sends All Notes Off on every channel, or the notes it was holding
   *  sound until something else releases them. */
  std::atomic<bool> midiReleaseAll{false};

  /** Where the plugin's OWN notes go, if anywhere. Null is the normal case
   *  and means the same thing it always did -- a synth has nothing to send
   *  and an effect that does has nowhere asked for. */
  midiout::Sink* midiSink = nullptr;
  /** A performance to play into the plugin, instead of the one note the
   *  offline render invents. What turns an instrument standalone into
   *  something that can render a demo of itself. */
  MidiSequence midiIn;
  bool midiFileLoaded = false; ///< True once --midi-in-file has filled midiIn.
  /** Samples since the run began, so a file sink writes ticks that mean
   *  something. Not a wall clock: two renders of the same thing have to
   *  produce the same file. */
  uint64_t framesElapsed = 0;
};

/**
 * One block of the plugin, exactly as the formats run it: drain UI events,
 * synthesize or pass input, run the DSP, publish the meters.
 *
 * `externalIn` is audio the DEVICE already has, interleaved: the ASIO path
 * gets input and output from one driver in one callback, so there is nothing
 * to open separately and nothing to resample. Null means the usual chain --
 * a capture device, a loaded file, or the test source -- applies instead.
 */
inline void processBlock(App& app, float* left, float* right, uint32_t frames,
                         const float* externalIn = nullptr, uint32_t externalChannels = 0) {
  const auto blockStart = std::chrono::steady_clock::now();
  Instance* inst = &app.instance;
  inst->midi.clear();
  // Whatever a keyboard sent since the last block, ahead of the UI events so
  // a note and a knob move in the same block keep the order they happened in.
  //
  // A keyboard closed or swapped: everything it was holding goes, AFTER what
  // it sent last -- notes still in the queue from the old keyboard came
  // through behind the release and stuck -- and the sustain pedal first,
  // since a note let go under a held pedal keeps sounding, and the pedal's
  // own release will never come from a keyboard that is gone.
  const bool releaseAll = app.midiReleaseAll.exchange(false, std::memory_order_acq_rel);
  if (app.midiOpen || releaseAll) app.midiQueue.drain(inst->midi);
  if (releaseAll)
    for (int channel = 0; channel < 16; ++channel) {
      inst->midi.addEvent(MidiMessage::controlChange(channel, 64, 0), 0);
      inst->midi.addEvent(MidiMessage::controlChange(channel, 123, 0), 0);
    }
  // A file being played, at the position the run has reached. Before the UI
  // events for the same reason a keyboard is: what happened first goes first.
  if (app.midiFileLoaded)
    app.midiIn.fillBufferFrames(inst->midi, (int64_t) app.framesElapsed, frames, app.sampleRate);
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
  } else if (app.fileSource.loaded() && app.fileRateMatches) {
    app.fileSource.read(left, right, frames);
  } else if (app.testSignal) {
    for (uint32_t i = 0; i < frames; ++i) left[i] = right[i] = app.source.next();
  } else {
    // Nothing is feeding this effect. Silence, not a tune nobody asked for.
    std::memset(left, 0, frames * sizeof(float));
    std::memset(right, 0, frames * sizeof(float));
  }

  clapwrap::sendTransport(inst->dsp, TransportInfo{});
  // The device is stereo; a mono-only DSP processes the left channel and the
  // result is mirrored so both speakers play, and a DSP that only runs wider
  // gets silent channels past the two, whose output is not played.
  const uint32_t width = standaloneWidth();
  const bool monoDsp = width < 2;
  float* chans[8] = {left, right};
  const size_t stride = app.scratch.wide.size() / (width > 2 ? width - 2 : 1);
  for (uint32_t c = 2; c < width; ++c) {
    chans[c] = app.scratch.wide.data() + (size_t) (c - 2) * stride;
    std::memset(chans[c], 0, sizeof(float) * frames);
  }
  AudioBlock<float> block(chans, width, frames);
  // The keys held, for a page that draws a keyboard (sonore.notes, onNotes):
  // read off the block's MIDI once everything is in it -- the keyboard, the
  // file, the page's own keys -- as every plugin format does before its DSP.
  // Never kept here, a page's keyboard stayed dark under every key played.
  trackNotes(inst->notes, inst->midi);
  clapwrap::snapshotParams(inst);
  runDspStandalone(inst->dsp, block, inst->paramsBlock, inst->midi, app.midiSink,
                   (double) app.framesElapsed / app.sampleRate, app.sampleRate,
                   kDesc.supportsMpe ? &inst->mpe : nullptr, &app.scratch, inst->offline);
  app.framesElapsed += frames;
  if (monoDsp && right != left) std::memcpy(right, left, sizeof(float) * frames);

  // Here the SDK is the host, and the host is what stands between a DSP and
  // the speakers. A NaN or an infinity -- one unstable filter, one divide by
  // zero -- went to the device as is, and what a device does with one is its
  // own business: a full-scale burst on many.
  //
  // The plugin formats' output guard, on what the device plays: a block with
  // any of either in it is sent as silence, counted, and said once from the
  // main thread (the UI tick, or the end of an offline run). The aux buses and
  // the channels past two are never played, so they are not the device's
  // business. A plugin that turns the guard off still gets the per-sample
  // scrub this host always did, because the speakers are still here.
  if (kDesc.guardOutput) {
    float* played[2] = {left, right};
    clapwrap::guardOutputs<float>(inst, played, right != left ? 2u : 1u, nullptr, 0, frames);
  } else {
    // The comparison is false for NaN as well as for infinity.
    for (uint32_t i = 0; i < frames; ++i) {
      if (!(std::fabs(left[i]) < 3.0e38f)) left[i] = 0.0f;
      if (!(std::fabs(right[i]) < 3.0e38f)) right[i] = 0.0f;
    }
  }

  inst->meter.push(measureBlock(left, (size_t) frames));
  // The scope and the load meter the editor shows, as every plugin format
  // feeds them. Only the level meter was fed: the CPU read 0, the scope
  // never moved.
  float* shown[1] = {left};
  inst->scope.push(shown, 1, frames);
  inst->load.record(
      std::chrono::duration<double>(std::chrono::steady_clock::now() - blockStart).count(), frames);
  app.blocksDone.fetch_add(1, std::memory_order_release);
}

/**
 * [audio-thread] One device callback's worth of the plugin, for an output
 * that takes interleaved float: processBlock() in slices of at most 256
 * frames, each written into the device's `channels`. A mono device gets both
 * sides at half each (given the left alone, it never played what a plugin
 * put on the right); channels past two get silence. `in`, when not null, is
 * the device's own interleaved input for the same frames, `inChannels` wide,
 * handed to processBlock() slice by slice: the ASIO path, one driver giving
 * both in one callback, two channels each way.
 *
 * The system output's callback and the ASIO one were each a copy of this
 * as a lambda, deciding per FRAME what a channel count fixed for the whole
 * stream: 1.2 ns a frame at stereo, more than the processBlock() calls of
 * the same period cost around a DSP that does nothing. The count is decided
 * per slice now, and stereo and mono get a loop of their own; any other
 * width takes the general loop, which is the old one. What reaches the
 * device is the same, value for value.
 */
inline void renderInterleaved(App& app, float* interleaved, uint32_t frames, uint32_t channels,
                              const float* in = nullptr, uint32_t inChannels = 0) {
  constexpr uint32_t kBlock = 256;
  static thread_local float left[kBlock], right[kBlock];
  uint32_t done = 0;
  while (done < frames) {
    const uint32_t n = (frames - done) < kBlock ? (frames - done) : kBlock;
    // The same slice of the input the output slice is about to replace.
    processBlock(app, left, right, n, in ? in + (size_t) done * inChannels : nullptr,
                 in ? inChannels : 0);
    float* out = interleaved + (size_t) done * channels;
    if (channels == 2) {
      for (uint32_t i = 0; i < n; ++i) {
        out[2 * i] = left[i];
        out[2 * i + 1] = right[i];
      }
    } else if (channels == 1) {
      for (uint32_t i = 0; i < n; ++i) out[i] = 0.5f * (left[i] + right[i]);
    } else {
      for (uint32_t i = 0; i < n; ++i) {
        float* frame = out + (size_t) i * channels;
        frame[0] = channels > 1 ? left[i] : 0.5f * (left[i] + right[i]);
        if (channels > 1) frame[1] = right[i];
        for (uint32_t c = 2; c < channels; ++c) frame[c] = 0.0f;
      }
    }
    done += n;
  }
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

/** Every parameter at its default, once. Called as the program starts, not
 *  only from prepare(): a window opens even when no audio device does (none
 *  there, one held exclusively, an --asio driver unplugged), and the knobs
 *  then sat at 0 rather than at their defaults -- and the first device the
 *  user picked afterwards set them all back, undoing whatever they had done. */
inline void initialiseParams(App& app) {
  if (app.paramsInitialised) return;
  for (int i = 0; i < SONORE_NUM_PARAMS && i < kDesc.numParams; ++i)
    app.instance.params[i] = kDesc.params[i].defaultValue;
  app.paramsInitialised = true;
}

/** `offline` is a render no device is waiting for (--render, --verify): the
 *  DSP is told, in the spec and in every block's context, as every format
 *  tells it when its host bounces -- a DSP that trades quality for CPU while
 *  monitoring renders the better version. A device is never offline. */
inline void prepare(App& app, double sampleRate, uint32_t maxBlock, bool offline = false) {
  tellHostInfo(app);
  // The run's position is kept in frames and read in seconds over the rate,
  // so a device at another rate would move it: a MIDI file's playback point
  // and --midi-out-file's timestamps jumped by the ratio of the two rates on
  // every switch. Carried across at the same time instead.
  if (app.sampleRate > 0.0 && sampleRate > 0.0 && sampleRate != app.sampleRate)
    app.framesElapsed = (uint64_t) ((double) app.framesElapsed * sampleRate / app.sampleRate + 0.5);
  app.sampleRate = sampleRate;
  ProcessSpec spec;
  spec.sampleRate = sampleRate;
  spec.maximumBlockSize = maxBlock;
  spec.numChannels = standaloneWidth();
  spec.offline = offline;
  app.instance.offline = offline;
  // The DSP forgets its voices here, so no key stays lit on the page for a
  // note nothing is sounding -- as every format's prepareDsp() does.
  app.instance.notes.allOff();
  app.instance.dsp.prepare(spec);
  // Each block's deadline is frames over the DEVICE's rate; unset, the load
  // meter judged everything against 48 kHz.
  app.instance.load.reset(sampleRate);
  app.scratch.prepare(maxBlock);
  app.source.prepare(sampleRate);
  // A file plays at its own rate or not at all (see FileSource), and the rate
  // that counts is the one the device runs at, settled here on every open and
  // switch. It was checked once, against 48 kHz, before any device opened: a
  // 44.1 kHz device refused a 44.1 kHz file and played a 48 kHz one 8.8% slow,
  // a semitone and a half flat.
  if (app.fileSource.loaded() && app.fileSource.sampleRate() > 0) {
    const bool matches = std::fabs((double) app.fileSource.sampleRate() - sampleRate) <= 1.0;
    if (!matches && app.fileRateMatches)
      std::fprintf(stderr,
                   "[sonore] the input file is %u Hz and the audio runs at %.0f Hz; it is not played "
                   "(no resampling is done)\n",
                   (unsigned) app.fileSource.sampleRate(), sampleRate);
    if (matches && !app.fileRateMatches)
      std::fprintf(stderr, "[sonore] the input file plays again at %.0f Hz\n", sampleRate);
    app.fileRateMatches = matches;
  }
  // Defaults on the FIRST prepare only. This also runs on every device switch
  // and every driver reset, and writing defaults there snapped every knob back
  // the moment somebody picked another soundcard -- or the moment an ASIO
  // driver asked to be reopened, which the user did not do at all.
  initialiseParams(app);
  // Before the audio starts, so its first block has a whole set to fall back
  // on if a preset is loading beside it.
  clapwrap::primeParamsBlock(&app.instance);
}

// ── Offline rendering ────────────────────────────────────────────────────────

/** How much of a plugin's reported tail an offline render plays: all of it,
 *  up to 30 seconds. A frozen reverb reports an endless tail -- INT_MAX, which
 *  CLAP spells "infinite" -- and a render that added it to a file's length
 *  reserved 17 GB and would have run for half a day. Thirty seconds is past
 *  any decay a user means to bounce; an endless one is cut there. */
constexpr double kMaxOfflineTailSeconds = 30.0;
/** The tail an offline render adds, in frames: `tailSamples`, held to
 *  kMaxOfflineTailSeconds at `sampleRate`. */
inline uint64_t offlineTailFrames(uint32_t tailSamples, double sampleRate) {
  const uint64_t cap = (uint64_t) (kMaxOfflineTailSeconds * sampleRate);
  return (uint64_t) tailSamples < cap ? (uint64_t) tailSamples : cap;
}

/** Render `seconds` offline at 48 kHz. For instruments, a short phrase is
 *  played programmatically so the render contains sound to judge. */
inline std::vector<float> renderOffline(App& app, double seconds) {
  constexpr double kRate = 48000.0;
  constexpr uint32_t kBlock = 128;
  // As a bounce: nothing waits for these blocks, and the DSP was told it was
  // monitoring, so a plugin that renders better offline rendered its
  // real-time version into the file.
  prepare(app, kRate, kBlock, /*offline=*/true);
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
            offlineTailFrames(clapwrap::dspTail(app.instance.dsp), kRate) + (uint64_t) kRate;
  }
  if (app.fileSource.loaded()) {
    // The material decides, plus the plugin's own tail so a reverb is not
    // cut off at the last input sample. Asking a user to also pass --seconds
    // matching the file they just passed would be asking them to do a
    // subtraction the program can do.
    app.fileSource.rewind();
    total = (uint64_t) app.fileSource.numFrames() +
            offlineTailFrames(clapwrap::dspTail(app.instance.dsp), kRate);
  }
  std::vector<float> interleaved;
  interleaved.reserve((size_t) total * 2);

  // The plugin's own delay, removed as a host's delay compensation removes it
  // from a bounce: that many more frames are run and the first that many are
  // not kept. Unremoved, a look-ahead limiter's file came out late by its
  // look-ahead and -- the tail being added for decay, not delay -- lost as
  // much of the material's end: a 1000-frame ramp through a 100-sample delay
  // rendered 100 frames of silence and never its last 100 frames. Held to the
  // same 30 seconds as a tail, so an absurd report cannot run for hours.
  const uint64_t latency = offlineTailFrames(clapwrap::dspLatency(app.instance.dsp), kRate);
  const uint64_t run = total + latency;

  float left[kBlock], right[kBlock];
  uint64_t done = 0;
  // Explicit one-shot flags. A first version encoded the phrase state in a
  // note number's sign, and after note-off the "no note" test matched again:
  // the note retriggered every block and the release-tail check caught it.
  bool noteOnSent = false;
  bool noteOffSent = false;
  while (done < run) {
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
    const uint32_t frames = (uint32_t) ((run - done) < kBlock ? (run - done) : kBlock);
    processBlock(app, left, right, frames);
    for (uint32_t i = 0; i < frames; ++i) {
      if (done + i < latency) continue;
      interleaved.push_back(left[i]);
      interleaved.push_back(right[i]);
    }
    done += frames;
  }
  // Offline, the blocks ran on this thread, which is the main one: the output
  // guard's report, if the render owes one, is made here.
  clapwrap::reportOutputGuard(&app.instance);
  return interleaved;
}

/**
 * Record the live input through the plugin, straight to disk, for a fixed
 * time.
 *
 * Not renderOffline with a different source: offline runs as fast as the CPU
 * allows, and a capture device delivers at exactly one speed. This has to
 * wait, which is the whole difference. It once waited against the computer's
 * CLOCK (a fixed sleep per block drifted, so the pacing was made absolute);
 * it now waits for what the DEVICE has delivered, since any second clock
 * drifts against the device's -- the loop says what that cost.
 *
 * This used to grow a std::vector for the whole take and write it at the end,
 * which is fine for the twenty seconds a test renders and turns into
 * gigabytes for an actual recording -- and puts a reallocation in the middle
 * of a capture loop. WavRecorder hands each block to a writer thread instead,
 * so memory is one ring however long the take runs.
 *
 * Returns the number of frames that reached the file, or 0 if it could not be
 * opened or the input never was (which the caller has already been told
 * about). The file is interleaved stereo at the input's rate.
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
  auto lastDelivery = std::chrono::steady_clock::now();
  while (done < total) {
    const uint32_t frames = (uint32_t) ((total - done) < kBlock ? (total - done) : kBlock);
    // Paced by what the DEVICE has delivered. It was paced by the computer's
    // clock, which is a second clock: the two drift, and a device running
    // 50 ppm slow was read ahead of itself, a short read was padded with
    // silence and the ring refilled its cushion with more -- 8,448 silent
    // frames in 21 gaps over an hour, most of them counted nowhere. A
    // recording has one clock, the device's, and reads only what it gave.
    if (app.audioInput.availableFrames() < frames) {
      // A device that stops delivering ends the recording rather than
      // hanging it.
      if (std::chrono::steady_clock::now() - lastDelivery > std::chrono::seconds(2)) {
        std::printf("[sonore] the input stopped delivering; the recording ends at %.1fs\n",
                    (double) done / rate);
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      continue;
    }
    lastDelivery = std::chrono::steady_clock::now();
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
  clapwrap::reportOutputGuard(&app.instance); // this thread ran the blocks
  return recorder.framesWritten();
}

/** The health assertions --verify runs. The same physics the plugin gates use:
 *  finite always; audible for an effect (it processes the source) and for an
 *  instrument's held note; silent again after an instrument's release. */
inline int verify(App& app) {
  std::printf("standalone verify -- %s (%s)\n", kDesc.name,
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

  {
    // The output guard turns a block of NaN or infinity into silence before
    // it reaches the render, so the samples alone can no longer say whether
    // the DSP made one. The guard's count does: a block it had to silence is
    // a DSP bug, which is what this check is for.
    const uint32_t silenced = app.instance.outputGuard.trips.load(std::memory_order_acquire);
    char line[200];
    std::snprintf(line, sizeof(line),
                  "the render is finite (no NaN/Inf; the output guard silenced %u block%s)",
                  (unsigned) silenced, silenced == 1 ? "" : "s");
    check(finite && silenced == 0, line);
  }
  {
    int bad = -1;
    const char* why = ::sonore::clapwrap::pluginDescriptorProblem(&bad);
    char line[320];
    if (!why)
      std::snprintf(line, sizeof(line), "the descriptor is well-formed");
    else if (bad >= 0)
      std::snprintf(line, sizeof(line), "the descriptor is well-formed (parameter %d has %s)", bad, why);
    else
      std::snprintf(line, sizeof(line), "the descriptor is well-formed (it has %s)", why);
    check(why == nullptr, line);
  }
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
    check(read, "...and the file reads back as a Standard MIDI File");

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
    check(notes > 0, "...containing the notes the plugin played");
    // Inside the render it was made from. A tempo or tick conversion that is
    // off by a factor still produces a valid file, and this is what catches
    // it: two seconds of arpeggio cannot end at twenty.
    check(lastSecond > 0.0 && lastSecond <= 2.05,
          "...at times that fall inside the two seconds rendered");
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

    // On the HEAP, both of them: an App carries the Instance and the Instance
    // carries the DSP inline -- main() makes its own App static for exactly
    // this reason. The examples are small enough that a stack App passed
    // every ctest; a generated echo with 2.4 MB of delay lines overflowed
    // the 8 MB Linux stack (and the 1 MB Windows one) in --verify alone,
    // which is the mode a build farm and a seller run first.
    std::unique_ptr<App> fileApp(new App);
    const bool loaded = fileApp->fileSource.load(kInputPath, 48000.0);
    check(loaded, "...and read back as material");

    // A rate that does not match is REFUSED, not resampled behind the user's
    // back. Playing a 44.1k file at 48k comes out sharp and short, and doing
    // that silently to somebody's material is worse than declining it.
    std::unique_ptr<App> wrongRate(new App);
    check(!wrongRate->fileSource.load(kInputPath, 44100.0),
          "...and a rate that does not match is refused rather than resampled quietly");
    check(!wrongRate->fileSource.error().empty(), "...with the two rates named");
    // ...and nothing is loaded: the refused file was kept, and loaded() said
    // yes to a source that had just said no.
    check(!wrongRate->fileSource.loaded(), "...and leaves nothing loaded");
    check(!fileApp->fileSource.load("no-such-file-anywhere.wav", 48000.0),
          "a file that is not there is refused");
    // A refused load keeps the file loaded before it, whole: the render below
    // plays it.
    check(fileApp->fileSource.loaded() && fileApp->fileSource.numFrames() == kFrames &&
              fileApp->fileSource.sampleRate() == 48000,
          "...and the file loaded before it is still the one held");

    if (loaded) {
      const std::vector<float> rendered = renderOffline(*fileApp, 0.0);
      const size_t frames = rendered.size() / 2;

      // Long enough to hold the material AND the plugin's own tail. A render
      // that stops at the last input sample cuts a reverb off mid-decay.
      check(frames >= kFrames, "the render is at least as long as the material");

      double fileEnergy = 0.0;
      bool fileFinite = true;
      for (float v : rendered) {
        if (!std::isfinite(v)) fileFinite = false;
        fileEnergy += (double) v * v;
      }
      check(fileFinite && fileApp->instance.outputGuard.trips.load() == 0,
            "...and finite, with no block the output guard had to silence");
      check(fileEnergy > 0.0, "...and not silence");

      // The point of the whole thing: the FILE was processed, not the test
      // tone. Rendering the same plugin without a file must not produce the
      // same audio, or --input changed nothing and this check passed for no
      // reason.
      std::unique_ptr<App> toneApp(new App); // heap, as above
      const std::vector<float> tone = renderOffline(*toneApp, (double) kFrames / 48000.0);
      const size_t compare = frames < tone.size() / 2 ? frames : tone.size() / 2;
      double difference = 0.0;
      for (size_t i = 0; i < compare * 2; ++i)
        difference += std::fabs((double) rendered[i] - (double) tone[i]);
      const double perSample = compare ? difference / (double) (compare * 2) : 0.0;
      std::printf("  ---- %zu frames in, %zu out; mean difference from the test tone %.4f ----\n",
                  (size_t) kFrames, frames, perSample);
      check(perSample > 0.01, "...and it is the FILE that was processed, not the test tone");

      // Silence in, silence out. A source that was ignored would give the
      // test tone here and this is what would catch it.
      {
        std::vector<float> quiet((size_t) kFrames * 2, 0.0f);
        const char* kQuietPath = "sonore-verify-silence.wav";
        if (sonore::writeWav(kQuietPath, quiet.data(), kFrames, 2, 48000)) {
          std::unique_ptr<App> silentApp(new App); // heap, as above
          if (silentApp->fileSource.load(kQuietPath, 48000.0)) {
            const std::vector<float> out = renderOffline(*silentApp, 0.0);
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
/** The system output the standalone plays through: WASAPI on Windows, Core
 *  Audio on macOS, the Linux backend elsewhere. ASIO is chosen apart. */
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
 * single-integer format forever is worse. The settings keep the device's
 * number AND its name now, and the name is what finds it again.
 */
inline int loadRememberedDevice() {
  UserSettings settings(kDesc.id);
  if (!settings.load()) return -1;
  const int64_t v = settings.values().getInt("audioDevice", -1);
  const int index = (v >= 0 && v < 256) ? (int) v : -1;
  // Found again by NAME, when the name was kept. The list is numbered by
  // position, and a device unplugged since -- or one plugged in ahead of
  // it -- moves every number after it: the number alone opened somebody
  // else's speakers. Not here now, it is the default, and said so.
  const std::string name = settings.values().getString("audioDeviceName", "");
  if (index < 0 || name.empty()) return index;
  const int now = detail::findListedDevice(PlatformAudio::listDevices(), index, name);
  if (now < 0)
    std::printf("  The remembered output \"%s\" is not connected; using the default\n",
                name.c_str());
  return now;
}

/** Save output device `index` of PlatformAudio::listDevices(), and its name,
 *  in the plugin's user settings for the next launch, and forget any ASIO
 *  driver. Nothing for an index below 0. Writes a file: not for the audio
 *  thread. */
inline void rememberDevice(int index) {
  if (index < 0) return;
  UserSettings settings(kDesc.id);
  // Read first: this file belongs to the whole plugin, and writing only the
  // key we care about would delete every other preference in it.
  settings.load();
  settings.values().setInt("audioDevice", index);
  settings.values().setString("audioDeviceName",
                              detail::listedDevice(PlatformAudio::listDevices(), index));
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

/** Save ASIO driver `driver`, by name, for the next launch. Nothing for an
 *  empty name. Writes a file. */
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
  // The input is interleaved as wide as the output: the driver's first two
  // of each, both ways (AsioOutput's scratch).
  const bool ok = out.open(driverName, [&app](const float* in, float* interleaved,
                                               uint32_t frames, uint32_t channels) {
    renderInterleaved(app, interleaved, frames, channels, in, channels);
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

/** "No keyboard", chosen in the picker -- remembered, so the next launch does
 *  not open one anyway. Distinct from -1, which is "never chosen": that opens
 *  the first device there is. */
constexpr int kMidiNone = -2;

/** The MIDI input saved last time, found again by name in
 *  midiin::Device::listDevices(): its index, kMidiNone for "no keyboard",
 *  or -1 when nothing was saved or the device is not connected. Reads a
 *  file. */
inline int loadRememberedMidi() {
  UserSettings settings(kDesc.id);
  if (!settings.load()) return -1;
  const int64_t v = settings.values().getInt("midiDevice", -1);
  if (v == kMidiNone) return kMidiNone;
  const int index = (v >= 0 && v < 256) ? (int) v : -1;
  // By name, as the output is: a second keyboard plugged in renumbers the
  // list. One that is not here now is "never chosen" -- the first there is.
  const std::string name = settings.values().getString("midiDeviceName", "");
  if (index < 0 || name.empty()) return index;
  return detail::findListedDevice(sonore::midiin::Device::listDevices(), index, name);
}

/** Save MIDI input `index`, and its name, or kMidiNone, for the next
 *  launch. Nothing for any other negative index. Writes a file. */
inline void rememberMidi(int index) {
  if (index < 0 && index != kMidiNone) return;
  UserSettings settings(kDesc.id);
  settings.load();
  settings.values().setInt("midiDevice", index);
  settings.values().setString("midiDeviceName",
                              detail::listedDevice(sonore::midiin::Device::listDevices(), index));
  settings.save();
}

/** [main-thread] Close the keyboard: the audio thread stops draining its
 *  queue first, and the next block sends All Notes Off for any key held. */
inline void stopMidi(App& app) {
  if (app.midiOpen) app.midiReleaseAll.store(true, std::memory_order_release);
  app.midiOpen = false; // the audio thread stops draining before the device goes
  app.midiDevice.close();
  app.midiIndex = -1;
}

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
inline void startMidi(App& app, int deviceIndex = -1) {
  const int chosen = deviceIndex >= 0 ? deviceIndex : loadRememberedMidi();
  if (chosen == kMidiNone) {
    stopMidi(app);
    std::printf("  MIDI input: none, as chosen\n");
    return;
  }
  ++app.midiOpens;
  // Another keyboard in place of this one: whatever this one was holding is
  // released, since its note-offs will never come.
  if (app.midiOpen) app.midiReleaseAll.store(true, std::memory_order_release);
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

/** The picker's choice. "None" (any index below zero) CLOSES the keyboard and
 *  is remembered: it called startMidi(-1), whose -1 means "whatever was
 *  remembered", so choosing None reopened the very keyboard being turned
 *  off. */
inline bool selectMidi(App& app, int index) {
  if (index < 0) {
    stopMidi(app);
    rememberMidi(kMidiNone);
    return true;
  }
  startMidi(app, index);
  return app.midiOpen;
}

/**
 * Tell the DSP which audio workgroup the device's IO thread is in, if it
 * declared workgroupChanged() (audio_workgroup.h) -- so a DSP rendering on
 * helper threads can have them join it. With audio stopped, between prepare
 * and run, so never beside a process() call.
 *
 * macOS 11 and later only: CoreAudio is the one backend with a workgroup to
 * give, and every other backend takes the template below, which does
 * nothing because there is nothing to say. A Mac that cannot provide one says
 * why, once, to a DSP that asked. Compiled for macOS, never run there.
 */
#if defined(__APPLE__)
inline void tellDeviceWorkgroup(App& app, CoreAudioOutput& output) {
  std::string why;
  const AudioWorkgroup workgroup = output.ioWorkgroup(&why);
  if (!workgroup && ::sonore::detail::HasWorkgroupHook<SonoreDsp>::value)
    std::fprintf(stderr, "[sonore] no audio workgroup for the device: %s\n", why.c_str());
  dspWorkgroupChanged(app.instance.dsp, workgroup);
}
#endif
template <typename Output>
inline void tellDeviceWorkgroup(App&, Output&) {}

/** `remember` false is a reopen the program made on its own, which must not
 *  replace the device the user chose as the one to open next time. */
inline bool startAudio(App& app, PlatformAudio& audio, int deviceIndex = -1, bool remember = true) {
  // A device the user picked, otherwise the one they picked last time,
  // otherwise the system default.
  const int chosen = deviceIndex >= 0 ? deviceIndex : loadRememberedDevice();
  // Set even when it is -1, the default: the object is reused from one
  // reopen to the next, and a number left from the previous device was
  // opened in place of the default.
  audio.setDeviceIndex(chosen);
  const bool ok = audio.open([&app](float* interleaved, uint32_t frames, uint32_t channels) {
    renderInterleaved(app, interleaved, frames, channels);
  });
  if (!ok) return false;
  app.outputDevice = chosen;
  prepare(app, audio.sampleRate(), 256);
  tellDeviceWorkgroup(app, audio);

  std::printf("  Audio output: %s\n",
              audio.deviceName().empty() ? "(unnamed device)" : audio.deviceName().c_str());
  if (deviceIndex >= 0 && remember) rememberDevice(deviceIndex);

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

/** The standalone's command line, as parseOptions() reads it and main acts
 *  on it: an offline mode (verify, render, capture, dump the page) or the
 *  live window, which devices to open, and what to feed the plugin. Plain
 *  data, filled once at startup; printUsage() lists every flag. Pointers
 *  point into argv and live as long as the program. */
struct Options {
  bool verifyMode = false; ///< --verify: render offline, check the output is healthy, exit with the verdict.
  /** --test-signal: play the built-in arpeggio through an effect. */
  bool testSignal = false;
  bool listDevices = false; ///< --devices: list the output devices.
  bool listMidiIns = false; ///< --midi-inputs: list the MIDI inputs.
  int midiDeviceIndex = -1; ///< --midi-input n; -1 for the remembered one, or the first.
  /** Cycle every audio device live, reporting each. The picker's mechanism
   *  without the picker: a page clicking through the list is exactly this, and
   *  a switch that works from here works from there. */
  bool switchTest = false;
  /** Write the page this program would show, and the bridge script that runs
   *  in it, to a file. The interface is HTML; being able to READ it without a
   *  window is worth one flag. */
  const char* dumpUi = nullptr;
  int deviceIndex = -1;             ///< --device n; below 0, whatever was remembered, or the system default.
  const char* renderPath = nullptr; ///< --render: the WAV an offline render writes.
  const char* shotPath = nullptr;   ///< --shot: the screenshot to save (Windows).
  bool playOnly = false;            ///< --play: live audio without a window.
  double seconds = 2.0;             ///< How long --render, --play, --shot and --capture run; main() takes 0 to 3600.
  /** Material to feed an effect, instead of the built-in test tone. */
  const char* inputPath = nullptr;
  /** A MIDI file to play into an instrument, instead of the single note the
   *  offline render invents. */
  const char* midiInPath = nullptr;
  /** --inputs: list the capture devices. */
  bool listInputs = false;
  /** --input-device n: a live capture device, which outranks both. <0 means
   *  none. */
  int inputDeviceIndex = -1;
  /** Record the live input, through the plugin, to this file. */
  const char* capturePath = nullptr;
  bool listMidiOuts = false; ///< --midi-outs: list the MIDI outputs.
  /** Where the plugin's own MIDI goes. A port, a file, or nowhere. */
  int midiOutIndex = -1;
  const char* midiOutPath = nullptr; ///< --midi-out-file: a Standard MIDI File for it instead.
  /** An ASIO driver BY NAME. Not by index: the registry order is not stable,
   *  an installer can renumber it, and a number remembered in a settings file
   *  would then point at somebody else's hardware. */
  const char* asioDriver = nullptr;
  bool listAsio = false; ///< --asios: list the ASIO drivers (Windows).
  /** What track to CLAIM to be on. There is no track here, so none of this is
   *  true unless a developer says it is -- which is the point: a plugin that
   *  reacts to its track is otherwise only testable inside a DAW. */
  const char* trackName = nullptr;
  const char* trackColour = nullptr; ///< --track-colour: "#rgb" or "#rrggbb".
  bool trackIsReturn = false;        ///< --track-return.
  bool trackIsBus = false;           ///< --track-bus.
  bool trackIsMaster = false;        ///< --track-master.
  /** --help: the list below, then exit. */
  bool showHelp = false;
  /** The first argument that looked like an option and was not one -- or was
   *  one missing the value it needs, or given one that is not a number. A typo such as --rendr used to start the live
   *  window as though nothing had been asked. */
  const char* unknown = nullptr;
};

/** Every option parseOptions() reads, for --help. Six were described at the
 *  top of this file; it reads twenty-seven. */
inline void printUsage(const char* program) {
  std::printf(
      "usage: %s [options]\n"
      "\n"
      "With no options: the plugin's window, playing through the default audio device.\n"
      "\n"
      "Offline -- no audio device, no display:\n"
      "  --verify                 render, check the output is healthy; the exit code is the verdict\n"
      "  --render <out.wav> [s]   render s seconds (default 2) to a 32-bit float WAV\n"
      "  --dump-ui <out.html>     write the page the interface runs, bridge script included\n"
      "\n"
      "Live:\n"
      "  --play [s]               play for s seconds (default 2) without a window, then exit\n"
      "  --shot <out.bmp> [s]     (Windows) open the window, wait s seconds, save a screenshot\n"
      "  --capture <out.wav>      record the live input through the plugin for --seconds\n"
      "  --seconds <s>            length for --render, --shot and --capture (at most 3600)\n"
      "  --switch-devices         (Windows) open and play every output device in turn, then exit\n"
      "\n"
      "Audio devices (a chosen device is remembered for the next run):\n"
      "  --devices                list the output devices (* = current)\n"
      "  --device <n>             play through output device n\n"
      "  --asios                  (Windows) list the ASIO drivers\n"
      "  --asio <name>            (Windows) play through an ASIO driver, by name\n"
      "\n"
      "What an effect hears (an effect opens on silence otherwise):\n"
      "  --inputs                 list the capture devices, then exit\n"
      "  --input-device <n>       capture device n, live\n"
      "  --input <file>           an audio file, played at its own rate: it must match the\n"
      "                           device's (48000 Hz in the offline modes)\n"
      "  --test-signal            the built-in arpeggio\n"
      "\n"
      "MIDI:\n"
      "  --midi-inputs            list the MIDI inputs (* = current)\n"
      "  --midi-input <n>         play the plugin from MIDI input n\n"
      "  --midi-in-file <f.mid>   play a MIDI file into the plugin\n"
      "  --midi-outs              list the MIDI outputs\n"
      "  --midi-out <n>           send the plugin's own MIDI to output n\n"
      "  --midi-out-file <f.mid>  write the plugin's own MIDI to a file\n"
      "\n"
      "The track the plugin is told it is on (there is none; nothing is claimed unless asked):\n"
      "  --track-name <name>  --track-colour <#rrggbb> (or --track-color)  --track-return\n"
      "  --track-bus  --track-master\n"
      "\n"
      "  --help, -h               this list\n"
      , program ? program : "standalone");
}

/** Read the command line. Nothing is refused: an unknown option, or one
 *  without the number it needs, is left in Options::unknown for the caller
 *  to report, and the rest is still read. The paths point into `argv`. */
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
    // A number the option cannot do without. Missing, or not a number --
    // --device Speakers -- it fell back to the default without a word, and
    // the program played somewhere the user had not asked for.
    auto required = [&](double fallback) {
      const int before = i;
      const double v = number(fallback);
      if (i == before && !opt.unknown) opt.unknown = arg;
      return v;
    };
    // A place in a device list: a whole number, or the option is reported.
    // Cast straight to int, NaN and 1e30 were undefined behaviour; and
    // --midi-input, read with atoi, took "Keystation" as 0 -- the first port,
    // opened and remembered over the user's own choice without a word.
    auto index = [&]() {
      const double v = required(-1.0);
      if (v == std::floor(v) && v >= -1.0 && v <= 1.0e6) return (int) v;
      if (!opt.unknown) opt.unknown = arg;
      return -1;
    };
    if (std::strcmp(arg, "--help") == 0 || std::strcmp(arg, "-h") == 0) opt.showHelp = true;
    else if (std::strcmp(arg, "--switch-devices") == 0) opt.switchTest = true;
    else if (std::strcmp(arg, "--dump-ui") == 0 && i + 1 < argc) opt.dumpUi = argv[++i];
    else if (std::strcmp(arg, "--devices") == 0) opt.listDevices = true;
    else if (std::strcmp(arg, "--midi-inputs") == 0) opt.listMidiIns = true;
    else if (std::strcmp(arg, "--midi-input") == 0) opt.midiDeviceIndex = index();
    else if (std::strcmp(arg, "--device") == 0) opt.deviceIndex = index();
    else if (std::strcmp(arg, "--verify") == 0) opt.verifyMode = true;
    else if (std::strcmp(arg, "--test-signal") == 0) opt.testSignal = true;
    // The optional seconds after the path, as the usage at the top of this
    // file and the README both show. Only the path used to be read, so
    // `--render out.wav 10` rendered the default two seconds and left "10"
    // to be ignored.
    else if (std::strcmp(arg, "--render") == 0 && i + 1 < argc) {
      opt.renderPath = argv[++i];
      opt.seconds = number(opt.seconds);
    } else if (std::strcmp(arg, "--shot") == 0 && i + 1 < argc) {
      opt.shotPath = argv[++i];
      opt.seconds = number(opt.seconds);
    }
    else if (std::strcmp(arg, "--play") == 0) { opt.playOnly = true; opt.seconds = number(2.0); }
    else if (std::strcmp(arg, "--seconds") == 0) opt.seconds = required(opt.seconds);
    else if (std::strcmp(arg, "--input") == 0 && i + 1 < argc) opt.inputPath = argv[++i];
    else if (std::strcmp(arg, "--midi-in-file") == 0 && i + 1 < argc)
      opt.midiInPath = argv[++i];
    else if (std::strcmp(arg, "--inputs") == 0) opt.listInputs = true;
    else if (std::strcmp(arg, "--capture") == 0 && i + 1 < argc) opt.capturePath = argv[++i];
    else if (std::strcmp(arg, "--input-device") == 0) opt.inputDeviceIndex = index();
    else if (std::strcmp(arg, "--midi-outs") == 0) opt.listMidiOuts = true;
    else if (std::strcmp(arg, "--midi-out") == 0) opt.midiOutIndex = index();
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
    else if (arg[0] == '-' && !opt.unknown) opt.unknown = arg;
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
inline bool startLiveAudio(App& app, PlatformAudio& audio, int deviceIndex = -1,
                           bool remember = true) {
  if (!startAudio(app, audio, deviceIndex, remember)) {
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
                                    uint32_t bufferFrames, int currentMidi,
                                    int currentInput, bool testSignal) {
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
  json += "],inputs:[";
  const std::vector<std::string> inputs = PlatformAudioInput::listDevices();
  for (size_t i = 0; i < inputs.size(); ++i) {
    if (i) json += ",";
    json += "'" + escapeForJs(inputs[i].c_str()) + "'";
  }
  json += "],currentInput:" + std::to_string(currentInput);
  json += ",testSignal:" + std::string(testSignal ? "true" : "false");
  json += ",currentMidi:" + std::to_string(currentMidi);
  json += ",current:{backend:'" + escapeForJs(currentBackend.c_str()) + "',index:" +
          std::to_string(currentIndex) + ",name:'" + escapeForJs(currentName.c_str()) + "'},";
  json += "sampleRate:" + std::to_string((long) (rate + 0.5)) + ",";
  json += "bufferFrames:" + std::to_string((unsigned long) bufferFrames) + "}";
  return json;
}

inline bool startLiveInput(App& app, int deviceIndex, double rate);

/** The audio device, and the ability to change it while the program runs.
 *
 *  Holds REFERENCES to the two outputs the run path already owns rather
 *  than owning them itself: an interface that is open has a window sitting
 *  on it, and a hub that owned the device would have to outlive or be
 *  outlived by that window in some order nobody would remember.
 *
 *  UI thread only: select() and the service calls are driven from the
 *  page's messages and the UI tick. A switch stops the audio, or clears the
 *  flag the callback checks, before it changes anything the callback reads. */
struct DeviceHub {
  App& app;
  PlatformAudio& system;
#if defined(_WIN32)
  sonore::asio::AsioOutput& asioOut;
#endif
  /** "system" or "asio" -- which of the two is currently making sound. */
  std::string backend = "system";
  int index = -1;
  std::string name{};
  bool open = false;
  /** Which capture device feeds the effect, or -1. The picker shows it. */
  int inputIndex = -1;
  /** What the two numbers were called when they were chosen. The lists are
   *  numbered by position, and a device that goes away takes its number to
   *  its neighbour: a device is found again by this, never by number. */
  std::string entry{};
  std::string inputEntry{};
  /** The last list handed to the page, so a poll can tell new from same. */
  std::string lastPushed_{};

  double sampleRate() const {
#if defined(_WIN32)
    if (backend == "asio") return asioOut.sampleRate();
#endif
    return system.sampleRate();
  }

  /** The names of what the program opened at start, before anything can
   *  have moved. */
  void learnNames() {
    entry = backend == "system" ? detail::listedDevice(PlatformAudio::listDevices(), index)
                                : std::string();
    inputEntry.clear();
    if (inputIndex < 0) return;
    const std::vector<std::string> inputs = PlatformAudioInput::listDevices();
    // A number past the list -- --input-device 1 on a Mac, whose list is the
    // default alone -- opened the default, which every backend lists first.
    // Left nameless, the first output switch found no such device and closed
    // an input that was working.
    if ((size_t) inputIndex >= inputs.size() && !inputs.empty()) inputIndex = 0;
    inputEntry = detail::listedDevice(inputs, inputIndex);
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
    // What is making sound, which with no output open is nothing: no device,
    // no rate. A switch stops the old device before it opens the new one, so
    // a refused switch -- an interface not plugged in, a device held
    // exclusively -- leaves nothing open, and the page was handed the old
    // device as the current one at its old rate: a picker saying "Speakers,
    // 48000 Hz" over silence, the reason only on stderr. A launch with no
    // device said the same of a device that never opened.
    if (!open)
      return audioDevicesJson(std::string(), -1, std::string(), 0.0, 0, app.midiIndex,
                              app.inputOpen ? inputIndex : -1, app.testSignal);
    return audioDevicesJson(backend, index, name, sampleRate(), bufferFrames(), app.midiIndex,
                            app.inputOpen ? inputIndex : -1, app.testSignal);
  }

  /**
   * Reopen the ASIO driver if it asked to be. Polled from the UI tick.
   *
   * A driver whose buffer size or rate was changed in its own control panel
   * asks the HOST to tear it down and build it again, and applies nothing
   * until that happens. Here, on this thread, with the same order a switch
   * uses -- stop, open, prepare at whatever the driver now says, run -- and
   * the parameters left where the user put them.
   *
   * Returns true when the device was reopened, so the caller can tell the
   * page what the rate and block size are now.
   */
  bool serviceDriverReset() {
#if defined(_WIN32)
    if (backend != "asio" || !open || !asioOut.takeResetRequest()) return false;
    const std::string driver = name;
    stopAll();
    if (!startAsio(app, asioOut, driver.c_str())) return false; // silent, and says so
    name = asioOut.driverName();
    open = true;
    return true;
#else
    return false;
#endif
  }

  /**
   * Reopen when the system device went away. Polled from the UI tick.
   *
   * An unplugged interface invalidates its stream for good, and the stream
   * used to stay "open" and silent until the user picked a device by hand.
   * The default device is reopened instead -- the OS has already moved its
   * own default to whatever is still plugged in -- in the order a switch
   * uses, so the DSP is prepared at the new rate before the clock starts.
   * Returns true when something changed, so the caller can tell the page.
   */
  bool serviceDeviceLost() {
    // A capture device that went away closes the input, and the picker
    // shows it closed: the effect then processes silence because it was
    // TOLD to, not because a room went quiet.
    bool changed = false;
    if (app.inputOpen && app.audioInput.takeDeviceLost()) {
      std::fprintf(stderr, "[sonore] the input device went away; input closed\n");
      app.inputOpen = false;
      app.audioInput.stop();
      inputIndex = -1;
      changed = true;
    }
    if (backend != "system" || !open || !system.takeDeviceLost()) return changed;
    std::fprintf(stderr, "[sonore] the audio stream was lost; reopening its device, or the default\n");
    // The device the user chose, if it is still there -- a format change in
    // Sound settings or an exclusive-mode takeover ends a stream whose device
    // remains -- and the system default when it is not. This reopened index
    // 0 and remembered it, so an automatic recovery overwrote the user's
    // choice for every later launch; and on macOS, where 0 is the first
    // device in the list rather than the default, it moved the output to
    // whichever device came first.
    //
    // Found by NAME. The device that went away took its place in the list
    // with it, so its old number named its neighbour: an unplugged USB
    // interface at 1 reopened as the HDMI output at 2, now 1.
    const int reopen = detail::findListedDevice(PlatformAudio::listDevices(), index, entry);
    stopAll();
    if (!startLiveAudio(app, system, reopen, /*remember=*/false)) {
      std::fprintf(stderr, "[sonore] no audio device could be opened; audio is stopped\n");
      return true; // silent, and the picker says so
    }
    index = app.outputDevice;
    if (index < 0) entry.clear();
    name = system.deviceName();
    open = true;
    followOutputRate();
    return true;
  }

  /** Reopen the capture device at the output's rate, after the output
   *  moved. It was opened converting to the OLD rate -- WASAPI's AUTOCONVERT,
   *  CoreAudio by moving the microphone, ALSA by resampling -- and left so: a
   *  switch from 48 to 44.1 kHz then wrote about 3,900 frames a second more
   *  than were read, a full ring and crackle within two seconds, and the
   *  other way round starved it. The flag first, as every input switch does. */
  void followOutputRate() {
    if (!app.inputOpen || inputIndex < 0) return;
    // First: is the microphone still there? A headset unplugged takes its
    // microphone with it, and the output's loss can be handled before the
    // input's -- so this is the first to hear of it. Reopened by its old
    // number, the input became whichever microphone took that number (or
    // the new default, for "System default"), a live room went through the
    // effect to the speakers, and the reopen cleared the lost flag that
    // would have closed it. Closed instead, as a lost input is.
    const std::vector<std::string> list = PlatformAudioInput::listDevices();
    const std::string actual = app.audioInput.deviceName();
    bool stillThere = actual.empty();
    for (const std::string& listed : list)
      if (listed == actual) stillThere = true;
    const bool lost = app.audioInput.takeDeviceLost();
    const int at = detail::findListedDevice(list, inputIndex, inputEntry);
    app.inputOpen = false;
    app.audioInput.stop();
    if (lost || !stillThere || at < 0) {
      std::fprintf(stderr, "[sonore] the input device went away; input closed\n");
      inputIndex = -1;
      inputEntry.clear();
      return;
    }
    if (!startLiveInput(app, at, sampleRate())) {
      std::fprintf(stderr, "[sonore] the input could not follow the output to %.0f Hz; input closed\n",
                   sampleRate());
      inputIndex = -1;
      inputEntry.clear();
      return;
    }
    inputIndex = at;
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
    if (wantBackend == "midi") return selectMidi(app, wantIndex);
    if (wantBackend == "test") {
      app.testSignal = wantIndex != 0;
      return true;
    }
    if (wantBackend == "input") {
      // The flag first, then the device: the callback checks the flag before
      // it reads, and a block already past the check finishes on the ring it
      // holds -- the same order the ASIO switch below has always used.
      if (app.inputOpen) {
        app.inputOpen = false;
        app.audioInput.stop();
      }
      inputIndex = -1;
      inputEntry.clear();
      if (wantIndex < 0 || !open) return true;
      if (!startLiveInput(app, wantIndex, sampleRate())) return false;
      inputIndex = wantIndex;
      inputEntry = detail::listedDevice(PlatformAudioInput::listDevices(), wantIndex);
      return true;
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
    index = app.outputDevice;
    entry = detail::listedDevice(PlatformAudio::listDevices(), index);
    name = system.deviceName();
    open = true;
    followOutputRate();
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
  // Every tick, not on the slow interval: a driver waiting to be reopened is
  // a device making no sound, and the check is one atomic exchange.
  if (hub.serviceDriverReset() && hub.changedSinceLastPush()) pushDevices(inst, hub);
  if (hub.serviceDeviceLost()) pushDevices(inst, hub);
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

/**
 * The standalone's own chrome, drawn INSIDE the page over the plugin's face.
 *
 * A plugin window shows the plugin's interface and nothing else, and that is
 * right for a plugin: the host owns the devices. A standalone IS the host, and
 * a generated faceplate has no device picker -- so somebody who opened their
 * delay had no way to choose an output, an input, or to hear anything at all
 * beyond a line in a console they may not have seen.
 *
 * In the page rather than as a native menu, because a native menu is three
 * implementations (Win32, NSMenu, GTK) of one thing, and this project has found
 * the same rule diverging across platforms often enough to stop writing three
 * of anything. One script, appended to the bridge only by the standalone, so
 * no plugin ever carries it. Everything it shows comes from sonore.audioDevices,
 * which the standalone already pushes; everything it does goes through the
 * bridge calls the fallback panel already makes.
 */
inline std::string standaloneChromeScript() {
  std::string js;
  js += "(function(){function build(){if(document.getElementById('sonore-host'))return;"
        "var css=document.createElement('style');css.textContent="
        "'#sonore-host{position:fixed;right:10px;bottom:10px;z-index:2147483000;font:12px system-ui,sans-serif;color:#dce5ee}"
        "#sonore-host button.h{width:30px;height:30px;border-radius:15px;border:1px solid #3a4250;background:#151a21;color:#dce5ee;cursor:pointer;opacity:.75;font-size:15px;line-height:1}"
        "#sonore-host button.h:hover{opacity:1}"
        "#sonore-host .p{display:none;position:absolute;right:0;bottom:36px;width:260px;padding:10px 12px;border-radius:10px;border:1px solid #3a4250;background:#151a21;box-shadow:0 12px 30px rgba(0,0,0,.5)}"
        "#sonore-host.open .p{display:block}"
        "#sonore-host label{display:block;margin:6px 0 2px;font-size:10px;letter-spacing:.06em;text-transform:uppercase;color:#8a96a6}"
        "#sonore-host select{width:100%;padding:4px 6px;border-radius:6px;border:1px solid #3a4250;background:#0d1014;color:#dce5ee;font-size:12px}"
        "#sonore-host .t{display:flex;align-items:center;gap:8px;margin-top:10px;font-size:12px;color:#dce5ee}"
        "#sonore-host .t input{margin:0}"
        "#sonore-host .s{margin-top:8px;font-size:10px;color:#8a96a6}';"
        "document.head.appendChild(css);"
        "var host=document.createElement('div');host.id='sonore-host';"
        "host.innerHTML='<div class=p><label>Output</label><select id=sh-out></select>"
        "<label>Input</label><select id=sh-in></select>"
        "<label>MIDI</label><select id=sh-midi></select>"
        "<div class=t><input type=checkbox id=sh-test><label for=sh-test style=\"display:inline;margin:0;text-transform:none;font-size:12px;color:#dce5ee\">Built-in test tone</label></div>"
        "<div class=s id=sh-rate></div></div>"
        "<button class=h title=\"Audio devices\">&#9881;</button>';"
        "document.body.appendChild(host);"
        "host.querySelector('button.h').onclick=function(){host.classList.toggle('open');};"
        "var out=host.querySelector('#sh-out'),inp=host.querySelector('#sh-in'),midi=host.querySelector('#sh-midi'),test=host.querySelector('#sh-test'),rate=host.querySelector('#sh-rate');"
        "function opt(sel,label,value,selected){var o=document.createElement('option');o.textContent=label;o.value=value;if(selected)o.selected=true;sel.appendChild(o);}"
        "function fill(d){"
        // Nothing open: said as an entry of its own. With none selected the
        // list showed its first device as chosen, and choosing that one again
        // changed nothing, so it fired no change and could not be reopened.
        "out.innerHTML='';var i;if(!d.current.backend)opt(out,'None (no output open)','none:',true);"
        "for(i=0;i<d.system.length;i++)opt(out,d.system[i],'system:'+i,d.current.backend==='system'&&d.current.index===i);"
        "for(i=0;i<d.asio.length;i++)opt(out,'ASIO: '+d.asio[i],'asio:'+d.asio[i],d.current.backend==='asio'&&d.current.name===d.asio[i]);"
        "inp.innerHTML='';opt(inp,'None (silence)','-1',d.currentInput<0);"
        "var ins=d.inputs||[];for(i=0;i<ins.length;i++)opt(inp,ins[i],String(i),d.currentInput===i);"
        "midi.innerHTML='';opt(midi,'None','-1',d.currentMidi<0);"
        "for(i=0;i<d.midi.length;i++)opt(midi,d.midi[i],String(i),d.currentMidi===i);"
        "test.checked=!!d.testSignal;"
        "rate.textContent=d.sampleRate?(d.sampleRate+' Hz'+(d.bufferFrames?' / '+d.bufferFrames+' frames':'')):'no audio device';}"
        "sonore.onAudioDevices(fill);if(sonore.audioDevices)fill(sonore.audioDevices);"
        "out.onchange=function(){var v=out.value,k=v.indexOf(':'),b=v.slice(0,k),w=v.slice(k+1);"
        "if(b!=='none')sonore.selectAudioDevice(b,b==='asio'?w:parseInt(w,10));};"
        "inp.onchange=function(){sonore.selectAudioDevice('input',parseInt(inp.value,10));};"
        "midi.onchange=function(){sonore.selectMidiDevice(parseInt(midi.value,10));};"
        "test.onchange=function(){sonore.setTestSignal(test.checked);};}"
        "if(document.readyState==='loading')document.addEventListener('DOMContentLoaded',build);else build();})();\n";
  return js;
}

/** The bridge a standalone installs: the plugin's, plus the chrome above. */
inline std::string standaloneBridge() {
  return bridgeScript(kDesc) + standaloneChromeScript();
}

/**
 * What every platform does once its window exists: the device picker's hub is
 * wired to the page, and the UI tick both ticks the wrapper and re-checks the
 * devices. This was the same eleven lines in three places, and eleven lines in
 * three places is where the next platform-only bug lives.
 */
inline void attachPicker(Instance* inst, DeviceHub& hub, uint64_t& uiTick, uint64_t& nextPoll) {
  installDeviceBridge(inst, hub);
  inst->webview.onTick = [inst, &hub, &uiTick, &nextPoll]() {
    clapwrap::guiTick(inst);
    // The main thread's clock: where the output guard's report is made, once,
    // after the device callback had to silence a block.
    clapwrap::reportOutputGuard(inst);
    // With no device open, no audio thread reads the page's events, and the
    // first block after one opened replayed every edit made meanwhile, oldest
    // first -- from a queue that, full, had dropped the NEWEST: Drive dragged
    // to 20 came back as 13.1, and the knob snapped to it. Emptied here
    // instead, and not applied: a knob, the native editor and a preset all
    // write the parameter as well as queueing it, so the parameters already
    // hold the latest values and the queue holds only older ones; gestures
    // have no host to tell; notes have nothing to play them, and sounding them
    // all at once when audio started was the other half of the bug. While
    // nothing plays, this is the queue's only reader.
    if (!hub.open) {
      UiEventQueue::Event stale;
      while (inst->uiEvents.pop(&stale)) {
      }
    }
    pollDevices(inst, hub, nextPoll, ++uiTick, 30);
  };
}

/** Wait for the audio thread to finish the block it may be in. Clearing
 *  inputOpen stops the NEXT block reading the capture ring, not one already
 *  past the check -- and opening an input resets the ring, reallocating it
 *  when it grows, under a reader that may still be in it. One finished block
 *  after the flag is enough: it either started before the flag and is done,
 *  or started after and never looked. A tenth of a second at most, for an
 *  output that is not running at all -- or that is between two callbacks,
 *  where no block is in progress to wait for.
 *
 *  By the clock, not by counting sleeps: a 1 ms sleep lasts 15.6 ms on a
 *  Windows timer nobody has raised, and a hundred of them held the interface
 *  for a second and a half. A block in progress ends in well under a
 *  millisecond, so the first two are spent yielding rather than asleep. */
inline void waitForBlockBoundary(App& app) {
  const uint64_t seen = app.blocksDone.load(std::memory_order_acquire);
  const auto start = std::chrono::steady_clock::now();
  while (app.blocksDone.load(std::memory_order_acquire) == seen) {
    const auto waited = std::chrono::steady_clock::now() - start;
    if (waited >= std::chrono::milliseconds(100)) break;
    if (waited < std::chrono::milliseconds(2)) std::this_thread::yield();
    else std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

/** What --play says when its time is up, on the main thread: the output
 *  guard's report if the device callback owes one, and "cleanly" only when
 *  nothing had to be silenced. */
inline void reportPlayed(App& app, double seconds) {
  clapwrap::reportOutputGuard(&app.instance);
  const uint32_t silenced = app.instance.outputGuard.trips.load(std::memory_order_acquire);
  if (silenced == 0)
    std::printf("[sonore] played %.1fs cleanly\n", seconds);
  else
    std::printf("[sonore] played %.1fs; the output guard silenced %u block%s of NaN or infinity\n",
                seconds, (unsigned) silenced, silenced == 1 ? "" : "s");
}

/** [main-thread] Open capture device `deviceIndex` (0 the system default) at
 *  `rate`, the DSP's, and start it feeding the effect. False, with the reason
 *  on stderr and nothing left open, when it will not open or start. */
inline bool startLiveInput(App& app, int deviceIndex, double rate) {
  // Called with inputOpen clear; a block that read it before then may still
  // be reading the ring that open() is about to reset.
  waitForBlockBoundary(app);
  // A failed open or start is stopped as well: it can leave the client, the
  // unit or the COM reference it got that far with, and the next selection
  // skipped stop() because the input was not marked open, so they leaked.
  if (!app.audioInput.open(deviceIndex, rate)) {
    std::fprintf(stderr, "[sonore] audio input unavailable: %s\n",
                 app.audioInput.error().c_str());
    app.audioInput.stop();
    return false;
  }
  if (!app.audioInput.run()) {
    std::fprintf(stderr, "[sonore] audio input would not start: %s\n",
                 app.audioInput.error().c_str());
    app.audioInput.stop();
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

/** The standalone window's procedure: quits on WM_DESTROY, and keeps the
 *  webview sized and scaled with the window. */
inline LRESULT CALLBACK appWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  auto* inst = (Instance*) GetWindowLongPtrW(hwnd, GWLP_USERDATA);
  if (msg == WM_DESTROY) {
    PostQuitMessage(0);
    return 0;
  }
  // The window moved to a monitor with a different scale: Windows says where
  // it should now sit, in device pixels, and the webview follows the client.
  if (msg == 0x02E0 /* WM_DPICHANGED */ && inst) {
    inst->guiScale = (double) HIWORD(wp) / 96.0;
    const RECT* r = (const RECT*) lp;
    SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    return 0;
  }
  if (msg == WM_SIZE && inst && wp != SIZE_MINIMIZED) {
#if defined(SONORE_HAS_WEBVIEW_BACKEND)
    inst->webview.setSize(LOWORD(lp), HIWORD(lp));
#endif
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
    // The limits are logical; the frame is device pixels.
    const double s = inst ? inst->guiScale : 1.0;
    minW = clapwrap::scaledPixels(minW, s);
    minH = clapwrap::scaledPixels(minH, s);
    maxW = clapwrap::scaledPixels(maxW, s);
    maxH = clapwrap::scaledPixels(maxH, s);

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
  std::FILE* f = ::sonore::pathio::open(path, "wb");
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

/** The standalone's window, at the editor's logical size times `dpiScale` in
 *  device pixels, not yet shown. Null if Windows would not make one. */
inline HWND createAppWindow(App& app, double dpiScale) {
  app.instance.guiScale = dpiScale;

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

  // The window is the LOGICAL size times the scale, in device pixels; the
  // page inside lays out in logical pixels and the webview renders it sharp.
  RECT want{0, 0, (LONG) clapwrap::scaledPixels(app.instance.guiWidth, dpiScale),
            (LONG) clapwrap::scaledPixels(app.instance.guiHeight, dpiScale)};
  AdjustWindowRect(&want, kStyle, FALSE);
  const std::wstring title = win32::widen(kDesc.name);
  HWND hwnd = CreateWindowExW(0, L"SonoreStandalone", title.c_str(), kStyle,
                              CW_USEDEFAULT, CW_USEDEFAULT, want.right - want.left,
                              want.bottom - want.top, nullptr, nullptr, wc.hInstance, nullptr);
  if (!hwnd) return nullptr;
  // appWndProc reaches the instance through the window: WM_DPICHANGED and
  // WM_SIZE have to move the webview, and WM_GETMINMAXINFO has to scale.
  SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR) &app.instance);
  // And the size again, now that it can. CreateWindowEx asks for the limits
  // (WM_GETMINMAXINFO comes before even WM_NCCREATE) and holds the window to
  // them, and at that moment the window has no instance, so the limits were
  // the logical ones, unscaled. A fixed-size editor at 125% got a 620x300
  // client for a 775x375 page: cut off, and with no edge to drag it larger.
  // Not yet shown, so nobody sees the first size.
  SetWindowPos(hwnd, nullptr, 0, 0, want.right - want.left, want.bottom - want.top,
               SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
  return hwnd;
}

/** [main-thread] The live modes: open the audio device `opt` names (or the
 *  remembered one), the keyboard and the input, then the window and its
 *  message loop -- or, for --play and --switch-devices, no window. Returns
 *  the process's exit code: 0 when it ran, 1 when the audio could not start
 *  or a test mode (--switch-devices, --shot) failed. */
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
    hub.index = useAsio ? -1 : app.outputDevice;
    hub.name = useAsio ? asioOut.driverName() : audio.deviceName();
    hub.open = audioOk;
    hub.inputIndex = app.inputOpen ? opt.inputDeviceIndex : -1;
    hub.learnNames();

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
    reportPlayed(app, opt.seconds);
    return 0;
  }
  // Without audio the window still opens: seeing the UI beats a refusal.

  // Per-monitor DPI awareness, declared here rather than in a manifest the
  // consumer's build might not carry. Without it Windows scales the whole
  // window as a bitmap at 150% and 200% -- legible, and blurry in a way a
  // plugin next to a sharp DAW makes obvious. Both calls are Windows 10
  // (1703 / 1607); resolved at run time so an older machine still opens the
  // window, unaware, rather than failing to load the executable.
  double dpiScale = 1.0;
  if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
    using SetCtxFn = BOOL(WINAPI*)(HANDLE);
    using DpiFn = UINT(WINAPI*)();
    if (auto setCtx = (SetCtxFn) GetProcAddress(user32, "SetProcessDpiAwarenessContext"))
      setCtx((HANDLE) (intptr_t) -4 /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 */);
    if (auto getDpi = (DpiFn) GetProcAddress(user32, "GetDpiForSystem"))
      dpiScale = (double) getDpi() / 96.0;
  }
  HWND hwnd = createAppWindow(app, dpiScale);
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
  // Where the output really is: a device remembered from the last run was
  // opened with no number given, and the picker started from -1 -- showing
  // nothing, or the default, beside the device that was playing.
  hub.index = useAsio ? -1 : app.outputDevice;
  hub.name = useAsio ? asioOut.driverName() : audio.deviceName();
  hub.open = audioOk;
  hub.inputIndex = app.inputOpen ? opt.inputDeviceIndex : -1;
  hub.learnNames();
  uint64_t uiTick = 0, nextDevicePoll = 0;
  attachPicker(inst, hub, uiTick, nextDevicePoll);
  if (inst->webview.create(hwnd, clapwrap::scaledPixels(inst->guiWidth, dpiScale),
                           clapwrap::scaledPixels(inst->guiHeight, dpiScale), clapwrap::uiHtml(),
                           standaloneBridge(), kDesc.id)) {
    inst->webview.setVisible(true);
  } else {
    // No webview -- the runtime is not installed. The plugin formats fall
    // back to their native editor; this window says why it is empty, and the
    // audio, which needs no webview, keeps running.
    RECT client{};
    GetClientRect(hwnd, &client);
    CreateWindowExW(0, L"STATIC",
                    L"This interface needs the Microsoft Edge WebView2 Runtime, which is not "
                    L"installed. The audio is running.",
                    WS_CHILD | WS_VISIBLE | 0x00000001L /* SS_CENTER */, 0, client.bottom / 3,
                    client.right, client.bottom / 3, hwnd, nullptr,
                    (HINSTANCE) GetModuleHandleW(nullptr), nullptr);
  }

  // 64 bits: the 32-bit tick count wraps every 49.7 days of uptime, and a
  // deadline set across the wrap fired at once.
  const ULONGLONG deadline =
      opt.shotPath ? GetTickCount64() + (ULONGLONG) (opt.seconds * 1000.0) : 0;
  MSG msg;
  int exitCode = 0;
  for (;;) {
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT) goto done;
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    if (opt.shotPath && GetTickCount64() >= deadline) {
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
    reportPlayed(app, opt.seconds);
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
  hub.index = app.outputDevice;
  hub.name = audio.deviceName();
  hub.open = audioOk;
  hub.inputIndex = app.inputOpen ? opt.inputDeviceIndex : -1;
  hub.learnNames();
  uint64_t uiTick = 0, nextDevicePoll = 0;
  attachPicker(inst, hub, uiTick, nextDevicePoll);
  inst->webview.createInContainer(window, inst->guiWidth, inst->guiHeight, clapwrap::uiHtml(),
                                  standaloneBridge(), kDesc.id);
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
// ctest; this interactive path (a real device, a real window) has not, because
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
    reportPlayed(app, opt.seconds);
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
  hub.index = app.outputDevice;
  hub.name = audio.deviceName();
  hub.open = audioOk;
  hub.inputIndex = app.inputOpen ? opt.inputDeviceIndex : -1;
  hub.learnNames();
  uint64_t uiTick = 0, nextDevicePoll = 0;
  attachPicker(inst, hub, uiTick, nextDevicePoll);
  inst->webview.create(content, inst->guiWidth, inst->guiHeight, clapwrap::uiHtml(),
                       standaloneBridge(), kDesc.id);

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

// A test may want the app WITHOUT its entry point -- tests/standalone_rt_test.cpp
// drives processBlock itself -- and says so with this macro.
#if !defined(SONORE_STANDALONE_NO_MAIN)
int main(int argc, char** argv) {
  using namespace sonore::standalone;
#if defined(_WIN32)
  // argv arrives in the ANSI code page, and every path in the SDK is UTF-8:
  // `--render C:\Музыка\take.wav` reached the file layer as question marks.
  // The command line is read again as UTF-16 and handed on as UTF-8.
  static std::vector<std::string> utf8Args;
  static std::vector<char*> utf8Argv;
  {
    int wideCount = 0;
    if (LPWSTR* wide = CommandLineToArgvW(GetCommandLineW(), &wideCount)) {
      for (int i = 0; i < wideCount; ++i) utf8Args.push_back(sonore::pathio::narrow(wide[i]));
      LocalFree(wide);
      for (std::string& a : utf8Args) utf8Argv.push_back(&a[0]);
      utf8Argv.push_back(nullptr);
      argc = wideCount;
      argv = utf8Argv.data();
    }
  }
#endif
  Options opt = parseOptions(argc, argv);
  if (opt.showHelp) {
    printUsage(argc > 0 ? argv[0] : nullptr);
    return 0;
  }
  // Said, not refused: a launcher may add arguments of its own, and the
  // program still does what the rest of the line asked.
  if (opt.unknown)
    std::fprintf(stderr, "[sonore] ignoring %s: not an option, or its value is missing or not a number it can use (--help lists them)\n",
                 opt.unknown);
  // A length is seconds a render holds in memory, a capture records and a
  // --play sleeps for. Unchecked, -1 and 1e12 crashed on an allocation, NaN
  // and inf grew memory without bound, and --play -1 slept for 49 days.
  if (!(opt.seconds > 0.0) || opt.seconds > 3600.0) {
    std::fprintf(stderr, "[sonore] a length must be more than 0 and at most 3600 seconds, not %g\n",
                 opt.seconds);
    return 2;
  }
  static App app; // static: the Instance carries large inline DSP state
  initialiseParams(app);

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
    FILE* f = ::sonore::pathio::open(opt.dumpUi, "wb");
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
    // Offline, the file's length IS the render's -- held in memory whole,
    // so it is held to the limit --seconds is. It was not: a 37-byte file
    // with one delta of 0x0FFFFFFF ticks plays for four years, and --render
    // and --verify died reserving 51 TB (0xC0000409), as --seconds 1e12 did
    // before it was checked. Live, a far event is simply never reached.
    if ((opt.renderPath || opt.verifyMode) && app.midiIn.duration() > 3600.0) {
      std::fprintf(stderr,
                   "[sonore] %s plays for %.0f seconds; a render is at most 3600 seconds\n",
                   opt.midiInPath, app.midiIn.duration());
      return 2;
    }
    std::printf("[sonore] playing %s: %zu events over %.1fs\n", opt.midiInPath,
                app.midiIn.size(), app.midiIn.duration());
  }

  if (opt.inputPath) {
    if (kDesc.isInstrument) {
      std::printf("[sonore] --input is for effects; %s is an instrument\n", kDesc.name);
      return 1;
    }
    // Offline modes run at 48 kHz and refuse another rate here; a live run
    // checks against the device's rate once it is open (see prepare()).
    const bool offline = opt.renderPath || opt.verifyMode || opt.capturePath;
    if (!app.fileSource.load(opt.inputPath, offline ? 48000.0 : 0.0)) {
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
    // Fed from the audio thread unless this run renders offline, on this
    // thread. A plain FileSink grows a vector on every send -- an allocation,
    // and a copy of every event so far, inside the device callback.
    if (!opt.renderPath) fileSink->startLive();
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
#endif // !SONORE_STANDALONE_NO_MAIN
