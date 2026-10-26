// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: the Audio Unit v3 format wrapper (AUAudioUnit), for macOS and iOS.
//
// ── What AUv3 is, next to the AUv2 this SDK already builds ──────────────────
//
// An AUv2 (au_wrapper.h) is a bundle of C functions a host dlopens into its
// own process. An AUv3 is an Objective-C OBJECT -- a subclass of Apple's
// AUAudioUnit -- that lives in an app EXTENSION (.appex), shipped inside an
// ordinary application, and usually runs in a process of its own that the
// host talks to across the system's audio-unit bridge. It is the only kind
// of plugin iOS and iPadOS load (GarageBand, AUM, Cubasis, Logic Pro for
// iPad), and the only kind the Mac App Store accepts. Logic and every other
// Mac host load both, and auval validates both.
//
// ── Written against the runtime, as every Apple file here is ────────────────
//
// The SDK stays plain C++, so there is no .mm file: the AUAudioUnit subclass
// is REGISTERED at runtime (objc_allocateClassPair), each method a C function
// with the selector's signature, and every call into a framework class is
// objc_msgSend through a correctly typed pointer -- exactly what the compiler
// emits for Objective-C. The blocks AUv3 trades in (the render block, the
// parameter tree's observers) are built by hand to clang's block ABI
// (apple_block.h); the blocks a HOST hands over (pull input, musical context,
// transport, MIDI out) are called through their invoke pointers.
//
// The C structs AUv3 declares only in Objective-C headers -- the render
// events, the MIDI event list -- are mirrored below with the layout asserted,
// as window_cocoa.h mirrors CGRect.
//
// ── What it shares, and what it adds ───────────────────────────────────────
//
// Include it AFTER clap_wrapper.h (formats.h does, for SONORE_BUILD_AUV3):
// the DSP instance, parameters, the state blob, presets, the bypass
// crossfade, the output guard and both editors are the shared Instance's, so
// an AUv3 saves the same bytes and sounds the same as every other format.
// The state dictionary is AUv2's ClassInfo layout (type, subtype,
// manufacturer, version, name, SonoreState), so a preset file reads the same
// in either.
//
// AUv3 adds what AUv2 never had: parameter changes as timed render events
// (sample-accurate here when the descriptor asks, PluginDescriptor::
// automationGranularity, by the same block splitting CLAP and VST3 use), a
// begin/end GESTURE for every edit an editor makes, MIDI 2.0 event lists,
// and the track's name (contextName).
//
// ── Status ──────────────────────────────────────────────────────────────────
//
// Compiled on Windows against tests/au_shim on every build (the shim's README
// says what that proves and what it does not). On macOS CI it is built as an
// .appex inside a container app, registered with pluginkit and validated by
// auval, and tests/auv3_selftest.cpp drives the class in-process the way a
// host does. On iOS CI it is built for the simulator and the same self-test
// runs there. Opened in a real host's window and played from a real
// keyboard: not yet, by nobody in this loop.
#pragma once

#if !defined(__APPLE__) && !defined(SONORE_APPLE_SYNTAX_CHECK)
#error "auv3_wrapper.h is for Apple platforms (macOS and iOS)"
#endif

#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CFRunLoop.h>
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <objc/message.h>
#include <objc/objc.h>
#include <objc/runtime.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>
#include <vector>

#include "apple_block.h"
#include "apple_platform.h"
#include "audio.h"
#include "gui.h"
#include "plugin.h"
#include "presets.h"
#include "ump.h"

#ifndef SONORE_NUM_PARAMS
#error "Define SONORE_NUM_PARAMS (and struct SonoreDsp) before including auv3_wrapper.h"
#endif

/** The class name the extension's Info.plist names as NSExtensionPrincipalClass.
 *  It has to be known when the plist is written, so it cannot be made unique
 *  per image the way every other runtime class here is: sonore_add_auv3 derives
 *  it from the target and passes it in. A build without the helper (a syntax
 *  check, a self-test) gets this fallback. */
#ifndef SONORE_AUV3_FACTORY_CLASS
#define SONORE_AUV3_FACTORY_CLASS "SonoreAUv3Factory"
#endif

namespace sonore {
namespace auv3 {

using clapwrap::Instance;

// ── The C side of AUv3's Objective-C headers ────────────────────────────────
//
// AUAudioUnit.h and AUAudioUnitImplementation.h are Objective-C only, so the
// types a render block receives are restated here from Apple's declarations.
// Layouts are pinned below with static_assert: a field one byte off would
// read every event wrong and still compile.

using Value = float;              // AUValue
using ParameterAddress = uint64_t; // AUParameterAddress
using EventSampleTime = int64_t;  // AUEventSampleTime
using FrameCount = uint32_t;      // AUAudioFrameCount
using ObjcInteger = long;         // NSInteger on every 64-bit Apple target
using ObjcUInteger = unsigned long;

/** AUEventSampleTimeImmediate: "as soon as possible", plus an offset under
 *  4096 a caller may add. A render list should carry real times, but a host
 *  passing this through is not one to crash on. */
constexpr EventSampleTime kEventSampleTimeImmediate = (EventSampleTime) 0xffffffff00000000LL;

/** AURenderEventType. */
enum : uint8_t {
  kEventParameter = 1,
  kEventParameterRamp = 2,
  kEventMIDI = 8,
  kEventMIDISysEx = 9,
  kEventMIDIEventList = 10,
};

/** AURenderEventHeader: what every render event starts with. */
struct EventHeader {
  const EventHeader* next;
  EventSampleTime eventSampleTime;
  uint8_t eventType;
  uint8_t reserved;
};

/** AUParameterEvent, for kEventParameter and kEventParameterRamp. */
struct ParameterEvent {
  const EventHeader* next;
  EventSampleTime eventSampleTime;
  uint8_t eventType;
  uint8_t reserved[3];
  FrameCount rampDurationSampleFrames;
  ParameterAddress parameterAddress;
  Value value;
};

/** AUMIDIEvent, for kEventMIDI and kEventMIDISysEx. `data` is declared three
 *  bytes long and runs on for `length` bytes when the event is a SysEx. */
struct MidiEvent {
  const EventHeader* next;
  EventSampleTime eventSampleTime;
  uint8_t eventType;
  uint8_t reserved;
  uint16_t length;
  uint8_t cable;
  uint8_t data[3];
};

#pragma pack(push, 4)
/** MIDIEventPacket (CoreMIDI, macOS 11 / iOS 14): Universal MIDI Packet words. */
struct UmpPacket {
  uint64_t timeStamp;
  uint32_t wordCount;
  uint32_t words[64];
};
/** MIDIEventList: the packets, back to back. */
struct UmpEventList {
  int32_t protocol;
  uint32_t numPackets;
  UmpPacket packet[1];
};
#pragma pack(pop)

/** AUMIDIEventList, for kEventMIDIEventList. */
struct MidiEventListEvent {
  const EventHeader* next;
  EventSampleTime eventSampleTime;
  uint8_t eventType;
  uint8_t reserved;
  uint8_t cable;
  UmpEventList eventList;
};

static_assert(offsetof(EventHeader, eventSampleTime) == 8 && offsetof(EventHeader, eventType) == 16,
              "AURenderEventHeader layout");
static_assert(offsetof(ParameterEvent, rampDurationSampleFrames) == 20 &&
                  offsetof(ParameterEvent, parameterAddress) == 24 && offsetof(ParameterEvent, value) == 32,
              "AUParameterEvent layout");
static_assert(offsetof(MidiEvent, length) == 18 && offsetof(MidiEvent, cable) == 20 &&
                  offsetof(MidiEvent, data) == 21,
              "AUMIDIEvent layout");
static_assert(offsetof(UmpPacket, wordCount) == 8 && offsetof(UmpPacket, words) == 12 &&
                  sizeof(UmpPacket) == 268,
              "MIDIEventPacket layout");
static_assert(offsetof(MidiEventListEvent, eventList) == 20, "AUMIDIEventList layout");

/** The fields every block starts with (apple_block.h builds whole ones; this
 *  is for calling a block somebody else made). */
struct BlockHeader {
  void* isa;
  int flags;
  int reserved;
  void* invoke;
};

/** Call a block: its invoke function, with the block itself first. */
template <typename Ret, typename... Args>
inline Ret callBlock(void* block, Args... args) {
  using Fn = Ret (*)(void*, Args...);
  return reinterpret_cast<Fn>(static_cast<BlockHeader*>(block)->invoke)(block, args...);
}

// Block shapes, as clang lays out their invoke functions.
using RenderInvoke = int32_t (*)(void* block, AudioUnitRenderActionFlags* actionFlags,
                                 const AudioTimeStamp* timestamp, FrameCount frameCount,
                                 ObjcInteger outputBusNumber, AudioBufferList* outputData,
                                 const EventHeader* realtimeEventListHead, void* pullInputBlock);
using ValueObserverInvoke = void (*)(void* block, id parameter, Value value);
using ValueProviderInvoke = Value (*)(void* block, id parameter);
using StringFromValueInvoke = id (*)(void* block, id parameter, const Value* value);
using ValueFromStringInvoke = Value (*)(void* block, id parameter, id string);

/** AUHostTransportStateFlags (NSUInteger). */
enum : ObjcUInteger {
  kTransportChanged = 1,
  kTransportMoving = 2,
  kTransportRecording = 4,
  kTransportCycling = 8,
};

/** AUParameterAutomationEventType. */
enum : uint32_t {
  kAutomationValue = 0,
  kAutomationTouch = 1,
  kAutomationRelease = 2,
};

/** AUAudioUnitBusType. */
enum : ObjcInteger {
  kBusTypeInput = 1,
  kBusTypeOutput = 2,
};

// ── Messaging ───────────────────────────────────────────────────────────────

inline SEL sel(const char* name) { return sel_registerName(name); }

template <typename Ret, typename... Args>
inline Ret send(id target, const char* selector, Args... args) {
  using Fn = Ret (*)(id, SEL, Args...);
  return reinterpret_cast<Fn>(objc_msgSend)(target, sel_registerName(selector), args...);
}

inline id classNamed(const char* name) { return (id) objc_getClass(name); }

/** objc_super's layout: the receiver, then the class the method search
 *  STARTS at -- the framework class our runtime class extends, named outright.
 *  Never object_getClass(self)'s superclass: a key-value observer subclasses
 *  the object at runtime, and that superclass is then our own class. */
struct SuperRef {
  id receiver;
  Class superClass;
};

template <typename Ret, typename... Args>
inline Ret sendSuper(id self, Class base, const char* selector, Args... args) {
  SuperRef ref{self, base};
  using Fn = Ret (*)(SuperRef*, SEL, Args...);
  return reinterpret_cast<Fn>(objc_msgSendSuper)(&ref, sel_registerName(selector), args...);
}

inline id retain(id o) { return o ? send<id>(o, "retain") : o; }
inline void release(id o) {
  if (o) send<void>(o, "release");
}

/** An autoreleased NSString, through CF (toll-free bridged). */
inline id string(const char* utf8) {
  CFStringRef s = CFStringCreateWithCString(nullptr, utf8 ? utf8 : "", kCFStringEncodingUTF8);
  return s ? (id) CFAutorelease(s) : nil;
}

inline std::string utf8Of(id str) {
  if (!str) return std::string();
  char text[512] = {};
  if (CFStringGetCString((CFStringRef) str, text, sizeof(text), kCFStringEncodingUTF8)) return text;
  return std::string();
}

inline id number(long value) { return send<id>(classNamed("NSNumber"), "numberWithLong:", value); }

inline id array(const id* objects, ObjcUInteger count) {
  return send<id>(classNamed("NSArray"), "arrayWithObjects:count:", objects, count);
}

/** Write `status` into an NSError** the caller may have passed. */
inline void setError(id* outError, OSStatus status) {
  if (!outError) return;
  static id* const domain = (id*) dlsym(RTLD_DEFAULT, "NSOSStatusErrorDomain");
  *outError = send<id>(classNamed("NSError"), "errorWithDomain:code:userInfo:",
                       domain ? *domain : string("NSOSStatusErrorDomain"), (ObjcInteger) status, (id) nil);
}

/** A key-value notification, both halves: how a host learns a property an
 *  AU owns has moved (latency, every parameter at once). [main-thread] */
inline void announce(id object, const char* key) {
  if (!object) return;
  id k = string(key);
  send<void>(object, "willChangeValueForKey:", k);
  send<void>(object, "didChangeValueForKey:", k);
}

/** Run `fn(context)` on the main thread, later. libdispatch's C entry points,
 *  found by name like every other framework symbol here. */
inline void onMainThread(void* context, void (*fn)(void*)) {
  using AsyncF = void (*)(void* queue, void* context, void (*work)(void*));
  static const AsyncF asyncF = (AsyncF) dlsym(RTLD_DEFAULT, "dispatch_async_f");
  static void* const mainQueue = dlsym(RTLD_DEFAULT, "_dispatch_main_q");
  if (asyncF && mainQueue) asyncF(mainQueue, context, fn);
  else fn(context); // no libdispatch: a syntax check, or something very wrong
}

inline bool isMainThread() {
  using IsMain = int (*)();
  static const IsMain isMain = (IsMain) dlsym(RTLD_DEFAULT, "pthread_main_np");
  return isMain ? isMain() != 0 : true;
}

/** Copy a block a host handed over, so it outlives the call that gave it. */
inline void* copyBlock(void* block) {
  using Copy = void* (*)(const void*);
  static const Copy copy = (Copy) dlsym(RTLD_DEFAULT, "_Block_copy");
  return block && copy ? copy(block) : nullptr;
}
inline void releaseBlock(void* block) {
  using Release = void (*)(const void*);
  static const Release rel = (Release) dlsym(RTLD_DEFAULT, "_Block_release");
  if (block && rel) rel(block);
}

// ── One instance ─────────────────────────────────────────────────────────────

/**
 * Everything one AUAudioUnit object holds: the shared Instance every format
 * drives, the framework objects the unit publishes (buses, parameter tree,
 * presets), the render block, and the host's blocks for the timeline and MIDI
 * output, copied when render resources are allocated.
 *
 * Owned by the Objective-C object (an instance variable), created in its
 * initialiser and deleted in its dealloc. Render reads only what
 * allocateRenderResources sized.
 */
struct Unit {
  Instance shared;
  /** The AUAudioUnit object, unretained: it owns this. */
  id self = nil;
  /** The same object, weakly: what the main-thread clock reads, so a tick
   *  that races the object's last release on another thread finds nil
   *  rather than an object halfway through dealloc. */
  id weakSelf = nil;
  AudioComponentDescription componentDesc{};

  // Framework objects, retained.
  id inputBusArray = nil;
  id outputBusArray = nil;
  id parameterTree = nil;
  id parameters[SONORE_NUM_PARAMS > 0 ? SONORE_NUM_PARAMS : 1] = {};
  /** The value each AUParameter was last given. An AUParameter CACHES its
   *  value -- a host reads the cache, not the provider -- so every change the
   *  plugin makes itself (a render event, a state, a preset, its own editor)
   *  has to be pushed back with setValue:originator:, and this is what tells
   *  publishParameters() which ones moved. Written by the observer too, on
   *  whatever thread the host set a value from: atomic. */
  std::atomic<float> published[SONORE_NUM_PARAMS > 0 ? SONORE_NUM_PARAMS : 1] = {};
  id factoryPresets = nil;
  id currentPreset = nil;

  double sampleRate = 44100.0;
  uint32_t maxFrames = 512;
  uint32_t chans = 2;
  bool prepared = false;
  std::atomic<bool> bypassed{false};

  using RenderBlockLiteral = ::sonore::detail::BlockLiteral<Unit*, RenderInvoke>;
  RenderBlockLiteral renderBlock{};
  ::sonore::detail::BlockLiteral<Unit*, ValueObserverInvoke> valueObserver{};
  ::sonore::detail::BlockLiteral<Unit*, ValueProviderInvoke> valueProvider{};
  ::sonore::detail::BlockLiteral<Unit*, StringFromValueInvoke> stringFromValue{};
  ::sonore::detail::BlockLiteral<Unit*, ValueFromStringInvoke> valueFromString{};

  /** The host's blocks, copied at allocateRenderResources and released at
   *  deallocate. Read on the render thread: Apple's own guidance is to cache
   *  them there rather than ask the object for them while rendering. */
  void* musicalContext = nullptr;
  void* transportState = nullptr;
  void* midiOutput = nullptr;

  /** Storage for a host that hands over null mData ("use your own buffer"),
   *  and for what the pull block returns by reference. */
  std::vector<float> outScratch;
  std::vector<float> scScratch[2];
  std::vector<float> auxScratch[clapwrap::kMaxAuxOutputs * clapwrap::kMaxAudioChannels];
  double auxRenderedAt = -1.0;

  /** The main-thread clock: latency changes, the output guard's report,
   *  parameter edits an editor made. 10 Hz, on the main run loop. */
  CFRunLoopTimerRef housekeeping = nullptr;
  uint32_t reportedLatency = 0;
  uint32_t reportedTail = 0;
  std::atomic<bool> stateDirtyPending{false};

  /** The editor's parent view while one is open, and the view controller
   *  that owns it (unretained: the controller owns the editor, not this). */
  id editorView = nil;
  id viewController = nil;
};

/** The runtime class's instance variable that holds the Unit. */
constexpr const char* kUnitIvar = "sonoreUnit";

inline Unit* unitOf(id self) {
  void* unit = nullptr;
  if (self) object_getInstanceVariable(self, kUnitIvar, &unit);
  return static_cast<Unit*>(unit);
}

inline uint32_t parameterIndex(id parameter) {
  return parameter ? (uint32_t) send<ParameterAddress>(parameter, "address") : UINT32_MAX;
}

inline bool validIndex(uint32_t index) {
  return index < (uint32_t) kDesc.numParams && index < (uint32_t) SONORE_NUM_PARAMS;
}

// ── Parameters ───────────────────────────────────────────────────────────────

/** The unit enum a host draws a parameter with; the same choice AUv2 makes
 *  (au_wrapper.h), restated because an iOS build has no AUv2 to borrow from. */
inline AudioUnitParameterUnit unitFor(const ParamInfo& p) {
  if (isNamedChoice(p)) return kAudioUnitParameterUnit_Indexed;
  if (!p.unit || !p.unit[0]) return kAudioUnitParameterUnit_Generic;
  if (std::strcmp(p.unit, "dB") == 0) return kAudioUnitParameterUnit_Decibels;
  if (std::strcmp(p.unit, "Hz") == 0) return kAudioUnitParameterUnit_Hertz;
  if (std::strcmp(p.unit, "%") == 0) return kAudioUnitParameterUnit_Percent;
  if (std::strcmp(p.unit, "s") == 0) return kAudioUnitParameterUnit_Seconds;
  if (std::strcmp(p.unit, "ms") == 0) return kAudioUnitParameterUnit_Milliseconds;
  return kAudioUnitParameterUnit_CustomUnit;
}

/** AudioUnitParameterOptions, read as AUv2 reads ParamInfo (see au_wrapper.h
 *  for why automatable and hidden have no AU spelling). */
inline uint32_t flagsFor(const ParamInfo& p) {
  uint32_t flags = kAudioUnitParameterFlag_IsReadable | kAudioUnitParameterFlag_ValuesHaveStrings;
  if (!p.readOnly) flags |= kAudioUnitParameterFlag_IsWritable;
  else flags |= kAudioUnitParameterFlag_MeterReadOnly;
  if (p.skew > 0.0f && p.skew < 1.0f) flags |= kAudioUnitParameterFlag_DisplayLogarithmic;
  // A parameter the host may ramp: everything continuous. A stepped one jumps.
  if (p.stepCount == 0 && !p.readOnly) flags |= kAudioUnitParameterFlag_CanRamp;
  return flags;
}

/** Set while this thread is pushing the plugin's own values into its
 *  parameters: setValue: calls the observer back synchronously, and the value
 *  it hands over may already be older than one the render thread has applied
 *  since -- written back, it would undo that one. */
inline bool& publishingHere() {
  static thread_local bool publishing = false;
  return publishing;
}

/** [any thread] The host, or its generic editor, set a value. */
inline void observeValue(void* block, id parameter, Value value) {
  Unit* unit = ::sonore::detail::blockCapture<Unit*, ValueObserverInvoke>(block);
  const uint32_t i = parameterIndex(parameter);
  if (!unit || !validIndex(i) || publishingHere()) return;
  const float v = clampToRange(kDesc.params[i], value);
  unit->shared.params[i] = v;
  // The parameter holds this value now: nothing to push back.
  unit->published[i].store(v, std::memory_order_relaxed);
}

/** [any thread] The value the host reads. */
inline Value provideValue(void* block, id parameter) {
  Unit* unit = ::sonore::detail::blockCapture<Unit*, ValueProviderInvoke>(block);
  const uint32_t i = parameterIndex(parameter);
  if (!unit || !validIndex(i)) return 0.0f;
  return unit->shared.params[i];
}

/** [any thread] The text a host shows: the same text every format shows. */
inline id stringFromValue(void* block, id parameter, const Value* value) {
  Unit* unit = ::sonore::detail::blockCapture<Unit*, StringFromValueInvoke>(block);
  const uint32_t i = parameterIndex(parameter);
  if (!unit || !validIndex(i)) return nil;
  const float v = value ? *value : (float) unit->shared.params[i];
  char text[96];
  formatParamValue(kDesc.params[i], v, text, sizeof(text));
  return string(text);
}

/** [any thread] A value typed into the host's field. */
inline Value valueFromString(void* block, id parameter, id text) {
  Unit* unit = ::sonore::detail::blockCapture<Unit*, ValueFromStringInvoke>(block);
  const uint32_t i = parameterIndex(parameter);
  if (!unit || !validIndex(i)) return 0.0f;
  float value = unit->shared.params[i];
  const std::string typed = utf8Of(text);
  if (!parseParamValue(kDesc.params[i], typed.c_str(), &value)) return unit->shared.params[i];
  return clampToRange(kDesc.params[i], value);
}

/** The tree, built once at init: one AUParameter per descriptor entry, its
 *  address the index, in the descriptor's groups. */
inline void buildParameterTree(Unit* unit) {
  const id treeClass = classNamed("AUParameterTree");
  if (!treeClass) return;
  const GroupTable& groups = clapwrap::descriptorGroups();
  std::vector<id> top, groupChildren[GroupTable::kMaxGroups];

  for (int i = 0; i < kDesc.numParams && i < SONORE_NUM_PARAMS; ++i) {
    const ParamInfo& p = kDesc.params[i];
    id valueStrings = nil;
    if (isNamedChoice(p)) {
      std::vector<id> names;
      for (int k = 0; k < p.stepCount && k < 256; ++k) names.push_back(string(p.valueNames[k]));
      valueStrings = array(names.data(), (ObjcUInteger) names.size());
    }
    const AudioUnitParameterUnit u = unitFor(p);
    id unitName = u == kAudioUnitParameterUnit_CustomUnit ? string(p.unit) : nil;
    // The identifier is what a host saves automation against, so it is the
    // descriptor's stable id, never the label a designer may rename.
    char identifier[64];
    std::snprintf(identifier, sizeof(identifier), "%s", p.id && p.id[0] ? p.id : "param");
    id param = send<id>(treeClass,
                        "createParameterWithIdentifier:name:address:min:max:unit:unitName:flags:"
                        "valueStrings:dependentParameters:",
                        string(identifier), string(p.label), (ParameterAddress) i, (Value) p.minValue,
                        (Value) p.maxValue, u, unitName, flagsFor(p), valueStrings, (id) nil);
    if (!param) continue;
    send<void>(param, "setValue:", (Value) p.defaultValue);
    unit->published[i].store(p.defaultValue, std::memory_order_relaxed);
    unit->parameters[i] = retain(param);
    const int g = groups.indexOf(p.group);
    if (g >= 0 && g < GroupTable::kMaxGroups) groupChildren[g].push_back(param);
    else top.push_back(param);
  }
  for (int g = 0; g < groups.count && g < GroupTable::kMaxGroups; ++g) {
    if (groupChildren[g].empty()) continue;
    char identifier[48];
    std::snprintf(identifier, sizeof(identifier), "group%d", g + 1);
    id group = send<id>(treeClass, "createGroupWithIdentifier:name:children:", string(identifier),
                        string(groups.names[g]),
                        array(groupChildren[g].data(), (ObjcUInteger) groupChildren[g].size()));
    if (group) top.push_back(group);
  }
  id tree = send<id>(treeClass, "createTreeWithChildren:", array(top.data(), (ObjcUInteger) top.size()));
  if (!tree) return;
  unit->parameterTree = retain(tree);

  // The implementor's blocks, on the TREE: every node without its own uses
  // its nearest ancestor's. Global blocks living in the Unit, which outlives
  // the tree (dealloc releases the tree first).
  using namespace ::sonore::detail;
  if (makeGlobalBlock(unit->valueObserver, &observeValue, unit))
    send<void>(tree, "setImplementorValueObserver:", (void*) &unit->valueObserver);
  if (makeGlobalBlock(unit->valueProvider, &provideValue, unit))
    send<void>(tree, "setImplementorValueProvider:", (void*) &unit->valueProvider);
  if (makeGlobalBlock(unit->stringFromValue, &stringFromValue, unit))
    send<void>(tree, "setImplementorStringFromValueCallback:", (void*) &unit->stringFromValue);
  if (makeGlobalBlock(unit->valueFromString, &valueFromString, unit))
    send<void>(tree, "setImplementorValueFromStringCallback:", (void*) &unit->valueFromString);
}

/** [main-thread] Tell the host one parameter moved because the PLUGIN moved
 *  it -- an editor's knob -- with the gesture phase AUv3 lets us say. Through
 *  the AUParameter itself, which is what a host's automation records. */
inline void tellHost(Unit* unit, int index, float value, uint32_t eventType) {
  if (!unit || index < 0 || !validIndex((uint32_t) index) || !unit->parameters[index]) return;
  unit->published[index].store(value, std::memory_order_relaxed);
  publishingHere() = true;
  send<void>(unit->parameters[index], "setValue:originator:atHostTime:eventType:", (Value) value,
             (void*) nullptr, (uint64_t) 0, eventType);
  publishingHere() = false;
}

/** [main-thread] Every value the plugin moved itself, into its AUParameter,
 *  which is what a host reads (see Unit::published). Without an originator,
 *  so every observer hears it -- the host's controls and its automation. */
inline void publishParameters(Unit* unit) {
  publishingHere() = true;
  for (int i = 0; i < kDesc.numParams && i < SONORE_NUM_PARAMS; ++i) {
    if (!unit->parameters[i]) continue;
    const float v = unit->shared.params[i];
    if (unit->published[i].load(std::memory_order_relaxed) == v) continue;
    unit->published[i].store(v, std::memory_order_relaxed);
    send<void>(unit->parameters[i], "setValue:originator:", (Value) v, (void*) nullptr);
  }
  publishingHere() = false;
}

/** [main-thread] Every parameter moved at once -- a preset, a restored state
 *  -- and the host wrote none of them: each pushed into its AUParameter, and
 *  AUv3's one notification for the lot. */
inline void announceAllParameters(Unit* unit) {
  unit->shared.uiEchoValid = false;
  publishParameters(unit);
  announce(unit->self, "allParameterValues");
}

// ── Presets and state ────────────────────────────────────────────────────────

inline id makePreset(ObjcInteger number, const char* name) {
  id preset = send<id>(send<id>(classNamed("AUAudioUnitPreset"), "alloc"), "init");
  if (!preset) return nil;
  send<void>(preset, "setNumber:", number);
  send<void>(preset, "setName:", string(name));
  return preset; // +1
}

inline void setCurrentPresetObject(Unit* unit, id preset /* +1 or nil */) {
  release(unit->currentPreset);
  unit->currentPreset = preset;
}

inline void buildFactoryPresets(Unit* unit) {
  std::vector<id> list;
  for (int i = 0; i < kDesc.numPresets && kDesc.presets; ++i) {
    id preset = makePreset(i, kDesc.presets[i].name);
    if (!preset) continue;
    list.push_back(preset);
  }
  unit->factoryPresets = retain(array(list.data(), (ObjcUInteger) list.size()));
  for (id preset : list) release(preset); // the array holds them now
}

/** [main-thread] Factory preset `index`, as AUv2's PresentPreset applies one:
 *  the whole set in one step, through the bounded helper. */
inline bool applyFactoryPreset(Unit* unit, int index) {
  if (index < 0 || index >= kDesc.numPresets || !kDesc.presets) return false;
  float values[SONORE_NUM_PARAMS > 0 ? SONORE_NUM_PARAMS : 1];
  if (!clapwrap::factoryPresetValues(kDesc, index, values, SONORE_NUM_PARAMS)) return false;
  clapwrap::writeParamsTogether(&unit->shared, [&] {
    for (int i = 0; i < kDesc.numParams && i < SONORE_NUM_PARAMS; ++i) unit->shared.params[i] = values[i];
  });
  clapwrap::presetsOf(&unit->shared).noteFactoryPresetApplied(index);
  return true;
}

/** The state as AUv2's ClassInfo dictionary: the identity fields a host
 *  checks, the preset's name, and the same versioned SNRS blob every format
 *  writes. CF objects, which ARE the Foundation ones (toll-free bridging). */
inline CFMutableDictionaryRef buildState(Unit* unit) {
  CFMutableDictionaryRef dict = CFDictionaryCreateMutable(
      nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
  if (!dict) return nullptr;
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
  {
    std::string name = "Default";
    if (unit->currentPreset) {
      const std::string current = utf8Of(send<id>(unit->currentPreset, "name"));
      if (!current.empty()) name = current;
    }
    CFStringRef value = CFStringCreateWithCString(nullptr, name.c_str(), kCFStringEncodingUTF8);
    CFDictionarySetValue(dict, CFSTR(kAUPresetNameKey), value);
    CFRelease(value);
  }
  std::vector<uint8_t> blob;
  clapwrap::saveStateBody(&unit->shared, [&blob](const void* d, size_t n) -> size_t {
    const auto* p = (const uint8_t*) d;
    blob.insert(blob.end(), p, p + n);
    return n;
  });
  CFDataRef data = CFDataCreate(nullptr, blob.empty() ? nullptr : blob.data(), (CFIndex) blob.size());
  CFDictionarySetValue(dict, CFSTR("SonoreState"), data);
  CFRelease(data);
  return dict;
}

/** Apply a dictionary buildState() wrote -- or AUv2's ClassInfo, which is the
 *  same layout. Another plugin's dictionary is refused before a byte of it is
 *  read, as AUv2 refuses one. */
inline bool applyState(Unit* unit, id state) {
  if (!state || CFGetTypeID((CFTypeRef) state) != CFDictionaryGetTypeID()) return false;
  auto dict = (CFDictionaryRef) state;
  auto matches = [dict](const char* key, OSType mine) {
    CFStringRef k = CFStringCreateWithCString(nullptr, key, kCFStringEncodingUTF8);
    auto value = (CFNumberRef) CFDictionaryGetValue(dict, k);
    CFRelease(k);
    if (!value || CFGetTypeID(value) != CFNumberGetTypeID()) return true;
    SInt32 theirs = 0;
    CFNumberGetValue(value, kCFNumberSInt32Type, &theirs);
    return (OSType) theirs == mine;
  };
  if (!matches(kAUPresetTypeKey, unit->componentDesc.componentType) ||
      !matches(kAUPresetSubtypeKey, unit->componentDesc.componentSubType) ||
      !matches(kAUPresetManufacturerKey, unit->componentDesc.componentManufacturer))
    return false;
  auto data = (CFDataRef) CFDictionaryGetValue(dict, CFSTR("SonoreState"));
  if (!data || CFGetTypeID(data) != CFDataGetTypeID()) return false;
  const UInt8* bytes = CFDataGetBytePtr(data);
  const CFIndex length = CFDataGetLength(data);
  size_t offset = 0;
  const bool loaded = clapwrap::loadStateBody(&unit->shared, [bytes, length, &offset](void* dst, size_t want) -> size_t {
    const size_t remaining = (size_t) length - offset;
    const size_t n = want < remaining ? want : remaining;
    if (n) std::memcpy(dst, bytes + offset, n);
    offset += n;
    return n;
  });
  if (!loaded) return false;
  auto name = (CFStringRef) CFDictionaryGetValue(dict, CFSTR(kAUPresetNameKey));
  if (name && CFGetTypeID(name) == CFStringGetTypeID()) {
    char text[256] = {};
    if (CFStringGetCString(name, text, sizeof(text), kCFStringEncodingUTF8))
      setCurrentPresetObject(unit, makePreset(-1, text));
  }
  return true;
}

// ── The main-thread clock ───────────────────────────────────────────────────

/** [main-thread] What render could only record, told to the host: the
 *  latency and tail (key-value notifications a host re-reads them on), the
 *  output guard's report, and state no parameter carries. */
inline void housekeepingTick(CFRunLoopTimerRef, void* info) {
  auto* unit = static_cast<Unit*>(info);
  // The Unit outlives every tick (teardownUnit runs on this thread, after
  // the timer is invalidated); the object may not, hence the weak read.
  id self = unit ? objc_loadWeak(&unit->weakSelf) : nil;
  if (!self) return;
  if (clapwrap::HasLatency<SonoreDsp>::value) {
    const uint32_t latency = clapwrap::dspLatency(unit->shared.dsp);
    if (latency != unit->reportedLatency) {
      unit->reportedLatency = latency;
      unit->shared.reportedLatency = latency;
      announce(self, "latency");
    }
  }
  const uint32_t tail = clapwrap::dspTail(unit->shared.dsp);
  if (tail != unit->reportedTail) {
    unit->reportedTail = tail;
    announce(self, "tailTime");
  }
  // What render moved -- the host's own scheduled events, which land in the
  // DSP's values and not in the parameters' caches -- and what the DSP moved.
  publishParameters(unit);
  // AUv3 has no "my document changed" call. What a host does observe is
  // fullState, which it re-reads when the key-value notification fires.
  if (unit->stateDirtyPending.exchange(false, std::memory_order_acq_rel)) announce(self, "fullState");
  clapwrap::reportOutputGuard(&unit->shared);
}

inline void startHousekeeping(Unit* unit) {
  CFRunLoopTimerContext context{};
  context.info = unit;
  unit->housekeeping = CFRunLoopTimerCreate(nullptr, CFAbsoluteTimeGetCurrent() + 0.1, 0.1, 0, 0,
                                            &housekeepingTick, &context);
  if (unit->housekeeping)
    CFRunLoopAddTimer(CFRunLoopGetMain(), unit->housekeeping, kCFRunLoopCommonModes);
}

inline void stopHousekeeping(Unit* unit) {
  if (!unit->housekeeping) return;
  CFRunLoopTimerInvalidate(unit->housekeeping);
  CFRelease(unit->housekeeping);
  unit->housekeeping = nullptr;
}

/** The plugin changed its own state from its editor (a preset, an undo). */
inline void ownStateLoaded(void* context) {
  auto* unit = static_cast<Unit*>(context);
  announceAllParameters(unit);
  unit->stateDirtyPending.store(true, std::memory_order_release);
}

inline void presetSelected(void* context) {
  auto* unit = static_cast<Unit*>(context);
  const PresetManager::Selection& selection = unit->shared.presets.selection();
  const ObjcInteger number = selection.origin == PresetManager::Origin::Factory
                                 ? (ObjcInteger) selection.factoryIndex
                                 : -1;
  setCurrentPresetObject(unit, makePreset(number, selection.name.c_str()));
  announce(unit->self, "currentPreset");
}

// ── Render ───────────────────────────────────────────────────────────────────

/** [audio-thread] The host's timeline, from the two blocks it may have set.
 *  Beats are quarter notes, as every other format reads them. */
inline TransportInfo readTransport(const Unit* unit) {
  TransportInfo info;
  if (unit->musicalContext) {
    double tempo = 0.0, numerator = 0.0, beat = 0.0, downbeat = 0.0;
    ObjcInteger denominator = 0, toNextBeat = 0;
    if (callBlock<BOOL>(unit->musicalContext, &tempo, &numerator, &denominator, &beat, &toNextBeat,
                        &downbeat)) {
      if (tempo > 0.0) {
        info.tempo = tempo;
        info.hasTempo = true;
      }
      info.positionBeats = beat;
      info.hasBeats = true;
      if (numerator > 0.0 && denominator > 0) {
        info.timeSigNumerator = (int) (numerator + 0.5);
        info.timeSigDenominator = (int) denominator;
      }
      info.barStartBeats = downbeat;
      info.barNumber = (int32_t) std::floor(downbeat / info.beatsPerBar() + 0.5);
    }
  }
  if (unit->transportState) {
    ObjcUInteger flags = 0;
    double sample = 0.0, cycleStart = 0.0, cycleEnd = 0.0;
    if (callBlock<BOOL>(unit->transportState, &flags, &sample, &cycleStart, &cycleEnd)) {
      info.isPlaying = (flags & kTransportMoving) != 0;
      info.isRecording = (flags & kTransportRecording) != 0;
      info.isLooping = (flags & kTransportCycling) != 0;
      if (info.isLooping && cycleEnd > cycleStart) {
        info.loopStartBeats = cycleStart;
        info.loopEndBeats = cycleEnd;
        info.hasLoop = true;
      }
      if (unit->sampleRate > 0.0) {
        info.positionSeconds = sample / unit->sampleRate;
        info.hasSeconds = true;
      }
    }
  }
  return info;
}

/** `info` moved on by `frames`, for a piece that starts later in the block. */
inline void advanceTransport(TransportInfo& info, uint32_t frames, double sampleRate) {
  if (!info.isPlaying || frames == 0 || sampleRate <= 0.0) return;
  const double seconds = (double) frames / sampleRate;
  if (info.hasSeconds) info.positionSeconds += seconds;
  if (info.hasBeats && info.hasTempo) info.positionBeats += seconds * info.tempo / 60.0;
}

/** Where in this block an event falls, 0..frames-1. */
inline uint32_t eventOffset(const EventHeader* e, const AudioTimeStamp* timestamp, uint32_t frames) {
  EventSampleTime t = e->eventSampleTime;
  EventSampleTime offset;
  if (t >= kEventSampleTimeImmediate && t < kEventSampleTimeImmediate + 4096)
    offset = t - kEventSampleTimeImmediate;
  else
    offset = t - (EventSampleTime) (timestamp ? timestamp->mSampleTime : 0.0);
  if (offset < 0) offset = 0;
  if (frames > 0 && offset >= (EventSampleTime) frames) offset = (EventSampleTime) frames - 1;
  return (uint32_t) offset;
}

inline bool isParameterEvent(const EventHeader* e) {
  return e->eventType == kEventParameter || e->eventType == kEventParameterRamp;
}

/** Where the piece starting at `start` ends: the next parameter change at
 *  least `granularity` frames in, as clapwrap::pieceEnd cuts a CLAP block. */
inline uint32_t pieceEnd(const EventHeader* e, const AudioTimeStamp* ts, uint32_t frames,
                         uint32_t start, uint32_t granularity) {
  if (granularity == 0) return frames;
  for (; e; e = e->next) {
    if (!isParameterEvent(e)) continue;
    const uint32_t t = eventOffset(e, ts, frames);
    if (t < start + granularity) continue;
    uint32_t cut = t;
    if (cut + granularity > frames) cut = frames - granularity;
    return cut >= start + granularity ? cut : frames;
  }
  return frames;
}

/** [audio-thread] One render event into the block: a parameter written, a
 *  MIDI message queued at its offset within the piece. */
inline void applyEvent(Unit* unit, const EventHeader* e, uint32_t offsetInPiece) {
  switch (e->eventType) {
    case kEventParameter:
    case kEventParameterRamp: {
      // A ramp lands on its destination at its start: the DSP's own
      // smoothing does the rest, as it does for every other format's steps.
      const auto* p = reinterpret_cast<const ParameterEvent*>(e);
      if (p->parameterAddress < (ParameterAddress) kDesc.numParams &&
          p->parameterAddress < (ParameterAddress) SONORE_NUM_PARAMS) {
        const int i = (int) p->parameterAddress;
        unit->shared.params[i] = clampToRange(kDesc.params[i], p->value);
      }
      return;
    }
    case kEventMIDI: {
      const auto* m = reinterpret_cast<const MidiEvent*>(e);
      if (m->length < 1) return;
      const int status = m->data[0];
      if (status < 0x80) return;
      unit->shared.midi.addEvent(
          MidiMessage(status, m->length > 1 ? m->data[1] : 0, m->length > 2 ? m->data[2] : 0),
          (int) offsetInPiece);
      return;
    }
    case kEventMIDISysEx: {
      const auto* m = reinterpret_cast<const MidiEvent*>(e);
      if (m->length > 0) unit->shared.midi.addSysex(m->data, m->length, (int) offsetInPiece);
      return;
    }
    case kEventMIDIEventList: {
      // Universal MIDI Packets: a MIDI 1.0 channel-voice word as it is, a
      // MIDI 2.0 one narrowed to 7 bits (ump.h says what that gives up).
      const auto* list = &reinterpret_cast<const MidiEventListEvent*>(e)->eventList;
      const UmpPacket* packet = &list->packet[0];
      for (uint32_t n = 0; n < list->numPackets; ++n) {
        const uint32_t count = packet->wordCount < 64 ? packet->wordCount : 64;
        for (uint32_t w = 0; w < count;) {
          const int size = ump::packetWords(packet->words[w]);
          if (size <= 0 || w + (uint32_t) size > count) break;
          MidiMessage message;
          if (ump::toMidi1(packet->words[w], &message) ||
              ump::toMidi1Narrowing(&packet->words[w], (size_t) size, &message))
            unit->shared.midi.addEvent(message, (int) offsetInPiece);
          w += (uint32_t) size;
        }
        packet = reinterpret_cast<const UmpPacket*>(
            reinterpret_cast<const uint8_t*>(&packet->words[0]) + sizeof(uint32_t) * count);
      }
      return;
    }
    default:
      return;
  }
}

/** [audio-thread] What the editors queued, applied before the block. */
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
        unit->shared.midi.addEvent(MidiMessage::noteOn(0, e.index, velocity < 1 ? 1 : velocity), 0);
        break;
      }
      case UiEventQueue::Event::Kind::NoteOff:
        unit->shared.midi.addEvent(MidiMessage::noteOff(0, e.index), 0);
        break;
      default:
        break; // gestures went to the host from the main thread
    }
  }
}

/** [audio-thread] One piece of the block through the DSP: frames
 *  [start, start + len) of every bus, with the MIDI already queued for it. */
inline void renderPiece(Unit* unit, float* const* chans, uint32_t nch, float* const* sc,
                        float* const (*aux)[clapwrap::kMaxAudioChannels], uint32_t nAux,
                        const AudioTimeStamp* timestamp, uint32_t start, uint32_t len,
                        const TransportInfo& transportAtBlock) {
  float* pc[clapwrap::kMaxAudioChannels];
  for (uint32_t c = 0; c < nch; ++c) pc[c] = chans[c] + start;
  float* ps[2] = {sc[0] ? sc[0] + start : nullptr, sc[1] ? sc[1] + start : nullptr};

  if (clapwrap::HasLatency<SonoreDsp>::value) unit->shared.bypass.follow(clapwrap::dspLatency(unit->shared.dsp));
  clapwrap::bypassCapture(unit->shared.bypass, pc, nch, len);
  AudioBlock<float> block(pc, nch, (size_t) len);
  AudioBlock<float> scBlock(ps, 2, ps[0] ? (size_t) len : 0);

  AudioBlock<float> auxBlocks[clapwrap::kMaxAuxOutputs] = {};
  float* auxPtrs[clapwrap::kMaxAuxOutputs][clapwrap::kMaxAudioChannels] = {};
  for (uint32_t b = 0; b < nAux; ++b) {
    const uint32_t width = clapwrap::auxBusChannels(b);
    for (uint32_t c = 0; c < width; ++c) auxPtrs[b][c] = aux[b][c] + start;
    auxBlocks[b] = AudioBlock<float>(auxPtrs[b], width, (size_t) len);
  }

  unit->shared.midiOut.clear();
  uint8_t roles[clapwrap::kMaxAudioChannels];
  const uint32_t numRoles = rolesFromMask(defaultChannelMask(unit->chans), roles, clapwrap::kMaxAudioChannels);
  unit->shared.expression.clear();
  unit->shared.midi.clampPositions((int) len);
  clapwrap::snapshotParams(&unit->shared);
  clapwrap::decodeMpe(&unit->shared, unit->shared.midi, unit->shared.paramsBlock);
  ProcessContext ctx{block,
                     auxBlocks,
                     nAux,
                     scBlock,
                     unit->shared.midi,
                     unit->shared.midiOut,
                     numRoles ? roles : nullptr,
                     &unit->shared.expression,
                     nullptr,
                     true,
                     unit->shared.offline};
  trackNotes(unit->shared.notes, ctx.midi);
  if (clapwrap::WantsTransport<SonoreDsp>::value) {
    TransportInfo transport = transportAtBlock;
    advanceTransport(transport, start, unit->sampleRate);
    clapwrap::sendTransport(unit->shared.dsp, transport);
  }
  clapwrap::runDspCtx(unit->shared.dsp, ctx, clapwrap::dspParams(&unit->shared));

  // Emitted MIDI, through the host's block, at its frame on the host's clock.
  if (kDesc.producesMidi && unit->midiOutput && !unit->shared.midiOut.isEmpty()) {
    const double base = timestamp ? timestamp->mSampleTime : 0.0;
    for (const auto& e : unit->shared.midiOut) {
      const uint8_t bytes[3] = {(uint8_t) e.message.getRawStatus(), (uint8_t) e.message.getRawData1(),
                                (uint8_t) e.message.getRawData2()};
      const int length = shortMessageLength(bytes[0]);
      if (length <= 0) continue;
      const EventSampleTime when =
          (EventSampleTime) base + (EventSampleTime) start + (EventSampleTime) (e.samplePosition < 0 ? 0 : e.samplePosition);
      callBlock<OSStatus>(unit->midiOutput, when, (uint8_t) 0, (ObjcInteger) length, (const uint8_t*) bytes);
    }
  }
  if (clapwrap::dspConsumeStateDirty(unit->shared.dsp))
    unit->stateDirtyPending.store(true, std::memory_order_release);
  clapwrap::bypassApply(unit->shared.bypass, pc, nch, len);
  unit->shared.midi.clear();
}

/** The render block's invoke function: AUInternalRenderBlock. */
inline int32_t render(void* block, AudioUnitRenderActionFlags* flags, const AudioTimeStamp* timestamp,
                      FrameCount frames, ObjcInteger outputBus, AudioBufferList* io,
                      const EventHeader* events, void* pullInput) {
  Unit* unit = ::sonore::detail::blockCapture<Unit*, RenderInvoke>(block);
  const auto blockStart = std::chrono::steady_clock::now();
  const uint32_t nAux = clapwrap::numAuxOutputs();
  if (!unit || !unit->prepared) return kAudioUnitErr_Uninitialized;
  if (outputBus < 0 || (uint32_t) outputBus > nAux || !io || io->mNumberBuffers < 1)
    return kAudioUnitErr_InvalidParameter;
  if (frames > unit->maxFrames) return kAudioUnitErr_TooManyFramesToProcess;

  // An aux bus: what bus 0 computed for this timestamp, as AUv2 hands it out.
  if (outputBus > 0) {
    const uint32_t b = (uint32_t) outputBus - 1;
    const uint32_t width = clapwrap::auxBusChannels(b);
    const bool fresh = timestamp && unit->auxRenderedAt == timestamp->mSampleTime;
    uint32_t flat = 0;
    for (uint32_t i = 0; i < b; ++i) flat += clapwrap::auxBusChannels(i);
    for (UInt32 c = 0; c < io->mNumberBuffers && c < width; ++c) {
      if (!io->mBuffers[c].mData) {
        io->mBuffers[c].mData = unit->auxScratch[flat + c].data();
        io->mBuffers[c].mDataByteSize = frames * (UInt32) sizeof(float);
        continue; // the scratch already holds what bus 0 rendered
      }
      float* dst = (float*) io->mBuffers[c].mData;
      const auto& src = unit->auxScratch[flat + c];
      if (fresh && src.size() >= frames) std::memcpy(dst, src.data(), sizeof(float) * frames);
      else std::memset(dst, 0, sizeof(float) * frames);
    }
    return noErr;
  }

  uint32_t nch = unit->chans;
  if (io->mNumberBuffers < nch) nch = io->mNumberBuffers;
  if (nch > clapwrap::kMaxAudioChannels) nch = clapwrap::kMaxAudioChannels;
  float* chans[clapwrap::kMaxAudioChannels];
  for (uint32_t c = 0; c < nch; ++c) {
    // Null mData: "render into your own buffer and say where it is".
    if (!io->mBuffers[c].mData) {
      if (unit->outScratch.size() < (size_t) (c + 1) * unit->maxFrames) return kAudioUnitErr_TooManyFramesToProcess;
      io->mBuffers[c].mData = unit->outScratch.data() + (size_t) c * unit->maxFrames;
    }
    io->mBuffers[c].mDataByteSize = frames * (UInt32) sizeof(float);
    chans[c] = (float*) io->mBuffers[c].mData;
  }

  if (kDesc.isInstrument || !pullInput) {
    for (uint32_t c = 0; c < nch; ++c) std::memset(chans[c], 0, sizeof(float) * frames);
  } else {
    // Pulled into the output buffers, which is where the DSP works in place.
    // The source may answer with buffers of its own instead: then copied.
    struct {
      AudioBufferList list;
      AudioBuffer more[clapwrap::kMaxAudioChannels];
    } in{};
    in.list.mNumberBuffers = nch;
    for (uint32_t c = 0; c < nch; ++c) {
      in.list.mBuffers[c].mNumberChannels = 1;
      in.list.mBuffers[c].mDataByteSize = frames * (UInt32) sizeof(float);
      in.list.mBuffers[c].mData = chans[c];
    }
    AudioUnitRenderActionFlags pullFlags = 0;
    const int32_t r = callBlock<int32_t>(pullInput, &pullFlags, timestamp, frames, (ObjcInteger) 0, &in.list);
    if (r != noErr) return r;
    for (uint32_t c = 0; c < nch; ++c)
      if (in.list.mBuffers[c].mData && in.list.mBuffers[c].mData != chans[c])
        std::memcpy(chans[c], in.list.mBuffers[c].mData, sizeof(float) * frames);
  }

  // The sidechain: input bus 1, pulled into our scratch, silent if unwired.
  float* sc[2] = {nullptr, nullptr};
  if constexpr (clapwrap::TakesSidechain<SonoreDsp>::value) {
    if (!kDesc.isInstrument && unit->scScratch[0].size() >= frames && unit->scScratch[1].size() >= frames) {
      sc[0] = unit->scScratch[0].data();
      sc[1] = unit->scScratch[1].data();
      std::memset(sc[0], 0, sizeof(float) * frames);
      std::memset(sc[1], 0, sizeof(float) * frames);
      if (pullInput) {
        struct {
          AudioBufferList list;
          AudioBuffer extra;
        } key{};
        key.list.mNumberBuffers = 2;
        for (UInt32 c = 0; c < 2; ++c) {
          key.list.mBuffers[c].mNumberChannels = 1;
          key.list.mBuffers[c].mDataByteSize = frames * (UInt32) sizeof(float);
          key.list.mBuffers[c].mData = sc[c];
        }
        AudioUnitRenderActionFlags pullFlags = 0;
        if (callBlock<int32_t>(pullInput, &pullFlags, timestamp, frames, (ObjcInteger) 1, &key.list) == noErr) {
          for (UInt32 c = 0; c < 2; ++c)
            if (key.list.mBuffers[c].mData && key.list.mBuffers[c].mData != sc[c])
              std::memcpy(sc[c], key.list.mBuffers[c].mData, sizeof(float) * frames);
        } else {
          std::memset(sc[0], 0, sizeof(float) * frames);
          std::memset(sc[1], 0, sizeof(float) * frames);
        }
      }
    }
  }

  float* aux[clapwrap::kMaxAuxOutputs][clapwrap::kMaxAudioChannels] = {};
  AudioBlock<float> auxWhole[clapwrap::kMaxAuxOutputs] = {};
  {
    uint32_t flat = 0;
    for (uint32_t b = 0; b < nAux; ++b) {
      const uint32_t width = clapwrap::auxBusChannels(b);
      for (uint32_t c = 0; c < width; ++c, ++flat) {
        if (unit->auxScratch[flat].size() < frames) return kAudioUnitErr_TooManyFramesToProcess;
        aux[b][c] = unit->auxScratch[flat].data();
      }
      auxWhole[b] = AudioBlock<float>(aux[b], width, (size_t) frames);
    }
  }

  unit->shared.bypass.engaged = unit->bypassed.load(std::memory_order_relaxed);
  drainUiEvents(unit);
  const TransportInfo transport = clapwrap::WantsTransport<SonoreDsp>::value ? readTransport(unit) : TransportInfo();

  // The block in pieces where a parameter moves, if the descriptor asks for
  // sample-accurate automation; else whole, with every parameter at its
  // last value. Events are in time order, as a render list is.
  const uint32_t granularity =
      kDesc.automationGranularity > 0 ? (uint32_t) kDesc.automationGranularity : 0u;
  const EventHeader* e = events;
  for (uint32_t start = 0; start < frames;) {
    const uint32_t end = pieceEnd(e, timestamp, frames, start, granularity);
    const bool last = end >= frames;
    while (e) {
      const uint32_t t = eventOffset(e, timestamp, frames);
      if (!last && t >= end) break;
      applyEvent(unit, e, t > start ? t - start : 0);
      e = e->next;
    }
    renderPiece(unit, chans, nch, sc, aux, nAux, timestamp, start, end - start, transport);
    start = end;
  }

  unit->auxRenderedAt = timestamp ? timestamp->mSampleTime : -1.0;
  clapwrap::guardOutputs(&unit->shared, chans, nch, auxWhole, nAux, frames);
  unit->shared.meter.push(measureBlock(chans[0], (size_t) frames));
  unit->shared.scope.push(chans, 1, (uint32_t) frames);
  unit->shared.load.record(
      std::chrono::duration<double>(std::chrono::steady_clock::now() - blockStart).count(), (uint32_t) frames);
  if (flags) *flags &= ~kAudioUnitRenderAction_OutputIsSilence;
  return noErr;
}

// ── Buses ────────────────────────────────────────────────────────────────────

/** A bus at `channels` deinterleaved floats: AVAudioFormat's standard format. */
inline id makeBus(double sampleRate, uint32_t channels, uint32_t maxChannels) {
  id format = send<id>(send<id>(classNamed("AVAudioFormat"), "alloc"),
                       "initStandardFormatWithSampleRate:channels:", sampleRate, channels);
  if (!format) return nil;
  id error = nil;
  id bus = send<id>(send<id>(classNamed("AUAudioUnitBus"), "alloc"), "initWithFormat:error:", format, &error);
  release(format);
  if (!bus) return nil;
  send<void>(bus, "setMaximumChannelCount:", maxChannels);
  return bus; // +1
}

inline id makeBusArray(id self, ObjcInteger type, const std::vector<id>& buses) {
  id busArray = send<id>(send<id>(classNamed("AUAudioUnitBusArray"), "alloc"), "initWithAudioUnit:busType:busses:",
                         self, type, array(buses.data(), (ObjcUInteger) buses.size()));
  for (id bus : buses) release(bus); // the array holds them
  return busArray; // +1
}

inline id busAt(id busArray, ObjcUInteger index) {
  if (!busArray || send<ObjcUInteger>(busArray, "count") <= index) return nil;
  return send<id>(busArray, "objectAtIndexedSubscript:", index);
}

inline uint32_t busChannels(id bus) {
  id format = bus ? send<id>(bus, "format") : nil;
  return format ? send<uint32_t>(format, "channelCount") : 0;
}

inline double busRate(id bus) {
  id format = bus ? send<id>(bus, "format") : nil;
  return format ? send<double>(format, "sampleRate") : 0.0;
}

// ── The AUAudioUnit subclass ─────────────────────────────────────────────────

inline Class audioUnitBaseClass() { return objc_getClass("AUAudioUnit"); }
inline Class audioUnitClass();

/** -initWithComponentDescription:options:error: */
inline id auInit(id self, SEL, AudioComponentDescription desc, uint32_t options, id* outError) {
  // The Unit first, so a getter the superclass calls while it initialises
  // finds one -- with no buses yet, which every getter here allows for.
  auto* unit = new (std::nothrow) Unit();
  if (!unit) {
    setError(outError, kAudioUnitErr_FailedInitialization);
    release(self);
    return nil;
  }
  unit->self = self;
  objc_storeWeak(&unit->weakSelf, self);
  unit->componentDesc = desc;
  object_setInstanceVariable(self, kUnitIvar, unit);
  self = sendSuper<id>(self, audioUnitBaseClass(), "initWithComponentDescription:options:error:", desc, options,
                       outError);
  if (!self) return nil; // the superclass released it, and dealloc deleted the Unit

  unit->shared.formatContext = unit;
  unit->shared.onOwnStateLoad = &ownStateLoaded;
  unit->shared.onPresetSelected = &presetSelected;
  unit->chans = clapwrap::defaultMainChannels();
  for (int i = 0; i < SONORE_NUM_PARAMS && i < kDesc.numParams; ++i)
    unit->shared.params[i] = kDesc.params[i].defaultValue;

  const uint32_t maxCh = clapwrap::maxMainChannels();
  std::vector<id> outs{makeBus(unit->sampleRate, unit->chans, maxCh)};
  for (uint32_t b = 0; b < clapwrap::numAuxOutputs(); ++b) {
    const uint32_t width = clapwrap::auxBusChannels(b);
    outs.push_back(makeBus(unit->sampleRate, width, width));
  }
  std::vector<id> ins;
  if (!kDesc.isInstrument) {
    ins.push_back(makeBus(unit->sampleRate, unit->chans, maxCh));
    if (clapwrap::TakesSidechain<SonoreDsp>::value) ins.push_back(makeBus(unit->sampleRate, 2, 2));
  }
  for (id bus : outs)
    if (!bus) {
      setError(outError, kAudioUnitErr_FailedInitialization);
      release(self);
      return nil;
    }
  unit->outputBusArray = makeBusArray(self, kBusTypeOutput, outs);
  unit->inputBusArray = makeBusArray(self, kBusTypeInput, ins);

  buildParameterTree(unit);
  buildFactoryPresets(unit);
  setCurrentPresetObject(unit, makePreset(-1, "Default"));
  ::sonore::detail::makeGlobalBlock(unit->renderBlock, &render, unit);
  // At the largest the host may ask for, so the most this unit can be handed.
  send<void>(self, "setMaximumFramesToRender:", (FrameCount) 4096);
  startHousekeeping(unit);
  return self;
}

/** [main-thread] What only the main thread may take down: the clock and an
 *  open editor. Then the Unit itself. */
inline void teardownUnit(void* context) {
  auto* unit = static_cast<Unit*>(context);
  stopHousekeeping(unit);
  unit->shared.nativeEditor.close();
#if defined(SONORE_HAS_WEBVIEW_BACKEND)
  unit->shared.webview.destroy();
#endif
  delete unit;
}

/** -dealloc. A host may release its last reference on any thread.
 *
 *  The Unit is taken down only after AUAudioUnit's own dealloc has run: that
 *  releases the blocks it made around ours -- the render block it wraps -- and
 *  ours live in the Unit, so a Unit already deleted is read there. (The Mac's
 *  allocator happened to leave the memory readable; the iOS simulator's had
 *  unmapped it, and the self-test crashed in AUEventSchedule's destructor.) */
inline void auDealloc(id self, SEL) {
  Unit* unit = unitOf(self);
  if (unit) {
    object_setInstanceVariable(self, kUnitIvar, nullptr);
    unit->self = nil;
    objc_storeWeak(&unit->weakSelf, nil);
    releaseBlock(unit->musicalContext);
    releaseBlock(unit->transportState);
    releaseBlock(unit->midiOutput);
    unit->musicalContext = unit->transportState = unit->midiOutput = nullptr;
    // A host may hold on to the tree after the unit: its blocks live in the
    // Unit, so they are taken off it before the Unit goes.
    if (unit->parameterTree) {
      send<void>(unit->parameterTree, "setImplementorValueObserver:", (void*) nullptr);
      send<void>(unit->parameterTree, "setImplementorValueProvider:", (void*) nullptr);
      send<void>(unit->parameterTree, "setImplementorStringFromValueCallback:", (void*) nullptr);
      send<void>(unit->parameterTree, "setImplementorValueFromStringCallback:", (void*) nullptr);
    }
    release(unit->parameterTree);
    unit->parameterTree = nil;
    for (id& p : unit->parameters) {
      release(p);
      p = nil;
    }
    release(unit->factoryPresets);
    release(unit->currentPreset);
    release(unit->inputBusArray);
    release(unit->outputBusArray);
    unit->factoryPresets = unit->currentPreset = unit->inputBusArray = unit->outputBusArray = nil;
  }
  sendSuper<void>(self, audioUnitBaseClass(), "dealloc");
  if (unit) {
    if (isMainThread()) teardownUnit(unit);
    else onMainThread(unit, &teardownUnit);
  }
}

inline id auInputBusses(id self, SEL) {
  Unit* unit = unitOf(self);
  return unit ? unit->inputBusArray : nil;
}

inline id auOutputBusses(id self, SEL) {
  Unit* unit = unitOf(self);
  return unit ? unit->outputBusArray : nil;
}

inline void* auInternalRenderBlock(id self, SEL) {
  Unit* unit = unitOf(self);
  return unit && unit->renderBlock.invoke ? (void*) &unit->renderBlock : nullptr;
}

/** -allocateRenderResourcesAndReturnError: */
inline BOOL auAllocate(id self, SEL, id* outError) {
  Unit* unit = unitOf(self);
  if (!unit) return NO;
  unit->prepared = false;
  if (!sendSuper<BOOL>(self, audioUnitBaseClass(), "allocateRenderResourcesAndReturnError:", outError)) return NO;

  id out = busAt(unit->outputBusArray, 0);
  const uint32_t channels = busChannels(out);
  const double rate = busRate(out);
  bool ok = rate > 0.0 && channels >= clapwrap::minMainChannels() && channels <= clapwrap::maxMainChannels();
  // In = out for an effect: the only configuration channelCapabilities offers.
  if (ok && !kDesc.isInstrument) ok = busChannels(busAt(unit->inputBusArray, 0)) == channels;
  if (!ok) {
    sendSuper<void>(self, audioUnitBaseClass(), "deallocateRenderResources");
    setError(outError, kAudioUnitErr_FormatNotSupported);
    return NO;
  }
  unit->chans = channels;
  unit->sampleRate = rate;
  unit->maxFrames = send<FrameCount>(self, "maximumFramesToRender");
  if (unit->maxFrames < 1) unit->maxFrames = 512;

  // The host's blocks, copied now: render may not ask the object for them.
  releaseBlock(unit->musicalContext);
  releaseBlock(unit->transportState);
  releaseBlock(unit->midiOutput);
  unit->musicalContext = copyBlock(send<void*>(self, "musicalContextBlock"));
  unit->transportState = copyBlock(send<void*>(self, "transportStateBlock"));
  unit->midiOutput = kDesc.producesMidi ? copyBlock(send<void*>(self, "MIDIOutputEventBlock")) : nullptr;

  unit->shared.sampleRate = rate;
  unit->shared.maxFrames = unit->maxFrames;
  unit->shared.mainChannels = channels;
  unit->shared.setMainLayout(channels, nullptr);
  clapwrap::prepareDsp(&unit->shared); // the one place allocation is allowed
  unit->reportedLatency = unit->shared.reportedLatency = clapwrap::dspLatency(unit->shared.dsp);
  unit->reportedTail = clapwrap::dspTail(unit->shared.dsp);
  unit->shared.bypass.prepare(
      rate, unit->maxFrames, clapwrap::dspLatency(unit->shared.dsp), channels,
      clapwrap::HasLatency<SonoreDsp>::value
          ? clapwrap::BypassState::roomFor(clapwrap::dspLatency(unit->shared.dsp), rate)
          : 0);
  unit->outScratch.assign((size_t) channels * unit->maxFrames, 0.0f);
  if constexpr (clapwrap::TakesSidechain<SonoreDsp>::value)
    if (!kDesc.isInstrument)
      for (auto& v : unit->scScratch) v.assign(unit->maxFrames, 0.0f);
  {
    uint32_t flat = 0;
    for (uint32_t b = 0; b < clapwrap::numAuxOutputs(); ++b)
      for (uint32_t c = 0; c < clapwrap::auxBusChannels(b); ++c, ++flat)
        unit->auxScratch[flat].assign(unit->maxFrames, 0.0f);
  }
  unit->auxRenderedAt = -1.0;
  unit->shared.activated = true;
  unit->prepared = true;
  return YES;
}

inline void auDeallocate(id self, SEL) {
  if (Unit* unit = unitOf(self)) {
    unit->prepared = false;
    unit->shared.activated = false;
    releaseBlock(unit->musicalContext);
    releaseBlock(unit->transportState);
    releaseBlock(unit->midiOutput);
    unit->musicalContext = unit->transportState = unit->midiOutput = nullptr;
    clapwrap::reportOutputGuard(&unit->shared);
  }
  sendSuper<void>(self, audioUnitBaseClass(), "deallocateRenderResources");
}

inline void auReset(id self, SEL) {
  sendSuper<void>(self, audioUnitBaseClass(), "reset");
  Unit* unit = unitOf(self);
  if (!unit || !unit->prepared) return;
  clapwrap::resetDsp(&unit->shared);
  unit->shared.bypass.prepare(
      unit->sampleRate, unit->maxFrames, clapwrap::dspLatency(unit->shared.dsp), unit->chans,
      clapwrap::HasLatency<SonoreDsp>::value
          ? clapwrap::BypassState::roomFor(clapwrap::dspLatency(unit->shared.dsp), unit->sampleRate)
          : 0);
}

inline id auParameterTree(id self, SEL) {
  Unit* unit = unitOf(self);
  return unit ? unit->parameterTree : nil;
}

inline id auFullState(id self, SEL) {
  Unit* unit = unitOf(self);
  if (!unit) return nil;
  CFMutableDictionaryRef dict = buildState(unit);
  return dict ? (id) CFAutorelease(dict) : nil;
}

inline void auSetFullState(id self, SEL, id state) {
  Unit* unit = unitOf(self);
  if (!unit || !applyState(unit, state)) return;
  announceAllParameters(unit);
  announce(self, "currentPreset");
}

inline id auFactoryPresets(id self, SEL) {
  Unit* unit = unitOf(self);
  return unit ? unit->factoryPresets : nil;
}

inline id auCurrentPreset(id self, SEL) {
  Unit* unit = unitOf(self);
  return unit ? unit->currentPreset : nil;
}

inline void auSetCurrentPreset(id self, SEL, id preset) {
  Unit* unit = unitOf(self);
  if (!unit || !preset) return;
  const ObjcInteger number = send<ObjcInteger>(preset, "number");
  // A factory preset is applied. A negative number is the host's own (a
  // user preset), whose state the host restores through fullState.
  if (number >= 0 && !applyFactoryPreset(unit, (int) number)) return;
  setCurrentPresetObject(unit, retain(preset));
  announceAllParameters(unit);
}

inline double auLatency(id self, SEL) {
  Unit* unit = unitOf(self);
  if (!unit || unit->sampleRate <= 0.0) return 0.0;
  return (double) clapwrap::dspLatency(unit->shared.dsp) / unit->sampleRate;
}

inline double auTailTime(id self, SEL) {
  Unit* unit = unitOf(self);
  if (!unit || unit->sampleRate <= 0.0) return 0.0;
  return (double) clapwrap::dspTail(unit->shared.dsp) / unit->sampleRate;
}

/** Pairs of input/output channel counts: N in, N out for every N the DSP
 *  handles; an instrument has no input (0). */
inline id auChannelCapabilities(id, SEL) {
  std::vector<id> pairs;
  for (uint32_t n = clapwrap::minMainChannels(); n <= clapwrap::maxMainChannels(); ++n) {
    if (!clapwrap::channelCountAllowed(n)) continue;
    pairs.push_back(number(kDesc.isInstrument ? 0 : (long) n));
    pairs.push_back(number((long) n));
  }
  return array(pairs.data(), (ObjcUInteger) pairs.size());
}

inline BOOL auCanProcessInPlace(id, SEL) { return YES; }

inline BOOL auShouldBypass(id self, SEL) {
  Unit* unit = unitOf(self);
  return unit && unit->bypassed.load(std::memory_order_relaxed) ? YES : NO;
}

/** The bypass is ours, not the superclass's: the shared crossfade, with the
 *  dry path delayed by the latency, as every other format bypasses. */
inline void auSetShouldBypass(id self, SEL, BOOL bypass) {
  if (Unit* unit = unitOf(self)) unit->bypassed.store(bypass != NO, std::memory_order_relaxed);
}

inline id auMidiOutputNames(id, SEL) {
  if (!kDesc.producesMidi) return array(nullptr, 0);
  id name = string("MIDI Out");
  return array(&name, 1);
}

inline BOOL auProvidesUserInterface(id, SEL) {
  return clapwrap::editorChoice().backend != EditorBackend::None ? YES : NO;
}

inline BOOL auSupportsMpe(id, SEL) { return kDesc.supportsMpe ? YES : NO; }

/** The track's name, which a host sets on the unit: the DSP is told, as
 *  every format with a way to say it tells it. */
inline void auSetContextName(id self, SEL, id name) {
  sendSuper<void>(self, audioUnitBaseClass(), "setContextName:", name);
  Unit* unit = unitOf(self);
  if (!unit) return;
  unit->shared.trackInfo.name = utf8Of(name);
  unit->shared.trackInfo.hasName = name != nil;
  clapwrap::sendTrackInfo(unit->shared.dsp, unit->shared.trackInfo);
}

/** A host proposing a bus format: refused when the DSP cannot run at it,
 *  before it is set rather than at allocation. */
inline BOOL auShouldChangeToFormat(id self, SEL, id format, id bus) {
  Unit* unit = unitOf(self);
  if (!unit || !format) return NO;
  const uint32_t channels = send<uint32_t>(format, "channelCount");
  const double rate = send<double>(format, "sampleRate");
  if (rate <= 0.0) return NO;
  for (uint32_t b = 0; b < clapwrap::numAuxOutputs(); ++b)
    if (bus == busAt(unit->outputBusArray, b + 1)) return channels == clapwrap::auxBusChannels(b) ? YES : NO;
  if (clapwrap::TakesSidechain<SonoreDsp>::value && bus == busAt(unit->inputBusArray, 1))
    return channels == 1 || channels == 2 ? YES : NO;
  if (!clapwrap::channelCountAllowed(channels)) return NO;
  return sendSuper<BOOL>(self, audioUnitBaseClass(), "shouldChangeToFormat:forBus:", format, bus);
}

/** An override, registered with the encoding of the method it replaces. */
inline void addOverride(Class cls, Class base, const char* name, IMP imp, const char* fallback) {
  SEL s = sel_registerName(name);
  Method m = base ? class_getInstanceMethod(base, s) : nullptr;
  const char* types = m ? method_getTypeEncoding(m) : fallback;
  class_addMethod(cls, s, imp, types ? types : fallback);
}

// BOOL's encoding: `c` where it is a signed char, `B` where it is bool.
#if defined(__arm64__) || defined(__aarch64__)
#define SONORE_AUV3_BOOL "B"
#else
#define SONORE_AUV3_BOOL "c"
#endif

/** The AUAudioUnit subclass, registered once per image. */
inline Class audioUnitClass() {
  static Class klass = [] {
    Class base = audioUnitBaseClass();
    if (!base) return (Class) nullptr;
    char name[96];
    cocoa::uniqueClassName("SonoreAUv3Unit", (void*) &auInit, name, sizeof(name));
    if (Class existing = objc_getClass(name)) return existing;
    Class c = objc_allocateClassPair(base, name, 0);
    if (!c) return (Class) nullptr;
    class_addIvar(c, kUnitIvar, sizeof(void*), (uint8_t) (sizeof(void*) == 8 ? 3 : 2), "^v");
    addOverride(c, base, "initWithComponentDescription:options:error:", (IMP) auInit,
                "@@:{AudioComponentDescription=IIIII}I^@");
    addOverride(c, base, "dealloc", (IMP) auDealloc, "v@:");
    addOverride(c, base, "inputBusses", (IMP) auInputBusses, "@@:");
    addOverride(c, base, "outputBusses", (IMP) auOutputBusses, "@@:");
    addOverride(c, base, "internalRenderBlock", (IMP) auInternalRenderBlock, "@?@:");
    addOverride(c, base, "allocateRenderResourcesAndReturnError:", (IMP) auAllocate, SONORE_AUV3_BOOL "@:^@");
    addOverride(c, base, "deallocateRenderResources", (IMP) auDeallocate, "v@:");
    addOverride(c, base, "reset", (IMP) auReset, "v@:");
    addOverride(c, base, "parameterTree", (IMP) auParameterTree, "@@:");
    addOverride(c, base, "fullState", (IMP) auFullState, "@@:");
    addOverride(c, base, "setFullState:", (IMP) auSetFullState, "v@:@");
    addOverride(c, base, "factoryPresets", (IMP) auFactoryPresets, "@@:");
    addOverride(c, base, "currentPreset", (IMP) auCurrentPreset, "@@:");
    addOverride(c, base, "setCurrentPreset:", (IMP) auSetCurrentPreset, "v@:@");
    addOverride(c, base, "latency", (IMP) auLatency, "d@:");
    addOverride(c, base, "tailTime", (IMP) auTailTime, "d@:");
    addOverride(c, base, "channelCapabilities", (IMP) auChannelCapabilities, "@@:");
    addOverride(c, base, "canProcessInPlace", (IMP) auCanProcessInPlace, SONORE_AUV3_BOOL "@:");
    addOverride(c, base, "shouldBypassEffect", (IMP) auShouldBypass, SONORE_AUV3_BOOL "@:");
    addOverride(c, base, "setShouldBypassEffect:", (IMP) auSetShouldBypass, "v@:" SONORE_AUV3_BOOL);
    addOverride(c, base, "MIDIOutputNames", (IMP) auMidiOutputNames, "@@:");
    addOverride(c, base, "providesUserInterface", (IMP) auProvidesUserInterface, SONORE_AUV3_BOOL "@:");
    addOverride(c, base, "supportsMPE", (IMP) auSupportsMpe, SONORE_AUV3_BOOL "@:");
    addOverride(c, base, "setContextName:", (IMP) auSetContextName, "v@:@");
    addOverride(c, base, "shouldChangeToFormat:forBus:", (IMP) auShouldChangeToFormat,
                SONORE_AUV3_BOOL "@:@@");
    objc_registerClassPair(c);
    return c;
  }();
  return klass;
}

/** Make one unit, as the extension's factory or an in-process host does. */
inline id createUnit(AudioComponentDescription desc, id* outError) {
  Class c = audioUnitClass();
  if (!c) {
    setError(outError, kAudioUnitErr_FailedInitialization);
    return nil;
  }
  id unit = send<id>((id) c, "alloc");
  using Init = id (*)(id, SEL, AudioComponentDescription, uint32_t, id*);
  return reinterpret_cast<Init>(objc_msgSend)(unit, sel("initWithComponentDescription:options:error:"), desc,
                                               (uint32_t) 0, outError); // +1
}

// ── The editor and the view controller ───────────────────────────────────────

/** [main-thread] A message from the web editor: an edit is told to the host
 *  through its AUParameter, with the gesture AUv3 has words for. */
inline void editorMessage(Unit* unit, const BridgeMessage& message) {
  if (!unit) return;
  if ((message.kind == BridgeMessage::Kind::Set || message.kind == BridgeMessage::Kind::GestureBegin ||
       message.kind == BridgeMessage::Kind::GestureEnd) &&
      !uiMayWrite(kDesc, message.index))
    return;
  UiEventQueue::Event e;
  switch (message.kind) {
    case BridgeMessage::Kind::Ready:
      clapwrap::pageReady(&unit->shared);
      return;
    case BridgeMessage::Kind::Set: {
      if (message.index < 0 || message.index >= SONORE_NUM_PARAMS) return;
      const float plain = clampToRange(kDesc.params[message.index], (float) message.value);
      unit->shared.params[message.index] = plain;
      unit->shared.uiEcho[message.index] = plain;
      tellHost(unit, message.index, plain, kAutomationValue);
      return;
    }
    case BridgeMessage::Kind::GestureBegin:
      tellHost(unit, message.index, unit->shared.params[message.index], kAutomationTouch);
      return;
    case BridgeMessage::Kind::GestureEnd:
      tellHost(unit, message.index, unit->shared.params[message.index], kAutomationRelease);
      return;
    case BridgeMessage::Kind::LoadPreset:
      if (applyFactoryPreset(unit, message.index)) {
        setCurrentPresetObject(unit, makePreset(message.index, kDesc.presets[message.index].name));
        announceAllParameters(unit);
        announce(unit->self, "currentPreset");
      }
      return;
    case BridgeMessage::Kind::NoteOn:
      if (message.note < 0 || message.note > 127) return;
      e.kind = UiEventQueue::Event::Kind::NoteOn;
      e.index = message.note;
      e.value = (float) (message.velocity < 1 ? 1 : (message.velocity > 127 ? 127 : message.velocity));
      break;
    case BridgeMessage::Kind::NoteOff:
      if (message.note < 0 || message.note > 127) return;
      e.kind = UiEventQueue::Event::Kind::NoteOff;
      e.index = message.note;
      break;
    default:
      // ChooseFile and ContextMenu: an AUv3 has no way to ask its host for a
      // menu, and a sandboxed extension opens no panel of its own.
      return;
  }
  unit->shared.uiEvents.push(e);
}

/** The native editor's host, with every edit also told to the AU host. */
inline gfx::EditorHost makeEditorHost(Unit* unit) {
  gfx::EditorHost host = clapwrap::makeEditorHost(&unit->shared);
  auto set = host.setParameter;
  host.setParameter = [unit, set](int index, float value) {
    if (set) set(index, value);
    if (index >= 0 && validIndex((uint32_t) index))
      tellHost(unit, index, unit->shared.params[index], kAutomationValue);
  };
  auto begin = host.beginGesture;
  host.beginGesture = [unit, begin](int index) {
    if (begin) begin(index);
    if (index >= 0 && validIndex((uint32_t) index))
      tellHost(unit, index, unit->shared.params[index], kAutomationTouch);
  };
  auto end = host.endGesture;
  host.endGesture = [unit, end](int index) {
    if (end) end(index);
    if (index >= 0 && validIndex((uint32_t) index))
      tellHost(unit, index, unit->shared.params[index], kAutomationRelease);
  };
  return host;
}

/** [main-thread] The web page's clock: what moved since the page last heard. */
inline void editorTick(Unit* unit) {
#if defined(SONORE_HAS_WEBVIEW_BACKEND)
  if (!unit || unit->shared.guiIsNative || !unit->shared.webview.ready()) return;
  unit->shared.webview.eval(clapwrap::uiTickScript(unit->shared));
#else
  (void) unit;
#endif
}

/** [main-thread] The editor, into `container`: native or web, by the same
 *  choice every format makes. */
inline bool openEditor(Unit* unit, id container, uint32_t width, uint32_t height) {
  if (!unit || !container) return false;
  unit->shared.nativeEditor.close();
#if defined(SONORE_HAS_WEBVIEW_BACKEND)
  unit->shared.webview.destroy();
#endif
  unit->shared.uiEchoValid = false;
  unit->editorView = container;
  const EditorChoice choice = clapwrap::editorChoice();
  unit->shared.guiIsNative = choice.backend == EditorBackend::Native;
  if (unit->shared.guiIsNative) {
    unit->shared.nativeEditor.setContentFactory(clapwrap::customEditorFactory(unit->shared.dsp));
    if (unit->shared.nativeEditor.open((void*) container, kDesc.params, (int) kDesc.numParams,
                                       makeEditorHost(unit), (int) width, (int) height))
      return true;
    unit->shared.guiIsNative = false;
  }
#if defined(SONORE_HAS_WEBVIEW_BACKEND)
  unit->shared.webview.onMessage = [unit](const BridgeMessage& m) { editorMessage(unit, m); };
  unit->shared.webview.onTick = [unit]() { editorTick(unit); };
  return unit->shared.webview.create((void*) container, width, height, clapwrap::uiHtml(),
                                     clapwrap::bridgeScriptFor(unit->shared), kDesc.id);
#else
  return false;
#endif
}

inline void closeEditor(Unit* unit, id container) {
  if (!unit || unit->editorView != container) return;
  unit->shared.nativeEditor.close();
#if defined(SONORE_HAS_WEBVIEW_BACKEND)
  unit->shared.webview.destroy();
#endif
  unit->editorView = nil;
}

/** The view controller's own instance variables. */
constexpr const char* kControllerUnitIvar = "sonoreAudioUnit"; // the AUAudioUnit, retained
constexpr const char* kControllerViewIvar = "sonoreContainer"; // the container, unretained

inline id controllerUnit(id controller) {
  void* unit = nullptr;
  object_getInstanceVariable(controller, kControllerUnitIvar, &unit);
  return (id) unit;
}

inline id controllerContainer(id controller) {
  void* view = nullptr;
  object_getInstanceVariable(controller, kControllerViewIvar, &view);
  return (id) view;
}

inline void editorSize(uint32_t* width, uint32_t* height) {
  *width = SONORE_UI_WIDTH;
  *height = SONORE_UI_HEIGHT;
  clapwrap::clampEditorSize(width, height);
}

/** [main-thread] When both halves exist -- the unit and the view, which a
 *  host may ask for in either order -- the editor goes into the view. */
inline void attachIfReady(id controller) {
  id audioUnit = controllerUnit(controller);
  id container = controllerContainer(controller);
  Unit* unit = unitOf(audioUnit);
  if (!unit || !container || unit->editorView == container) return;
  uint32_t width = 0, height = 0;
  editorSize(&width, &height);
  unit->viewController = controller;
  openEditor(unit, container, width, height);
}

inline void attachOnMain(void* controller) {
  attachIfReady((id) controller);
  release((id) controller); // the retain onMainThread's caller took
}

/** -createAudioUnitWithComponentDescription:error: (AUAudioUnitFactory). May
 *  be called on any thread; the editor attaches on the main one. */
inline id vcCreateAudioUnit(id self, SEL, AudioComponentDescription desc, id* outError) {
  id audioUnit = createUnit(desc, outError);
  if (!audioUnit) return nil;
  void* previous = nullptr;
  object_getInstanceVariable(self, kControllerUnitIvar, &previous);
  object_setInstanceVariable(self, kControllerUnitIvar, audioUnit); // owns the +1
  if (previous) release((id) previous);
  onMainThread(retain(self), &attachOnMain);
  return audioUnit; // +0 to the caller: the controller keeps its own reference
}

/** -loadView: the container the editor is drawn into. */
inline void vcLoadView(id self, SEL) {
  uint32_t width = 0, height = 0;
  editorSize(&width, &height);
#if defined(SONORE_IOS)
  const char* viewClass = "UIView";
#else
  const char* viewClass = "NSView";
#endif
  const cocoa::CGRectStruct frame{{0.0, 0.0}, {(double) width, (double) height}};
  using InitFrame = id (*)(id, SEL, cocoa::CGRectStruct);
  id view = reinterpret_cast<InitFrame>(objc_msgSend)(send<id>(classNamed(viewClass), "alloc"),
                                                      sel("initWithFrame:"), frame);
  if (!view) return;
  // Width and height follow the host's frame (the same bit values on UIKit
  // and AppKit: flexible width 2, flexible height 16).
  send<void>(view, "setAutoresizingMask:", (ObjcUInteger) (2 | 16));
  send<void>(self, "setView:", view);
  object_setInstanceVariable(self, kControllerViewIvar, view);
  release(view); // the controller's view property holds it
  attachIfReady(self);
}

inline cocoa::CGSizeStruct vcPreferredContentSize(id, SEL) {
  uint32_t width = 0, height = 0;
  editorSize(&width, &height);
  return cocoa::CGSizeStruct{(double) width, (double) height};
}

/** -viewDidLayoutSubviews (UIKit) / -viewDidLayout (AppKit): the host
 *  resized the editor's view; a native editor lays itself out to it, as a web
 *  page does by itself. */
inline void vcViewDidLayout(id self, SEL selector) {
  sendSuper<void>(self, objc_getClass("AUViewController"), sel_getName(selector));
  id container = controllerContainer(self);
  Unit* unit = unitOf(controllerUnit(self));
  if (!unit || !container || unit->editorView != container || !unit->shared.guiIsNative) return;
  using Bounds = cocoa::CGRectStruct (*)(id, SEL);
#if defined(__x86_64__)
  const cocoa::CGRectStruct b =
      reinterpret_cast<Bounds>(objc_msgSend_stret)(container, sel("bounds")); // over 16 bytes on x86_64
#else
  const cocoa::CGRectStruct b = reinterpret_cast<Bounds>(objc_msgSend)(container, sel("bounds"));
#endif
  if (b.size.width >= 1.0 && b.size.height >= 1.0)
    unit->shared.nativeEditor.setSize((int) b.size.width, (int) b.size.height);
}

inline void vcDealloc(id self, SEL) {
  id audioUnit = controllerUnit(self);
  if (Unit* unit = unitOf(audioUnit)) {
    closeEditor(unit, controllerContainer(self));
    if (unit->viewController == self) unit->viewController = nil;
  }
  object_setInstanceVariable(self, kControllerUnitIvar, nullptr);
  release(audioUnit);
  sendSuper<void>(self, objc_getClass("AUViewController"), "dealloc");
}

/** The protocol the extension point checks its principal class for. Taken
 *  from the runtime when a framework registered it; built here when not. */
inline Protocol* factoryProtocol() {
  Protocol* p = objc_getProtocol("AUAudioUnitFactory");
  if (p) return p;
  p = objc_allocateProtocol("AUAudioUnitFactory");
  if (!p) return objc_getProtocol("AUAudioUnitFactory");
  protocol_addMethodDescription(p, sel("createAudioUnitWithComponentDescription:error:"),
                                "@@:{AudioComponentDescription=IIIII}^@", YES, YES);
  objc_registerProtocol(p);
  return p;
}

/** The extension's principal class: an AUViewController that is also the
 *  unit factory. Its name is the one the Info.plist carries. */
inline Class viewControllerClass() {
  static Class klass = [] {
    if (Class existing = objc_getClass(SONORE_AUV3_FACTORY_CLASS)) return existing;
    Class base = objc_getClass("AUViewController");
    if (!base) return (Class) nullptr;
    Class c = objc_allocateClassPair(base, SONORE_AUV3_FACTORY_CLASS, 0);
    if (!c) return (Class) nullptr;
    class_addIvar(c, kControllerUnitIvar, sizeof(void*), (uint8_t) (sizeof(void*) == 8 ? 3 : 2), "^v");
    class_addIvar(c, kControllerViewIvar, sizeof(void*), (uint8_t) (sizeof(void*) == 8 ? 3 : 2), "^v");
    class_addMethod(c, sel("createAudioUnitWithComponentDescription:error:"), (IMP) vcCreateAudioUnit,
                    "@@:{AudioComponentDescription=IIIII}^@");
    addOverride(c, base, "loadView", (IMP) vcLoadView, "v@:");
    addOverride(c, base, "preferredContentSize", (IMP) vcPreferredContentSize, "{CGSize=dd}@:");
#if defined(SONORE_IOS)
    addOverride(c, base, "viewDidLayoutSubviews", (IMP) vcViewDidLayout, "v@:");
#else
    addOverride(c, base, "viewDidLayout", (IMP) vcViewDidLayout, "v@:");
#endif
    addOverride(c, base, "dealloc", (IMP) vcDealloc, "v@:");
    if (Protocol* p = factoryProtocol()) class_addProtocol(c, p);
    objc_registerClassPair(c);
    return c;
  }();
  return klass;
}

/** Both classes, before NSExtensionMain asks for the principal one. */
struct ClassRegistrar {
  ClassRegistrar() {
    audioUnitClass();
    viewControllerClass();
  }
};
static ClassRegistrar g_sonoreAuv3ClassRegistrar;

} // namespace auv3
} // namespace sonore

/**
 * Register this plugin's AUAudioUnit subclass in THIS process under `desc`,
 * as an app that hosts its own unit does (the iOS standalone) and as the
 * self-test does: AVAudioEngine and AUAudioUnit's own instantiate then find
 * it with no extension involved. Returns the class, or null.
 */
extern "C" __attribute__((visibility("default"))) Class
SonoreAUv3RegisterInProcess(const AudioComponentDescription* desc, const char* name, uint32_t version) {
  using namespace sonore::auv3;
  Class c = audioUnitClass();
  if (!c || !desc) return nullptr;
  using Register = void (*)(id, SEL, Class, AudioComponentDescription, id, uint32_t);
  reinterpret_cast<Register>(objc_msgSend)(classNamed("AUAudioUnit"),
                                           sel("registerSubclass:asComponentDescription:name:version:"), c,
                                           *desc, string(name ? name : kDesc.name), version);
  return c;
}
