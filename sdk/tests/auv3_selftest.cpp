// SPDX-License-Identifier: Apache-2.0
// The AUv3 wrapper, driven the way a host drives it: through AUAudioUnit's
// public API and nothing of the wrapper's own.
//
// Linked with one example plugin compiled with SONORE_BUILD_AUV3. The plugin's
// unit class is registered in this process (SonoreAUv3RegisterInProcess, as
// the iOS standalone registers it) and then made, configured and rendered as a
// host would: bus formats set, render resources allocated, the RENDER block
// called with a pull-input block, parameters and MIDI scheduled through the
// blocks AUAudioUnit hands out, the parameter tree read and written, the
// state saved and restored, a factory preset chosen, the effect bypassed.
//
// Runs on macOS (ctest, on CI's Mac) and in the iOS simulator (simctl spawn,
// on CI's iOS job). What it cannot reach: a real host's process boundary,
// which auval covers on the Mac by loading the built .appex.
//
//   auv3_selftest --effect|--instrument --params N [--presets N] [--midi-out]
//                 [--type aufx --subtype Sat3 --manufacturer Sonr]
#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <objc/message.h>
#include <objc/runtime.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <sonore/apple_block.h>

extern "C" Class SonoreAUv3RegisterInProcess(const AudioComponentDescription* desc, const char* name,
                                             uint32_t version);

namespace {

int g_checks = 0, g_failed = 0;

void check(bool ok, const char* what) {
  ++g_checks;
  if (!ok) ++g_failed;
  std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
}

template <typename... A>
void checkf(bool ok, const char* format, A... args) {
  char text[512];
  std::snprintf(text, sizeof(text), format, args...);
  check(ok, text);
}

template <typename Ret, typename... Args>
Ret send(id target, const char* selector, Args... args) {
  using Fn = Ret (*)(id, SEL, Args...);
  return reinterpret_cast<Fn>(objc_msgSend)(target, sel_registerName(selector), args...);
}

id cls(const char* name) { return (id) objc_getClass(name); }

id str(const char* utf8) {
  CFStringRef s = CFStringCreateWithCString(nullptr, utf8, kCFStringEncodingUTF8);
  return (id) CFAutorelease(s);
}

std::string utf8(id s) {
  if (!s) return {};
  char text[512] = {};
  CFStringGetCString((CFStringRef) s, text, sizeof(text), kCFStringEncodingUTF8);
  return text;
}

OSType fourCC(const char* s) {
  return ((OSType) (uint8_t) s[0] << 24) | ((OSType) (uint8_t) s[1] << 16) | ((OSType) (uint8_t) s[2] << 8) |
         (OSType) (uint8_t) s[3];
}

struct BlockHeader {
  void* isa;
  int flags;
  int reserved;
  void* invoke;
};

template <typename Ret, typename... Args>
Ret callBlock(void* block, Args... args) {
  using Fn = Ret (*)(void*, Args...);
  return reinterpret_cast<Fn>(static_cast<BlockHeader*>(block)->invoke)(block, args...);
}

constexpr int64_t kImmediate = (int64_t) 0xffffffff00000000LL;
constexpr uint32_t kFrames = 256;
constexpr double kRate = 48000.0;

/** The effect's input: a 220 Hz sine at -6 dBFS, continuous across blocks. */
struct Source {
  double phase = 0.0;
};

using PullInvoke = int32_t (*)(void* block, AudioUnitRenderActionFlags* flags, const AudioTimeStamp* ts,
                               uint32_t frames, long bus, AudioBufferList* in);

int32_t pullSine(void* block, AudioUnitRenderActionFlags*, const AudioTimeStamp*, uint32_t frames, long bus,
                 AudioBufferList* in) {
  Source* src = sonore::detail::blockCapture<Source*, PullInvoke>(block);
  const double step = 2.0 * 3.14159265358979323846 * 220.0 / kRate;
  double phase = src->phase;
  for (uint32_t i = 0; i < frames; ++i) {
    const float v = bus == 0 ? (float) (0.5 * std::sin(phase)) : 0.0f;
    for (UInt32 c = 0; c < in->mNumberBuffers; ++c)
      if (in->mBuffers[c].mData) ((float*) in->mBuffers[c].mData)[i] = v;
    phase += step;
  }
  if (bus == 0) src->phase = phase;
  return 0;
}

/** What the unit sends through the MIDI output block a host gives it. */
struct MidiCounter {
  int events = 0;
  int noteOns = 0;
};

using MidiOutInvoke = int32_t (*)(void* block, int64_t time, uint8_t cable, long length, const uint8_t* bytes);

int32_t countMidi(void* block, int64_t, uint8_t, long length, const uint8_t* bytes) {
  MidiCounter* counter = sonore::detail::blockCapture<MidiCounter*, MidiOutInvoke>(block);
  ++counter->events;
  if (length >= 3 && bytes && (bytes[0] & 0xF0) == 0x90 && bytes[2] > 0) ++counter->noteOns;
  return 0;
}

struct Harness {
  id unit = nil;
  void* renderBlock = nullptr;
  bool effect = true;
  Source source;
  sonore::detail::BlockLiteral<Source*, PullInvoke> pull{};
  std::vector<float> left = std::vector<float>(kFrames), right = std::vector<float>(kFrames);
  double sampleTime = 0.0;

  /** One block; true when the unit answered noErr. Peak and RMS of the left. */
  bool render(float* peak = nullptr, double* rms = nullptr, bool* finite = nullptr) {
    struct {
      AudioBufferList list;
      AudioBuffer second;
    } out{};
    out.list.mNumberBuffers = 2;
    out.list.mBuffers[0] = {1, kFrames * (UInt32) sizeof(float), left.data()};
    out.list.mBuffers[1] = {1, kFrames * (UInt32) sizeof(float), right.data()};
    AudioTimeStamp ts{};
    ts.mSampleTime = sampleTime;
    ts.mFlags = 1; // kAudioTimeStampSampleTimeValid
    AudioUnitRenderActionFlags flags = 0;
    using RenderFn = int32_t (*)(void*, AudioUnitRenderActionFlags*, const AudioTimeStamp*, uint32_t, long,
                                 AudioBufferList*, void*);
    const int32_t status = reinterpret_cast<RenderFn>(static_cast<BlockHeader*>(renderBlock)->invoke)(
        renderBlock, &flags, &ts, kFrames, 0, &out.list, effect ? (void*) &pull : nullptr);
    sampleTime += kFrames;
    const float* l = (const float*) out.list.mBuffers[0].mData;
    float p = 0.0f;
    double sum = 0.0;
    bool ok = true;
    for (uint32_t i = 0; i < kFrames && l; ++i) {
      if (!std::isfinite(l[i])) ok = false;
      p = std::fmax(p, std::fabs(l[i]));
      sum += (double) l[i] * l[i];
    }
    if (peak) *peak = p;
    if (rms) *rms = std::sqrt(sum / kFrames);
    if (finite) *finite = ok;
    return status == 0;
  }
};

id parameterAt(id unit, uint64_t address) {
  id tree = send<id>(unit, "parameterTree");
  return tree ? send<id>(tree, "parameterWithAddress:", address) : nil;
}

bool setBusFormat(id busArray, unsigned long index, uint32_t channels) {
  if (!busArray || send<unsigned long>(busArray, "count") <= index) return false;
  id bus = send<id>(busArray, "objectAtIndexedSubscript:", index);
  id format = send<id>(send<id>(cls("AVAudioFormat"), "alloc"), "initStandardFormatWithSampleRate:channels:",
                       kRate, channels);
  id error = nil;
  const BOOL ok = send<BOOL>(bus, "setFormat:error:", format, &error);
  send<void>(format, "release");
  return ok != NO;
}

} // namespace

int main(int argc, char** argv) {
  bool effect = true, midiOut = false;
  int params = -1, presets = -1;
  const char* type = nullptr;
  const char* subtype = "Tst3";
  const char* manufacturer = "Sonr";
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--instrument")) effect = false;
    else if (!std::strcmp(argv[i], "--effect")) effect = true;
    else if (!std::strcmp(argv[i], "--midi-out")) midiOut = true;
    else if (!std::strcmp(argv[i], "--params") && i + 1 < argc) params = std::atoi(argv[++i]);
    else if (!std::strcmp(argv[i], "--presets") && i + 1 < argc) presets = std::atoi(argv[++i]);
    else if (!std::strcmp(argv[i], "--type") && i + 1 < argc) type = argv[++i];
    else if (!std::strcmp(argv[i], "--subtype") && i + 1 < argc) subtype = argv[++i];
    else if (!std::strcmp(argv[i], "--manufacturer") && i + 1 < argc) manufacturer = argv[++i];
  }
  if (!type) type = effect ? "aufx" : "aumu";
  id pool = send<id>(send<id>(cls("NSAutoreleasePool"), "alloc"), "init");
  std::printf("AUv3 self-test (%s)\n", effect ? "effect" : "instrument");

  AudioComponentDescription desc{fourCC(type), fourCC(subtype), fourCC(manufacturer), 0, 0};
  Class unitClass = SonoreAUv3RegisterInProcess(&desc, "Sonorie: AUv3 self-test", 0x10000);
  check(unitClass != nullptr, "the unit's class registers in process");
  if (!unitClass) return 1;

  // ── Made as a host makes it ──
  Harness h;
  h.effect = effect;
  id error = nil;
  using Init = id (*)(id, SEL, AudioComponentDescription, uint32_t, id*);
  h.unit = reinterpret_cast<Init>(objc_msgSend)(send<id>((id) unitClass, "alloc"),
                                                sel_registerName("initWithComponentDescription:options:error:"),
                                                desc, (uint32_t) 0, &error);
  check(h.unit != nil, "initWithComponentDescription: makes a unit");
  if (!h.unit) return 1;

  id outputs = send<id>(h.unit, "outputBusses");
  id inputs = send<id>(h.unit, "inputBusses");
  check(outputs && send<unsigned long>(outputs, "count") >= 1, "at least one output bus");
  checkf(inputs && (send<unsigned long>(inputs, "count") >= 1) == effect, "%s",
         effect ? "an effect has an input bus" : "an instrument has none");

  id tree = send<id>(h.unit, "parameterTree");
  id all = tree ? send<id>(tree, "allParameters") : nil;
  const long count = all ? (long) send<unsigned long>(all, "count") : -1;
  if (params >= 0) checkf(count == params, "the tree holds the descriptor's %d parameters (%ld)", params, count);
  else check(count > 0, "the tree holds parameters");

  id p0 = parameterAt(h.unit, 0);
  check(p0 != nil, "parameter 0 is found by its address");
  if (p0) {
    const float lo = send<float>(p0, "minValue"), hi = send<float>(p0, "maxValue");
    checkf(hi > lo, "parameter 0 has a range (%g..%g)", lo, hi);
    id text = send<id>(p0, "stringFromValue:", (const float*) nullptr);
    checkf(!utf8(text).empty(), "parameter 0 has display text (\"%s\")", utf8(text).c_str());
    const float mid = lo + 0.25f * (hi - lo);
    send<void>(p0, "setValue:", mid);
    checkf(std::fabs(send<float>(p0, "value") - mid) <= 1e-3f * (hi - lo) + 1e-6f,
           "a value set on the parameter reads back (%g)", mid);
  }

  // ── Configured and allocated ──
  check(setBusFormat(outputs, 0, 2), "the output bus takes 48 kHz stereo");
  if (effect) check(setBusFormat(inputs, 0, 2), "the input bus takes 48 kHz stereo");
  // Nine channels is past every layout the SDK has (eight at most): the bus
  // asks the unit (shouldChangeToFormat:forBus:), which refuses it.
  check(!setBusFormat(outputs, 0, 9), "a nine-channel output bus is refused");
  setBusFormat(outputs, 0, 2);
  send<void>(h.unit, "setMaximumFramesToRender:", (uint32_t) 512);
  // A host that takes the unit's MIDI sets its block before allocating.
  MidiCounter midiCounter;
  sonore::detail::BlockLiteral<MidiCounter*, MidiOutInvoke> midiOutBlock{};
  if (midiOut) {
    sonore::detail::makeGlobalBlock(midiOutBlock, &countMidi, &midiCounter);
    send<void>(h.unit, "setMIDIOutputEventBlock:", (void*) &midiOutBlock);
  }
  id allocError = nil;
  const BOOL allocated = send<BOOL>(h.unit, "allocateRenderResourcesAndReturnError:", &allocError);
  check(allocated != NO, "render resources allocate");
  if (!allocated) return 1;
  check(send<BOOL>(h.unit, "renderResourcesAllocated") != NO, "and the unit says so");
  h.renderBlock = send<void*>(h.unit, "renderBlock");
  check(h.renderBlock != nullptr, "the unit hands out its render block");
  sonore::detail::makeGlobalBlock(h.pull, &pullSine, &h.source);
  checkf(send<double>(h.unit, "latency") >= 0.0, "latency reads (%g s)", send<double>(h.unit, "latency"));

  // ── Rendered ──
  if (effect) {
    float peak = 0.0f;
    bool finite = true, ok = true;
    for (int b = 0; b < 40; ++b) ok = h.render(&peak, nullptr, &finite) && ok;
    check(ok, "forty blocks render without an error");
    checkf(finite && peak > 0.01f, "the effect passes its input through its DSP (peak %.3f)", peak);
  } else if (midiOut) {
    // A note effect: a held chord comes back out as notes, through the
    // host's block, and no audio.
    void* midi = send<void*>(h.unit, "scheduleMIDIEventBlock");
    check(midi != nullptr, "the unit hands out a MIDI schedule block");
    for (int note : {60, 64, 67}) {
      const uint8_t on[3] = {0x90, (uint8_t) note, 100};
      if (midi) callBlock<void>(midi, kImmediate, (uint8_t) 0, (long) 3, on);
    }
    bool ok = true;
    for (int b = 0; b < 400 && midiCounter.noteOns < 2; ++b) ok = h.render() && ok;
    check(ok, "the note effect renders without an error");
    checkf(midiCounter.noteOns >= 2, "a held chord comes out of the MIDI output (%d note-ons, %d events)",
           midiCounter.noteOns, midiCounter.events);
  } else {
    void* midi = send<void*>(h.unit, "scheduleMIDIEventBlock");
    check(midi != nullptr, "the unit hands out a MIDI schedule block");
    float before = 0.0f;
    h.render(&before);
    checkf(before < 1e-4f, "silent before a note (peak %.5f)", before);
    if (midi) {
      const uint8_t on[3] = {0x90, 60, 100};
      callBlock<void>(midi, kImmediate, (uint8_t) 0, (long) 3, on);
    }
    float peak = 0.0f;
    bool finite = true;
    for (int b = 0; b < 20; ++b) {
      float p = 0.0f;
      h.render(&p, nullptr, &finite);
      peak = std::fmax(peak, p);
    }
    checkf(finite && peak > 0.001f, "a scheduled note-on sounds (peak %.4f)", peak);
    if (midi) {
      const uint8_t off[3] = {0x80, 60, 0};
      callBlock<void>(midi, kImmediate, (uint8_t) 0, (long) 3, off);
    }
    float tail = 1.0f;
    for (int b = 0; b < 1500 && tail > 1e-4f; ++b) h.render(&tail);
    checkf(tail <= 1e-4f, "the note-off lets it fall silent (peak %.5f)", tail);
  }

  // ── A scheduled parameter change lands in the tree's value ──
  if (p0) {
    void* schedule = send<void*>(h.unit, "scheduleParameterBlock");
    check(schedule != nullptr, "the unit hands out a parameter schedule block");
    const float lo = send<float>(p0, "minValue"), hi = send<float>(p0, "maxValue");
    const float target = lo + 0.75f * (hi - lo);
    if (schedule) callBlock<void>(schedule, kImmediate, (uint32_t) 0, (uint64_t) 0, target);
    h.render();
    checkf(std::fabs(send<float>(p0, "value") - target) <= 1e-3f * (hi - lo) + 1e-6f,
           "a scheduled change reaches the parameter (%g)", target);
  }

  // ── State ──
  id state = send<id>(h.unit, "fullState");
  check(state != nil, "fullState is a dictionary");
  if (state) {
    id data = send<id>(state, "objectForKey:", str("SonoreState"));
    check(data != nil, "it carries the SNRS blob every format writes");
    id typeNumber = send<id>(state, "objectForKey:", str("type"));
    checkf(typeNumber && (OSType) send<int>(typeNumber, "intValue") == desc.componentType,
           "and the unit's type, as AUv2's ClassInfo does");
    send<void>(state, "retain");
  }
  if (p0 && state) {
    const float saved = send<float>(p0, "value");
    send<void>(p0, "setValue:", send<float>(p0, "minValue"));
    send<void>(h.unit, "setFullState:", state);
    checkf(std::fabs(send<float>(p0, "value") - saved) <= 1e-4f * std::fabs(saved) + 1e-5f,
           "setFullState: brings the saved value back (%g)", saved);
  }
  if (state) send<void>(state, "release");
  {
    // Another plugin's dictionary is refused, the state kept.
    id foreign = send<id>(cls("NSMutableDictionary"), "dictionary");
    send<void>(foreign, "setObject:forKey:", send<id>(cls("NSNumber"), "numberWithInt:", (int) fourCC("XXXX")),
               str("subtype"));
    const float before = p0 ? send<float>(p0, "value") : 0.0f;
    send<void>(h.unit, "setFullState:", foreign);
    check(!p0 || send<float>(p0, "value") == before, "a dictionary that is another plugin's is not applied");
  }

  // ── Factory presets ──
  id factory = send<id>(h.unit, "factoryPresets");
  const long numPresets = factory ? (long) send<unsigned long>(factory, "count") : -1;
  if (presets >= 0) checkf(numPresets == presets, "%d factory presets are listed (%ld)", presets, numPresets);
  if (numPresets > 1) {
    id preset = send<id>(factory, "objectAtIndexedSubscript:", (unsigned long) 1);
    send<void>(h.unit, "setCurrentPreset:", preset);
    id current = send<id>(h.unit, "currentPreset");
    checkf(current && send<long>(current, "number") == 1, "choosing factory preset 1 makes it current (\"%s\")",
           utf8(send<id>(preset, "name")).c_str());
  }

  // ── Bypass: the dry signal, after the crossfade ──
  if (effect) {
    send<void>(h.unit, "setShouldBypassEffect:", (BOOL) YES);
    check(send<BOOL>(h.unit, "shouldBypassEffect") != NO, "the bypass reads back on");
    for (int b = 0; b < 20; ++b) h.render(); // past the 20 ms crossfade
    // A 0.5-peak sine is 0.354 RMS, whatever the latency delays it by --
    // measured over eight blocks, nine periods, where one block's part of a
    // period would move it by a few percent.
    double sum = 0.0;
    for (int b = 0; b < 8; ++b) {
      double rms = 0.0;
      h.render(nullptr, &rms);
      sum += rms * rms;
    }
    const double rms = std::sqrt(sum / 8.0);
    checkf(std::fabs(rms - 0.5 / std::sqrt(2.0)) < 0.005, "bypassed, the input comes out unchanged (rms %.4f)", rms);
    send<void>(h.unit, "setShouldBypassEffect:", (BOOL) NO);
  }

  check(send<BOOL>(h.unit, "canProcessInPlace") != NO, "it processes in place");
  if (midiOut) {
    id names = send<id>(h.unit, "MIDIOutputNames");
    check(names && send<unsigned long>(names, "count") == 1, "a MIDI-producing plugin names one MIDI output");
  }
  checkf(send<BOOL>(h.unit, "providesUserInterface") != NO, "it says it has an editor");

  send<void>(h.unit, "deallocateRenderResources");
  send<void>(h.unit, "release");
  check(true, "released after deallocating, without a crash");
  send<void>(pool, "drain");

  std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
  std::printf("%s\n", g_failed ? "AUV3 SELF-TEST FAILED" : "AUV3 SELF-TEST PASSED");
  return g_failed ? 1 : 0;
}
