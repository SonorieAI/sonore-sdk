// SPDX-License-Identifier: Apache-2.0
// Compile au_wrapper.h. The compiler reading the file is the test this file
// was written for; main() also drives one host sequence, which runs wherever
// the file is BUILT -- on a Mac against the real frameworks, or here against
// au_shim with the two CoreFoundation calls the sequence makes stubbed and
// the rest left unresolved, since it never reaches them.
//
// au_wrapper.h had been edited for many weeks with no compiler ever looking at
// it: macOS CI is the only machine that could, and it has not started a job
// since the account's spending limit was reached. So "does this file parse"
// was genuinely unknown, and every blind edit stacked on the last.
//
// See au_shim/README.md for exactly what a green run here does and does not
// prove. Short version: internal consistency, not ABI correctness.
#include <sonore/dsp.h>
#include <sonore/plugin.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

namespace {
/** What the DSP was last prepared for, and the largest block it was handed. */
double g_preparedRate = 0.0;
int g_preparedBlock = 0;
int g_largestBlock = 0;
/** The notes it was handed. */
std::atomic<long> g_noteOns{0}, g_noteOffs{0};
} // namespace

struct SonoreDsp {
  void prepare(const sonore::ProcessSpec& spec) {
    g_preparedRate = spec.sampleRate;
    g_preparedBlock = (int) spec.maximumBlockSize;
  }
  void process(sonore::AudioBlock<float>& io, const float*, const sonore::MidiBuffer& midi) {
    if ((int) io.getNumSamples() > g_largestBlock) g_largestBlock = (int) io.getNumSamples();
    for (const auto& e : midi) {
      if (e.message.isNoteOn()) ++g_noteOns;
      else if (e.message.isNoteOff()) ++g_noteOffs;
    }
  }
};

#define SONORE_NUM_PARAMS 2
static const sonore::ParamInfo kParamTable[SONORE_NUM_PARAMS] = {
    {"gain", "Gain", "dB", -60.0f, 6.0f, 0.0f, 0},
    {"mode", "Mode", "", 0.0f, 2.0f, 0.0f, 3},
};

static const sonore::PluginDescriptor kDesc = {
    "com.sonorie.test.ausyntax",
    "AU Syntax",
    "Sonorie",
    "1.0.0",
    "A descriptor that exists so a compiler can read au_wrapper.h.",
    "https://sonorie.com",
    false,
    kParamTable,
    SONORE_NUM_PARAMS,
};

#include <sonore/clap_wrapper.h>
#include <sonore/au_wrapper.h>

/** A host changing the slice and the rate of an INITIALISED unit, which
 *  AUBase refuses. Taken, the DSP prepared for 512 frames at 44.1 kHz was
 *  handed 4096 frames, and the unit said 96 kHz. Returns the failures. */
static int changesWhileInitialised() {
  using namespace sonore::au;
  int failures = 0;
  auto check = [&failures](bool ok, const char* what) {
    std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
  };
  AudioComponentPlugInInterface* plugin = SonoreAUFactory(nullptr);
  if (!plugin) return 1;
  void* self = plugin;
  plugin->Open(self, nullptr);
  UInt32 slice = 512;
  Float64 rate = 44100.0;
  setProperty(self, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &slice,
              sizeof(slice));
  setProperty(self, kAudioUnitProperty_SampleRate, kAudioUnitScope_Output, 0, &rate, sizeof(rate));
  check(initialize(self) == noErr && g_preparedBlock == 512 && g_preparedRate == 44100.0,
        "initialised: the DSP is prepared at 44.1 kHz for 512 frames");

  UInt32 larger = 4096;
  const OSStatus sliceSet = setProperty(self, kAudioUnitProperty_MaximumFramesPerSlice,
                                        kAudioUnitScope_Global, 0, &larger, sizeof(larger));
  std::vector<float> left(4096), right(4096);
  struct {
    AudioBufferList list;
    AudioBuffer second;
  } io{};
  io.list.mNumberBuffers = 2;
  float* channels[2] = {left.data(), right.data()};
  for (int c = 0; c < 2; ++c) {
    io.list.mBuffers[c].mNumberChannels = 1;
    io.list.mBuffers[c].mDataByteSize = 4096 * (UInt32) sizeof(float);
    io.list.mBuffers[c].mData = channels[c];
  }
  AudioTimeStamp when{};
  AudioUnitRenderActionFlags flags = 0;
  const OSStatus rendered = render(self, &flags, &when, 0, 4096, &io.list);
  char line[200];
  std::snprintf(line, sizeof(line),
                "a larger slice set while initialised is refused (%d) and never reaches the DSP "
                "(render %d, largest block %d)",
                (int) sliceSet, (int) rendered, g_largestBlock);
  check(sliceSet == kAudioUnitErr_Initialized && g_largestBlock <= g_preparedBlock, line);

  Float64 faster = 96000.0, reported = 0.0;
  UInt32 size = sizeof(reported);
  const OSStatus rateSet = setProperty(self, kAudioUnitProperty_SampleRate, kAudioUnitScope_Output,
                                       0, &faster, sizeof(faster));
  getProperty(self, kAudioUnitProperty_SampleRate, kAudioUnitScope_Output, 0, &reported, &size);
  std::snprintf(line, sizeof(line),
                "a new rate set while initialised is refused (%d); the unit says %.0f Hz, the DSP "
                "is at %.0f Hz",
                (int) rateSet, reported, g_preparedRate);
  check(rateSet == kAudioUnitErr_PropertyNotWritable && reported == g_preparedRate, line);
  check(setProperty(self, kAudioUnitProperty_SampleRate, kAudioUnitScope_Output, 0, &rate,
                    sizeof(rate)) == noErr,
        "...while the rate it already runs at is still accepted");

  // The slice and the format it already has are no change, and AUBase takes
  // them while initialised: it compares before it asks whether the property
  // is writable. Both were refused.
  const OSStatus sameSlice = setProperty(self, kAudioUnitProperty_MaximumFramesPerSlice,
                                         kAudioUnitScope_Global, 0, &slice, sizeof(slice));
  AudioStreamBasicDescription format{};
  size = sizeof(format);
  getProperty(self, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &format, &size);
  const OSStatus sameFormat = setProperty(self, kAudioUnitProperty_StreamFormat,
                                          kAudioUnitScope_Output, 0, &format, sizeof(format));
  std::snprintf(line, sizeof(line),
                "...and so are the slice (%d) and the stream format (%d) it already has",
                (int) sameSlice, (int) sameFormat);
  check(sameSlice == noErr && sameFormat == noErr, line);
  AudioStreamBasicDescription other = format;
  other.mSampleRate = 96000.0;
  check(setProperty(self, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &other,
                    sizeof(other)) == kAudioUnitErr_PropertyNotWritable,
        "...while a different format is still refused");

  uninitialize(self);
  check(setProperty(self, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0,
                    &larger, sizeof(larger)) == noErr &&
            setProperty(self, kAudioUnitProperty_SampleRate, kAudioUnitScope_Output, 0, &faster,
                        sizeof(faster)) == noErr &&
            initialize(self) == noErr && g_preparedBlock == 4096 && g_preparedRate == 96000.0,
        "uninitialised, both are taken, and the next initialise prepares for them");
  plugin->Close(self);
  return failures;
}

namespace {
std::atomic<long> g_notified{0}, g_wrongNotify{0};
OSStatus notifyA(void* user, AudioUnitRenderActionFlags*, const AudioTimeStamp*, UInt32, UInt32,
                 AudioBufferList*) {
  if (user != (void*) 0xA) ++g_wrongNotify;
  ++g_notified;
  return noErr;
}
OSStatus notifyB(void* user, AudioUnitRenderActionFlags*, const AudioTimeStamp*, UInt32, UInt32,
                 AudioBufferList*) {
  if (user != (void*) 0xB) ++g_wrongNotify;
  ++g_notified;
  return noErr;
}
} // namespace

/** A host adding and removing render notifications on its own thread while
 *  the unit renders on another, which AUBase allows. Render read the list
 *  in place, and a remove shifting it under the loop had render call a
 *  cleared entry: an access violation within seconds. Returns the
 *  failures; a crash is the other way it fails. */
static int renderNotifiesWhileRendering() {
  using namespace sonore::au;
  AudioComponentPlugInInterface* plugin = SonoreAUFactory(nullptr);
  if (!plugin) return 1;
  void* self = plugin;
  plugin->Open(self, nullptr);
  UInt32 slice = 64;
  setProperty(self, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &slice,
              sizeof(slice));
  initialize(self);
  std::atomic<bool> done{false};
  std::thread audio([&] {
    std::vector<float> left(64), right(64);
    struct {
      AudioBufferList list;
      AudioBuffer second;
    } io{};
    io.list.mNumberBuffers = 2;
    float* channels[2] = {left.data(), right.data()};
    for (int c = 0; c < 2; ++c) {
      io.list.mBuffers[c].mNumberChannels = 1;
      io.list.mBuffers[c].mDataByteSize = 64 * (UInt32) sizeof(float);
      io.list.mBuffers[c].mData = channels[c];
    }
    AudioTimeStamp when{};
    while (!done.load()) {
      AudioUnitRenderActionFlags flags = 0;
      render(self, &flags, &when, 0, 64, &io.list);
    }
  });
  long cycles = 0;
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (std::chrono::steady_clock::now() < until) {
    addRenderNotify(self, &notifyA, (void*) 0xA);
    addRenderNotify(self, &notifyB, (void*) 0xB);
    removeRenderNotify(self, &notifyA, (void*) 0xA);
    removeRenderNotify(self, &notifyB, (void*) 0xB);
    ++cycles;
  }
  done.store(true);
  audio.join();
  uninitialize(self);
  plugin->Close(self);
  std::printf("  %s render notifications edited while rendering: %ld edits, %ld calls, %ld with "
              "another's user data\n",
              g_wrongNotify.load() == 0 && g_notified.load() > 0 ? "ok  " : "FAIL", cycles,
              g_notified.load(), g_wrongNotify.load());
  return g_wrongNotify.load() == 0 && g_notified.load() > 0 ? 0 : 1;
}

/** A host sending MIDI from a thread of its own while the unit renders on
 *  another, which Apple's MusicDeviceMIDIEvent contract allows (with an
 *  offset of 0). The events went straight into the block's buffer, which
 *  render reads and then clears: notes were lost between the two, and
 *  replayed when the writers' count and entries disagreed. Returns the
 *  failures. */
static int midiFromAnotherThread() {
  using namespace sonore::au;
  AudioComponentPlugInInterface* plugin = SonoreAUFactory(nullptr);
  if (!plugin) return 1;
  void* self = plugin;
  plugin->Open(self, nullptr);
  UInt32 slice = 64;
  setProperty(self, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &slice,
              sizeof(slice));
  initialize(self);
  std::atomic<bool> done{false};
  std::atomic<long> renders{0};
  std::vector<float> left(64), right(64);
  auto renderOnce = [&] {
    struct {
      AudioBufferList list;
      AudioBuffer second;
    } io{};
    io.list.mNumberBuffers = 2;
    float* channels[2] = {left.data(), right.data()};
    for (int c = 0; c < 2; ++c) {
      io.list.mBuffers[c].mNumberChannels = 1;
      io.list.mBuffers[c].mDataByteSize = 64 * (UInt32) sizeof(float);
      io.list.mBuffers[c].mData = channels[c];
    }
    AudioTimeStamp when{};
    AudioUnitRenderActionFlags flags = 0;
    render(self, &flags, &when, 0, 64, &io.list);
  };
  std::thread audio([&] {
    while (!done.load()) {
      renderOnce();
      ++renders;
    }
  });
  // Fifty pairs at a time, each batch only once two renders have passed since
  // the last: never a hundred events in flight, so nothing can fill, and
  // anything missing is the race rather than an overflow.
  constexpr long kPairs = 20000;
  for (long i = 0; i < kPairs; ++i) {
    if (i % 50 == 0) {
      const long seen = renders.load();
      while (renders.load() < seen + 2) {
      }
    }
    midiEvent(self, 0x90, 60, 100, 0);
    midiEvent(self, 0x80, 60, 0, 0);
  }
  const long seen = renders.load();
  while (renders.load() < seen + 2) {
  }
  done.store(true);
  audio.join();
  uninitialize(self);
  plugin->Close(self);
  const bool ok = g_noteOns.load() == kPairs && g_noteOffs.load() == kPairs;
  std::printf("  %s MIDI sent from another thread while rendering: %ld note-ons and %ld "
              "note-offs sent, %ld and %ld played\n",
              ok ? "ok  " : "FAIL", kPairs, kPairs, g_noteOns.load(), g_noteOffs.load());
  return ok ? 0 : 1;
}

namespace {
int g_heard = 0;
void heardIt(void*, AudioUnit, AudioUnitPropertyID, AudioUnitScope, AudioUnitElement) {
  ++g_heard;
}
} // namespace

/** A host taking its property listeners away. AUBase's
 *  AudioUnitRemovePropertyListener -- the call without user data -- removes
 *  every listener with that property and proc, whatever its user data, and
 *  the WithUserData call every one that matches all three. Here the first
 *  needed a null user data, which no host registers, and each removed one
 *  entry: a listener the host had taken away and freed was still called.
 *  Returns the failures. */
static int listenersRemoved() {
  using namespace sonore::au;
  AudioComponentPlugInInterface* plugin = SonoreAUFactory(nullptr);
  if (!plugin) return 1;
  void* self = plugin;
  plugin->Open(self, nullptr);
  const AudioUnitPropertyID slice = kAudioUnitProperty_MaximumFramesPerSlice;
  UInt32 frames = 256;
  auto heardAfterSet = [&] {
    g_heard = 0;
    setProperty(self, slice, kAudioUnitScope_Global, 0, &frames, sizeof(frames));
    return g_heard;
  };
  addPropertyListener(self, slice, &heardIt, (void*) 0xA);
  addPropertyListener(self, slice, &heardIt, (void*) 0xB);
  const int registered = heardAfterSet();
  removePropertyListener(self, slice, &heardIt);
  const int afterOldRemove = heardAfterSet();
  addPropertyListener(self, slice, &heardIt, (void*) 0xA);
  addPropertyListener(self, slice, &heardIt, (void*) 0xA);
  addPropertyListener(self, slice, &heardIt, (void*) 0xB);
  removePropertyListenerWithUserData(self, slice, &heardIt, (void*) 0xA);
  const int afterRemoveA = heardAfterSet();
  plugin->Close(self);
  const bool ok = registered == 2 && afterOldRemove == 0 && afterRemoveA == 1;
  std::printf("  %s listeners removed as AUBase removes them: two registered heard %d time(s); "
              "after the call without user data %d (want 0); after removing 0xA, registered "
              "twice beside 0xB, %d (want 1)\n",
              ok ? "ok  " : "FAIL", registered, afterOldRemove, afterRemoveA);
  return ok ? 0 : 1;
}

int main() {
  const int failures = changesWhileInitialised() + renderNotifiesWhileRendering() +
                       midiFromAnotherThread() + listenersRemoved();
  std::printf("%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
}
