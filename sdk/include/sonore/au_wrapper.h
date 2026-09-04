// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: the Audio Unit (AUv2) format wrapper, for macOS.
//
// Compiled and validated on macOS since 2026-09-01: the SDK workflow
// (`.github/workflows/sdk.yml`, a macos-14 runner) builds every AU here and
// Apple's `auval` passes all six examples with zero warnings: instantiation,
// parameters, render, state, bypass and latency, each through the selectors
// below. What each round of fixes taught is kept as a comment where it
// applies.
//
// Written against the C AudioComponent API rather than Apple's AUBase C++
// classes, because AUBase was removed from modern Xcode and dragging in a copy
// would re-create the framework dependency this SDK exists to avoid. A modern
// AUv2 is a factory function returning an AudioComponentPlugInInterface whose
// Lookup() hands back an implementation per selector: plain C function
// pointers, which is the same shape CLAP and VST3 already use here.
//
// Include it AFTER clap_wrapper.h: that header owns the shared machinery (the
// DSP instance, parameters, state, the webview bridge) and this one adapts it,
// so one source file builds .clap, .vst3 and .component without drift.
#pragma once

// macOS only in a build. There is ONE other way in: SONORE_APPLE_SYNTAX_CHECK,
// which lets a compiler on any platform read this file against a shim of
// Apple's declarations.
//
// That is not a substitute for building on a Mac and it does not pretend to
// be -- it proves this file is internally consistent, not that its ABI is
// right. But this file had been edited for weeks without a compiler ever
// looking at it, and "does it parse" was genuinely unknown.
#if !defined(__APPLE__) && !defined(SONORE_APPLE_SYNTAX_CHECK)
#error "au_wrapper.h is macOS only"
#endif

#include <AudioToolbox/AudioToolbox.h>
#include <CoreMIDI/CoreMIDI.h>
#include <dlfcn.h>
#include <AudioUnit/AudioUnit.h>
#include <CoreFoundation/CoreFoundation.h>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <new>

#include "audio.h"
#include "gui.h"
#include "plugin.h"
#include "presets.h"
#include "sysex.h"

#ifndef SONORE_NUM_PARAMS
#error "Define SONORE_NUM_PARAMS (and struct SonoreDsp) before including au_wrapper.h"
#endif

/** Forward declaration: getProperty locates the bundle from this symbol. */
extern "C" AudioComponentPlugInInterface* SonoreAUFactory(const AudioComponentDescription*);

namespace sonore {
namespace au {

using clapwrap::Instance;

/** How many buses a scope has. ONE definition, because the element count a
 *  host reads and the elements a host may then address have to be the same
 *  answer -- reporting two inputs and refusing input 1 is a contract a host
 *  cannot work with. */
inline UInt32 elementCountFor(AudioUnitScope scope) {
  switch (scope) {
    case kAudioUnitScope_Global: return 1;
    case kAudioUnitScope_Output: return 1 + clapwrap::numAuxOutputs();
    case kAudioUnitScope_Input:
      if (kDesc.isInstrument) return 0;
      return clapwrap::TakesSidechain<SonoreDsp>::value ? 2 : 1;
    default: return 0;
  }
}

/** AU's scope/element pair addresses buses. Anything past the counts above is
 *  a request we cannot honour and must decline rather than ignore. */
inline bool validScope(AudioUnitScope scope, AudioUnitElement element) {
  return element < elementCountFor(scope);
}

/** Most of what an AU publishes belongs to the unit, not to a bus: latency,
 *  tail, bypass, presets, the editor. Answering for Input/0 as well is not
 *  harmless breadth -- a host that believes a per-bus latency exists will ask
 *  for it per bus and get the same number back, and auval says so plainly:
 *  "returning valid information for scope/element for [Input/0], which should
 *  be invalid". */
inline bool globalOnly(AudioUnitScope scope, AudioUnitElement element) {
  return scope == kAudioUnitScope_Global && element == 0;
}

struct Unit {
  AudioComponentPlugInInterface plugin{};
  Instance shared;

  /** The HOST's AudioUnit handle for this instance, captured at Open. Needed
   *  whenever we talk to the host about ourselves (AUParameterSet, listener
   *  callbacks): our own pointer is not a handle the host ever issued. */
  AudioUnit hostInstance = nullptr;

  /** Held across calls: MusicDeviceSysEx does not promise a whole message,
   *  and reassembly that reset every call would emit nothing for exactly the
   *  long messages worth having. */
  SysexAssembler sysex;

  /** Our own type/subtype/manufacturer, read back from the component the host
   *  opened. The class-info dictionary is REQUIRED to carry them -- it is the
   *  same dictionary a .aupreset file holds, and a host restoring one has to
   *  be able to tell whether the preset belongs to this plugin at all. */
  AudioComponentDescription componentDesc{};

  double sampleRate = 44100.0;
  UInt32 maxFrames = 1156; // AU's traditional default slice
  UInt32 chans = 2;        // the width the DSP runs at, fixed at initialize()

  /** The widths the host has DECLARED on the main input and output buses.
   *  They are separate because a host sets them separately, and holding one
   *  number for both meant whichever format arrived last silently won: auval
   *  set input 3 / output 1, we initialised happily, and it reported
   *  "Initialised Unit incorrectly set to InputChans:3 OutputChans:1" -- a
   *  configuration this plugin never advertised. */
  UInt32 mainInChans = 2;
  UInt32 mainOutChans = 2;
  bool initialised = false;

  /** The host's input callback, for effects. An AU pulls its input rather than
   *  being handed it, which is the single biggest structural difference from
   *  CLAP and VST3. */
  /** Input render callbacks, one per input element: 0 = main, 1 = sidechain.
   *  AUs PULL their input, and a sidechain is simply a second element the
   *  host wires its key signal into. */
  AURenderCallbackStruct inputCallback[2]{};
  bool hasInputCallback[2] = {false, false};
  /** The graph's way of feeding the same input: instead of a callback, a host
   *  names the unit and output bus to pull from. AUv2 hosts and AUGraph use
   *  this, and we implemented only the callback -- so auval's "Make connection
   *  to AU" got kAudioUnitErr_InvalidProperty from a property every effect is
   *  expected to accept. Setting one CLEARS the other, as AUBase does: two
   *  live sources for one input is not a state a host can have meant. */
  AudioUnitConnection inputConnection[2]{};
  bool hasInputConnection[2] = {false, false};
  std::vector<float> scScratch[2]; // pulled sidechain (or stays silent)

  /** Output storage for the case where a host hands us an AudioBufferList with
   *  NULL mData. In AU's pull model that is not an error -- it means "render
   *  into your own buffer and tell me where it is" -- and AUBase keeps exactly
   *  such a buffer. Sized at initialize(), never in render(). */
  std::vector<float> outScratch;
  /** Aux output buses. An AU renders ONE element per call, so bus 0 computes
   *  everything into here and the later calls for the same timestamp copy
   *  their slice out -- the same thing AUBase does for multi-output units. */
  std::vector<float> auxScratch[clapwrap::kMaxAuxOutputs * clapwrap::kMaxAudioChannels];
  Float64 auxRenderedAt = -1.0;
  /** Where a host wants emitted MIDI delivered, if it asked for any. */
  AUMIDIOutputCallbackStruct midiOutCallback{};
  bool hasMidiOutCallback = false;

  /** Property listeners the host registered, so parameter and preset changes
   *  can be announced. */
  struct Listener {
    AudioUnitPropertyID property = 0;
    AudioUnitPropertyListenerProc proc = nullptr;
    void* userData = nullptr;
  };
  static constexpr int kMaxListeners = 16;
  Listener listeners[kMaxListeners]{};
  int numListeners = 0;

  /** RENDER notifications: a host watching every block go past, before and
   *  after. Metering strips, tap points and auval all use them, and the two
   *  selectors were simply missing from the dispatch table -- so the framework
   *  answered -4 (unimpErr) on behalf of a plugin that had never been asked.
   *  Fixed capacity for the same reason the property listeners have one: this
   *  list is READ on the audio thread. */
  struct RenderNotify {
    AURenderCallback proc = nullptr;
    void* userData = nullptr;
  };
  static constexpr int kMaxRenderNotifies = 8;
  RenderNotify renderNotifies[kMaxRenderNotifies]{};
  int numRenderNotifies = 0;

  AUPreset currentPreset{};
};

/** The block OUR factory allocates. Apple's public headers stopped shipping
 *  AudioComponentPlugInInstance with the classic CoreAudio SDK (macOS CI's
 *  first compile of this file proved it) -- but the only ABI the system cares
 *  about is that the returned pointer's first bytes are the
 *  AudioComponentPlugInInterface. The rest of the block is ours; the layout
 *  mirrors Apple's so the hard-won addressing rule stays true: the instance
 *  storage begins AT the mInstanceStorage field. */
struct AudioComponentPlugInInstance {
  AudioComponentPlugInInterface PlugInInterface;
  void* mConstruct;
  void* mDestruct;
  void* mPad[2];
  UInt32 mInstanceStorage;
};

/** THE ONE WAY to reach the instance from a dispatched call. Apple's dispatch
 *  does `(AUBase*)&ACPI->mInstanceStorage`, and so must every one of our
 *  methods: `self` points at the BLOCK, and the Unit lives at an offset inside
 *  it. Open constructed it there and every other method used a bare
 *  `(Unit*) self` -- forty bytes early, on top of the interface pointers. auval
 *  is what finally said so out loud: the sample rate read 0 Hz, the current
 *  preset came back nameless and numbered 0, and the render test aborted, all
 *  of which are one object being written at one address and read at another.
 *  Global-scope parameters passed throughout, because those answers come from
 *  the static descriptor and never touch the instance -- which is exactly why
 *  a wrapper can look healthy while pointing at the wrong memory. */
inline Unit* unitOf(void* self) {
  return (Unit*) &((AudioComponentPlugInInstance*) self)->mInstanceStorage;
}


inline void notifyListeners(Unit* unit, AudioUnitPropertyID property) {
  for (int i = 0; i < unit->numListeners; ++i) {
    const auto& l = unit->listeners[i];
    if (l.property == property && l.proc)
      l.proc(l.userData, unit->hostInstance, property, kAudioUnitScope_Global, 0);
  }
}

// ── Parameters ───────────────────────────────────────────────────────────────
// AU speaks PLAIN values, like ours: no normalisation, unlike VST3. The unit
// enum is the only translation, and picking the wrong one makes a host display
// "3.5 dB" for a frequency.

inline AudioUnitParameterUnit unitFor(const ParamInfo& p) {
  if (!p.unit || !p.unit[0]) return kAudioUnitParameterUnit_Generic;
  if (std::strcmp(p.unit, "dB") == 0) return kAudioUnitParameterUnit_Decibels;
  if (std::strcmp(p.unit, "Hz") == 0) return kAudioUnitParameterUnit_Hertz;
  if (std::strcmp(p.unit, "%") == 0) return kAudioUnitParameterUnit_Percent;
  if (std::strcmp(p.unit, "s") == 0) return kAudioUnitParameterUnit_Seconds;
  if (std::strcmp(p.unit, "ms") == 0) return kAudioUnitParameterUnit_Milliseconds;
  return kAudioUnitParameterUnit_CustomUnit;
}

inline OSStatus getParameter(void* self, AudioUnitParameterID id, AudioUnitScope scope,
                             AudioUnitElement element, AudioUnitParameterValue* value) {
  auto* unit = unitOf(self);
  if (scope != kAudioUnitScope_Global || element != 0 || !value) return kAudioUnitErr_InvalidScope;
  if (id >= (AudioUnitParameterID) SONORE_NUM_PARAMS) return kAudioUnitErr_InvalidParameter;
  *value = unit->shared.params[id];
  return noErr;
}

inline OSStatus setParameter(void* self, AudioUnitParameterID id, AudioUnitScope scope,
                             AudioUnitElement element, AudioUnitParameterValue value, UInt32) {
  auto* unit = unitOf(self);
  if (scope != kAudioUnitScope_Global || element != 0) return kAudioUnitErr_InvalidScope;
  if (id >= (AudioUnitParameterID) SONORE_NUM_PARAMS) return kAudioUnitErr_InvalidParameter;
  unit->shared.params[id] = clampToRange(kDesc.params[id], value);
  return noErr;
}

inline OSStatus scheduleParameters(void* self, const AudioUnitParameterEvent* events,
                                   UInt32 numEvents) {
  auto* unit = unitOf(self);
  for (UInt32 i = 0; i < numEvents; ++i) {
    const auto& e = events[i];
    if (e.parameter >= (AudioUnitParameterID) SONORE_NUM_PARAMS) continue;
    // Ramped automation is applied at its END value: block granularity, which
    // is what the toolkit's smoothers exist to make inaudible.
    const float v = e.eventType == kParameterEvent_Immediate
                        ? e.eventValues.immediate.value
                        : e.eventValues.ramp.endValue;
    unit->shared.params[e.parameter] = clampToRange(kDesc.params[e.parameter], v);
  }
  return noErr;
}

/** Private property: hands the Cocoa view factory the Unit* behind a host's
 *  AudioUnit handle. That handle is an opaque wrapper owned by the
 *  AudioComponent framework, CASTING it is a crash, so the view asks the
 *  instance itself, through the one channel a host guarantees to route:
 *  AudioUnitGetProperty. Custom property IDs live at 64000 and above. */
constexpr AudioUnitPropertyID kSonorePropertyUnitPointer = 64901;

/** The Cocoa view factory's class name, unique to THIS binary.
 *
 *  The name is handed to the host through kAudioUnitProperty_CocoaUI and
 *  registered with the Objective-C runtime by au_view.h; both read it here
 *  so they cannot disagree. Unique per image, because the runtime has one
 *  class namespace per process and two plugins built from different SDK
 *  versions would otherwise share one class whose methods live in whichever
 *  binary registered first -- see cocoa::uniqueClassName. */
inline const char* auViewFactoryClassName() {
  static char name[96] = {0};
  if (!name[0])
    std::snprintf(name, sizeof(name), "SonoreAUViewFactory_%llx",
                  (unsigned long long) (uintptr_t) (void*) &auViewFactoryClassName);
  return name;
}

// ── Properties ───────────────────────────────────────────────────────────────

inline void fillStreamFormat(AudioStreamBasicDescription* format, double sampleRate,
                             UInt32 channels) {
  std::memset(format, 0, sizeof(*format));
  format->mSampleRate = sampleRate;
  format->mFormatID = kAudioFormatLinearPCM;
  // Non-interleaved float is what every AU host expects and what our planar DSP
  // already speaks; interleaving would mean a copy per block for nothing.
  format->mFormatFlags = kAudioFormatFlagsNativeFloatPacked | kAudioFormatFlagIsNonInterleaved;
  format->mBytesPerPacket = sizeof(float);
  format->mFramesPerPacket = 1;
  format->mBytesPerFrame = sizeof(float);
  format->mChannelsPerFrame = channels;
  format->mBitsPerChannel = 32;
}

inline OSStatus getPropertyInfo(void* self, AudioUnitPropertyID property, AudioUnitScope scope,
                                AudioUnitElement element, UInt32* outSize, Boolean* outWritable) {
  auto* unit = unitOf(self);
  UInt32 size = 0;
  Boolean writable = false;

  switch (property) {
    case kAudioUnitProperty_StreamFormat:
      if (!validScope(scope, element)) return kAudioUnitErr_InvalidScope;
      size = sizeof(AudioStreamBasicDescription);
      // Writable only while UNINITIALISED, as AUBase is: prepare() has already
      // been given a width and a rate, and changing them underneath it would
      // leave the DSP sized for a format the host is no longer sending.
      writable = !unit->initialised;
      break;
    case kAudioUnitProperty_SampleRate:
      if (!validScope(scope, element)) return kAudioUnitErr_InvalidScope;
      size = sizeof(Float64);
      writable = true;
      break;
    case kAudioUnitProperty_MaximumFramesPerSlice:
      if (!globalOnly(scope, element)) return kAudioUnitErr_InvalidScope;
      size = sizeof(UInt32);
      writable = true;
      break;
    case kAudioUnitProperty_ParameterList:
      // Every parameter we have is global. Reporting the same list on the
      // input and output scopes told auval there were four parameters there,
      // it asked AudioUnitGetParameter for each, and got InvalidScope back
      // four times -- a contradiction we published ourselves. A scope with no
      // parameters answers with an EMPTY list, which is what AUBase does.
      size = globalOnly(scope, element)
                 ? (UInt32) (sizeof(AudioUnitParameterID) * SONORE_NUM_PARAMS)
                 : 0;
      break;
    case kAudioUnitProperty_ParameterInfo:
      size = sizeof(AudioUnitParameterInfo);
      break;
    case kAudioUnitProperty_ParameterClumpName:
      if (!globalOnly(scope, element)) return kAudioUnitErr_InvalidScope;
      size = sizeof(AudioUnitParameterNameInfo);
      break;
    case kAudioUnitProperty_ElementCount:
      size = sizeof(UInt32);
      break;
    case kAudioUnitProperty_Latency:
      if (!globalOnly(scope, element)) return kAudioUnitErr_InvalidScope;
      size = sizeof(Float64);
      break;
    case kAudioUnitProperty_TailTime:
      if (!globalOnly(scope, element)) return kAudioUnitErr_InvalidScope;
      size = sizeof(Float64);
      break;
    case kAudioUnitProperty_BypassEffect:
      if (kDesc.isInstrument) return kAudioUnitErr_InvalidProperty;
      if (!globalOnly(scope, element)) return kAudioUnitErr_InvalidScope;
      size = sizeof(UInt32);
      writable = true;
      break;
    case kAudioUnitProperty_MIDIOutputCallbackInfo:
      if (!kDesc.producesMidi) return kAudioUnitErr_InvalidProperty;
      if (!globalOnly(scope, element)) return kAudioUnitErr_InvalidScope;
      size = sizeof(CFArrayRef);
      break;
    case kAudioUnitProperty_MIDIOutputCallback:
      if (!kDesc.producesMidi) return kAudioUnitErr_InvalidProperty;
      if (!globalOnly(scope, element)) return kAudioUnitErr_InvalidScope;
      size = sizeof(AUMIDIOutputCallbackStruct);
      writable = true;
      break;
    case kAudioUnitProperty_SupportedNumChannels:
      if (!globalOnly(scope, element)) return kAudioUnitErr_InvalidScope;
      size = sizeof(AUChannelInfo) *
             (clapwrap::maxMainChannels() - clapwrap::minMainChannels() + 1);
      break;
    case kAudioUnitProperty_SetRenderCallback:
      if (kDesc.isInstrument) return kAudioUnitErr_InvalidProperty;
      if (scope != kAudioUnitScope_Input) return kAudioUnitErr_InvalidScope;
      size = sizeof(AURenderCallbackStruct);
      writable = true;
      break;
    case kAudioUnitProperty_MakeConnection:
      if (kDesc.isInstrument) return kAudioUnitErr_InvalidProperty;
      if (!validScope(scope, element) || scope != kAudioUnitScope_Input)
        return kAudioUnitErr_InvalidScope;
      size = sizeof(AudioUnitConnection);
      writable = true;
      break;
    case kAudioUnitProperty_FactoryPresets:
      if (kDesc.numPresets <= 0) return kAudioUnitErr_InvalidProperty;
      if (!globalOnly(scope, element)) return kAudioUnitErr_InvalidScope;
      size = sizeof(CFArrayRef);
      break;
    case kAudioUnitProperty_PresentPreset:
      if (!globalOnly(scope, element)) return kAudioUnitErr_InvalidScope;
      size = sizeof(AUPreset);
      writable = true;
      break;
    case kAudioUnitProperty_ClassInfo:
      if (!globalOnly(scope, element)) return kAudioUnitErr_InvalidScope;
      size = sizeof(CFPropertyListRef);
      writable = true;
      break;
    case kSonorePropertyUnitPointer:
      if (!globalOnly(scope, element)) return kAudioUnitErr_InvalidScope;
      size = sizeof(void*);
      break;
    case kAudioUnitProperty_CocoaUI:
      if (!globalOnly(scope, element)) return kAudioUnitErr_InvalidScope;
      size = sizeof(AudioUnitCocoaViewInfo);
      break;
    default:
      return kAudioUnitErr_InvalidProperty;
  }
  (void) unit;
  if (outSize) *outSize = size;
  if (outWritable) *outWritable = writable;
  return noErr;
}

/** The plugin's state as a CFDictionary, which is what AU persists. The bytes
 *  inside are the SAME versioned `SNRS` blob the other formats write, so one
 *  product has one state format however it was loaded. */
inline CFPropertyListRef buildClassInfo(Unit* unit) {
  CFMutableDictionaryRef dict = CFDictionaryCreateMutable(
      nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

  // The four identity fields are not optional: auval refuses the dictionary
  // outright ("Class Data does not have required field:<type> ==
  // componentType") and a host cannot tell one plugin's .aupreset from
  // another's without them.
  auto putNumber = [dict](const char* key, SInt32 value) {
    CFNumberRef ref = CFNumberCreate(nullptr, kCFNumberSInt32Type, &value);
    CFStringRef k = CFStringCreateWithCString(nullptr, key, kCFStringEncodingUTF8);
    CFDictionarySetValue(dict, k, ref);
    CFRelease(k);
    CFRelease(ref);
  };
  putNumber(kAUPresetVersionKey, (SInt32) clapwrap::kStateVersion);
  putNumber(kAUPresetTypeKey, (SInt32) unit->componentDesc.componentType);
  putNumber(kAUPresetSubtypeKey, (SInt32) unit->componentDesc.componentSubType);
  putNumber(kAUPresetManufacturerKey, (SInt32) unit->componentDesc.componentManufacturer);

  // The name is what a host shows in its preset menu. Without it the current
  // preset reads as an unnamed slot 0, whatever the unit actually has loaded.
  CFStringRef presetName = unit->currentPreset.presetName;
  CFDictionarySetValue(dict, CFSTR(kAUPresetNameKey),
                       presetName ? presetName : CFSTR("Default"));

  // The FULL versioned SNRS blob, built by the one shared serializer -- params,
  // bypass, selected preset, editor size AND the DSP's StateBag -- so a
  // sampler's loaded file survives a Logic/Live session. This used to store
  // only the params array plus a bypass flag, silently dropping the bag (and
  // the preset and the editor size) that every other format keeps; the doc
  // comment above claimed the SNRS blob while the code wrote neither the bag
  // nor the header. One CFData now, the same bytes CLAP/VST3/LV2 write.
  std::vector<uint8_t> blob;
  clapwrap::saveStateBody(&unit->shared, [&blob](const void* d, size_t n) -> size_t {
    const auto* p = (const uint8_t*) d;
    blob.insert(blob.end(), p, p + n);
    return n;
  });
  CFDataRef data = CFDataCreate(nullptr, blob.empty() ? nullptr : blob.data(),
                                (CFIndex) blob.size());
  CFDictionarySetValue(dict, CFSTR("SonoreState"), data);
  CFRelease(data);
  return dict;
}

inline OSStatus applyClassInfo(Unit* unit, CFPropertyListRef plist) {
  if (!plist || CFGetTypeID(plist) != CFDictionaryGetTypeID()) return kAudioUnitErr_InvalidPropertyValue;
  auto dict = (CFDictionaryRef) plist;

  // A class-info dictionary is a .aupreset, and a .aupreset carries whose it
  // is. Loading another plugin's file would apply that plugin's parameter
  // values to ours by index -- plausible numbers, wrong meaning -- so the
  // identity is checked before a byte of state is read. A dictionary that
  // simply omits the fields is accepted: our own older blobs did.
  auto matches = [dict](const char* key, OSType mine) {
    CFStringRef k = CFStringCreateWithCString(nullptr, key, kCFStringEncodingUTF8);
    auto number = (CFNumberRef) CFDictionaryGetValue(dict, k);
    CFRelease(k);
    if (!number || CFGetTypeID(number) != CFNumberGetTypeID()) return true;
    SInt32 theirs = 0;
    CFNumberGetValue(number, kCFNumberSInt32Type, &theirs);
    return (OSType) theirs == mine;
  };
  if (!matches(kAUPresetTypeKey, unit->componentDesc.componentType) ||
      !matches(kAUPresetSubtypeKey, unit->componentDesc.componentSubType) ||
      !matches(kAUPresetManufacturerKey, unit->componentDesc.componentManufacturer))
    return kAudioUnitErr_InvalidPropertyValue;

  auto data = (CFDataRef) CFDictionaryGetValue(dict, CFSTR("SonoreState"));
  if (!data || CFGetTypeID(data) != CFDataGetTypeID()) return kAudioUnitErr_InvalidPropertyValue;

  const UInt8* bytes = CFDataGetBytePtr(data);
  const CFIndex length = CFDataGetLength(data);
  // The same shared parser every other format uses: params, bypass, preset,
  // editor size and the StateBag, tolerant of a changed param count and handing
  // an empty bag to a DSP whose blob predates bags. Walk the CFData by offset;
  // the get callback returns 0 (EOF) once the bag has consumed the rest, which
  // is how the serializer knows the bag ended.
  size_t offset = 0;
  clapwrap::loadStateBody(&unit->shared,
                          [bytes, length, &offset](void* dst, size_t want) -> size_t {
                            const size_t remaining = (size_t) length - offset;
                            const size_t n = want < remaining ? want : remaining;
                            if (n) std::memcpy(dst, bytes + offset, n);
                            offset += n;
                            return n;
                          });
  unit->shared.uiEchoValid = false;
  return noErr;
}

/** Fills the buffer. The size check is its CALLER's job -- see getProperty. */
inline OSStatus getPropertyBody(void* self, AudioUnitPropertyID property, AudioUnitScope scope,
                                AudioUnitElement element, void* outData) {
  auto* unit = unitOf(self);
  if (!outData) return kAudioUnitErr_InvalidPropertyValue;

  switch (property) {
    case kAudioUnitProperty_StreamFormat:
      if (!validScope(scope, element)) return kAudioUnitErr_InvalidScope;
      // An AUX output has its OWN declared width -- reporting the main bus's
      // for all of them would tell a host to wire a stereo band split into a
      // mono bus, or the reverse. Element 0 of any scope is the main bus.
      fillStreamFormat((AudioStreamBasicDescription*) outData, unit->sampleRate,
                       (scope == kAudioUnitScope_Output && element > 0)
                           ? clapwrap::auxBusChannels(element - 1)
                           : (scope == kAudioUnitScope_Input ? unit->mainInChans
                                                             : unit->mainOutChans));
      return noErr;

    case kAudioUnitProperty_SampleRate:
      *(Float64*) outData = unit->sampleRate;
      return noErr;

    case kAudioUnitProperty_MaximumFramesPerSlice:
      *(UInt32*) outData = unit->maxFrames;
      return noErr;

    case kAudioUnitProperty_ElementCount:
      *(UInt32*) outData = elementCountFor(scope);
      return noErr;

    case kAudioUnitProperty_ParameterList: {
      if (!globalOnly(scope, element)) return noErr; // an empty list, as promised
      auto* ids = (AudioUnitParameterID*) outData;
      for (int i = 0; i < SONORE_NUM_PARAMS; ++i) ids[i] = (AudioUnitParameterID) i;
      return noErr;
    }

    case kAudioUnitProperty_ParameterClumpName: {
      // inID carries the clump the host wants named; ids are 1-based.
      auto* clump = (AudioUnitParameterNameInfo*) outData;
      const GroupTable groups = collectGroups(kDesc.params, kDesc.numParams);
      const int index = (int) clump->inID - 1;
      if (index < 0 || index >= groups.count) return kAudioUnitErr_InvalidPropertyValue;
      clump->outName =
          CFStringCreateWithCString(nullptr, groups.names[index], kCFStringEncodingUTF8);
      return noErr;
    }

    case kAudioUnitProperty_ParameterInfo: {
      // The host passes the id in as the element.
      if (element >= (AudioUnitElement) kDesc.numParams) return kAudioUnitErr_InvalidParameter;
      const ParamInfo& p = kDesc.params[element];
      auto* info = (AudioUnitParameterInfo*) outData;
      std::memset(info, 0, sizeof(*info));
      std::snprintf(info->name, sizeof(info->name), "%s", p.label);
      info->cfNameString = CFStringCreateWithCString(nullptr, p.label, kCFStringEncodingUTF8);
      info->unit = unitFor(p);
      if (info->unit == kAudioUnitParameterUnit_CustomUnit)
        info->unitName = CFStringCreateWithCString(nullptr, p.unit, kCFStringEncodingUTF8);
      info->minValue = p.minValue;
      info->maxValue = p.maxValue;
      info->defaultValue = p.defaultValue;
      info->flags = kAudioUnitParameterFlag_IsReadable |
                    kAudioUnitParameterFlag_HasCFNameString |
                    kAudioUnitParameterFlag_CFNameRelease;
      // AU has one flag where CLAP and VST3 have two: writable or not.
      //
      //   readOnly     maps exactly -- drop IsWritable and the host displays
      //                the value without offering to set it.
      //   automatable  has no equivalent. AU hosts automate anything
      //                writable, so a parameter that says it should not be
      //                automated stays writable and is automated anyway.
      //                Refusing to make it writable would be worse: the user
      //                could no longer change it either.
      //   hidden       has no equivalent either; AU's parameter list is the
      //                whole list.
      if (!p.readOnly) info->flags |= kAudioUnitParameterFlag_IsWritable;
      // AU has display CURVES rather than an exponent: the host picks the
      // shape from a flag. Logarithmic is the one that matches a skew below
      // one, which is every frequency and time control there is. A skew
      // ABOVE one has no AU flag and falls back to linear rather than being
      // approximated by the wrong curve.
      if (p.skew > 0.0f && p.skew < 1.0f)
        info->flags |= kAudioUnitParameterFlag_DisplayLogarithmic;
      // AU groups parameters into numbered "clumps"; the host asks for each
      // clump's name separately. Clump 0 means ungrouped, so ids start at 1.
      {
        const GroupTable groups = collectGroups(kDesc.params, kDesc.numParams);
        const int g = groups.indexOf(p.group);
        if (g >= 0) {
          info->clumpID = (UInt32) (g + 1);
          info->flags |= kAudioUnitParameterFlag_HasClump;
        }
      }
      return noErr;
    }

    case kAudioUnitProperty_Latency:
      *(Float64*) outData =
          unit->sampleRate > 0.0
              ? (Float64) clapwrap::dspLatency(unit->shared.dsp) / unit->sampleRate
              : 0.0;
      return noErr;

    case kAudioUnitProperty_TailTime:
      *(Float64*) outData = unit->sampleRate > 0.0
                                ? (Float64) clapwrap::dspTail(unit->shared.dsp) / unit->sampleRate
                                : 0.0;
      return noErr;

    case kAudioUnitProperty_BypassEffect:
      if (kDesc.isInstrument) return kAudioUnitErr_InvalidProperty;
      *(UInt32*) outData = unit->shared.bypass.engaged ? 1 : 0;
      return noErr;

    case kAudioUnitProperty_MIDIOutputCallbackInfo: {
      // The names of the MIDI streams we emit; a host lists these for routing.
      if (!kDesc.producesMidi) return kAudioUnitErr_InvalidProperty;
      CFStringRef name = CFSTR("MIDI Out");
      CFArrayRef array = CFArrayCreate(nullptr, (const void**) &name, 1,
                                       &kCFTypeArrayCallBacks);
      *(CFArrayRef*) outData = array; // the host owns it from here
      return noErr;
    }

    case kAudioUnitProperty_SupportedNumChannels: {
      // One entry per supported width; auval reads exactly the byte count the
      // property-info promised, so the two must agree.
      auto* info = (AUChannelInfo*) outData;
      int i = 0;
      for (uint32_t n = clapwrap::minMainChannels(); n <= clapwrap::maxMainChannels();
           ++n, ++i) {
        info[i].inChannels = kDesc.isInstrument ? 0 : (SInt16) n;
        info[i].outChannels = (SInt16) n;
      }
      return noErr;
    }

    case kAudioUnitProperty_FactoryPresets: {
      if (kDesc.numPresets <= 0) return kAudioUnitErr_InvalidProperty;
      // The convention is a CFArray of AUPreset POINTERS: hosts cast each
      // element straight to (const AUPreset*). The structs therefore live in
      // process-lifetime static storage, and the array stores raw pointers
      // (null callbacks). An earlier version wrapped them in CFData, which
      // hands the host a pointer to the WRAPPER: misread as a preset.
      static const AUPreset* presets = [] {
        auto* list = new AUPreset[kDesc.numPresets];
        for (int i = 0; i < kDesc.numPresets; ++i) {
          list[i].presetNumber = i;
          list[i].presetName = CFStringCreateWithCString(nullptr, kDesc.presets[i].name,
                                                         kCFStringEncodingUTF8);
        }
        return (const AUPreset*) list;
      }();
      CFMutableArrayRef array = CFArrayCreateMutable(nullptr, kDesc.numPresets, nullptr);
      if (!array) return kAudioUnitErr_InvalidPropertyValue;
      for (int i = 0; i < kDesc.numPresets; ++i) CFArrayAppendValue(array, &presets[i]);
      *(CFArrayRef*) outData = array; // the host releases the array
      return noErr;
    }

    case kAudioUnitProperty_PresentPreset:
      *(AUPreset*) outData = unit->currentPreset;
      if (unit->currentPreset.presetName) CFRetain(unit->currentPreset.presetName);
      return noErr;

    case kAudioUnitProperty_ClassInfo:
      *(CFPropertyListRef*) outData = buildClassInfo(unit);
      return noErr;

    case kSonorePropertyUnitPointer:
      *(Unit**) outData = unit;
      return noErr;

    case kAudioUnitProperty_CocoaUI: {
      // The editor is an NSView factory class registered at runtime by
      // au_view.h; naming it here is what makes a host open our webview.
      // The bundle URL must be REAL: hosts CFRetain and resolve it, and a
      // null crashes them, so it is recovered from this module's own path:
      // .../Foo.component/Contents/MacOS/Foo, three levels above the binary.
      Dl_info dl{};
      if (!dladdr((const void*) &SonoreAUFactory, &dl) || !dl.dli_fname)
        return kAudioUnitErr_InvalidProperty;
      CFURLRef url = CFURLCreateFromFileSystemRepresentation(
          nullptr, (const UInt8*) dl.dli_fname, (CFIndex) std::strlen(dl.dli_fname), false);
      for (int up = 0; url && up < 3; ++up) {
        CFURLRef parent = CFURLCreateCopyDeletingLastPathComponent(nullptr, url);
        CFRelease(url);
        url = parent;
      }
      if (!url) return kAudioUnitErr_InvalidProperty;
      auto* info = (AudioUnitCocoaViewInfo*) outData;
      info->mCocoaAUViewBundleLocation = url; // the host releases it
      info->mCocoaAUViewClass[0] =
          CFStringCreateWithCString(nullptr, auViewFactoryClassName(), kCFStringEncodingUTF8);
      return noErr;
    }

    default:
      return kAudioUnitErr_InvalidProperty;
  }
}

/** kAudioUnitGetPropertySelect. The size argument is not decoration: the
 *  contract is Apple's own DoGetProperty -- ask what the property actually
 *  needs, REFUSE a buffer that cannot hold it, and report the real size back on
 *  success. Ours took five arguments where the selector passes six, so
 *  ioDataSize was never read and never written: a host asking for eight bytes
 *  of a sixteen-byte property was handed sixteen and told it had asked
 *  correctly. auval calls that "GetProperty() call accepts & returns bad data
 *  size", and it said it about Latency, Tail Time and Bypass Effect alike --
 *  one defect, three symptoms. */
inline OSStatus getProperty(void* self, AudioUnitPropertyID property, AudioUnitScope scope,
                            AudioUnitElement element, void* outData, UInt32* ioDataSize) {
  if (!ioDataSize) return kAudioUnitErr_InvalidPropertyValue;
  UInt32 needed = 0;
  Boolean writable = false;
  const OSStatus info = getPropertyInfo(self, property, scope, element, &needed, &writable);
  if (info != noErr) return info;
  // A caller may ask for the SIZE ALONE by passing a null buffer; that is not
  // an error, and answering it is how a host learns what to allocate.
  if (!outData) {
    *ioDataSize = needed;
    return noErr;
  }
  if (*ioDataSize < needed) return kAudioUnitErr_InvalidPropertyValue;
  const OSStatus result = getPropertyBody(self, property, scope, element, outData);
  if (result == noErr) *ioDataSize = needed;
  return result;
}

inline OSStatus setProperty(void* self, AudioUnitPropertyID property, AudioUnitScope scope,
                            AudioUnitElement element, const void* inData, UInt32 size) {
  auto* unit = unitOf(self);

  switch (property) {
    case kAudioUnitProperty_StreamFormat: {
      if (!validScope(scope, element) || !inData || size < sizeof(AudioStreamBasicDescription))
        return kAudioUnitErr_InvalidPropertyValue;
      if (unit->initialised) return kAudioUnitErr_PropertyNotWritable;
      const auto* format = (const AudioStreamBasicDescription*) inData;
      // Float LPCM only -- accepting anything else would mean silently
      // mis-reading the host's buffers. The WIDTH, though, is whatever the
      // descriptor allows: hardcoding stereo here contradicted the channel
      // list this same wrapper publishes, so a 1..8 plugin advertised eight
      // layouts and then refused seven of them.
      if (format->mFormatID != kAudioFormatLinearPCM ||
          !(format->mFormatFlags & kAudioFormatFlagIsFloat))
        return kAudioUnitErr_FormatNotSupported;
      const UInt32 want = format->mChannelsPerFrame;
      const bool auxBus = scope == kAudioUnitScope_Output && element > 0;
      if (auxBus) {
        if (want != clapwrap::auxBusChannels(element - 1))
          return kAudioUnitErr_FormatNotSupported;
      } else if (want < clapwrap::minMainChannels() || want > clapwrap::maxMainChannels()) {
        return kAudioUnitErr_FormatNotSupported;
      }
      // A rate of zero is not a request, it is a missing answer -- and an AU
      // that stores one then reports it back describes a format no host can
      // render. Refuse it rather than carry it.
      if (!(format->mSampleRate > 0.0)) return kAudioUnitErr_FormatNotSupported;
      unit->sampleRate = format->mSampleRate;
      // The main bus width follows the format; the sidechain (element 1) is a
      // key input and does not decide how wide the plugin runs.
      // Record what the host declared, per bus. Which of them the plugin can
      // actually RUN at is settled at initialize(), where both are known --
      // deciding here would mean judging a pair on half the evidence.
      // A sidechain is a key input and an aux output is a fixed-width tap;
      // neither is the main bus.
      if (auxBus) {
        // nothing to record: an aux width is fixed by the descriptor
      } else if (scope == kAudioUnitScope_Input) {
        if (element == 0) unit->mainInChans = want;
      } else {
        unit->mainOutChans = want;
      }
      notifyListeners(unit, kAudioUnitProperty_StreamFormat);
      return noErr;
    }

    case kAudioUnitProperty_SampleRate:
      if (!inData || size < sizeof(Float64)) return kAudioUnitErr_InvalidPropertyValue;
      if (!(*(const Float64*) inData > 0.0)) return kAudioUnitErr_InvalidPropertyValue;
      unit->sampleRate = *(const Float64*) inData;
      notifyListeners(unit, kAudioUnitProperty_SampleRate);
      return noErr;

    case kAudioUnitProperty_MaximumFramesPerSlice:
      if (!inData || size < sizeof(UInt32)) return kAudioUnitErr_InvalidPropertyValue;
      unit->maxFrames = *(const UInt32*) inData;
      // A property a host can SET is a property a host can be WATCHING, and an
      // editor sizing its meters to the block length has no other way to hear
      // about this one. Only PresentPreset ever announced itself, which auval
      // states plainly: "Changing max frames property did not fire off a
      // property changed notification".
      notifyListeners(unit, kAudioUnitProperty_MaximumFramesPerSlice);
      return noErr;

    case kAudioUnitProperty_SetRenderCallback:
      if (kDesc.isInstrument) return kAudioUnitErr_InvalidProperty;
      if (!inData || size < sizeof(AURenderCallbackStruct))
        return kAudioUnitErr_InvalidPropertyValue;
      {
        const UInt32 el = element < 2 ? element : 0;
        unit->inputCallback[el] = *(const AURenderCallbackStruct*) inData;
        unit->hasInputCallback[el] = unit->inputCallback[el].inputProc != nullptr;
        unit->hasInputConnection[el] = false; // one source per input
      }
      return noErr;

    case kAudioUnitProperty_MakeConnection: {
      if (kDesc.isInstrument) return kAudioUnitErr_InvalidProperty;
      if (scope != kAudioUnitScope_Input) return kAudioUnitErr_InvalidScope;
      if (!inData || size < sizeof(AudioUnitConnection))
        return kAudioUnitErr_InvalidPropertyValue;
      const auto* conn = (const AudioUnitConnection*) inData;
      // destInputNumber is the element the host means; the scope element is
      // the same thing, and a host may set either. Trust the struct.
      const UInt32 el = conn->destInputNumber < 2 ? conn->destInputNumber : element;
      if (el >= elementCountFor(kAudioUnitScope_Input)) return kAudioUnitErr_InvalidElement;
      unit->inputConnection[el] = *conn;
      // A null source is how a host DISCONNECTS. Treating it as a connection
      // would make the next render pull from nothing.
      unit->hasInputConnection[el] = conn->sourceAudioUnit != nullptr;
      if (unit->hasInputConnection[el]) unit->hasInputCallback[el] = false;
      return noErr;
    }

    case kAudioUnitProperty_PresentPreset: {
      if (!inData || size < sizeof(AUPreset)) return kAudioUnitErr_InvalidPropertyValue;
      const auto* preset = (const AUPreset*) inData;
      if (preset->presetNumber >= 0 && preset->presetNumber < kDesc.numPresets &&
          kDesc.presets) {
        const Preset& p = kDesc.presets[preset->presetNumber];
        if (p.numValues != kDesc.numParams) return kAudioUnitErr_InvalidPropertyValue;
        for (int i = 0; i < kDesc.numParams; ++i)
          unit->shared.params[i] = clampToRange(kDesc.params[i], p.values[i]);
      }
      if (unit->currentPreset.presetName) CFRelease(unit->currentPreset.presetName);
      unit->currentPreset = *preset;
      if (unit->currentPreset.presetName) CFRetain(unit->currentPreset.presetName);
      unit->shared.uiEchoValid = false;
      notifyListeners(unit, kAudioUnitProperty_PresentPreset);
      return noErr;
    }

    case kAudioUnitProperty_BypassEffect:
      if (kDesc.isInstrument) return kAudioUnitErr_InvalidProperty;
      if (!inData || size < sizeof(UInt32)) return kAudioUnitErr_InvalidPropertyValue;
      unit->shared.bypass.engaged = *(const UInt32*) inData != 0;
      notifyListeners(unit, kAudioUnitProperty_BypassEffect);
      return noErr;

    case kAudioUnitProperty_MIDIOutputCallback:
      if (!kDesc.producesMidi) return kAudioUnitErr_InvalidProperty;
      if (!inData || size < sizeof(AUMIDIOutputCallbackStruct))
        return kAudioUnitErr_InvalidPropertyValue;
      unit->midiOutCallback = *(const AUMIDIOutputCallbackStruct*) inData;
      unit->hasMidiOutCallback = unit->midiOutCallback.midiOutputCallback != nullptr;
      return noErr;

    case kAudioUnitProperty_ClassInfo:
      if (!inData || size < sizeof(CFPropertyListRef)) return kAudioUnitErr_InvalidPropertyValue;
      return applyClassInfo(unit, *(const CFPropertyListRef*) inData);

    default:
      return kAudioUnitErr_InvalidProperty;
  }
}

inline OSStatus addPropertyListener(void* self, AudioUnitPropertyID property,
                                    AudioUnitPropertyListenerProc proc, void* userData) {
  auto* unit = unitOf(self);
  if (unit->numListeners >= Unit::kMaxListeners) return kAudioUnitErr_TooManyFramesToProcess;
  unit->listeners[unit->numListeners++] = {property, proc, userData};
  return noErr;
}

inline OSStatus removePropertyListenerWithUserData(void* self, AudioUnitPropertyID property,
                                                   AudioUnitPropertyListenerProc proc,
                                                   void* userData) {
  auto* unit = unitOf(self);
  for (int i = 0; i < unit->numListeners; ++i) {
    const auto& l = unit->listeners[i];
    if (l.property == property && l.proc == proc && l.userData == userData) {
      unit->listeners[i] = unit->listeners[--unit->numListeners];
      return noErr;
    }
  }
  return noErr;
}

inline OSStatus removePropertyListener(void* self, AudioUnitPropertyID property,
                                       AudioUnitPropertyListenerProc proc) {
  return removePropertyListenerWithUserData(self, property, proc, nullptr);
}

inline OSStatus addRenderNotify(void* self, AURenderCallback proc, void* userData) {
  auto* unit = unitOf(self);
  if (!proc) return kAudioUnitErr_InvalidParameter;
  for (int i = 0; i < unit->numRenderNotifies; ++i)
    if (unit->renderNotifies[i].proc == proc && unit->renderNotifies[i].userData == userData)
      return noErr; // already registered; adding twice would notify twice
  if (unit->numRenderNotifies >= Unit::kMaxRenderNotifies) return kAudioUnitErr_InvalidParameter;
  unit->renderNotifies[unit->numRenderNotifies++] = {proc, userData};
  return noErr;
}

inline OSStatus removeRenderNotify(void* self, AURenderCallback proc, void* userData) {
  auto* unit = unitOf(self);
  for (int i = 0; i < unit->numRenderNotifies; ++i) {
    if (unit->renderNotifies[i].proc != proc || unit->renderNotifies[i].userData != userData)
      continue;
    for (int j = i; j + 1 < unit->numRenderNotifies; ++j)
      unit->renderNotifies[j] = unit->renderNotifies[j + 1];
    unit->renderNotifies[--unit->numRenderNotifies] = {};
    return noErr;
  }
  return noErr;
}

/** Fire the render notifications for one phase. The flags word is the host's
 *  own: it tells each callback WHICH phase this is. */
inline void notifyRender(Unit* unit, AudioUnitRenderActionFlags phase,
                         const AudioTimeStamp* timestamp, UInt32 bus, UInt32 frames,
                         AudioBufferList* io) {
  for (int i = 0; i < unit->numRenderNotifies; ++i) {
    AudioUnitRenderActionFlags flags = phase;
    unit->renderNotifies[i].proc(unit->renderNotifies[i].userData, &flags, timestamp, bus, frames,
                                 io);
  }
}

// ── Lifecycle and rendering ──────────────────────────────────────────────────

inline OSStatus initialize(void* self) {
  auto* unit = unitOf(self);

  // Cleared FIRST. Initialize can fail below, and a unit whose initialisation
  // failed is not an initialised unit -- leaving the flag set from an earlier
  // successful call is what made auval report "Initialised Unit incorrectly
  // set to InputChans:3 OutputChans:1" about a call we had just refused.
  unit->initialised = false;

  // REFUSE a configuration we never advertised. Our AUChannelInfo says the
  // input and output widths are equal (N in, N out, for N in the descriptor's
  // range), so an asymmetric pair has to be declined HERE -- initialize is the
  // only moment both buses are known, and accepting one leaves a host
  // rendering into a bus the DSP will not fill.
  const UInt32 out = unit->mainOutChans;
  if (out < clapwrap::minMainChannels() || out > clapwrap::maxMainChannels())
    return kAudioUnitErr_FormatNotSupported;
  if (!kDesc.isInstrument && unit->mainInChans != out)
    return kAudioUnitErr_FormatNotSupported;
  unit->chans = out;

  ProcessSpec spec;
  spec.sampleRate = unit->sampleRate;
  spec.maximumBlockSize = unit->maxFrames;
  spec.numChannels = unit->chans;
  unit->shared.dsp.prepare(spec); // the one place allocation is allowed
  unit->shared.bypass.prepare(unit->sampleRate, unit->maxFrames,
                              clapwrap::dspLatency(unit->shared.dsp), unit->chans);
  // Reserved here so render() never has to allocate for a host that passes a
  // null-buffer AudioBufferList. Full width, because the widths a host may ask
  // for are known now and a resize later would be the malloc this avoids.
  unit->outScratch.assign((size_t) clapwrap::kMaxAudioChannels * unit->maxFrames, 0.0f);
  unit->initialised = true;
  return noErr;
}

inline OSStatus uninitialize(void* self) {
  unitOf(self)->initialised = false;
  return noErr;
}

inline OSStatus reset(void* self, AudioUnitScope, AudioUnitElement) {
  auto* unit = unitOf(self);
  unit->shared.midi.clear();
  ProcessSpec spec;
  spec.sampleRate = unit->sampleRate;
  spec.maximumBlockSize = unit->maxFrames;
  spec.numChannels = unit->chans;
  unit->shared.dsp.prepare(spec); // prepare() IS our reset
  unit->shared.bypass.prepare(unit->sampleRate, unit->maxFrames,
                              clapwrap::dspLatency(unit->shared.dsp), unit->chans);
  return noErr;
}

/** Apply UI edits on the audio thread. The host was already told from the main
 *  thread (see au_view.h), so this only has to reach the DSP. */
inline void drainUiEvents(Unit* unit) {
  UiEventQueue::Event e;
  while (unit->shared.uiEvents.pop(&e)) {
    switch (e.kind) {
      case UiEventQueue::Event::Kind::ParamSet:
        if (e.index >= 0 && e.index < SONORE_NUM_PARAMS)
          unit->shared.params[e.index] = clampToRange(kDesc.params[e.index], e.value);
        break;
      case UiEventQueue::Event::Kind::NoteOn: {
        const int velocity = (int) e.value;
        unit->shared.midi.addEvent(
            MidiMessage::noteOn(0, e.index, velocity < 1 ? 1 : velocity), 0);
        break;
      }
      case UiEventQueue::Event::Kind::NoteOff:
        unit->shared.midi.addEvent(MidiMessage::noteOff(0, e.index), 0);
        break;
      default:
        break; // gestures already went to the host
    }
  }
}

/** Fill `list` with element `el`'s input, from whichever source the host
 *  wired: a render callback, a graph connection, or nothing at all (in which
 *  case the buffer is left as the caller prepared it, which for the sidechain
 *  is silence). ONE function, because "where does the input come from" had two
 *  answers and only one of them was ever asked. */
inline OSStatus pullInput(Unit* unit, UInt32 el, const AudioTimeStamp* timestamp, UInt32 frames,
                          AudioBufferList* list) {
  AudioUnitRenderActionFlags flags = 0;
  if (unit->hasInputConnection[el]) {
    const AudioUnitConnection& c = unit->inputConnection[el];
    return AudioUnitRender(c.sourceAudioUnit, &flags, timestamp, c.sourceOutputNumber, frames,
                           list);
  }
  if (unit->hasInputCallback[el])
    return unit->inputCallback[el].inputProc(unit->inputCallback[el].inputProcRefCon, &flags,
                                             timestamp, el, frames, list);
  // Nothing wired. The buffer a host hands an effect is an OUTPUT buffer, so
  // leaving it alone would feed the DSP whatever was last in that memory --
  // denormals and NaN included. An unconnected input is silence.
  for (UInt32 c = 0; c < list->mNumberBuffers; ++c)
    if (list->mBuffers[c].mData) std::memset(list->mBuffers[c].mData, 0, sizeof(float) * frames);
  return noErr;
}

inline OSStatus render(void* self, AudioUnitRenderActionFlags* flags,
                       const AudioTimeStamp* timestamp, UInt32 outputBus, UInt32 frames,
                       AudioBufferList* io) {
  auto* unit = unitOf(self);
  const uint32_t nAux = clapwrap::numAuxOutputs();
  // Rendering an uninitialised unit is a host bug, and answering it with audio
  // would hide one: prepare() has not run, so the DSP's buffers are whatever
  // the constructor left. Initialize() is also where the channel pair is
  // settled, so before it there is no agreed width to render at.
  if (!unit->initialised) return kAudioUnitErr_Uninitialized;
  if (outputBus > nAux || !io || io->mNumberBuffers < 1) return kAudioUnitErr_InvalidParameter;
  if (frames > unit->maxFrames) return kAudioUnitErr_TooManyFramesToProcess;

  // Before anything is pulled or computed: a host watching this bus is
  // entitled to see the block on its way in as well as on its way out.
  notifyRender(unit, kAudioUnitRenderAction_PreRender, timestamp, outputBus, frames, io);

  // An aux element: hand back what bus 0 already computed for this timestamp.
  // A host that asks for an aux bus WITHOUT rendering the main one first gets
  // silence rather than the previous block's audio.
  if (outputBus > 0) {
    const uint32_t b = outputBus - 1;
    const uint32_t width = clapwrap::auxBusChannels(b);
    const bool fresh = timestamp && unit->auxRenderedAt == timestamp->mSampleTime;
    uint32_t flat = 0;
    for (uint32_t i = 0; i < b; ++i) flat += clapwrap::auxBusChannels(i);
    for (UInt32 c = 0; c < io->mNumberBuffers && c < width; ++c) {
      float* dst = (float*) io->mBuffers[c].mData;
      if (!dst) continue;
      const auto& src = unit->auxScratch[flat + c];
      if (fresh && src.size() >= frames) std::memcpy(dst, src.data(), sizeof(float) * frames);
      else std::memset(dst, 0, sizeof(float) * frames);
    }
    if (flags) *flags &= ~kAudioUnitRenderAction_OutputIsSilence;
    notifyRender(unit, kAudioUnitRenderAction_PostRender, timestamp, outputBus, frames, io);
    return noErr;
  }

  uint32_t nch = unit->chans;
  if (io->mNumberBuffers < nch) nch = io->mNumberBuffers;
  if (nch > clapwrap::kMaxAudioChannels) nch = clapwrap::kMaxAudioChannels;
  float* chans[clapwrap::kMaxAudioChannels];
  // NULL mData means "use your own buffer and hand it back", not "no buffer".
  // Only channel 0 was ever checked, so a host doing this got a refusal on the
  // first channel and a null dereference on any other.
  const size_t wanted = (size_t) nch * frames;
  for (uint32_t c = 0; c < nch; ++c) {
    if (!io->mBuffers[c].mData) {
      // Allocating here would be a malloc on the audio thread; if initialize()
      // did not reserve enough, decline rather than allocate.
      if (unit->outScratch.size() < wanted) return kAudioUnitErr_TooManyFramesToProcess;
      io->mBuffers[c].mData = unit->outScratch.data() + (size_t) c * frames;
      io->mBuffers[c].mDataByteSize = frames * (UInt32) sizeof(float);
    }
    chans[c] = (float*) io->mBuffers[c].mData;
  }

  if (kDesc.isInstrument) {
    for (uint32_t c = 0; c < nch; ++c) std::memset(chans[c], 0, sizeof(float) * frames);
  } else {
    // An AU PULLS its input. Getting this wrong is the classic way an effect
    // ends up processing silence while looking perfectly healthy.
    const OSStatus r = pullInput(unit, 0, timestamp, frames, io);
    if (r != noErr) return r;
  }

  // The sidechain, when the DSP wants one: pull element 1 into our scratch.
  // No callback wired means the key input simply stays silent.
  float* scChans[2] = {nullptr, nullptr};
  if (clapwrap::TakesSidechain<SonoreDsp>::value && !kDesc.isInstrument) {
    if (unit->scScratch[0].size() < frames)
      for (auto& v : unit->scScratch) v.assign(frames, 0.0f);
    scChans[0] = unit->scScratch[0].data();
    scChans[1] = unit->scScratch[1].data();
    std::memset(scChans[0], 0, sizeof(float) * frames);
    std::memset(scChans[1], 0, sizeof(float) * frames);
    if (unit->hasInputCallback[1] || unit->hasInputConnection[1]) {
      // AudioBufferList declares mBuffers[1] and expects callers to allocate
      // the real length behind it -- the classic CoreAudio variable-size
      // struct. Stack storage with room for two.
      struct {
        AudioBufferList list;
        AudioBuffer extra;
      } scStorage{};
      AudioBufferList& scList = scStorage.list;
      scList.mNumberBuffers = 2;
      for (int c = 0; c < 2; ++c) {
        scList.mBuffers[c].mNumberChannels = 1;
        scList.mBuffers[c].mDataByteSize = frames * (UInt32) sizeof(float);
        scList.mBuffers[c].mData = scChans[c];
      }
      pullInput(unit, 1, timestamp, frames, &scList);
    }
  }

  drainUiEvents(unit);

  clapwrap::bypassCapture(unit->shared.bypass, chans, frames);
  AudioBlock<float> block(chans, nch, (size_t) frames);
  AudioBlock<float> scBlock(scChans, 2, scChans[0] ? (size_t) frames : 0);

  // Aux buses render into our own storage; the host collects them element by
  // element afterwards.
  AudioBlock<float> auxBlocks[clapwrap::kMaxAuxOutputs] = {};
  float* auxPtrs[clapwrap::kMaxAuxOutputs][clapwrap::kMaxAudioChannels] = {};
  {
    uint32_t flat = 0;
    for (uint32_t b = 0; b < nAux; ++b) {
      const uint32_t width = clapwrap::auxBusChannels(b);
      for (uint32_t c = 0; c < width; ++c, ++flat) {
        auto& buf = unit->auxScratch[flat];
        if (buf.size() < frames) buf.assign(unit->maxFrames, 0.0f);
        auxPtrs[b][c] = buf.data();
      }
      auxBlocks[b] = AudioBlock<float>(auxPtrs[b], width, (size_t) frames);
    }
  }

  unit->shared.midiOut.clear();
  uint8_t roles[clapwrap::kMaxAudioChannels];
  const uint32_t numRoles =
      rolesFromMask(defaultChannelMask(unit->chans), roles, clapwrap::kMaxAudioChannels);
  unit->shared.expression.clear();
  if (kDesc.supportsMpe) unit->shared.mpe.process(unit->shared.midi, unit->shared.expression);
  ProcessContext ctx{block,
                     auxBlocks,
                     nAux,
                     scBlock,
                     unit->shared.midi,
                     unit->shared.midiOut,
                     numRoles ? roles : nullptr,
                     &unit->shared.expression};
  trackNotes(unit->shared.notes, ctx.midi);
  clapwrap::snapshotParams(&unit->shared);
  clapwrap::runDspCtx(unit->shared.dsp, ctx, unit->shared.paramsBlock);

  // AU delivers emitted MIDI through a host callback taking a CoreMIDI packet
  // list. MIDIPacketList declares one packet and expects the caller to
  // allocate the rest -- the same variable-length convention as
  // AudioBufferList, and the same trap.
  if (kDesc.producesMidi && unit->hasMidiOutCallback && !unit->shared.midiOut.isEmpty()) {
    alignas(4) uint8_t storage[sizeof(MIDIPacketList) + 64 * sizeof(MIDIPacket)];
    auto* list = (MIDIPacketList*) storage;
    MIDIPacket* packet = MIDIPacketListInit(list);
    for (const auto& e : unit->shared.midiOut) {
      const uint8_t bytes[3] = {(uint8_t) e.message.getRawStatus(),
                                (uint8_t) e.message.getRawData1(),
                                (uint8_t) e.message.getRawData2()};
      const MIDITimeStamp when =
          (MIDITimeStamp) (e.samplePosition < 0 ? 0 : e.samplePosition);
      packet = MIDIPacketListAdd(list, sizeof(storage), packet, when, 3, bytes);
      if (!packet) break; // full: deliver what fits rather than overrun
    }
    unit->midiOutCallback.midiOutputCallback(unit->midiOutCallback.userData, timestamp, 0,
                                             list);
  }
  unit->auxRenderedAt = timestamp ? timestamp->mSampleTime : -1.0;
  clapwrap::bypassApply(unit->shared.bypass, chans, frames);
  unit->shared.midi.clear();

  {
    // Vectorised: one pass each for peak and energy. Measured 1.7x native
    // and 1.4x in WebAssembly over the hand-rolled loop, and the meters run
    // on every block whether or not an editor is open.
    unit->shared.meter.push(measureBlock(chans[0], (size_t) frames));
  }

  if (flags) *flags &= ~kAudioUnitRenderAction_OutputIsSilence;
  notifyRender(unit, kAudioUnitRenderAction_PostRender, timestamp, outputBus, frames, io);
  return noErr;
}

/** Notes, for instruments. AU delivers raw MIDI bytes. */
inline OSStatus midiEvent(void* self, UInt32 status, UInt32 data1, UInt32 data2, UInt32 offset) {
  auto* unit = unitOf(self);
  // NOT masked to the high nibble first: 0xF8 & 0xF0 is 0xF0, so masking
  // turned every realtime byte into something the old test rejected.
  if (!deliverableToDsp((int) status)) return noErr;
  unit->shared.midi.addEvent(MidiMessage((int) status, (int) data1, (int) data2), (int) offset);
  return noErr;
}

/**
 * SysEx, which AU delivers through its OWN entry point.
 *
 * Not the MIDI callback above: MusicDeviceSysEx is a separate selector taking
 * a pointer and a length, because a SysEx does not fit in the three UInt32s
 * midiEvent carries. That is why this was the last format in the SDK with no
 * SysEx path -- the bytes were never arriving anywhere the MIDI code looked.
 *
 * Apple does not promise a complete message per call. A host may hand over
 * whatever it received, so the bytes go through the same SysexAssembler the
 * three MIDI backends use, and a DSP still only ever sees a finished message.
 *
 * Compiled on macOS; still unrun against a device, because auval sends no
 * SysEx and a CI runner has no MIDI hardware. What makes it worth having is
 * that the risky part -- deciding what a byte MEANS -- is the assembler, which
 * is shared with three backends and has 20 checks against it. What is new
 * here is only where the bytes come from.
 */
inline OSStatus sysExEvent(void* self, const UInt8* inData, UInt32 inLength) {
  auto* unit = unitOf(self);
  if (!inData || inLength == 0) return noErr;
  unit->sysex.pushBlock(inData, (size_t) inLength,
                        [unit](const uint8_t* bytes, size_t n) {
                          // Offset 0: MusicDeviceSysEx carries no frame
                          // position, and inventing one would be worse than
                          // the honest answer that it arrived this block.
                          unit->shared.midi.addSysex(bytes, n, 0);
                        });
  return noErr;
}

inline OSStatus startNote(void* self, MusicDeviceInstrumentID, MusicDeviceGroupID,
                          NoteInstanceID* outNote, UInt32 offset,
                          const MusicDeviceNoteParams* params) {
  auto* unit = unitOf(self);
  if (!params) return kAudioUnitErr_InvalidParameter;
  const int note = (int) params->mPitch;
  int velocity = (int) params->mVelocity;
  if (velocity < 1) velocity = 1;
  unit->shared.midi.addEvent(MidiMessage::noteOn(0, note, velocity), (int) offset);
  if (outNote) *outNote = (NoteInstanceID) note;
  return noErr;
}

inline OSStatus stopNote(void* self, MusicDeviceGroupID, NoteInstanceID note, UInt32 offset) {
  auto* unit = unitOf(self);
  unit->shared.midi.addEvent(MidiMessage::noteOff(0, (int) note), (int) offset);
  return noErr;
}

// ── The component plug-in interface ──────────────────────────────────────────

inline OSStatus componentOpen(void* self, AudioUnit hostInstance) {
  // The instance storage BEGINS AT the mInstanceStorage field: Apple's own
  // dispatch does `(AUBase*)&ACPI->mInstanceStorage`. Reading the field's
  // VALUE instead (an earlier version did) dereferences uninitialised memory
  // on the first call a host ever makes.
  auto* unit = unitOf(self);
  new (unit) Unit();
  // The host-side handle, kept for everything that must speak to the host in
  // its own terms: AUParameterSet and property-listener notifications. Our
  // internal pointer is NOT that handle, and passing it out would hand the
  // host a pointer it never issued.
  unit->hostInstance = hostInstance;
  // Asked of the framework rather than guessed from the CMake plist: the two
  // could drift, and only one of them is what the host actually loaded.
  if (hostInstance)
    AudioComponentGetDescription(AudioComponentInstanceGetComponent(hostInstance),
                                 &unit->componentDesc);
  unit->chans = clapwrap::defaultMainChannels();
  unit->mainInChans = unit->chans;
  unit->mainOutChans = unit->chans;
  for (int i = 0; i < SONORE_NUM_PARAMS && i < kDesc.numParams; ++i)
    unit->shared.params[i] = kDesc.params[i].defaultValue;
  unit->currentPreset.presetNumber = -1;
  unit->currentPreset.presetName =
      CFStringCreateWithCString(nullptr, "Default", kCFStringEncodingUTF8);
  return noErr;
}

inline OSStatus componentClose(void* self) {
  auto* unit = unitOf(self);
  if (unit->currentPreset.presetName) CFRelease(unit->currentPreset.presetName);
  unit->~Unit();
  // The block is OURS: the factory calloc'd it, so Close is the only place it
  // can be given back. Destroying the Unit and leaving the allocation behind
  // leaked an entire instance -- DSP state included, which for a reverb is a
  // third of a megabyte -- every time a host scanned or closed the plugin.
  std::free(self);
  return noErr;
}

/** AU dispatch: the host asks for one selector at a time and calls what it gets
 *  back. An unknown selector must return null rather than something plausible:
 *  a wrong function pointer here is a crash inside the host. */
inline AudioComponentMethod componentLookup(SInt16 selector) {
  switch (selector) {
    case kAudioUnitInitializeSelect: return (AudioComponentMethod) initialize;
    case kAudioUnitUninitializeSelect: return (AudioComponentMethod) uninitialize;
    case kAudioUnitGetPropertyInfoSelect: return (AudioComponentMethod) getPropertyInfo;
    case kAudioUnitGetPropertySelect: return (AudioComponentMethod) getProperty;
    case kAudioUnitSetPropertySelect: return (AudioComponentMethod) setProperty;
    case kAudioUnitAddPropertyListenerSelect: return (AudioComponentMethod) addPropertyListener;
    case kAudioUnitRemovePropertyListenerSelect:
      return (AudioComponentMethod) removePropertyListener;
    case kAudioUnitRemovePropertyListenerWithUserDataSelect:
      return (AudioComponentMethod) removePropertyListenerWithUserData;
    case kAudioUnitAddRenderNotifySelect: return (AudioComponentMethod) addRenderNotify;
    case kAudioUnitRemoveRenderNotifySelect: return (AudioComponentMethod) removeRenderNotify;
    case kAudioUnitGetParameterSelect: return (AudioComponentMethod) getParameter;
    case kAudioUnitSetParameterSelect: return (AudioComponentMethod) setParameter;
    case kAudioUnitScheduleParametersSelect: return (AudioComponentMethod) scheduleParameters;
    case kAudioUnitRenderSelect: return (AudioComponentMethod) render;
    case kAudioUnitResetSelect: return (AudioComponentMethod) reset;
    // Only a plugin that actually READS MIDI answers these. Publishing them
    // unconditionally made every effect look like a music effect that had
    // filed itself under the wrong type: "AU implements MusicDeviceMIDIEvent
    // but is of type 'aufx' (it should be 'aumf')". An unimplemented selector
    // is answered by the framework, which is the honest reply for a saturator.
    case kMusicDeviceMIDIEventSelect:
      return clapwrap::wantsMidiIn() ? (AudioComponentMethod) midiEvent : nullptr;
    case kMusicDeviceSysExSelect:
      return clapwrap::wantsMidiIn() ? (AudioComponentMethod) sysExEvent : nullptr;
    case kMusicDeviceStartNoteSelect:
      return kDesc.isInstrument ? (AudioComponentMethod) startNote : nullptr;
    case kMusicDeviceStopNoteSelect:
      return kDesc.isInstrument ? (AudioComponentMethod) stopNote : nullptr;
    default: return nullptr;
  }
}

} // namespace au
} // namespace sonore

/** The factory the bundle's Info.plist names. One per product. */
extern "C" __attribute__((visibility("default"))) AudioComponentPlugInInterface*
SonoreAUFactory(const AudioComponentDescription*) {
  // QUALIFIED. This struct is ours -- declared in sonore::au because Apple
  // stopped shipping AudioComponentPlugInInstance in the public headers -- and
  // this function is at global scope, where the bare name finds nothing.
  //
  // So on a current macOS SDK this line did not compile, and on an older one
  // it silently picked up APPLE's layout while componentOpen addressed the
  // instance using ours. Neither is what was meant. Found by the first
  // successful parse of this file, which is also the first time any compiler
  // had read it.
  using Block = sonore::au::AudioComponentPlugInInstance;
  auto* instance =
      (Block*) std::calloc(1, offsetof(Block, mInstanceStorage) + sizeof(sonore::au::Unit));
  if (!instance) return nullptr;
  instance->PlugInInterface.Open = sonore::au::componentOpen;
  instance->PlugInInterface.Close = sonore::au::componentClose;
  instance->PlugInInterface.Lookup = sonore::au::componentLookup;
  instance->PlugInInterface.reserved = nullptr;
  return &instance->PlugInInterface;
}
