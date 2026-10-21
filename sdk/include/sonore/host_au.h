// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: hosting an Audio Unit (AUv2, and AUv3 through AUv2's API).
//
// The macOS half of host.h, in the same shape as the other three formats: a
// scan that describes without loading, a loader, and an AuPlugin that is a
// HostedPlugin like ClapPlugin, Vst3Plugin and Lv2Plugin -- prepare, process,
// parameters and their edits, state, presets, latency and tail, bypass, and
// the plugin's own editor in an NSView the caller owns. host.h includes this
// file at its end; a host that wants AU need include nothing else, and
// loadPlugin() takes a description whose format is "AU".
//
// ── Written blind, and built to be checked ──────────────────────────────────
//
// There is no Mac in this loop and macOS CI is not running, so NOT ONE LINE of
// the Apple half has executed. It is compiled -- by MSVC, for macOS, against
// the declarations in tests/au_shim (tests/mac_syntax_mac_host.cpp) -- which
// proves every name exists and every call fits its declaration, and proves
// nothing about whether Apple's runtime agrees. Every Apple call's status is
// checked and every failure lands in lastError() or the error string a
// loader is handed, so a run on a Mac says what went wrong instead of quietly
// hosting nothing. tests/mac_host_selftest.cpp is that run: it loads Apple's
// own AUDelay and DLSMusicDevice and prints what each step returned.
//
// The parts that need no Mac -- the ids, the parameter mapping, the channel
// negotiation, the MIDI a unit emits, the error names -- are portable
// functions in host::detail, run by tests/mac_host_test.cpp on every
// platform. The Apple half is the thinnest layer that can call them.
//
// ── How an AU is found ──────────────────────────────────────────────────────
//
// Not by file. AUs live in a system registry: AudioComponentFindNext walks
// it by type -- effect 'aufx', instrument 'aumu', music effect 'aumf', MIDI
// processor 'aumi', generator 'augn' -- and finds .component bundles, Apple's
// built-in units and AUv3 app extensions alike, the last of which have no
// bundle a folder scan could see. So AudioUnitPluginFormat::findAllComponents
// is the scan, and the id that identifies one is its registry triple,
// "type:subtype:manufacturer" ("aufx:dely:appl"). A .component in a folder
// host.h's scanDirectory walks is described too, from its Info.plist's
// AudioComponents list, so a folder scan and the plugin cache see AUs as they
// see everything else.
//
// ── Why the frameworks are opened, not linked ───────────────────────────────
//
// host.h is included by every host on the Mac, CLAP-only ones too. Linking
// AudioToolbox, CoreFoundation and libobjc for this file would make every one
// of them name three frameworks it never asked for, and an installed SDK's
// consumers would all break at link time. So the functions are found with
// dlsym once, typed from Apple's own declarations (decltype(&::AudioUnitRender)
// and so on, which reads the header and references no symbol), exactly as
// gfx/window_cocoa.h finds CoreGraphics. A missing symbol is reported by name.
//
// ── Threading, as host.h states it ──────────────────────────────────────────
//
// Loading, prepare, parameters, state, presets and the editor are main-thread.
// process() is the audio thread and allocates nothing: buffers, the output
// buffer list and the MIDI out buffer are all sized in prepare(). An AUv3, or
// any unit whose component says it must be created asynchronously, is
// created with AudioComponentInstantiate, and the loader waits for it by
// RUNNING the main run loop when it is on the main thread -- an out-of-process
// unit's completion can need that loop, and a loader that blocked it would
// wait out its own timeout.
#pragma once

#include "host.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(__APPLE__)
#include <AudioToolbox/AudioToolbox.h>
#include <AudioUnit/AudioUnit.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMIDI/CoreMIDI.h>
#include <dlfcn.h>
#include <objc/message.h>
#include <objc/objc.h>
#include <objc/runtime.h>
#include <pthread.h>

#include "apple_block.h"
#endif

namespace sonore {
namespace host {

// ── The portable half ────────────────────────────────────────────────────────

namespace detail {

/**
 * Apple's numbers, restated so the logic that reads them runs anywhere.
 *
 * On a Mac the Apple half passes these functions the values Apple's own
 * headers produce, so a wrong number here is a wrong answer there.
 * tests/mac_host_constants.h compares every one of them with Apple's name for
 * it; a Mac running tests/mac_host_selftest.cpp prints any that disagree.
 */
namespace aucode {

constexpr uint32_t fourCC(char a, char b, char c, char d) {
  return ((uint32_t) (uint8_t) a << 24) | ((uint32_t) (uint8_t) b << 16) |
         ((uint32_t) (uint8_t) c << 8) | (uint32_t) (uint8_t) d;
}

// Component types (AUComponent.h).
constexpr uint32_t kTypeEffect = fourCC('a', 'u', 'f', 'x');
constexpr uint32_t kTypeMusicDevice = fourCC('a', 'u', 'm', 'u');
constexpr uint32_t kTypeMusicEffect = fourCC('a', 'u', 'm', 'f');
constexpr uint32_t kTypeMidiProcessor = fourCC('a', 'u', 'm', 'i');
constexpr uint32_t kTypeGenerator = fourCC('a', 'u', 'g', 'n');

// AudioComponentFlags (AudioComponent.h).
constexpr uint32_t kFlagUnsearchable = 1;
constexpr uint32_t kFlagSandboxSafe = 2;
constexpr uint32_t kFlagIsV3AudioUnit = 4;
constexpr uint32_t kFlagRequiresAsyncInstantiation = 8;
constexpr uint32_t kFlagCanLoadInProcess = 0x10;

// AudioUnitParameterOptions (AudioUnitProperties.h).
constexpr uint32_t kParamCFNameRelease = 1u << 4;
constexpr uint32_t kParamMeterReadOnly = 1u << 15;
constexpr uint32_t kParamHasClump = 1u << 20;
constexpr uint32_t kParamValuesHaveStrings = 1u << 21;
constexpr uint32_t kParamDisplayLogarithmic = 1u << 22;
constexpr uint32_t kParamHasCFNameString = 1u << 27;
constexpr uint32_t kParamIsReadable = 1u << 30;
constexpr uint32_t kParamIsWritable = 1u << 31;

// AudioUnitParameterUnit (AudioUnitProperties.h).
constexpr uint32_t kUnitGeneric = 0;
constexpr uint32_t kUnitIndexed = 1;
constexpr uint32_t kUnitBoolean = 2;
constexpr uint32_t kUnitPercent = 3;
constexpr uint32_t kUnitSeconds = 4;
constexpr uint32_t kUnitSampleFrames = 5;
constexpr uint32_t kUnitPhase = 6;
constexpr uint32_t kUnitRate = 7;
constexpr uint32_t kUnitHertz = 8;
constexpr uint32_t kUnitCents = 9;
constexpr uint32_t kUnitRelativeSemiTones = 10;
constexpr uint32_t kUnitMIDINoteNumber = 11;
constexpr uint32_t kUnitMIDIController = 12;
constexpr uint32_t kUnitDecibels = 13;
constexpr uint32_t kUnitLinearGain = 14;
constexpr uint32_t kUnitDegrees = 15;
constexpr uint32_t kUnitEqualPowerCrossfade = 16;
constexpr uint32_t kUnitMixerFaderCurve1 = 17;
constexpr uint32_t kUnitPan = 18;
constexpr uint32_t kUnitMeters = 19;
constexpr uint32_t kUnitAbsoluteCents = 20;
constexpr uint32_t kUnitOctaves = 21;
constexpr uint32_t kUnitBPM = 22;
constexpr uint32_t kUnitBeats = 23;
constexpr uint32_t kUnitMilliseconds = 24;
constexpr uint32_t kUnitRatio = 25;
constexpr uint32_t kUnitCustomUnit = 26;

// Properties this file reads, and the two that arrived late enough to be
// spelled here: kAudioUnitProperty_RequestViewController (10.11) and
// kAudioUnitProperty_RenderContextObserver (macOS 11, used by au_wrapper.h).
constexpr uint32_t kPropertyRequestViewController = 56;
constexpr uint32_t kPropertyRenderContextObserver = 60;

/** kAUParameterListener_AnyParameter: "every parameter moved". */
constexpr uint32_t kAnyParameter = 0xFFFFFFFFu;

} // namespace aucode

/** A four-character code as text an id can carry. Printable ASCII stands for
 *  itself -- spaces included, "dls " is Apple's -- and anything else, or a ':'
 *  or '%' that would confuse the id's own syntax, is written %HH. */
inline std::string auCodeText(uint32_t code) {
  std::string out;
  for (int shift = 24; shift >= 0; shift -= 8) {
    const unsigned char c = (unsigned char) ((code >> shift) & 0xFF);
    if (c >= 0x20 && c <= 0x7E && c != ':' && c != '%') {
      out += (char) c;
    } else {
      char escaped[4];
      std::snprintf(escaped, sizeof(escaped), "%%%02X", (unsigned) c);
      out += escaped;
    }
  }
  return out;
}

/** The reverse. Exactly four bytes, or false. */
inline bool auParseCode(const std::string& text, uint32_t* code) {
  uint32_t value = 0;
  int bytes = 0;
  for (size_t i = 0; i < text.size(); ++bytes) {
    if (bytes >= 4) return false;
    unsigned char c = (unsigned char) text[i];
    if (c == '%') {
      if (i + 2 >= text.size()) return false;
      auto hex = [](char h) -> int {
        if (h >= '0' && h <= '9') return h - '0';
        if (h >= 'a' && h <= 'f') return h - 'a' + 10;
        if (h >= 'A' && h <= 'F') return h - 'A' + 10;
        return -1;
      };
      const int hi = hex(text[i + 1]), lo = hex(text[i + 2]);
      if (hi < 0 || lo < 0) return false;
      c = (unsigned char) (hi * 16 + lo);
      i += 3;
    } else {
      if (c < 0x20 || c > 0x7E || c == ':') return false;
      i += 1;
    }
    value = (value << 8) | c;
  }
  if (bytes != 4) return false;
  if (code) *code = value;
  return true;
}

/** "type:subtype:manufacturer": what identifies an AU in the registry, and so
 *  what PluginDescription::id holds for one. */
inline std::string auIdFor(uint32_t type, uint32_t subtype, uint32_t manufacturer) {
  return auCodeText(type) + ":" + auCodeText(subtype) + ":" + auCodeText(manufacturer);
}

inline bool auParseId(const std::string& id, uint32_t* type, uint32_t* subtype,
                      uint32_t* manufacturer) {
  const size_t first = id.find(':');
  if (first == std::string::npos) return false;
  const size_t second = id.find(':', first + 1);
  if (second == std::string::npos || id.find(':', second + 1) != std::string::npos) return false;
  return auParseCode(id.substr(0, first), type) &&
         auParseCode(id.substr(first + 1, second - first - 1), subtype) &&
         auParseCode(id.substr(second + 1), manufacturer);
}

/** A component version (0xMMMMmmbb) as "major.minor.bugfix". */
inline std::string auVersionText(uint32_t version) {
  char text[32];
  std::snprintf(text, sizeof(text), "%u.%u.%u", (unsigned) (version >> 16),
                (unsigned) ((version >> 8) & 0xFF), (unsigned) (version & 0xFF));
  return text;
}

/** A component's registered name is "Manufacturer: Name". Split at the first
 *  ": "; a name without one is all name. */
inline void auSplitName(const std::string& full, std::string* vendor, std::string* name) {
  const size_t colon = full.find(": ");
  if (colon == std::string::npos) {
    if (vendor) vendor->clear();
    if (name) *name = full;
    return;
  }
  if (vendor) *vendor = full.substr(0, colon);
  if (name) *name = full.substr(colon + 2);
}

/** The word PluginDescription::features starts with for a component type. */
inline const char* auTypeWord(uint32_t type) {
  switch (type) {
    case aucode::kTypeEffect: return "Effect";
    case aucode::kTypeMusicDevice: return "Instrument";
    case aucode::kTypeMusicEffect: return "MusicEffect";
    case aucode::kTypeMidiProcessor: return "MIDIProcessor";
    case aucode::kTypeGenerator: return "Generator";
    default: return "Other";
  }
}

/** The types that take MIDI through MusicDeviceMIDIEvent. */
inline bool auTypeTakesMidi(uint32_t type) {
  return type == aucode::kTypeMusicDevice || type == aucode::kTypeMusicEffect ||
         type == aucode::kTypeMidiProcessor;
}

/**
 * An OSStatus in words.
 *
 * The AU and component errors by name, because "-10868" in a log is a search
 * and "kAudioUnitErr_FormatNotSupported" is an answer. Anything else that is
 * four printable characters is shown as one -- much of CoreAudio returns
 * codes like 'fmt?' -- and the rest as a number.
 */
inline std::string auStatusText(int32_t status) {
  const char* name = nullptr;
  switch (status) {
    case 0: name = "noErr"; break;
    case -50: name = "paramErr"; break;
    case -4: name = "unimpErr"; break;
    case -10879: name = "kAudioUnitErr_InvalidProperty"; break;
    case -10878: name = "kAudioUnitErr_InvalidParameter"; break;
    case -10877: name = "kAudioUnitErr_InvalidElement"; break;
    case -10876: name = "kAudioUnitErr_NoConnection"; break;
    case -10875: name = "kAudioUnitErr_FailedInitialization"; break;
    case -10874: name = "kAudioUnitErr_TooManyFramesToProcess"; break;
    case -10871: name = "kAudioUnitErr_InvalidFile"; break;
    case -10870: name = "kAudioUnitErr_UnknownFileType"; break;
    case -10869: name = "kAudioUnitErr_FileNotSpecified"; break;
    case -10868: name = "kAudioUnitErr_FormatNotSupported"; break;
    case -10867: name = "kAudioUnitErr_Uninitialized"; break;
    case -10866: name = "kAudioUnitErr_InvalidScope"; break;
    case -10865: name = "kAudioUnitErr_PropertyNotWritable"; break;
    case -10863: name = "kAudioUnitErr_CannotDoInCurrentContext"; break;
    case -10851: name = "kAudioUnitErr_InvalidPropertyValue"; break;
    case -10850: name = "kAudioUnitErr_PropertyNotInUse"; break;
    case -10849: name = "kAudioUnitErr_Initialized"; break;
    case -10848: name = "kAudioUnitErr_InvalidOfflineRender"; break;
    case -10847: name = "kAudioUnitErr_Unauthorized"; break;
    case -66754: name = "kAudioComponentErr_InstanceTimedOut"; break;
    case -66753: name = "kAudioUnitErr_MIDIOutputBufferFull"; break;
    case -66752: name = "kAudioComponentErr_DuplicateDescription"; break;
    case -66751: name = "kAudioComponentErr_UnsupportedType"; break;
    case -66750: name = "kAudioComponentErr_TooManyInstances"; break;
    case -66749: name = "kAudioComponentErr_InstanceInvalidated"; break;
    case -66748: name = "kAudioComponentErr_NotPermitted"; break;
    case -66747: name = "kAudioComponentErr_InitializationTimedOut"; break;
    case -66746: name = "kAudioComponentErr_InvalidFormat"; break;
    case -66745: name = "kAudioUnitErr_RenderTimeout"; break;
    case -66744: name = "kAudioUnitErr_ExtensionNotFound"; break;
    default: break;
  }
  char text[96];
  if (name) {
    std::snprintf(text, sizeof(text), "%s (%d)", name, (int) status);
    return text;
  }
  const uint32_t code = (uint32_t) status;
  bool printable = true;
  for (int shift = 24; shift >= 0; shift -= 8) {
    const unsigned c = (code >> shift) & 0xFF;
    if (c < 0x20 || c > 0x7E) printable = false;
  }
  if (printable) {
    std::snprintf(text, sizeof(text), "'%c%c%c%c' (%d)", (char) (code >> 24), (char) (code >> 16),
                  (char) (code >> 8), (char) code, (int) status);
    return text;
  }
  std::snprintf(text, sizeof(text), "OSStatus %d", (int) status);
  return text;
}

/** What a unit says about one parameter, reduced to what the mapping needs.
 *  Filled from AudioUnitParameterInfo by the Apple half; built by hand by a
 *  test. */
struct AuParamFacts {
  uint32_t id = 0;
  /** cfNameString when the unit gave one (kParamHasCFNameString), else the
   *  52-byte C name. */
  std::string name;
  /** The clump's name, when the parameter is in one. */
  std::string clump;
  uint32_t unit = aucode::kUnitGeneric;
  float minValue = 0.0f;
  float maxValue = 1.0f;
  float defaultValue = 0.0f;
  uint32_t flags = 0;
};

/** Can a host set it? Writable, and not a meter -- a meter is the unit's
 *  output, and some units flag meters writable anyway. */
inline bool auParamIsSettable(uint32_t flags) {
  return (flags & aucode::kParamIsWritable) != 0 && (flags & aucode::kParamMeterReadOnly) == 0;
}

/** Whole-number units, which a host shows as steps. */
inline bool auUnitIsStepped(uint32_t unit) {
  return unit == aucode::kUnitIndexed || unit == aucode::kUnitBoolean ||
         unit == aucode::kUnitMIDINoteNumber || unit == aucode::kUnitMIDIController;
}

/** One AU parameter as a HostedParam. AU speaks PLAIN values in the
 *  parameter's own units, as HostedParam does, so the range carries across
 *  as it is: only a reversed range is put the right way round and a default
 *  outside it pulled in. */
inline HostedParam auHostedParam(const AuParamFacts& facts) {
  HostedParam p;
  p.id = facts.id;
  if (!facts.name.empty()) {
    p.name = facts.name;
  } else {
    char fallback[32];
    std::snprintf(fallback, sizeof(fallback), "Parameter %u", (unsigned) facts.id);
    p.name = fallback;
  }
  p.module = facts.clump;
  double lo = facts.minValue, hi = facts.maxValue;
  if (hi < lo) {
    const double t = lo;
    lo = hi;
    hi = t;
  }
  p.minValue = lo;
  p.maxValue = hi;
  double def = facts.defaultValue;
  if (def < lo) def = lo;
  if (def > hi) def = hi;
  p.defaultValue = def;
  p.stepped = auUnitIsStepped(facts.unit);
  return p;
}

/** The suffix a value of this unit is written with, when the unit has no text
 *  of its own for it. Empty for the unitless ones. */
inline const char* auUnitLabel(uint32_t unit) {
  switch (unit) {
    case aucode::kUnitPercent: return "%";
    case aucode::kUnitSeconds: return "s";
    case aucode::kUnitSampleFrames: return "samples";
    case aucode::kUnitPhase: return "deg";
    case aucode::kUnitRate: return "x";
    case aucode::kUnitHertz: return "Hz";
    case aucode::kUnitCents: return "cents";
    case aucode::kUnitRelativeSemiTones: return "st";
    case aucode::kUnitDecibels: return "dB";
    case aucode::kUnitDegrees: return "deg";
    case aucode::kUnitMeters: return "m";
    case aucode::kUnitAbsoluteCents: return "cents";
    case aucode::kUnitOctaves: return "oct";
    case aucode::kUnitBPM: return "BPM";
    case aucode::kUnitBeats: return "beats";
    case aucode::kUnitMilliseconds: return "ms";
    case aucode::kUnitRatio: return ":1";
    default: return "";
  }
}

/** One AUChannelInfo entry: a channel count per side, or Apple's negative
 *  wildcards. */
struct AuChannelPair {
  int in = -1;
  int out = -1;
};

/**
 * Does one kAudioUnitProperty_SupportedNumChannels entry admit `in` x `out`?
 *
 * Apple's rules: {-1,-1} any count, the same on both sides; {-1,-2} or
 * {-2,-1} (or {-2,-2}) any count on each side, independently; -1 or -2
 * against a number, any count on that side; any other -N, one to N; a
 * non-negative number, exactly that.
 */
inline bool auChannelEntryAdmits(AuChannelPair entry, int in, int out) {
  const int a = entry.in, b = entry.out;
  if (a == -1 && b == -1) return in == out;
  auto side = [](int spec, int n) {
    if (spec >= 0) return n == spec;
    if (spec == -1 || spec == -2) return true;
    return n >= 1 && n <= -spec;
  };
  return side(a, in) && side(b, out);
}

/**
 * Every configuration worth trying, best first, for a caller that wants
 * `wanted` channels each way (none in, for a unit without an input bus).
 *
 * The same count both ways first, then stereo, then mono, then the other
 * widths up to eight; then any entry that names exact counts. A unit that
 * publishes no list is taken at {-1,-1}, which is what the property's
 * documentation says its absence means.
 */
inline std::vector<AuChannelPair> auChannelCandidates(const std::vector<AuChannelPair>& entries,
                                                      int wanted, bool hasInput) {
  const std::vector<AuChannelPair> rules =
      entries.empty() ? std::vector<AuChannelPair>{{-1, -1}} : entries;
  if (wanted < 1) wanted = 1;
  std::vector<int> widths{wanted, 2, 1};
  for (int w = 3; w <= 8; ++w) widths.push_back(w);
  std::vector<AuChannelPair> out;
  auto add = [&out](AuChannelPair p) {
    for (const AuChannelPair& q : out)
      if (q.in == p.in && q.out == p.out) return;
    out.push_back(p);
  };
  for (int w : widths) {
    const AuChannelPair p{hasInput ? w : 0, w};
    for (const AuChannelPair& rule : rules)
      if (auChannelEntryAdmits(rule, p.in, p.out)) {
        add(p);
        break;
      }
  }
  for (const AuChannelPair& rule : rules)
    if (rule.in >= 0 && rule.out > 0 && (hasInput || rule.in == 0)) add(rule);
  return out;
}

/**
 * The MIDI in one CoreMIDI packet, into a MidiBuffer at one sample position.
 *
 * A packet holds whole messages, possibly several, with running status
 * between channel messages, and realtime bytes wherever they fall; a SysEx
 * goes in whole when its F7 is in the same packet and is dropped -- counted
 * by the caller -- when it is not, because half a SysEx is a different
 * message. Returns how many events were added.
 */
inline int auParseMidiPacket(const uint8_t* data, size_t length, int samplePosition,
                             MidiBuffer& out) {
  int added = 0;
  int running = 0;
  size_t i = 0;
  while (data && i < length) {
    const uint8_t b = data[i];
    if (b == 0xF0) {
      size_t end = i + 1;
      while (end < length && data[end] < 0x80) ++end;
      if (end < length && data[end] == 0xF7) {
        if (out.addSysex(data + i, end - i + 1, samplePosition)) ++added;
        i = end + 1;
      } else {
        i = end; // unterminated here: dropped, and whatever status ended it is read next
      }
      running = 0;
      continue;
    }
    if (b >= 0xF8) { // realtime: one byte, anywhere, running status untouched
      out.addEvent(MidiMessage(b, 0, 0), samplePosition);
      ++added;
      ++i;
      continue;
    }
    int status = 0;
    if (b >= 0x80) {
      status = b;
      ++i;
      running = status < 0xF0 ? status : 0;
    } else {
      if (running == 0) {
        ++i; // a data byte with no status before it
        continue;
      }
      status = running;
    }
    const int size = shortMessageLength(status);
    if (size <= 0) continue; // 0xF4, 0xF5, a stray 0xF7
    const size_t dataBytes = (size_t) (size - 1);
    if (i + dataBytes > length) break; // truncated: nothing to deliver
    const int d1 = dataBytes >= 1 ? data[i] : 0;
    const int d2 = dataBytes >= 2 ? data[i + 1] : 0;
    if ((dataBytes >= 1 && d1 >= 0x80) || (dataBytes >= 2 && d2 >= 0x80)) {
      // A status byte where data should be: this message is broken, the next
      // one starts here.
      running = 0;
      continue;
    }
    i += dataBytes;
    out.addEvent(MidiMessage(status, d1, d2), samplePosition);
    ++added;
  }
  return added;
}

} // namespace detail

// ── The loaders host.h calls, declared there and defined below ───────────────
//
// describeAudioUnitBundle() and loadAudioUnit() exist on every platform, so
// host.h can call them without an #if; off macOS they say why there is
// nothing, which is the truth.

class AudioUnitPluginFormat;

#if defined(__APPLE__)

namespace detail {

/** AudioComponentInstantiate takes a block, whose declaration only a
 *  blocks-enabled compiler reads; its ABI is a pointer. */
using AuInstantiateFn = void (*)(AudioComponent component, UInt32 options, void* completion);

/**
 * Every Apple function this file calls, found once.
 *
 * `ok` is everything hosting needs; `editorOk` the Objective-C runtime the
 * editor also needs; `instantiate` may be null on a system without it, which
 * fails only the units that require it. `error` names whatever is missing.
 */
struct AuApi {
  bool ok = false;
  bool editorOk = false;
  std::string error;

  // AudioToolbox
  decltype(&::AudioComponentFindNext) findNext = nullptr;
  decltype(&::AudioComponentGetDescription) getDescription = nullptr;
  decltype(&::AudioComponentCopyName) copyName = nullptr;
  decltype(&::AudioComponentGetVersion) getVersion = nullptr;
  decltype(&::AudioComponentInstanceNew) instanceNew = nullptr;
  AuInstantiateFn instantiate = nullptr;
  decltype(&::AudioComponentInstanceDispose) instanceDispose = nullptr;
  decltype(&::AudioUnitInitialize) initialize = nullptr;
  decltype(&::AudioUnitUninitialize) uninitialize = nullptr;
  decltype(&::AudioUnitGetPropertyInfo) getPropertyInfo = nullptr;
  decltype(&::AudioUnitGetProperty) getProperty = nullptr;
  decltype(&::AudioUnitSetProperty) setProperty = nullptr;
  decltype(&::AudioUnitGetParameter) getParameter = nullptr;
  decltype(&::AudioUnitSetParameter) setParameter = nullptr;
  decltype(&::AudioUnitRender) render = nullptr;
  decltype(&::AudioUnitReset) reset = nullptr;
  decltype(&::MusicDeviceMIDIEvent) midiEvent = nullptr;
  decltype(&::MusicDeviceSysEx) sysEx = nullptr;
  decltype(&::AUEventListenerCreate) eventListenerCreate = nullptr;
  decltype(&::AUEventListenerAddEventType) eventListenerAdd = nullptr;
  decltype(&::AUListenerDispose) listenerDispose = nullptr;

  // CoreFoundation
  decltype(&::CFRelease) release = nullptr;
  decltype(&::CFGetTypeID) getTypeID = nullptr;
  decltype(&::CFStringGetTypeID) stringTypeID = nullptr;
  decltype(&::CFArrayGetTypeID) arrayTypeID = nullptr;
  decltype(&::CFDictionaryGetTypeID) dictionaryTypeID = nullptr;
  decltype(&::CFNumberGetTypeID) numberTypeID = nullptr;
  decltype(&::CFStringCreateWithCString) stringCreate = nullptr;
  decltype(&::CFStringGetCString) stringGetCString = nullptr;
  decltype(&::CFStringGetLength) stringGetLength = nullptr;
  decltype(&::CFStringGetMaximumSizeForEncoding) stringMaxSize = nullptr;
  decltype(&::CFDataCreate) dataCreate = nullptr;
  decltype(&::CFDataGetLength) dataGetLength = nullptr;
  decltype(&::CFDataGetBytePtr) dataGetBytePtr = nullptr;
  decltype(&::CFPropertyListCreateData) plistCreateData = nullptr;
  decltype(&::CFPropertyListCreateWithData) plistCreateWithData = nullptr;
  decltype(&::CFArrayGetCount) arrayGetCount = nullptr;
  decltype(&::CFArrayGetValueAtIndex) arrayGetValueAtIndex = nullptr;
  decltype(&::CFDictionaryGetValue) dictionaryGetValue = nullptr;
  decltype(&::CFNumberGetValue) numberGetValue = nullptr;
  decltype(&::CFURLCreateFromFileSystemRepresentation) urlCreateFromPath = nullptr;
  decltype(&::CFURLGetFileSystemRepresentation) urlGetPath = nullptr;
  decltype(&::CFBundleCreate) bundleCreate = nullptr;
  decltype(&::CFBundleGetValueForInfoDictionaryKey) bundleInfoValue = nullptr;
  decltype(&::CFRunLoopGetMain) runLoopGetMain = nullptr;
  decltype(&::CFRunLoopRunInMode) runLoopRunInMode = nullptr;

  // The Objective-C runtime, for the editor.
  decltype(&::objc_getClass) getClass = nullptr;
  decltype(&::sel_registerName) selector = nullptr;
  void* msgSend = nullptr;
  void* msgSendStret = nullptr; // x86_64 only: arm64 has no _stret
};

inline const AuApi& auApi() {
  static const AuApi api = [] {
    AuApi a;
    // Already loaded in any process that plays audio; this takes a handle.
    // AudioUnit.framework is the older home of the AudioComponent calls and
    // re-exports them today, so it is the second place each one is sought.
    void* toolbox =
        dlopen("/System/Library/Frameworks/AudioToolbox.framework/AudioToolbox", RTLD_LAZY | RTLD_LOCAL);
    void* audioUnit =
        dlopen("/System/Library/Frameworks/AudioUnit.framework/AudioUnit", RTLD_LAZY | RTLD_LOCAL);
    void* cf = dlopen("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation",
                      RTLD_LAZY | RTLD_LOCAL);
    void* objc = dlopen("/usr/lib/libobjc.A.dylib", RTLD_LAZY | RTLD_LOCAL);

    std::string missing, missingEditor;
    auto find = [](void* first, void* second, const char* name, void* slot,
                   std::string* missingList) {
      void* symbol = first ? dlsym(first, name) : nullptr;
      if (!symbol && second) symbol = dlsym(second, name);
      if (!symbol && missingList) {
        if (!missingList->empty()) *missingList += ", ";
        *missingList += name;
      }
      std::memcpy(slot, &symbol, sizeof(symbol));
    };
    find(toolbox, audioUnit, "AudioComponentFindNext", &a.findNext, &missing);
    find(toolbox, audioUnit, "AudioComponentGetDescription", &a.getDescription, &missing);
    find(toolbox, audioUnit, "AudioComponentCopyName", &a.copyName, &missing);
    find(toolbox, audioUnit, "AudioComponentGetVersion", &a.getVersion, &missing);
    find(toolbox, audioUnit, "AudioComponentInstanceNew", &a.instanceNew, &missing);
    find(toolbox, audioUnit, "AudioComponentInstantiate", &a.instantiate, nullptr);
    find(toolbox, audioUnit, "AudioComponentInstanceDispose", &a.instanceDispose, &missing);
    find(toolbox, audioUnit, "AudioUnitInitialize", &a.initialize, &missing);
    find(toolbox, audioUnit, "AudioUnitUninitialize", &a.uninitialize, &missing);
    find(toolbox, audioUnit, "AudioUnitGetPropertyInfo", &a.getPropertyInfo, &missing);
    find(toolbox, audioUnit, "AudioUnitGetProperty", &a.getProperty, &missing);
    find(toolbox, audioUnit, "AudioUnitSetProperty", &a.setProperty, &missing);
    find(toolbox, audioUnit, "AudioUnitGetParameter", &a.getParameter, &missing);
    find(toolbox, audioUnit, "AudioUnitSetParameter", &a.setParameter, &missing);
    find(toolbox, audioUnit, "AudioUnitRender", &a.render, &missing);
    find(toolbox, audioUnit, "AudioUnitReset", &a.reset, &missing);
    find(toolbox, audioUnit, "MusicDeviceMIDIEvent", &a.midiEvent, &missing);
    find(toolbox, audioUnit, "MusicDeviceSysEx", &a.sysEx, &missing);
    find(toolbox, audioUnit, "AUEventListenerCreate", &a.eventListenerCreate, &missing);
    find(toolbox, audioUnit, "AUEventListenerAddEventType", &a.eventListenerAdd, &missing);
    find(toolbox, audioUnit, "AUListenerDispose", &a.listenerDispose, &missing);

    find(cf, nullptr, "CFRelease", &a.release, &missing);
    find(cf, nullptr, "CFGetTypeID", &a.getTypeID, &missing);
    find(cf, nullptr, "CFStringGetTypeID", &a.stringTypeID, &missing);
    find(cf, nullptr, "CFArrayGetTypeID", &a.arrayTypeID, &missing);
    find(cf, nullptr, "CFDictionaryGetTypeID", &a.dictionaryTypeID, &missing);
    find(cf, nullptr, "CFNumberGetTypeID", &a.numberTypeID, &missing);
    find(cf, nullptr, "CFStringCreateWithCString", &a.stringCreate, &missing);
    find(cf, nullptr, "CFStringGetCString", &a.stringGetCString, &missing);
    find(cf, nullptr, "CFStringGetLength", &a.stringGetLength, &missing);
    find(cf, nullptr, "CFStringGetMaximumSizeForEncoding", &a.stringMaxSize, &missing);
    find(cf, nullptr, "CFDataCreate", &a.dataCreate, &missing);
    find(cf, nullptr, "CFDataGetLength", &a.dataGetLength, &missing);
    find(cf, nullptr, "CFDataGetBytePtr", &a.dataGetBytePtr, &missing);
    find(cf, nullptr, "CFPropertyListCreateData", &a.plistCreateData, &missing);
    find(cf, nullptr, "CFPropertyListCreateWithData", &a.plistCreateWithData, &missing);
    find(cf, nullptr, "CFArrayGetCount", &a.arrayGetCount, &missing);
    find(cf, nullptr, "CFArrayGetValueAtIndex", &a.arrayGetValueAtIndex, &missing);
    find(cf, nullptr, "CFDictionaryGetValue", &a.dictionaryGetValue, &missing);
    find(cf, nullptr, "CFNumberGetValue", &a.numberGetValue, &missing);
    find(cf, nullptr, "CFURLCreateFromFileSystemRepresentation", &a.urlCreateFromPath, &missing);
    find(cf, nullptr, "CFURLGetFileSystemRepresentation", &a.urlGetPath, &missing);
    find(cf, nullptr, "CFBundleCreate", &a.bundleCreate, &missing);
    find(cf, nullptr, "CFBundleGetValueForInfoDictionaryKey", &a.bundleInfoValue, &missing);
    find(cf, nullptr, "CFRunLoopGetMain", &a.runLoopGetMain, &missing);
    find(cf, nullptr, "CFRunLoopRunInMode", &a.runLoopRunInMode, &missing);

    find(objc, nullptr, "objc_getClass", &a.getClass, &missingEditor);
    find(objc, nullptr, "sel_registerName", &a.selector, &missingEditor);
    find(objc, nullptr, "objc_msgSend", &a.msgSend, &missingEditor);
#if defined(__x86_64__)
    find(objc, nullptr, "objc_msgSend_stret", &a.msgSendStret, &missingEditor);
#endif

    a.ok = missing.empty();
    a.editorOk = a.ok && missingEditor.empty();
    if (!a.ok) {
      a.error = "Audio Unit hosting could not find " + missing;
      if (!toolbox) a.error += " (AudioToolbox did not load)";
      if (!cf) a.error += " (CoreFoundation did not load)";
    } else if (!a.editorOk) {
      a.error = "Audio Unit editors could not find " + missingEditor;
    }
    return a;
  }();
  return api;
}

/** A CFString as UTF-8; empty for null or anything that is not a string. */
inline std::string auCfText(const AuApi& api, CFTypeRef value) {
  if (!value || api.getTypeID(value) != api.stringTypeID()) return std::string();
  CFStringRef s = (CFStringRef) value;
  const CFIndex capacity = api.stringMaxSize(api.stringGetLength(s), kCFStringEncodingUTF8) + 1;
  if (capacity <= 1) return std::string();
  std::string out((size_t) capacity, '\0');
  if (!api.stringGetCString(s, &out[0], capacity, kCFStringEncodingUTF8)) return std::string();
  out.resize(std::strlen(out.c_str()));
  return out;
}

/** A new CFString, which the caller releases. No CFSTR: that names a symbol
 *  in CoreFoundation, which this file does not link. */
inline CFStringRef auCfString(const AuApi& api, const char* text) {
  return api.stringCreate(nullptr, text ? text : "", kCFStringEncodingUTF8);
}

/** `selector` sent to `target` through the runtime's objc_msgSend, typed as
 *  the method is: on arm64 a wrongly typed send corrupts its arguments. */
template <typename Ret, typename... Args>
inline Ret auSend(id target, const char* selector, Args... args) {
  const AuApi& api = auApi();
  using Fn = Ret (*)(id, SEL, Args...);
  Fn fn = nullptr;
  std::memcpy(&fn, &api.msgSend, sizeof(fn));
  return fn(target, api.selector(selector), args...);
}

struct AuPoint {
  double x, y;
};
struct AuSize {
  double width, height;
};
/** An NSRect: four doubles, as CGRect lays them out. */
struct AuRect {
  AuPoint origin;
  AuSize size;
};

/** A message answering an NSRect. 32 bytes come back through a hidden pointer
 *  on x86_64, which needs objc_msgSend_stret; arm64 returns them through
 *  objc_msgSend itself. See gfx/window_cocoa.h's msgRect. */
inline AuRect auSendRect(id target, const char* selector) {
  const AuApi& api = auApi();
  using Fn = AuRect (*)(id, SEL);
  Fn fn = nullptr;
#if defined(__x86_64__)
  std::memcpy(&fn, &api.msgSendStret, sizeof(fn));
#else
  std::memcpy(&fn, &api.msgSend, sizeof(fn));
#endif
  if (!fn || !target) return AuRect{{0.0, 0.0}, {0.0, 0.0}};
  return fn(target, api.selector(selector));
}

inline id auClass(const char* name) {
  const AuApi& api = auApi();
  return api.getClass ? (id) api.getClass(name) : (id) nullptr;
}

/**
 * Wait for `state` to leave `pending`, for at most `seconds`.
 *
 * On the main thread the main run loop RUNS while it waits, in short slices:
 * an out-of-process AUv3's instantiation and its view controller both arrive
 * through it, and blocking it would be waiting for something the wait itself
 * prevents. Off the main thread it sleeps in millisecond steps.
 */
inline bool auWaitWhile(const std::atomic<int>& state, int pending, double seconds) {
  const AuApi& api = auApi();
  // The default mode by its value: modes compare as strings, so this is
  // kCFRunLoopDefaultMode without naming the exported constant.
  static const CFStringRef mode = auCfString(api, "kCFRunLoopDefaultMode");
  const bool onMain = pthread_main_np() != 0;
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds((long long) (seconds * 1000.0));
  while (state.load(std::memory_order_acquire) == pending) {
    if (std::chrono::steady_clock::now() >= deadline) return false;
    if (onMain && mode) {
      // A loop with nothing in it returns at once; then sleep instead of
      // spinning.
      if (api.runLoopRunInMode(mode, 0.01, true) == kCFRunLoopRunFinished)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
  return true;
}

/**
 * One AudioComponentInstantiate in flight.
 *
 * The completion runs on a thread of the system's choosing, perhaps after the
 * loader stopped waiting. So the request lives on the heap with two
 * references -- the loader's and the completion's -- and `state` decides,
 * atomically, who owns the instance: a completion that finds the loader gone
 * disposes it. A completion that never comes leaks this small object, which
 * is the only safe thing to do with memory the system still holds a block
 * pointer into.
 */
struct AuInstantiateRequest {
  using Invoke = void (*)(void* block, AudioComponentInstance instance, OSStatus status);
  static constexpr int kWaiting = 0, kDelivered = 1, kAbandoned = 2;

  ::sonore::detail::BlockLiteral<AuInstantiateRequest*, Invoke> block;
  std::atomic<int> state{kWaiting};
  std::atomic<int> references{2};
  AudioComponentInstance instance = nullptr;
  OSStatus status = 0;

  static void completed(void* literal, AudioComponentInstance instance, OSStatus status) {
    AuInstantiateRequest* self =
        ::sonore::detail::blockCapture<AuInstantiateRequest*, Invoke>(literal);
    self->instance = instance;
    self->status = status;
    int expected = kWaiting;
    if (!self->state.compare_exchange_strong(expected, kDelivered, std::memory_order_acq_rel) &&
        instance)
      // Nobody is waiting for it any more, and nobody is left to tell if
      // disposing it fails: the loader has already reported its timeout.
      (void) auApi().instanceDispose(instance);
    self->drop();
  }

  void drop() {
    if (references.fetch_sub(1, std::memory_order_acq_rel) == 1) delete this;
  }
};

/** An AUv3's view controller, asked for through
 *  kAudioUnitProperty_RequestViewController. Same ownership as
 *  AuInstantiateRequest; the controller is retained on arrival, and one
 *  that arrives after the editor stopped waiting is released on the main
 *  thread, where an NSViewController may be freed. */
struct AuViewRequest {
  using Invoke = void (*)(void* block, void* viewController);
  static constexpr int kWaiting = 0, kDelivered = 1, kAbandoned = 2;

  ::sonore::detail::BlockLiteral<AuViewRequest*, Invoke> block;
  std::atomic<int> state{kWaiting};
  std::atomic<int> references{2};
  id viewController = nullptr;

  static void completed(void* literal, void* viewController) {
    AuViewRequest* self = ::sonore::detail::blockCapture<AuViewRequest*, Invoke>(literal);
    id controller = (id) viewController;
    // Retained by us whatever the convention on the argument is: a
    // reference we take and later give back is right either way, where
    // trusting a +1 that was really +0 would be a crash.
    if (controller) auSend<id>(controller, "retain");
    self->viewController = controller;
    int expected = kWaiting;
    if (!self->state.compare_exchange_strong(expected, kDelivered, std::memory_order_acq_rel) &&
        controller)
      auSend<void>(controller, "performSelectorOnMainThread:withObject:waitUntilDone:",
                   auApi().selector("release"), (id) nullptr, (BOOL) NO);
    self->drop();
  }

  void drop() {
    if (references.fetch_sub(1, std::memory_order_acq_rel) == 1) delete this;
  }
};

/** A component's description as PluginDescription. Name and version are
 *  asked of the component; neither runs the unit's own code. */
inline PluginDescription auDescribeComponent(const AuApi& api, AudioComponent component,
                                             const AudioComponentDescription& acd) {
  PluginDescription d;
  d.format = "AU";
  d.id = auIdFor(acd.componentType, acd.componentSubType, acd.componentManufacturer);
  CFStringRef name = nullptr;
  if (api.copyName(component, &name) == noErr && name) {
    auSplitName(auCfText(api, name), &d.vendor, &d.name);
    api.release(name);
  }
  if (d.name.empty()) d.name = auCodeText(acd.componentSubType);
  if (d.vendor.empty()) d.vendor = auCodeText(acd.componentManufacturer);
  UInt32 version = 0;
  if (api.getVersion(component, &version) == noErr) d.version = auVersionText(version);
  d.features = auTypeWord(acd.componentType);
  if (acd.componentFlags & aucode::kFlagIsV3AudioUnit) d.features += ";AUv3";
  d.isInstrument = acd.componentType == aucode::kTypeMusicDevice;
  return d;
}

/** Non-interleaved 32-bit float at a rate and a width: what every AU host
 *  speaks and what this SDK's planar buffers already are. */
inline AudioStreamBasicDescription auFloatFormat(double sampleRate, UInt32 channels) {
  AudioStreamBasicDescription f;
  std::memset(&f, 0, sizeof(f));
  f.mSampleRate = sampleRate;
  f.mFormatID = kAudioFormatLinearPCM;
  f.mFormatFlags = kAudioFormatFlagsNativeFloatPacked | kAudioFormatFlagIsNonInterleaved;
  f.mBytesPerPacket = sizeof(float);
  f.mFramesPerPacket = 1;
  f.mBytesPerFrame = sizeof(float);
  f.mChannelsPerFrame = channels;
  f.mBitsPerChannel = 32;
  return f;
}

} // namespace detail

/**
 * One instantiated Audio Unit, driven as host.h drives every format.
 *
 * Main-thread for everything but process(). AU-specific extras: lastError()
 * says what the last thing that failed was and with which OSStatus, by name;
 * renderFailures() counts blocks the unit refused to render (those leave the
 * block as it came, as a block too long does).
 */
class AuPlugin final : public HostedPlugin {
public:
  ~AuPlugin() override { unload(); }

  const PluginDescription& description() const override { return desc_; }
  bool isValid() const override { return unit_ != nullptr; }

  bool prepare(double sampleRate, uint32_t maxBlockSize, uint32_t numChannels = 2) override;

  void release() override {
    if (unit_ && initialized_) {
      const OSStatus status = detail::auApi().uninitialize(unit_);
      if (status != noErr) note("AudioUnitUninitialize", status);
    }
    initialized_ = false;
    prepared_ = false;
  }

  void reset() override {
    if (!unit_ || !initialized_) return;
    const OSStatus status = detail::auApi().reset(unit_, kAudioUnitScope_Global, 0);
    if (status != noErr) note("AudioUnitReset", status);
  }

  void process(AudioBlock<float>& io, const MidiBuffer* midi = nullptr) override;

  uint32_t latencySamples() const override { return secondsProperty(kAudioUnitProperty_Latency); }
  uint32_t tailSamples() const override { return secondsProperty(kAudioUnitProperty_TailTime); }
  bool takeRestartRequest() override { return restartRequested_.exchange(false); }
  bool takeStateDirty() override { return stateDirty_.exchange(false); }
  bool acceptsMidi() const override { return acceptsMidi_; }
  bool hasAudioInput() const override { return inputBuses_ > 0 && inChannels_ > 0; }

  int numParameters() const override { return (int) params_.size(); }
  const HostedParam& parameter(int index) const override { return params_[(size_t) index]; }

  double parameterValue(int index) const override {
    if (!unit_ || index < 0 || index >= numParameters()) return 0.0;
    AudioUnitParameterValue value = 0.0f;
    const OSStatus status = detail::auApi().getParameter(unit_, params_[(size_t) index].id,
                                                         kAudioUnitScope_Global, 0, &value);
    return status == noErr ? (double) value : params_[(size_t) index].defaultValue;
  }

  /** AudioUnitSetParameter, which a unit applies to its next render and which
   *  -- unlike AUParameterSet -- does not echo back to listeners, so the
   *  host's own writes do not come back as edits. */
  bool setParameterValue(int index, double value) override {
    if (!unit_ || index < 0 || index >= numParameters()) return false;
    if (!detail::auParamIsSettable(paramFlags_[(size_t) index])) return false;
    const HostedParam& p = params_[(size_t) index];
    const double clamped = value < p.minValue ? p.minValue : (value > p.maxValue ? p.maxValue : value);
    const OSStatus status = detail::auApi().setParameter(unit_, p.id, kAudioUnitScope_Global, 0,
                                                         (AudioUnitParameterValue) clamped, 0);
    if (status != noErr) {
      note("AudioUnitSetParameter", status);
      return false;
    }
    return true;
  }

  std::string parameterText(int index, double value) const override;

  /** kAudioUnitProperty_ClassInfo, written as an XML property list -- which
   *  is byte for byte what a .aupreset file holds, so plugin_cache.h's preset
   *  files for an AU are .aupreset files other hosts read. */
  bool saveState(std::vector<uint8_t>& out) const override;
  /** Any property list a unit's ClassInfo came from, XML or binary. The unit
   *  checks the type/subtype/manufacturer inside it and refuses another
   *  unit's state. */
  bool loadState(const uint8_t* data, size_t size) override;

  const MidiBuffer& producedMidi() const override { return produced_; }

  bool hasBypass() const override { return hasBypass_; }
  bool isBypassed() const override {
    UInt32 on = 0;
    return hasBypass_ && getValue(kAudioUnitProperty_BypassEffect, kAudioUnitScope_Global, 0, on) ==
                             noErr && on != 0;
  }
  bool setBypassed(bool bypassed) override {
    if (!hasBypass_) return false;
    const UInt32 on = bypassed ? 1 : 0;
    const OSStatus status = setValue(kAudioUnitProperty_BypassEffect, kAudioUnitScope_Global, 0, on);
    if (status != noErr) note("set kAudioUnitProperty_BypassEffect", status);
    return status == noErr;
  }

  int numPresets() const override { return (int) presets_.size(); }
  std::string presetName(int index) const override {
    if (index < 0 || index >= numPresets()) return std::string();
    return presets_[(size_t) index].name;
  }
  bool loadPreset(int index) override;

  size_t drainParameterEdits(std::vector<ParamEdit>& out) override {
    std::lock_guard<std::mutex> lock(editLock_);
    const size_t added = edits_.size();
    out.insert(out.end(), edits_.begin(), edits_.end());
    edits_.clear();
    return added;
  }

  bool hasEditor() const override {
    return detail::auApi().editorOk && (cocoaView_ || viewController_);
  }
  bool openEditor(void* parent) override;
  void closeEditor() override;
  /** Nothing to do: an AU's view runs on the platform's own run loop. */
  void idleEditor() override {}
  bool editorSize(uint32_t& width, uint32_t& height) const override {
    if (!editorView_) return false;
    const detail::AuRect frame = detail::auSendRect(editorView_, "frame");
    if (!(frame.size.width > 0.0) || !(frame.size.height > 0.0)) return false;
    width = (uint32_t) (frame.size.width + 0.5);
    height = (uint32_t) (frame.size.height + 0.5);
    return true;
  }
  /** Cocoa scales a view itself, by its window's backing factor; there is
   *  nothing to tell an AU. */
  bool setEditorScale(double) override { return false; }

  // ── AU-specific ────────────────────────────────────────────────────────

  /** The last thing that failed, with its OSStatus by name. Main-thread. */
  const std::string& lastError() const { return lastError_; }
  /** Blocks the unit refused to render since prepare(), and the status of
   *  the last refusal. */
  uint32_t renderFailures() const { return renderFailures_.load(std::memory_order_relaxed); }
  /** MIDI messages the unit refused (MusicDeviceMIDIEvent or MusicDeviceSysEx
   *  returned an error) since prepare(). */
  uint32_t midiFailures() const { return midiFailures_.load(std::memory_order_relaxed); }
  int32_t lastRenderStatus() const { return lastRenderStatus_.load(std::memory_order_relaxed); }
  /** The AudioUnit itself, for anything this class does not cover. */
  AudioUnit audioUnit() const { return unit_; }
  /** The registry's description, with its flags. */
  const AudioComponentDescription& componentDescription() const { return component_; }
  bool isV3() const { return (component_.componentFlags & detail::aucode::kFlagIsV3AudioUnit) != 0; }
  /** The widths prepare() settled on: what the unit renders, before host.h's
   *  usual mapping onto the caller's block. */
  uint32_t inputChannels() const { return inChannels_; }
  uint32_t outputChannels() const { return outChannels_; }

private:
  friend std::unique_ptr<HostedPlugin> loadAudioUnit(const PluginDescription&, std::string*);
  AuPlugin() = default;

  struct Preset {
    SInt32 number = 0;
    std::string name;
  };

  /** Input memory a unit may ask for (renderInput): this many buses of this
   *  many channels, a block each. Past it, a render fails and says so. */
  static constexpr UInt32 kScratchBuses = 4;
  static constexpr UInt32 kScratchChannels = 8;

  // ── Small helpers, each one status-checked ─────────────────────────────

  void note(const char* what, OSStatus status) const {
    lastError_ = std::string(what) + " failed: " + detail::auStatusText(status);
  }

  template <typename T>
  OSStatus getValue(AudioUnitPropertyID property, AudioUnitScope scope, AudioUnitElement element,
                    T& out) const {
    UInt32 size = (UInt32) sizeof(T);
    return detail::auApi().getProperty(unit_, property, scope, element, &out, &size);
  }

  template <typename T>
  OSStatus setValue(AudioUnitPropertyID property, AudioUnitScope scope, AudioUnitElement element,
                    const T& value) {
    return detail::auApi().setProperty(unit_, property, scope, element, &value, (UInt32) sizeof(T));
  }

  bool hasProperty(AudioUnitPropertyID property, AudioUnitScope scope, AudioUnitElement element,
                   UInt32* size = nullptr, Boolean* writable = nullptr) const {
    UInt32 s = 0;
    Boolean w = false;
    const OSStatus status = detail::auApi().getPropertyInfo(unit_, property, scope, element, &s, &w);
    if (size) *size = s;
    if (writable) *writable = w;
    return status == noErr;
  }

  /** A seconds-valued property in samples at the prepared rate. */
  uint32_t secondsProperty(AudioUnitPropertyID property) const {
    if (!unit_) return 0;
    Float64 seconds = 0.0;
    if (getValue(property, kAudioUnitScope_Global, 0, seconds) != noErr) return 0;
    const double samples = seconds * sampleRate_;
    if (!(samples > 0.0)) return 0;
    return samples >= 4294967295.0 ? 0xFFFFFFFFu : (uint32_t) (samples + 0.5);
  }

  UInt32 elementCount(AudioUnitScope scope) const {
    UInt32 count = 0;
    return getValue(kAudioUnitProperty_ElementCount, scope, 0, count) == noErr ? count : 0;
  }

  /** Read once at load, and again after a unit says its list changed. */
  void readParameters();
  void readPresets();
  void readCapabilities();
  void listen();
  bool setStreamFormats(detail::AuChannelPair channels);
  bool openCocoaView(id parent);
  bool openViewController(id parent);
  void embed(id view, id parent);

  void recordEdit(ParamEdit::Kind kind, uint32_t id, double value) {
    if (edits_.size() >= 4096) return;
    if (id == detail::aucode::kAnyParameter) {
      // "Everything moved", from a preset or a restored state: each one as
      // the value it now has.
      for (size_t i = 0; i < params_.size() && edits_.size() < 4096; ++i)
        edits_.push_back({ParamEdit::Kind::kValue, (int) i, parameterValue((int) i)});
      return;
    }
    const int index = paramIndex_.find(id);
    if (index >= 0) edits_.push_back({kind, index, value});
  }

  // ── Callbacks the unit makes ───────────────────────────────────────────

  /** The unit pulling its input -- an AU's input is a callback it calls,
   *  from inside AudioUnitRender, on the audio thread. Bus 0 is the caller's
   *  block; any other bus (a sidechain) is silence, because host.h's process()
   *  carries one input. */
  static OSStatus renderInput(void* refCon, AudioUnitRenderActionFlags* flags,
                              const AudioTimeStamp*, UInt32 bus, UInt32 frames,
                              AudioBufferList* data) {
    auto* self = static_cast<AuPlugin*>(refCon);
    if (!self || !data) return noErr;
    const AudioBlock<float>* source = bus == 0 ? self->currentInput_ : nullptr;
    const size_t sourceChannels = source ? source->getNumChannels() : 0;
    const size_t sourceFrames = source ? source->getNumSamples() : 0;
    for (UInt32 b = 0; b < data->mNumberBuffers; ++b) {
      AudioBuffer& buffer = data->mBuffers[b];
      if (!buffer.mData) {
        // The unit asks for our memory, which AU allows: hand it one of the
        // blocks prepare() made -- its own per bus and channel, so two buses
        // pulled in one render never share one -- never one grown here.
        if (bus >= kScratchBuses || b >= kScratchChannels || frames > self->maxBlock_)
          return kAudioUnitErr_TooManyFramesToProcess;
        buffer.mData = self->inStore_.data() +
                       ((size_t) bus * kScratchChannels + b) * self->maxBlock_;
        buffer.mDataByteSize = frames * (UInt32) sizeof(float);
      }
      float* dest = static_cast<float*>(buffer.mData);
      UInt32 n = buffer.mDataByteSize / (UInt32) sizeof(float);
      if (n > frames) n = frames;
      if (sourceChannels > 0) {
        // A wider unit than the caller's block hears the last channel again,
        // as the LV2 host does.
        const size_t channel = b < sourceChannels ? b : sourceChannels - 1;
        const UInt32 copy = (UInt32) (sourceFrames < n ? sourceFrames : n);
        std::memcpy(dest, source->getChannelPointer(channel), copy * sizeof(float));
        if (copy < n) std::memset(dest + copy, 0, (n - copy) * sizeof(float));
      } else {
        std::memset(dest, 0, n * sizeof(float));
      }
    }
    if (sourceChannels == 0 && flags) *flags |= kAudioUnitRenderAction_OutputIsSilence;
    return noErr;
  }

  /** What the unit emits, during AudioUnitRender, on the audio thread. Each
   *  packet's timestamp is its sample offset in the block -- what
   *  au_wrapper.h writes, and what AU hosts read. */
  static OSStatus midiOutput(void* userData, const AudioTimeStamp*, UInt32,
                             const MIDIPacketList* list) {
    auto* self = static_cast<AuPlugin*>(userData);
    if (!self || !list) return noErr;
    const int last = self->blockFrames_ > 0 ? (int) self->blockFrames_ - 1 : 0;
    const MIDIPacket* packet = &list->packet[0];
    for (UInt32 i = 0; i < list->numPackets; ++i) {
      const int at = packet->timeStamp > (MIDITimeStamp) last ? last : (int) packet->timeStamp;
      detail::auParseMidiPacket(packet->data, packet->length, at, self->produced_);
      packet = MIDIPacketNext(packet);
    }
    return noErr;
  }

  /** AUEventListener's callback, on the main run loop: an edit made on the
   *  unit's own face (AUParameterSet), a gesture, or a property the host
   *  must act on. */
  static void onEvent(void* refCon, void*, const AudioUnitEvent* event, UInt64,
                      AudioUnitParameterValue value) {
    auto* self = static_cast<AuPlugin*>(refCon);
    if (!self || !event) return;
    switch (event->mEventType) {
      case kAudioUnitEvent_ParameterValueChange: {
        std::lock_guard<std::mutex> lock(self->editLock_);
        self->recordEdit(ParamEdit::Kind::kValue, event->mArgument.mParameter.mParameterID, value);
        break;
      }
      case kAudioUnitEvent_BeginParameterChangeGesture: {
        std::lock_guard<std::mutex> lock(self->editLock_);
        self->recordEdit(ParamEdit::Kind::kGestureBegin, event->mArgument.mParameter.mParameterID, 0.0);
        break;
      }
      case kAudioUnitEvent_EndParameterChangeGesture: {
        std::lock_guard<std::mutex> lock(self->editLock_);
        self->recordEdit(ParamEdit::Kind::kGestureEnd, event->mArgument.mParameter.mParameterID, 0.0);
        break;
      }
      case kAudioUnitEvent_PropertyChange:
        switch (event->mArgument.mProperty.mPropertyID) {
          case kAudioUnitProperty_Latency:
          case kAudioUnitProperty_ParameterList:
            // A new latency is applied by preparing again, as for the other
            // formats; a new parameter list is read there too.
            self->restartRequested_.store(true);
            if (event->mArgument.mProperty.mPropertyID == kAudioUnitProperty_ParameterList)
              self->parametersStale_.store(true);
            break;
          case kAudioUnitProperty_ClassInfo:
            // How an AU says its state moved without a parameter: the host's
            // cue to mark the session edited.
            self->stateDirty_.store(true);
            break;
          default:
            break;
        }
        break;
      default:
        break;
    }
  }

  void unload() {
    closeEditor();
    const detail::AuApi& api = detail::auApi();
    if (listener_) {
      const OSStatus status = api.listenerDispose(listener_);
      if (status != noErr) note("AUListenerDispose", status);
      listener_ = nullptr;
    }
    if (unit_) {
      if (initialized_) {
        const OSStatus status = api.uninitialize(unit_);
        if (status != noErr) note("AudioUnitUninitialize", status);
      }
      const OSStatus status = api.instanceDispose(unit_);
      if (status != noErr) note("AudioComponentInstanceDispose", status);
    }
    unit_ = nullptr;
    initialized_ = prepared_ = false;
  }

  PluginDescription desc_;
  AudioComponentDescription component_{};
  AudioUnit unit_ = nullptr;
  AUEventListenerRef listener_ = nullptr;

  std::vector<HostedParam> params_;
  std::vector<uint32_t> paramFlags_;
  std::vector<uint32_t> paramUnits_;
  /** Per parameter: the names an indexed parameter's values have, if any. */
  std::vector<std::vector<std::string>> valueStrings_;
  ParamIdIndex paramIndex_;
  std::vector<Preset> presets_;

  std::mutex editLock_;
  std::vector<ParamEdit> edits_;
  std::atomic<bool> restartRequested_{false};
  std::atomic<bool> stateDirty_{false};
  std::atomic<bool> parametersStale_{false};

  bool acceptsMidi_ = false;
  bool hasBypass_ = false;
  bool cocoaView_ = false;
  bool viewController_ = false;
  bool emitsMidi_ = false;
  UInt32 inputBuses_ = 0, outputBuses_ = 0;

  // prepare()'s results
  double sampleRate_ = 44100.0;
  uint32_t maxBlock_ = 0;
  uint32_t inChannels_ = 0, outChannels_ = 0;
  bool initialized_ = false;
  bool prepared_ = false;
  std::vector<float> inStore_, outStore_;
  std::vector<float*> outPtrs_;
  /** The output AudioBufferList: a header and outChannels_ buffers, in
   *  64-bit words so the list is aligned as Apple's struct is. */
  std::vector<uint64_t> outList_;

  // process()'s state, touched only on the audio thread
  const AudioBlock<float>* currentInput_ = nullptr;
  uint32_t blockFrames_ = 0;
  Float64 sampleTime_ = 0.0;
  MidiBuffer produced_;
  std::atomic<uint32_t> renderFailures_{0};
  std::atomic<uint32_t> midiFailures_{0};
  std::atomic<int32_t> lastRenderStatus_{0};

  // The editor
  id editorView_ = nullptr;
  id editorController_ = nullptr;

  mutable std::string lastError_;
};

inline void AuPlugin::readParameters() {
  const detail::AuApi& api = detail::auApi();
  params_.clear();
  paramFlags_.clear();
  paramUnits_.clear();
  valueStrings_.clear();
  UInt32 size = 0;
  if (!hasProperty(kAudioUnitProperty_ParameterList, kAudioUnitScope_Global, 0, &size) ||
      size < sizeof(AudioUnitParameterID)) {
    paramIndex_.build(params_);
    return; // a unit with no parameters is legal
  }
  std::vector<AudioUnitParameterID> ids(size / sizeof(AudioUnitParameterID));
  OSStatus status = api.getProperty(unit_, kAudioUnitProperty_ParameterList, kAudioUnitScope_Global,
                                    0, ids.data(), &size);
  if (status != noErr) {
    note("get kAudioUnitProperty_ParameterList", status);
    paramIndex_.build(params_);
    return;
  }
  ids.resize(size / sizeof(AudioUnitParameterID));

  std::vector<std::pair<UInt32, std::string>> clumps;
  for (AudioUnitParameterID pid : ids) {
    AudioUnitParameterInfo info;
    std::memset(&info, 0, sizeof(info));
    UInt32 infoSize = sizeof(info);
    status = api.getProperty(unit_, kAudioUnitProperty_ParameterInfo, kAudioUnitScope_Global, pid,
                             &info, &infoSize);
    if (status != noErr) {
      note("get kAudioUnitProperty_ParameterInfo", status);
      continue;
    }
    detail::AuParamFacts facts;
    facts.id = pid;
    facts.unit = info.unit;
    facts.minValue = info.minValue;
    facts.maxValue = info.maxValue;
    facts.defaultValue = info.defaultValue;
    facts.flags = info.flags;
    if ((info.flags & detail::aucode::kParamHasCFNameString) && info.cfNameString) {
      facts.name = detail::auCfText(api, info.cfNameString);
      // Released only when the unit says the reference is ours. unitName is
      // left alone: whether CFNameRelease covers it is not stated plainly,
      // and a leak is the recoverable mistake of the two.
      if (info.flags & detail::aucode::kParamCFNameRelease) api.release(info.cfNameString);
    }
    if (facts.name.empty()) {
      size_t n = 0;
      while (n < sizeof(info.name) && info.name[n]) ++n;
      facts.name.assign(info.name, n);
    }
    if ((info.flags & detail::aucode::kParamHasClump) && info.clumpID != 0) {
      bool known = false;
      for (const auto& c : clumps)
        if (c.first == info.clumpID) {
          facts.clump = c.second;
          known = true;
        }
      if (!known) {
        AudioUnitParameterNameInfo clump;
        std::memset(&clump, 0, sizeof(clump));
        clump.inID = info.clumpID;
        clump.inDesiredLength = kAudioUnitParameterName_Full;
        UInt32 clumpSize = sizeof(clump);
        if (api.getProperty(unit_, kAudioUnitProperty_ParameterClumpName, kAudioUnitScope_Global, 0,
                            &clump, &clumpSize) == noErr &&
            clump.outName) {
          facts.clump = detail::auCfText(api, clump.outName);
          api.release(clump.outName); // a copy, the caller's to release
        }
        clumps.push_back({info.clumpID, facts.clump});
      }
    }
    std::vector<std::string> names;
    if (info.flags & detail::aucode::kParamValuesHaveStrings) {
      CFArrayRef strings = nullptr;
      UInt32 stringsSize = sizeof(strings);
      if (api.getProperty(unit_, kAudioUnitProperty_ParameterValueStrings, kAudioUnitScope_Global,
                          pid, &strings, &stringsSize) == noErr &&
          strings) {
        if (api.getTypeID(strings) == api.arrayTypeID()) {
          const CFIndex count = api.arrayGetCount(strings);
          for (CFIndex k = 0; k < count; ++k)
            names.push_back(detail::auCfText(api, api.arrayGetValueAtIndex(strings, k)));
        }
        api.release(strings);
      }
    }
    params_.push_back(detail::auHostedParam(facts));
    paramFlags_.push_back(info.flags);
    paramUnits_.push_back(info.unit);
    valueStrings_.push_back(std::move(names));
  }
  paramIndex_.build(params_);
  parametersStale_.store(false);
}

inline void AuPlugin::readPresets() {
  const detail::AuApi& api = detail::auApi();
  presets_.clear();
  if (!hasProperty(kAudioUnitProperty_FactoryPresets, kAudioUnitScope_Global, 0)) return;
  CFArrayRef list = nullptr;
  const OSStatus status = getValue(kAudioUnitProperty_FactoryPresets, kAudioUnitScope_Global, 0, list);
  if (status != noErr || !list) {
    if (status != noErr) note("get kAudioUnitProperty_FactoryPresets", status);
    return;
  }
  // A CFArray of AUPreset POINTERS into the unit's own storage, by
  // convention: copied out here, then the array is the caller's to release.
  const CFIndex count = api.arrayGetCount(list);
  for (CFIndex i = 0; i < count; ++i) {
    const auto* preset = static_cast<const AUPreset*>(api.arrayGetValueAtIndex(list, i));
    if (!preset) continue;
    Preset p;
    p.number = preset->presetNumber;
    p.name = detail::auCfText(api, preset->presetName);
    presets_.push_back(std::move(p));
  }
  api.release(list);
}

inline void AuPlugin::readCapabilities() {
  inputBuses_ = elementCount(kAudioUnitScope_Input);
  outputBuses_ = elementCount(kAudioUnitScope_Output);
  acceptsMidi_ = detail::auTypeTakesMidi(component_.componentType);
  Boolean writable = false;
  hasBypass_ = hasProperty(kAudioUnitProperty_BypassEffect, kAudioUnitScope_Global, 0, nullptr,
                           &writable) &&
               writable;
  UInt32 size = 0;
  cocoaView_ = hasProperty(kAudioUnitProperty_CocoaUI, kAudioUnitScope_Global, 0, &size) &&
               size >= sizeof(AudioUnitCocoaViewInfo);
  viewController_ = hasProperty((AudioUnitPropertyID) detail::aucode::kPropertyRequestViewController,
                                kAudioUnitScope_Global, 0, &size) &&
                    size == sizeof(void*);
  emitsMidi_ = hasProperty(kAudioUnitProperty_MIDIOutputCallbackInfo, kAudioUnitScope_Global, 0);
  readParameters();
  readPresets();
}

inline void AuPlugin::listen() {
  const detail::AuApi& api = detail::auApi();
  // The main run loop, in its common modes so edits keep arriving while the
  // host tracks a mouse or runs a modal dialog. By value, like the default
  // mode: kCFRunLoopCommonModes is that string.
  CFStringRef common = detail::auCfString(api, "kCFRunLoopCommonModes");
  OSStatus status = api.eventListenerCreate(&AuPlugin::onEvent, this, api.runLoopGetMain(), common,
                                            0.0f, 0.0f, &listener_);
  if (common) api.release(common);
  if (status != noErr || !listener_) {
    listener_ = nullptr;
    note("AUEventListenerCreate", status);
    return;
  }
  // One registration per parameter and kind: the wildcard id is for
  // notifying, not for listening.
  const AudioUnitEventType kinds[] = {kAudioUnitEvent_ParameterValueChange,
                                      kAudioUnitEvent_BeginParameterChangeGesture,
                                      kAudioUnitEvent_EndParameterChangeGesture};
  for (const HostedParam& p : params_)
    for (AudioUnitEventType kind : kinds) {
      AudioUnitEvent event;
      std::memset(&event, 0, sizeof(event));
      event.mEventType = kind;
      event.mArgument.mParameter.mAudioUnit = unit_;
      event.mArgument.mParameter.mParameterID = p.id;
      event.mArgument.mParameter.mScope = kAudioUnitScope_Global;
      event.mArgument.mParameter.mElement = 0;
      status = api.eventListenerAdd(listener_, this, &event);
      if (status != noErr) note("AUEventListenerAddEventType(parameter)", status);
    }
  const AudioUnitPropertyID properties[] = {kAudioUnitProperty_Latency, kAudioUnitProperty_ClassInfo,
                                            kAudioUnitProperty_ParameterList};
  for (AudioUnitPropertyID property : properties) {
    AudioUnitEvent event;
    std::memset(&event, 0, sizeof(event));
    event.mEventType = kAudioUnitEvent_PropertyChange;
    event.mArgument.mProperty.mAudioUnit = unit_;
    event.mArgument.mProperty.mPropertyID = property;
    event.mArgument.mProperty.mScope = kAudioUnitScope_Global;
    event.mArgument.mProperty.mElement = 0;
    status = api.eventListenerAdd(listener_, this, &event);
    if (status != noErr) note("AUEventListenerAddEventType(property)", status);
  }
}

inline bool AuPlugin::setStreamFormats(detail::AuChannelPair channels) {
  // Main output, then main input: the two widths that are being negotiated.
  const AudioStreamBasicDescription out = detail::auFloatFormat(sampleRate_, (UInt32) channels.out);
  OSStatus status = setValue(kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, out);
  if (status != noErr) {
    note("set kAudioUnitProperty_StreamFormat (output 0)", status);
    return false;
  }
  if (inputBuses_ > 0 && channels.in > 0) {
    const AudioStreamBasicDescription in = detail::auFloatFormat(sampleRate_, (UInt32) channels.in);
    status = setValue(kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, in);
    if (status != noErr) {
      note("set kAudioUnitProperty_StreamFormat (input 0)", status);
      return false;
    }
  }
  // Every other bus keeps its width and takes the rate: a unit whose buses
  // disagree on the rate may refuse to initialise. A refusal here is noted,
  // not fatal -- host.h renders bus 0 only.
  for (int pass = 0; pass < 2; ++pass) {
    const AudioUnitScope scope = pass == 0 ? kAudioUnitScope_Input : kAudioUnitScope_Output;
    const UInt32 buses = pass == 0 ? inputBuses_ : outputBuses_;
    for (UInt32 bus = 1; bus < buses; ++bus) {
      AudioStreamBasicDescription format;
      std::memset(&format, 0, sizeof(format));
      if (getValue(kAudioUnitProperty_StreamFormat, scope, bus, format) != noErr) continue;
      if (format.mSampleRate == sampleRate_) continue;
      format.mSampleRate = sampleRate_;
      status = setValue(kAudioUnitProperty_StreamFormat, scope, bus, format);
      if (status != noErr) note("set kAudioUnitProperty_StreamFormat (another bus's rate)", status);
    }
  }
  inChannels_ = inputBuses_ > 0 ? (uint32_t) channels.in : 0;
  outChannels_ = (uint32_t) channels.out;
  return true;
}

inline bool AuPlugin::prepare(double sampleRate, uint32_t maxBlockSize, uint32_t numChannels) {
  if (!unit_) return false;
  const detail::AuApi& api = detail::auApi();
  if (initialized_) {
    const OSStatus status = api.uninitialize(unit_);
    if (status != noErr) note("AudioUnitUninitialize", status);
    initialized_ = false;
  }
  prepared_ = false;
  if (parametersStale_.load()) {
    readParameters();
    if (listener_) {
      const OSStatus status = api.listenerDispose(listener_);
      if (status != noErr) note("AUListenerDispose", status);
      listener_ = nullptr;
    }
    listen();
  }
  sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
  maxBlock_ = maxBlockSize > 0 ? maxBlockSize : 1;
  int wanted = (int) numChannels;
  if (wanted < 1) wanted = 1;
  if (wanted > 16) wanted = 16;

  inputBuses_ = elementCount(kAudioUnitScope_Input);
  outputBuses_ = elementCount(kAudioUnitScope_Output);
  if (outputBuses_ == 0) {
    lastError_ = "the unit has no output bus to render";
    return false;
  }

  // What widths the unit takes, then the first that it actually accepts.
  std::vector<detail::AuChannelPair> entries;
  UInt32 size = 0;
  if (hasProperty(kAudioUnitProperty_SupportedNumChannels, kAudioUnitScope_Global, 0, &size) &&
      size >= sizeof(AUChannelInfo)) {
    std::vector<AUChannelInfo> infos(size / sizeof(AUChannelInfo));
    const OSStatus status = api.getProperty(unit_, kAudioUnitProperty_SupportedNumChannels,
                                            kAudioUnitScope_Global, 0, infos.data(), &size);
    if (status == noErr) {
      infos.resize(size / sizeof(AUChannelInfo));
      for (const AUChannelInfo& info : infos) entries.push_back({info.inChannels, info.outChannels});
    } else {
      note("get kAudioUnitProperty_SupportedNumChannels", status);
    }
  }
  std::vector<detail::AuChannelPair> candidates =
      detail::auChannelCandidates(entries, wanted, inputBuses_ > 0);
  if (candidates.empty()) candidates.push_back({inputBuses_ > 0 ? wanted : 0, wanted});
  bool formatted = false;
  for (const detail::AuChannelPair& candidate : candidates)
    if (setStreamFormats(candidate)) {
      formatted = true;
      break;
    }
  if (!formatted) return false; // lastError_ says which format was refused

  const UInt32 maxFrames = maxBlock_;
  OSStatus status = setValue(kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0,
                             maxFrames);
  if (status != noErr) {
    note("set kAudioUnitProperty_MaximumFramesPerSlice", status);
    return false;
  }

  // An input callback on every input bus: a unit that pulls a bus nobody
  // feeds gets kAudioUnitErr_NoConnection and renders nothing.
  for (UInt32 bus = 0; bus < inputBuses_; ++bus) {
    AURenderCallbackStruct callback;
    callback.inputProc = &AuPlugin::renderInput;
    callback.inputProcRefCon = this;
    status = setValue(kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, bus, callback);
    if (status != noErr) {
      note("set kAudioUnitProperty_SetRenderCallback", status);
      if (bus == 0) return false;
    }
  }

  if (emitsMidi_) {
    AUMIDIOutputCallbackStruct out;
    out.midiOutputCallback = &AuPlugin::midiOutput;
    out.userData = this;
    status = setValue(kAudioUnitProperty_MIDIOutputCallback, kAudioUnitScope_Global, 0, out);
    if (status != noErr) note("set kAudioUnitProperty_MIDIOutputCallback", status);
  }

  status = api.initialize(unit_);
  if (status != noErr) {
    note("AudioUnitInitialize", status);
    return false;
  }
  initialized_ = true;

  // Every buffer process() uses, sized here.
  inStore_.assign((size_t) (inputBuses_ > 0 ? kScratchBuses * kScratchChannels : 0) * maxBlock_, 0.0f);
  outStore_.assign((size_t) outChannels_ * maxBlock_, 0.0f);
  outPtrs_.assign(outChannels_, nullptr);
  for (size_t c = 0; c < outPtrs_.size(); ++c) outPtrs_[c] = outStore_.data() + c * maxBlock_;
  const size_t listBytes = offsetof(AudioBufferList, mBuffers) + (size_t) outChannels_ * sizeof(AudioBuffer);
  outList_.assign((listBytes + sizeof(uint64_t) - 1) / sizeof(uint64_t), 0);

  sampleTime_ = 0.0;
  renderFailures_.store(0);
  midiFailures_.store(0);
  lastRenderStatus_.store(0);
  prepared_ = true;
  return true;
}

inline void AuPlugin::process(AudioBlock<float>& io, const MidiBuffer* midi) {
  produced_.clear(); // this block's emissions only, whether or not it renders
  if (!prepared_ || !unit_) return;
  const uint32_t frames = (uint32_t) io.getNumSamples();
  if (frames == 0 || frames > maxBlock_) return;
  const detail::AuApi& api = detail::auApi();

  if (midi && acceptsMidi_) {
    // Ahead of the render, each at its offset: MusicDeviceMIDIEvent is how
    // AU schedules MIDI into the next AudioUnitRender.
    for (const auto& entry : *midi) {
      const MidiMessage& m = entry.getMessage();
      const int status = m.getRawStatus();
      if (shortMessageLength(status) <= 0) continue;
      const int at = entry.samplePosition < 0
                         ? 0
                         : (entry.samplePosition >= (int) frames ? (int) frames - 1 : entry.samplePosition);
      if (api.midiEvent(unit_, (UInt32) status, (UInt32) m.getRawData1(), (UInt32) m.getRawData2(),
                        (UInt32) at) != noErr)
        midiFailures_.fetch_add(1, std::memory_order_relaxed);
    }
    for (const MidiBuffer::SysexEntry* s = midi->sysexBegin(); s != midi->sysexEnd(); ++s)
      if (api.sysEx(unit_, midi->sysexData(*s), (UInt32) s->length) != noErr)
        midiFailures_.fetch_add(1, std::memory_order_relaxed);
  }

  // The list points at our buffers on every block: a unit may have pointed
  // it at its own last time, which AU permits.
  auto* list = reinterpret_cast<AudioBufferList*>(outList_.data());
  list->mNumberBuffers = outChannels_;
  for (uint32_t b = 0; b < outChannels_; ++b) {
    list->mBuffers[b].mNumberChannels = 1;
    list->mBuffers[b].mDataByteSize = frames * (UInt32) sizeof(float);
    list->mBuffers[b].mData = outPtrs_[b];
  }

  AudioTimeStamp stamp;
  std::memset(&stamp, 0, sizeof(stamp));
  stamp.mSampleTime = sampleTime_;
  stamp.mFlags = kAudioTimeStampSampleTimeValid;
  AudioUnitRenderActionFlags flags = 0;
  currentInput_ = &io;
  blockFrames_ = frames;
  const OSStatus status = api.render(unit_, &flags, &stamp, 0, frames, list);
  currentInput_ = nullptr;
  sampleTime_ += frames;
  if (status != noErr) {
    // Refused: the block is left as it came, as a block too long is.
    lastRenderStatus_.store(status, std::memory_order_relaxed);
    renderFailures_.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  const uint32_t chans = (uint32_t) io.getNumChannels();
  for (uint32_t c = 0; c < chans && outChannels_ > 0; ++c) {
    const AudioBuffer& buffer = list->mBuffers[c < outChannels_ ? c : outChannels_ - 1];
    float* dest = io.getChannelPointer(c);
    if (buffer.mData)
      std::memcpy(dest, buffer.mData, frames * sizeof(float));
    else
      std::memset(dest, 0, frames * sizeof(float));
  }
}

inline std::string AuPlugin::parameterText(int index, double value) const {
  if (index < 0 || index >= numParameters()) return std::string();
  const detail::AuApi& api = detail::auApi();
  const HostedParam& p = params_[(size_t) index];
  // An indexed parameter's own names first: what its menu says.
  const std::vector<std::string>& names = valueStrings_[(size_t) index];
  if (!names.empty()) {
    const long k = (long) (value - p.minValue + 0.5);
    if (k >= 0 && (size_t) k < names.size()) return names[(size_t) k];
  }
  // Then the unit's text for the value, which AU hosts show.
  if (unit_) {
    AudioUnitParameterValue v = (AudioUnitParameterValue) value;
    AudioUnitParameterStringFromValue request;
    std::memset(&request, 0, sizeof(request));
    request.inParamID = p.id;
    request.inValue = &v;
    UInt32 size = sizeof(request);
    if (api.getProperty(unit_, kAudioUnitProperty_ParameterStringFromValue, kAudioUnitScope_Global,
                        0, &request, &size) == noErr &&
        request.outString) {
      std::string text = detail::auCfText(api, request.outString);
      api.release(request.outString); // a created string, the caller's to release
      if (!text.empty()) return text;
    }
  }
  char buffer[64];
  const char* label = detail::auUnitLabel(paramUnits_[(size_t) index]);
  std::snprintf(buffer, sizeof(buffer), label[0] ? "%g %s" : "%g", value, label);
  return std::string(buffer);
}

inline bool AuPlugin::saveState(std::vector<uint8_t>& out) const {
  out.clear();
  if (!unit_) return false;
  const detail::AuApi& api = detail::auApi();
  CFPropertyListRef plist = nullptr;
  OSStatus status = getValue(kAudioUnitProperty_ClassInfo, kAudioUnitScope_Global, 0, plist);
  if (status != noErr || !plist) {
    note("get kAudioUnitProperty_ClassInfo", status);
    return false;
  }
  CFErrorRef error = nullptr;
  CFDataRef data = api.plistCreateData(nullptr, plist, kCFPropertyListXMLFormat_v1_0, 0, &error);
  api.release(plist);
  if (!data) {
    if (error) api.release(error);
    lastError_ = "CFPropertyListCreateData could not write the unit's ClassInfo";
    return false;
  }
  const CFIndex length = api.dataGetLength(data);
  const UInt8* bytes = api.dataGetBytePtr(data);
  if (length > 0 && bytes) out.assign(bytes, bytes + length);
  api.release(data);
  return !out.empty();
}

inline bool AuPlugin::loadState(const uint8_t* data, size_t size) {
  if (!unit_ || !data || size == 0) return false;
  const detail::AuApi& api = detail::auApi();
  CFDataRef bytes = api.dataCreate(nullptr, data, (CFIndex) size);
  if (!bytes) {
    lastError_ = "CFDataCreate failed";
    return false;
  }
  CFErrorRef error = nullptr;
  CFPropertyListFormat format = kCFPropertyListXMLFormat_v1_0;
  CFPropertyListRef plist =
      api.plistCreateWithData(nullptr, bytes, kCFPropertyListImmutable, &format, &error);
  api.release(bytes);
  if (!plist) {
    if (error) api.release(error);
    lastError_ = "the state is not a property list (CFPropertyListCreateWithData refused it)";
    return false;
  }
  const OSStatus status = setValue(kAudioUnitProperty_ClassInfo, kAudioUnitScope_Global, 0, plist);
  api.release(plist);
  if (status != noErr) {
    note("set kAudioUnitProperty_ClassInfo", status);
    return false;
  }
  return true;
}

inline bool AuPlugin::loadPreset(int index) {
  if (!unit_ || index < 0 || index >= numPresets()) return false;
  const detail::AuApi& api = detail::auApi();
  AUPreset preset;
  preset.presetNumber = presets_[(size_t) index].number;
  // A real name, not null: a unit may retain whatever it is handed.
  preset.presetName = detail::auCfString(api, presets_[(size_t) index].name.c_str());
  const OSStatus status = setValue(kAudioUnitProperty_PresentPreset, kAudioUnitScope_Global, 0, preset);
  if (preset.presetName) api.release(preset.presetName);
  if (status != noErr) {
    note("set kAudioUnitProperty_PresentPreset", status);
    return false;
  }
  return true;
}

inline void AuPlugin::embed(id view, id parent) {
  detail::auSend<id>(view, "retain");
  detail::auSend<void>(parent, "addSubview:", view);
  // The top left of the parent, whichever way up it is: a flipped view's
  // origin is its top, an ordinary one's its bottom.
  const BOOL flipped = detail::auSend<BOOL>(parent, "isFlipped");
  const detail::AuRect bounds = detail::auSendRect(parent, "bounds");
  const detail::AuRect frame = detail::auSendRect(view, "frame");
  const detail::AuPoint origin{0.0, flipped ? 0.0 : bounds.size.height - frame.size.height};
  detail::auSend<void>(view, "setFrameOrigin:", origin);
  editorView_ = view;
}

inline bool AuPlugin::openCocoaView(id parent) {
  const detail::AuApi& api = detail::auApi();
  UInt32 size = 0;
  if (!hasProperty(kAudioUnitProperty_CocoaUI, kAudioUnitScope_Global, 0, &size) ||
      size < sizeof(AudioUnitCocoaViewInfo)) {
    lastError_ = "the unit no longer offers kAudioUnitProperty_CocoaUI";
    return false;
  }
  // A URL and then as many class names as the size has room for.
  std::vector<uint64_t> storage((size + sizeof(uint64_t) - 1) / sizeof(uint64_t), 0);
  auto* info = reinterpret_cast<AudioUnitCocoaViewInfo*>(storage.data());
  OSStatus status = api.getProperty(unit_, kAudioUnitProperty_CocoaUI, kAudioUnitScope_Global, 0, info,
                                    &size);
  if (status != noErr) {
    note("get kAudioUnitProperty_CocoaUI", status);
    return false;
  }
  const size_t classes = (size - sizeof(CFURLRef)) / sizeof(CFStringRef);
  // Every reference the property handed over is released, whatever happens
  // below.
  auto releaseInfo = [&]() {
    if (info->mCocoaAUViewBundleLocation) api.release(info->mCocoaAUViewBundleLocation);
    for (size_t k = 0; k < classes; ++k)
      if (info->mCocoaAUViewClass[k]) api.release(info->mCocoaAUViewClass[k]);
  };
  char path[4096] = {0};
  if (!info->mCocoaAUViewBundleLocation || classes == 0 ||
      !api.urlGetPath(info->mCocoaAUViewBundleLocation, true, (UInt8*) path, (CFIndex) sizeof(path))) {
    releaseInfo();
    lastError_ = "kAudioUnitProperty_CocoaUI named no bundle or no view class";
    return false;
  }
  id nsPath = detail::auSend<id>(detail::auClass("NSString"), "stringWithUTF8String:",
                                 (const char*) path);
  id bundle = nsPath ? detail::auSend<id>(detail::auClass("NSBundle"), "bundleWithPath:", nsPath)
                     : (id) nullptr;
  if (!bundle) {
    releaseInfo();
    lastError_ = std::string("NSBundle could not open the view bundle ") + path;
    return false;
  }
  // classNamed: loads the bundle's code when it has not been loaded yet. A
  // CFString IS an NSString (toll-free bridged), so the name goes as it is.
  Class viewClass = detail::auSend<Class>(bundle, "classNamed:", (id) info->mCocoaAUViewClass[0]);
  const std::string className = detail::auCfText(api, info->mCocoaAUViewClass[0]);
  releaseInfo();
  if (!viewClass) {
    lastError_ = "the view bundle has no class " + className;
    return false;
  }
  // AUCocoaUIBase, checked by the one method it requires.
  const SEL make = api.selector("uiViewForAudioUnit:withSize:");
  if (!detail::auSend<BOOL>((id) viewClass, "instancesRespondToSelector:", make)) {
    lastError_ = className + " does not implement uiViewForAudioUnit:withSize:";
    return false;
  }
  id factory = detail::auSend<id>((id) viewClass, "alloc");
  factory = factory ? detail::auSend<id>(factory, "init") : (id) nullptr;
  if (!factory) {
    lastError_ = className + " would not initialise";
    return false;
  }
  // The size is a preference the view is free to ignore; 400 x 300 is what
  // hosts conventionally offer when they have no better idea.
  id view = detail::auSend<id>(factory, "uiViewForAudioUnit:withSize:", (void*) unit_,
                               detail::AuSize{400.0, 300.0});
  detail::auSend<void>(factory, "release");
  if (!view) {
    lastError_ = className + " returned no view";
    return false;
  }
  embed(view, parent);
  return true;
}

inline bool AuPlugin::openViewController(id parent) {
  const detail::AuApi& api = detail::auApi();
  auto* request = new detail::AuViewRequest();
  if (!::sonore::detail::makeGlobalBlock(request->block, &detail::AuViewRequest::completed, request)) {
    delete request;
    lastError_ = "no block runtime (_NSConcreteGlobalBlock) to receive the view controller";
    return false;
  }
  void* block = &request->block;
  const OSStatus status = api.setProperty(
      unit_, (AudioUnitPropertyID) detail::aucode::kPropertyRequestViewController,
      kAudioUnitScope_Global, 0, &block, sizeof(block));
  if (status != noErr) {
    // Refused. Whether the unit still calls the block is not promised either
    // way, so the request is abandoned the way a timeout abandons it: a late
    // completion then releases what it brought and its own reference, and
    // one that never comes leaves this small object behind.
    int expected = detail::AuViewRequest::kWaiting;
    if (request->state.compare_exchange_strong(expected, detail::AuViewRequest::kAbandoned)) {
      request->drop();
    } else {
      if (request->viewController) detail::auSend<void>(request->viewController, "release");
      request->drop();
    }
    note("set kAudioUnitProperty_RequestViewController", status);
    return false;
  }
  if (!detail::auWaitWhile(request->state, detail::AuViewRequest::kWaiting, 5.0)) {
    int expected = detail::AuViewRequest::kWaiting;
    if (request->state.compare_exchange_strong(expected, detail::AuViewRequest::kAbandoned)) {
      request->drop();
      lastError_ = "the AUv3 view controller did not arrive within 5 s";
      return false;
    }
  }
  id controller = request->viewController;
  request->drop();
  if (!controller) {
    lastError_ = "the unit answered the view request with no view controller";
    return false;
  }
  id view = detail::auSend<id>(controller, "view");
  if (!view) {
    detail::auSend<void>(controller, "release");
    lastError_ = "the AUv3 view controller has no view";
    return false;
  }
  editorController_ = controller; // retained in the completion
  embed(view, parent);
  return true;
}

inline bool AuPlugin::openEditor(void* parent) {
  if (!unit_ || !parent || editorView_) return false;
  if (!detail::auApi().editorOk) {
    lastError_ = detail::auApi().error;
    return false;
  }
  // The Cocoa view first: an AUv2's, and what most AUv3s also answer through
  // the v2 bridge. The view controller is the AUv3 way and the fallback.
  if (cocoaView_ && openCocoaView((id) parent)) return true;
  if (viewController_ && openViewController((id) parent)) return true;
  if (!cocoaView_ && !viewController_) lastError_ = "the unit has no editor of its own";
  return false;
}

inline void AuPlugin::closeEditor() {
  if (editorView_) {
    detail::auSend<void>(editorView_, "removeFromSuperview");
    detail::auSend<void>(editorView_, "release");
    editorView_ = nullptr;
  }
  if (editorController_) {
    detail::auSend<void>(editorController_, "release");
    editorController_ = nullptr;
  }
}

/** Instantiate an Audio Unit from its description. See host.h's loadPlugin,
 *  which calls this for format "AU". */
inline std::unique_ptr<HostedPlugin> loadAudioUnit(const PluginDescription& description,
                                                   std::string* error) {
  auto fail = [error](std::string why) -> std::unique_ptr<HostedPlugin> {
    if (error) *error = std::move(why);
    return nullptr;
  };
  const detail::AuApi& api = detail::auApi();
  if (!api.ok) return fail(api.error);
  uint32_t type = 0, subtype = 0, manufacturer = 0;
  if (!detail::auParseId(description.id, &type, &subtype, &manufacturer))
    return fail("'" + description.id + "' is not an Audio Unit id (type:subtype:manufacturer)");

  AudioComponentDescription wanted;
  std::memset(&wanted, 0, sizeof(wanted));
  wanted.componentType = type;
  wanted.componentSubType = subtype;
  wanted.componentManufacturer = manufacturer;
  AudioComponent component = api.findNext(nullptr, &wanted);
  if (!component)
    return fail("no Audio Unit is registered as " + description.id +
                ": not installed, or not registered yet (a new .component is seen after the "
                "next login, or once AudioComponentRegistrar is restarted)");
  AudioComponentDescription actual;
  std::memset(&actual, 0, sizeof(actual));
  OSStatus status = api.getDescription(component, &actual);
  if (status != noErr) return fail("AudioComponentGetDescription failed: " + detail::auStatusText(status));

  std::unique_ptr<AuPlugin> plugin(new AuPlugin());
  plugin->desc_ = description;
  plugin->component_ = actual;

  AudioComponentInstance instance = nullptr;
  const bool async = (actual.componentFlags & (detail::aucode::kFlagIsV3AudioUnit |
                                               detail::aucode::kFlagRequiresAsyncInstantiation)) != 0;
  if (async) {
    // An AUv3 -- usually out of process -- or a unit that says it must be
    // made asynchronously. AudioComponentInstanceNew on one of those can
    // deadlock the main thread, which is where a host loads plugins.
    if (!api.instantiate)
      return fail("this unit must be created with AudioComponentInstantiate, which this macOS lacks");
    auto* request = new detail::AuInstantiateRequest();
    if (!::sonore::detail::makeGlobalBlock(request->block, &detail::AuInstantiateRequest::completed,
                                           request)) {
      delete request;
      return fail("no block runtime (_NSConcreteGlobalBlock) for AudioComponentInstantiate");
    }
    api.instantiate(component, 0, &request->block);
    if (!detail::auWaitWhile(request->state, detail::AuInstantiateRequest::kWaiting, 15.0)) {
      int expected = detail::AuInstantiateRequest::kWaiting;
      if (request->state.compare_exchange_strong(expected, detail::AuInstantiateRequest::kAbandoned)) {
        request->drop();
        return fail("AudioComponentInstantiate did not complete within 15 s");
      }
    }
    instance = request->instance;
    status = request->status;
    request->drop();
  } else {
    status = api.instanceNew(component, &instance);
  }
  if (status != noErr || !instance) {
    if (instance) api.instanceDispose(instance);
    return fail(std::string(async ? "AudioComponentInstantiate" : "AudioComponentInstanceNew") +
                " failed: " + detail::auStatusText(status));
  }
  plugin->unit_ = instance;
  plugin->readCapabilities();
  plugin->listen();
  if (error) *error = plugin->lastError(); // anything that was noted on the way, or empty
  return std::unique_ptr<HostedPlugin>(plugin.release());
}

/** The units a .component bundle declares, from its Info.plist's
 *  AudioComponents list -- the bundle's code is not run. A unit the
 *  registry does not know (yet) is listed with the feature word
 *  "unregistered", which is why loading it would fail. */
inline std::vector<PluginDescription> describeAudioUnitBundle(const std::string& path,
                                                              std::string* error) {
  std::vector<PluginDescription> found;
  const detail::AuApi& api = detail::auApi();
  if (!api.ok) {
    if (error) *error = api.error;
    return found;
  }
  CFURLRef url = api.urlCreateFromPath(nullptr, (const UInt8*) path.c_str(), (CFIndex) path.size(), true);
  CFBundleRef bundle = url ? api.bundleCreate(nullptr, url) : nullptr;
  if (url) api.release(url);
  if (!bundle) {
    if (error) *error = "not a bundle: " + path;
    return found;
  }
  CFStringRef key = detail::auCfString(api, "AudioComponents");
  CFTypeRef list = key ? api.bundleInfoValue(bundle, key) : nullptr; // the Get rule: not ours
  if (key) api.release(key);
  if (!list || api.getTypeID(list) != api.arrayTypeID()) {
    api.release(bundle);
    if (error) *error = path + " declares no AudioComponents";
    return found;
  }
  CFStringRef keys[6] = {detail::auCfString(api, "type"), detail::auCfString(api, "subtype"),
                         detail::auCfString(api, "manufacturer"), detail::auCfString(api, "name"),
                         detail::auCfString(api, "version"), nullptr};
  const CFIndex count = api.arrayGetCount((CFArrayRef) list);
  for (CFIndex i = 0; i < count; ++i) {
    CFTypeRef entry = api.arrayGetValueAtIndex((CFArrayRef) list, i);
    if (!entry || api.getTypeID(entry) != api.dictionaryTypeID()) continue;
    auto text = [&](int k) {
      return keys[k] ? detail::auCfText(api, api.dictionaryGetValue((CFDictionaryRef) entry, keys[k]))
                     : std::string();
    };
    uint32_t codes[3] = {0, 0, 0};
    bool good = true;
    for (int k = 0; k < 3; ++k) {
      const std::string code = text(k);
      if (code.size() != 4) {
        good = false;
        break;
      }
      codes[k] = detail::aucode::fourCC(code[0], code[1], code[2], code[3]);
    }
    if (!good) {
      if (error) *error = path + ": an AudioComponents entry without a four-character type, subtype "
                                 "and manufacturer";
      continue;
    }
    PluginDescription d;
    d.path = path;
    d.format = "AU";
    d.id = detail::auIdFor(codes[0], codes[1], codes[2]);
    detail::auSplitName(text(3), &d.vendor, &d.name);
    if (d.name.empty()) d.name = detail::auCodeText(codes[1]);
    if (d.vendor.empty()) d.vendor = detail::auCodeText(codes[2]);
    const void* version = keys[4] ? api.dictionaryGetValue((CFDictionaryRef) entry, keys[4]) : nullptr;
    SInt64 v = 0;
    if (version && api.getTypeID(version) == api.numberTypeID() &&
        api.numberGetValue((CFNumberRef) version, kCFNumberSInt64Type, &v))
      d.version = detail::auVersionText((uint32_t) v);
    d.features = detail::auTypeWord(codes[0]);
    d.isInstrument = codes[0] == detail::aucode::kTypeMusicDevice;
    AudioComponentDescription wanted;
    std::memset(&wanted, 0, sizeof(wanted));
    wanted.componentType = codes[0];
    wanted.componentSubType = codes[1];
    wanted.componentManufacturer = codes[2];
    if (!api.findNext(nullptr, &wanted)) d.features += ";unregistered";
    found.push_back(d);
  }
  for (CFStringRef k : keys)
    if (k) api.release(k);
  api.release(bundle);
  return found;
}

#else // not macOS

inline std::unique_ptr<HostedPlugin> loadAudioUnit(const PluginDescription& description,
                                                   std::string* error) {
  if (error)
    *error = "'" + description.id + "' is an Audio Unit, and Audio Units are a macOS component "
             "format: this build is for another platform";
  return nullptr;
}

inline std::vector<PluginDescription> describeAudioUnitBundle(const std::string& path,
                                                              std::string* error) {
  if (error) *error = path + " is an Audio Unit bundle, which only macOS can load";
  return std::vector<PluginDescription>();
}

#endif // __APPLE__

/**
 * The Audio Unit format, as one object to ask.
 *
 * findAllComponents() is the scan: the system registry, by type -- effects,
 * instruments, music effects, MIDI processors and generators -- which finds
 * .component bundles, Apple's own units and AUv3 app extensions alike. A
 * description it returns has an empty path, because an AU is identified by
 * its id alone. createInstance() is loadPlugin() for one; describeBundle()
 * reads one .component without running it.
 *
 * Off macOS every call answers empty or null and says why: Audio Units are a
 * macOS component format and there is nothing to find.
 */
class AudioUnitPluginFormat {
public:
  /** "AU": the PluginDescription::format of every unit found here. */
  static const char* formatName() { return "AU"; }

  /** False, with the reason, where there is no AU runtime: off macOS, or a
   *  Mac where AudioToolbox could not be found. */
  static bool isAvailable(std::string* why = nullptr) {
#if defined(__APPLE__)
    const detail::AuApi& api = detail::auApi();
    if (!api.ok && why) *why = api.error;
    return api.ok;
#else
    if (why) *why = "Audio Units are a macOS component format; this build is for another platform";
    return false;
#endif
  }

  /** [main-thread] Every unit the system registry lists of the five types
   *  above, described without being opened. `error`, when given, gets the
   *  last failure; the units that could be described are still returned.
   *  Empty off macOS. */
  static std::vector<PluginDescription> findAllComponents(std::string* error = nullptr) {
    std::vector<PluginDescription> found;
#if defined(__APPLE__)
    const detail::AuApi& api = detail::auApi();
    if (!api.ok) {
      if (error) *error = api.error;
      return found;
    }
    const uint32_t types[] = {detail::aucode::kTypeEffect, detail::aucode::kTypeMusicDevice,
                              detail::aucode::kTypeMusicEffect, detail::aucode::kTypeMidiProcessor,
                              detail::aucode::kTypeGenerator};
    for (uint32_t type : types) {
      AudioComponentDescription wanted;
      std::memset(&wanted, 0, sizeof(wanted));
      wanted.componentType = type;
      for (AudioComponent c = api.findNext(nullptr, &wanted); c; c = api.findNext(c, &wanted)) {
        AudioComponentDescription actual;
        std::memset(&actual, 0, sizeof(actual));
        const OSStatus status = api.getDescription(c, &actual);
        if (status != noErr) {
          if (error) *error = "AudioComponentGetDescription failed: " + detail::auStatusText(status);
          continue;
        }
        found.push_back(detail::auDescribeComponent(api, c, actual));
      }
    }
#else
    (void) isAvailable(error);
#endif
    return found;
  }

  /** describeAudioUnitBundle(): one .component's units, from its Info.plist. */
  static std::vector<PluginDescription> describeBundle(const std::string& componentPath,
                                                       std::string* error = nullptr) {
    return describeAudioUnitBundle(componentPath, error);
  }

  /** loadAudioUnit(): an instance of what findAllComponents() described, or
   *  null with the reason in `error`. */
  static std::unique_ptr<HostedPlugin> createInstance(const PluginDescription& description,
                                                      std::string* error = nullptr) {
    return loadAudioUnit(description, error);
  }

  /** The reference's requiresUnblockedMessageThreadDuringCreation: an AUv3,
   *  or a unit that must be created asynchronously, whose creation runs the
   *  main run loop while it waits (see loadAudioUnit). */
  static bool createsAsynchronously(const PluginDescription& description) {
#if defined(__APPLE__)
    const detail::AuApi& api = detail::auApi();
    uint32_t type = 0, subtype = 0, manufacturer = 0;
    if (!api.ok || !detail::auParseId(description.id, &type, &subtype, &manufacturer)) return false;
    AudioComponentDescription wanted;
    std::memset(&wanted, 0, sizeof(wanted));
    wanted.componentType = type;
    wanted.componentSubType = subtype;
    wanted.componentManufacturer = manufacturer;
    AudioComponent c = api.findNext(nullptr, &wanted);
    AudioComponentDescription actual;
    std::memset(&actual, 0, sizeof(actual));
    if (!c || api.getDescription(c, &actual) != noErr) return false;
    return (actual.componentFlags & (detail::aucode::kFlagIsV3AudioUnit |
                                     detail::aucode::kFlagRequiresAsyncInstantiation)) != 0;
#else
    (void) description;
    return false;
#endif
  }

  /** Where .component bundles are installed: the machine's folder and the
   *  user's. Empty off macOS. */
  static std::vector<std::string> defaultSearchPaths() {
    std::vector<std::string> paths;
#if defined(__APPLE__)
    paths.push_back("/Library/Audio/Plug-Ins/Components");
    if (const char* home = std::getenv("HOME"))
      paths.push_back(std::string(home) + "/Library/Audio/Plug-Ins/Components");
#endif
    return paths;
  }
};

} // namespace host
} // namespace sonore
