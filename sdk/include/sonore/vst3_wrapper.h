// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: the VST3 format wrapper.
//
// Written against Steinberg's C ABI (`vst3_c_api.h`, BSD-3-Clause) rather than
// the C++ SDK. That choice matters: the C header is one file under a permissive
// licence, so everything below is OUR code and the plugin links no framework at
// all. The C++ SDK would drag in a build system, a class hierarchy, and the dual
// GPLv3/proprietary terms this SDK exists to avoid.
//
// Include it AFTER clap_wrapper.h in the same translation unit. The CLAP wrapper
// owns the machinery both formats share: the DSP instance, parameters, state,
// the webview bridge, and this file adapts it to VST3's shape, so one source
// file builds a .clap and a .vst3 that cannot drift apart.
//
// Three differences drive everything here:
//
//   1. VST3 parameters are NORMALISED 0..1; ours are plain values. Every
//      crossing converts, and getting one direction wrong is silent: the knob
//      simply lands somewhere other than where the user put it.
//   2. VST3 is COM-like: refcounted objects reached through queryInterface. One
//      object implements IComponent + IAudioProcessor + IEditController (a
//      "single component effect"), which is what modern hosts expect and which
//      halves the state that would otherwise need syncing.
//   3. UI edits must reach the host through IComponentHandler on the MAIN
//      thread: unlike CLAP, where the plugin emits them from the audio thread.
#pragma once

#include <vst3_c_api.h>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>

#include "audio.h"
#include "gui.h"
#include "plugin.h"
#include "presets.h"

#ifndef SONORE_NUM_PARAMS
#error "Define SONORE_NUM_PARAMS (and struct SonoreDsp) before including vst3_wrapper.h"
#endif

namespace sonore {
namespace vst3 {

using clapwrap::Instance;
// `kDesc` is the plugin's own descriptor at global scope: the same one the
// CLAP wrapper reads, found by ordinary lookup.

// ── Identity ─────────────────────────────────────────────────────────────────

/** VST3 identifies classes by a 16-byte UID. Ours is DERIVED from the plugin's
 *  reverse-URI id (FNV-1a, twice, with a salt), so it is stable across builds
 *  and unique per product without anyone generating and pasting a GUID: a step
 *  a generator would get wrong exactly once and then ship forever. */
inline void deriveUid(const char* id, const char* salt, Steinberg_TUID out) {
  uint64_t h1 = 0xcbf29ce484222325ULL;
  uint64_t h2 = 0x9e3779b97f4a7c15ULL;
  for (const char* p = id; p && *p; ++p) {
    h1 = (h1 ^ (uint64_t) (uint8_t) *p) * 0x100000001b3ULL;
    h2 = (h2 ^ (uint64_t) (uint8_t) *p) * 0x100000001b3ULL;
    h2 = (h2 << 7) | (h2 >> 57);
  }
  for (const char* p = salt; p && *p; ++p) h2 = (h2 ^ (uint64_t) (uint8_t) *p) * 0x100000001b3ULL;
  for (int i = 0; i < 8; ++i) {
    out[i] = (Steinberg_int8) ((h1 >> (56 - i * 8)) & 0xff);
    out[i + 8] = (Steinberg_int8) ((h2 >> (56 - i * 8)) & 0xff);
  }
}

inline const Steinberg_int8* componentUid() {
  static Steinberg_TUID uid;
  static bool ready = false;
  if (!ready) {
    deriveUid(kDesc.id, "component", uid);
    ready = true;
  }
  return uid;
}

inline bool sameUid(const Steinberg_TUID a, const Steinberg_TUID b) {
  return std::memcmp(a, b, sizeof(Steinberg_TUID)) == 0;
}

/** UTF-8 to VST3's String128 (UTF-16, 128 units including the terminator).
 *  The whole range: a code point past the BMP becomes a surrogate pair, and
 *  is dropped whole when only one unit of room is left -- half a pair is
 *  worse than a missing character. A malformed byte becomes '?' and the
 *  decoder moves on one byte, so garbage can never run past the terminator. */
inline void toString128(const char* utf8, Steinberg_Vst_String128 out) {
  int n = 0;
  const unsigned char* p = (const unsigned char*) (utf8 ? utf8 : "");
  const auto cont = [](unsigned char c) { return (c & 0xC0u) == 0x80u; };
  while (*p && n < 127) {
    uint32_t cp = *p;
    if (cp < 0x80) {
      ++p;
    } else if ((cp >> 5) == 0x6 && cont(p[1])) {
      cp = ((cp & 0x1fu) << 6) | (p[1] & 0x3fu);
      p += 2;
    } else if ((cp >> 4) == 0xe && cont(p[1]) && cont(p[2])) {
      cp = ((cp & 0x0fu) << 12) | ((p[1] & 0x3fu) << 6) | (p[2] & 0x3fu);
      p += 3;
    } else if ((cp >> 3) == 0x1e && cont(p[1]) && cont(p[2]) && cont(p[3])) {
      cp = ((cp & 0x07u) << 18) | ((p[1] & 0x3fu) << 12) | ((p[2] & 0x3fu) << 6) |
           (p[3] & 0x3fu);
      p += 4;
    } else {
      cp = '?';
      ++p;
    }
    if (cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) cp = '?';
    if (cp > 0xFFFFu) {
      if (n > 125) break; // no room for both halves of the pair
      cp -= 0x10000u;
      out[n++] = (Steinberg_char16) (0xD800u | (cp >> 10));
      out[n++] = (Steinberg_char16) (0xDC00u | (cp & 0x3FFu));
    } else {
      out[n++] = (Steinberg_char16) cp;
    }
  }
  out[n] = 0;
}

/** VST3 hands parameter text back as UTF-16. We only parse numbers out of it,
 *  so narrowing is the honest conversion rather than a lossy pretence. */
inline void fromString128(const Steinberg_Vst_TChar* in, char* out, size_t capacity) {
  size_t n = 0;
  for (; in && in[n] && n + 1 < capacity; ++n)
    out[n] = (char) (in[n] < 128 ? (char) in[n] : '?');
  out[n] = '\0';
}

/**
 * UTF-16 to UTF-8, properly, for the strings a USER typed.
 *
 * fromString128 above narrows anything non-ASCII to '?', which is honest for
 * the numbers it parses and wrong for a name: a track called "Bläser" would
 * reach the plugin as "Bl?ser" and be displayed that way in its UI. Surrogate
 * pairs are recombined rather than emitted as two lone halves, which is the
 * difference between an emoji in a track name and two replacement boxes.
 *
 * `maxUnits` bounds the read: VST3's String128 is not required to be
 * terminated when the text fills it exactly.
 */
inline std::string utf16ToUtf8(const Steinberg_Vst_TChar* in, int maxUnits) {
  std::string out;
  if (!in) return out;
  for (int i = 0; i < maxUnits && in[i]; ++i) {
    unsigned int cp = (unsigned int) (unsigned short) in[i];
    if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < maxUnits) {
      const unsigned int low = (unsigned int) (unsigned short) in[i + 1];
      if (low >= 0xDC00 && low <= 0xDFFF) {
        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
        ++i;
      }
    }
    if (cp < 0x80) {
      out.push_back((char) cp);
    } else if (cp < 0x800) {
      out.push_back((char) (0xC0 | (cp >> 6)));
      out.push_back((char) (0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      out.push_back((char) (0xE0 | (cp >> 12)));
      out.push_back((char) (0x80 | ((cp >> 6) & 0x3F)));
      out.push_back((char) (0x80 | (cp & 0x3F)));
    } else {
      out.push_back((char) (0xF0 | (cp >> 18)));
      out.push_back((char) (0x80 | ((cp >> 12) & 0x3F)));
      out.push_back((char) (0x80 | ((cp >> 6) & 0x3F)));
      out.push_back((char) (0x80 | (cp & 0x3F)));
    }
  }
  return out;
}

// ── Parameter normalisation ──────────────────────────────────────────────────
// The most error-prone crossing in this file, so both directions live together
// where they cannot drift apart.

inline double toNormalised(const ParamInfo& p, double plain) {
  // Forwarded, not copied. The curve lives in plugin.h because a plugin's own
  // knob and the host's automation lane must agree about where a value sits,
  // and two implementations of a power law do not stay agreed.
  return toNormalisedValue(p, plain);
}

inline double toPlain(const ParamInfo& p, double normalised) {
  double plain = toPlainValue(p, normalised);
  // A stepped control must land ON a step, or a host's automation lane reads
  // back a value the plugin can never actually produce.
  if (p.stepCount > 0) plain = (double) snapToStep(p, (float) plain);
  return plain;
}

/** VST3 spells host bypass as one extra automatable parameter carrying the
 *  kIsBypass flag, with id == SONORE_NUM_PARAMS so the real parameters keep
 *  index==id. Effects only: an instrument has no dry signal to pass, and
 *  hosts treat instrument bypass as their own mute. */
constexpr Steinberg_Vst_ParamID kBypassParamId = (Steinberg_Vst_ParamID) SONORE_NUM_PARAMS;

// ── Factory presets, which VST3 calls programs ───────────────────────────────
//
// A plugin's presets showed up in CLAP through preset discovery and in LV2 as
// pset:Preset, and in VST3 as nothing at all: the IUnitInfo program half was a
// row of stubs returning "none". A saturator shipping three presets offered
// none of them in Cubase or Reaper.
//
// VST3's model is a PROGRAM LIST attached to a unit, selected by writing to a
// parameter the plugin flags kIsProgramChange. So there are two halves: the
// list, which is metadata, and the parameter, which is how a host actually
// changes one.
constexpr Steinberg_Vst_ParamID kProgramParamId =
    (Steinberg_Vst_ParamID) SONORE_NUM_PARAMS + 1;
constexpr Steinberg_Vst_ProgramListID kProgramListId = 1;

inline bool hasPrograms() { return kDesc.presets != nullptr && kDesc.numPresets > 0; }

/** Normalised 0..1 to a preset index, ROUNDED rather than truncated: a host
 *  sending 0.999 for the last of three means the last one, and truncation
 *  would land on the middle. */
inline int programIndexFromNormalised(double normalised) {
  if (!hasPrograms()) return 0;
  const double steps = kDesc.numPresets > 1 ? (double) (kDesc.numPresets - 1) : 1.0;
  int index = (int) (normalised * steps + 0.5);
  if (index < 0) index = 0;
  if (index >= kDesc.numPresets) index = kDesc.numPresets - 1;
  return index;
}

// ── MIDI controllers, which VST3 does not deliver as MIDI ────────────────────
//
// This is the part of VST3 that surprises everyone who has written for any
// other format. Note on and note off arrive as events. Control change, pitch
// bend and aftertouch DO NOT. Steinberg's design routes them as ordinary
// PARAMETER changes, on ids the plugin publishes through IMidiMapping, and a
// plugin that does not implement that interface never sees a single one of
// them. No mod wheel, no sustain pedal, no pitch bend, no aftertouch.
//
// So these are hidden parameters, one per (channel, controller), translated
// back into MidiMessages before the DSP ever sees them. The DSP reads
// ctx.midi exactly as it does in CLAP and LV2, and never learns that this
// format took the long way round.
//
// All sixteen channels, because MPE is channel-per-note: collapsing them onto
// one id would make every expressive controller bend every voice at once,
// which is precisely the bug MPE exists to avoid.
constexpr Steinberg_Vst_ParamID kMidiCcParamBase = 0x10000000u;
constexpr int kMidiCcCount = 130; // 0..127 CC, 128 aftertouch, 129 pitch bend
constexpr int kMidiChannels = 16;

inline bool wantsMidiMapping() { return clapwrap::wantsMidiIn(); }
inline int midiCcParamCount() { return wantsMidiMapping() ? kMidiChannels * kMidiCcCount : 0; }
inline Steinberg_Vst_ParamID midiCcParamId(int channel, int cc) {
  return kMidiCcParamBase + (Steinberg_Vst_ParamID) (channel * kMidiCcCount + cc);
}
inline bool isMidiCcParam(Steinberg_Vst_ParamID id) {
  return wantsMidiMapping() && id >= kMidiCcParamBase &&
         id < kMidiCcParamBase + (Steinberg_Vst_ParamID) midiCcParamCount();
}
/** Pitch bend rests at CENTRE, not at zero: a host restoring defaults would
 *  otherwise bend every voice to the bottom of its range. */
inline double midiCcDefault(int cc) {
  return cc == Steinberg_Vst_ControllerNumbers_kPitchBend ? 0.5 : 0.0;
}
inline bool hasBypassParam() { return !kDesc.isInstrument; }

/** VST3 speaker arrangements are bitmasks; the CHANNEL COUNT is the popcount.
 *  Negotiation stores whatever mask the host proposed (and echoes it back in
 *  getBusArrangement) rather than second-guessing which 6-channel layout it
 *  meant -- the DSP only ever sees the count. */
inline uint32_t speakerCount(Steinberg_Vst_SpeakerArrangement arr) {
  uint32_t n = 0;
  while (arr) {
    n += (uint32_t) (arr & 1);
    arr >>= 1;
  }
  return n;
}

inline Steinberg_Vst_SpeakerArrangement defaultArrangementFor(uint32_t channels) {
  switch (channels) {
    case 1: return Steinberg_Vst_kSpeakerC;
    case 2: return Steinberg_Vst_kSpeakerL | Steinberg_Vst_kSpeakerR;
    default:
      // The industry-standard layout for this width, in the shared vocabulary:
      // 6 channels means 5.1 (with an LFE), not "the first six speaker bits".
      return (Steinberg_Vst_SpeakerArrangement) defaultChannelMask(channels);
  }
}


// ── Interface shims ──────────────────────────────────────────────────────────
// Each interface the host holds must be a distinct address whose FIRST member is
// the vtable pointer. Carrying an owner pointer beside it is how we get back to
// the object without `offsetof` on a type that is not standard-layout.

struct Plugin;

/** How many VST3 units the parameter table implies. Declared here because
 *  queryInterface consults it before the definition. */
inline int unitCount();

/** IUnitInfo: how VST3 spells a parameter hierarchy. Hosts read it to build
 *  the folders their generic editor shows. */
struct UnitInfoIface {
  Steinberg_Vst_IUnitInfoVtbl* lpVtbl = nullptr;
  Plugin* owner = nullptr;
};
struct MidiMappingIface {
  Steinberg_Vst_IMidiMappingVtbl* lpVtbl = nullptr;
  Plugin* owner = nullptr;
};
struct NoteExpressionIface {
  Steinberg_Vst_INoteExpressionControllerVtbl* lpVtbl = nullptr;
  Plugin* owner = nullptr;
};
struct ContextRequirementsIface {
  Steinberg_Vst_IProcessContextRequirementsVtbl* lpVtbl = nullptr;
  Plugin* owner = nullptr;
};
struct InfoListenerIface {
  Steinberg_Vst_ChannelContext_IInfoListenerVtbl* lpVtbl = nullptr;
  Plugin* owner = nullptr;
};
struct View;

struct ComponentIface {
  Steinberg_Vst_IComponentVtbl* lpVtbl = nullptr;
  Plugin* owner = nullptr;
};
struct ProcessorIface {
  Steinberg_Vst_IAudioProcessorVtbl* lpVtbl = nullptr;
  Plugin* owner = nullptr;
};
struct ControllerIface {
  Steinberg_Vst_IEditControllerVtbl* lpVtbl = nullptr;
  Plugin* owner = nullptr;
};
struct ViewIface {
  Steinberg_IPlugViewVtbl* lpVtbl = nullptr;
  View* owner = nullptr;
};
/** The scale interface is a SEPARATE object with its own vtable pointer,
 *  because a caller that queried for it will call through it as if it were an
 *  IPlugViewContentScaleSupport -- handing back the view itself would have it
 *  dispatch setContentScaleFactor into whatever IPlugView keeps at slot 3. */
struct ViewScaleIface {
  Steinberg_IPlugViewContentScaleSupportVtbl* lpVtbl = nullptr;
  View* owner = nullptr;
};

/**
 * The editor. Its lifetime is INDEPENDENT of the plugin's: a host may create a
 * view, close the window, and release it much later, so it holds a plain
 * pointer back and is told when the plugin goes away.
 */
struct View {
  ViewIface iface;
  ViewScaleIface scaleIface;
  Steinberg_uint32 refs = 1;
  Plugin* plugin = nullptr;
  Steinberg_IPlugFrame* frame = nullptr;
  /** LOGICAL size: what the page is laid out in, whatever the monitor does. */
  uint32_t width = SONORE_UI_WIDTH;
  uint32_t height = SONORE_UI_HEIGHT;
  /** Device pixels per logical pixel, as the host reported it. One until a
   *  host says otherwise, which covers every 100% display and every host that
   *  never asks. */
  float contentScale = 1.0f;
  bool attached = false;
};

/** Logical pixels to device pixels, rounded rather than truncated: at 150% a
 *  620-pixel editor is 930 and at 125% it is 775, and truncation loses a
 *  pixel off the edge on every odd multiple. */
inline uint32_t scaled(uint32_t logical, float factor) {
  const double v = (double) logical * (double) factor;
  return (uint32_t) (v + 0.5);
}

/**
 * One object, three interfaces.
 */
struct Plugin {
  ComponentIface component;
  ProcessorIface processor;
  ControllerIface controller;
  UnitInfoIface units;
  MidiMappingIface midiMapping;
  NoteExpressionIface noteExpression;
  ContextRequirementsIface contextRequirements;
  InfoListenerIface infoListener;

  /** Which factory preset the host is showing. Remembered rather than derived
   *  from the parameter values, which would guess wrong the moment a user
   *  turns one knob after loading a preset. */
  // The selected preset lives in the SHARED state, not here: it is saved with
  // everything else, and a copy in the VST3 half would be a second answer to
  // the same question that the state format does not know about.
  Steinberg_int32 currentProgram() const { return (Steinberg_int32) shared.selectedPreset; }
  void setCurrentProgram(Steinberg_int32 index) { shared.selectedPreset = (int32_t) index; }
  Steinberg_uint32 refs = 1;
  /** Set when the DSP reports its own state moved; cleared once the host has
   *  been told. Written on the audio thread and read on the main one, which
   *  for a single bool that only ever goes one way is the one case where that
   *  is not a race worth an atomic. */
  clapwrap::SharedFlag stateDirty; // set on the audio thread, cleared on the main one
  /** Last value the host set for each (channel, controller). Kept because a
   *  host reads parameters back for display and for its own automation lanes,
   *  and a parameter that forgets what it was told is a parameter that
   *  pluginval fails. Held here rather than in the shared DSP state: it is
   *  this format's private plumbing, not something the DSP or the other
   *  wrappers should carry. */
  struct MidiCcValues {
    float v[kMidiChannels][kMidiCcCount];
    MidiCcValues() {
      for (int c = 0; c < kMidiChannels; ++c)
        for (int n = 0; n < kMidiCcCount; ++n) v[c][n] = (float) midiCcDefault(n);
    }
  };
  MidiCcValues midiCc;

  /** Which note each noteId belongs to.
   *
   *  VST3 identifies a note expression event by its noteId ALONE: the event
   *  carries no key and no channel. The DSP correlates expression with its
   *  voices by key plus channel, because that is the one pairing every format
   *  provides, so this wrapper has to remember the association itself.
   *
   *  Leaving it out did not look like a bug: the fields simply stayed at their
   *  defaults and every expression event addressed key 0 on channel 0, so
   *  nothing matched and nothing moved. It went unseen because until
   *  INoteExpressionController existed no host would send one.
   *
   *  Fixed size and no allocation: this is touched from process(). */
  struct NoteIdMap {
    static constexpr int kSize = 128;
    Steinberg_int32 id[kSize];
    Steinberg_int16 key[kSize];
    Steinberg_int16 channel[kSize];
    NoteIdMap() {
      for (int i = 0; i < kSize; ++i) { id[i] = -1; key[i] = 0; channel[i] = 0; }
    }
    void add(Steinberg_int32 nid, Steinberg_int16 k, Steinberg_int16 ch) {
      if (nid < 0) return; // a host that assigns no id cannot express per-note
      int freeSlot = -1;
      for (int i = 0; i < kSize; ++i) {
        if (id[i] == nid) { key[i] = k; channel[i] = ch; return; }
        if (id[i] < 0 && freeSlot < 0) freeSlot = i;
      }
      // Out of slots means 128 notes are sounding at once. Dropping the OLDEST
      // rather than refusing the newest matches how voice stealing behaves:
      // the note the player just pressed is the one they are listening to.
      const int slot = freeSlot >= 0 ? freeSlot : 0;
      id[slot] = nid;
      key[slot] = k;
      channel[slot] = ch;
    }
    void remove(Steinberg_int32 nid) {
      if (nid < 0) return;
      for (int i = 0; i < kSize; ++i)
        if (id[i] == nid) { id[i] = -1; return; }
    }
    bool find(Steinberg_int32 nid, Steinberg_int16* k, Steinberg_int16* ch) const {
      if (nid < 0) return false;
      for (int i = 0; i < kSize; ++i)
        if (id[i] == nid) { *k = key[i]; *ch = channel[i]; return true; }
      return false;
    }
  };
  NoteIdMap noteIds;
  Steinberg_Vst_IComponentHandler* handler = nullptr;
  View* view = nullptr;

  /** Everything the formats share: DSP, parameters, meters, the UI queue. */
  Instance shared;

  double sampleRate = 48000.0;
  int32_t maxBlock = 512;
  bool active = false;
  /** Negotiated main-bus arrangement; sidechain stays mono/stereo. */
  Steinberg_Vst_SpeakerArrangement mainArr = 0;
  Steinberg_Vst_SpeakerArrangement scArr = 0;
};

void destroyPlugin(Plugin* self);

// ── FUnknown, shared by all three interfaces ─────────────────────────────────

inline Steinberg_tresult queryPlugin(Plugin* self, const Steinberg_TUID iid, void** obj) {
  if (!obj) return Steinberg_kInvalidArgument;
  *obj = nullptr;
  if (sameUid(iid, Steinberg_FUnknown_iid) || sameUid(iid, Steinberg_IPluginBase_iid) ||
      sameUid(iid, Steinberg_Vst_IComponent_iid)) {
    *obj = &self->component;
  } else if (sameUid(iid, Steinberg_Vst_IAudioProcessor_iid)) {
    *obj = &self->processor;
  } else if (sameUid(iid, Steinberg_Vst_IEditController_iid)) {
    *obj = &self->controller;
  } else if (sameUid(iid, Steinberg_Vst_IProcessContextRequirements_iid)) {
    *obj = &self->contextRequirements;
  } else if (sameUid(iid, Steinberg_Vst_INoteExpressionController_iid) && kDesc.supportsMpe) {
    // Only a DSP that actually plays expressively. Declaring expression types
    // a plugin then ignores is how a host ends up routing a whole controller
    // into something that hears none of it.
    *obj = &self->noteExpression;
  } else if (sameUid(iid, Steinberg_Vst_ChannelContext_IInfoListener_iid) &&
             clapwrap::WantsTrackInfo<SonoreDsp>::value) {
    // Only a DSP that asked. A host gathers the track's name and colour to
    // hand over here; doing that for a plugin which drops it is work done for
    // nobody.
    *obj = &self->infoListener;
  } else if (sameUid(iid, Steinberg_Vst_IMidiMapping_iid) && wantsMidiMapping()) {
    // Only a plugin that takes MIDI. Offering this on an effect would have a
    // host publishing 2080 hidden parameters that nothing will ever read.
    *obj = &self->midiMapping;
  } else if (sameUid(iid, Steinberg_Vst_IUnitInfo_iid) && (unitCount() > 0 || hasPrograms())) {
    // Offered when there is a hierarchy to describe -- OR a program list,
    // which lives on a unit and is unreachable without this interface.
    //
    // That second clause was missing and it made the factory presets
    // invisible on exactly the plugins most likely to have them: the
    // saturator declares three presets and no parameter groups, so unitCount
    // was zero, IUnitInfo was refused, and the list nobody could reach may as
    // well not have existed.
    *obj = &self->units;
  } else {
    return Steinberg_kNoInterface;
  }
  ++self->refs;
  return Steinberg_kResultOk;
}

inline Steinberg_uint32 addRefPlugin(Plugin* self) { return ++self->refs; }

inline Steinberg_uint32 releasePlugin(Plugin* self) {
  const Steinberg_uint32 n = --self->refs;
  if (n == 0) destroyPlugin(self);
  return n;
}

#define SONORE_VST3_UNKNOWN(Shim, recover)                                                    \
  static Steinberg_tresult SMTG_STDMETHODCALLTYPE Shim##QueryInterface(                       \
      void* self, const Steinberg_TUID iid, void** obj) {                                     \
    return queryPlugin(recover(self), iid, obj);                                              \
  }                                                                                           \
  static Steinberg_uint32 SMTG_STDMETHODCALLTYPE Shim##AddRef(void* self) {                   \
    return addRefPlugin(recover(self));                                                       \
  }                                                                                           \
  static Steinberg_uint32 SMTG_STDMETHODCALLTYPE Shim##Release(void* self) {                  \
    return releasePlugin(recover(self));                                                      \
  }

/** Units: one per declared group, plus the mandatory root. */
inline int unitCount() { // NOLINT: forward-declared above
  const GroupTable groups = collectGroups(kDesc.params, kDesc.numParams);
  if (groups.count > 0) return groups.count + 1; // root plus one per group
  // A plugin with no groups still needs a ROOT unit when it has programs: a
  // program list belongs to a unit, and a host looks for it by asking a unit
  // which list it owns. Offering the list with no unit to hang it on is a
  // list nothing can reach -- which is what the saturator had for one build.
  return hasPrograms() ? 1 : 0;
}

inline Steinberg_Vst_UnitID unitIdFor(const ParamInfo& p) {
  const GroupTable groups = collectGroups(kDesc.params, kDesc.numParams);
  const int g = groups.indexOf(p.group);
  return g >= 0 ? (Steinberg_Vst_UnitID) (g + 1) : 0; // 0 is the root unit
}

inline Plugin* ownerOfUnits(void* p) { return ((UnitInfoIface*) p)->owner; }

inline Plugin* ownerOfComponent(void* p) { return ((ComponentIface*) p)->owner; }
inline Plugin* ownerOfProcessor(void* p) { return ((ProcessorIface*) p)->owner; }
inline Plugin* ownerOfController(void* p) { return ((ControllerIface*) p)->owner; }
inline Plugin* ownerOfMidiMapping(void* p) { return ((MidiMappingIface*) p)->owner; }
inline Plugin* ownerOfNoteExpression(void* p) { return ((NoteExpressionIface*) p)->owner; }
inline Plugin* ownerOfContextReq(void* p) { return ((ContextRequirementsIface*) p)->owner; }
inline Plugin* ownerOfInfoListener(void* p) { return ((InfoListenerIface*) p)->owner; }

SONORE_VST3_UNKNOWN(component, ownerOfComponent)
SONORE_VST3_UNKNOWN(processor, ownerOfProcessor)
SONORE_VST3_UNKNOWN(controller, ownerOfController)
SONORE_VST3_UNKNOWN(units, ownerOfUnits)
SONORE_VST3_UNKNOWN(midiMapping, ownerOfMidiMapping)
SONORE_VST3_UNKNOWN(noteExpression, ownerOfNoteExpression)
SONORE_VST3_UNKNOWN(contextReq, ownerOfContextReq)
SONORE_VST3_UNKNOWN(infoListener, ownerOfInfoListener)

#undef SONORE_VST3_UNKNOWN

// ── State, shared with the CLAP wrapper ──────────────────────────────────────
// The SAME versioned `SNRS` blob, so a session saved by the VST3 build loads in
// the CLAP build and vice versa: one product, one state format.

inline Steinberg_tresult writeState(Plugin* self, Steinberg_IBStream* stream) {
  if (!stream) return Steinberg_kInvalidArgument;
  // The FORMAT lives in clap_wrapper.h and is written once. This file used to
  // carry its own copy, and the two drifted: the shared one learned to reset
  // unknown parameters to their defaults, to read a v1 blob's bypass as off,
  // and to hand an empty bag to a DSP whose blob predates bags, and this one
  // learned none of it. The same session file restored differently depending
  // on which build of the same plugin opened it.
  return clapwrap::saveStateBody(&self->shared,
                                 [stream](const void* data, size_t size) -> size_t {
                                   Steinberg_int32 wrote = 0;
                                   if (stream->lpVtbl->write(stream, (void*) data,
                                                             (Steinberg_int32) size,
                                                             &wrote) != Steinberg_kResultOk)
                                     return 0;
                                   return wrote > 0 ? (size_t) wrote : 0;
                                 })
             ? Steinberg_kResultOk
             : Steinberg_kResultFalse;
}

inline Steinberg_tresult readState(Plugin* self, Steinberg_IBStream* stream) {
  if (!stream) return Steinberg_kInvalidArgument;
  return clapwrap::loadStateBody(&self->shared,
                                 [stream](void* data, size_t size) -> size_t {
                                   Steinberg_int32 got = 0;
                                   if (stream->lpVtbl->read(stream, data, (Steinberg_int32) size,
                                                            &got) != Steinberg_kResultOk)
                                     return 0;
                                   return got > 0 ? (size_t) got : 0;
                                 })
             ? Steinberg_kResultOk
             : Steinberg_kResultFalse;
}

// ── IComponent ───────────────────────────────────────────────────────────────

static Steinberg_tresult SMTG_STDMETHODCALLTYPE componentInitialize(
    void* self, Steinberg_FUnknown* context) {
  Plugin* plugin = ownerOfComponent(self);

  // VST3 makes you ask. The context handed to initialize is an FUnknown that
  // MAY be an IHostApplication, and that is the only place a host names
  // itself -- there is no version or vendor in it, unlike CLAP, so those stay
  // empty rather than being invented.
  //
  // The parameter used to be discarded entirely, which is why a plugin could
  // not tell Cubase from Reaper and had to apply every host workaround to
  // everybody.
  if (context) {
    Steinberg_Vst_IHostApplication* app = nullptr;
    if (context->lpVtbl->queryInterface(context, Steinberg_Vst_IHostApplication_iid,
                                        (void**) &app) == Steinberg_kResultOk &&
        app) {
      Steinberg_Vst_String128 wide{};
      if (app->lpVtbl->getName(app, wide) == Steinberg_kResultOk)
        plugin->shared.hostInfo.name = utf16ToUtf8(wide, 128);
      app->lpVtbl->release(app);
    }
  }
  clapwrap::sendHostInfo(plugin->shared.dsp, plugin->shared.hostInfo);

  plugin->shared.mainChannels = clapwrap::defaultMainChannels();
  plugin->mainArr = defaultArrangementFor(plugin->shared.mainChannels);
  plugin->shared.mainChannelMask = (uint64_t) plugin->mainArr;
  plugin->scArr = defaultArrangementFor(2);
  for (int i = 0; i < SONORE_NUM_PARAMS && i < kDesc.numParams; ++i)
    plugin->shared.params[i] = kDesc.params[i].defaultValue;
  return Steinberg_kResultOk;
}

static Steinberg_tresult SMTG_STDMETHODCALLTYPE componentTerminate(void*) {
  return Steinberg_kResultOk;
}

static Steinberg_tresult SMTG_STDMETHODCALLTYPE componentGetControllerClassId(
    void*, Steinberg_TUID classId) {
  // Same class: this is a single-component effect, and saying so is what tells
  // the host not to go looking for a separate controller object.
  std::memcpy(classId, componentUid(), sizeof(Steinberg_TUID));
  return Steinberg_kResultOk;
}

static Steinberg_tresult SMTG_STDMETHODCALLTYPE componentSetIoMode(void*, Steinberg_Vst_IoMode) {
  return Steinberg_kResultOk;
}

static Steinberg_int32 SMTG_STDMETHODCALLTYPE componentGetBusCount(void*,
                                                                   Steinberg_Vst_MediaType type,
                                                                   Steinberg_Vst_BusDirection dir) {
  if (type == Steinberg_Vst_MediaTypes_kAudio) {
    // A synth has no audio input: declaring one makes hosts feed it silence and
    // show a meaningless input meter.
    if (dir == Steinberg_Vst_BusDirections_kInput && kDesc.isInstrument) return 0;
    if (dir == Steinberg_Vst_BusDirections_kInput && clapwrap::TakesSidechain<SonoreDsp>::value)
      return 2;
    if (dir == Steinberg_Vst_BusDirections_kOutput)
      return 1 + (Steinberg_int32) clapwrap::numAuxOutputs();
    return 1;
  }
  if (type == Steinberg_Vst_MediaTypes_kEvent) {
    if (dir == Steinberg_Vst_BusDirections_kInput)
      return clapwrap::wantsMidiIn() ? 1 : 0;
    return kDesc.producesMidi ? 1 : 0;
  }
  return 0;
}

static Steinberg_tresult SMTG_STDMETHODCALLTYPE componentGetBusInfo(
    void* self, Steinberg_Vst_MediaType type, Steinberg_Vst_BusDirection dir,
    Steinberg_int32 index, Steinberg_Vst_BusInfo* bus) {
  if (!bus || index < 0) return Steinberg_kInvalidArgument;
  if (index >= componentGetBusCount(self, type, dir)) return Steinberg_kInvalidArgument;
  std::memset(bus, 0, sizeof(*bus));
  bus->mediaType = type;
  bus->direction = dir;
  const bool sidechain = type == Steinberg_Vst_MediaTypes_kAudio &&
                         dir == Steinberg_Vst_BusDirections_kInput && index == 1;
  const bool auxOut = type == Steinberg_Vst_MediaTypes_kAudio &&
                      dir == Steinberg_Vst_BusDirections_kOutput && index >= 1;
  // The sidechain is an AUX bus: hosts show it as a routable key input rather
  // than wiring the track's own signal into it.
  bus->busType =
      (sidechain || auxOut) ? Steinberg_Vst_BusTypes_kAux : Steinberg_Vst_BusTypes_kMain;
  bus->flags = 1; // kDefaultActive
  if (type == Steinberg_Vst_MediaTypes_kAudio) {
    Plugin* plugin = ownerOfComponent(self);
    const uint32_t width =
        sidechain  ? (plugin->scArr ? speakerCount(plugin->scArr) : 2)
        : auxOut   ? clapwrap::auxBusChannels((uint32_t) index - 1)
                   : (plugin->mainArr ? speakerCount(plugin->mainArr)
                                      : clapwrap::defaultMainChannels());
    bus->channelCount = (Steinberg_int32) width;
    toString128(sidechain ? "Sidechain"
                : auxOut  ? kDesc.auxOutputs[index - 1].name
                : (dir == Steinberg_Vst_BusDirections_kInput ? "Input" : "Output"),
                bus->name);
  } else {
    bus->channelCount = 1;
    toString128(dir == Steinberg_Vst_BusDirections_kInput ? "MIDI In" : "MIDI Out", bus->name);
  }
  return Steinberg_kResultOk;
}

static Steinberg_tresult SMTG_STDMETHODCALLTYPE componentGetRoutingInfo(
    void*, Steinberg_Vst_RoutingInfo*, Steinberg_Vst_RoutingInfo*) {
  return Steinberg_kResultFalse;
}

static Steinberg_tresult SMTG_STDMETHODCALLTYPE componentActivateBus(void*,
                                                                     Steinberg_Vst_MediaType,
                                                                     Steinberg_Vst_BusDirection,
                                                                     Steinberg_int32,
                                                                     Steinberg_TBool) {
  return Steinberg_kResultOk;
}

static Steinberg_tresult SMTG_STDMETHODCALLTYPE componentSetActive(void* self,
                                                                   Steinberg_TBool state) {
  Plugin* plugin = ownerOfComponent(self);
  plugin->active = state != 0;
  if (plugin->active) {
    ProcessSpec spec;
    spec.sampleRate = plugin->sampleRate;
    spec.maximumBlockSize = (uint32_t) plugin->maxBlock;
    spec.numChannels = plugin->shared.mainChannels;
    spec.offline = plugin->shared.offline; // set by setupProcessing, just above
    plugin->shared.dsp.prepare(spec); // the one place allocation is allowed
    plugin->shared.bypass.prepare(plugin->sampleRate, (uint32_t) plugin->maxBlock,
                                  clapwrap::dspLatency(plugin->shared.dsp),
                                  plugin->shared.mainChannels);
    if (clapwrap::TakesSidechain<SonoreDsp>::value) {
      const size_t n = (size_t) (plugin->maxBlock > 0 ? plugin->maxBlock : 1);
      plugin->shared.scSilence.assign(n, 0.0f);
      if (clapwrap::supportsDouble()) plugin->shared.scSilence64.assign(n, 0.0);
    }
  }
  return Steinberg_kResultOk;
}

static Steinberg_tresult SMTG_STDMETHODCALLTYPE componentSetState(void* self,
                                                                  Steinberg_IBStream* stream) {
  return readState(ownerOfComponent(self), stream);
}

static Steinberg_tresult SMTG_STDMETHODCALLTYPE componentGetState(void* self,
                                                                  Steinberg_IBStream* stream) {
  return writeState(ownerOfComponent(self), stream);
}

// ── IAudioProcessor ──────────────────────────────────────────────────────────

static Steinberg_tresult SMTG_STDMETHODCALLTYPE processorSetBusArrangements(
    void* self, Steinberg_Vst_SpeakerArrangement* inputs, Steinberg_int32 numIns,
    Steinberg_Vst_SpeakerArrangement* outputs, Steinberg_int32 numOuts) {
  Plugin* plugin = ownerOfProcessor(self);
  const Steinberg_int32 wantIns =
      kDesc.isInstrument ? 0 : (clapwrap::TakesSidechain<SonoreDsp>::value ? 2 : 1);
  const Steinberg_int32 wantOuts = 1 + (Steinberg_int32) clapwrap::numAuxOutputs();
  if (numOuts != wantOuts || !outputs) return Steinberg_kResultFalse;
  if (numIns != wantIns) return Steinberg_kResultFalse;
  // Each aux output keeps the width the descriptor declared for it.
  for (Steinberg_int32 i = 1; i < wantOuts; ++i)
    if (speakerCount(outputs[i]) != clapwrap::auxBusChannels((uint32_t) i - 1))
      return Steinberg_kResultFalse;
  const uint32_t outCount = speakerCount(outputs[0]);
  if (!clapwrap::channelCountAllowed(outCount)) return Steinberg_kResultFalse;
  // Effects are symmetric by contract: the main input must match the output.
  if (wantIns >= 1) {
    if (!inputs || inputs[0] != outputs[0]) return Steinberg_kResultFalse;
  }
  if (wantIns >= 2) {
    const uint32_t scCount = speakerCount(inputs[1]);
    if (scCount < 1 || scCount > 2) return Steinberg_kResultFalse;
    plugin->scArr = inputs[1];
  }
  plugin->mainArr = outputs[0];
  plugin->shared.mainChannels = outCount;
  // VST3's speaker bit positions match our role numbering value for value
  // (both descend from WAVE_FORMAT_EXTENSIBLE), so the arrangement IS the
  // role mask -- no lookup table, and no chance of the two drifting.
  plugin->shared.mainChannelMask = (uint64_t) outputs[0];
  return Steinberg_kResultOk;
}

static Steinberg_tresult SMTG_STDMETHODCALLTYPE processorGetBusArrangement(
    void* self, Steinberg_Vst_BusDirection dir, Steinberg_int32 index,
    Steinberg_Vst_SpeakerArrangement* arr) {
  if (!arr) return Steinberg_kInvalidArgument;
  Plugin* plugin = ownerOfProcessor(self);
  const Steinberg_Vst_SpeakerArrangement main =
      plugin->mainArr ? plugin->mainArr
                      : defaultArrangementFor(clapwrap::defaultMainChannels());
  if (dir == Steinberg_Vst_BusDirections_kOutput) {
    if (index == 0) {
      *arr = main;
      return Steinberg_kResultOk;
    }
    if ((uint32_t) index <= clapwrap::numAuxOutputs()) {
      *arr = defaultArrangementFor(clapwrap::auxBusChannels((uint32_t) index - 1));
      return Steinberg_kResultOk;
    }
  }
  if (dir == Steinberg_Vst_BusDirections_kInput && !kDesc.isInstrument) {
    if (index == 0) {
      *arr = main;
      return Steinberg_kResultOk;
    }
    if (index == 1 && clapwrap::TakesSidechain<SonoreDsp>::value) {
      *arr = plugin->scArr ? plugin->scArr : defaultArrangementFor(2);
      return Steinberg_kResultOk;
    }
  }
  return Steinberg_kInvalidArgument;
}

static Steinberg_tresult SMTG_STDMETHODCALLTYPE processorCanProcessSampleSize(
    void*, Steinberg_int32 symbolicSampleSize) {
  // 64-bit is accepted only by a DSP that genuinely speaks it; for a float-only
  // DSP we decline rather than silently downcasting the host's 64-bit path.
  if (symbolicSampleSize == Steinberg_Vst_SymbolicSampleSizes_kSample32)
    return Steinberg_kResultTrue;
  if (symbolicSampleSize == Steinberg_Vst_SymbolicSampleSizes_kSample64 &&
      clapwrap::supportsDouble())
    return Steinberg_kResultTrue;
  return Steinberg_kResultFalse;
}

static Steinberg_uint32 SMTG_STDMETHODCALLTYPE processorGetLatencySamples(void* self) {
  return clapwrap::dspLatency(ownerOfProcessor(self)->shared.dsp);
}

static Steinberg_tresult SMTG_STDMETHODCALLTYPE processorSetupProcessing(
    void* self, Steinberg_Vst_ProcessSetup* setup) {
  if (!setup) return Steinberg_kInvalidArgument;
  Plugin* plugin = ownerOfProcessor(self);
  plugin->sampleRate = setup->sampleRate;
  plugin->maxBlock = setup->maxSamplesPerBlock;
  // VST3 says which kind of render this is here rather than through an
  // extension. kPrefetch is disk streaming ahead of playback -- still driven
  // by a clock, so it counts as real time; only kOffline is a bounce.
  plugin->shared.offline = setup->processMode == Steinberg_Vst_ProcessModes_kOffline;
  plugin->shared.maxFrames = (uint32_t) (setup->maxSamplesPerBlock > 0
                                             ? setup->maxSamplesPerBlock
                                             : 128);
  return Steinberg_kResultOk;
}

static Steinberg_tresult SMTG_STDMETHODCALLTYPE processorSetProcessing(void*, Steinberg_TBool) {
  return Steinberg_kResultOk;
}

static Steinberg_uint32 SMTG_STDMETHODCALLTYPE processorGetTailSamples(void* self) {
  return clapwrap::dspTail(ownerOfProcessor(self)->shared.dsp);
}

/** Musical time, VST3's spelling of it. Unlike CLAP's fixed-point beats these
 *  are already doubles in quarter notes, so the conversion is only about which
 *  validity flags the host set. */
inline TransportInfo readTransport(const Steinberg_Vst_ProcessContext* ctx) {
  TransportInfo info;
  if (!ctx) return info;
  info.isPlaying = (ctx->state & Steinberg_Vst_ProcessContext_StatesAndFlags_kPlaying) != 0;
  info.isRecording = (ctx->state & Steinberg_Vst_ProcessContext_StatesAndFlags_kRecording) != 0;
  info.isLooping = (ctx->state & Steinberg_Vst_ProcessContext_StatesAndFlags_kCycleActive) != 0;
  if (ctx->state & Steinberg_Vst_ProcessContext_StatesAndFlags_kTempoValid) {
    info.tempo = ctx->tempo;
    info.hasTempo = true;
  }
  if (ctx->state & Steinberg_Vst_ProcessContext_StatesAndFlags_kProjectTimeMusicValid) {
    info.positionBeats = ctx->projectTimeMusic;
    info.hasBeats = true;
  }
  if (ctx->state & Steinberg_Vst_ProcessContext_StatesAndFlags_kBarPositionValid)
    info.barStartBeats = ctx->barPositionMusic;
  if (ctx->sampleRate > 0.0) {
    info.positionSeconds = (double) ctx->projectTimeSamples / ctx->sampleRate;
    info.hasSeconds = true;
  }
  if (ctx->state & Steinberg_Vst_ProcessContext_StatesAndFlags_kTimeSigValid) {
    info.timeSigNumerator = ctx->timeSigNumerator;
    info.timeSigDenominator = ctx->timeSigDenominator;
  }
  return info;
}

/** Apply what the user did to the interface. Values were already sent to the
 *  host from the main thread (see viewOnMessage), so here they only have to
 *  land on the DSP: this must not call back into the host. */
inline void drainUiEvents(Plugin* plugin) {
  UiEventQueue::Event e;
  while (plugin->shared.uiEvents.pop(&e)) {
    switch (e.kind) {
      case UiEventQueue::Event::Kind::ParamSet:
        if (e.index >= 0 && e.index < SONORE_NUM_PARAMS)
          plugin->shared.params[e.index] = clampToRange(kDesc.params[e.index], e.value);
        break;
      case UiEventQueue::Event::Kind::NoteOn: {
        const int vel = (int) e.value;
        plugin->shared.midi.addEvent(MidiMessage::noteOn(0, e.index, vel < 1 ? 1 : vel), 0);
        break;
      }
      case UiEventQueue::Event::Kind::NoteOff:
        plugin->shared.midi.addEvent(MidiMessage::noteOff(0, e.index), 0);
        break;
      case UiEventQueue::Event::Kind::GestureBegin:
      case UiEventQueue::Event::Kind::GestureEnd:
        break; // gestures already went to the host on the main thread
    }
  }
}

/**
 * Translate the DSP's MIDI-out back into VST3 events. Format-agnostic (it reads
 * shared.midiOut, not the audio buffer), so BOTH the float and the double
 * process paths call it -- the 64-bit path used to skip it entirely, so an
 * arpeggiator went mute the moment a host switched to 64-bit processing.
 *
 * Note on/off map exactly; anything else (CC, bend) has no VST3 event
 * equivalent in this direction and travels as parameters in this format, so
 * passing it would invent a channel the host does not have.
 */
static void emitVst3MidiOut(Plugin* plugin, struct Steinberg_Vst_ProcessData* data,
                            Steinberg_int32 frames) {
  if (!kDesc.producesMidi || !data->outputEvents) return;
  for (const auto& e : plugin->shared.midiOut) {
    Steinberg_Vst_Event ev{};
    ev.busIndex = 0;
    ev.sampleOffset = e.samplePosition < 0 ? 0 : e.samplePosition;
    if (ev.sampleOffset >= frames) ev.sampleOffset = frames - 1;
    ev.flags = Steinberg_Vst_Event_EventFlags_kIsLive;
    const MidiMessage& m = e.message;
    if (m.isNoteOn()) {
      ev.type = Steinberg_Vst_Event_EventTypes_kNoteOnEvent;
      ev.Steinberg_Vst_Event_noteOn.channel = (Steinberg_int16) (m.getChannel() - 1);
      ev.Steinberg_Vst_Event_noteOn.pitch = (Steinberg_int16) m.getNoteNumber();
      ev.Steinberg_Vst_Event_noteOn.velocity = m.getFloatVelocity();
      ev.Steinberg_Vst_Event_noteOn.noteId = -1;
    } else if (m.isNoteOff()) {
      ev.type = Steinberg_Vst_Event_EventTypes_kNoteOffEvent;
      ev.Steinberg_Vst_Event_noteOff.channel = (Steinberg_int16) (m.getChannel() - 1);
      ev.Steinberg_Vst_Event_noteOff.pitch = (Steinberg_int16) m.getNoteNumber();
      ev.Steinberg_Vst_Event_noteOff.velocity = m.getFloatVelocity();
      ev.Steinberg_Vst_Event_noteOff.noteId = -1;
    } else {
      continue;
    }
    data->outputEvents->lpVtbl->addEvent(data->outputEvents, &ev);
  }
  for (auto it = plugin->shared.midiOut.sysexBegin();
       it != plugin->shared.midiOut.sysexEnd(); ++it) {
    Steinberg_Vst_Event ev{};
    ev.busIndex = 0;
    ev.sampleOffset = it->samplePosition < 0 ? 0 : it->samplePosition;
    if (ev.sampleOffset >= frames) ev.sampleOffset = frames - 1;
    ev.flags = Steinberg_Vst_Event_EventFlags_kIsLive;
    ev.type = Steinberg_Vst_Event_EventTypes_kDataEvent;
    ev.Steinberg_Vst_Event_data.type = Steinberg_Vst_DataEvent_DataTypes_kMidiSysEx;
    ev.Steinberg_Vst_Event_data.size = it->length;
    ev.Steinberg_Vst_Event_data.bytes = plugin->shared.midiOut.sysexData(*it);
    data->outputEvents->lpVtbl->addEvent(data->outputEvents, &ev);
  }
}

static Steinberg_tresult SMTG_STDMETHODCALLTYPE processorProcess(
    void* self, Steinberg_Vst_ProcessData* data) {
  Plugin* plugin = ownerOfProcessor(self);
  if (!data) return Steinberg_kInvalidArgument;

  // Notes are cleared BEFORE the parameter loop, because in VST3 some of the
  // MIDI arrives as parameters: clearing after would throw away every
  // controller the host just sent. MidiBuffer::addEvent inserts by sample
  // offset, so the note events added further down still land in order.
  plugin->shared.midi.clear();
  plugin->shared.expression.clear();

  // Parameter automation. VST3 delivers a queue per parameter with points at
  // sample offsets; we take the LAST point of each, which is block-granular:
  // what the smoothers in the toolkit exist to make inaudible.
  if (data->inputParameterChanges) {
    auto* changes = data->inputParameterChanges;
    const Steinberg_int32 count = changes->lpVtbl->getParameterCount(changes);
    for (Steinberg_int32 i = 0; i < count; ++i) {
      auto* queue = changes->lpVtbl->getParameterData(changes, i);
      if (!queue) continue;
      const Steinberg_Vst_ParamID id = queue->lpVtbl->getParameterId(queue);
      const Steinberg_int32 points = queue->lpVtbl->getPointCount(queue);
      if (points <= 0) continue;
      Steinberg_int32 offset = 0;
      Steinberg_Vst_ParamValue value = 0.0;
      if (queue->lpVtbl->getPoint(queue, points - 1, &offset, &value) != Steinberg_kResultOk)
        continue;
      if (hasPrograms() && id == kProgramParamId) {
        const int index = programIndexFromNormalised(value);
        plugin->setCurrentProgram((Steinberg_int32) index);
        float presetValues[SONORE_NUM_PARAMS > 0 ? SONORE_NUM_PARAMS : 1];
        if (applyPreset(kDesc.presets[index], kDesc.params, kDesc.numParams, presetValues))
          for (int i = 0; i < kDesc.numParams; ++i) plugin->shared.params[i] = presetValues[i];
        continue;
      }
      if (isMidiCcParam(id)) {
        // Back into the MIDI the DSP expects. Block-granular like every other
        // parameter here: taking every point instead would let one sweeping
        // mod wheel fill a 64-event buffer and starve the notes out of it,
        // which is a worse failure than a controller that updates per block.
        const int n = (int) (id - kMidiCcParamBase);
        const int channel = n / kMidiCcCount, cc = n % kMidiCcCount;
        const double v = value < 0.0 ? 0.0 : (value > 1.0 ? 1.0 : value);
        plugin->midiCc.v[channel][cc] = (float) v;
        const int at = offset < 0 ? 0 : offset;
        if (cc == Steinberg_Vst_ControllerNumbers_kPitchBend)
          plugin->shared.midi.addEvent(MidiMessage::pitchBend(channel, (int) (v * 16383.0 + 0.5)),
                                       at);
        else if (cc == Steinberg_Vst_ControllerNumbers_kAfterTouch)
          plugin->shared.midi.addEvent(
              MidiMessage::channelPressure(channel, (int) (v * 127.0 + 0.5)), at);
        else
          plugin->shared.midi.addEvent(
              MidiMessage::controlChange(channel, cc, (int) (v * 127.0 + 0.5)), at);
        continue;
      }
      if (id > kBypassParamId) continue;
      if (id == kBypassParamId) {
        if (hasBypassParam()) plugin->shared.bypass.engaged = value >= 0.5;
      } else {
        plugin->shared.params[id] = (float) toPlain(kDesc.params[id], value);
      }
    }
  }

  // Notes. VST3 gives them their own event list, already sample-ordered.
  if (data->inputEvents) {
    auto* events = data->inputEvents;
    const Steinberg_int32 count = events->lpVtbl->getEventCount(events);
    for (Steinberg_int32 i = 0; i < count; ++i) {
      Steinberg_Vst_Event e{};
      if (events->lpVtbl->getEvent(events, i, &e) != Steinberg_kResultOk) continue;
      const int offset = e.sampleOffset < 0 ? 0 : e.sampleOffset;
      if (e.type == Steinberg_Vst_Event_EventTypes_kNoteOnEvent) {
        const auto& on = e.Steinberg_Vst_Event_noteOn;
        int vel = (int) (on.velocity * 127.0f + 0.5f);
        if (vel < 1) vel = 1;
        plugin->shared.midi.addEvent(
            MidiMessage::noteOn(on.channel < 0 ? 0 : on.channel, on.pitch, vel), offset);
        plugin->noteIds.add(on.noteId, on.pitch, on.channel);
      } else if (e.type == Steinberg_Vst_Event_EventTypes_kDataEvent &&
                 e.Steinberg_Vst_Event_data.type ==
                     Steinberg_Vst_DataEvent_DataTypes_kMidiSysEx) {
        // VST3 has no SysEx EVENT; it has a generic data event whose type
        // says what the bytes are. The pointer belongs to the host and is
        // valid only for this call, so addSysex copies.
        const auto& d = e.Steinberg_Vst_Event_data;
        if (d.bytes && d.size >= 2)
          plugin->shared.midi.addSysex(d.bytes, (size_t) d.size, offset);
      } else if (e.type == Steinberg_Vst_Event_EventTypes_kNoteOffEvent) {
        const auto& off = e.Steinberg_Vst_Event_noteOff;
        plugin->shared.midi.addEvent(
            MidiMessage::noteOff(off.channel < 0 ? 0 : off.channel, off.pitch), offset);
        plugin->noteIds.remove(off.noteId);
      } else if (kDesc.supportsMpe &&
                 e.type == Steinberg_Vst_Event_EventTypes_kNoteExpressionValueEvent) {
        // VST3's own per-note expression. Its ids and RANGES differ from
        // ours: tuning arrives as a normalised 0..1 around centre with a
        // ±120 semitone span, so it is converted rather than passed through.
        const auto& ne = e.Steinberg_Vst_Event_noteExpressionValue;
        NoteExpressionBuffer::Entry entry;
        entry.noteId = ne.noteId;
        entry.samplePosition = offset;
        // The key and channel this id refers to. Without them the DSP has
        // nothing to match against and the expression is silently discarded,
        // which is exactly what happened before this lookup existed.
        Steinberg_int16 exprKey = 0, exprChannel = 0;
        if (plugin->noteIds.find(ne.noteId, &exprKey, &exprChannel)) {
          entry.key = (int16_t) exprKey;
          entry.channel = (int16_t) exprChannel;
        } else {
          // An id we never saw a note-on for. Dropped rather than aimed at
          // whatever key 0 happens to be playing.
          continue;
        }
        switch (ne.typeId) {
          case Steinberg_Vst_NoteExpressionTypeIDs_kVolumeTypeID:
            entry.expression = kExprVolume;
            // TIMES FOUR, and this was missing.
            //
            // NoteExpression in audio.h documents kExprVolume as a linear gain
            // 0..4 with 1 at unity, which is CLAP's scale: the SDK's
            // expression enum matches CLAP's ids and ranges exactly, so every
            // other format has to convert INTO it. VST3's normalised volume
            // runs 0 = silence, 0.25 = 0 dB, 0.5 = +6 dB, 1 = +12 dB, which is
            // that same gain divided by four. Passing it through raw handed
            // the DSP 0.25 where the host meant unity: every note 12 dB down.
            //
            // It went unnoticed because nothing could send it. Without
            // INoteExpressionController, added below, a VST3 host has no way
            // to know this plugin accepts note expression at all.
            entry.value = (float) (ne.value * 4.0);
            break;
          case Steinberg_Vst_NoteExpressionTypeIDs_kPanTypeID:
            entry.expression = kExprPan;
            entry.value = (float) ne.value;
            break;
          case Steinberg_Vst_NoteExpressionTypeIDs_kTuningTypeID:
            entry.expression = kExprTuning;
            entry.value = (float) ((ne.value - 0.5) * 240.0);
            break;
          case Steinberg_Vst_NoteExpressionTypeIDs_kVibratoTypeID:
            entry.expression = kExprVibrato;
            entry.value = (float) ne.value;
            break;
          case Steinberg_Vst_NoteExpressionTypeIDs_kExpressionTypeID:
            entry.expression = kExprExpression;
            entry.value = (float) ne.value;
            break;
          case Steinberg_Vst_NoteExpressionTypeIDs_kBrightnessTypeID:
            entry.expression = kExprBrightness;
            entry.value = (float) ne.value;
            break;
          default:
            // VST3 has no standard PRESSURE id -- it carries pressure as poly
            // aftertouch, which reaches us as raw MIDI and is decoded there.
            // An id we do not model is dropped rather than guessed at.
            continue;
        }
        plugin->shared.expression.addEvent(entry);
      }
    }
  }

  if (kDesc.supportsMpe) plugin->shared.mpe.process(plugin->shared.midi, plugin->shared.expression);

  drainUiEvents(plugin);

  const Steinberg_int32 frames = data->numSamples;
  if (frames <= 0 || data->numOutputs < 1 || !data->outputs) return Steinberg_kResultOk;

  auto& out = data->outputs[0];

  // ── 64-bit path ───────────────────────────────────────────────────────────
  if constexpr (clapwrap::supportsDouble()) {
    if (data->symbolicSampleSize == Steinberg_Vst_SymbolicSampleSizes_kSample64 &&
        out.Steinberg_Vst_AudioBusBuffers_channelBuffers64 && out.numChannels >= 1) {
      uint32_t nch64 = plugin->shared.mainChannels;
      if ((uint32_t) out.numChannels < nch64) nch64 = (uint32_t) out.numChannels;
      if (nch64 > clapwrap::kMaxAudioChannels) nch64 = clapwrap::kMaxAudioChannels;
      double* chans64[clapwrap::kMaxAudioChannels];
      for (uint32_t c = 0; c < nch64; ++c)
        chans64[c] = out.Steinberg_Vst_AudioBusBuffers_channelBuffers64[c];

      if (kDesc.isInstrument) {
        for (uint32_t c = 0; c < nch64; ++c)
          std::memset(chans64[c], 0, sizeof(double) * frames);
      } else if (data->numInputs > 0 && data->inputs &&
                 data->inputs[0].Steinberg_Vst_AudioBusBuffers_channelBuffers64) {
        const auto& in = data->inputs[0];
        for (uint32_t c = 0; c < nch64; ++c) {
          const double* src = in.Steinberg_Vst_AudioBusBuffers_channelBuffers64
              [(Steinberg_int32) c < in.numChannels ? c : (uint32_t) in.numChannels - 1];
          if (src && src != chans64[c]) std::memcpy(chans64[c], src, sizeof(double) * frames);
        }
      }

      clapwrap::sendTransport(plugin->shared.dsp, readTransport(data->processContext));

      // Sidechain, aux outputs and MIDI-out -- the same wiring the float path
      // has, which the 64-bit path used to skip: a ducker's key input was
      // silently ignored, a splitter's aux buses were never written (and never
      // cleared, so the host played stale memory), and an arpeggiator went mute
      // the moment a host used 64-bit processing.
      // Whole-block silence when nothing is routed, like the float path: the
      // contract is "silence-filled", not "null with zero frames".
      double* zero64 =
          plugin->shared.scSilence64.empty() ? nullptr : plugin->shared.scSilence64.data();
      double* scChans64[2] = {zero64, zero64};
      if (data->numInputs > 1 && data->inputs &&
          data->inputs[1].Steinberg_Vst_AudioBusBuffers_channelBuffers64 &&
          data->inputs[1].numChannels > 0) {
        const auto& sc = data->inputs[1];
        scChans64[0] = sc.Steinberg_Vst_AudioBusBuffers_channelBuffers64[0];
        scChans64[1] = sc.Steinberg_Vst_AudioBusBuffers_channelBuffers64[sc.numChannels > 1 ? 1 : 0];
      }
      AudioBlock<double> scBlock64(scChans64, 2, scChans64[0] ? (size_t) frames : 0);

      AudioBlock<double> auxBlocks64[clapwrap::kMaxAuxOutputs] = {};
      double* auxPtrs64[clapwrap::kMaxAuxOutputs][clapwrap::kMaxAudioChannels] = {};
      const uint32_t nAux64 = clapwrap::numAuxOutputs();
      for (uint32_t b = 0; b < nAux64; ++b) {
        uint32_t width = 0;
        const Steinberg_int32 busIndex = (Steinberg_int32) (1 + b);
        if (busIndex < data->numOutputs && data->outputs) {
          const auto& ab = data->outputs[busIndex];
          if (ab.Steinberg_Vst_AudioBusBuffers_channelBuffers64) {
            width = (uint32_t) ab.numChannels;
            const uint32_t want = clapwrap::auxBusChannels(b);
            if (width > want) width = want;
            if (width > clapwrap::kMaxAudioChannels) width = clapwrap::kMaxAudioChannels;
            for (uint32_t c = 0; c < width; ++c)
              auxPtrs64[b][c] = ab.Steinberg_Vst_AudioBusBuffers_channelBuffers64[c];
          }
        }
        auxBlocks64[b] = AudioBlock<double>(auxPtrs64[b], width, width ? (size_t) frames : 0);
      }

      uint8_t roles64[clapwrap::kMaxAudioChannels];
      const uint32_t numRoles64 = rolesFromMask(plugin->shared.mainChannelMask, roles64,
                                                clapwrap::kMaxAudioChannels);
      AudioBlock<double> block64(chans64, nch64, (size_t) frames);
      plugin->shared.midiOut.clear();
      ProcessContextT<double> ctx64{block64,
                                    auxBlocks64,
                                    nAux64,
                                    scBlock64,
                                    plugin->shared.midi,
                                    plugin->shared.midiOut,
                                    numRoles64 ? roles64 : nullptr,
                                    &plugin->shared.expression,
                                    nullptr, // VST3 has no per-port activation
                                    true,
                                    plugin->shared.offline};
      clapwrap::bypassCapture(plugin->shared.bypass, chans64, (uint32_t) frames);
      trackNotes(plugin->shared.notes, ctx64.midi);
      clapwrap::snapshotParams(&plugin->shared);
      clapwrap::runDspCtx64(plugin->shared.dsp, ctx64, plugin->shared.paramsBlock);
      emitVst3MidiOut(plugin, data, frames);
      clapwrap::bypassApply(plugin->shared.bypass, chans64, (uint32_t) frames);

      float peak = 0.0f;
      double sum = 0.0;
      for (Steinberg_int32 i = 0; i < frames; ++i) {
        const double v = chans64[0][i];
        const float a = (float) (v < 0 ? -v : v);
        if (a > peak) peak = a;
        sum += v * v;
      }
      plugin->shared.meter.push(peak, (float) std::sqrt(sum / (double) frames));
      out.silenceFlags = 0;
      return Steinberg_kResultOk;
    }
  }

  if (!out.Steinberg_Vst_AudioBusBuffers_channelBuffers32 || out.numChannels < 1)
    return Steinberg_kResultOk;

  uint32_t nch = plugin->shared.mainChannels;
  if ((uint32_t) out.numChannels < nch) nch = (uint32_t) out.numChannels;
  if (nch > clapwrap::kMaxAudioChannels) nch = clapwrap::kMaxAudioChannels;
  float* chans[clapwrap::kMaxAudioChannels];
  for (uint32_t c = 0; c < nch; ++c)
    chans[c] = out.Steinberg_Vst_AudioBusBuffers_channelBuffers32[c];

  if (kDesc.isInstrument) {
    for (uint32_t c = 0; c < nch; ++c) std::memset(chans[c], 0, sizeof(float) * frames);
  } else if (data->numInputs > 0 && data->inputs &&
             data->inputs[0].Steinberg_Vst_AudioBusBuffers_channelBuffers32) {
    const auto& in = data->inputs[0];
    for (uint32_t c = 0; c < nch; ++c) {
      const float* src = in.Steinberg_Vst_AudioBusBuffers_channelBuffers32
          [(Steinberg_int32) c < in.numChannels ? c : (uint32_t) in.numChannels - 1];
      if (src && src != chans[c]) std::memcpy(chans[c], src, sizeof(float) * frames);
    }
  }

  clapwrap::sendTransport(plugin->shared.dsp, readTransport(data->processContext));

  float* zero = plugin->shared.scSilence.empty() ? nullptr : plugin->shared.scSilence.data();
  float* scChans[2] = {zero, zero};
  if (data->numInputs > 1 && data->inputs &&
      data->inputs[1].Steinberg_Vst_AudioBusBuffers_channelBuffers32 &&
      data->inputs[1].numChannels > 0) {
    const auto& sc = data->inputs[1];
    scChans[0] = sc.Steinberg_Vst_AudioBusBuffers_channelBuffers32[0];
    scChans[1] = sc.Steinberg_Vst_AudioBusBuffers_channelBuffers32[sc.numChannels > 1 ? 1 : 0];
  }
  AudioBlock<float> scBlock(scChans, 2, scChans[0] ? (size_t) frames : 0);

  // Aux output buses; a bus the host left unconnected arrives zero-width.
  AudioBlock<float> auxBlocks[clapwrap::kMaxAuxOutputs] = {};
  float* auxPtrs[clapwrap::kMaxAuxOutputs][clapwrap::kMaxAudioChannels] = {};
  const uint32_t nAux = clapwrap::numAuxOutputs();
  for (uint32_t b = 0; b < nAux; ++b) {
    uint32_t width = 0;
    const Steinberg_int32 busIndex = (Steinberg_int32) (1 + b);
    if (busIndex < data->numOutputs && data->outputs) {
      const auto& ab = data->outputs[busIndex];
      if (ab.Steinberg_Vst_AudioBusBuffers_channelBuffers32) {
        width = (uint32_t) ab.numChannels;
        const uint32_t want = clapwrap::auxBusChannels(b);
        if (width > want) width = want;
        if (width > clapwrap::kMaxAudioChannels) width = clapwrap::kMaxAudioChannels;
        for (uint32_t c = 0; c < width; ++c)
          auxPtrs[b][c] = ab.Steinberg_Vst_AudioBusBuffers_channelBuffers32[c];
      }
    }
    auxBlocks[b] = AudioBlock<float>(auxPtrs[b], width, width ? (size_t) frames : 0);
  }

  clapwrap::bypassCapture(plugin->shared.bypass, chans, (uint32_t) frames);
  AudioBlock<float> block(chans, nch, (size_t) frames);
  plugin->shared.midiOut.clear();
  uint8_t roles[clapwrap::kMaxAudioChannels];
  const uint32_t numRoles =
      rolesFromMask(plugin->shared.mainChannelMask, roles, clapwrap::kMaxAudioChannels);
  ProcessContext ctx{block,
                     auxBlocks,
                     nAux,
                     scBlock,
                     plugin->shared.midi,
                     plugin->shared.midiOut,
                     numRoles ? roles : nullptr,
                     &plugin->shared.expression,
                     nullptr, // VST3 has no per-port activation; every bus is live
                     true,
                     plugin->shared.offline};
  trackNotes(plugin->shared.notes, ctx.midi);
  clapwrap::snapshotParams(&plugin->shared);
  clapwrap::runDspCtx(plugin->shared.dsp, ctx, plugin->shared.paramsBlock);

  emitVst3MidiOut(plugin, data, frames);
  clapwrap::bypassApply(plugin->shared.bypass, chans, (uint32_t) frames);

  // Meters for the editor: two atomic stores, the same as every other format.
  {
    // Vectorised: one pass each for peak and energy. Measured 1.7x native
    // and 1.4x in WebAssembly over the hand-rolled loop, and the meters run
    // on every block whether or not an editor is open.
    const float peak = simd::peakAbs(chans[0], (size_t) frames);
    const double sum = simd::sumSquares(chans[0], (size_t) frames);
    plugin->shared.meter.push(peak, (float) std::sqrt(sum / (double) frames));
  }

  out.silenceFlags = 0;
  for (uint32_t b = 0; b < nAux; ++b) {
    const Steinberg_int32 busIndex = (Steinberg_int32) (1 + b);
    if (busIndex < data->numOutputs && data->outputs) data->outputs[busIndex].silenceFlags = 0;
  }
  // The DSP may have changed state the host has no other way to learn about.
  // Recorded, not announced: setDirty is a main-thread call and this is not,
  // and VST3 offers no way to ask to be called there. The parameter poll
  // delivers it, same as the latency.
  if (clapwrap::dspConsumeStateDirty(plugin->shared.dsp)) plugin->stateDirty = true;

  return Steinberg_kResultOk;
}

// ── IUnitInfo ────────────────────────────────────────────────────────────────
//
// Unit 0 is the root every VST3 host expects; each declared group becomes a
// child of it. The ROOT unit also owns the factory program list, where there
// is one -- that is where a host looks for presets, and answering "none" is
// what made them invisible in this format alone.

static Steinberg_int32 SMTG_STDMETHODCALLTYPE unitsGetUnitCount(void*) {
  return (Steinberg_int32) unitCount();
}

static Steinberg_tresult SMTG_STDMETHODCALLTYPE unitsGetUnitInfo(
    void*, Steinberg_int32 unitIndex, Steinberg_Vst_UnitInfo* info) {
  if (!info || unitIndex < 0 || unitIndex >= unitCount()) return Steinberg_kInvalidArgument;
  std::memset(info, 0, sizeof(*info));
  info->programListId = -1; // kNoProgramListId
  if (unitIndex == 0) {
    info->id = 0;
    info->parentUnitId = -1; // kNoParentUnitId
    toString128("Root", info->name);
    // The ROOT unit owns the factory list. A host looks for the list on a
    // unit rather than on the plugin, so leaving this at "none" is why the
    // presets were invisible even once they existed elsewhere.
    if (hasPrograms()) info->programListId = kProgramListId;
    return Steinberg_kResultOk;
  }
  const GroupTable groups = collectGroups(kDesc.params, kDesc.numParams);
  info->id = unitIndex;
  info->parentUnitId = 0;
  toString128(groups.names[unitIndex - 1], info->name);
  return Steinberg_kResultOk;
}

static Steinberg_int32 SMTG_STDMETHODCALLTYPE unitsGetProgramListCount(void*) {
  return hasPrograms() ? 1 : 0;
}

static Steinberg_tresult SMTG_STDMETHODCALLTYPE unitsGetProgramListInfo(
    void*, Steinberg_int32 listIndex, Steinberg_Vst_ProgramListInfo* info) {
  if (!info || !hasPrograms() || listIndex != 0) return Steinberg_kResultFalse;
  std::memset(info, 0, sizeof(*info));
  info->id = kProgramListId;
  toString128("Factory", info->name);
  info->programCount = kDesc.numPresets;
  return Steinberg_kResultOk;
}

static Steinberg_tresult SMTG_STDMETHODCALLTYPE unitsGetProgramName(
    void*, Steinberg_Vst_ProgramListID listId, Steinberg_int32 programIndex,
    Steinberg_Vst_String128 name) {
  if (!name || !hasPrograms() || listId != kProgramListId) return Steinberg_kResultFalse;
  if (programIndex < 0 || programIndex >= kDesc.numPresets) return Steinberg_kResultFalse;
  toString128(kDesc.presets[programIndex].name, name);
  return Steinberg_kResultOk;
}

static Steinberg_tresult SMTG_STDMETHODCALLTYPE unitsGetProgramInfo(void*,
                                                                    Steinberg_Vst_ProgramListID,
                                                                    Steinberg_int32,
                                                                    Steinberg_Vst_CString,
                                                                    Steinberg_Vst_String128) {
  return Steinberg_kResultFalse;
}

static Steinberg_tresult SMTG_STDMETHODCALLTYPE unitsHasProgramPitchNames(
    void*, Steinberg_Vst_ProgramListID, Steinberg_int32) {
  return Steinberg_kResultFalse;
}

static Steinberg_tresult SMTG_STDMETHODCALLTYPE unitsGetProgramPitchName(
    void*, Steinberg_Vst_ProgramListID, Steinberg_int32, Steinberg_int16,
    Steinberg_Vst_String128) {
  return Steinberg_kResultFalse;
}

static Steinberg_Vst_UnitID SMTG_STDMETHODCALLTYPE unitsGetSelectedUnit(void*) { return 0; }

static Steinberg_tresult SMTG_STDMETHODCALLTYPE unitsSelectUnit(void*, Steinberg_Vst_UnitID) {
  return Steinberg_kResultOk;
}

static Steinberg_tresult SMTG_STDMETHODCALLTYPE unitsGetUnitByBus(void*, Steinberg_Vst_MediaType,
                                                                  Steinberg_Vst_BusDirection,
                                                                  Steinberg_int32, Steinberg_int32,
                                                                  Steinberg_Vst_UnitID* unitId) {
  if (unitId) *unitId = 0; // every bus belongs to the root
  return Steinberg_kResultOk;
}

static Steinberg_tresult SMTG_STDMETHODCALLTYPE unitsSetUnitProgramData(void*, Steinberg_int32,
                                                                        Steinberg_int32,
                                                                        Steinberg_IBStream*) {
  return Steinberg_kResultFalse;
}

inline Steinberg_Vst_IUnitInfoVtbl* unitsVtbl() {
  static Steinberg_Vst_IUnitInfoVtbl v = {
      unitsQueryInterface,      unitsAddRef,
      unitsRelease,             unitsGetUnitCount,
      unitsGetUnitInfo,         unitsGetProgramListCount,
      unitsGetProgramListInfo,  unitsGetProgramName,
      unitsGetProgramInfo,      unitsHasProgramPitchNames,
      unitsGetProgramPitchName, unitsGetSelectedUnit,
      unitsSelectUnit,          unitsGetUnitByBus,
      unitsSetUnitProgramData,
  };
  return &v;
}

// ── IEditController ──────────────────────────────────────────────────────────

static Steinberg_tresult SMTG_STDMETHODCALLTYPE controllerInitialize(void*, Steinberg_FUnknown*) {
  return Steinberg_kResultOk;
}

static Steinberg_tresult SMTG_STDMETHODCALLTYPE controllerTerminate(void*) {
  return Steinberg_kResultOk;
}

static Steinberg_tresult SMTG_STDMETHODCALLTYPE controllerSetComponentState(
    void* self, Steinberg_IBStream* stream) {
  // Single component: the controller shares the processor's state, so this is
  // the same read rather than a second copy that could drift.
  return readState(ownerOfController(self), stream);
}

static Steinberg_tresult SMTG_STDMETHODCALLTYPE controllerSetState(void*, Steinberg_IBStream*) {
  return Steinberg_kResultOk; // no controller-only state exists
}

static Steinberg_tresult SMTG_STDMETHODCALLTYPE controllerGetState(void*, Steinberg_IBStream*) {
  return Steinberg_kResultOk;
}

/** Index of the first hidden MIDI-controller parameter. */
inline Steinberg_int32 firstMidiCcIndex() {
  return (Steinberg_int32) kDesc.numParams + (hasBypassParam() ? 1 : 0) +
         (hasPrograms() ? 1 : 0);
}

/** Index of the program-change parameter, or -1. */
inline Steinberg_int32 programParamIndex() {
  return hasPrograms() ? (Steinberg_int32) kDesc.numParams + (hasBypassParam() ? 1 : 0) : -1;
}

static Steinberg_int32 SMTG_STDMETHODCALLTYPE controllerGetParameterCount(void*) {
  return firstMidiCcIndex() + (Steinberg_int32) midiCcParamCount();
}

/** "Mod Wheel Ch3", "Pitch Bend Ch1", "CC 34 Ch16" -- hidden, but a host that
 *  shows them in a MIDI-learn list should show something a human can read. */
inline void midiCcName(int channel, int cc, char* out, size_t capacity) {
  const char* named = nullptr;
  switch (cc) {
    case 1: named = "Mod Wheel"; break;
    case 2: named = "Breath"; break;
    case 7: named = "Volume"; break;
    case 10: named = "Pan"; break;
    case 11: named = "Expression"; break;
    case 64: named = "Sustain"; break;
    case 74: named = "Brightness"; break;
    case Steinberg_Vst_ControllerNumbers_kAfterTouch: named = "Aftertouch"; break;
    case Steinberg_Vst_ControllerNumbers_kPitchBend: named = "Pitch Bend"; break;
    default: break;
  }
  if (named) std::snprintf(out, capacity, "%s Ch%d", named, channel + 1);
  else std::snprintf(out, capacity, "CC %d Ch%d", cc, channel + 1);
}

static Steinberg_tresult SMTG_STDMETHODCALLTYPE controllerGetParameterInfo(
    void*, Steinberg_int32 index, Steinberg_Vst_ParameterInfo* info) {
  if (!info || index < 0) return Steinberg_kInvalidArgument;
  if (index == kDesc.numParams && hasBypassParam()) {
    std::memset(info, 0, sizeof(*info));
    info->id = kBypassParamId;
    toString128("Bypass", info->title);
    toString128("Bypass", info->shortTitle);
    toString128("", info->units);
    info->stepCount = 1; // a switch: one step between Off and On
    info->defaultNormalizedValue = 0.0;
    info->unitId = 0; // bypass belongs to the plugin, not to a group
    info->flags = Steinberg_Vst_ParameterInfo_ParameterFlags_kCanAutomate |
                  Steinberg_Vst_ParameterInfo_ParameterFlags_kIsBypass;
    return Steinberg_kResultOk;
  }
  if (index == programParamIndex()) {
    std::memset(info, 0, sizeof(*info));
    info->id = kProgramParamId;
    toString128("Preset", info->title);
    toString128("Preset", info->shortTitle);
    toString128("", info->units);
    // stepCount is the number of steps BETWEEN values, so three presets is
    // two. Off by one here and the last preset is unreachable -- the same
    // trap the ordinary stepped parameters have a comment about.
    info->stepCount = kDesc.numPresets > 1 ? kDesc.numPresets - 1 : 0;
    info->defaultNormalizedValue = 0.0;
    info->unitId = 0; // the root unit owns the list
    info->flags = Steinberg_Vst_ParameterInfo_ParameterFlags_kCanAutomate |
                  Steinberg_Vst_ParameterInfo_ParameterFlags_kIsList |
                  Steinberg_Vst_ParameterInfo_ParameterFlags_kIsProgramChange;
    return Steinberg_kResultOk;
  }
  if (index >= firstMidiCcIndex() && index < firstMidiCcIndex() + (Steinberg_int32) midiCcParamCount()) {
    const int n = (int) (index - firstMidiCcIndex());
    const int channel = n / kMidiCcCount, cc = n % kMidiCcCount;
    std::memset(info, 0, sizeof(*info));
    info->id = midiCcParamId(channel, cc);
    char name[64];
    midiCcName(channel, cc, name, sizeof(name));
    toString128(name, info->title);
    toString128(name, info->shortTitle);
    toString128("", info->units);
    info->stepCount = 0;
    info->defaultNormalizedValue = midiCcDefault(cc);
    info->unitId = 0;
    // Automatable so the host will actually send values, hidden so it does not
    // clutter a parameter list with two thousand entries nobody chose.
    info->flags = Steinberg_Vst_ParameterInfo_ParameterFlags_kCanAutomate |
                  Steinberg_Vst_ParameterInfo_ParameterFlags_kIsHidden;
    return Steinberg_kResultOk;
  }
  if (index >= kDesc.numParams) return Steinberg_kInvalidArgument;
  const ParamInfo& p = kDesc.params[index];
  std::memset(info, 0, sizeof(*info));
  info->id = (Steinberg_Vst_ParamID) index; // index IS the id, as in CLAP
  toString128(p.label, info->title);
  toString128(p.label, info->shortTitle);
  toString128(p.unit, info->units);
  // VST3's stepCount is the number of steps BETWEEN values, so a 4-position
  // switch is 3. Off by one here and the last position becomes unreachable.
  info->stepCount = p.stepCount > 1 ? p.stepCount - 1 : 0;
  info->defaultNormalizedValue = toNormalised(p, p.defaultValue);
  info->unitId = unitIdFor(p);
  // Same order of precedence as CLAP: read-only outranks automatable, since a
  // value the host cannot set has nothing to record.
  info->flags = 0;
  if (p.readOnly) info->flags |= Steinberg_Vst_ParameterInfo_ParameterFlags_kIsReadOnly;
  else if (p.automatable) info->flags |= Steinberg_Vst_ParameterInfo_ParameterFlags_kCanAutomate;
  if (p.hidden) info->flags |= Steinberg_Vst_ParameterInfo_ParameterFlags_kIsHidden;
  if (p.stepCount > 1) info->flags |= Steinberg_Vst_ParameterInfo_ParameterFlags_kIsList;
  return Steinberg_kResultOk;
}

static Steinberg_tresult SMTG_STDMETHODCALLTYPE controllerGetParamStringByValue(
    void*, Steinberg_Vst_ParamID id, Steinberg_Vst_ParamValue valueNormalized,
    Steinberg_Vst_String128 string) {
  if (!string) return Steinberg_kInvalidArgument;
  if (id == kBypassParamId && hasBypassParam()) {
    toString128(valueNormalized >= 0.5 ? "On" : "Off", string);
    return Steinberg_kResultOk;
  }
  if (hasPrograms() && id == kProgramParamId) {
    // The preset's NAME, which is what a host shows in its program menu.
    const int index = programIndexFromNormalised(valueNormalized);
    toString128(kDesc.presets[index].name, string);
    return Steinberg_kResultOk;
  }
  if (isMidiCcParam(id)) {
    const int n = (int) (id - kMidiCcParamBase), cc = n % kMidiCcCount;
    char text[32];
    // Pitch bend is 14-bit on the wire; everything else is 7.
    const int span = cc == Steinberg_Vst_ControllerNumbers_kPitchBend ? 16383 : 127;
    std::snprintf(text, sizeof(text), "%d", (int) (valueNormalized * span + 0.5));
    toString128(text, string);
    return Steinberg_kResultOk;
  }
  if (id >= (Steinberg_Vst_ParamID) kDesc.numParams) return Steinberg_kInvalidArgument;
  const ParamInfo& p = kDesc.params[id];
  char text[64];
  formatParamValue(p, (float) toPlain(p, valueNormalized), text, sizeof(text));
  toString128(text, string);
  return Steinberg_kResultOk;
}

static Steinberg_tresult SMTG_STDMETHODCALLTYPE controllerGetParamValueByString(
    void*, Steinberg_Vst_ParamID id, Steinberg_Vst_TChar* string,
    Steinberg_Vst_ParamValue* valueNormalized) {
  if (!string || !valueNormalized) return Steinberg_kInvalidArgument;
  char text[64];
  fromString128(string, text, sizeof(text));
  if (id == kBypassParamId && hasBypassParam()) {
    const bool on = (text[0] == 'O' || text[0] == 'o') && (text[1] == 'n' || text[1] == 'N');
    *valueNormalized = (on || text[0] == '1') ? 1.0 : 0.0;
    return Steinberg_kResultOk;
  }
  if (isMidiCcParam(id)) {
    const int n = (int) (id - kMidiCcParamBase), cc = n % kMidiCcCount;
    const double span = cc == Steinberg_Vst_ControllerNumbers_kPitchBend ? 16383.0 : 127.0;
    const double v = std::atof(text) / span;
    *valueNormalized = v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
    return Steinberg_kResultOk;
  }
  if (id >= (Steinberg_Vst_ParamID) kDesc.numParams) return Steinberg_kInvalidArgument;
  float plain = 0.0f;
  if (!parseParamValue(kDesc.params[id], text, &plain)) return Steinberg_kResultFalse;
  *valueNormalized = toNormalised(kDesc.params[id], plain);
  return Steinberg_kResultOk;
}

static Steinberg_Vst_ParamValue SMTG_STDMETHODCALLTYPE controllerNormalizedParamToPlain(
    void*, Steinberg_Vst_ParamID id, Steinberg_Vst_ParamValue valueNormalized) {
  if (hasPrograms() && id == kProgramParamId)
    return (Steinberg_Vst_ParamValue) programIndexFromNormalised(valueNormalized);
  if (id == kBypassParamId && hasBypassParam()) return valueNormalized; // 0..1 IS plain
  if (isMidiCcParam(id)) {
    const int cc = (int) (id - kMidiCcParamBase) % kMidiCcCount;
    return valueNormalized *
           (cc == Steinberg_Vst_ControllerNumbers_kPitchBend ? 16383.0 : 127.0);
  }
  if (id >= (Steinberg_Vst_ParamID) kDesc.numParams) return 0.0;
  return toPlain(kDesc.params[id], valueNormalized);
}

static Steinberg_Vst_ParamValue SMTG_STDMETHODCALLTYPE controllerPlainParamToNormalized(
    void*, Steinberg_Vst_ParamID id, Steinberg_Vst_ParamValue plainValue) {
  if (hasPrograms() && id == kProgramParamId) {
    const double steps = kDesc.numPresets > 1 ? (double) (kDesc.numPresets - 1) : 1.0;
    const double v = plainValue / steps;
    return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
  }
  if (id == kBypassParamId && hasBypassParam()) return plainValue >= 0.5 ? 1.0 : 0.0;
  if (isMidiCcParam(id)) {
    const int cc = (int) (id - kMidiCcParamBase) % kMidiCcCount;
    const double span = cc == Steinberg_Vst_ControllerNumbers_kPitchBend ? 16383.0 : 127.0;
    const double v = plainValue / span;
    return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
  }
  if (id >= (Steinberg_Vst_ParamID) kDesc.numParams) return 0.0;
  return toNormalised(kDesc.params[id], plainValue);
}

/**
 * [main-thread] Tell the host the session changed underneath it.
 *
 * A host knows about anything it did itself. It has no idea about anything
 * the plugin did on its own -- a sampler that loaded a file through its own
 * browser, a convolver given a new impulse, anything living in the StateBag
 * rather than in a parameter. Without this the session is never marked dirty,
 * the DAW closes without asking, and the work is gone.
 *
 * setDirty lives on IComponentHandler2, which is an OPTIONAL extension of the
 * handler a host provides. A host that does not offer it is a host with no
 * way to be told, which is a normal answer and not a failure -- so the query
 * result is checked rather than assumed.
 */
inline void markDirtyIfNeeded(Plugin* plugin) {
  if (!plugin->stateDirty || !plugin->handler) return;
  Steinberg_Vst_IComponentHandler2* handler2 = nullptr;
  if (plugin->handler->lpVtbl->queryInterface(plugin->handler,
                                              Steinberg_Vst_IComponentHandler2_iid,
                                              (void**) &handler2) != Steinberg_kResultOk ||
      !handler2) {
    // Cleared anyway: there is nobody to tell, and holding the flag for ever
    // would mean querying a missing interface on every parameter poll.
    plugin->stateDirty = false;
    return;
  }
  handler2->lpVtbl->setDirty(handler2, 1);
  handler2->lpVtbl->release(handler2);
  plugin->stateDirty = false;
}

/**
 * [main-thread] Tell the host the latency moved, if it did.
 *
 * VST3 has no callback a plugin can ask for, so this is called from the
 * controller -- which IS the main thread -- after anything that could have
 * changed a parameter. restartComponent(kLatencyChanged) makes the host
 * re-read getLatencySamples and redo its delay compensation.
 *
 * Without it a plugin whose oversampling or look-ahead is a switch keeps the
 * latency it had when the session opened, and everything running in parallel
 * with it is quietly early. Nobody hears that as "the latency is wrong"; they
 * hear a smeared mix and blame the plugin.
 */
inline void notifyLatencyIfChanged(Plugin* plugin) {
  if (!clapwrap::HasLatency<SonoreDsp>::value || !plugin->handler) return;
  const uint32_t latency = clapwrap::dspLatency(plugin->shared.dsp);
  if (latency == plugin->shared.reportedLatency) return;
  plugin->shared.reportedLatency = latency;
  plugin->handler->lpVtbl->restartComponent(plugin->handler,
                                            Steinberg_Vst_RestartFlags_kLatencyChanged);
}

static Steinberg_Vst_ParamValue SMTG_STDMETHODCALLTYPE controllerGetParamNormalized(
    void* self, Steinberg_Vst_ParamID id) {
  Plugin* plugin = ownerOfController(self);
  // Checked HERE, of all places, because VST3 gives a plugin no way to ask to
  // be called on the main thread.
  //
  // CLAP has request_callback: the audio thread notices the latency moved and
  // asks to be rung back somewhere it is allowed to speak. VST3 has nothing of
  // the kind, and restartComponent must not be called from process(). What it
  // does have is a host that polls its parameters to keep its display current,
  // on the main thread, constantly. So that poll is the hook.
  //
  // The comparison is two integers and the notification only fires when they
  // differ, so a host polling sixty times a second pays nothing for it.
  notifyLatencyIfChanged(plugin);
  // Same hook, same reason: VST3 gives a plugin no way to ask to be called on
  // the main thread, and setDirty must not be called from process().
  markDirtyIfNeeded(plugin);
  if (hasPrograms() && id == kProgramParamId) {
    const double steps = kDesc.numPresets > 1 ? (double) (kDesc.numPresets - 1) : 1.0;
    return (double) plugin->currentProgram() / steps;
  }
  if (id == kBypassParamId && hasBypassParam()) return plugin->shared.bypass.engaged ? 1.0 : 0.0;
  if (isMidiCcParam(id)) {
    const int n = (int) (id - kMidiCcParamBase);
    return plugin->midiCc.v[n / kMidiCcCount][n % kMidiCcCount];
  }
  if (id >= (Steinberg_Vst_ParamID) SONORE_NUM_PARAMS) return 0.0;
  return toNormalised(kDesc.params[id], plugin->shared.params[id]);
}

static Steinberg_tresult SMTG_STDMETHODCALLTYPE controllerSetParamNormalized(
    void* self, Steinberg_Vst_ParamID id, Steinberg_Vst_ParamValue value) {
  Plugin* plugin = ownerOfController(self);
  if (id == kBypassParamId && hasBypassParam()) {
    plugin->shared.bypass.engaged = value >= 0.5;
    return Steinberg_kResultOk;
  }
  if (hasPrograms() && id == kProgramParamId) {
    const int index = programIndexFromNormalised(value);
    plugin->setCurrentProgram((Steinberg_int32) index);
    // Through applyPreset, like every other path that loads one: the rule
    // about a stale preset lives in one place and this is not a second copy.
    float values[SONORE_NUM_PARAMS > 0 ? SONORE_NUM_PARAMS : 1];
    if (applyPreset(kDesc.presets[index], kDesc.params, kDesc.numParams, values))
      for (int i = 0; i < kDesc.numParams; ++i) plugin->shared.params[i] = values[i];
    // Every other parameter just moved, and the host did not write any of
    // them. kParamValuesChanged is the only way VST3 has of saying so, and
    // without it a host keeps drawing the values from before the program --
    // the preset is heard but not seen, which reads as a preset that did not
    // load. Not performEdit: the host asked for this, so it is not an edit to
    // record, it is a re-read to request.
    //
    // Safe from here and only from here: this is the controller on the main
    // thread. The same program change arriving through the process event list
    // must NOT do it, because that is the audio thread.
    if (plugin->handler)
      plugin->handler->lpVtbl->restartComponent(plugin->handler,
                                                Steinberg_Vst_RestartFlags_kParamValuesChanged);
    // A preset rewrites every parameter at once, so it is one of the likeliest
    // ways for the latency to move.
    notifyLatencyIfChanged(plugin);
    return Steinberg_kResultOk;
  }
  if (isMidiCcParam(id)) {
    const int n = (int) (id - kMidiCcParamBase);
    const double v = value < 0.0 ? 0.0 : (value > 1.0 ? 1.0 : value);
    plugin->midiCc.v[n / kMidiCcCount][n % kMidiCcCount] = (float) v;
    return Steinberg_kResultOk;
  }
  if (id >= (Steinberg_Vst_ParamID) SONORE_NUM_PARAMS) return Steinberg_kInvalidArgument;
  plugin->shared.params[id] = (float) toPlain(kDesc.params[id], value);
  // The DSP reads its latency from the parameters, so a write here can move
  // it. Asked after every one rather than guessing which parameter matters:
  // the wrapper does not know what the DSP looks at.
  notifyLatencyIfChanged(plugin);
  return Steinberg_kResultOk;
}

// ── IProcessContextRequirements ──────────────────────────────────────────────
//
// VST3 3.7 turned the process context from "the host fills in what it can" into
// something the plugin has to ASK for. A plugin that does not implement this
// interface is treated as wanting everything, which sounds harmless and is
// not: Cubase logs it as a fault, and the SDK's own validator flags it. The
// point of the interface is that a host can skip work nobody wants -- querying
// a video engine for a frame rate on every block is not free.
//
// So this declares exactly the fields readTransport() actually reads, and
// nothing else. Asking for a chord track we never look at would be the same
// mistake in the other direction.

static Steinberg_uint32 SMTG_STDMETHODCALLTYPE contextReqGet(void*) {
  // Matched one for one against readTransport(): playing/recording/cycle come
  // from the transport state, then tempo, the musical position, the bar the
  // position sits in, and the time signature. projectTimeSamples needs no
  // flag -- VST3 always provides it.
  return (Steinberg_uint32) (
      Steinberg_Vst_IProcessContextRequirements_Flags_kNeedTransportState |
      Steinberg_Vst_IProcessContextRequirements_Flags_kNeedTempo |
      Steinberg_Vst_IProcessContextRequirements_Flags_kNeedProjectTimeMusic |
      Steinberg_Vst_IProcessContextRequirements_Flags_kNeedBarPositionMusic |
      Steinberg_Vst_IProcessContextRequirements_Flags_kNeedTimeSignature);
}

inline Steinberg_Vst_IProcessContextRequirementsVtbl* contextReqVtbl() {
  static Steinberg_Vst_IProcessContextRequirementsVtbl v = {
      contextReqQueryInterface, contextReqAddRef, contextReqRelease, contextReqGet,
  };
  return &v;
}

// ── IInfoListener: the track this instance sits on ──────────────────────
//
// VST3's version of CLAP's track-info: the host pushes an attribute list and
// the plugin reads out the keys it recognises. Pushed rather than pulled, so
// a plugin that ignores a key gets nothing rather than a wrong answer, and a
// host that knows nothing sends an empty list.
//
// What VST3 can say here is NARROWER than CLAP. There is a name, a colour, an
// index and where in the channel strip the plugin sits -- but nothing that
// distinguishes a return track from a bus from the master, which is the half
// of track info that changes a plugin's DEFAULTS rather than its appearance.
// So those three flags stay false on VST3 and a reverb cannot start wet by
// itself here. That is the format's limit, not a gap in this wrapper.

static Steinberg_tresult SMTG_STDMETHODCALLTYPE
infoListenerSetInfos(void* self, struct Steinberg_Vst_IAttributeList* list) {
  Plugin* plugin = ownerOfInfoListener(self);
  if (!plugin || !list || !list->lpVtbl) return Steinberg_kResultFalse;

  // Built fresh: a host sends the WHOLE state each time, so a key it stops
  // sending has stopped being true. Merging into what we had would leave a
  // track's old colour on it after the user cleared it.
  TrackInfo info;

  // The length key is advisory -- hosts have been seen sending the name
  // without it -- so it is a hint for the buffer, never a requirement.
  Steinberg_int64 length = 0;
  if (list->lpVtbl->getInt(list, Steinberg_Vst_ChannelContext_kChannelNameLengthKey, &length) !=
      Steinberg_kResultOk)
    length = 0;
  if (length < 0) length = 0;
  const size_t units = (size_t) (length > 0 && length < 4096 ? length : 128) + 1;
  std::vector<Steinberg_Vst_TChar> wide(units, 0);
  if (list->lpVtbl->getString(list, Steinberg_Vst_ChannelContext_kChannelNameKey, wide.data(),
                              (Steinberg_uint32) (units * sizeof(Steinberg_Vst_TChar))) ==
      Steinberg_kResultOk) {
    info.name = utf16ToUtf8(wide.data(), (int) units);
    // An empty name is not a name. A host that sends the key with nothing in
    // it should not have a plugin displaying a blank label where it would
    // otherwise have shown nothing at all.
    info.hasName = !info.name.empty();
  }

  Steinberg_int64 colour = 0;
  if (list->lpVtbl->getInt(list, Steinberg_Vst_ChannelContext_kChannelColorKey, &colour) ==
      Steinberg_kResultOk) {
    const unsigned int spec = (unsigned int) colour; // ARGB, one byte each
    info.hasColour = true;
    info.alpha = (unsigned char) ((spec >> 24) & 0xFF);
    info.red = (unsigned char) ((spec >> 16) & 0xFF);
    info.green = (unsigned char) ((spec >> 8) & 0xFF);
    info.blue = (unsigned char) (spec & 0xFF);
  }

  plugin->shared.trackInfo = info;
  clapwrap::sendTrackInfo(plugin->shared.dsp, plugin->shared.trackInfo);
  return Steinberg_kResultOk;
}

inline Steinberg_Vst_ChannelContext_IInfoListenerVtbl* infoListenerVtbl() {
  static Steinberg_Vst_ChannelContext_IInfoListenerVtbl v = {
      infoListenerQueryInterface, infoListenerAddRef, infoListenerRelease, infoListenerSetInfos,
  };
  return &v;
}

// ── INoteExpressionController ────────────────────────────────────────────────
//
// The wrapper has decoded VST3's own per-note expression events since the day
// it learned MPE. That code was unreachable: a host does not send note
// expression to a plugin that has not declared which types it accepts, so the
// switch statement in process() was waiting for events that could never come.
// This is what turns it on.
//
// Exactly the six types the decoder handles, and no more. Declaring a seventh
// that process() drops on the floor would be worse than declaring none: the
// host would show the control, the player would move it, and nothing would
// happen. VST3 has no standard PRESSURE id -- it carries pressure as poly
// aftertouch, which arrives as raw MIDI and is decoded there instead.

struct NoteExpressionSpec {
  Steinberg_Vst_NoteExpressionTypeID id;
  const char* title;
  const char* shortTitle;
  const char* units;
  double defaultValue; // in VST3's normalised domain, which is what a host sends
  bool bipolar;
};

inline const NoteExpressionSpec* noteExpressionTable(int* countOut) {
  // Defaults are the NO-EFFECT point of our own decoding, not tidy round
  // numbers: 0.5 tuning is (0.5-0.5)*240 = no detune, and 0.25 volume is
  // 0.25*4 = unity gain. A host resetting an expression to its default must
  // leave the sound exactly as it was.
  static const NoteExpressionSpec table[] = {
      {Steinberg_Vst_NoteExpressionTypeIDs_kTuningTypeID, "Tuning", "Tune", "semitones", 0.5,
       true},
      {Steinberg_Vst_NoteExpressionTypeIDs_kVolumeTypeID, "Volume", "Vol", "", 0.25, false},
      {Steinberg_Vst_NoteExpressionTypeIDs_kPanTypeID, "Pan", "Pan", "", 0.5, true},
      {Steinberg_Vst_NoteExpressionTypeIDs_kBrightnessTypeID, "Brightness", "Bright", "", 0.5,
       false},
      {Steinberg_Vst_NoteExpressionTypeIDs_kVibratoTypeID, "Vibrato", "Vib", "", 0.0, false},
      {Steinberg_Vst_NoteExpressionTypeIDs_kExpressionTypeID, "Expression", "Expr", "", 1.0,
       false},
  };
  if (countOut) *countOut = (int) (sizeof(table) / sizeof(table[0]));
  return table;
}

static Steinberg_int32 SMTG_STDMETHODCALLTYPE noteExpressionGetCount(void*,
                                                                     Steinberg_int32 busIndex,
                                                                     Steinberg_int16 channel) {
  if (busIndex != 0 || channel < 0 || channel >= kMidiChannels) return 0;
  int n = 0;
  noteExpressionTable(&n);
  return (Steinberg_int32) n;
}

static Steinberg_tresult SMTG_STDMETHODCALLTYPE noteExpressionGetInfo(
    void*, Steinberg_int32 busIndex, Steinberg_int16 channel, Steinberg_int32 index,
    struct Steinberg_Vst_NoteExpressionTypeInfo* info) {
  if (!info || busIndex != 0 || channel < 0 || channel >= kMidiChannels)
    return Steinberg_kInvalidArgument;
  int n = 0;
  const NoteExpressionSpec* table = noteExpressionTable(&n);
  if (index < 0 || index >= n) return Steinberg_kInvalidArgument;
  const NoteExpressionSpec& spec = table[index];
  std::memset(info, 0, sizeof(*info));
  info->typeId = spec.id;
  toString128(spec.title, info->title);
  toString128(spec.shortTitle, info->shortTitle);
  toString128(spec.units, info->units);
  info->unitId = 0;
  info->valueDesc.defaultValue = spec.defaultValue;
  info->valueDesc.minimum = 0.0;
  info->valueDesc.maximum = 1.0;
  info->valueDesc.stepCount = 0; // continuous
  info->associatedParameterId = 0;
  info->flags = spec.bipolar
                    ? Steinberg_Vst_NoteExpressionTypeInfo_NoteExpressionTypeFlags_kIsBipolar
                    : 0;
  return Steinberg_kResultOk;
}

static Steinberg_tresult SMTG_STDMETHODCALLTYPE noteExpressionStringByValue(
    void*, Steinberg_int32, Steinberg_int16, Steinberg_Vst_NoteExpressionTypeID id,
    Steinberg_Vst_NoteExpressionValue valueNormalized, Steinberg_Vst_String128 string) {
  if (!string) return Steinberg_kInvalidArgument;
  char text[64];
  if (id == Steinberg_Vst_NoteExpressionTypeIDs_kTuningTypeID)
    std::snprintf(text, sizeof(text), "%+.2f", (valueNormalized - 0.5) * 240.0);
  else if (id == Steinberg_Vst_NoteExpressionTypeIDs_kVolumeTypeID)
    std::snprintf(text, sizeof(text), "%.3f", valueNormalized * 4.0);
  else
    std::snprintf(text, sizeof(text), "%.3f", valueNormalized);
  toString128(text, string);
  return Steinberg_kResultOk;
}

static Steinberg_tresult SMTG_STDMETHODCALLTYPE noteExpressionValueByString(
    void*, Steinberg_int32, Steinberg_int16, Steinberg_Vst_NoteExpressionTypeID id,
    const Steinberg_Vst_TChar* string, Steinberg_Vst_NoteExpressionValue* valueNormalized) {
  if (!string || !valueNormalized) return Steinberg_kInvalidArgument;
  char text[64];
  fromString128(string, text, sizeof(text));
  double v = std::atof(text);
  if (id == Steinberg_Vst_NoteExpressionTypeIDs_kTuningTypeID) v = v / 240.0 + 0.5;
  else if (id == Steinberg_Vst_NoteExpressionTypeIDs_kVolumeTypeID) v = v / 4.0;
  *valueNormalized = v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
  return Steinberg_kResultOk;
}

inline Steinberg_Vst_INoteExpressionControllerVtbl* noteExpressionVtbl() {
  static Steinberg_Vst_INoteExpressionControllerVtbl v = {
      noteExpressionQueryInterface, noteExpressionAddRef,       noteExpressionRelease,
      noteExpressionGetCount,       noteExpressionGetInfo,      noteExpressionStringByValue,
      noteExpressionValueByString,
  };
  return &v;
}

// ── IMidiMapping ─────────────────────────────────────────────────────────────

static Steinberg_tresult SMTG_STDMETHODCALLTYPE midiMappingGetAssignment(
    void*, Steinberg_int32 busIndex, Steinberg_int16 channel,
    Steinberg_Vst_CtrlNumber midiControllerNumber, Steinberg_Vst_ParamID* id) {
  if (!id) return Steinberg_kInvalidArgument;
  // One event bus, sixteen channels, the controllers VST3 defines. Anything
  // outside that is answered honestly with "no assignment" rather than with a
  // parameter id that would then never behave.
  if (busIndex != 0 || channel < 0 || channel >= kMidiChannels) return Steinberg_kResultFalse;
  if (midiControllerNumber < 0 || midiControllerNumber >= kMidiCcCount)
    return Steinberg_kResultFalse;
  *id = midiCcParamId((int) channel, (int) midiControllerNumber);
  return Steinberg_kResultOk;
}

inline Steinberg_Vst_IMidiMappingVtbl* midiMappingVtbl() {
  static Steinberg_Vst_IMidiMappingVtbl v = {
      midiMappingQueryInterface, midiMappingAddRef, midiMappingRelease,
      midiMappingGetAssignment,
  };
  return &v;
}

static Steinberg_tresult SMTG_STDMETHODCALLTYPE controllerSetComponentHandler(
    void* self, Steinberg_Vst_IComponentHandler* handler) {
  ownerOfController(self)->handler = handler;
  return Steinberg_kResultOk;
}

Steinberg_IPlugView* SMTG_STDMETHODCALLTYPE controllerCreateView(void* self,
                                                                 Steinberg_FIDString name);

// ── The class factory ────────────────────────────────────────────────────────

inline Steinberg_Vst_IComponentVtbl& componentVtbl() {
  static Steinberg_Vst_IComponentVtbl v = {
      componentQueryInterface, componentAddRef, componentRelease,
      componentInitialize, componentTerminate, componentGetControllerClassId,
      componentSetIoMode, componentGetBusCount, componentGetBusInfo,
      componentGetRoutingInfo, componentActivateBus, componentSetActive,
      componentSetState, componentGetState,
  };
  return v;
}

inline Steinberg_Vst_IAudioProcessorVtbl& processorVtbl() {
  static Steinberg_Vst_IAudioProcessorVtbl v = {
      processorQueryInterface, processorAddRef, processorRelease,
      processorSetBusArrangements, processorGetBusArrangement, processorCanProcessSampleSize,
      processorGetLatencySamples, processorSetupProcessing, processorSetProcessing,
      processorProcess, processorGetTailSamples,
  };
  return v;
}

inline Steinberg_Vst_IEditControllerVtbl& controllerVtbl() {
  static Steinberg_Vst_IEditControllerVtbl v = {
      controllerQueryInterface, controllerAddRef, controllerRelease,
      controllerInitialize, controllerTerminate, controllerSetComponentState,
      controllerSetState, controllerGetState, controllerGetParameterCount,
      controllerGetParameterInfo, controllerGetParamStringByValue,
      controllerGetParamValueByString, controllerNormalizedParamToPlain,
      controllerPlainParamToNormalized, controllerGetParamNormalized,
      controllerSetParamNormalized, controllerSetComponentHandler, controllerCreateView,
  };
  return v;
}

inline Plugin* createPlugin() {
  void* memory = std::malloc(sizeof(Plugin));
  if (!memory) return nullptr;
  Plugin* plugin = new (memory) Plugin();
  plugin->component.lpVtbl = &componentVtbl();
  plugin->component.owner = plugin;
  plugin->processor.lpVtbl = &processorVtbl();
  plugin->processor.owner = plugin;
  plugin->controller.lpVtbl = &controllerVtbl();
  plugin->controller.owner = plugin;
  plugin->units.lpVtbl = unitsVtbl();
  plugin->units.owner = plugin;
  plugin->midiMapping.lpVtbl = midiMappingVtbl();
  plugin->midiMapping.owner = plugin;
  plugin->noteExpression.lpVtbl = noteExpressionVtbl();
  plugin->noteExpression.owner = plugin;
  plugin->contextRequirements.lpVtbl = contextReqVtbl();
  plugin->contextRequirements.owner = plugin;
  plugin->infoListener.lpVtbl = infoListenerVtbl();
  plugin->infoListener.owner = plugin;
  for (int i = 0; i < SONORE_NUM_PARAMS && i < kDesc.numParams; ++i)
    plugin->shared.params[i] = kDesc.params[i].defaultValue;
  return plugin;
}

inline void destroyPlugin(Plugin* self) {
  // A view can outlive the plugin's last reference; cut its back-pointer rather
  // than leaving it to call into freed memory.
  if (self->view) self->view->plugin = nullptr;
  self->~Plugin();
  std::free(self);
}

} // namespace vst3
} // namespace sonore

// ─────────────────────────────────────────────────────────────────────────────
// The editor view, the class factory, and the module entry points.
// ─────────────────────────────────────────────────────────────────────────────

namespace sonore {
namespace vst3 {

/** Which platform handle this build embeds into. */
inline Steinberg_FIDString nativePlatformType() {
#if defined(_WIN32)
  return Steinberg_kPlatformTypeHWND;
#elif defined(__APPLE__)
  return Steinberg_kPlatformTypeNSView;
#else
  return Steinberg_kPlatformTypeX11EmbedWindowID;
#endif
}

inline View* ownerOfView(void* p) { return ((ViewIface*) p)->owner; }

void destroyView(View* view);

static Steinberg_tresult SMTG_STDMETHODCALLTYPE viewQueryInterface(void* self,
                                                                   const Steinberg_TUID iid,
                                                                   void** obj) {
  if (!obj) return Steinberg_kInvalidArgument;
  View* view = ownerOfView(self);
  if (sameUid(iid, Steinberg_FUnknown_iid) || sameUid(iid, Steinberg_IPlugView_iid)) {
    *obj = &view->iface;
    ++view->refs;
    return Steinberg_kResultOk;
  }
  if (sameUid(iid, Steinberg_IPlugViewContentScaleSupport_iid)) {
    *obj = &view->scaleIface;
    ++view->refs;
    return Steinberg_kResultOk;
  }
  *obj = nullptr;
  return Steinberg_kNoInterface;
}

static Steinberg_uint32 SMTG_STDMETHODCALLTYPE viewAddRef(void* self) {
  return ++ownerOfView(self)->refs;
}

static Steinberg_uint32 SMTG_STDMETHODCALLTYPE viewRelease(void* self) {
  View* view = ownerOfView(self);
  const Steinberg_uint32 n = --view->refs;
  if (n == 0) destroyView(view);
  return n;
}

static Steinberg_tresult SMTG_STDMETHODCALLTYPE viewIsPlatformTypeSupported(
    void*, Steinberg_FIDString type) {
  return (type && std::strcmp(type, nativePlatformType()) == 0) ? Steinberg_kResultTrue
                                                                : Steinberg_kResultFalse;
}

/**
 * A message from the page. Unlike CLAP: where the plugin emits parameter
 * events from the audio thread: VST3 requires the host to be told through
 * IComponentHandler on THIS (main) thread. So an edit goes two ways at once:
 * to the host for automation and undo, and into the lock-free queue for the
 * DSP. Sending only one of the two is how a knob either moves the sound
 * without being recorded, or gets recorded without being heard.
 */
/**
 * Ask the host to show ITS menu for one parameter.
 *
 * MIDI learn, "remove automation", "show automation lane" -- a plugin cannot
 * offer any of them, and the host cannot offer them if the plugin swallows the
 * right-click. So the click is handed straight back.
 *
 * VST3 spells this as an extra interface on the handler the host already gave
 * us: IComponentHandler3 builds a menu for a parameter, already populated with
 * the host's own items, and the plugin pops it up. A host that does not
 * implement it -- which is legal and common in older ones -- leaves the
 * right-click doing exactly what it did before.
 *
 * The menu is a COM object we now own: created here, popped up, released here.
 * Leaking one leaks a window as well as memory.
 */
inline bool showHostContextMenu(Plugin* plugin, int paramIndex, int x, int y) {
  if (!plugin || !plugin->handler || !plugin->view) return false;
  if (paramIndex < 0 || paramIndex >= SONORE_NUM_PARAMS) return false;

  Steinberg_Vst_IComponentHandler3* h3 = nullptr;
  if (plugin->handler->lpVtbl->queryInterface(plugin->handler,
                                              Steinberg_Vst_IComponentHandler3_iid,
                                              (void**) &h3) != Steinberg_kResultOk ||
      !h3)
    return false;

  const Steinberg_Vst_ParamID id = (Steinberg_Vst_ParamID) paramIndex;
  Steinberg_Vst_IContextMenu* menu =
      h3->lpVtbl->createContextMenu(h3, (struct Steinberg_IPlugView*) &plugin->view->iface, &id);
  h3->lpVtbl->release(h3);
  if (!menu) return false;

  const Steinberg_tresult shown = menu->lpVtbl->popup(menu, (Steinberg_UCoord) x,
                                                      (Steinberg_UCoord) y);
  menu->lpVtbl->release(menu);
  return shown == Steinberg_kResultOk;
}

/**
 * [main-thread] Show a native file browser and tell everyone what came back.
 *
 * The DSP first, then the page: the page usually reacts by showing the
 * filename, and showing it before the DSP has accepted the file puts a name
 * on screen for a sample that is not loaded yet.
 */
inline void chooseFileForPage(Plugin* plugin, const BridgeMessage& message) {
#if defined(SONORE_HAS_WEBVIEW_BACKEND)
  void* parent = plugin->shared.webview.nativeWindow();
  const std::string path = FileDialog::byMode(message.mode, parent);

  clapwrap::sendFile(plugin->shared.dsp, message.purpose.c_str(), path.c_str());
  plugin->shared.uiStateDirty = true; // the DSP may have taken a file

  // A file the plugin loaded is exactly the change no host can see: it moved
  // no parameter and called nothing. Without this the DAW closes without
  // offering to save, and the user loses the sample they just picked.
  if (!path.empty()) {
    plugin->shared.stateDirty = true;
    markDirtyIfNeeded(plugin);
  }

  plugin->shared.webview.eval(fileAnswerScript(message.purpose, path));
#else
  (void) plugin;
  (void) message;
#endif
}

inline void viewOnMessage(Plugin* plugin, const BridgeMessage& message) {
  if (!plugin) return;
  UiEventQueue::Event e;
  switch (message.kind) {
    case BridgeMessage::Kind::CaptureKeys:
      // Straight onto the webview. Nothing crosses to audio: this decides who
      // receives a keystroke, which is a question only the UI thread has.
      plugin->shared.webview.captureKeys = message.value >= 0.5;
      return;
    case BridgeMessage::Kind::ChooseFile:
      chooseFileForPage(plugin, message);
      return;
    case BridgeMessage::Kind::ContextMenu:
      // Straight through on this thread. The host's menu is modal, and the
      // only other place this could go is the audio callback.
      showHostContextMenu(plugin, message.index, message.x, message.y);
      return;
    case BridgeMessage::Kind::Set: {
      if (message.index < 0 || message.index >= SONORE_NUM_PARAMS) return;
      const ParamInfo& p = kDesc.params[message.index];
      const float plain = clampToRange(p, (float) message.value);
      e.kind = UiEventQueue::Event::Kind::ParamSet;
      e.index = message.index;
      e.value = plain;
      plugin->shared.uiEcho[message.index] = plain;
      if (plugin->handler)
        plugin->handler->lpVtbl->performEdit(plugin->handler, (Steinberg_Vst_ParamID) message.index,
                                             toNormalised(p, plain));
      break;
    }
    case BridgeMessage::Kind::GestureBegin:
    case BridgeMessage::Kind::GestureEnd: {
      if (message.index < 0 || message.index >= SONORE_NUM_PARAMS) return;
      // Gestures are what let a host fold a whole drag into one undo step and
      // override automation playback while the mouse is down.
      if (plugin->handler) {
        const auto id = (Steinberg_Vst_ParamID) message.index;
        if (message.kind == BridgeMessage::Kind::GestureBegin)
          plugin->handler->lpVtbl->beginEdit(plugin->handler, id);
        else
          plugin->handler->lpVtbl->endEdit(plugin->handler, id);
      }
      return; // nothing for the DSP to do
    }
    case BridgeMessage::Kind::LoadPreset: {
      if (message.index < 0 || message.index >= kDesc.numPresets || !kDesc.presets) return;
      // The same guard the CLAP wrapper uses, and now literally the same code:
      // one rule in presets.h with one test, rather than three copies of it
      // where the tested one was the copy nothing called.
      float presetValues[SONORE_NUM_PARAMS > 0 ? SONORE_NUM_PARAMS : 1];
      if (!applyPreset(kDesc.presets[message.index], kDesc.params, kDesc.numParams,
                       presetValues))
        return;
      for (int i = 0; i < kDesc.numParams; ++i) {
        const float plain = presetValues[i];
        UiEventQueue::Event pe;
        pe.kind = UiEventQueue::Event::Kind::ParamSet;
        pe.index = i;
        pe.value = plain;
        plugin->shared.uiEvents.push(pe);
        plugin->shared.uiEcho[i] = plain;
        if (plugin->handler)
          plugin->handler->lpVtbl->performEdit(plugin->handler, (Steinberg_Vst_ParamID) i,
                                               toNormalised(kDesc.params[i], plain));
      }
      return;
    }
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
      return;
  }
  plugin->shared.uiEvents.push(e);
}

/** The ~30 Hz editor clock: push whatever the page does not know yet. */
inline void viewTick(Plugin* plugin) {
  if (!plugin) return;
  // The native editor runs its own clock and pulls its own values; there is
  // no page to push a script into.
  if (plugin->shared.guiIsNative) {
    plugin->shared.nativeEditor.tick();
    return;
  }
#if defined(SONORE_HAS_WEBVIEW_BACKEND)
  if (!plugin->shared.webview.ready()) return;
  plugin->shared.webview.eval(clapwrap::uiTickScript(plugin->shared));
#endif
}

/**
 * The user dragged an edge. Ask the host, through the frame it gave us.
 *
 * IPlugFrame::resizeView is VST3's half of CLAP's request_resize, and it has
 * the same rule behind it: the host owns the window, so a view resizes by
 * asking. A host that never called setFrame has given no way to ask, and the
 * drag does nothing -- honest, and not a failure to report.
 *
 * resizeView calls BACK into onSize before it returns, which is where the size
 * is actually applied. Nothing is written here for that reason: a wrapper that
 * set the size itself and then let the callback set it again would have two
 * sources for one number.
 */
static void requestViewResize(View* view, int width, int height) {
  if (!view || !view->frame) return;
  uint32_t w = width > 0 ? (uint32_t) width : 1u;
  uint32_t h = height > 0 ? (uint32_t) height : 1u;
  clapwrap::clampEditorSize(&w, &h);
  Steinberg_ViewRect rect{};
  rect.right = (Steinberg_int32) scaled(w, view->contentScale);
  rect.bottom = (Steinberg_int32) scaled(h, view->contentScale);
  view->frame->lpVtbl->resizeView(view->frame, (Steinberg_IPlugView*) &view->iface, &rect);
}

/**
 * The CLAP-side editor host, with the parts only VST3 can answer replaced.
 *
 * makeEditorHost is shared because parameters, gestures and notes are the same
 * job in both formats. The context menu is not: it goes through
 * IComponentHandler3 here and clap_host_context_menu there, and the shared
 * version would quietly do nothing in a VST3 -- Instance::host is a clap_host_t
 * and there isn't one.
 */
static gfx::EditorHost makeVst3EditorHost(Plugin* plugin) {
  gfx::EditorHost host = clapwrap::makeEditorHost(&plugin->shared);
  host.showContextMenu = [plugin](int index, int x, int y) {
    showHostContextMenu(plugin, index, x, y);
  };
  return host;
}

/** What a native editor needs told before it opens, at both call sites. */
static void prepareNativeEditor(View* view, Plugin* plugin) {
  plugin->shared.nativeEditor.setResizeLimits(kDesc.editorLimits);
  plugin->shared.nativeEditor.onRequestResize = [view](int w, int h) {
    requestViewResize(view, w, h);
  };
}

static Steinberg_tresult SMTG_STDMETHODCALLTYPE viewAttached(void* self, void* parent,
                                                             Steinberg_FIDString type) {
  View* view = ownerOfView(self);
  if (!view->plugin || !parent) return Steinberg_kResultFalse;
  if (!type || std::strcmp(type, nativePlatformType()) != 0) return Steinberg_kResultFalse;
  Plugin* plugin = view->plugin;
  plugin->shared.uiEchoValid = false;

  // The same choice the CLAP side makes, from the same function, so one
  // plugin does not open a native editor in one host and a web one in
  // another. VST3 sizes its window in device pixels exactly as CLAP does.
  const EditorChoice choice = clapwrap::editorChoice();
  if (choice.backend == EditorBackend::None) return Steinberg_kResultFalse;
  plugin->shared.guiIsNative = choice.backend == EditorBackend::Native;
  if (plugin->shared.guiIsNative) {
    // BEFORE open: the border is built there and reads the limits then.
    prepareNativeEditor(view, plugin);
    // LOGICAL size, with the scale applied SEPARATELY -- the native editor
    // lays its component tree out in logical units, exactly as the CLAP side
    // does in guiSetParent. Handing it the device size (scaled) laid the tree
    // out in device pixels: at 200% the controls stayed the same pixel count,
    // i.e. half their physical size, and the window was right while the
    // interface was tiny. view->width IS logical.
    if (!plugin->shared.nativeEditor.open(parent, kDesc.params, (int) kDesc.numParams,
                                          makeVst3EditorHost(plugin), (int) view->width,
                                          (int) view->height))
      return Steinberg_kResultFalse;
    if (view->contentScale != 1.0f)
      plugin->shared.nativeEditor.setScale(view->contentScale);
    view->attached = true;
    return Steinberg_kResultOk;
  }
#if defined(SONORE_HAS_WEBVIEW_BACKEND)
  plugin->shared.webview.onMessage = [plugin](const BridgeMessage& m) { viewOnMessage(plugin, m); };
  plugin->shared.webview.onTick = [plugin]() { viewTick(plugin); };
#if defined(_WIN32)
  // The window is made in DEVICE pixels. The page is laid out in logical ones
  // and the webview draws each as `contentScale` device pixels, so a window
  // built at the logical size shows the top-left corner of a correctly drawn
  // page and nothing else.
  const bool ok = plugin->shared.webview.create(
      (HWND) parent, scaled(view->width, view->contentScale),
      scaled(view->height, view->contentScale), clapwrap::uiHtml(), bridgeScript(kDesc), kDesc.id);
#elif defined(__APPLE__)
  const bool ok = plugin->shared.webview.create(parent, view->width, view->height,
                                                clapwrap::uiHtml(), bridgeScript(kDesc), kDesc.id);
#else
  // X11EmbedWindowID hands the window id THROUGH the pointer, not as one.
  const bool ok = plugin->shared.webview.create((unsigned long) (uintptr_t) parent, view->width,
                                                view->height, clapwrap::uiHtml(),
                                                bridgeScript(kDesc), kDesc.id);
#endif
  if (!ok) {
    // The webview did not start -- no WebView2 runtime, no WebKitGTK. The
    // native editor needs neither, so the plugin loses its designed face and
    // keeps every control rather than the other way round. Same fallback as
    // the CLAP side, for the same reason.
    prepareNativeEditor(view, plugin);
    if (!gfx::NativeEditor::isAvailable() ||
        !plugin->shared.nativeEditor.open(parent, kDesc.params, (int) kDesc.numParams,
                                          makeVst3EditorHost(plugin), (int) view->width,
                                          (int) view->height)) {
      return Steinberg_kResultFalse;
    }
    if (view->contentScale != 1.0f)
      plugin->shared.nativeEditor.setScale(view->contentScale);
    plugin->shared.guiIsNative = true;
    view->attached = true;
    return Steinberg_kResultOk;
  }
  plugin->shared.webview.setVisible(true);
  view->attached = true;
  return Steinberg_kResultOk;
#else
  return Steinberg_kResultFalse;
#endif
}

static Steinberg_tresult SMTG_STDMETHODCALLTYPE viewRemoved(void* self) {
  View* view = ownerOfView(self);
  if (view->plugin) {
    // Both, unconditionally: guiIsNative says which one was opened, and the
    // webview fallback above changes that answer after the fact.
    view->plugin->shared.nativeEditor.close();
    // The resize callback captured THIS view, and the editor outlives it --
    // it lives on the shared state, the view does not. close() destroys the
    // border so nothing can currently reach a stale pointer, but a callback
    // that merely happens to be unreachable is one bug away from being called.
    // Cleared where the view goes, which is the only place that knows.
    view->plugin->shared.nativeEditor.onRequestResize = nullptr;
    view->plugin->shared.guiIsNative = false;
#if defined(SONORE_HAS_WEBVIEW_BACKEND)
    view->plugin->shared.webview.destroy();
#endif
  }
  view->attached = false;
  return Steinberg_kResultOk;
}

static Steinberg_tresult SMTG_STDMETHODCALLTYPE viewOnWheel(void*, float) {
  return Steinberg_kResultFalse; // the page handles its own input
}
static Steinberg_tresult SMTG_STDMETHODCALLTYPE viewOnKeyDown(void*, Steinberg_char16,
                                                              Steinberg_int16, Steinberg_int16) {
  return Steinberg_kResultFalse;
}
static Steinberg_tresult SMTG_STDMETHODCALLTYPE viewOnKeyUp(void*, Steinberg_char16,
                                                            Steinberg_int16, Steinberg_int16) {
  return Steinberg_kResultFalse;
}
static Steinberg_tresult SMTG_STDMETHODCALLTYPE viewOnFocus(void*, Steinberg_TBool) {
  return Steinberg_kResultOk;
}

/**
 * What the host is told the editor needs, in PHYSICAL pixels.
 *
 * The distinction only exists on a scaled display, and there it is the whole
 * story. The layout is 620x300 CSS pixels whatever the monitor does; at 200%
 * the webview draws each of those as two device pixels, so the window has to
 * be 1240x600 or the page is drawn correctly into a hole half its size and
 * the right-hand third of the plugin is simply missing.
 *
 * A host that was never told a scale leaves this at 1 and nothing changes,
 * which is every 100% display and every host that does not ask.
 */
static Steinberg_tresult SMTG_STDMETHODCALLTYPE viewGetSize(void* self,
                                                            Steinberg_ViewRect* size) {
  if (!size) return Steinberg_kInvalidArgument;
  View* view = ownerOfView(self);
  size->left = 0;
  size->top = 0;
  size->right = (Steinberg_int32) scaled(view->width, view->contentScale);
  size->bottom = (Steinberg_int32) scaled(view->height, view->contentScale);
  return Steinberg_kResultOk;
}

/**
 * The host telling the plugin what a logical pixel is worth here.
 *
 * VST3 makes this a separate interface a view MAY implement, and not
 * implementing it is a legitimate answer meaning "I am not DPI aware" -- at
 * which point a host is entitled to scale the window itself and hand back a
 * blurry bitmap. Saying yes and doing the arithmetic is the difference
 * between a crisp editor and a soft one on the majority of Windows laptops,
 * which ship at 125% or 150%.
 */
static Steinberg_tresult SMTG_STDMETHODCALLTYPE viewSetContentScaleFactor(void* self,
                                                                          float factor) {
  View* view = ownerOfView(self);
  // A zero or negative scale is not a value to clamp quietly -- it is a host
  // asking for something that cannot be drawn, and the honest answer is no.
  if (!(factor > 0.0f) || factor > 8.0f) return Steinberg_kInvalidArgument;
  if (view->contentScale == factor) return Steinberg_kResultOk;
  view->contentScale = factor;

  // Already on screen: the window has to change size NOW, and the host has to
  // be told or its frame keeps the old one. A scale change arriving after
  // attach is what happens when a user drags a plugin window between two
  // monitors with different scaling, which is a thing people do constantly.
  if (view->attached && view->plugin) {
    if (view->plugin->shared.guiIsNative) {
      // The scale, not a pre-multiplied device size. setScale re-lays-out at
      // the new scale keeping the logical size; setSize(device) would lay the
      // tree out in device pixels -- the half-size-controls bug again.
      view->plugin->shared.nativeEditor.setScale(factor);
    } else {
#if defined(SONORE_HAS_WEBVIEW_BACKEND)
      view->plugin->shared.webview.setSize(scaled(view->width, factor),
                                           scaled(view->height, factor));
#endif
    }
    if (view->frame) {
      Steinberg_ViewRect rect{};
      rect.right = (Steinberg_int32) scaled(view->width, factor);
      rect.bottom = (Steinberg_int32) scaled(view->height, factor);
      view->frame->lpVtbl->resizeView(view->frame, (Steinberg_IPlugView*) &view->iface, &rect);
    }
  }
  return Steinberg_kResultOk;
}


static Steinberg_tresult SMTG_STDMETHODCALLTYPE viewOnSize(void* self,
                                                           Steinberg_ViewRect* newSize) {
  if (!newSize) return Steinberg_kInvalidArgument;
  View* view = ownerOfView(self);
  const Steinberg_int32 w = newSize->right - newSize->left;
  const Steinberg_int32 h = newSize->bottom - newSize->top;
  if (w <= 0 || h <= 0) return Steinberg_kInvalidArgument;
  // The host's rect is in DEVICE pixels (that is what viewGetSize returns);
  // view->width and shared.guiWidth are LOGICAL. Convert before storing, or the
  // logical field fills with device values and viewGetSize re-scales them --
  // the editor doubles on every host round-trip at any non-1 scale, and the
  // inflated value is written into the shared state and the v5 state blob,
  // infecting saved sessions and the next editor of every format. A host may
  // resize without asking checkSizeConstraint first, so the clamp lands here.
  const double sc = view->contentScale > 0.0f ? (double) view->contentScale : 1.0;
  uint32_t cw = (uint32_t) ((double) w / sc + 0.5);
  uint32_t ch = (uint32_t) ((double) h / sc + 0.5);
  clapwrap::clampEditorSize(&cw, &ch); // logical bounds
  view->width = cw;
  view->height = ch;
  if (view->plugin) {
    // The LOGICAL size on the shared state, so the next editor -- in ANY
    // format -- reopens where the user left this one, at the right size.
    view->plugin->shared.guiWidth = view->width;
    view->plugin->shared.guiHeight = view->height;
    if (view->plugin->shared.guiIsNative) {
      view->plugin->shared.nativeEditor.setSize((int) view->width, (int) view->height);
    } else {
#if defined(SONORE_HAS_WEBVIEW_BACKEND)
      view->plugin->shared.webview.setSize(scaled(view->width, view->contentScale),
                                           scaled(view->height, view->contentScale));
#endif
    }
  }
  return Steinberg_kResultOk;
}

static Steinberg_tresult SMTG_STDMETHODCALLTYPE viewSetFrame(void* self,
                                                             Steinberg_IPlugFrame* frame) {
  // Kept now, where it used to be discarded. The page never asks for a resize
  // of its own -- HTML reflows to whatever it is given -- but a SCALE CHANGE
  // does need one: dragging a plugin window to a monitor at a different DPI
  // changes how many device pixels the same layout needs, and the host has to
  // be told or its window keeps the old size around a correctly drawn page.
  ownerOfView(self)->frame = frame;
  return Steinberg_kResultOk;
}

static Steinberg_tresult SMTG_STDMETHODCALLTYPE viewCanResize(void*) {
  // Answered from the descriptor rather than always yes. A host that is told
  // a fixed-size view can be resized draws a grip, lets the user drag it, and
  // then shows them a window whose contents do not follow -- which reads as a
  // broken plugin rather than as a fixed one.
  return (kDesc.editorLimits.resizableHorizontally || kDesc.editorLimits.resizableVertically)
             ? Steinberg_kResultTrue
             : Steinberg_kResultFalse;
}

static Steinberg_tresult SMTG_STDMETHODCALLTYPE viewCheckSizeConstraint(void*,
                                                                        Steinberg_ViewRect* rect) {
  if (!rect) return Steinberg_kInvalidArgument;
  // The same clamp CLAP's adjust_size uses. It was written out twice with
  // the numbers repeated, which is how the state restore ended up with a
  // third set that disagreed.
  uint32_t w = (uint32_t) (rect->right - rect->left);
  uint32_t h = (uint32_t) (rect->bottom - rect->top);
  clapwrap::clampEditorSize(&w, &h);
  rect->right = rect->left + (Steinberg_int32) w;
  rect->bottom = rect->top + (Steinberg_int32) h;
  return Steinberg_kResultTrue;
}

/** The scale interface's own three FUnknown slots forward to the view, so a
 *  host that releases through this pointer releases the view that owns it. */
static Steinberg_tresult SMTG_STDMETHODCALLTYPE viewScaleQueryInterface(void* self,
                                                                        const Steinberg_TUID iid,
                                                                        void** obj) {
  auto* iface = (ViewScaleIface*) self;
  return viewQueryInterface(&iface->owner->iface, iid, obj);
}
static Steinberg_uint32 SMTG_STDMETHODCALLTYPE viewScaleAddRef(void* self) {
  auto* iface = (ViewScaleIface*) self;
  return ++iface->owner->refs;
}


static Steinberg_uint32 SMTG_STDMETHODCALLTYPE viewScaleRelease(void* self);

inline Steinberg_IPlugViewContentScaleSupportVtbl& scaleVtbl() {
  static Steinberg_IPlugViewContentScaleSupportVtbl v = {
      viewScaleQueryInterface, viewScaleAddRef, viewScaleRelease, viewSetContentScaleFactor,
  };
  return v;
}

inline Steinberg_IPlugViewVtbl& viewVtbl() {
  static Steinberg_IPlugViewVtbl v = {
      viewQueryInterface, viewAddRef, viewRelease, viewIsPlatformTypeSupported,
      viewAttached, viewRemoved, viewOnWheel, viewOnKeyDown, viewOnKeyUp,
      viewGetSize, viewOnSize, viewOnFocus, viewSetFrame, viewCanResize,
      viewCheckSizeConstraint,
  };
  return v;
}

static Steinberg_uint32 SMTG_STDMETHODCALLTYPE viewScaleRelease(void* self) {
  auto* iface = (ViewScaleIface*) self;
  return viewRelease(&iface->owner->iface);
}

inline void destroyView(View* view) {
  if (view->plugin && view->plugin->view == view) view->plugin->view = nullptr;
  view->~View();
  std::free(view);
}

Steinberg_IPlugView* SMTG_STDMETHODCALLTYPE controllerCreateView(void* self,
                                                                 Steinberg_FIDString name) {
  if (name && std::strcmp(name, "editor") != 0) return nullptr;
  Plugin* plugin = ownerOfController(self);
  if (plugin->view) { // one editor per instance; hand back the existing one
    ++plugin->view->refs;
    return (Steinberg_IPlugView*) &plugin->view->iface;
  }
  void* memory = std::malloc(sizeof(View));
  if (!memory) return nullptr;
  View* view = new (memory) View();
  view->iface.lpVtbl = &viewVtbl();
  view->iface.owner = view;
  view->scaleIface.lpVtbl = &scaleVtbl();
  view->scaleIface.owner = view;
  view->plugin = plugin;
  view->width = plugin->shared.guiWidth;
  view->height = plugin->shared.guiHeight;
  plugin->view = view;
  return (Steinberg_IPlugView*) &view->iface;
}

// ── The class factory ────────────────────────────────────────────────────────

/** The well-known category string every VST3 audio plugin declares. It lives in
 *  the C++ SDK rather than the C header, so it is spelled out here. */
inline const char* audioModuleCategory() { return "Audio Module Class"; }

/** VST3 categorises plugins by a subcategory string; hosts sort their browsers
 *  by it, so an instrument that says "Fx" lands in the wrong folder forever. */
inline const char* subCategories() { return kDesc.isInstrument ? "Instrument|Synth" : "Fx"; }

struct Factory {
  Steinberg_IPluginFactory2Vtbl* lpVtbl = nullptr;
  Steinberg_uint32 refs = 1;
};

inline Factory& factory();

static Steinberg_tresult SMTG_STDMETHODCALLTYPE factoryQueryInterface(void* self,
                                                                      const Steinberg_TUID iid,
                                                                      void** obj) {
  if (!obj) return Steinberg_kInvalidArgument;
  if (sameUid(iid, Steinberg_FUnknown_iid) || sameUid(iid, Steinberg_IPluginFactory_iid) ||
      sameUid(iid, Steinberg_IPluginFactory2_iid)) {
    *obj = self;
    ++factory().refs;
    return Steinberg_kResultOk;
  }
  *obj = nullptr;
  return Steinberg_kNoInterface;
}

// The factory is a process-wide singleton, so its refcount is bookkeeping the
// host can watch but never a reason to free anything.
static Steinberg_uint32 SMTG_STDMETHODCALLTYPE factoryAddRef(void*) { return ++factory().refs; }
static Steinberg_uint32 SMTG_STDMETHODCALLTYPE factoryRelease(void*) {
  Factory& f = factory();
  return f.refs > 0 ? --f.refs : 0;
}

static Steinberg_tresult SMTG_STDMETHODCALLTYPE factoryGetFactoryInfo(
    void*, Steinberg_PFactoryInfo* info) {
  if (!info) return Steinberg_kInvalidArgument;
  std::memset(info, 0, sizeof(*info));
  std::snprintf(info->vendor, sizeof(info->vendor), "%s", kDesc.vendor);
  std::snprintf(info->url, sizeof(info->url), "%s", kDesc.url ? kDesc.url : "");
  std::snprintf(info->email, sizeof(info->email), "%s", "");
  info->flags = 0x10; // kUnicode
  return Steinberg_kResultOk;
}

static Steinberg_int32 SMTG_STDMETHODCALLTYPE factoryCountClasses(void*) { return 1; }

static Steinberg_tresult SMTG_STDMETHODCALLTYPE factoryGetClassInfo(void*, Steinberg_int32 index,
                                                                    Steinberg_PClassInfo* info) {
  if (!info || index != 0) return Steinberg_kInvalidArgument;
  std::memset(info, 0, sizeof(*info));
  std::memcpy(info->cid, componentUid(), sizeof(Steinberg_TUID));
  info->cardinality = 0x7FFFFFFF; // kManyInstances
  std::snprintf(info->category, sizeof(info->category), "%s", audioModuleCategory());
  std::snprintf(info->name, sizeof(info->name), "%s", kDesc.name);
  return Steinberg_kResultOk;
}

static Steinberg_tresult SMTG_STDMETHODCALLTYPE factoryCreateInstance(void*,
                                                                      Steinberg_FIDString cid,
                                                                      Steinberg_FIDString iid,
                                                                      void** obj) {
  if (!obj || !cid || !iid) return Steinberg_kInvalidArgument;
  *obj = nullptr;
  if (std::memcmp(cid, componentUid(), sizeof(Steinberg_TUID)) != 0) return Steinberg_kNoInterface;
  Plugin* plugin = createPlugin();
  if (!plugin) return Steinberg_kResultFalse;
  // The host asks for one specific interface; queryPlugin adds the reference it
  // hands back, so the extra one from creation is released here.
  void* iface = nullptr;
  const Steinberg_tresult r = queryPlugin(plugin, (const Steinberg_int8*) iid, &iface);
  releasePlugin(plugin);
  if (r != Steinberg_kResultOk) return r;
  *obj = iface;
  return Steinberg_kResultOk;
}

static Steinberg_tresult SMTG_STDMETHODCALLTYPE factoryGetClassInfo2(void*, Steinberg_int32 index,
                                                                     Steinberg_PClassInfo2* info) {
  if (!info || index != 0) return Steinberg_kInvalidArgument;
  std::memset(info, 0, sizeof(*info));
  std::memcpy(info->cid, componentUid(), sizeof(Steinberg_TUID));
  info->cardinality = 0x7FFFFFFF;
  std::snprintf(info->category, sizeof(info->category), "%s", audioModuleCategory());
  std::snprintf(info->name, sizeof(info->name), "%s", kDesc.name);
  info->classFlags = 0;
  std::snprintf(info->subCategories, sizeof(info->subCategories), "%s", subCategories());
  std::snprintf(info->vendor, sizeof(info->vendor), "%s", kDesc.vendor);
  std::snprintf(info->version, sizeof(info->version), "%s", kDesc.version);
  std::snprintf(info->sdkVersion, sizeof(info->sdkVersion), "%s", "VST 3.7.0");
  return Steinberg_kResultOk;
}

inline Steinberg_IPluginFactory2Vtbl& factoryVtbl() {
  static Steinberg_IPluginFactory2Vtbl v = {
      factoryQueryInterface, factoryAddRef, factoryRelease, factoryGetFactoryInfo,
      factoryCountClasses, factoryGetClassInfo, factoryCreateInstance, factoryGetClassInfo2,
  };
  return v;
}

inline Factory& factory() {
  static Factory f = [] {
    Factory made;
    made.lpVtbl = &factoryVtbl();
    return made;
  }();
  return f;
}

} // namespace vst3
} // namespace sonore

// ── Module entry points ──────────────────────────────────────────────────────
// Every platform wants its own pair of init/exit symbols alongside the factory.
// A missing one is not a soft failure: the host simply refuses to load the
// module, usually without saying why.

#if defined(_WIN32)
#define SONORE_VST3_EXPORT extern "C" __declspec(dllexport)
#else
#define SONORE_VST3_EXPORT extern "C" __attribute__((visibility("default")))
#endif

SONORE_VST3_EXPORT Steinberg_IPluginFactory* GetPluginFactory() {
  sonore::vst3::Factory& f = sonore::vst3::factory();
  return (Steinberg_IPluginFactory*) &f;
}

#if defined(_WIN32)
SONORE_VST3_EXPORT bool InitDll() { return true; }
SONORE_VST3_EXPORT bool ExitDll() { return true; }
#elif defined(__APPLE__)
SONORE_VST3_EXPORT bool bundleEntry(void*) { return true; }
SONORE_VST3_EXPORT bool bundleExit() { return true; }
#else
SONORE_VST3_EXPORT bool ModuleEntry(void*) { return true; }
SONORE_VST3_EXPORT bool ModuleExit() { return true; }
#endif
