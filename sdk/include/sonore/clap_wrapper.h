// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: the CLAP format wrapper.
//
// One header that turns a `SonoreDsp` + a `PluginDescriptor` into a complete
// CLAP plugin: factory, entry point, params, audio/note ports, state, latency,
// and sample-accurate event handling. Include it exactly once in a translation
// unit that has already defined the DSP and the descriptor -- through
// formats.h, which adds every other format the build asks for:
//
//   #define SONORE_NUM_PARAMS 4
//   #include <sonore/dsp.h>
//   struct SonoreDsp { ... };                       // the generated DSP
//   #include <sonore/plugin.h>
//   static const sonore::ParamInfo kParams[SONORE_NUM_PARAMS] = { ... };
//   static const sonore::PluginDescriptor kDesc = { ... };  // named kDesc
//   #include <sonore/formats.h>                      // defines clap_entry
//
// The descriptor's name is fixed: the wrappers read `kDesc`. SONORE_NUM_PARAMS
// must match the table it points at.
// Why CLAP is the native format rather than a shim over something else: it is
// MIT, it is a C ABI (no framework runtime to link, no licence to negotiate),
// and its process() model (planar float buffers plus a sorted event list) is
// already the shape our ABI has used since the wasm preview. The mapping is
// nearly one-to-one, which is what keeps preview and shipped binary identical.
//
// Real-time discipline: process() allocates nothing, takes no locks and does no
// IO. Parameter values cross threads as relaxed atomics (SharedParam), written
// on the audio thread from events and read by the main thread for get_value,
// and snapshotted once per block so the DSP sees one consistent set -- which
// is what lets the ThreadSanitizer leg of verify:sanitize be a gate. (This
// once described a plain float array and accepted torn reads; it stopped being
// true when the parameters became atomics.)
#pragma once

#include <clap/clap.h>
#include <clap/ext/posix-fd-support.h>
#include <clap/ext/surround.h>
#include <clap/ext/timer-support.h>
#include <clap/factory/preset-discovery.h>
#include <atomic>
#include <cmath>
#include <cstring>
#include <vector>
#include <chrono>
#include <cstdlib>
#include <new>
#include <type_traits>

#include "denormals.h"
#include "load.h"
#include "audio.h"
#include "output_guard.h"
#include "plugin.h"
#include "state_bag.h"
#include "presets.h"
#include "user_presets.h"
#include "gui.h"

// Sonorie licensing. A marketplace build defines BOTH of these; a personal
// export defines neither and every line below compiles out, so an unlicensed
// project is byte-identical to one built before licensing existed.
#if defined(SONORE_LICENSE_PRODUCT) && defined(SONORE_LICENSE_PUBKEY)
  #define SONORE_LICENSED_BUILD 1
  #include "license_runtime.h"
#endif
#include "file_dialog.h"

#include "gfx/native_editor.h"
#include "scope_buffer.h"

#if defined(_WIN32)
#include "webview_win32.h"
#elif defined(__APPLE__)
#include "webview_cocoa.h"
#elif defined(__linux__)
#include "webview_gtk.h"
#endif

#ifndef SONORE_NUM_PARAMS
#error "Define SONORE_NUM_PARAMS (and struct SonoreDsp) before including clap_wrapper.h"
#endif

// The plugin's face. A project that defines SONORE_UI_HTML to its own page
// gets it in a webview; without one the plugin gets the SDK-drawn native
// editor (EditorKind::Auto, plugin.h), or the generated HTML panel below when
// it asks for a webview anyway.
#ifndef SONORE_UI_WIDTH
#define SONORE_UI_WIDTH 700
#endif
#ifndef SONORE_UI_HEIGHT
#define SONORE_UI_HEIGHT 420
#endif

namespace sonore {
namespace clapwrap {

// One alias per platform, so everything below this line is written once. Only
// set_parent still branches: the handle a host hands over is genuinely a
// different type on each OS.
#if defined(_WIN32)
using PlatformWebView = win32::WebViewHost;
#define SONORE_HAS_WEBVIEW_BACKEND 1
#elif defined(__APPLE__)
using PlatformWebView = cocoa::WebViewHost;
#define SONORE_HAS_WEBVIEW_BACKEND 1
#elif defined(__linux__)
using PlatformWebView = gtk::WebViewHost;
#define SONORE_HAS_WEBVIEW_BACKEND 1
#endif

// ── Fixed capacities ─────────────────────────────────────────────────────────
// Bounded so every per-block pointer table stays on the stack: 8 channels
// covers 7.1, and past that the formats disagree about meaning anyway.
constexpr uint32_t kMaxAudioChannels = 8;
constexpr uint32_t kMaxAuxOutputs = 32;

// ── Does this DSP consume MIDI? ──────────────────────────────────────────────
// Same compile-time detection the wasm ABI uses: a 3-arg process() means an
// instrument. The dispatch lives in a template so `if constexpr` genuinely
// discards the untaken branch: in a plain function both branches would still
// have to type-check, which is exactly the mismatch this exists to avoid.
/** Whether T is an instrument: true when it declares the three-argument
 *  `process(AudioBlock<float>&, const float* params, MidiBuffer&)`. Every
 *  wrapper then gives it a MIDI input and hands it the block's events. */
template <typename T, typename = void>
struct TakesMidi : std::false_type {};
/** The match for TakesMidi: T declares the MIDI-taking process(). */
template <typename T>
struct TakesMidi<T, std::void_t<decltype(std::declval<T&>().process(
                        std::declval<AudioBlock<float>&>(),
                        static_cast<const float*>(nullptr),
                        std::declval<MidiBuffer&>()))>> : std::true_type {};

template <typename T>
inline void runDsp(T& dsp, AudioBlock<float>& block, const float* params, MidiBuffer& midi) {
  // Flush-to-zero for this block. Measured at 2.82x on a filter cascade
  // decaying through silence -- see denormals.h. Every wrapper reaches the
  // DSP through one of these functions, so this is the whole surface.
  const ScopedNoDenormals noDenormals;
  if constexpr (TakesMidi<T>::value) {
    dsp.process(block, params, midi);
  } else {
    (void) midi;
    dsp.process(block, params);
  }
}

// ── Does this DSP consume a sidechain? ───────────────────────────────────────
// A compressor keyed from another track declares
//   void process(AudioBlock<float>& main, AudioBlock<float>& sidechain,
//                const float* params)
// and every wrapper grows a second stereo input bus for it. The sidechain
// block is READ-ONLY by contract; when the host never routed one, the wrapper
// feeds silence, so the DSP needs no null checks. Detected like MIDI: by the
// signature, so nothing that doesn't care pays for it.
/** Whether T reads a sidechain: true when it declares
 *  `process(AudioBlock<float>& main, AudioBlock<float>& sidechain, const float*)`.
 *  Every wrapper then adds a stereo sidechain input bus, and feeds silence
 *  when the host routes nothing to it. */
template <typename T, typename = void>
struct TakesSidechain : std::false_type {};
/** The match for TakesSidechain: T declares the two-block process(). */
template <typename T>
struct TakesSidechain<T, std::void_t<decltype(std::declval<T&>().process(
                            std::declval<AudioBlock<float>&>(),
                            std::declval<AudioBlock<float>&>(),
                            static_cast<const float*>(nullptr)))>> : std::true_type {};

template <typename T>
inline void runDspSc(T& dsp, AudioBlock<float>& block, AudioBlock<float>& sidechain,
                     const float* params, MidiBuffer& midi) {
  // Flush-to-zero for this block. Measured at 2.82x on a filter cascade
  // decaying through silence -- see denormals.h. Every wrapper reaches the
  // DSP through one of these functions, so this is the whole surface.
  const ScopedNoDenormals noDenormals;
  if constexpr (TakesSidechain<T>::value) {
    (void) midi;
    dsp.process(block, sidechain, params);
  } else {
    runDsp(dsp, block, params, midi);
  }
}

// ── Does this DSP take the full context? ─────────────────────────────────────
// The signature a plugin with EXTRA OUTPUT BUSES uses. It supersedes the
// simple ones: aux-outs times MIDI times sidechain would otherwise need eight
// traits, and this collapses them into one object.
/** Whether T takes the whole ProcessContext: true when it declares
 *  `process(ProcessContext&, const float* params)`. Checked before the other
 *  signatures, and the only one that is handed aux output buses. */
template <typename T, typename = void>
struct TakesContext : std::false_type {};
/** The match for TakesContext: T declares the context-taking process(). */
template <typename T>
struct TakesContext<T, std::void_t<decltype(std::declval<T&>().process(
                           std::declval<ProcessContext&>(),
                           static_cast<const float*>(nullptr)))>> : std::true_type {};

// ── Can this DSP process in DOUBLE precision? ────────────────────────────────
// A DSP written as a template over the sample type answers yes for free; one
// written in plain float answers no, and every wrapper then DECLINES the
// host's 64-bit path rather than downcasting behind its back.
/** Whether T can run a 64-bit block: true when
 *  `process(AudioBlock<double>&, const float* params)` compiles for it. The
 *  parameters stay float either way. */
template <typename T, typename = void>
struct TakesDouble : std::false_type {};
/** The match for TakesDouble: T declares the double-block process(). */
template <typename T>
struct TakesDouble<T, std::void_t<decltype(std::declval<T&>().process(
                          std::declval<AudioBlock<double>&>(),
                          static_cast<const float*>(nullptr)))>> : std::true_type {};
/** TakesDouble for the context form: true when
 *  `process(ProcessContextT<double>&, const float* params)` compiles for T.
 *  Either answer yes makes supportsDouble() true. */
template <typename T, typename = void>
struct TakesDoubleCtx : std::false_type {};
/** The match for TakesDoubleCtx: T declares the double-context process(). */
template <typename T>
struct TakesDoubleCtx<T, std::void_t<decltype(std::declval<T&>().process(
                             std::declval<ProcessContextT<double>&>(),
                             static_cast<const float*>(nullptr)))>> : std::true_type {};

inline constexpr bool supportsDouble() {
  return TakesDouble<SonoreDsp>::value || TakesDoubleCtx<SonoreDsp>::value;
}

#if defined(SONORE_LICENSED_BUILD)
/** This binary's licence, read once and shared by every instance.
 *
 *  A function-local static rather than a namespace-scope object: a plugin is a
 *  shared library, and the first thing a host does is scan it. Construction
 *  reads a file and verifies a signature, and doing that during static init --
 *  before the host has even decided to instantiate us -- is work done in the
 *  one place where failure has nowhere to go. */
inline license::Gate& licenseGate() {
  static license::Gate gate(SONORE_LICENSE_PRODUCT, SONORE_LICENSE_PUBKEY);
  return gate;
}

/** [audio-thread] Degrade the main bus while unlicensed.
 *
 *  On the MAIN bus only, deliberately: it is what a listener hears, and a demo
 *  that also mangled a splitter's aux sends would be describing a broken
 *  plugin rather than an unactivated one. */
template <typename Sample>
inline void applyLicenceGate(AudioBlock<Sample>& block) {
  license::Gate& gate = licenseGate();
  if (gate.licensed()) return;
  Sample* ptrs[kMaxAudioChannels];
  size_t n = block.getNumChannels();
  if (n > kMaxAudioChannels) n = kMaxAudioChannels;
  for (size_t i = 0; i < n; ++i) ptrs[i] = block.getChannelPointer(i);
  gate.demoBlock(ptrs, (int) n, (int) block.getNumSamples());
}
#endif

/** Run a double-precision block through whichever double signature exists. */
template <typename T>
inline void runDspCtx64(T& dsp, ProcessContextT<double>& ctx, const float* params) {
  // Flush-to-zero for this block. Measured at 2.82x on a filter cascade
  // decaying through silence -- see denormals.h. Every wrapper reaches the
  // DSP through one of these functions, so this is the whole surface.
  const ScopedNoDenormals noDenormals;
  if constexpr (TakesDoubleCtx<T>::value) {
    dsp.process(ctx, params);
  } else if constexpr (TakesDouble<T>::value) {
    dsp.process(ctx.main, params);
  } else {
    (void) dsp;
    (void) ctx;
    (void) params;
  }
#if defined(SONORE_LICENSED_BUILD)
  applyLicenceGate(ctx.main);
#endif
}

/** Does the plugin want a MIDI INPUT port?
 *
 *  The classic three-argument signature says so by itself. The context form
 *  carries a MIDI buffer too, but a context-taking EFFECT (a splitter, say)
 *  should not grow a note port it never reads -- so for that form the
 *  descriptor decides: an instrument is note-driven by definition, and a
 *  plugin that emits MIDI consumes it. */
inline bool wantsMidiIn() {
  if (TakesMidi<SonoreDsp>::value) return true;
  return TakesContext<SonoreDsp>::value && (kDesc.isInstrument || kDesc.producesMidi);
}

/** How many aux output buses this plugin really has: what the descriptor
 *  declares, but only for a DSP that can actually be handed them. */
inline uint32_t numAuxOutputs() {
  // `if constexpr` on the trait, the plain `if` on the rest: MSVC 2022 calls
  // `!Trait || x` a constant condition (C4127) in a plugin whose DSP does not
  // have the trait, and a /W4 /WX build stops on it.
  if constexpr (!TakesContext<SonoreDsp>::value) {
    return 0;
  } else {
    if (!kDesc.auxOutputs) return 0;
    const int n = kDesc.numAuxOutputs;
    return (uint32_t) (n < 0 ? 0 : (n > 32 ? 32 : n));
  }
}

inline uint32_t auxBusChannels(uint32_t index) {
  if (index >= numAuxOutputs()) return 0;
  const int n = kDesc.auxOutputs[index].channels;
  return (uint32_t) (n < 1 ? 1 : (n > (int) kMaxAudioChannels ? (int) kMaxAudioChannels : n));
}

/** The one entry point every wrapper calls. */
template <typename T>
inline void runDspCtx(T& dsp, ProcessContext& ctx, const float* params) {
  // Flush-to-zero for this block. Measured at 2.82x on a filter cascade
  // decaying through silence -- see denormals.h. Every wrapper reaches the
  // DSP through one of these functions, so this is the whole surface.
  const ScopedNoDenormals noDenormals;
  if constexpr (TakesContext<T>::value) {
    dsp.process(ctx, params);
  } else {
    runDspSc(dsp, ctx.main, ctx.sidechain, params, ctx.midi);
  }
#if defined(SONORE_LICENSED_BUILD)
  // HERE rather than in each wrapper: this is the single function every format
  // reaches the DSP through, so one line protects CLAP, VST3, AU, LV2 and the
  // standalone at once -- and no future format can be added that forgets it.
  applyLicenceGate(ctx.main);
#endif
}

// Musical time is OPTIONAL: a DSP that wants it declares
// `void setTransport(const sonore::TransportInfo&)` and the wrapper calls it
// before every block. Detected the same way as the MIDI signature, so nothing
// that doesn't care pays for it or has to be edited.
/** Whether T wants musical time: true when it declares
 *  `void setTransport(const TransportInfo&)`, which the wrapper then calls on
 *  the audio thread before every block. */
template <typename T, typename = void>
struct WantsTransport : std::false_type {};
/** The match for WantsTransport: T declares setTransport(). */
template <typename T>
struct WantsTransport<T, std::void_t<decltype(std::declval<T&>().setTransport(
                             std::declval<const TransportInfo&>()))>> : std::true_type {};

template <typename T>
inline void sendTransport(T& dsp, const TransportInfo& info) {
  if constexpr (WantsTransport<T>::value) dsp.setTransport(info);
  else (void) info, (void) dsp;
}

/** A DSP that wants to know which host it is in declares
 *  `void setHostInfo(const HostInfo&)`. Detected like every other optional
 *  hook, so a DSP that does not care neither declares nor pays for it. */
template <typename T, typename = void>
struct WantsHostInfo : std::false_type {};
/** The match for WantsHostInfo: T declares setHostInfo(). */
template <typename T>
struct WantsHostInfo<
    T, std::void_t<decltype(std::declval<T&>().setHostInfo(std::declval<const HostInfo&>()))>>
    : std::true_type {};

/** Told ONCE, when the plugin is created. A host does not change identity
 *  mid-session, and telling a DSP every block something that cannot change
 *  would be a per-block cost for a constant. An Audio Unit is told when the
 *  host names itself through kAudioUnitProperty_AUHostIdentifier, early and
 *  on the main thread, and not at all by a host that never sets it. */
template <typename T>
inline void sendHostInfo(T& dsp, const HostInfo& info) {
  if constexpr (WantsHostInfo<T>::value) dsp.setHostInfo(info);
  else (void) info, (void) dsp;
}

/** A DSP that wants to know about the track it was dropped onto declares
 *  `void setTrackInfo(const TrackInfo&)`. Unlike the host's identity this one
 *  CHANGES -- a user renames a track or recolours it mid-session -- so it is
 *  a notification, not a one-off. */
template <typename T, typename = void>
struct WantsTrackInfo : std::false_type {};
/** The match for WantsTrackInfo: T declares setTrackInfo(). */
template <typename T>
struct WantsTrackInfo<
    T, std::void_t<decltype(std::declval<T&>().setTrackInfo(std::declval<const TrackInfo&>()))>>
    : std::true_type {};

template <typename T>
inline void sendTrackInfo(T& dsp, const TrackInfo& info) {
  if constexpr (WantsTrackInfo<T>::value) dsp.setTrackInfo(info);
  else (void) info, (void) dsp;
}

/**
 * A DSP that can be handed a FILE declares
 * `void loadFile(const char* purpose, const char* path)`.
 *
 * [main-thread], and that is the entire contract worth reading twice. It is
 * called while process() may be running on the audio thread, because a modal
 * dialog cannot block audio and a file cannot be read on the audio thread.
 * So the DSP OWNS the handover: read the file here, then publish it to the
 * audio side by swapping an atomic pointer or an index, the way
 * SampleStreamer already does. Writing straight into state that process()
 * reads is a race, and it is a race that sounds like a click on somebody
 * else's machine.
 *
 * `purpose` is whatever the page asked with, so a plugin with a sample slot
 * and an impulse-response slot can tell them apart. An empty `path` means the
 * user cancelled and is delivered too -- a plugin that wants to clear a slot
 * on cancel can, and one that does not simply returns.
 */
template <typename T, typename = void>
struct WantsFile : std::false_type {};
/** The match for WantsFile: T declares loadFile(purpose, path). */
template <typename T>
struct WantsFile<T, std::void_t<decltype(std::declval<T&>().loadFile(
                        std::declval<const char*>(), std::declval<const char*>()))>>
    : std::true_type {};

template <typename T>
inline void sendFile(T& dsp, const char* purpose, const char* path) {
  if constexpr (WantsFile<T>::value) dsp.loadFile(purpose, path);
  else (void) dsp, (void) purpose, (void) path;
}

/** Translate the host's transport event into ours. */
inline TransportInfo readTransport(const clap_event_transport_t* t) {
  TransportInfo info;
  if (!t) return info; // free-running host: the defaults are the honest answer
  info.isPlaying = (t->flags & CLAP_TRANSPORT_IS_PLAYING) != 0;
  info.isRecording = (t->flags & CLAP_TRANSPORT_IS_RECORDING) != 0;
  info.isLooping = (t->flags & CLAP_TRANSPORT_IS_LOOP_ACTIVE) != 0;
  if (t->flags & CLAP_TRANSPORT_HAS_TEMPO) {
    info.tempo = t->tempo;
    info.hasTempo = true;
  }
  if (t->flags & CLAP_TRANSPORT_HAS_BEATS_TIMELINE) {
    // CLAP beat time is a fixed-point value with CLAP_BEATTIME_FACTOR ticks
    // per quarter note: using it raw would be off by a factor of 2^31.
    info.positionBeats = (double) t->song_pos_beats / (double) CLAP_BEATTIME_FACTOR;
    info.barStartBeats = (double) t->bar_start / (double) CLAP_BEATTIME_FACTOR;
    info.barNumber = t->bar_number;
    info.hasBeats = true;
  }
  if (t->flags & CLAP_TRANSPORT_HAS_SECONDS_TIMELINE) {
    info.positionSeconds = (double) t->song_pos_seconds / (double) CLAP_SECTIME_FACTOR;
    info.hasSeconds = true;
  }
  if (t->flags & CLAP_TRANSPORT_HAS_TIME_SIGNATURE) {
    info.timeSigNumerator = t->tsig_num;
    info.timeSigDenominator = t->tsig_denom;
  }
  // The loop, which the host sends in the same beat time as the position.
  if ((t->flags & CLAP_TRANSPORT_IS_LOOP_ACTIVE) && (t->flags & CLAP_TRANSPORT_HAS_BEATS_TIMELINE)) {
    info.loopStartBeats = (double) t->loop_start_beats / (double) CLAP_BEATTIME_FACTOR;
    info.loopEndBeats = (double) t->loop_end_beats / (double) CLAP_BEATTIME_FACTOR;
    info.hasLoop = info.loopEndBeats > info.loopStartBeats;
  }
  return info;
}

/** The persisted state blob. Versioned from day one: a plugin that ships and
 *  then gains a parameter must still load its old sessions. */
struct StateHeader {
  char magic[4];      // 'S','N','R','S'
  uint32_t version;   // kStateVersion
  uint32_t numParams; // params that follow, as float32
};
// v1: header + values.
// v2: appends one byte, the host bypass flag.
// v3: appends a StateBag, the key/value state a plugin has beyond its
//     parameters, which is the only way a sampler can remember WHICH FILE it
//     loaded. Loaders accept all three, and an older blob simply leaves the
//     newer fields at their defaults, so no saved session is ever orphaned.
// v4: the selected factory preset. v5: the editor's size.
// v6: the preset the user is on (user_presets.h) -- factory or their own,
//     its name, and a hash of its sound -- so a project reopens showing the
//     preset it was saved on, marked modified if it was.
constexpr uint32_t kStateVersion = 6;

/**
 * What a state restore is FOR, which decides the fields that are the
 * session's rather than the sound's.
 *
 * Session: a host restoring a project -- everything in the blob applies.
 * Preset:  a preset the user picked -- the blob's sound applies, and the
 *          session keeps its bypass, its editor size and its selection (the
 *          preset manager sets that afterwards). A preset saved while
 *          bypassed must not bypass the track it is loaded on, and one saved
 *          from a small window must not resize this one.
 * Restore: an earlier state of THIS session coming back -- an undo. As
 *          Preset, but the selection comes back with the sound.
 */
enum class StateScope { Session, Preset, Restore };

/** The longest preset name or file name the v6 field carries. */
constexpr uint32_t kMaxStatePresetText = 4096;

/**
 * Bring a size inside what THIS plugin declared. The one place that decides.
 *
 * The numbers were written three times before this and differently: 320x200 in
 * CLAP's adjust_size, 320x200 again in VST3's checkSizeConstraint, and 120x120
 * in the state restore that reads a saved size back -- so a session could
 * restore an editor smaller than the plugin's own minimum, and nothing on the
 * resize path would ever have allowed it. Unifying them here fixed that.
 *
 * What it did NOT fix is that the one surviving answer was still invented. It
 * was 320x200 to 8192x8192, freely resizable, no aspect ratio, for EVERY
 * plugin -- so a synth with a five-octave keyboard along the bottom could be
 * dragged to 320 pixels wide by any host, and a fixed-proportion skin could be
 * stretched to any shape. The plugin had no way to say otherwise.
 *
 * It does now: PluginDescriptor::editorLimits, whose defaults are exactly the
 * constants that used to be here. The rules themselves live in plugin.h as a
 * pure function, so each one is a test rather than something checked by
 * dragging a window in a host and looking at it.
 */
inline void clampEditorSize(uint32_t* width, uint32_t* height) {
  applyEditorConstraints(kDesc.editorLimits, SONORE_UI_WIDTH, SONORE_UI_HEIGHT, width, height);
}

// ── State a DSP owns beyond its parameters ───────────────────────────────────
//
// Opt-in, detected the same way as every other capability in this SDK: a DSP
// that declares saveState/loadState gets its bag serialised with the session,
// and one that does not pays nothing.
/** Does the DSP say how many voices it has? A VoiceManager knows its own
 *  polyphony; a monophonic DSP has nothing to say and says nothing. */
template <typename T, typename = void>
struct HasVoiceCapacity : std::false_type {};
/** The match for HasVoiceCapacity: T declares `voiceCapacity() const`. */
template <typename T>
struct HasVoiceCapacity<T, std::void_t<decltype(std::declval<const T&>().voiceCapacity())>>
    : std::true_type {};

/** …how many of them the patch may use now: a Voices control's setting, at
 *  most the capacity. This is CLAP's voice_count; without it, the capacity
 *  is the count. */
template <typename T, typename = void>
struct HasVoiceCount : std::false_type {};
/** The match for HasVoiceCount: T declares `voiceCount() const`. */
template <typename T>
struct HasVoiceCount<T, std::void_t<decltype(std::declval<const T&>().voiceCount())>>
    : std::true_type {};

/** …and how many are sounding right now. Detected, and NOT what CLAP's
 *  voice_count asks for -- that is the voices the patch can use, never the
 *  ones in use (see fillVoiceInfo) -- so no format reports it. */
template <typename T, typename = void>
struct HasActiveVoices : std::false_type {};
/** The match for HasActiveVoices: T declares `activeVoices() const`. */
template <typename T>
struct HasActiveVoices<T, std::void_t<decltype(std::declval<const T&>().activeVoices())>>
    : std::true_type {};

/** Does the DSP have a pitch-bend range of its own -- a "bend range" control
 *  -- for MIDI that is not MPE? Optional:
 *  `float pitchBendRange(const float* params) const`, in semitones, given the
 *  values this block's process() will be handed, so a control answers it for
 *  the block it moved in. The wrapper's MPE decoder turns a plain keyboard's
 *  wheel into each note's tuning at this range (two semitones when the DSP
 *  has none). */
template <typename T, typename = void>
struct HasPitchBendRange : std::false_type {};
/** The match for HasPitchBendRange: T declares
 *  `pitchBendRange(const float* params) const`. */
template <typename T>
struct HasPitchBendRange<
    T, std::void_t<decltype(std::declval<const T&>().pitchBendRange((const float*) nullptr))>>
    : std::true_type {};

/** [audio-thread] The DSP's bend range, to the decoder, before it reads the
 *  block's MIDI, from the block's own parameter values. The formats reach it
 *  through decodeMpe(); the standalone, which runs a DSP outside an
 *  Instance's process path, calls it itself. */
/** A DSP still declaring the hook's first form, `pitchBendRange() const`. */
template <typename T, typename = void>
struct HasOldPitchBendRange : std::false_type {};
/** The match for HasOldPitchBendRange. */
template <typename T>
struct HasOldPitchBendRange<T, std::void_t<decltype(std::declval<const T&>().pitchBendRange())>>
    : std::true_type {};

template <typename T>
inline void syncBendRange(const T& dsp, MpeDecoder& mpe, const float* params) {
  static_assert(!HasOldPitchBendRange<T>::value || HasPitchBendRange<T>::value,
                "pitchBendRange() takes the block's parameter values now: declare "
                "float pitchBendRange(const float* params) const");
  if constexpr (HasPitchBendRange<T>::value) mpe.setLegacyBendRange((float) dsp.pitchBendRange(params));
  else {
    (void) dsp;
    (void) mpe;
    (void) params;
  }
}

/** A DSP's own reset(): start over -- forget what is sounding, keep what
 *  prepare() sized. Optional; see resetDsp(). */
template <typename T, typename = void>
struct HasReset : std::false_type {};
/** The match for HasReset: T declares reset(). */
template <typename T>
struct HasReset<T, std::void_t<decltype(std::declval<T&>().reset())>> : std::true_type {};

/** Whether T keeps state beyond its parameters: true when it declares
 *  `void saveState(StateBag&) const`. Such a DSP must also declare
 *  `void loadState(const StateBag&)`; the bag is then saved with every session
 *  and preset, and both are called on the main thread. The const matters: a
 *  saveState without it is not seen, which the hook checks below turn into a
 *  compile error. */
template <typename T, typename = void>
struct HasStateBag : std::false_type {};
/** The match for HasStateBag: T declares `saveState(StateBag&) const`. */
template <typename T>
struct HasStateBag<T, std::void_t<decltype(std::declval<const T&>().saveState(
                          std::declval<StateBag&>()))>> : std::true_type {};

template <typename T>
inline void saveDspState(const T& dsp, StateBag& bag) {
  if constexpr (HasStateBag<T>::value) {
    dsp.saveState(bag);
  } else {
    (void) dsp;
    (void) bag;
  }
}

template <typename T>
inline void loadDspState(T& dsp, const StateBag& bag) {
  if constexpr (HasStateBag<T>::value) {
    dsp.loadState(bag);
  } else {
    (void) dsp;
    (void) bag;
  }
}

// ── Main-bus channel range ───────────────────────────────────────────────────
//
// The descriptor declares what the DSP honestly handles; everything below
// negotiates within it. The hard cap of 8 covers 7.1 -- past that the formats
// disagree about meaning anyway.

inline uint32_t minMainChannels() {
  const int n = kDesc.minChannels;
  return (uint32_t) (n < 1 ? 1 : (n > (int) kMaxAudioChannels ? (int) kMaxAudioChannels : n));
}
inline uint32_t maxMainChannels() {
  const int n = kDesc.maxChannels;
  const uint32_t lo = minMainChannels();
  uint32_t hi = (uint32_t) (n < 1 ? 1 : (n > (int) kMaxAudioChannels ? (int) kMaxAudioChannels : n));
  return hi < lo ? lo : hi;
}
inline bool channelCountAllowed(uint32_t n) {
  return n >= minMainChannels() && n <= maxMainChannels();
}
/** What a host gets before it negotiates anything: stereo when the range
 *  allows it, else the nearest edge (a mono-only DSP defaults to mono). */
inline uint32_t defaultMainChannels() {
  return channelCountAllowed(2) ? 2u : (2u < minMainChannels() ? minMainChannels()
                                                               : maxMainChannels());
}
inline const char* channelLayoutName(uint32_t n) {
  switch (n) {
    case 1: return "Mono";
    case 2: return "Stereo";
    case 3: return "3.0";
    case 4: return "Quad";
    case 5: return "5.0";
    case 6: return "5.1";
    case 7: return "6.1";
    case 8: return "7.1";
    default: return "Custom";
  }
}

/**
 * A value one thread writes and another reads.
 *
 * Parameters are set by the host on the main thread (a knob, a preset, a
 * session load) and read by the audio thread every block. Two threads on one
 * plain float is a data race by the language, however benign the hardware
 * makes it, and it is the first thing ThreadSanitizer names. These wrap a
 * relaxed atomic behind the conversions a plain float had, so the forty
 * sites that assign or read one did not change -- a relaxed 32-bit load or
 * store is an ordinary move on every CPU this ships on -- and the audio
 * thread takes ONE snapshot per block (snapshotParams), which also gives the
 * DSP a parameter set that cannot change between two samples of one block.
 */
struct SharedParam {
  std::atomic<float> v{0.0f};
  SharedParam() = default;
  SharedParam(const SharedParam& o) : v(o.v.load(std::memory_order_relaxed)) {}
  SharedParam& operator=(const SharedParam& o) {
    v.store(o.v.load(std::memory_order_relaxed), std::memory_order_relaxed);
    return *this;
  }
  SharedParam& operator=(float x) {
    v.store(x, std::memory_order_relaxed);
    return *this;
  }
  operator float() const { return v.load(std::memory_order_relaxed); }
};
static_assert(std::atomic<float>::is_always_lock_free, "a parameter must never take a lock");

/** SharedParam for a flag: a relaxed atomic bool that reads and assigns like
 *  a plain one, and copies its value the way SharedParam does. For a fact one
 *  thread sets and another polls (bypass engaged, offline render, state
 *  dirty); relaxed means it orders nothing else, so it must not be used to
 *  publish other data. */
struct SharedFlag {
  std::atomic<bool> v{false};
  SharedFlag() = default;
  SharedFlag(const SharedFlag& o) : v(o.v.load(std::memory_order_relaxed)) {}
  SharedFlag& operator=(const SharedFlag& o) {
    v.store(o.v.load(std::memory_order_relaxed), std::memory_order_relaxed);
    return *this;
  }
  SharedFlag& operator=(bool x) {
    v.store(x, std::memory_order_relaxed);
    return *this;
  }
  operator bool() const { return v.load(std::memory_order_relaxed); }
};

/** Host-driven bypass with a click-free crossfade and a latency-aligned dry
 *  path. VST3 (kIsBypass parameter), AU (BypassEffect property) and LV2
 *  (lv2:enabled port) all drive this; CLAP hosts bypass plugins themselves,
 *  so the CLAP path never engages it. The dry signal is delayed by the DSP's
 *  reported latency -- mixing an undelayed dry against latency-delayed wet
 *  during the ramp would comb-filter every bypass toggle.
 *
 *  `engaged` may be set from any thread; everything else is the audio
 *  thread's. prepare() allocates the dry ring and runs at activation, on the
 *  main thread; bypassCapture() and bypassApply() then run on every block. */
struct BypassState {
  SharedFlag engaged;   // what the host asked for, from whichever thread asked
  float mix = 0.0f;     // crossfade position: 0 = processing, 1 = fully dry
  float step = 0.001f;  // per-sample ramp increment (~20 ms)
  uint32_t delay = 0;   // dry-path delay = reported DSP latency
  uint32_t pos = 0;     // ring write cursor
  uint32_t channels = 2;
  uint32_t maxFrames = 1; // the most a block brings
  /** What the ring stores. Double for a DSP with a 64-bit path, so the SAME
   *  ring serves both precisions exactly: every float is representable as a
   *  double and converts back unchanged, so a bypassed 64-bit path is
   *  bit-exact rather than quietly rounded. Float for every other DSP, which
   *  only ever hands this ring floats: they are stored exactly in half the
   *  memory -- at LV2's 8192-frame slice plus a 192 kHz latency room, 219 KB
   *  of an instance's 513. The crossfade still reads them as double (see
   *  bypassApply), so the mix it computes is the one it always computed. */
  using RingSample = std::conditional_t<supportsDouble(), double, float>;
  std::vector<RingSample> ring[kMaxAudioChannels];

  /** `room` is how far the delay may grow while running (see follow()); 0
   *  for a format whose host re-activates when the latency moves. */
  void prepare(double sampleRate, uint32_t maxFramesPerBlock, uint32_t latencySamples,
               uint32_t numChannels = 2, uint32_t room = 0) {
    delay = latencySamples;
    channels = numChannels < 1 ? 1
               : numChannels > kMaxAudioChannels ? kMaxAudioChannels
                                                 : numChannels;
    step = sampleRate > 0.0 ? (float) (1.0 / (0.020 * sampleRate)) : 0.05f;
    maxFrames = maxFramesPerBlock > 0 ? maxFramesPerBlock : 1;
    // None for an instrument. It has no dry signal: its bypass fades the
    // output to silence and never reads the ring (bypassApply), which was
    // still sized for it -- 64 KB of an LV2 synth's 151, 32 KB at a CLAP
    // host's 4096-frame block, kept for nothing.
    const size_t n =
        kDesc.isInstrument ? 0 : (size_t) (delay > room ? delay : room) + (size_t) maxFrames;
    for (uint32_t c = 0; c < kMaxAudioChannels; ++c)
      if (c < channels && n > 0) ring[c].assign(n, RingSample());
      else ring[c].clear();
    pos = 0;
    mix = engaged ? 1.0f : 0.0f;
  }

  bool idle() const { return !engaged && mix <= 0.0f; }

  /** [audio-thread] The DSP's latency as it is now. LV2 and AU have no
   *  restart: a host reads the new latency while the plugin runs and
   *  re-compensates there and then. The dry path stayed at the latency of
   *  the last activation, so a plugin whose latency moved was bypassed out
   *  of time with everything else -- 200 samples late after a look-ahead
   *  went from 100 to 300. As far as the ring reaches, which prepare()'s
   *  `room` sets; nothing is allocated here. */
  void follow(uint32_t latencySamples) {
    const size_t size = ring[0].size();
    const uint32_t reach = size > maxFrames ? (uint32_t) (size - maxFrames) : 0;
    delay = latencySamples < reach ? latencySamples : reach;
  }

  /** The room a running format reserves: twice the latency it starts with,
   *  and never less than 100 ms. */
  static uint32_t roomFor(uint32_t latencySamples, double sampleRate) {
    const uint32_t floor = sampleRate > 0.0 ? (uint32_t) (0.1 * sampleRate) : 4800u;
    const uint32_t twice = latencySamples > 0x7FFFFFFFu ? 0xFFFFFFFFu : latencySamples * 2u;
    return twice > floor ? twice : floor;
  }
};

/** Stash the dry input BEFORE the DSP runs (the wrappers process in place).
 *  With zero latency this may be skipped while idle -- the block an engage
 *  arrives in still captures in time. With latency it runs every block, or
 *  the first `delay` samples after an engage would crossfade against stale
 *  history. */
template <typename Sample>
inline void bypassCapture(BypassState& b, Sample* const* chans, uint32_t numChannels,
                          uint32_t frames) {
  if (kDesc.isInstrument || b.ring[0].empty()) return;
  if (b.idle() && b.delay == 0) return;
  const size_t size = b.ring[0].size();
  // The channels THIS call delivered, which a host may make fewer than it
  // negotiated: walking b.channels read and wrote through the pointers past
  // them, which nobody had set.
  const uint32_t width = numChannels < b.channels ? numChannels : b.channels;
  for (uint32_t c = 0; c < width; ++c) {
    if (c > 0 && chans[c] == chans[0]) break; // mono fed through aliased pointers
    const Sample* src = chans[c];
    BypassState::RingSample* ring = b.ring[c].data();
    if (frames > size) {
      for (uint32_t i = 0; i < frames; ++i)
        ring[(b.pos + i) % size] = (BypassState::RingSample) src[i];
      continue;
    }
    // Two straight runs, up to the end of the ring and from its start: a
    // 64-bit modulo per sample per channel was 470 ns of every block of a
    // plugin that reports latency.
    const size_t first = (size_t) (b.pos % size);
    const size_t run = size - first < frames ? size - first : frames;
    for (size_t i = 0; i < run; ++i) ring[first + i] = (BypassState::RingSample) src[i];
    for (size_t i = run; i < frames; ++i) ring[i - run] = (BypassState::RingSample) src[i];
  }
}

/** One sample of the bypass crossfade: a step toward `target` that lands ON
 *  it. It stepped and then clamped to [0, 1], so at full bypass `target >
 *  mix` was false, the next step went back down, and the mix ran 1, 1 - step,
 *  1, ... for as long as the bypass stayed engaged: the processed signal
 *  leaked through on every other sample, some 60 dB down and modulated at
 *  Nyquist. */
inline float bypassStep(float mix, float target, float step) {
  if (mix < target) return mix + step < target ? mix + step : target;
  return mix - step > target ? mix - step : target;
}

/** Crossfade the delayed dry path over the wet output and advance the ring.
 *  Instruments have no dry signal, so bypass fades them to silence. Fully
 *  bypassed is a copy (or silence), not a mix with a gain of 0: an Inf or a
 *  NaN out of the DSP times 0 is still NaN. */
template <typename Sample>
inline void bypassApply(BypassState& b, Sample* const* chans, uint32_t numChannels,
                        uint32_t frames) {
  const bool haveRing = !b.ring[0].empty();
  const uint32_t width = numChannels < b.channels ? numChannels : b.channels; // see bypassCapture
  if (b.idle()) {
    if (haveRing) b.pos = (uint32_t) ((b.pos + frames) % b.ring[0].size());
    return;
  }
  const float target = b.engaged ? 1.0f : 0.0f;
  float mix = b.mix;
  if (kDesc.isInstrument || !haveRing) {
    for (uint32_t i = 0; i < frames; ++i) {
      mix = bypassStep(mix, target, b.step);
      const Sample gain = (Sample) (1.0f - mix);
      for (uint32_t c = 0; c < width; ++c) {
        if (c > 0 && chans[c] == chans[0]) break;
        chans[c][i] = mix >= 1.0f ? (Sample) 0 : chans[c][i] * gain;
      }
    }
    b.mix = mix;
    return;
  }
  const size_t size = b.ring[0].size();
  for (uint32_t i = 0; i < frames; ++i) {
    mix = bypassStep(mix, target, b.step);
    const size_t read = (b.pos + size + i - b.delay) % size;
    for (uint32_t c = 0; c < width; ++c) {
      if (c > 0 && chans[c] == chans[0]) break;
      // The dry sample read as double whatever the ring stores: the sum is
      // then evaluated in double, as it was when the ring was always double,
      // and a float ring changes the memory and not one output bit.
      const double dry = (double) b.ring[c][read];
      chans[c][i] = mix >= 1.0f ? (Sample) dry
                                : (Sample) (chans[c][i] * (1.0f - mix) + dry * mix);
    }
  }
  b.mix = mix;
  b.pos = (uint32_t) ((b.pos + frames) % size);
}

/**
 * [audio-thread] The aux buses a block hands the DSP (ProcessContext::aux),
 * kept in the instance instead of built on the stack.
 *
 * Every process path declared them per block -- kMaxAuxOutputs AudioBlocks
 * and a kMaxAuxOutputs x kMaxAudioChannels table of channel pointers, all of
 * it zeroed -- whether the plugin had an aux bus or not: 3 KB of stores, 64 ns
 * of every block, more than a quarter of what a 32-frame block cost the CLAP
 * wrapper around a DSP that does nothing. Here a bus past the plugin's own
 * count is never written, so it stays what it was on every block before, an
 * empty block with null channels; a bus it has is set up each block as it
 * was, from a row nulled first. One per sample type: the 64-bit path has its
 * own.
 */
namespace processdetail {
template <typename Sample>
struct AuxScratch {
  AudioBlock<Sample> blocks[kMaxAuxOutputs];
  Sample* ptrs[kMaxAuxOutputs][kMaxAudioChannels] = {};

  /** Bus `b`'s channel pointers, all null, for the caller to fill. */
  Sample** row(uint32_t b) {
    for (uint32_t c = 0; c < kMaxAudioChannels; ++c) ptrs[b][c] = nullptr;
    return ptrs[b];
  }
};
} // namespace processdetail

/**
 * One plugin instance as every format holds it: the DSP, its parameters, the
 * MIDI and expression buffers, bypass, latency and tail bookkeeping, the
 * editor (web or native) and the user presets.
 *
 * The CLAP wrapper allocates one per create_plugin and frees it in destroy;
 * VST3, AU, LV2 and the standalone each embed one in their own object and
 * drive it through the same functions, which is why a fix made here reaches
 * every format. A wrapper's internal type, not something a plugin constructs.
 *
 * Fields are grouped by who may touch them, and the member comments say so:
 * `params` is shared between threads through SharedParam, `paramsBlock` and
 * the MIDI buffers belong to the audio thread, the GUI and preset state to
 * the main thread.
 */
struct Instance {
  clap_plugin_t plugin{};
  const clap_host_t* host = nullptr;

  SonoreDsp dsp{};
  /** The parameters as the host last set them, readable from any thread. */
  SharedParam params[SONORE_NUM_PARAMS > 0 ? SONORE_NUM_PARAMS : 1]{};
  /** The audio thread's copy for the block in hand -- what the DSP is given.
   *  Taken at the top of every process call by snapshotParams(). */
  float paramsBlock[SONORE_NUM_PARAMS > 0 ? SONORE_NUM_PARAMS : 1]{};
  /** Odd while writeParamsTogether() is writing the whole set: a state or a
   *  preset loaded on the main thread while audio runs. */
  std::atomic<uint32_t> paramsSeq{0};
  bool paramsSnapped = false; // paramsBlock has been filled at least once
  MidiBuffer midi;
  /** Which key and channel each recent note_id was played on, so a note-off,
   *  a choke, an expression or a modulation that names only the id reaches
   *  that one note. A note takes its id's old slot, a free one, or past 64
   *  the oldest, and keeps it until the host has let it go and the DSP has
   *  reported it ended (NOTE_END): a pad held under a long run of shorter
   *  notes keeps its name, and so does a note in its release. An id no slot
   *  holds is ignored. */
  struct NoteIdSlot {
    int32_t id = -1;
    int16_t key = -1, channel = -1;
    /** The host has let the note go (NOTE_OFF or a choke). */
    bool released = false;
    /** The DSP reported the note ended (NOTE_END). A slot is freed once both
     *  have happened, in either order: a voice can fall silent under a key
     *  still down, and the host's note-off that follows may name the note by
     *  its id alone. */
    bool ended = false;
  };
  NoteIdSlot noteIds[64];
  int noteIdNext = 0;
  /** The id of each of the block's note-ons, in order, for the DSP
   *  (ProcessContext::noteIds); and the notes it reports ended, for the
   *  host (NOTE_END). */
  NoteIdBuffer noteIdsIn;
  NoteIdBuffer endedNotes;
  /** The host's modulation: per note, for the DSP (ProcessContext::
   *  paramMods); for the whole plugin, an offset per parameter the wrapper
   *  adds to what the DSP is handed. Audio thread only. */
  ParamModBuffer paramMods;
  /** The plugin-wide offset on each parameter, as the host last sent it. */
  float paramMod[SONORE_NUM_PARAMS > 0 ? SONORE_NUM_PARAMS : 1]{};
  /** paramsBlock with paramMod added, when any offset is set. */
  float paramsModulated[SONORE_NUM_PARAMS > 0 ? SONORE_NUM_PARAMS : 1]{};
  /** The DSP's voiceCount() as the last block left it, published for the
   *  main thread's voice-info answer (0: no block yet), and whether the host
   *  is still to be told it moved. */
  SharedParam voiceCountNow;
  SharedFlag voiceInfoDirty;
  /** What the DSP emitted this block, drained to the host after process. */
  MidiBuffer midiOut;
  /** Per-note expression for this block, and the decoder that fills it from
   *  MPE's raw MIDI when the host speaks that instead. */
  NoteExpressionBuffer expression;
  MpeDecoder mpe;
  /** Who is running us. Empty where the format has no way to say. */
  HostInfo hostInfo;
  TrackInfo trackInfo;
  /** Which keys are sounding. Written by the audio thread as it decodes note
   *  events, read by the editor on its own clock. */
  NoteState notes;
  /** What the page was last told, so a keyboard is only redrawn when it
   *  actually moved. */
  uint64_t notesEchoLow = 0, notesEchoHigh = 0;
  bool notesEchoValid = false;
  /** The editor has not been shown the DSP's state bag yet, or something has
   *  happened that could have changed it. Set rather than polled: rebuilding
   *  the bag thirty times a second to find out it is the same would be work
   *  for nothing, and the handful of things that CAN change it are all known
   *  to the wrapper -- a preset load, a file the user picked, a session
   *  restored by the host. */
  bool uiStateDirty = true;
#if defined(SONORE_LICENSED_BUILD)
  /** What this page currently shows: -1 nothing yet, 0 the activation
   *  overlay, 1 the plugin. Compared against the gate on every tick, so an
   *  activation performed in ANOTHER instance -- or another host -- reaches
   *  this editor without it having to be reopened. */
  int licenseUi = -1;
#endif

  double sampleRate = 48000.0;
  bool activated = false;
  /** Set when the DSP reports its own state moved, cleared when the host has
   *  been told. Not cleared on the audio thread, because mark_dirty is
   *  [main-thread] and dropping the flag before telling anyone is dropping a
   *  save prompt. */
  SharedFlag stateDirty;
  /** The output guard's count of silenced blocks and whether the main thread
   *  has said so yet (output_guard.h). Every format's process path runs it
   *  through guardOutputs(); the report is reportOutputGuard()'s. */
  detail::OutputGuard outputGuard;
  /** The tail last announced. Separate from the latency's because the two are
   *  told to the host by opposite rules -- see announceTailIfChanged. */
  uint32_t reportedTail = 0;
  bool tailReported = false;
  /** Which factory preset is currently selected, or 0.
   *
   *  Not derivable from the parameters: once a user tweaks a knob the values
   *  match no preset at all, and the name the plugin should still be showing
   *  is "Crunch (modified)" rather than the first one in the list. Saved with
   *  the state from v4, which is what stops a reopened session forgetting
   *  which preset it was on.
   *
   *  Atomic, relaxed: VST3 sets it from process() when a program change
   *  arrives in the block, while the main thread reads it to save the state
   *  and to answer the host. A plain int written on one thread and read on
   *  another is a data race, whatever x86 makes of it. */
  std::atomic<int32_t> selectedPreset{0};
  /** Negotiated main-bus width (CLAP audio-ports-config, VST3 arrangements,
   *  AU stream format all land here). Set to the default at init. */
  uint32_t mainChannels = 2;
  /** What the negotiated channels MEAN, as a role bitmask. Set alongside the
   *  width; the surround extension reports it and process() hands the roles
   *  to the DSP. */
  uint64_t mainChannelMask = 0;
  /** The channel ORDER a host explicitly asked for, when one did.
   *
   *  A bitmask says which speakers are present; it cannot say in what order,
   *  because a set has none. Every path here except configurable-audio-ports
   *  works from the mask and gets ascending role order back, which is the
   *  conventional layout and what a host that did not specify expects.
   *
   *  configurable-audio-ports is different: the host hands over an actual
   *  channel map and then checks that surround.get_channel_map() gives the
   *  same one back. clap-validator caught exactly that -- "Wrong surround map
   *  set for output port (index 0)" on a 6-channel request -- because the
   *  width was honoured and the ORDER was quietly replaced by our default 5.1.
   *  Zero count means there is nothing to remember and the mask decides. */
  uint8_t mainRoleOrder[kMaxAudioChannels] = {};
  uint32_t numMainRoleOrder = 0;

  /** The main bus's channel roles: what the host asked for if it asked, and
   *  the mask's own order if it did not. */
  uint32_t mainRoles(uint8_t* out, uint32_t capacity) const {
    if (numMainRoleOrder > 0 && out) {
      const uint32_t n = numMainRoleOrder < capacity ? numMainRoleOrder : capacity;
      for (uint32_t i = 0; i < n; ++i) out[i] = mainRoleOrder[i];
      return n;
    }
    return rolesFromMask(mainChannelMask, out, capacity);
  }

  /** Set the main bus layout. `map` is an explicit channel order, or null for
   *  the conventional one; passing null CLEARS any previous explicit order,
   *  which is what every fresh negotiation should do. */
  void setMainLayout(uint32_t width, const uint8_t* map) {
    mainChannels = width;
    numMainRoleOrder = 0;
    if (map && width > 0 && width <= kMaxAudioChannels) {
      uint64_t mask = 0;
      for (uint32_t i = 0; i < width; ++i) {
        mainRoleOrder[i] = map[i];
        if (map[i] < 64) mask |= (uint64_t) 1 << map[i];
      }
      numMainRoleOrder = width;
      mainChannelMask = mask;
      return;
    }
    mainChannelMask = defaultChannelMask(width);
  }
  BypassState bypass;
  /** The aux buses each block hands the DSP, the audio thread's (see
   *  processdetail::AuxScratch). */
  processdetail::AuxScratch<float> aux32;
  processdetail::AuxScratch<double> aux64;
  /** Zeros handed to a sidechain DSP when the host routed nothing. Sized at
   *  activate; both channel pointers alias the same silence. */
  std::vector<float> scSilence;
  /** The same silence at double precision, for the 64-bit path. It used to
   *  hand the DSP a one-element scratch and a zero-length block, while the
   *  float path hands it a whole block of zeros -- and the contract says
   *  "silence-filled", so a DSP reading sidechain[i] for every i was
   *  correct in a 32-bit host and read past the stack in a 64-bit one. */
  std::vector<double> scSilence64;
  /** Last latency handed to the host, so a change is reported once. */
  uint32_t reportedLatency = 0;

  /** CLAP audio-ports-activation: which ports the host says it is using.
   *  Everything starts ACTIVE, which the extension requires, and none of it is
   *  saved in the plugin state -- the host restores it after creating the
   *  instance. Held in a self-initialising holder so that Instance keeps being
   *  a type nothing has to construct specially. */
  struct PortFlags {
    bool flags[1 + kMaxAuxOutputs];
    PortFlags() {
      for (uint32_t i = 0; i < 1 + kMaxAuxOutputs; ++i) flags[i] = true;
    }
    bool& operator[](uint32_t i) { return flags[i < 1 + kMaxAuxOutputs ? i : 0]; }
    bool operator[](uint32_t i) const { return flags[i < 1 + kMaxAuxOutputs ? i : 0]; }
  };
  /** Offline render, from clap.render. Hosts set this around a bounce, on
   *  the main thread, and process() reads it. */
  SharedFlag offline;
  /** The block size the host actually negotiated. Kept because prepare() has
   *  to be re-run on a reset and on a render-mode change, and rebuilding a
   *  ProcessSpec from defaults would tell the DSP something untrue. */
  uint32_t maxFrames = 128;
  PortFlags inputPortActive;  // 0 main, 1 sidechain
  PortFlags outputPortActive; // 0 main, 1.. aux

  // ── GUI state (main thread, except meter which the audio thread publishes) ─
  MeterState meter;
  /** How much of each block's deadline the DSP is using. Measured in every
   *  format, because the DSP being measured was written by a model that has
   *  never heard it run. */
  LoadMeasurer load;
  /** The scope's history, fed on every block whether or not an editor is open.
   *  Owned here rather than by the editor so a window opened mid-session shows
   *  the last second of audio instead of starting blank -- the same reason the
   *  meter lives here. */
  AudioScopeBuffer scope;
  UiEventQueue uiEvents;
  /** Parameter values the PAGE last knows about, so the timer only pushes what
   *  actually changed: a host automating one control must not cost a full
   *  parameter broadcast every frame. */
  float uiEcho[SONORE_NUM_PARAMS > 0 ? SONORE_NUM_PARAMS : 1]{};
  bool uiEchoValid = false;
  /** The meter and load calls the page last ran, so a page showing a
   *  plugin at rest is not woken thirty times a second to be told the same
   *  numbers. The renderer is a process of its own, and an idle web editor
   *  kept it at 1.3-1.6% of a core doing exactly that. Cleared with the
   *  page (pageReady), which then needs everything again. */
  std::string uiLastMeter, uiLastLoad;
  /** The parent the host handed to set_parent, kept so a PARKED webview can
   *  be re-attached on show. See guiHide for what parking is and why. */
  void* guiParentHandle = nullptr;
  /** When the host hid the editor (steady-clock ms), 0 while visible. */
  int64_t guiHiddenAtMs = 0;
  /** The webview was destroyed after sitting hidden past the grace period.
   *  The GUI is still "created" as far as the host knows; show() rebuilds. */
  bool webviewParked = false;
  uint32_t guiWidth = SONORE_UI_WIDTH;
  uint32_t guiHeight = SONORE_UI_HEIGHT;
  bool guiCreated = false;
  double guiScale = 1.0;
#if defined(SONORE_HAS_WEBVIEW_BACKEND)
  PlatformWebView webview;
#endif
  /** The other editor. Both are members rather than a union or a pointer:
   *  each is empty until opened, and which one a host gets is decided once
   *  per gui_create rather than once per build. */
  gfx::NativeEditor nativeEditor;
  bool guiIsNative = false;
  /** How the native editor is being driven, on a platform where the SDK cannot
   *  drive it itself. See registerEditorPump. */
  int editorFd = -1;
  clap_id editorTimerId = 0;
  bool editorTimerRunning = false;

  // ── User presets [main-thread] ────────────────────────────────────────────
  /** The user's presets and the one this instance is on (user_presets.h).
   *  Set up on first use by presetsOf(): an instance nobody opens an editor
   *  on never touches the disk. The selection is saved in the state (v6). */
  PresetManager presets;
  /** How THIS format tells its host that the plugin changed its own state --
   *  a preset loaded from the editor, an undo -- and which preset it is now
   *  on. Null in CLAP, where the clap host is told (announceOwnStateLoad);
   *  VST3 and AU set them when they make the instance, because the news goes
   *  through their own objects: IComponentHandler, AU property listeners. */
  void (*onOwnStateLoad)(void* context) = nullptr;
  void (*onPresetSelected)(void* context) = nullptr;
  void* formatContext = nullptr;
};

/** [audio-thread] Copy the shared parameters into the block's own array.
 *  One relaxed load per parameter, once per block; the DSP then reads plain
 *  floats that cannot move under it for the rest of the call.
 *
 *  A copy taken while writeParamsTogether() was writing -- a state or preset
 *  loaded on the main thread mid-playback, which every format allows -- was
 *  half one set and half the other: a block of a gain from one session with
 *  the pan from another, a set nobody saved. Such a copy is dropped, and the
 *  block runs on the last whole set; the next takes the new one. Never waits:
 *  a reader that retried could spin for as long as a load takes. */
inline void snapshotParams(Instance* inst) {
  const uint32_t before = inst->paramsSeq.load(std::memory_order_acquire);
  float fresh[SONORE_NUM_PARAMS > 0 ? SONORE_NUM_PARAMS : 1];
  for (int i = 0; i < SONORE_NUM_PARAMS; ++i) fresh[i] = inst->params[i];
  std::atomic_thread_fence(std::memory_order_acquire);
  const uint32_t after = inst->paramsSeq.load(std::memory_order_relaxed);
  if (((before & 1u) != 0 || before != after) && inst->paramsSnapped) return;
  for (int i = 0; i < SONORE_NUM_PARAMS; ++i) inst->paramsBlock[i] = fresh[i];
  inst->paramsSnapped = true;
}

/** [main-thread, before processing starts] The block's copy, filled with a
 *  whole set before the first block. snapshotParams() keeps the last whole
 *  set when a copy is torn -- except on the very first block, which had no
 *  set to keep and took the torn copy as it was. Under load that was one
 *  block in about 800,000 in the state-race test: the first one after
 *  activation, with a state loading beside it. Done where the first prepare
 *  is, on the main thread, where a state load cannot be half-way through. */
inline void primeParamsBlock(Instance* inst) {
  if (inst->paramsSnapped) return;
  for (int i = 0; i < SONORE_NUM_PARAMS; ++i) inst->paramsBlock[i] = inst->params[i];
  inst->paramsSnapped = true;
}

/** [main-thread] Write the whole parameter set as one change: a block sees
 *  all of it or none of it (see snapshotParams). `write` stores the values.
 *
 *  Every caller is a state or a preset replacing the whole set, so an edit
 *  the editor queued before it is older than every value written here, and
 *  its drain would otherwise put it back over them (see
 *  UiEventQueue::supersedeParamEdits). Marked first: a drain is then left
 *  only the instant between its own check and its own store, where the edit
 *  and the load are as simultaneous as two things on two threads can be. */
template <typename Write>
inline void writeParamsTogether(Instance* inst, Write&& write) {
  inst->uiEvents.supersedeParamEdits();
  const uint32_t seq = inst->paramsSeq.load(std::memory_order_relaxed);
  inst->paramsSeq.store(seq + 1, std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_release);
  write();
  inst->paramsSeq.store(seq + 2, std::memory_order_release);
}

inline Instance* self(const clap_plugin_t* p) {
  return static_cast<Instance*>(p->plugin_data);
}

// ── Parameters ───────────────────────────────────────────────────────────────

/** Effects get a bypass, instruments do not -- bypassing a synth would mean
 *  routing its input to its output and it has no input. The same rule the
 *  VST3 and LV2 wrappers already follow, so one DSP behaves the same way
 *  everywhere. */
inline bool hasClapBypassParam() { return !kDesc.isInstrument; }

/** The bypass's id: fixed, and far from every index a parameter table can
 *  reach ('byps' in ASCII, below 2^31). It was one past the DSP's own
 *  parameters, so a version that APPENDED a parameter moved it -- and a
 *  session's bypass automation, saved against the old number, drove the new
 *  parameter. Now appending moves nothing. */
constexpr clap_id kClapBypassParamId = 0x62797073u;
static_assert(kClapBypassParamId > (clap_id) kMaxParams, "the bypass id must not be an index");

inline uint32_t paramsCount(const clap_plugin_t*) {
  return (uint32_t) SONORE_NUM_PARAMS + (hasClapBypassParam() ? 1u : 0u);
}

inline bool paramsGetInfo(const clap_plugin_t* plugin, uint32_t index, clap_param_info_t* info) {
  // The bypass, which this wrapper did not have.
  //
  // VST3 exposes one, LV2 exposes one, and the crossfade and latency-aligned
  // dry path behind them are shared machinery -- but in CLAP none of it was
  // reachable, because there was no control for a host to press. The same
  // plugin bypassed cleanly in two formats and not at all in the third.
  //
  // CLAP_PARAM_IS_BYPASS is what tells a host this is ITS bypass button
  // rather than a knob that happens to be called Bypass.
  if (hasClapBypassParam() && index == (uint32_t) kDesc.numParams) {
    std::memset(info, 0, sizeof(*info));
    info->id = kClapBypassParamId;
    std::snprintf(info->name, sizeof(info->name), "Bypass");
    info->min_value = 0.0;
    info->max_value = 1.0;
    info->default_value = 0.0;
    info->flags = CLAP_PARAM_IS_BYPASS | CLAP_PARAM_IS_STEPPED | CLAP_PARAM_IS_AUTOMATABLE;
    return true;
  }
  if (index >= (uint32_t) kDesc.numParams) return false;
  const ParamInfo& p = kDesc.params[index];
  std::memset(info, 0, sizeof(*info));
  info->id = index; // index IS the id: the generated contract never reorders
  // Read-only wins over automatable: there is nothing to record about a value
  // the host cannot set, and claiming otherwise gets it an automation lane
  // that does nothing.
  info->flags = 0;
  if (p.readOnly) info->flags |= CLAP_PARAM_IS_READONLY;
  else if (p.automatable) info->flags |= CLAP_PARAM_IS_AUTOMATABLE;
  if (p.hidden) info->flags |= CLAP_PARAM_IS_HIDDEN;
  // Modulation is an offset to a value the host can set, so never for a
  // read-only one. Per note, a note named any of the three ways CLAP names
  // one: by id, by key, by channel.
  if (!p.readOnly && (p.modulatable || p.modulatablePerNote))
    info->flags |= CLAP_PARAM_IS_MODULATABLE;
  if (!p.readOnly && p.modulatablePerNote)
    info->flags |= CLAP_PARAM_IS_MODULATABLE_PER_NOTE_ID | CLAP_PARAM_IS_MODULATABLE_PER_KEY |
                   CLAP_PARAM_IS_MODULATABLE_PER_CHANNEL;
  // CLAP's stepped flag promises the CONSECUTIVE integers min..max -- a host
  // steps such a control by one. A control stepping 2, 4, 6, 8 (or 0, 0.5,
  // 1) cannot keep that promise, so it is declared continuous here and the
  // parameter path snaps whatever arrives: the DSP still only sees a step,
  // and value_to_text still shows the step's name.
  if (stepsAreConsecutiveIntegers(p)) {
    info->flags |= CLAP_PARAM_IS_STEPPED;
    // Every step named: a choice, not a count. CLAP_PARAM_IS_ENUM is what
    // lets a host draw a menu instead of a knob, and it was never set -- a
    // filter type got a dial whose positions said nothing until turned.
    if (isNamedChoice(p)) info->flags |= CLAP_PARAM_IS_ENUM;
  }
  info->min_value = p.minValue;
  info->max_value = p.maxValue;
  info->default_value = p.defaultValue;
  std::snprintf(info->name, sizeof(info->name), "%s", p.label);
  // CLAP spells the hierarchy as a slash-separated module path; a host builds
  // its tree from it. One level is all a parameter table expresses.
  if (p.group && p.group[0]) std::snprintf(info->module, sizeof(info->module), "%s", p.group);
  else info->module[0] = '\0';
  (void) plugin;
  return true;
}

inline bool paramsGetValue(const clap_plugin_t* plugin, clap_id id, double* out) {
  if (hasClapBypassParam() && id == kClapBypassParamId) {
    *out = self(plugin)->bypass.engaged ? 1.0 : 0.0;
    return true;
  }
  if (id >= (uint32_t) SONORE_NUM_PARAMS) return false;
  *out = (double) self(plugin)->params[id];
  return true;
}

inline bool paramsValueToText(const clap_plugin_t*, clap_id id, double value,
                              char* out, uint32_t capacity) {
  if (hasClapBypassParam() && id == kClapBypassParamId) {
    std::snprintf(out, (size_t) capacity, "%s", value >= 0.5 ? "On" : "Off");
    return true;
  }
  if (id >= (uint32_t) kDesc.numParams) return false;
  formatParamValue(kDesc.params[id], (float) value, out, (size_t) capacity);
  return true;
}

inline bool paramsTextToValue(const clap_plugin_t*, clap_id id, const char* text, double* out) {
  if (hasClapBypassParam() && id == kClapBypassParamId) {
    bool on = false;
    if (!parseSwitchText(text, &on)) return false;
    *out = on ? 1.0 : 0.0;
    return true;
  }
  if (id >= (uint32_t) kDesc.numParams) return false;
  float v = 0.0f;
  if (!parseParamValue(kDesc.params[id], text, &v)) return false;
  *out = (double) v;
  return true;
}

/** The event as its type's struct -- if the header says there is enough of it.
 *
 *  `size` is in the header so that a plugin can check it, and the first
 *  version of this did not: a header that claimed sixteen bytes was cast to a
 *  fifty-byte sysex event and its buffer pointer read from whatever followed
 *  the host's storage. The event fuzz in the host test found it in the plugin
 *  that accepts MIDI. A short event is ignored, never partially read. */
template <typename T>
inline const T* eventAs(const clap_event_header_t* hdr) {
  return hdr->size >= sizeof(T) ? reinterpret_cast<const T*>(hdr) : nullptr;
}

/** Where in a block of `frames` an event lands: its own time, clamped into the
 *  block.
 *
 *  The host's time was passed on as it came. A note-off at or past the end --
 *  one frame of rounding in a host's scheduler -- sat where no DSP that stops
 *  at the last frame reached it, and the note hung; a time of 2^31 became a
 *  negative position. With no block (flush, a zero-length call) everything is
 *  at 0. */
inline int eventTime(const clap_event_header_t* hdr, uint32_t frames) {
  if (frames == 0) return 0;
  return (int) (hdr->time < frames ? hdr->time : frames - 1);
}

/** Apply one event to our state. Shared by process() and flush() so a parameter
 *  can never mean two different things depending on which path delivered it.
 *  `frames` is the block the event belongs to: 0 from flush. */
inline void applyEvent(Instance* inst, const clap_event_header_t* hdr, uint32_t frames) {
  if (!hdr || hdr->space_id != CLAP_CORE_EVENT_SPACE_ID) return;
  if (hdr->size < sizeof(clap_event_header_t)) return;
  const int time = eventTime(hdr, frames);
  switch (hdr->type) {
    case CLAP_EVENT_PARAM_VALUE: {
      const auto* e = eventAs<clap_event_param_value_t>(hdr);
      if (!e) break;
      if (hasClapBypassParam() && e->param_id == kClapBypassParamId) {
        // Engaged, not jumped: BypassState crossfades over 20 ms so pressing
        // the host's bypass button mid-note does not click.
        inst->bypass.engaged = e->value >= 0.5;
        break;
      }
      if (e->param_id < (uint32_t) SONORE_NUM_PARAMS)
        inst->params[e->param_id] = clampToRange(kDesc.params[e->param_id], (float) e->value);
      break;
    }
    case CLAP_EVENT_PARAM_MOD: {
      // Modulation: an offset to the value, never written into it -- not
      // saved, not shown on the knob, gone when the host sends 0.
      const auto* e = eventAs<clap_event_param_mod_t>(hdr);
      if (!e || e->param_id >= (uint32_t) SONORE_NUM_PARAMS) break;
      const ParamInfo& p = kDesc.params[e->param_id];
      if (p.readOnly || !(p.modulatable || p.modulatablePerNote)) break;
      const float amount = std::isfinite(e->amount) ? (float) e->amount : 0.0f;
      const bool wide = e->note_id < 0 && e->key < 0 && e->channel < 0;
      // The plugin-wide offset is kept either way: added to what process() is
      // handed, or for a parameter modulated per note, handed to its voices
      // again whenever queued events are thrown away (requeueWideMods).
      if (wide) inst->paramMod[e->param_id] = amount;
      if (!p.modulatablePerNote) break;
      // A parameter modulated per note has its plugin-wide offset handed to
      // the voices as well, as an entry naming no note: a note's own amount
      // replaces the plugin-wide one for that note (CLAP: it already includes
      // it), and only the voices know which notes have one. Added to what
      // process() is handed instead, it would be counted twice on every such
      // note.
      ParamModBuffer::Entry entry;
      entry.noteId = e->note_id;
      entry.key = e->key;
      entry.channel = e->channel;
      // Named by its id alone, as an expression can be: key and channel
      // from the note-on that carried the id, or nothing for an id nobody
      // played.
      if (e->note_id >= 0 && (e->key < 0 || e->channel < 0)) {
        const Instance::NoteIdSlot* slot = nullptr;
        for (const Instance::NoteIdSlot& sl : inst->noteIds)
          if (sl.id == e->note_id) slot = &sl;
        if (!slot) break;
        if (entry.key < 0) entry.key = slot->key;
        if (entry.channel < 0) entry.channel = slot->channel;
      }
      entry.param = (int32_t) e->param_id;
      entry.amount = amount;
      entry.samplePosition = time;
      inst->paramMods.addEvent(entry);
      break;
    }
    case CLAP_EVENT_NOTE_ON: {
      const auto* e = eventAs<clap_event_note_t>(hdr);
      if (!e || e->key < 0) break;
      const int ch = e->channel < 0 ? 0 : e->channel;
      const int vel = (int) (e->velocity * 127.0 + 0.5);
      inst->midi.addEvent(MidiMessage::noteOn(ch, e->key, vel < 1 ? 1 : vel), time);
      // Every note-on, named or not, so the DSP pairs each with its own.
      inst->noteIdsIn.add(time, e->key, ch, e->note_id);
      if (e->note_id >= 0) {
        // The id's own slot if it has one -- a host may use an id again once
        // the note is released, and two slots under one id would steer an
        // event for the new note by the old note's key -- else a free one,
        // else the oldest.
        int pick = -1;
        for (int k = 0; k < 64 && pick < 0; ++k)
          if (inst->noteIds[k].id == e->note_id) pick = k;
        for (int k = 0; k < 64 && pick < 0; ++k) {
          const int at = (inst->noteIdNext + k) % 64;
          if (inst->noteIds[at].id < 0) pick = at;
        }
        if (pick < 0) pick = inst->noteIdNext;
        inst->noteIdNext = (pick + 1) % 64;
        Instance::NoteIdSlot& slot = inst->noteIds[pick];
        slot.id = e->note_id;
        slot.key = (int16_t) e->key;
        slot.channel = (int16_t) ch;
        slot.released = slot.ended = false;
      }
      break;
    }
    case CLAP_EVENT_NOTE_OFF:
    case CLAP_EVENT_NOTE_CHOKE: {
      const auto* e = eventAs<clap_event_note_t>(hdr);
      if (!e) break;
      int key = e->key, channel = e->channel;
      // A note_id names ONE voice. With the key or channel left open it was
      // read as the wildcard it is without one, and a choke meant for a single
      // note silenced the whole chord. Resolved through the ids the note-ons
      // carried; an id nobody played is a no-op, not a panic.
      if (e->note_id >= 0 && (key < 0 || channel < 0)) {
        bool found = false;
        for (Instance::NoteIdSlot& slot : inst->noteIds) {
          if (slot.id != e->note_id) continue;
          if (key < 0) key = slot.key;
          if (channel < 0) channel = slot.channel;
          found = true;
          break;
        }
        if (!found) break;
      }
      // The names this lets go of: kept through the release, which is still
      // the note, and freed when the DSP reports the end -- or now, if it
      // already has.
      for (Instance::NoteIdSlot& slot : inst->noteIds) {
        if (slot.id < 0) continue;
        if (e->note_id >= 0 ? slot.id != e->note_id
                            : ((key >= 0 && slot.key != key) || (channel >= 0 && slot.channel != channel)))
          continue;
        slot.released = true;
        if (slot.ended) slot.id = -1;
      }
      // A wildcard CHANNEL is every channel. Read as channel 0, it released
      // nothing on an MPE instrument, whose notes sit on 1-15.
      const int firstChannel = channel < 0 ? 0 : channel;
      const int lastChannel = channel < 0 ? 15 : channel;
      for (int ch = firstChannel; ch <= lastChannel; ++ch) {
        if (key < 0) {
          // CLAP's wildcard: key -1 means "every note" -- how a host panic and
          // a choke-all arrive. Turned into the MIDI that says exactly that:
          // All Notes Off (CC 123), or All Sound Off (CC 120) for the harder
          // choke, both of which the toolkit's applyPedals() maps to
          // allNotesOff(). A raw noteOff would mask -1 to key 127 and release
          // the ONE note nobody was playing, leaving the chord stuck on.
          const int cc = hdr->type == CLAP_EVENT_NOTE_CHOKE ? 120 : 123;
          inst->midi.addEvent(MidiMessage::controlChange(ch, cc, 0), time);
        } else {
          inst->midi.addEvent(MidiMessage::noteOff(ch, key), time);
        }
      }
      break;
    }
    case CLAP_EVENT_NOTE_EXPRESSION: {
      // The native spelling: the host already resolved which note is meant.
      const auto* e = eventAs<clap_event_note_expression_t>(hdr);
      if (!e) break;
      // Except pressure, which MIDI has a message for: poly aftertouch. A
      // host speaking CLAP's dialect -- the one this plugin prefers -- sends
      // a key's pressure as this expression, and it was dropped for every DSP
      // that had not declared MPE, so one reading aftertouch from its MIDI
      // heard none in this format alone; the VST3 build hands its poly
      // pressure over in both forms. Both forms here too.
      if (!kDesc.supportsMpe && e->expression_id != CLAP_NOTE_EXPRESSION_PRESSURE) break;
      NoteExpressionBuffer::Entry entry;
      entry.noteId = e->note_id;
      entry.key = e->key;
      entry.channel = e->channel;
      // A note_id names one voice, and a host may name it by that alone, key
      // and channel left open. A DSP finds the voice by key and channel, so
      // they come from the ids the note-ons carried, as a note-off's do:
      // passed on as -1, the expression reached no voice at all. An id
      // nobody played is dropped, not aimed at every note.
      if (e->note_id >= 0 && (e->key < 0 || e->channel < 0)) {
        const Instance::NoteIdSlot* slot = nullptr;
        for (const Instance::NoteIdSlot& s : inst->noteIds)
          if (s.id == e->note_id) slot = &s;
        if (!slot) break;
        if (entry.key < 0) entry.key = slot->key;
        if (entry.channel < 0) entry.channel = slot->channel;
      }
      if (e->expression_id == CLAP_NOTE_EXPRESSION_PRESSURE && entry.key >= 0 && entry.key < 128 &&
          entry.channel >= 0 && entry.channel < 16) {
        const double v = e->value > 0.0 ? (e->value < 1.0 ? e->value : 1.0) : 0.0; // NaN is 0
        inst->midi.addEvent(MidiMessage::polyPressure(entry.channel, entry.key, (int) (v * 127.0 + 0.5)),
                            time);
      }
      if (!kDesc.supportsMpe) break;
      entry.expression = (uint8_t) e->expression_id;
      entry.value = (float) e->value;
      entry.samplePosition = time;
      inst->expression.addEvent(entry);
      break;
    }
    case CLAP_EVENT_MIDI: {
      // Hosts that speak raw MIDI rather than CLAP notes.
      const auto* e = eventAs<clap_event_midi_t>(hdr);
      if (!e) break;
      const int status = e->data[0];
      // One policy, in audio.h, shared with every other wrapper. SysEx is the
      // only thing excluded, and it is excluded because a three-byte event
      // cannot carry it -- not because a DSP should not see it.
      if (!deliverableToDsp(status)) break;
      inst->midi.addEvent(MidiMessage(status, e->data[1], e->data[2]), time);
      break;
    }
    case CLAP_EVENT_MIDI_SYSEX: {
      // CLAP hands over a POINTER that is valid only for this call, so the
      // bytes are copied into the block's own arena rather than referenced.
      // A DSP reading them after process() returns would be reading the
      // host's freed buffer.
      const auto* e = eventAs<clap_event_midi_sysex_t>(hdr);
      if (!e) break;
      if (e->buffer && e->size >= 2)
        inst->midi.addSysex(e->buffer, (size_t) e->size, time);
      break;
    }
    default:
      break;
  }
}

/** Forget the notes and expression waiting for a block: the DSP has had them,
 *  or there is no DSP block to give them to. */
inline void discardEvents(Instance* inst) {
  inst->midi.clear();
  inst->expression.clear();
  inst->noteIdsIn.clear();
  inst->paramMods.clear();
}

/** [audio-thread] What a reset or an activation leaves behind of the host's
 *  notes: every note is silenced, so the names they were played under go.
 *  The plugin-wide offsets stay: they are the host's modulators, still set,
 *  and a host does not resend a modulation that has not moved. */
inline void forgetHostNotes(Instance* inst) {
  for (Instance::NoteIdSlot& slot : inst->noteIds) slot.id = -1;
}

/** [audio-thread] The values the DSP is handed this block: the block's
 *  snapshot, with the host's modulation for the whole plugin added and
 *  clamped to each range. The snapshot itself when nothing is modulated, so
 *  a plugin no host modulates pays one pass over its parameters. */
inline const float* dspParams(Instance* inst) {
  // A parameter modulated per note takes all its modulation through the
  // voices (ParamModBuffer), never here: here it would be counted twice on
  // every note with an amount of its own.
  auto folded = [](int i) { return !kDesc.params[i].modulatablePerNote; };
  bool any = false;
  for (int i = 0; i < SONORE_NUM_PARAMS; ++i)
    if (inst->paramMod[i] != 0.0f && folded(i)) any = true;
  if (!any) return inst->paramsBlock;
  for (int i = 0; i < SONORE_NUM_PARAMS; ++i)
    inst->paramsModulated[i] = inst->paramMod[i] != 0.0f && folded(i)
                                   ? clampToRange(kDesc.params[i], inst->paramsBlock[i] + inst->paramMod[i])
                                   : inst->paramsBlock[i];
  return inst->paramsModulated;
}

/** [audio-thread] The plugin-wide offsets of the parameters modulated per
 *  note, queued for the voices again: what was queued has just been thrown
 *  away (an activation, a reset, a block with no output), and a host does
 *  not resend a modulation that has not moved. */
inline void requeueWideMods(Instance* inst) {
  for (int i = 0; i < SONORE_NUM_PARAMS; ++i) {
    if (!kDesc.params[i].modulatablePerNote || inst->paramMod[i] == 0.0f) continue;
    ParamModBuffer::Entry wide;
    wide.param = i;
    wide.amount = inst->paramMod[i];
    inst->paramMods.addEvent(wide);
  }
}

/** [audio-thread] MPE's channel-per-note MIDI into per-note expression, for a
 *  DSP whose descriptor says supportsMpe: the plain channels' bend range
 *  from the DSP's pitchBendRange(), against `params` -- the values the DSP
 *  is about to be handed, so a Bend Range control moved for this block bends
 *  this block -- and then `midi`. Every format calls this, and only this, to
 *  decode, after the block's one snapshotParams(). */
inline void decodeMpe(Instance* inst, const MidiBuffer& midi, const float* params) {
  if (!kDesc.supportsMpe) return;
  syncBendRange(inst->dsp, inst->mpe, params);
  inst->mpe.process(midi, inst->expression);
}

/** Drain the UI queue: apply each edit locally AND tell the host, so a knob
 *  turned on our webview records as automation just like a host-side control.
 *  Runs on the audio thread; the queue is lock-free and this allocates nothing. */
inline void drainUiEvents(Instance* inst, const clap_output_events_t* out) {
  UiEventQueue::Event e;
  while (inst->uiEvents.pop(&e)) {
    switch (e.kind) {
      case UiEventQueue::Event::Kind::ParamSet: {
        if (e.index < 0 || e.index >= SONORE_NUM_PARAMS) break;
        inst->params[e.index] = clampToRange(kDesc.params[e.index], e.value);
        if (out) {
          clap_event_param_value_t ev{};
          ev.header.size = sizeof(ev);
          ev.header.time = 0;
          ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
          ev.header.type = CLAP_EVENT_PARAM_VALUE;
          ev.header.flags = CLAP_EVENT_IS_LIVE; // a human moved this, right now
          ev.param_id = (clap_id) e.index;
          ev.note_id = -1;
          ev.port_index = -1;
          ev.channel = -1;
          ev.key = -1;
          ev.value = inst->params[e.index];
          out->try_push(out, &ev.header);
        }
        break;
      }
      case UiEventQueue::Event::Kind::GestureBegin:
      case UiEventQueue::Event::Kind::GestureEnd: {
        if (e.index < 0 || e.index >= SONORE_NUM_PARAMS || !out) break;
        // Gestures are what let a host coalesce a drag into one undo step and
        // override automation playback while the mouse is down.
        clap_event_param_gesture_t ev{};
        ev.header.size = sizeof(ev);
        ev.header.time = 0;
        ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        ev.header.type = (uint16_t) (e.kind == UiEventQueue::Event::Kind::GestureBegin
                                         ? CLAP_EVENT_PARAM_GESTURE_BEGIN
                                         : CLAP_EVENT_PARAM_GESTURE_END);
        ev.param_id = (clap_id) e.index;
        out->try_push(out, &ev.header);
        break;
      }
      // Kept until a block plays them: drained by flush, they wait for the
      // next process(). What a deactivated plugin queues, activate() drops.
      case UiEventQueue::Event::Kind::NoteOn: {
        const int vel = (int) e.value;
        inst->midi.addEvent(MidiMessage::noteOn(0, e.index, vel < 1 ? 1 : vel), 0);
        break;
      }
      case UiEventQueue::Event::Kind::NoteOff:
        inst->midi.addEvent(MidiMessage::noteOff(0, e.index), 0);
        break;
    }
  }
}

/** Defined once dspLatency() exists, which is much further down; declared
 *  here because the flush below is one of the places that has to call them. */
inline void requestRestartIfLatencyChanged(Instance* inst);
inline void announceTailIfChanged(Instance* inst);
inline void markDirtyIfNeeded(Instance* inst);
template <typename T>
inline bool dspConsumeStateDirty(T& dsp);

inline void paramsFlush(const clap_plugin_t* plugin, const clap_input_events_t* in,
                        const clap_output_events_t* out) {
  Instance* inst = self(plugin);
  const uint32_t n = in ? in->size(in) : 0;
  for (uint32_t i = 0; i < n; ++i) applyEvent(inst, in->get(in, i), 0);
  // A parameter written while idle can move the latency. Not announced here,
  // whatever the plugin's state: asked for, and said in activate.
  requestRestartIfLatencyChanged(inst);
  // Anything the DSP has been sitting on can be handed over now rather than
  // waiting for a process call that may not come while the transport is stopped.
  if (dspConsumeStateDirty(inst->dsp)) inst->stateDirty = true;
  // But flush is [active ? audio-thread : main-thread], and mark_dirty is
  // [main-thread]. The first version of this assumed flush was always the main
  // thread and called mark_dirty straight through -- an audio-thread violation
  // any time a knob moved while the plugin was active with the transport
  // stopped. So when active we do what process() does (record and ask for a
  // main-thread callback); only when deactivated is the direct call legal.
  if (inst->activated) {
    if (inst->stateDirty && inst->host) inst->host->request_callback(inst->host);
  } else {
    markDirtyIfNeeded(inst);
  }
  // While the plugin is idle the host still calls flush, and the user can still
  // be turning our knobs. Without this, an edit made with transport stopped
  // would sit in the queue until playback resumed.
  drainUiEvents(inst, out);
}

inline const clap_plugin_params_t kParamsExt = {
    paramsCount, paramsGetInfo, paramsGetValue,
    paramsValueToText, paramsTextToValue, paramsFlush,
};

// ── Audio ports ──────────────────────────────────────────────────────────────

inline uint32_t audioPortsCount(const clap_plugin_t*, bool isInput) {
  // A synth has no audio input: declaring one would make hosts feed it silence
  // and show a pointless input meter.
  if (isInput && kDesc.isInstrument) return 0;
  if (isInput && TakesSidechain<SonoreDsp>::value) return 2;
  if (!isInput) return 1 + numAuxOutputs();
  return 1;
}

inline bool audioPortsGet(const clap_plugin_t* plugin, uint32_t index, bool isInput,
                          clap_audio_port_info_t* info) {
  const bool sidechain = isInput && index == 1 && TakesSidechain<SonoreDsp>::value &&
                         !kDesc.isInstrument;
  const bool auxOut = !isInput && index >= 1 && index <= numAuxOutputs();
  if (index != 0 && !sidechain && !auxOut) return false;
  if (isInput && kDesc.isInstrument) return false;
  std::memset(info, 0, sizeof(*info));
  const uint32_t width = sidechain    ? 2u
                         : auxOut     ? auxBusChannels(index - 1)
                                      : self(plugin)->mainChannels;
  // Ids are stable and unique across directions: 0 main in, 1 main out,
  // 2 sidechain, 16+ aux outs. A host stores these in its routing.
  info->id = isInput ? (sidechain ? 2 : 0) : (auxOut ? 16 + (index - 1) : 1);
  std::snprintf(info->name, sizeof(info->name), "%s",
                sidechain  ? "Sidechain"
                : auxOut   ? kDesc.auxOutputs[index - 1].name
                : isInput  ? "Input"
                           : "Output");
  info->channel_count = width;
  // The first port of each direction is MAIN; the sidechain and the aux outs
  // are ports hosts route explicitly. clap-validator's config tests filter on
  // this flag, so a main input without it reads as "no main input" to a host.
  info->flags = (sidechain || auxOut) ? 0u : (uint32_t) CLAP_AUDIO_PORT_IS_MAIN;
  if (supportsDouble()) info->flags |= (uint32_t) CLAP_AUDIO_PORT_SUPPORTS_64BITS;
  info->port_type = width == 1   ? CLAP_PORT_MONO
                    : width == 2 ? CLAP_PORT_STEREO
                                 : CLAP_PORT_SURROUND;
  info->in_place_pair = CLAP_INVALID_ID;
  return true;
}

inline const clap_plugin_audio_ports_t kAudioPortsExt = {audioPortsCount, audioPortsGet};

// ── Audio ports CONFIG: how a CLAP host asks for mono or surround ────────────
//
// Exposed only when the descriptor declares a real range -- a fixed-stereo
// plugin neither lists configs nor pays for them, and clap-validator keeps
// skipping the tests it used to skip.

inline uint32_t audioPortsConfigCount(const clap_plugin_t*) {
  return maxMainChannels() - minMainChannels() + 1;
}

inline bool audioPortsConfigGet(const clap_plugin_t*, uint32_t index,
                                clap_audio_ports_config_t* config) {
  const uint32_t n = minMainChannels() + index;
  if (!config || n > maxMainChannels()) return false;
  std::memset(config, 0, sizeof(*config));
  config->id = n;
  std::snprintf(config->name, sizeof(config->name), "%s", channelLayoutName(n));
  const bool effect = !kDesc.isInstrument;
  config->input_port_count = effect ? (TakesSidechain<SonoreDsp>::value ? 2u : 1u) : 0u;
  config->output_port_count = 1 + numAuxOutputs();
  config->has_main_input = effect;
  config->main_input_channel_count = effect ? n : 0;
  config->main_input_port_type = n == 1   ? CLAP_PORT_MONO
                                : n == 2 ? CLAP_PORT_STEREO
                                         : CLAP_PORT_SURROUND;
  config->has_main_output = true;
  config->main_output_channel_count = n;
  config->main_output_port_type = config->main_input_port_type;
  return true;
}

inline bool audioPortsConfigSelect(const clap_plugin_t* plugin, clap_id configId) {
  Instance* inst = self(plugin);
  if (inst->activated) return false; // the contract: only while deactivated
  if (!channelCountAllowed((uint32_t) configId)) return false;
  // A config select is a fresh negotiation, so any order a previous
  // configurable-audio-ports request left behind is no longer current.
  inst->setMainLayout((uint32_t) configId, nullptr);
  return true;
}

// ── Surround: what each channel MEANS ────────────────────────────────────────
//
// Exposed only by plugins that can actually be wider than stereo -- for a
// fixed-stereo plugin the answer is never in doubt and the extension is noise.

inline bool surroundIsMaskSupported(const clap_plugin_t*, uint64_t mask) {
  uint32_t n = 0;
  for (int i = 0; i < 64; ++i)
    if (mask & ((uint64_t) 1 << i)) ++n;
  return channelCountAllowed(n);
}

inline uint32_t surroundGetChannelMap(const clap_plugin_t* plugin, bool isInput,
                                      uint32_t portIndex, uint8_t* channelMap,
                                      uint32_t channelMapCapacity) {
  Instance* inst = self(plugin);
  if (portIndex != 0 || !channelMap) return 0;
  if (isInput && kDesc.isInstrument) return 0;
  return inst->mainRoles(channelMap, channelMapCapacity);
}

inline const clap_plugin_surround_t kSurroundExt = {surroundIsMaskSupported,
                                                    surroundGetChannelMap};

inline const clap_plugin_audio_ports_config_t kAudioPortsConfigExt = {
    audioPortsConfigCount, audioPortsConfigGet, audioPortsConfigSelect};

// ── Audio ports ACTIVATION: which ports the host is really using ────────────
//
// A host that has nothing patched into a sidechain, or that routes nothing out
// of an aux bus, can say so. Note what this does NOT mean: the spec requires
// the host to keep handing over the buffers, zero-filled, with constant_mask
// set. So this can never turn into a null-pointer hazard, and a plugin that
// ignores it stays correct. What it buys is permission to skip work -- which
// is why the flags are passed to the DSP through ProcessContext rather than
// merely stored here to satisfy a validator.

inline bool audioPortsActivationCanActivateWhileProcessing(const clap_plugin_t*) {
  // True, and cheaply so: set_active writes one bool. Answering false would
  // force the host to deactivate the whole plugin to unpatch a sidechain,
  // which is a stall the user hears.
  return true;
}

inline bool audioPortsActivationSetActive(const clap_plugin_t* plugin, bool isInput,
                                          uint32_t portIndex, bool isActive,
                                          uint32_t /*sampleSize*/) {
  Instance* inst = self(plugin);
  if (!inst) return false;
  const uint32_t count = audioPortsCount(plugin, isInput);
  if (portIndex >= count) return false; // "false if failed, or invalid parameters"
  if (isInput) inst->inputPortActive[portIndex] = isActive;
  else inst->outputPortActive[portIndex] = isActive;
  return true;
}

// -- Render mode: a bounce is not a performance ------------------------------
//
// The host tells the plugin when it is rendering offline, and a DSP that
// trades quality for CPU while monitoring can stop trading. A DSP taking a
// ProcessContext reads ctx.offline every block; one using the simple
// process() signature hears about it from prepare(), at its next activation.

/** Re-run prepare() with the settings the host ACTUALLY negotiated.
 *
 *  Worth having for a second reason: the reset path used to build a
 *  ProcessSpec from DEFAULTS -- 128 samples, 2 channels -- whatever the
 *  session really was. A DSP that sizes a buffer from maximumBlockSize would
 *  have shrunk it to 128 on every reset and then been handed larger blocks. */
inline void prepareDsp(Instance* inst) {
  // Nothing can be held down across a prepare: the DSP is about to forget its
  // voices, and a key left lit is a key the user cannot put out. Here rather
  // than in each wrapper because every format prepares through this function,
  // and one that forgot would strand a note in exactly one host.
  inst->notes.allOff();
  ProcessSpec spec;
  spec.sampleRate = inst->sampleRate;
  spec.maximumBlockSize = inst->maxFrames;
  spec.numChannels = inst->mainChannels;
  spec.offline = inst->offline;
  inst->dsp.prepare(spec);
  primeParamsBlock(inst);
  // The load meter judges each block against its deadline, which is frames
  // over THIS rate. Nothing ever told it, so it assumed 48 kHz in every
  // format: at 96 kHz the editor showed half the real load.
  inst->load.reset(inst->sampleRate);
#if defined(SONORE_LICENSED_BUILD)
  // Every format prepares through this function, so this is the whole surface.
  licenseGate().setSampleRate(inst->sampleRate);
#endif
}

/** A host's "start over" at the same rate and block size: CLAP's reset() and
 *  an AU's Reset, both on the audio thread. The DSP's own reset() when it has
 *  one, so nothing prepare() sized is freed or allocated there; prepare()
 *  again when it has not, as it always was. Held notes go either way. */
template <typename T>
inline void resetDspOf(T& dsp, Instance* inst) {
  if constexpr (HasReset<T>::value) {
    inst->notes.allOff();
    dsp.reset();
  } else {
    (void) dsp;
    prepareDsp(inst);
  }
}
inline void resetDsp(Instance* inst) { resetDspOf(inst->dsp, inst); }

inline bool renderHasHardRealtimeRequirement(const clap_plugin_t*) {
  // False: nothing here is a proxy for a piece of hardware. Answering true
  // would tell the host this plugin CANNOT be bounced faster than real time,
  // which for software DSP is simply untrue.
  return false;
}

inline bool renderSet(const clap_plugin_t* plugin, clap_plugin_render_mode mode) {
  Instance* inst = self(plugin);
  if (!inst) return false;
  const bool offline = mode == CLAP_RENDER_OFFLINE;
  if (offline == inst->offline) return true;
  inst->offline = offline;
  // [main-thread] is ALL the contract says: nothing requires the plugin to
  // be deactivated, and a host may well flip this while the transport runs.
  // The first version re-prepared the DSP right here on the assumption that
  // it never would, and ThreadSanitizer showed prepare() writing every
  // filter's coefficients while process() was reading them. So the flag is
  // only RECORDED: an atomic, read per block through ctx.offline, and handed
  // to prepare() by the next activate().
  //
  // NOT a request_restart, which is what the second version did, and a real
  // DAW is where it showed. REAPER switches its plugins to offline mode as a
  // render starts, while they are active and processing, and it honours a
  // restart request when it gets to it -- a third of a second into the bounce
  // in one run, a whole second in another. prepare() then ran mid-render:
  // every voice cut, every filter and envelope back to zero, half a block of
  // silence where the reverb's tail had been, an arpeggiator that dropped its
  // held chord and played nothing for the rest of the file. The VST3 build of
  // the same plugins rendered clean, because VST3 only changes the process
  // mode while the plugin is deactivated. A simple-signature DSP that wants
  // the offline flag gets it at its next activation, as a VST3 one does.
  return true;
}

inline const clap_plugin_render_t kRenderExt = {renderHasHardRealtimeRequirement, renderSet};

// ── Voice info: how many fingers this instrument can hold ────────────────────
//
// A host that does not know a plugin's polyphony guesses at it. Bitwig uses
// this to decide how many MPE member channels are worth allocating and to draw
// a voice meter; without it an expressive controller can be given more
// simultaneous notes than the instrument can hold, and the player hears voices
// stolen for no reason they can see.
//
// Two fields are easy to misread, and both were misread here.
//
// voice_count is the voices the patch CAN use -- the extension says it is not
// to be confused with the number sounding, and that it is at least 1. It was
// the number sounding: 0 for an instrument at rest, below the floor the
// extension sets, and 1 while one note played -- which a host may take for a
// monophonic patch, to which it sends one voice's worth of modulation.
//
// CLAP_VOICE_INFO_SUPPORTS_OVERLAPPING_NOTES lets a host send two NOTE_ONs on
// the same port, channel and key, which the plugin "will then rely upon the
// note_id to distinguish". This SDK cannot: notes reach a DSP as MIDI, and a
// VoiceManager tells voices apart by key and channel, so a note-off for one
// of the two released both -- the other note, still held, went silent. It is
// not what MPE needs either: two fingers on one pitch are on two channels.
// So it is not claimed, and a host resolves overlaps itself before sending.

/** Templated so the branch that is not taken is never INSTANTIATED.
 *
 *  `if constexpr` inside a plain function still requires both branches to
 *  compile, so asking a monophonic DSP for a voiceCapacity() it does not have
 *  is an error even when the condition is false. Making the DSP type a
 *  template parameter is what actually discards it. */
template <typename T>
inline bool fillVoiceInfo(const T& dsp, clap_voice_info_t* info, int count = 0) {
  if constexpr (HasVoiceCapacity<T>::value) {
    const int capacity = dsp.voiceCapacity();
    if (capacity <= 0) return false;
    info->voice_capacity = (uint32_t) capacity;
    // The voices the patch can use: a DSP with a Voices control says how many
    // through voiceCount(), as the last block left it (`count`); one without
    // can use every voice it has. See above for why this is not
    // activeVoices().
    if (count < 1 || count > capacity) count = capacity;
    info->voice_count = (uint32_t) count;
    info->flags = 0;
    return true;
  } else {
    (void) dsp;
    (void) info;
    (void) count;
    return false;
  }
}

inline bool voiceInfoGet(const clap_plugin_t* plugin, clap_voice_info_t* info) {
  if (!info || !kDesc.isInstrument) return false;
  Instance* inst = self(plugin);
  return fillVoiceInfo(inst->dsp, info, (int) (float) inst->voiceCountNow);
}

/** [audio-thread] Publish the DSP's voiceCount() and, when it moved, ask for
 *  the main thread to tell the host: clap_host_voice_info::changed is
 *  [main-thread]. */
template <typename T>
inline void publishVoiceCount(Instance* inst, const T& dsp) {
  if constexpr (HasVoiceCount<T>::value) {
    const float now = (float) dsp.voiceCount();
    if (now == (float) inst->voiceCountNow) return;
    // The first block's count is news too: a host that asked before it was
    // answered with the capacity, and a session may have restored fewer.
    inst->voiceCountNow = now;
    inst->voiceInfoDirty = true;
    if (inst->host) inst->host->request_callback(inst->host);
  } else {
    (void) inst;
    (void) dsp;
  }
}

/** [main-thread] Tell the host the voice count moved, once it has. */
inline void announceVoiceInfoIfChanged(Instance* inst) {
  if (!inst->voiceInfoDirty) return;
  inst->voiceInfoDirty = false;
  if (!inst->host) return;
  if (const auto* ext = static_cast<const clap_host_voice_info_t*>(
          inst->host->get_extension(inst->host, CLAP_EXT_VOICE_INFO)))
    if (ext->changed) ext->changed(inst->host);
}

inline const clap_plugin_voice_info_t kVoiceInfoExt = {voiceInfoGet};

// ── Remote controls: how the plugin looks on a hardware controller ───────────
//
// A host with an eight-knob controller has to decide which eight. Without this
// it takes the first eight parameters in declaration order, which for a synth
// means whatever happened to be written first -- attack, decay, sustain,
// release, and nothing that shapes the sound.
//
// The pages are built from the parameter GROUPS the descriptor already
// declares. A plugin that bothered to say "Filter" and "Envelope" has already
// said how it wants to be laid out, and inventing a second, different grouping
// for hardware would be a way for the two to disagree.
//
// A group of more than eight spills onto a second page rather than being
// truncated: "Filter 1", "Filter 2". A plugin with no groups at all gets plain
// pages of eight, which is still better than the host's arbitrary first eight
// because at least the pages are stable across versions.

/**
 * The descriptor's parameter groups, worked out once.
 *
 * collectGroups() walks the whole table, comparing every parameter's group
 * with each group found so far, and every format walked it again for each
 * question about ONE parameter: VST3's getParameterInfo for its unit, an AU
 * for its clump, a CLAP host for each remote-controls page. A host reads
 * every parameter's info as it makes an instance, so a VST3 of 256
 * parameters walked its table 256 times, a millisecond per instance and
 * most of what making one cost. kDesc does not change while the module is
 * loaded, and neither do its groups: the same table, built on first use.
 */
inline const GroupTable& descriptorGroups() {
  static const GroupTable groups = collectGroups(kDesc.params, kDesc.numParams);
  return groups;
}

/** Which of descriptorGroups() parameter `index` is in, -1 for none: what
 *  descriptorGroups().indexOf(kDesc.params[index].group) says, looked up.
 *  The remote-controls pages ask it of every parameter, for every group,
 *  for every page -- a host listing the 33 pages of a 256-parameter plugin
 *  made some 400,000 string comparisons of it, 1.6 ms per instance. */
inline int descriptorGroupOf(int index) {
  static const std::vector<int> of = [] {
    std::vector<int> table((size_t) (kDesc.numParams > 0 ? kDesc.numParams : 0), -1);
    for (int i = 0; i < kDesc.numParams; ++i)
      table[(size_t) i] = descriptorGroups().indexOf(kDesc.params[i].group);
    return table;
  }();
  return index >= 0 && index < (int) of.size() ? of[(size_t) index] : -1;
}

/** Which parameters belong on page `pageIndex`, and what to call it.
 *  Returns false when the page does not exist. */
inline bool remoteControlsPageAt(uint32_t pageIndex, clap_remote_controls_page_t* page) {
  if (!page) return false;
  const GroupTable& groups = descriptorGroups();

  // Walk the pages in order rather than computing an index: the arithmetic for
  // "which group does page 5 fall in, and how far into it" is the part that
  // goes wrong, and there are never enough pages for the walk to cost
  // anything.
  uint32_t page_ = 0;
  auto emit = [&](const char* section, int which, int ofGroup, const int* ids, int count) {
    std::memset(page, 0, sizeof(*page));
    page->page_id = page_;
    if (section && section[0]) {
      std::snprintf(page->section_name, sizeof(page->section_name), "%s", section);
      if (ofGroup > 1)
        std::snprintf(page->page_name, sizeof(page->page_name), "%s %d", section, which + 1);
      else std::snprintf(page->page_name, sizeof(page->page_name), "%s", section);
    } else {
      std::snprintf(page->page_name, sizeof(page->page_name), "Page %d", which + 1);
    }
    for (int i = 0; i < CLAP_REMOTE_CONTROLS_COUNT; ++i)
      page->param_ids[i] = i < count ? (clap_id) ids[i] : CLAP_INVALID_ID;
    // Not preset-specific: these pages describe the plugin, not whatever
    // preset happens to be loaded, so they stay put as the user browses.
    page->is_for_preset = false;
  };

  int ids[CLAP_REMOTE_CONTROLS_COUNT];
  if (groups.count > 0) {
    for (int g = 0; g < groups.count; ++g) {
      int inGroup = 0;
      for (int i = 0; i < kDesc.numParams; ++i)
        if (descriptorGroupOf(i) == g) ++inGroup;
      const int pagesHere = (inGroup + CLAP_REMOTE_CONTROLS_COUNT - 1) / CLAP_REMOTE_CONTROLS_COUNT;
      for (int sub = 0; sub < pagesHere; ++sub) {
        if (page_ == pageIndex) {
          int n = 0, seen = 0;
          for (int i = 0; i < kDesc.numParams && n < CLAP_REMOTE_CONTROLS_COUNT; ++i) {
            if (descriptorGroupOf(i) != g) continue;
            if (seen++ < sub * CLAP_REMOTE_CONTROLS_COUNT) continue;
            ids[n++] = i;
          }
          emit(groups.names[g], sub, pagesHere, ids, n);
          return true;
        }
        ++page_;
      }
    }
    // Parameters with no group at all still need somewhere to live, or a
    // controller simply cannot reach them.
    int ungrouped = 0;
    for (int i = 0; i < kDesc.numParams; ++i)
      if (descriptorGroupOf(i) < 0) ++ungrouped;
    const int extra = (ungrouped + CLAP_REMOTE_CONTROLS_COUNT - 1) / CLAP_REMOTE_CONTROLS_COUNT;
    for (int sub = 0; sub < extra; ++sub) {
      if (page_ == pageIndex) {
        int n = 0, seen = 0;
        for (int i = 0; i < kDesc.numParams && n < CLAP_REMOTE_CONTROLS_COUNT; ++i) {
          if (descriptorGroupOf(i) >= 0) continue;
          if (seen++ < sub * CLAP_REMOTE_CONTROLS_COUNT) continue;
          ids[n++] = i;
        }
        emit("", sub, extra, ids, n);
        return true;
      }
      ++page_;
    }
    return false;
  }

  // No groups: plain pages of eight, in declaration order.
  const int total = kDesc.numParams;
  const int pages = (total + CLAP_REMOTE_CONTROLS_COUNT - 1) / CLAP_REMOTE_CONTROLS_COUNT;
  if ((int) pageIndex >= pages) return false;
  int n = 0;
  for (int i = (int) pageIndex * CLAP_REMOTE_CONTROLS_COUNT;
       i < total && n < CLAP_REMOTE_CONTROLS_COUNT; ++i)
    ids[n++] = i;
  page_ = pageIndex;
  emit("", (int) pageIndex, pages, ids, n);
  return true;
}

inline uint32_t remoteControlsCount(const clap_plugin_t*) {
  uint32_t n = 0;
  clap_remote_controls_page_t scratch{};
  while (remoteControlsPageAt(n, &scratch)) ++n;
  return n;
}

inline bool remoteControlsGet(const clap_plugin_t*, uint32_t pageIndex,
                              clap_remote_controls_page_t* page) {
  return remoteControlsPageAt(pageIndex, page);
}

inline const clap_plugin_remote_controls_t kRemoteControlsExt = {remoteControlsCount,
                                                                 remoteControlsGet};

inline const clap_plugin_audio_ports_activation_t kAudioPortsActivationExt = {
    audioPortsActivationCanActivateWhileProcessing, audioPortsActivationSetActive};

// ── Configurable audio ports: the host ASKS instead of picking from a list ──
//
// audio-ports-config above offers a menu of whole layouts. This is the other
// direction: the host proposes a width per port and the plugin says yes or no.
// A host wanting mono in and mono out asks for exactly that rather than
// hunting for the config whose name suggests it.
//
// Requests may name any subset of the ports; anything not named keeps what it
// has. The main input and main output are NOT independent for an effect -- the
// DSP processes in place -- so a request naming both with different widths is
// refused rather than half-applied.

inline bool configurableAudioPortsCheck(const clap_plugin_t* plugin,
                                        const clap_audio_port_configuration_request_t* requests,
                                        uint32_t count, uint32_t* agreedMain,
                                        const uint8_t** agreedMap) {
  if (!requests && count > 0) return false;
  Instance* inst = self(plugin);
  uint32_t main = inst ? inst->mainChannels : defaultMainChannels();
  const uint8_t* mainMap = nullptr;
  bool mainNamed = false;

  for (uint32_t i = 0; i < count; ++i) {
    const clap_audio_port_configuration_request_t& r = requests[i];
    if (r.port_index >= audioPortsCount(plugin, r.is_input)) return false;

    const bool sidechain = r.is_input && r.port_index == 1;
    const bool auxOut = !r.is_input && r.port_index >= 1;

    if (sidechain) {
      // The sidechain is a fixed stereo key; a host asking for another width
      // is asking for something the DSP has no way to read.
      if (r.channel_count != 2) return false;
      continue;
    }
    if (auxOut) {
      // Aux buses have declared widths. They are the plugin's own structure,
      // not a negotiation.
      if (r.channel_count != auxBusChannels(r.port_index - 1)) return false;
      continue;
    }

    // A main port.
    if (!channelCountAllowed(r.channel_count)) return false;
    if (mainNamed && main != r.channel_count) return false; // in and out must agree
    main = r.channel_count;
    mainNamed = true;
    // For a surround request port_details IS the channel map, and the host
    // checks that we hand the same one back. Mono and stereo carry no details
    // and their order is not in question.
    if (r.port_type && std::strcmp(r.port_type, CLAP_PORT_SURROUND) == 0 && r.port_details)
      mainMap = static_cast<const uint8_t*>(r.port_details);
  }
  if (agreedMain) *agreedMain = main;
  if (agreedMap) *agreedMap = mainMap;
  return true;
}

inline bool configurableAudioPortsCanApply(
    const clap_plugin_t* plugin, const clap_audio_port_configuration_request_t* requests,
    uint32_t count) {
  return configurableAudioPortsCheck(plugin, requests, count, nullptr, nullptr);
}

inline bool configurableAudioPortsApply(const clap_plugin_t* plugin,
                                        const clap_audio_port_configuration_request_t* requests,
                                        uint32_t count) {
  Instance* inst = self(plugin);
  if (!inst || inst->activated) return false; // [main-thread && !active]
  uint32_t main = inst->mainChannels;
  const uint8_t* map = nullptr;
  // Checked in full BEFORE anything is written: a partially applied layout is
  // worse than a refused one, because the host believes it took.
  if (!configurableAudioPortsCheck(plugin, requests, count, &main, &map)) return false;
  inst->setMainLayout(main, map);
  return true;
}

inline const clap_plugin_configurable_audio_ports_t kConfigurableAudioPortsExt = {
    configurableAudioPortsCanApply, configurableAudioPortsApply};

// ── Note ports (instruments only) ────────────────────────────────────────────

inline uint32_t notePortsCount(const clap_plugin_t*, bool isInput) {
  if (isInput) return wantsMidiIn() ? 1u : 0u;
  return kDesc.producesMidi ? 1u : 0u;
}

inline bool notePortsGet(const clap_plugin_t*, uint32_t index, bool isInput,
                         clap_note_port_info_t* info) {
  if (index != 0) return false;
  if (isInput && !wantsMidiIn()) return false;
  if (!isInput && !kDesc.producesMidi) return false;
  std::memset(info, 0, sizeof(*info));
  info->id = isInput ? 0 : 1;
  if (isInput) {
    // Accept both dialects: CLAP notes from modern hosts, raw MIDI from the rest.
    info->supported_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI;
    // MPE is announced only by a DSP that actually plays expressively; a host
    // told otherwise would route a whole expressive controller into a
    // synth that ignores every nuance of it.
    if (kDesc.supportsMpe) info->supported_dialects |= CLAP_NOTE_DIALECT_MIDI_MPE;
    info->preferred_dialect = CLAP_NOTE_DIALECT_CLAP;
  } else {
    // We emit raw MIDI: the DSP's own buffer holds 3-byte messages, and
    // claiming a dialect we do not actually speak would be a lie a host acts on.
    info->supported_dialects = CLAP_NOTE_DIALECT_MIDI;
    info->preferred_dialect = CLAP_NOTE_DIALECT_MIDI;
  }
  std::snprintf(info->name, sizeof(info->name), "%s", isInput ? "MIDI In" : "MIDI Out");
  return true;
}

inline const clap_plugin_note_ports_t kNotePortsExt = {notePortsCount, notePortsGet};

// ── State ────────────────────────────────────────────────────────────────────

/**
 * The fields of the blob, in order, from wherever they come from.
 *
 * Shared by saveStateBody (an instance's own) and factoryPresetState (a
 * factory preset's, with no instance at all), so the format is written in
 * one place however many things write it. `bag` null writes no bag, which a
 * loader reads as "this blob does not describe the DSP's own state" and
 * leaves that state alone.
 */
template <typename Put>
inline bool writeStateFields(Put&& put, const float* values, uint8_t bypassByte, int32_t preset,
                             int32_t editorWidth, int32_t editorHeight,
                             const PresetManager::Selection& selection, const std::vector<uint8_t>* bag) {
  auto write = [&](const void* data, size_t size) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    while (size > 0) {
      const size_t wrote = put(p, size);
      if (wrote == 0) return false;
      p += wrote;
      size -= wrote;
    }
    return true;
  };

  StateHeader h{{'S', 'N', 'R', 'S'}, kStateVersion, (uint32_t) SONORE_NUM_PARAMS};
  if (!write(&h, sizeof(h))) return false;
  if (!write(values, sizeof(float) * (size_t) SONORE_NUM_PARAMS)) return false;

  if (!write(&bypassByte, 1)) return false;

  // v4: which preset is selected. BEFORE the bag, not after, because the bag
  // is read until the stream stops giving -- anything written past it is
  // swallowed by it and never seen again.
  if (!write(&preset, sizeof(preset))) return false;

  // v5: how big the user made the editor.
  //
  // A plugin whose window forgets its size is a plugin the user resizes every
  // time they open it. Plugins conventionally keep this in their state for
  // the same reason, and it belongs in the SESSION rather than in user settings: two
  // instances of the same plugin on different tracks may reasonably be
  // different sizes, and the one on the master that somebody made large
  // should stay large only there.
  //
  // Written BEFORE the bag, like the preset index and for the same reason:
  // the bag is read until the stream stops giving, so anything after it is
  // swallowed.
  const int32_t editorSize[2] = {editorWidth, editorHeight};
  if (!write(editorSize, sizeof(editorSize))) return false;

  // v6: the preset the user is on -- origin (0 none, 1 factory, 2 user), the
  // factory index, the name, a user preset's file name, and the hash of the
  // sound when it was loaded or saved. Before the bag, for the same reason.
  const uint8_t origin = selection.origin == PresetManager::Origin::Factory ? 1
                         : selection.origin == PresetManager::Origin::User  ? 2
                                                                            : 0;
  const int32_t factoryIndex = origin == 1 ? (int32_t) selection.factoryIndex : -1;
  const std::string name = origin ? selection.name.substr(0, kMaxStatePresetText) : std::string();
  const std::string file = origin == 2 ? selection.fileName.substr(0, kMaxStatePresetText) : std::string();
  const uint32_t nameLength = (uint32_t) name.size(), fileLength = (uint32_t) file.size();
  const uint64_t baseline = origin ? selection.baseline : 0;
  if (!write(&origin, 1) || !write(&factoryIndex, sizeof(factoryIndex)) || !write(&nameLength, 4) ||
      (nameLength && !write(name.data(), nameLength)) || !write(&fileLength, 4) ||
      (fileLength && !write(file.data(), fileLength)) || !write(&baseline, sizeof(baseline)))
    return false;

  // v3: whatever the DSP itself wants to remember.
  if (bag && !bag->empty() && !write(bag->data(), bag->size())) return false;
  return true;
}

/**
 * The state format itself, told nothing about streams: saveStateBody writes
 * an instance's blob and loadStateBody below reads one back.
 *
 * This existed TWICE -- once here for CLAP and once in the VST3 wrapper --
 * and the two had already drifted. The CLAP copy learned to reset unknown
 * parameters to their defaults, to treat a v1 blob's bypass as off, and to
 * hand an empty bag to a DSP whose blob predates bags. The VST3 copy learned
 * none of it, so the same session file restored differently depending on
 * which build of the same plugin opened it.
 *
 * `put` and `get` move bytes and return how many they moved; zero means the
 * stream is finished. Everything else about the format lives here, once.
 */
template <typename Put>
inline bool saveStateBody(const Instance* inst, Put&& put) {
  float values[SONORE_NUM_PARAMS > 0 ? SONORE_NUM_PARAMS : 1];
  for (int i = 0; i < SONORE_NUM_PARAMS; ++i) values[i] = inst->params[i];
  StateBag bag;
  saveDspState(inst->dsp, bag);
  std::vector<uint8_t> bagBytes;
  bag.serialise(bagBytes);
  return writeStateFields(put, values, (uint8_t) (inst->bypass.engaged ? 1 : 0),
                          inst->selectedPreset.load(std::memory_order_relaxed),
                          (int32_t) inst->guiWidth, (int32_t) inst->guiHeight, inst->presets.selection(),
                          &bagBytes);
}

inline uint64_t presetSoundHash(const Instance* inst, int numParams = SONORE_NUM_PARAMS);

template <typename Get>
inline bool loadStateBody(Instance* inst, Get&& get, StateScope scope = StateScope::Session) {
  // A stream is allowed to return short reads, and treating one as EOF is how
  // half a session silently disappears.
  auto readExactly = [&](void* dst, size_t size) {
    uint8_t* p = static_cast<uint8_t*>(dst);
    while (size > 0) {
      const size_t got = get(p, size);
      if (got == 0) return false;
      p += got;
      size -= got;
    }
    return true;
  };

  StateHeader h{};
  if (!readExactly(&h, sizeof(h))) return false;
  if (std::memcmp(h.magic, "SNRS", 4) != 0) return false;
  if (h.version > kStateVersion) return false; // written by a newer build
  // A corrupt count is refused outright: the read loop below runs h.numParams
  // times, and a blob claiming four billion parameters would spin the loader
  // for as long as the stream keeps feeding it.
  if (h.numParams > (uint32_t) kMaxParams) return false;

  // Everything is read into locals FIRST and committed at the end. The first
  // version wrote each parameter into the instance as it was read, so a blob
  // truncated at the fourth value was "refused" with three values already
  // applied -- the half-applied load the refusal exists to prevent, found by
  // feeding the loader every truncation of a real session in the host test.
  float staged[SONORE_NUM_PARAMS > 0 ? SONORE_NUM_PARAMS : 1];

  // A parameter count mismatch is NOT fatal: load what both sides share and
  // leave the rest at its default, so an added control can't orphan a session.
  const uint32_t shared =
      h.numParams < (uint32_t) SONORE_NUM_PARAMS ? h.numParams : (uint32_t) SONORE_NUM_PARAMS;
  for (uint32_t i = 0; i < h.numParams; ++i) {
    float v = 0.0f;
    if (!readExactly(&v, sizeof(v))) return false;
    // Through the same clamp a host's edit goes through, which is also what
    // turns a NaN -- a blob is bytes, and bytes can spell one -- into the
    // default instead of the DSP's next buffer index.
    if (i < shared) staged[i] = clampToRange(kDesc.params[i], v);
  }
  // Anything the blob never heard of goes to its DEFAULT. It looks harmless
  // because a host usually loads into a fresh instance where those already
  // ARE the defaults. It stops looking harmless the moment a user loads a
  // preset into a plugin they have been turning knobs on: half the controls
  // come from the preset and half are left over, which is a state neither of
  // them ever saved.
  for (uint32_t i = shared; i < (uint32_t) SONORE_NUM_PARAMS; ++i)
    staged[i] = kDesc.params[i].defaultValue;

  bool bypassEngaged = false;
  if (h.version >= 2) {
    uint8_t bypassByte = 0;
    if (!readExactly(&bypassByte, 1)) return false;
    // Only the FLAG. The crossfade position is the audio thread's: a load
    // before activation is snapped to the flag by bypass.prepare(), and a
    // load while running -- which CLAP allows -- crossfades over 20 ms
    // instead of jumping under a process() call that is reading it.
    bypassEngaged = bypassByte != 0;
  }
  // A v1 blob predates the bypass entirely, so "off" is what it meant.
  // Leaving a bypass engaged because the instance happened to have it
  // engaged would restore a session into a state it was never saved in --
  // and silent output is the version of that bug a user notices immediately
  // and cannot explain.

  int32_t selectedPreset = 0;
  if (h.version >= 4) {
    int32_t preset = 0;
    if (!readExactly(&preset, sizeof(preset))) return false;
    // Restored, and NOT applied. The parameters have just been loaded from
    // the blob and they are the truth; re-applying the preset would throw
    // away every tweak the user made after choosing it, which is the state
    // they actually saved.
    selectedPreset = (preset >= 0 && preset < kDesc.numPresets) ? preset : 0;
  }
  // An older blob does not say, and guessing would put a name on the display
  // that the sound does not match: it stays at 0.

  // An older blob predates the editor size, so the plugin opens at the size
  // it was designed at -- which is what it did before this existed.
  uint32_t guiWidth = SONORE_UI_WIDTH, guiHeight = SONORE_UI_HEIGHT;
  if (h.version >= 5) {
    int32_t editorSize[2] = {0, 0};
    if (!readExactly(editorSize, sizeof(editorSize))) return false;
    // Sanity-checked rather than trusted. A blob that has been through a
    // careless host, or a session written by a build with a different
    // default, can carry a size no screen has -- and a window restored at
    // 40000 pixels is a window the user cannot reach the corner of to fix.
    if (editorSize[0] > 0 && editorSize[1] > 0) {
      // Through the same clamp every other size goes through. The first
      // version of this had its own bounds -- 120 pixels -- which let a
      // session restore an editor smaller than the resize path would ever
      // have produced.
      guiWidth = (uint32_t) editorSize[0];
      guiHeight = (uint32_t) editorSize[1];
      clampEditorSize(&guiWidth, &guiHeight);
    } else {
      // A size the blob does not vouch for keeps whatever the instance has,
      // as it always did.
      guiWidth = inst->guiWidth;
      guiHeight = inst->guiHeight;
    }
  }

  // v6: the preset the session was on. Lengths are checked against a cap
  // before anything is read for them: they come from the blob.
  PresetManager::Selection selection;
  if (h.version >= 6) {
    uint8_t origin = 0;
    int32_t factoryIndex = -1;
    uint32_t nameLength = 0, fileLength = 0;
    uint64_t baseline = 0;
    if (!readExactly(&origin, 1) || !readExactly(&factoryIndex, sizeof(factoryIndex)) ||
        !readExactly(&nameLength, 4) || nameLength > kMaxStatePresetText)
      return false;
    std::string name(nameLength, '\0');
    if (nameLength && !readExactly(&name[0], nameLength)) return false;
    if (!readExactly(&fileLength, 4) || fileLength > kMaxStatePresetText) return false;
    std::string file(fileLength, '\0');
    if (fileLength && !readExactly(&file[0], fileLength)) return false;
    if (!readExactly(&baseline, sizeof(baseline))) return false;
    // An origin this build does not know is no selection at all -- shown as
    // nothing rather than as a guess.
    if (origin == 1 && factoryIndex >= 0 && factoryIndex < kDesc.numPresets) {
      selection.origin = PresetManager::Origin::Factory;
      selection.factoryIndex = factoryIndex;
    } else if (origin == 2) {
      selection.origin = PresetManager::Origin::User;
      selection.fileName = cleanPresetName(file);
    }
    if (selection.origin != PresetManager::Origin::Unset) {
      selection.name = name;
      selection.baseline = baseline;
    }
  }
  // An older blob does not say, and the selection it leaves is none.

  StateBag bag;
  bool haveBag = false;
  if (h.version >= 3) {
    // The bag runs to the end of the stream, so it is read until the stream
    // stops giving rather than by a length we would have to keep in sync.
    // Read and checked whether or not this DSP keeps one: every blob carries
    // at least its count, and a blob cut inside it is as damaged as one cut
    // anywhere else.
    std::vector<uint8_t> bagBytes;
    uint8_t chunk[512];
    for (;;) {
      const size_t got = get(chunk, sizeof(chunk));
      if (got == 0) break;
      bagBytes.insert(bagBytes.end(), chunk, chunk + got);
    }
    // Nothing at all is a session saved before the DSP kept a bag, and loads
    // as one. Bytes that do not parse are a truncated or damaged session, and
    // are refused like any other part of the blob: they were loaded with
    // every parameter applied and the bag dropped, so a sampler reopened
    // with its settings and without its sample, and the host was told the
    // load had worked.
    if (!bagBytes.empty()) {
      if (!bag.deserialise(bagBytes.data(), bagBytes.size())) return false;
      haveBag = true;
    }
  }

  // Nothing above touched the instance. From here on nothing can fail.
  writeParamsTogether(inst, [&] {
    for (int i = 0; i < SONORE_NUM_PARAMS; ++i) inst->params[i] = staged[i];
  });
  inst->selectedPreset.store(selectedPreset, std::memory_order_relaxed);
  // The session's own, from a session only: see StateScope.
  if (scope == StateScope::Session) {
    inst->bypass.engaged = bypassEngaged;
    inst->guiWidth = guiWidth;
    inst->guiHeight = guiHeight;
  }
  if (h.version >= 3 && HasStateBag<SonoreDsp>::value) {
    if (haveBag) loadDspState(inst->dsp, bag);
    inst->uiStateDirty = true; // an open editor is now showing the wrong session
  } else if (HasStateBag<SonoreDsp>::value) {
    // An older blob carries no bag, which means the DSP's own state is not
    // described by it. Handing over an EMPTY one rather than skipping the call
    // keeps the rule the rest of this function follows: what the blob does not
    // say, the plugin returns to its default for. A sampler restoring a v2
    // session goes back to its built-in sound instead of keeping a file the
    // session never mentioned.
    StateBag empty;
    loadDspState(inst->dsp, empty);
  }
  if (scope != StateScope::Preset) {
    // A session from a build with fewer parameters carries that build's
    // hash, which covered its values only: every session reopened "modified"
    // after an update added a parameter, though the user had changed
    // nothing. Over the values it knew, and the DSP's state just restored,
    // the hash it saved is either matched -- the sound was the preset's, and
    // the baseline becomes this build's hash of it -- or not, and the
    // session was modified and stays so. After the DSP's load, so the hash
    // reads the state the session carries.
    if (selection.origin != PresetManager::Origin::Unset &&
        h.numParams < (uint32_t) SONORE_NUM_PARAMS &&
        presetSoundHash(inst, (int) h.numParams) == selection.baseline)
      selection.baseline = presetSoundHash(inst);
    inst->presets.restoreSelection(selection);
  }
  // The page holds its own copy of the values and would keep showing the old
  // ones otherwise.
  inst->uiEchoValid = false;
  return true;
}

inline bool stateSave(const clap_plugin_t* plugin, const clap_ostream_t* stream) {
  Instance* inst = self(plugin);
  return saveStateBody(inst, [stream](const void* data, size_t size) -> size_t {
    const int64_t wrote = stream->write(stream, data, size);
    return wrote > 0 ? (size_t) wrote : 0;
  });
}

inline bool stateLoad(const clap_plugin_t* plugin, const clap_istream_t* stream) {
  Instance* inst = self(plugin);
  if (!loadStateBody(inst, [stream](void* data, size_t size) -> size_t {
        const int64_t got = stream->read(stream, data, size);
        return got > 0 ? (size_t) got : 0;
      }))
    return false;

  // The host assumes parameter values only move when IT moves them; a state
  // load just rewrote all of them behind its back. CLAP's contract is to
  // announce that with a rescan -- without it a host's generic UI keeps
  // showing the pre-load values (clap-validator: "parameter values changed
  // without a rescan request"). Both state.load() and rescan() are
  // [main-thread], so the direct call is legal here.
  if (inst->host) {
    if (const auto* hostParams = static_cast<const clap_host_params_t*>(
            inst->host->get_extension(inst->host, CLAP_EXT_PARAMS)))
      hostParams->rescan(inst->host, CLAP_PARAM_RESCAN_VALUES);
  }
  return true;
}

inline const clap_plugin_state_t kStateExt = {stateSave, stateLoad};

// ── User presets ─────────────────────────────────────────────────────────────
//
// A preset is a state blob in a file (user_presets.h), and loading one is a
// state restore: through loadStateBody, the function a host's restore goes
// through, so the audio thread sees a preset arrive exactly as it sees a
// session arrive -- the whole parameter set in one step (writeParamsTogether)
// and the DSP's own state through its loadState. The only differences are the
// fields a preset must not carry over (StateScope::Preset) and who is told
// afterwards: the HOST did not ask for this, so it has to be told its
// parameters moved, the way each format says it (announceOwnStateLoad).

/** [main-thread] The blob, exactly as the host's save path writes it. */
inline bool captureStateBlob(const Instance* inst, std::vector<uint8_t>& out) {
  out.clear();
  return saveStateBody(inst, [&out](const void* data, size_t size) -> size_t {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    out.insert(out.end(), p, p + size);
    return size;
  });
}

/**
 * [main-thread] A hash of the SOUND: the parameters and the DSP's own state,
 * not the editor size, the bypass or the selection. What "modified" is
 * judged by, so resizing the window or bypassing the track does not mark a
 * preset changed. `numParams` fewer than this build's is the hash an older
 * build with that many parameters gave the same sound.
 */
inline uint64_t presetSoundHash(const Instance* inst, int numParams) {
  uint64_t h = 1469598103934665603ull; // FNV-1a
  auto mix = [&h](const void* data, size_t size) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) h = (h ^ p[i]) * 1099511628211ull;
  };
  for (int i = 0; i < SONORE_NUM_PARAMS && i < numParams; ++i) {
    const float v = inst->params[i];
    mix(&v, sizeof(v));
  }
  StateBag bag;
  saveDspState(inst->dsp, bag);
  std::vector<uint8_t> bytes;
  bag.serialise(bytes);
  mix(bytes.data(), bytes.size());
  return h;
}

/**
 * Factory preset `index` of `d`, through applyPreset, into `out`, which holds
 * `capacity` values. Refused -- false, nothing written -- when the descriptor
 * declares more parameters than that.
 *
 * applyPreset writes one value for every parameter the DESCRIPTOR declares,
 * and every array a wrapper hands it, like the parameters the values are then
 * copied to, holds SONORE_NUM_PARAMS. A release build loads a descriptor that
 * declares more (entryInit refuses one only in debug; see makeEditorHost), and
 * loading any of its presets wrote past the end of the array: the preset
 * discovery provider and the preset bar through factoryPresetState, and VST3's
 * program change on the controller, in process() and from the page. Refused,
 * as a stale preset is: a preset the arrays cannot hold is not one to apply
 * half of.
 */
inline bool factoryPresetValues(const PluginDescriptor& d, int index, float* out, int capacity) {
  if (!out || index < 0 || index >= d.numPresets || !d.presets) return false;
  if (d.numParams > capacity) return false;
  return applyPreset(d.presets[index], d.params, d.numParams, out);
}

/**
 * Factory preset `index` as a state blob: its values through applyPreset (the
 * one rule for a stale preset), the preset index, and NO bag -- a factory
 * preset is parameter values, and a blob without a bag leaves the DSP's own
 * state as it is, which is what loading one by index always did. Needs no
 * instance, so the preset discovery provider writes the same bytes.
 */
inline bool factoryPresetState(int index, std::vector<uint8_t>& out) {
  out.clear();
  if (index < 0 || index >= kDesc.numPresets || !kDesc.presets) return false;
  float values[SONORE_NUM_PARAMS > 0 ? SONORE_NUM_PARAMS : 1] = {};
  if (!factoryPresetValues(kDesc, index, values, SONORE_NUM_PARAMS)) return false;
  PresetManager::Selection selection;
  selection.origin = PresetManager::Origin::Factory;
  selection.factoryIndex = index;
  selection.name = kDesc.presets[index].name ? kDesc.presets[index].name : "";
  return writeStateFields([&out](const void* data, size_t size) -> size_t {
                            const uint8_t* p = static_cast<const uint8_t*>(data);
                            out.insert(out.end(), p, p + size);
                            return size;
                          },
                          values, 0, (int32_t) index, 0, 0, selection, nullptr);
}

/**
 * [main-thread] Tell the host the plugin changed its own state.
 *
 * CLAP, by clap_plugin_params' "loading a preset" scenario: rescan the
 * values (a host's generic panel and automation lanes otherwise go on showing
 * the old ones), mark the session dirty (the user changed it, and closing
 * without a prompt would lose it), and ask for a restart if the latency
 * moved -- a latency is announced in activate() and nowhere else. VST3 and AU
 * speak through their own objects and set onOwnStateLoad.
 */
inline void announceOwnStateLoad(Instance* inst) {
  inst->uiStateDirty = true;
  if (inst->onOwnStateLoad) {
    inst->onOwnStateLoad(inst->formatContext);
    return;
  }
  if (!inst->host) return;
  if (const auto* hostParams =
          static_cast<const clap_host_params_t*>(inst->host->get_extension(inst->host, CLAP_EXT_PARAMS)))
    hostParams->rescan(inst->host, CLAP_PARAM_RESCAN_VALUES);
  inst->stateDirty = true;
  markDirtyIfNeeded(inst);
  requestRestartIfLatencyChanged(inst);
}

/** [main-thread] A blob through the restore path, then the host told. */
inline bool applyOwnState(Instance* inst, const std::vector<uint8_t>& blob, StateScope scope, std::string* why) {
  size_t pos = 0;
  const bool ok = loadStateBody(
      inst,
      [&](void* data, size_t size) -> size_t {
        const size_t take = size < blob.size() - pos ? size : blob.size() - pos;
        if (take) std::memcpy(data, blob.data() + pos, take);
        pos += take;
        return take;
      },
      scope);
  if (!ok) {
    if (why)
      *why = "the plugin refused the state in it -- damaged, or saved by a newer version of the plugin";
    return false;
  }
  announceOwnStateLoad(inst);
  return true;
}

/** Where factory preset `index` is as a file, if the discovery provider has
 *  written it (syncFactoryPresetFiles); empty otherwise. */
inline std::string factoryPresetFile(int index) {
  if (index < 0 || index >= kDesc.numPresets || !kDesc.presets) return std::string();
  const std::string folder = defaultFactoryPresetFolder(kDesc.vendor ? kDesc.vendor : "", kDesc.name ? kDesc.name : "");
  if (folder.empty()) return std::string();
  char prefix[16];
  std::snprintf(prefix, sizeof(prefix), "%02d ", index + 1);
  const std::string path = joinPath(folder, std::string(prefix) +
                                                presetFileStem(kDesc.presets[index].name ? kDesc.presets[index].name : "") +
                                                userPresetExtension());
  return fileExists(path) ? path : std::string();
}

/**
 * [main-thread] After the PLUGIN loaded a preset (its own browser, not the
 * host's): tell a CLAP host which one, through clap_host_preset_load's
 * "loaded", so the host's preset browser can show the same preset as the
 * plugin's. A preset the host loaded through preset-load is not reported
 * back -- the host already knows.
 */
inline void announcePresetToHost(Instance* inst, const PresetManager::Selection& selection) {
  if (inst->onPresetSelected) inst->onPresetSelected(inst->formatContext);
  if (!inst->host) return;
  const auto* ext = static_cast<const clap_host_preset_load_t*>(
      inst->host->get_extension(inst->host, CLAP_EXT_PRESET_LOAD));
  if (!ext) ext = static_cast<const clap_host_preset_load_t*>(
      inst->host->get_extension(inst->host, CLAP_EXT_PRESET_LOAD_COMPAT));
  if (!ext || !ext->loaded) return;
  if (selection.origin == PresetManager::Origin::User && !selection.fileName.empty()) {
    const std::string path = joinPath(inst->presets.store().folder(), selection.fileName);
    ext->loaded(inst->host, CLAP_PRESET_DISCOVERY_LOCATION_FILE, path.c_str(), nullptr);
  } else if (selection.origin == PresetManager::Origin::Factory) {
    const std::string file = factoryPresetFile(selection.factoryIndex);
    if (!file.empty()) {
      ext->loaded(inst->host, CLAP_PRESET_DISCOVERY_LOCATION_FILE, file.c_str(), nullptr);
    } else {
      char key[32];
      std::snprintf(key, sizeof(key), "%d", selection.factoryIndex);
      ext->loaded(inst->host, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr, key);
    }
  }
}

/** The descriptor's identity, as the preset files carry it. */
inline PresetManager::Identity presetIdentity() {
  PresetManager::Identity id;
  id.pluginId = kDesc.id ? kDesc.id : "";
  id.pluginName = kDesc.name ? kDesc.name : "";
  id.vendor = kDesc.vendor ? kDesc.vendor : "";
  id.version = kDesc.version ? kDesc.version : "";
  for (int i = 0; kDesc.presets && i < kDesc.numPresets; ++i)
    id.factoryNames.push_back(kDesc.presets[i].name ? kDesc.presets[i].name : "");
  return id;
}

/** [main-thread] This instance's presets, set up the first time anything
 *  asks: an editor opening, a host loading a preset. */
inline PresetManager& presetsOf(Instance* inst) {
  PresetManager& presets = inst->presets;
  if (presets.isConfigured()) return presets;
  presets.configure(presetIdentity());
  PresetManager::Hooks hooks;
  hooks.saveState = [inst](std::vector<uint8_t>& out) { return captureStateBlob(inst, out); };
  hooks.loadState = [inst](const std::vector<uint8_t>& blob, PresetManager::LoadScope scope, std::string* why) {
    return applyOwnState(inst, blob,
                         scope == PresetManager::LoadScope::Preset ? StateScope::Preset : StateScope::Restore, why);
  };
  hooks.factoryState = [](int index, std::vector<uint8_t>& out) { return factoryPresetState(index, out); };
  hooks.soundHash = [inst]() { return presetSoundHash(inst); };
  hooks.loaded = [inst](const PresetManager::Selection& s) { announcePresetToHost(inst, s); };
  presets.setHooks(std::move(hooks));
  return presets;
}

// ── Latency ──────────────────────────────────────────────────────────────────
//
// A DSP that delays its signal MUST say so, or the host cannot time-align it
// against the rest of the session: a look-ahead limiter or a convolution
// reverb that reports zero smears every parallel mix it sits in, silently.
//
// So this is not a constant: a DSP declares `int latencySamples() const` and
// the wrapper reports it. Detected like the other optional hooks, so anything
// zero-latency (most designs) neither declares nor pays for it.
/** Whether T reports a latency: true when it declares
 *  `int latencySamples() const`, in samples at the current rate. The wrapper
 *  reports it to the host, delays the bypass dry path by it, and asks for a
 *  restart when it changes. Without the const it is not seen, which a
 *  static_assert below reports. */
template <typename T, typename = void>
struct HasLatency : std::false_type {};
/** The match for HasLatency: T declares `latencySamples() const`. */
template <typename T>
struct HasLatency<T, std::void_t<decltype(std::declval<const T&>().latencySamples())>>
    : std::true_type {};

template <typename T>
inline uint32_t dspLatency(const T& dsp) {
  if constexpr (HasLatency<T>::value) {
    const int n = dsp.latencySamples();
    return n > 0 ? (uint32_t) n : 0u;
  } else {
    (void) dsp;
    return 0;
  }
}

// ── Gain reduction ───────────────────────────────────────────────────────────
//
// How far the DSP is turning the signal down, for a compressor's meter.
// Nothing about the OUTPUT can supply it: no measurement of the output tells
// 6 dB of compression from a gain knob, so the DSP has to say. Every dynamics
// class in the SDK answers gainReduction() in positive decibels, so a plugin
// forwards its own:
//
//     float gainReduction() const { return comp.gainReduction(); }
//
// Detected like latency, so a DSP that does not compress declares nothing and
// the meter rests on 0, which is what a compressor's meter shows when it is
// not compressing, not a fault.
/** Whether T reports gain reduction: true when it declares
 *  `gainReduction() const`, in positive decibels. The editors show it as
 *  `sonore.gr`. Without the const it is not seen, which a static_assert
 *  below reports. */
template <typename T, typename = void>
struct HasGainReduction : std::false_type {};
/** The match for HasGainReduction: T declares `gainReduction() const`. */
template <typename T>
struct HasGainReduction<T, std::void_t<decltype(std::declval<const T&>().gainReduction())>>
    : std::true_type {};

/** The DSP's gain reduction in positive decibels, 0 when it declares none.
 *  A NaN, a negative number or a runaway is not a reading and reads 0. */
template <typename T>
inline float dspGainReduction(const T& dsp) {
  if constexpr (HasGainReduction<T>::value) {
    const float g = (float) dsp.gainReduction();
    return (g > 0.0f && g < 200.0f) ? g : 0.0f;
  } else {
    (void) dsp;
    return 0.0f;
  }
}

// ── The state the host cannot see ────────────────────────────────────────────
//
// A host knows the session changed when it MOVED something: a parameter it
// automated, a preset it loaded, a state it restored. It has no idea about
// anything the plugin did on its own: a sampler that loaded a file through
// its own browser, a convolver given a new impulse, anything at all that
// lives in the StateBag rather than in a parameter.
//
// So the session is not marked dirty, the DAW closes without asking, and the
// work is gone. There is no error message and nothing to diagnose; the user
// simply loaded a sample and then lost it.
//
// A DSP says so by declaring `bool consumeStateDirty()`, which returns true
// ONCE per change and clears itself. Detected like every other optional hook,
// so a DSP with no hidden state neither declares nor pays for it.
/** Whether T can say its own state changed: true when it declares
 *  `bool consumeStateDirty()`, which must return true once per change and
 *  clear itself. The wrapper polls it at the end of every block -- on the
 *  audio thread, so it must not lock or allocate -- and has the host's
 *  session marked dirty from the main thread. */
template <typename T, typename = void>
struct HasStateDirty : std::false_type {};
/** The match for HasStateDirty: T declares consumeStateDirty(). */
template <typename T>
struct HasStateDirty<T, std::void_t<decltype(std::declval<T&>().consumeStateDirty())>>
    : std::true_type {};

template <typename T>
inline bool dspConsumeStateDirty(T& dsp) {
  if constexpr (HasStateDirty<T>::value) {
    return dsp.consumeStateDirty();
  } else {
    (void) dsp;
    return false;
  }
}

/**
 * [main-thread] Tell the host the session changed underneath it.
 *
 * mark_dirty is [main-thread], and process() is not, so the audio thread
 * sets a flag and asks to be called here, which is what on_main_thread is
 * for. Losing a notification would be losing a save prompt, so the flag is
 * only cleared once the host has actually been told.
 */
inline void markDirtyIfNeeded(Instance* inst) {
  if (!inst->stateDirty) return;
  if (!inst->host) {
    inst->stateDirty = false;
    return;
  }
  if (const auto* ext = static_cast<const clap_host_state_t*>(
          inst->host->get_extension(inst->host, CLAP_EXT_STATE))) {
    ext->mark_dirty(inst->host);
    inst->stateDirty = false;
  }
}

/** [main-thread] Where the output guard's report goes when no host log takes
 *  it: stderr, and on Windows the debugger's output as well -- a plugin is a
 *  DLL inside a GUI host, whose stderr is usually connected to nothing, and
 *  a developer chasing the bug has a debugger attached. */
inline void writeOutputGuardReport(const char* message) {
  std::fprintf(stderr, "%s\n", message);
  std::fflush(stderr);
#if defined(_WIN32)
  OutputDebugStringA(message);
  OutputDebugStringA("\n");
#endif
}

/**
 * [main-thread] Say, once, that the output guard silenced something.
 *
 * The audio thread counts the trips and, the first time, asks to be called
 * here (guardOutputs). A warning in the host's log where it has one
 * (CLAP_EXT_LOG), stderr otherwise; once per instance, naming the plugin and
 * how many blocks it has lost so far. The formats with no request_callback
 * call this from whatever main-thread hook they already poll: the count is
 * there whenever they look.
 */
inline void reportOutputGuard(Instance* inst) {
  char message[400];
  if (!inst->outputGuard.takeReport(kDesc.name, message, sizeof(message))) return;
  if (inst->host) {
    if (const auto* log = static_cast<const clap_host_log_t*>(
            inst->host->get_extension(inst->host, CLAP_EXT_LOG))) {
      if (log->log) {
        log->log(inst->host, CLAP_LOG_WARNING, message);
        return;
      }
    }
  }
  writeOutputGuardReport(message);
}

/**
 * ANNOUNCE a new latency. Legal in exactly one place.
 *
 * The header says "The latency is only allowed to change during
 * plugin->activate", and clap-validator means it literally: not "while
 * deactivated", but WITHIN the activate call. A host cannot re-plan its delay
 * compensation under a running graph, and outside activate it has no defined
 * moment to do the re-planning in.
 *
 * So this is called from activate() and nowhere else. The first version
 * called it from the parameter flush too -- legal-looking, since the plugin
 * may well be deactivated when a knob is turned with the transport stopped,
 * and refused all the same.
 */
inline void announceLatency(Instance* inst) {
  if (!HasLatency<SonoreDsp>::value) return;
  const uint32_t latency = dspLatency(inst->dsp);
  if (latency == inst->reportedLatency) return;
  inst->reportedLatency = latency;
  if (!inst->host) return;
  if (const auto* ext =
          static_cast<const clap_host_latency_t*>(inst->host->get_extension(inst->host,
                                                                           CLAP_EXT_LATENCY)))
    ext->changed(inst->host);
}

/**
 * ASK to be restarted, because the latency moved and this is not activate.
 *
 * The other half of the same rule, and the only thing a plugin may do about a
 * latency change anywhere else. request_restart is [thread-safe], which is
 * what lets process() and the parameter flush share one function instead of
 * needing a main-thread callback between them.
 *
 * The reported value is deliberately NOT updated here: the restart lands in
 * activate(), which is where the new number is allowed to be spoken.
 */
inline void requestRestartIfLatencyChanged(Instance* inst) {
  if constexpr (HasLatency<SonoreDsp>::value) { // see numAuxOutputs()
    if (!inst->host) return;
    if (dspLatency(inst->dsp) == inst->reportedLatency) return;
    inst->host->request_restart(inst->host);
  } else {
    (void) inst;
  }
}

inline uint32_t latencyGet(const clap_plugin_t* plugin) {
  return dspLatency(self(plugin)->dsp);
}
inline const clap_plugin_latency_t kLatencyExt = {latencyGet};

// ── Tail ─────────────────────────────────────────────────────────────────────
//
// A reverb or delay keeps sounding after its input stops; a host that does not
// know for how long will cut the decay when the transport stops or when it
// renders. Same trait pattern as latency: a DSP that rings declares
// `tailSamples()`, everything else neither declares nor pays for it.
/** Whether T rings on after its input stops: true when it declares
 *  `tailSamples() const`, in samples. Like latencySamples, a declaration
 *  without the const is not seen, and a static_assert below says so. */
template <typename T, typename = void>
struct HasTail : std::false_type {};
/** The match for HasTail: T declares `tailSamples() const`. */
template <typename T>
struct HasTail<T, std::void_t<decltype(std::declval<const T&>().tailSamples())>>
    : std::true_type {};

// A hook declared without its const is a hook no wrapper sees: the traits
// above ask a const DSP. `int latencySamples()` was ignored without a word and
// the host told no latency; `tailSamples()` no tail; a non-const saveState()
// meant the state was never saved and loadState() never called. Said here,
// at compile time, instead.
namespace hookdetail {
template <typename T, typename = void>
struct AnyLatency : std::false_type {};
template <typename T>
struct AnyLatency<T, std::void_t<decltype(std::declval<T&>().latencySamples())>> : std::true_type {};
template <typename T, typename = void>
struct AnyTail : std::false_type {};
template <typename T>
struct AnyTail<T, std::void_t<decltype(std::declval<T&>().tailSamples())>> : std::true_type {};
template <typename T, typename = void>
struct AnySaveState : std::false_type {};
template <typename T>
struct AnySaveState<T, std::void_t<decltype(std::declval<T&>().saveState(std::declval<StateBag&>()))>>
    : std::true_type {};
template <typename T, typename = void>
struct AnyGainReduction : std::false_type {};
template <typename T>
struct AnyGainReduction<T, std::void_t<decltype(std::declval<T&>().gainReduction())>> : std::true_type {};
} // namespace hookdetail
static_assert(HasLatency<SonoreDsp>::value || !hookdetail::AnyLatency<SonoreDsp>::value,
              "SonoreDsp::latencySamples() must be const -- `int latencySamples() const` -- or no "
              "wrapper sees it and the host is told there is no latency");
static_assert(HasTail<SonoreDsp>::value || !hookdetail::AnyTail<SonoreDsp>::value,
              "SonoreDsp::tailSamples() must be const -- `int tailSamples() const` -- or no wrapper "
              "sees it and the host is told there is no tail");
static_assert(HasStateBag<SonoreDsp>::value || !hookdetail::AnySaveState<SonoreDsp>::value,
              "SonoreDsp::saveState() must be const -- `void saveState(sonore::StateBag&) const` -- "
              "or the state is never saved and loadState() never called");
static_assert(HasGainReduction<SonoreDsp>::value || !hookdetail::AnyGainReduction<SonoreDsp>::value,
              "SonoreDsp::gainReduction() must be const -- `float gainReduction() const` -- or no "
              "wrapper sees it and the meter reads no reduction");

template <typename T>
inline uint32_t dspTail(const T& dsp) {
  if constexpr (HasTail<T>::value) {
    const int n = dsp.tailSamples();
    return n > 0 ? (uint32_t) n : 0u;
  } else {
    (void) dsp;
    return 0;
  }
}

/**
 * Tell the host the tail changed. From the AUDIO THREAD, which is the
 * interesting part.
 *
 * Every other notification in this wrapper is [main-thread] and has to be
 * smuggled off the audio thread through request_callback or a host's own
 * parameter poll. This one is the reverse: the header marks
 * clap_host_tail::changed as [audio-thread], so process() is not merely
 * allowed to call it, it is the right place.
 *
 * Which makes sense once you see what each is for. A latency change forces
 * the host to re-plan its delay compensation, which it cannot do under a
 * running graph. A tail is only a hint about how long to keep processing
 * after the transport stops -- nothing has to be re-planned, so nothing has
 * to stop.
 *
 * It matters for exactly one thing and it matters a lot there: a reverb whose
 * decay is a parameter has a tail that moves, and a host bouncing offline
 * with a stale short tail cuts the reverb off mid-decay.
 */
inline void announceTailIfChanged(Instance* inst) {
  if constexpr (HasTail<SonoreDsp>::value) { // see numAuxOutputs()
    if (!inst->host) return;
    const uint32_t tail = dspTail(inst->dsp);
    if (inst->tailReported && tail == inst->reportedTail) return;
    inst->reportedTail = tail;
    inst->tailReported = true;
    if (const auto* ext =
            static_cast<const clap_host_tail_t*>(inst->host->get_extension(inst->host,
                                                                          CLAP_EXT_TAIL)))
      ext->changed(inst->host);
  } else {
    (void) inst;
  }
}

inline uint32_t tailGet(const clap_plugin_t* plugin) {
  return dspTail(self(plugin)->dsp);
}
inline const clap_plugin_tail_t kTailExt = {tailGet};

// ── Which track this instance is on ──────────────────────────────
//
// CLAP hands this over in two halves: the host tells the plugin something
// changed, and the plugin then asks what it changed TO. The asking half is an
// extension the host provides, so a host that sends changed() without
// offering the other half -- which is legal and does happen -- gets a plugin
// that quietly keeps what it had rather than one that clears the track name
// it was previously told.
//
// [main-thread] on both sides, which is why the DSP is called directly here
// and not through the parameter queue.

inline void refreshTrackInfo(Instance* inst) {
  if (!inst->host) return;
  const auto* ext = static_cast<const clap_host_track_info_t*>(
      inst->host->get_extension(inst->host, CLAP_EXT_TRACK_INFO));
  if (!ext && inst->host->get_extension)
    ext = static_cast<const clap_host_track_info_t*>(
        inst->host->get_extension(inst->host, CLAP_EXT_TRACK_INFO_COMPAT));
  if (!ext || !ext->get) return;

  clap_track_info_t raw{};
  if (!ext->get(inst->host, &raw)) return; // the host declined; keep what we had

  TrackInfo info;
  if (raw.flags & CLAP_TRACK_INFO_HAS_TRACK_NAME) {
    info.hasName = true;
    raw.name[CLAP_NAME_SIZE - 1] = 0; // a host that fills the field exactly
    info.name = raw.name;
  }
  if (raw.flags & CLAP_TRACK_INFO_HAS_TRACK_COLOR) {
    info.hasColour = true;
    info.red = (unsigned char) raw.color.red;
    info.green = (unsigned char) raw.color.green;
    info.blue = (unsigned char) raw.color.blue;
    info.alpha = (unsigned char) raw.color.alpha;
  }
  if (raw.flags & CLAP_TRACK_INFO_HAS_AUDIO_CHANNEL)
    info.audioChannelCount = (int) raw.audio_channel_count;
  info.isReturnTrack = (raw.flags & CLAP_TRACK_INFO_IS_FOR_RETURN_TRACK) != 0;
  info.isBus = (raw.flags & CLAP_TRACK_INFO_IS_FOR_BUS) != 0;
  info.isMaster = (raw.flags & CLAP_TRACK_INFO_IS_FOR_MASTER) != 0;

  inst->trackInfo = info;
  sendTrackInfo(inst->dsp, inst->trackInfo);
}

inline void trackInfoChanged(const clap_plugin_t* plugin) { refreshTrackInfo(self(plugin)); }

inline const clap_plugin_track_info_t kTrackInfoExt = {trackInfoChanged};

// ── The plugin itself ────────────────────────────────────────────────────────

inline bool pluginInit(const clap_plugin_t* plugin) {
  Instance* inst = self(plugin);
  inst->mainChannels = defaultMainChannels();
  inst->setMainLayout(inst->mainChannels, nullptr);
  for (int i = 0; i < SONORE_NUM_PARAMS && i < kDesc.numParams; ++i)
    inst->params[i] = kDesc.params[i].defaultValue;
  // Asked once up front, now that get_extension is legal (it is not in
  // factoryCreate). A reverb landing on a return track wants to start wet, and
  // by the time the host's first changed() arrives -- if it sends one at all,
  // which is not required -- the user has already heard the wrong default.
  refreshTrackInfo(inst);
  return true;
}

inline void pluginDestroy(const clap_plugin_t* plugin) {
  Instance* inst = self(plugin);
  inst->~Instance();
  std::free(inst);
}

inline bool pluginActivate(const clap_plugin_t* plugin, double sampleRate,
                           uint32_t /*minFrames*/, uint32_t maxFrames) {
  Instance* inst = self(plugin);
  inst->sampleRate = sampleRate;
  inst->maxFrames = maxFrames;
  // Through prepareDsp, like every other format and like reset(): it is
  // where the load meter learns this rate, held notes are let go and a
  // licensed build's gate is told the rate. Calling the DSP's prepare()
  // directly skipped all three, so after activating at 96 kHz the editor's
  // CPU meter judged every block against a 48 kHz deadline.
  // Whatever was queued for a block before this activation is not for one
  // after it, and the notes it was for are gone.
  discardEvents(inst);
  forgetHostNotes(inst);
  requeueWideMods(inst);
  prepareDsp(inst); // the one place allocation is allowed
  inst->bypass.prepare(sampleRate, maxFrames, dspLatency(inst->dsp), inst->mainChannels);
  if (TakesSidechain<SonoreDsp>::value) inst->scSilence.assign(maxFrames > 0 ? maxFrames : 1, 0.0f);
  if constexpr (TakesSidechain<SonoreDsp>::value && supportsDouble()) // see numAuxOutputs()
    inst->scSilence64.assign(maxFrames > 0 ? maxFrames : 1, 0.0);
  // The one moment changed() may be called.
  announceLatency(inst);
  inst->activated = true;

  // Look-ahead and block latencies are expressed in SAMPLES, so they change
  // with the rate. Telling the host only at load time would leave it
  // compensating by the wrong amount for the rest of the session.
  return true;
}

inline void pluginDeactivate(const clap_plugin_t* plugin) { self(plugin)->activated = false; }
inline bool pluginStartProcessing(const clap_plugin_t*) { return true; }
inline void pluginStopProcessing(const clap_plugin_t*) {}

inline void pluginReset(const clap_plugin_t* plugin) {
  Instance* inst = self(plugin);
  discardEvents(inst);
  forgetHostNotes(inst);
  requeueWideMods(inst);
  // A reset means the plugin is not in the middle of ANYTHING, and a bypass
  // crossfade in flight is something. Snapping it to its target rather than
  // letting it keep ramping is what makes a reset deterministic: the host
  // test caught the difference as telemetry that drifted by a few percent for
  // several blocks after every reset, which is the sort of wrongness that
  // never quite looks like a bug.
  inst->bypass.mix = inst->bypass.engaged ? 1.0f : 0.0f;
  inst->bypass.pos = 0;
  for (uint32_t c = 0; c < kMaxAudioChannels; ++c)
    for (BypassState::RingSample& v : inst->bypass.ring[c]) v = 0;
  // On the audio thread: the DSP's reset() if it has one, prepare() if not.
  resetDsp(inst);
}

/** constant_mask is an OUTPUT field on an output buffer: the plugin says
 *  which channels it left constant. The main port always said 0; the aux
 *  ports said whatever the host's memory held, and a host that reads a set
 *  bit skips a channel the DSP wrote. */
inline void clearAuxConstantMasks(const clap_process_t* process, uint32_t numAux) {
  for (uint32_t b = 0; b < numAux; ++b)
    if (1 + b < process->audio_outputs_count) process->audio_outputs[1 + b].constant_mask = 0;
}

/**
 * Everything a process() call owes the host AFTER the DSP has run, at either
 * precision. One function, called from both paths, because the 64-bit path
 * used to return as soon as its meters were fed and so did none of it: a
 * double-precision host got no MIDI out of an arpeggiator, was never asked to
 * restart for a latency change, never heard of a new tail, and never learned
 * the session had changed.
 */
inline void finishProcess(Instance* inst, const clap_process_t* process, uint32_t frames) {
  // The DSP has had this block's events.
  discardEvents(inst);
  // Emitted MIDI goes out as raw MIDI events, the dialect our output port
  // declares. Offsets are clamped into the block: an event scheduled past the
  // end is a host-visible protocol error, not something to pass along.
  //
  // In TIME ORDER, short messages and SysEx merged, which is what CLAP
  // requires of an output list. The SysEx used to go out after every short
  // message, so a SysEx early in the block followed a note-off late in it.
  //
  // The notes the DSP reports ended go out in the same list, as NOTE_END on
  // the note port they came in on: how a host that modulates per note learns
  // that a voice is gone, release tail and all, and stops modulating it.
  const bool sendsNoteEnds = kDesc.isInstrument && !inst->endedNotes.isEmpty();
  if ((kDesc.producesMidi || sendsNoteEnds) && process->out_events) {
    auto clampTime = [frames](int position) {
      uint32_t t = (uint32_t) (position < 0 ? 0 : position);
      return t >= frames ? frames - 1 : t;
    };
    const MidiBuffer::Entry* e = kDesc.producesMidi ? inst->midiOut.begin() : nullptr;
    const MidiBuffer::Entry* const eEnd = kDesc.producesMidi ? inst->midiOut.end() : nullptr;
    const MidiBuffer::SysexEntry* x = kDesc.producesMidi ? inst->midiOut.sysexBegin() : nullptr;
    const MidiBuffer::SysexEntry* const xEnd = kDesc.producesMidi ? inst->midiOut.sysexEnd() : nullptr;
    const NoteIdBuffer::Entry* n = sendsNoteEnds ? inst->endedNotes.begin() : nullptr;
    const NoteIdBuffer::Entry* const nEnd = sendsNoteEnds ? inst->endedNotes.end() : nullptr;
    while (e != eEnd || x != xEnd || n != nEnd) {
      if (n != nEnd && (e == eEnd || n->samplePosition < e->samplePosition) &&
          (x == xEnd || n->samplePosition < x->samplePosition)) {
        clap_event_note_t ev{};
        ev.header.size = sizeof(ev);
        ev.header.type = CLAP_EVENT_NOTE_END;
        ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        ev.header.time = clampTime(n->samplePosition);
        ev.note_id = n->noteId;
        ev.port_index = 0;
        ev.channel = n->channel;
        ev.key = n->key;
        ev.velocity = 0.0;
        process->out_events->try_push(process->out_events, &ev.header);
        ++n;
      } else if (x == xEnd || (e != eEnd && e->samplePosition <= x->samplePosition)) {
        clap_event_midi_t ev{};
        ev.header.size = sizeof(ev);
        ev.header.type = CLAP_EVENT_MIDI;
        ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        ev.header.time = clampTime(e->samplePosition);
        ev.port_index = 0;
        ev.data[0] = (uint8_t) e->message.getRawStatus();
        ev.data[1] = (uint8_t) e->message.getRawData1();
        ev.data[2] = (uint8_t) e->message.getRawData2();
        process->out_events->try_push(process->out_events, &ev.header);
        ++e;
      } else {
        // The event carries a POINTER, and CLAP requires it to stay valid
        // until process() returns -- which is exactly why these bytes live in
        // the Instance's own arena rather than in a temporary here.
        clap_event_midi_sysex_t ev{};
        ev.header.size = sizeof(ev);
        ev.header.type = CLAP_EVENT_MIDI_SYSEX;
        ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        ev.header.time = clampTime(x->samplePosition);
        ev.port_index = 0;
        ev.buffer = inst->midiOut.sysexData(*x);
        ev.size = x->length;
        process->out_events->try_push(process->out_events, &ev.header);
        ++x;
      }
    }
  }

  // A note that ended gives up its name once the host has let it go too --
  // the slot that is still its own: a host may have given the id to a new
  // note on another key since.
  for (const NoteIdBuffer::Entry& ended : inst->endedNotes) {
    if (ended.noteId < 0) continue;
    for (Instance::NoteIdSlot& slot : inst->noteIds) {
      if (slot.id != ended.noteId || slot.key != ended.key || slot.channel != ended.channel) continue;
      slot.ended = true;
      if (slot.released) slot.id = -1;
    }
  }

  // The latency may have moved this block. A plugin cannot announce that from
  // here -- it asks to be restarted, which is thread-safe and therefore legal
  // from the audio thread.
  requestRestartIfLatencyChanged(inst);

  // So may the voices the patch can use (a Voices control), which a host
  // reads through voice-info and is told about on the main thread.
  if (kDesc.isInstrument) publishVoiceCount(inst, inst->dsp);

  // The tail, by contrast, is announced from HERE. clap_host_tail::changed is
  // [audio-thread] -- the only notification in this wrapper that is.
  announceTailIfChanged(inst);

  // And the DSP may have changed state the host has no other way to learn
  // about. Same problem, different call: mark_dirty is [main-thread], so the
  // news is recorded and a callback asked for.
  if (dspConsumeStateDirty(inst->dsp)) {
    inst->stateDirty = true;
    if (inst->host) inst->host->request_callback(inst->host);
  }
}

/**
 * [audio-thread] The output guard (output_guard.h) over everything a block
 * hands back: the main bus's channels and every aux bus. A NaN or an infinity
 * on any of them and all of them leave as silence.
 *
 * Called after the bypass, so what is checked is what the host gets, and
 * before the meters and the scope, so an editor shows that silence rather
 * than a NaN. The first trip asks for the main thread, which says so once
 * (reportOutputGuard); request_callback is [thread-safe]. Every format's
 * process path calls this, and only CLAP has a host to ask: the others find
 * the count from a main-thread hook of their own, or -- LV2's worker -- act
 * on the true this returns for that first trip.
 */
template <typename Sample>
inline bool guardOutputs(Instance* inst, Sample* const* main, uint32_t numMain,
                         const AudioBlock<Sample>* aux, uint32_t numAux, uint32_t frames) {
  if (!kDesc.guardOutput) return false;
  if (!inst->outputGuard.check(main, numMain, aux, numAux, (size_t) frames)) return false;
  if (inst->host) inst->host->request_callback(inst->host);
  return true;
}

/** One block of at most maxFrames: the whole of process(), for a host that
 *  keeps to what it promised in activate(), and one piece of it otherwise
 *  (see pluginProcess). An oversized block is guarded piece by piece, each
 *  as the block the DSP ran. */
inline clap_process_status processBlock(Instance* inst, const clap_process_t* process) {
  // The clock starts before anything in the block, so what is measured is
  // what the HOST waited for: event handling and bus setup, not just the DSP.
  const auto blockStart = std::chrono::steady_clock::now();
  const uint32_t frames = process->frames_count;

  // Drain the whole event list up front. Notes keep their sample offsets (the
  // DSP applies them per-frame), so a synth stays sample-accurate; parameters
  // land at block granularity, which is what smoothing is for.
  //
  // ADDED to what is already waiting, which is cleared once a DSP has had it
  // (finishProcess). It was cleared here, before anything was added, and two
  // calls leave notes no DSP has seen yet: a zero-length process(), which
  // decoded its events and returned, and params.flush(), which drains the
  // editor's keyboard. The next block threw both away, a note-off among them,
  // and the note hung.
  if (process->in_events) {
    const uint32_t n = process->in_events->size(process->in_events);
    for (uint32_t i = 0; i < n; ++i)
      applyEvent(inst, process->in_events->get(process->in_events, i), frames);
  }
  // No block to play them in: they wait, at time 0, for the next one.
  if (frames == 0) return CLAP_PROCESS_CONTINUE;

  // Whatever the user did to the INTERFACE since the last block. These go back
  // out to the host as live events too, which is what makes turning a knob on
  // our webview recordable as automation exactly like a host-side control.
  drainUiEvents(inst, process->out_events);

  // The block's parameters, once, the plugin-wide modulation added: what the
  // decoder reads the bend range from and what the DSP is handed below.
  snapshotParams(inst);
  const float* const blockParams = dspParams(inst);

  // Raw-MIDI hosts express per note through MPE's channel layout instead of
  // through typed events, so the same block is read both ways. After the
  // interface's events, so a key played on the editor's keyboard is a note
  // the decoder knows: a bend on its channel reaches it.
  decodeMpe(inst, inst->midi, blockParams);

  if (process->audio_outputs_count < 1) {
    discardEvents(inst);
    requeueWideMods(inst);
    return CLAP_PROCESS_CONTINUE;
  }

  clap_audio_buffer_t* out = &process->audio_outputs[0];

  // ── 64-bit path ───────────────────────────────────────────────────────────
  // Taken only when the host handed us doubles AND the DSP genuinely speaks
  // them. Everything the float path does happens here too -- bypass (through
  // the shared exact ring), aux buses, meters -- at the host's precision, and
  // finishProcess() after it, which this path once returned before.
  if constexpr (supportsDouble()) {
    if (!out->data32 && out->data64 && out->channel_count >= 1) {
      uint32_t nch64 = inst->mainChannels;
      if (out->channel_count < nch64) nch64 = out->channel_count;
      if (nch64 > kMaxAudioChannels) nch64 = kMaxAudioChannels;
      double* chans64[kMaxAudioChannels];
      for (uint32_t c = 0; c < nch64; ++c) chans64[c] = out->data64[c];
      for (uint32_t c = nch64; c < out->channel_count; ++c)
        if (out->data64[c]) std::memset(out->data64[c], 0, sizeof(double) * frames);

      if (kDesc.isInstrument) {
        for (uint32_t c = 0; c < nch64; ++c)
          std::memset(chans64[c], 0, sizeof(double) * frames);
      } else if (process->audio_inputs_count > 0 && process->audio_inputs[0].data64 &&
                 process->audio_inputs[0].channel_count > 0) {
        const clap_audio_buffer_t* in = &process->audio_inputs[0];
        for (uint32_t c = 0; c < nch64; ++c) {
          const double* src = in->data64[c < in->channel_count ? c : in->channel_count - 1];
          if (src && src != chans64[c]) std::memcpy(chans64[c], src, sizeof(double) * frames);
        }
      } else {
        // No input to copy -- none connected, or one with no channels, whose
        // count - 1 would have indexed channel 4294967295. Silence, not
        // whatever the output buffer held.
        for (uint32_t c = 0; c < nch64; ++c)
          std::memset(chans64[c], 0, sizeof(double) * frames);
      }

      sendTransport(inst->dsp, readTransport(process->transport));

      double* zero64 = inst->scSilence64.empty() ? nullptr : inst->scSilence64.data();
      double* scChans64[2] = {zero64, zero64};
      if (process->audio_inputs_count > 1 && process->audio_inputs[1].data64 &&
          process->audio_inputs[1].channel_count > 0) {
        const clap_audio_buffer_t* sc = &process->audio_inputs[1];
        scChans64[0] = sc->data64[0];
        scChans64[1] = sc->data64[sc->channel_count > 1 ? 1 : 0];
      }
      AudioBlock<double> scBlock64(scChans64, 2, scChans64[0] ? frames : 0);

      AudioBlock<double>* const auxBlocks64 = inst->aux64.blocks;
      const uint32_t nAux64 = numAuxOutputs();
      for (uint32_t b = 0; b < nAux64; ++b) {
        double** const auxRow = inst->aux64.row(b);
        uint32_t width = 0;
        const uint32_t portIndex = 1 + b;
        if (portIndex < process->audio_outputs_count) {
          const clap_audio_buffer_t& ab = process->audio_outputs[portIndex];
          if (ab.data64) {
            width = ab.channel_count;
            const uint32_t want = auxBusChannels(b);
            if (width > want) width = want;
            if (width > kMaxAudioChannels) width = kMaxAudioChannels;
            for (uint32_t c = 0; c < width; ++c) auxRow[c] = ab.data64[c];
          }
        }
        auxBlocks64[b] = AudioBlock<double>(auxRow, width, width ? frames : 0);
      }

      uint8_t roles64[kMaxAudioChannels];
      const uint32_t numRoles64 =
          inst->mainRoles(roles64, kMaxAudioChannels);
      AudioBlock<double> block64(chans64, nch64, frames);
      inst->midiOut.clear();
      inst->endedNotes.clear();
      ProcessContextT<double> ctx64{block64,   auxBlocks64, nAux64,
                                    scBlock64, inst->midi,  inst->midiOut,
                                    numRoles64 ? roles64 : nullptr, &inst->expression,
                                    &inst->outputPortActive.flags[1],
                                    inst->inputPortActive[1],
                                    inst->offline,
                                    &inst->noteIdsIn,
                                    &inst->endedNotes,
                                    &inst->paramMods};
      bypassCapture(inst->bypass, chans64, nch64, frames);
      trackNotes(inst->notes, ctx64.midi);
      runDspCtx64(inst->dsp, ctx64, blockParams);
      bypassApply(inst->bypass, chans64, nch64, frames);
      guardOutputs(inst, chans64, nch64, auxBlocks64, nAux64, frames);

      inst->meter.push(measureBlock(chans64[0], (size_t) frames));
      // The 64-bit path feeds the same scope. A plugin running in a
      // double-precision host would otherwise have a display that never moved,
      // which reads as a broken plugin rather than as an unsupported mode.
      inst->scope.push(chans64, 1, (uint32_t) frames);
      inst->load.record(std::chrono::duration<double>(
                            std::chrono::steady_clock::now() - blockStart).count(),
                        frames);
      out->constant_mask = 0;
      clearAuxConstantMasks(process, nAux64);
      finishProcess(inst, process, frames);
      return CLAP_PROCESS_CONTINUE;
    }
  }

  if (!out->data32 || out->channel_count < 1) {
    discardEvents(inst);
    requeueWideMods(inst);
    return CLAP_PROCESS_CONTINUE;
  }

  // The block is as wide as BOTH sides agree on: the negotiated width, capped
  // by what the host actually delivered this call.
  uint32_t nch = inst->mainChannels;
  if (out->channel_count < nch) nch = out->channel_count;
  if (nch > kMaxAudioChannels) nch = kMaxAudioChannels;
  float* chans[kMaxAudioChannels];
  for (uint32_t c = 0; c < nch; ++c) chans[c] = out->data32[c];
  // Channels past the ones the DSP writes are the host's and undefined on
  // entry. Silence, not whatever the buffer held: a stereo plugin on a
  // four-channel bus played the host's stale memory on the other two.
  for (uint32_t c = nch; c < out->channel_count; ++c)
    if (out->data32[c]) std::memset(out->data32[c], 0, sizeof(float) * frames);

  if (kDesc.isInstrument) {
    // A synth writes; it never reads an input that doesn't exist.
    for (uint32_t c = 0; c < nch; ++c) std::memset(chans[c], 0, sizeof(float) * frames);
  } else if (process->audio_inputs_count > 0 && process->audio_inputs[0].data32 &&
             process->audio_inputs[0].channel_count > 0) {
    // Copy input to output when the host didn't give us in-place buffers.
    const clap_audio_buffer_t* in = &process->audio_inputs[0];
    for (uint32_t c = 0; c < nch; ++c) {
      const float* src = in->data32[c < in->channel_count ? c : in->channel_count - 1];
      if (src && src != chans[c]) std::memcpy(chans[c], src, sizeof(float) * frames);
    }
  } else {
    // No input to copy -- none connected, or one with no channels, whose
    // count - 1 would have indexed channel 4294967295. Silence, not whatever
    // the output buffer held.
    for (uint32_t c = 0; c < nch; ++c) std::memset(chans[c], 0, sizeof(float) * frames);
  }

  sendTransport(inst->dsp, readTransport(process->transport));

  float* zero = inst->scSilence.empty() ? nullptr : inst->scSilence.data();
  float* scChans[2] = {zero, zero};
  if (process->audio_inputs_count > 1 && process->audio_inputs[1].data32 &&
      process->audio_inputs[1].channel_count > 0) {
    const clap_audio_buffer_t* sc = &process->audio_inputs[1];
    scChans[0] = sc->data32[0];
    scChans[1] = sc->data32[sc->channel_count > 1 ? 1 : 0];
  }
  AudioBlock<float> scBlock(scChans, 2, scChans[0] ? frames : 0);

  AudioBlock<float> block(chans, nch, frames);

  // Aux output buses. A host may leave any of them unconnected; that bus
  // arrives zero-width rather than null, so a DSP needs no host-specific
  // checks. Their contents are undefined on entry by contract, but a host
  // that hands us a bus expects SOMETHING defined in it, so an aux the DSP
  // does not write stays whatever the host had -- which is why the splitter
  // example clears what it does not fill.
  // Held in the instance (processdetail::AuxScratch).
  AudioBlock<float>* const auxBlocks = inst->aux32.blocks;
  const uint32_t nAux = numAuxOutputs();
  for (uint32_t b = 0; b < nAux; ++b) {
    float** const auxRow = inst->aux32.row(b);
    const uint32_t portIndex = 1 + b;
    uint32_t width = 0;
    if (portIndex < process->audio_outputs_count) {
      const clap_audio_buffer_t& ab = process->audio_outputs[portIndex];
      if (ab.data32) {
        width = ab.channel_count;
        const uint32_t want = auxBusChannels(b);
        if (width > want) width = want;
        if (width > kMaxAudioChannels) width = kMaxAudioChannels;
        for (uint32_t c = 0; c < width; ++c) auxRow[c] = ab.data32[c];
      }
    }
    auxBlocks[b] = AudioBlock<float>(auxRow, width, width ? frames : 0);
  }

  inst->midiOut.clear();
  inst->endedNotes.clear();
  uint8_t roles[kMaxAudioChannels];
  const uint32_t numRoles = inst->mainRoles(roles, kMaxAudioChannels);
  // Aux flags start at index 1 of the output array: index 0 is the MAIN output,
  // which a DSP cannot skip and which ctx.aux does not include.
  ProcessContext ctx{block,      auxBlocks,     nAux,
                     scBlock,    inst->midi,    inst->midiOut,
                     numRoles ? roles : nullptr, &inst->expression,
                     &inst->outputPortActive.flags[1],
                     inst->inputPortActive[1],
                     inst->offline,
                     &inst->noteIdsIn,
                     &inst->endedNotes,
                     &inst->paramMods};
  // Stash the dry input, run the DSP, then crossfade back to the dry if the
  // host has engaged the bypass.
  //
  // These two calls existed only in the 64-bit path, which is the one almost
  // no host takes. So in CLAP the bypass has never worked: the state blob has
  // carried the flag since version 2 and BypassState has crossfaded and
  // latency-aligned all along, and none of it was ever reached at 32 bits.
  // Adding the parameter that lets a host press it is what finally made that
  // visible -- the check that measures the contract failed on the first run.
  bypassCapture(inst->bypass, chans, nch, frames);
  trackNotes(inst->notes, ctx.midi);
  runDspCtx(inst->dsp, ctx, blockParams);
  bypassApply(inst->bypass, chans, nch, frames);
  guardOutputs(inst, chans, nch, auxBlocks, nAux, frames);

  // Publish what the meters should show. Two atomic stores: the interface can
  // be closed, opening, or absent and this costs the same either way.
  {
    // Vectorised, peak and energy in one pass (measureBlock), and the meters
    // run on every block whether or not an editor is open.
    inst->meter.push(measureBlock(chans[0], (size_t) frames));
    // Allocates nothing and takes no lock, which is not a claim: rt_safety_test
    // compiles this wrapper into itself with a counting operator new and arms
    // it around process(), so a scope that allocated would fail the audit.
    inst->scope.push(chans, 1, (uint32_t) frames);
    inst->load.record(std::chrono::duration<double>(
                          std::chrono::steady_clock::now() - blockStart).count(),
                      frames);
  }

  // Mono host port fed from a stereo DSP: nothing to fold, we wrote one channel.
  out->constant_mask = 0;
  clearAuxConstantMasks(process, nAux);
  finishProcess(inst, process, frames);
  return CLAP_PROCESS_CONTINUE;
}

// ── A block longer than activate() allowed ──────────────────────────────────
//
// max_frames_count is the host's promise, and the DSP was prepared to it: its
// scratch, the sidechain's silence, the bypass ring are all that long. A host
// that broke the promise -- one rendering offline, say -- had the wrapper and
// the DSP write past the end of every one of them (ASan found the sidechain's
// silence first). Such a block is run as several that keep it: each with the
// events that fall in it at their times within it, the host's buffers from
// where it starts, and the transport moved on to there.

/** The host's events for one piece of a block, at times within the piece.
 *  Each get() copies the event over the last one, which is all process()
 *  needs: it applies each event before asking for the next. */
struct SliceEventsIn {
  clap_input_events_t list{};
  const clap_input_events_t* src = nullptr;
  uint32_t first = 0, count = 0, offset = 0;
  uint64_t copy[16]; // 128 bytes, aligned for any core event
};

inline uint32_t sliceEventsSize(const clap_input_events_t* list) {
  return static_cast<const SliceEventsIn*>(list->ctx)->count;
}

inline const clap_event_header_t* sliceEventsGet(const clap_input_events_t* list, uint32_t i) {
  auto* s = static_cast<SliceEventsIn*>(list->ctx);
  if (i >= s->count) return nullptr;
  const clap_event_header_t* hdr = s->src->get(s->src, s->first + i);
  // Larger than any core event: passed as it is, and its time clamped into
  // the piece by process() like any other.
  if (!hdr || hdr->size < sizeof(clap_event_header_t) || hdr->size > sizeof(s->copy)) return hdr;
  std::memcpy(s->copy, hdr, hdr->size);
  auto* moved = reinterpret_cast<clap_event_header_t*>(s->copy);
  moved->time = hdr->time > s->offset ? hdr->time - s->offset : 0;
  return moved;
}

/** And what process() sends back, moved from the piece's time to the host's. */
struct SliceEventsOut {
  clap_output_events_t list{};
  const clap_output_events_t* dst = nullptr;
  uint32_t offset = 0;
  uint64_t copy[16]; // 128 bytes, aligned for any core event
};

inline bool sliceEventsPush(const clap_output_events_t* list, const clap_event_header_t* hdr) {
  auto* s = static_cast<SliceEventsOut*>(list->ctx);
  if (hdr->size < sizeof(clap_event_header_t) || hdr->size > sizeof(s->copy))
    return s->dst->try_push(s->dst, hdr);
  std::memcpy(s->copy, hdr, hdr->size);
  auto* moved = reinterpret_cast<clap_event_header_t*>(s->copy);
  moved->time = hdr->time + s->offset;
  return s->dst->try_push(s->dst, moved);
}

/** One of the host's buses, from frame `offset` on. The channel pointers live
 *  in `p32` / `p64`, which hold kMaxAudioChannels: no more is ever read. */
inline void sliceBuffer(const clap_audio_buffer_t& from, uint32_t offset, clap_audio_buffer_t& to,
                        float** p32, double** p64) {
  to = from;
  to.channel_count = from.channel_count < kMaxAudioChannels ? from.channel_count : kMaxAudioChannels;
  to.data32 = nullptr;
  to.data64 = nullptr;
  if (from.data32) {
    for (uint32_t c = 0; c < to.channel_count; ++c)
      p32[c] = from.data32[c] ? from.data32[c] + offset : nullptr;
    to.data32 = p32;
  }
  if (from.data64) {
    for (uint32_t c = 0; c < to.channel_count; ++c)
      p64[c] = from.data64[c] ? from.data64[c] + offset : nullptr;
    to.data64 = p64;
  }
}

/** The transport `frames` later, while it plays: the song position in seconds
 *  and in beats, and the bar it is in. Not a loop's wrap -- a host that loops
 *  ends the block there itself. */
inline void advanceTransport(clap_event_transport_t& t, uint32_t frames, double sampleRate) {
  if (!(t.flags & CLAP_TRANSPORT_IS_PLAYING) || sampleRate <= 0.0) return;
  const double seconds = (double) frames / sampleRate;
  if (t.flags & CLAP_TRANSPORT_HAS_SECONDS_TIMELINE)
    t.song_pos_seconds += (clap_sectime) (seconds * (double) CLAP_SECTIME_FACTOR + 0.5);
  if ((t.flags & CLAP_TRANSPORT_HAS_BEATS_TIMELINE) && (t.flags & CLAP_TRANSPORT_HAS_TEMPO)) {
    t.song_pos_beats +=
        (clap_beattime) (seconds * t.tempo / 60.0 * (double) CLAP_BEATTIME_FACTOR + 0.5);
    if ((t.flags & CLAP_TRANSPORT_HAS_TIME_SIGNATURE) && t.tsig_num > 0 && t.tsig_denom > 0) {
      const clap_beattime bar =
          (clap_beattime) ((double) t.tsig_num * 4.0 / (double) t.tsig_denom *
                           (double) CLAP_BEATTIME_FACTOR);
      while (bar > 0 && t.song_pos_beats >= t.bar_start + bar) {
        t.bar_start += bar;
        ++t.bar_number;
      }
    }
  }
}

/** Whether a CLAP event moves a parameter for the whole plugin: a value, or a
 *  modulation that names no note of a parameter not modulated per note.
 *  What sample-accurate automation splits a block at (PluginDescriptor::
 *  automationGranularity); the rest land at their frames already
 *  (ParamModBuffer). */
inline bool movesParameter(const clap_event_header_t* hdr) {
  if (!hdr || hdr->space_id != CLAP_CORE_EVENT_SPACE_ID) return false;
  if (hdr->type == CLAP_EVENT_PARAM_VALUE) return true;
  if (hdr->type != CLAP_EVENT_PARAM_MOD) return false;
  const auto* e = eventAs<clap_event_param_mod_t>(hdr);
  // A parameter modulated per note takes every modulation through the voices,
  // at its frame already: only the others move what process() is handed.
  return e && e->note_id < 0 && e->key < 0 && e->channel < 0 && e->param_id < (uint32_t) SONORE_NUM_PARAMS &&
         !kDesc.params[e->param_id].modulatablePerNote;
}

/** Where the piece of a block that starts at `start` ends: at `end` (the
 *  block's end, or the most frames a piece may have), or at the first
 *  parameter change at least `granularity` frames in, so each piece runs
 *  with the values set in its first frames. A change in the last
 *  `granularity` frames before `end` splits there instead, a little early,
 *  rather than leave a sliver of a piece. `from` is the first event not yet
 *  given to a piece; a host's list is in time order, so the search stops at
 *  `end`. */
inline uint32_t pieceEnd(const clap_input_events_t* events, uint32_t from, uint32_t total,
                         uint32_t start, uint32_t end, uint32_t granularity) {
  if (!events || granularity == 0) return end;
  for (uint32_t i = from; i < total; ++i) {
    const clap_event_header_t* hdr = events->get(events, i);
    if (!hdr) continue;
    if (hdr->time >= end) break;
    if (hdr->time < start + granularity || !movesParameter(hdr)) continue;
    uint32_t cut = hdr->time;
    if (cut + granularity > end) cut = end - granularity;
    return cut >= start + granularity ? cut : end;
  }
  return end;
}

inline clap_process_status pluginProcess(const clap_plugin_t* plugin,
                                         const clap_process_t* process) {
  Instance* inst = self(plugin);
  const uint32_t frames = process->frames_count;
  const uint32_t most = inst->maxFrames;
  // Sample-accurate automation, where the descriptor asks for it: the block
  // is cut where a parameter moves, by the same slicing that keeps a piece
  // within the frames the DSP was prepared for.
  const uint32_t granularity =
      kDesc.automationGranularity > 0 ? (uint32_t) kDesc.automationGranularity : 0u;
  const uint32_t total = process->in_events ? process->in_events->size(process->in_events) : 0;
  if ((most == 0 || frames <= most) &&
      pieceEnd(process->in_events, 0, total, 0, frames, granularity) >= frames)
    return processBlock(inst, process);

  SliceEventsIn in;
  in.list.ctx = &in;
  in.list.size = sliceEventsSize;
  in.list.get = sliceEventsGet;
  in.src = process->in_events;
  SliceEventsOut outEvents;
  outEvents.list.ctx = &outEvents;
  outEvents.list.try_push = sliceEventsPush;
  outEvents.dst = process->out_events;

  constexpr uint32_t kOuts = 1 + kMaxAuxOutputs;
  const uint32_t nIns = process->audio_inputs_count < 2 ? process->audio_inputs_count : 2;
  const uint32_t nOuts = process->audio_outputs_count < kOuts ? process->audio_outputs_count : kOuts;
  clap_audio_buffer_t ins[2];
  clap_audio_buffer_t outs[kOuts];
  float* in32[2][kMaxAudioChannels];
  double* in64[2][kMaxAudioChannels];
  float* out32[kOuts][kMaxAudioChannels];
  double* out64[kOuts][kMaxAudioChannels];

  clap_process_t piece = *process;
  piece.in_events = process->in_events ? &in.list : nullptr;
  piece.out_events = process->out_events ? &outEvents.list : nullptr;
  piece.audio_inputs = nIns ? ins : nullptr;
  piece.audio_inputs_count = nIns;
  piece.audio_outputs = nOuts ? outs : nullptr;
  piece.audio_outputs_count = nOuts;
  clap_event_transport_t transport{};

  uint32_t next = 0;
  for (uint32_t start = 0; start < frames;) {
    uint32_t end = most > 0 && frames - start > most ? start + most : frames;
    end = pieceEnd(process->in_events, next, total, start, end, granularity);
    const uint32_t len = end - start;
    const bool last = end >= frames;
    // The events before this piece's end -- in a host's time-ordered list,
    // the next run of them. The last piece takes whatever is left.
    in.first = next;
    in.offset = start;
    while (next < total) {
      const clap_event_header_t* hdr = process->in_events->get(process->in_events, next);
      if (!last && hdr && hdr->time >= start + len) break;
      ++next;
    }
    in.count = next - in.first;
    outEvents.offset = start;
    for (uint32_t b = 0; b < nIns; ++b)
      sliceBuffer(process->audio_inputs[b], start, ins[b], in32[b], in64[b]);
    for (uint32_t b = 0; b < nOuts; ++b)
      sliceBuffer(process->audio_outputs[b], start, outs[b], out32[b], out64[b]);
    piece.frames_count = len;
    piece.steady_time = process->steady_time >= 0 ? process->steady_time + (int64_t) start : -1;
    if (process->transport) {
      transport = *process->transport;
      advanceTransport(transport, start, inst->sampleRate);
      piece.transport = &transport;
    }
    processBlock(inst, &piece);
    start = end;
  }
  for (uint32_t b = 0; b < nOuts; ++b) process->audio_outputs[b].constant_mask = 0;
  return CLAP_PROCESS_CONTINUE;
}

/** Ask the host to drain the UI queue now. [main-thread]
 *
 *  Everything an editor does goes down uiEvents, and process() drains it -- so
 *  while the transport is stopped, the plugin deactivated or the track asleep,
 *  an edit sits in the queue: the host records nothing, shows the old value in
 *  its own controls, and saves a project that does not have it. request_flush
 *  is how CLAP says "something is waiting although I am idle"; the host answers
 *  with flush() or process(), both of which drain the queue. Harmless while
 *  processing, and never from the audio thread, which is already inside one. */
inline void requestParamFlush(Instance* inst) {
  if (!inst->host) return;
  if (const auto* hostParams = static_cast<const clap_host_params_t*>(
          inst->host->get_extension(inst->host, CLAP_EXT_PARAMS)))
    hostParams->request_flush(inst->host);
}

// ── Presets ──────────────────────────────────────────────────────────────────
// Factory presets are compiled in (see presets.h), so the PLUGIN-location load
// key is simply the preset's index; user presets, and the factory presets
// written out as files, are FILE locations (see the provider below). Every
// load goes through the preset manager and so through the state path: the
// whole set reaches the audio thread in one step, and the host is asked to
// rescan the values, as clap_plugin_params' "loading a preset" scenario has
// it. A rescan is also what tells the host NOT to record the jump as
// automation.
//
// Factory loads used to write the shared parameters here directly. Going
// through the state path instead is what makes a factory preset, a user
// preset and a session restore one mechanism: the same atomic hand-over, the
// same notification, the same selection kept for the session.

/** `announce` is false when the HOST asked (preset-load): it already knows. */
inline bool loadPresetByIndex(Instance* inst, int index, bool announce = true) {
  if (index < 0 || index >= kDesc.numPresets || !kDesc.presets) return false;
  return presetsOf(inst).load(index, nullptr, announce);
}

/** [main-thread] clap_plugin_preset_load: a factory preset by index, or a
 *  preset file by path -- the user's, a factory one written out, any file
 *  the host's browser found. A refusal is told to the host with its reason
 *  (clap_host_preset_load's on_error), not only returned as false. */
inline bool presetFromLocation(const clap_plugin_t* plugin, uint32_t locationKind, const char* location,
                               const char* loadKey) {
  Instance* inst = self(plugin);
  PresetManager& presets = presetsOf(inst);
  std::string why;
  bool ok = false;
  if (locationKind == CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN) {
    char* end = nullptr;
    const long index = loadKey ? std::strtol(loadKey, &end, 10) : -1;
    if (!loadKey || end == loadKey || *end != '\0' || index < 0 || index >= kDesc.numPresets)
      why = "there is no such factory preset";
    else
      ok = loadPresetByIndex(inst, (int) index, false);
    if (!ok && why.empty()) why = "the factory preset could not be loaded";
  } else if (locationKind == CLAP_PRESET_DISCOVERY_LOCATION_FILE && location) {
    ok = presets.loadFile(location, false, &why);
  } else {
    why = "not a location this plugin has presets in";
  }
  if (!ok && inst->host) {
    const auto* ext = static_cast<const clap_host_preset_load_t*>(
        inst->host->get_extension(inst->host, CLAP_EXT_PRESET_LOAD));
    if (!ext) ext = static_cast<const clap_host_preset_load_t*>(
        inst->host->get_extension(inst->host, CLAP_EXT_PRESET_LOAD_COMPAT));
    if (ext && ext->on_error) ext->on_error(inst->host, locationKind, location, loadKey, 0, why.c_str());
  }
  return ok;
}

// ── GUI ──────────────────────────────────────────────────────────────────────
// The plugin's face is the SDK-drawn native editor, or -- when the project
// supplies a page or asks for EditorKind::Web -- the same HTML the studio
// preview renders, in an OS webview, wired through the `window.sonore`
// bridge. Only the EMBEDDED case is
// supported: it is what every host implements, and a floating window would give
// us a second lifetime to get wrong for no user-visible gain.

/** The page source, for a webview editor: the project's `uihtml` when it
 *  defines one, else a plain panel generated from the parameter table. */
inline const std::string& uiHtml() {
  static const std::string html =
#if defined(SONORE_UI_HTML)
      std::string(SONORE_UI_HTML);
#else
      fallbackHtml(kDesc);
#endif
  return html;
}

/** The bridge for this instance's page, with the values it has NOW.
 *
 *  Built at each open: a page draws its first frame from the parameter
 *  table the bridge hands it, before the editor's tick has pushed anything,
 *  and a table holding the defaults drew every control at its default for
 *  that frame and up to 33 ms after it. A parameter past SONORE_NUM_PARAMS
 *  (a release build loading a descriptor the two disagree on) keeps its
 *  default, as it has no slot to read. */
inline std::string bridgeScriptFor(const Instance& inst) {
  std::vector<float> now(kDesc.numParams > 0 ? (size_t) kDesc.numParams : 0u);
  for (int i = 0; i < kDesc.numParams; ++i)
    now[(size_t) i] = i < SONORE_NUM_PARAMS ? (float) inst.params[i] : kDesc.params[i].defaultValue;
  return bridgeScript(kDesc, now.empty() ? nullptr : now.data());
}

/** Whether this plugin supplied a face of its own. */
inline bool hasWebPage() {
#if defined(SONORE_UI_HTML)
  return true;
#else
  return false;
#endif
}

/** Which editor this build, this platform and this descriptor add up to. */
inline EditorChoice editorChoice() {
#if defined(SONORE_HAS_WEBVIEW_BACKEND)
  const bool webAvailable = true;
#else
  const bool webAvailable = false;
#endif
  return chooseEditorBackend(kDesc.editor, hasWebPage(), gfx::NativeEditor::isAvailable(),
                             webAvailable, gfx::NativeEditor::unavailableReason());
}

// Defined below, next to the rest of the context-menu plumbing. Declared here
// because makeEditorHost binds it into the editor and comes first.
inline bool showHostContextMenu(Instance* inst, int paramIndex, int x, int y);

/** [main-thread] The four things the native editor needs from the plugin.
 *
 *  Every edit goes down the SAME queue the page uses, so a knob turned here
 *  is automation the host records, an undo step the host can collapse, and a
 *  value the DSP receives on the audio thread -- not a private number the
 *  editor keeps to itself. */
inline gfx::EditorHost makeEditorHost(Instance* inst) {
  gfx::EditorHost host;
  host.getParameter = [inst](int index) -> float {
    if (index < 0 || index >= SONORE_NUM_PARAMS) return 0.0f;
    return inst->params[index];
  };
  // uiMayWrite bounds by kDesc.numParams; params[] and uiEcho[] are sized
  // SONORE_NUM_PARAMS, and a release build loads a descriptor where the two
  // disagree (entryInit refuses it only in debug). Both bounds, then.
  host.setParameter = [inst](int index, float value) {
    if (index >= SONORE_NUM_PARAMS || !uiMayWrite(kDesc, index)) return;
    const float clamped = clampToRange(kDesc.params[index], value);
    // Written here on the main thread as well as queued, so the editor reads
    // back what the user just did rather than what the audio thread has not
    // drained yet. Without it a knob snaps backwards for one frame on every
    // move, and snaps back permanently while the plugin is deactivated.
    inst->params[index] = clamped;
    inst->uiEcho[index] = clamped;
    UiEventQueue::Event e;
    e.kind = UiEventQueue::Event::Kind::ParamSet;
    e.index = index;
    e.value = clamped;
    // Deliberately NOT marked dirty here. drainUiEvents reports the change to
    // the host as a param_value event, which is how a host learns any control
    // moved; the page path does the same, and an editor that marked the
    // project dirty where the other one did not would be two behaviours for
    // one user action.
    inst->uiEvents.push(e);
    requestParamFlush(inst);
  };
  host.beginGesture = [inst](int index) {
    if (index >= SONORE_NUM_PARAMS || !uiMayWrite(kDesc, index)) return;
    UiEventQueue::Event e;
    e.kind = UiEventQueue::Event::Kind::GestureBegin;
    e.index = index;
    inst->uiEvents.push(e);
    requestParamFlush(inst);
  };
  host.endGesture = [inst](int index) {
    if (index >= SONORE_NUM_PARAMS || !uiMayWrite(kDesc, index)) return;
    UiEventQueue::Event e;
    e.kind = UiEventQueue::Event::Kind::GestureEnd;
    e.index = index;
    inst->uiEvents.push(e);
    requestParamFlush(inst);
  };

  // Only for an instrument, and that is how the editor decides whether to show
  // a keyboard: an effect leaves these empty and gets no keys. Nothing has to
  // pass an "is this a synth" flag down three layers to arrive at the answer
  // the descriptor already had.
  //
  // Down the SAME queue the page's keyboard uses, so a note played on the
  // native editor reaches the DSP on the audio thread and is reported to the
  // host exactly as one from the page would be.
  if (kDesc.isInstrument) {
    host.noteOn = [inst](int note, float velocity) {
      if (note < 0 || note > 127) return;
      UiEventQueue::Event e;
      e.kind = UiEventQueue::Event::Kind::NoteOn;
      e.index = note;
      // 0..1 to 1..127. Never zero: a note-on with velocity zero is a note-OFF
      // in MIDI, and sending one here would start a note that never sounds.
      const float scaled = velocity * 127.0f;
      e.value = scaled < 1.0f ? 1.0f : (scaled > 127.0f ? 127.0f : scaled);
      inst->uiEvents.push(e);
    };
    host.noteOff = [inst](int note) {
      if (note < 0 || note > 127) return;
      UiEventQueue::Event e;
      e.kind = UiEventQueue::Event::Kind::NoteOff;
      e.index = note;
      inst->uiEvents.push(e);
    };
  }

  // ── Right-click reaches the host's own parameter menu ────────────────────
  //
  // showHostContextMenu has existed since the context-menu extension was
  // wired, and the ONLY thing that ever called it was the bridge message from
  // the web editor's sonore.contextMenu. So the moment the native editor
  // became the default, right-clicking a knob stopped doing anything -- and a
  // right-click on a knob is how a user reaches MIDI learn and automation in
  // every DAW there is.
  //
  // Declared here rather than at the two open sites so it cannot be wired into
  // one and forgotten in the other.
  host.showContextMenu = [inst](int index, int x, int y) {
    // The editor works in LOGICAL pixels; the extension wants coordinates in
    // the window, which is device pixels. On a 1x display they are the same
    // number, which is exactly why this is easy to leave out and only wrong on
    // the machines that are harder to test on.
    const double s = inst->guiScale > 0.0 ? inst->guiScale : 1.0;
    showHostContextMenu(inst, index, (int) (x * s), (int) (y * s));
  };

  // The preset bar, and anything else in the editor that saves or loads a
  // whole state: through this instance's manager, so a preset lands by the
  // state path whichever format is hosting.
  host.presets = &presetsOf(inst);
  return host;
}

/** Which windowing API this OS speaks. Keyed on the PLATFORM, never on whether
 *  a webview backend exists: a headless build still has to tell the host the
 *  truth about what kind of window it would embed into. */
inline const char* nativeWindowApi() {
#if defined(_WIN32)
  return CLAP_WINDOW_API_WIN32;
#elif defined(__APPLE__)
  return CLAP_WINDOW_API_COCOA;
#else
  return CLAP_WINDOW_API_X11;
#endif
}

inline bool guiIsApiSupported(const clap_plugin_t*, const char* api, bool isFloating) {
  if (isFloating) return false;
  return api && std::strcmp(api, nativeWindowApi()) == 0;
}

inline bool guiGetPreferredApi(const clap_plugin_t*, const char** api, bool* isFloating) {
  *api = nativeWindowApi();
  *isFloating = false;
  return true;
}

/**
 * Everything the page does not know yet, as one script.
 *
 * This body existed three times -- once per format with an editor -- and was
 * identical in all three down to the comment about setlocale(). That is the
 * shape a bug takes when it gets fixed in one copy: the AU editor would keep
 * a defect the CLAP one had lost, and nothing would fail until somebody
 * opened the plugin in Logic. So the formats now differ only in how they
 * reach their Instance, which is the only thing about them that is actually
 * different.
 *
 * [main-thread] at the editor's ~30 Hz clock.
 */
inline std::string uiTickScript(Instance& inst) {
  std::string js;
  for (int i = 0; i < SONORE_NUM_PARAMS; ++i) {
    if (inst.uiEchoValid && inst.uiEcho[i] == inst.params[i]) continue;
    inst.uiEcho[i] = inst.params[i];
    // jsNumber, not %g: hosts call setlocale(), and a locale that prints
    // "0,5" turns this call into a JS syntax error (see gui.h).
    char value[40], call[96];
    jsNumber(value, sizeof(value), (double) inst.params[i]);
    std::snprintf(call, sizeof(call), "window.sonore.__update(%d,%s);", i, value);
    js += call;
  }
  inst.uiEchoValid = true;

  // Which keys are sounding, only when that CHANGED. A keyboard moves a few
  // times a second where the meters move thirty, and a page redrawing 128
  // keys on every tick is a page that makes a DAW feel slow.
  const uint64_t low = inst.notes.low(), high = inst.notes.high();
  if (!inst.notesEchoValid || low != inst.notesEchoLow || high != inst.notesEchoHigh) {
    inst.notesEchoLow = low;
    inst.notesEchoHigh = high;
    inst.notesEchoValid = true;
    // Printed as unsigned decimal: a 64-bit mask does not survive a double,
    // but JavaScript parses the digits itself and only loses precision above
    // 2^53 -- which is why the page is handed two words rather than one.
    char noteCall[96];
    std::snprintf(noteCall, sizeof(noteCall), "window.sonore.__notes(%u,%u,%u,%u);",
                  inst.notes.word(0), inst.notes.word(1), inst.notes.word(2),
                  inst.notes.word(3));
    js += noteCall;
  }

  // The DSP's own state, when something could have moved it. Parameters have
  // their own channel; this is everything else -- the sample it loaded, the
  // preset it is on. Without it an editor lies on reopen: the sampler still
  // has the file and the UI draws an empty slot beside a plugin that is
  // playing a kit.
  if (inst.uiStateDirty) {
    inst.uiStateDirty = false;
    // Through saveDspState, not straight at the DSP: `if constexpr` only
    // discards inside a TEMPLATE, and this function is not one -- so the call
    // would be compiled for every plugin, including the ones with no state
    // bag at all.
    if (HasStateBag<SonoreDsp>::value) {
      StateBag bag;
      saveDspState(inst.dsp, bag);
      js += "window.sonore.__state(";
      js += bagToJson(bag);
      js += ");";
    }
  }

  inst.meter.pushGr(dspGainReduction(inst.dsp));
  inst.meter.tickNow(); // the time that passed: a 33 ms timer does not tick every 33 ms
  char lv[40], db[40], vu[40], gr[40], meterCall[200];
  jsNumber(lv, sizeof(lv), (double) inst.meter.level());
  jsNumber(db, sizeof(db), (double) inst.meter.db());
  jsNumber(vu, sizeof(vu), (double) inst.meter.vu());
  jsNumber(gr, sizeof(gr), (double) inst.meter.gr());
  std::snprintf(meterCall, sizeof(meterCall), "window.sonore.__meter(%s,%s,%s,%s);", lv, db, vu, gr);
  if (inst.uiLastMeter != meterCall) {
    inst.uiLastMeter = meterCall;
    js += meterCall;
  }
  // The load to a tenth of a percent: finer than any readout shows, and
  // coarse enough that the jitter of a measured idle load does not count as
  // a change worth waking the page for.
  const auto tenth = [](double v) { return std::round(v * 1000.0) / 1000.0; };
  char cpu[40], peak[40], cpuCall[200];
  jsNumber(cpu, sizeof(cpu), tenth((double) inst.load.load()));
  jsNumber(peak, sizeof(peak), tenth((double) inst.load.peakLoad()));
  std::snprintf(cpuCall, sizeof(cpuCall),
                "if(window.sonore.__cpu)window.sonore.__cpu(%s,%s,%llu);", cpu, peak,
                (unsigned long long) inst.load.xruns());
  if (inst.uiLastLoad != cpuCall) {
    inst.uiLastLoad = cpuCall;
    js += cpuCall;
  }
  return js;
}

inline int64_t steadyNowMs() {
  return (int64_t) std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

/**
 * How long a hidden web editor keeps its renderer, in milliseconds.
 *
 * webview_bench measured what the alternatives buy. Suspending a hidden
 * renderer (TrySuspend + the LOW memory target, both confirmed succeeding)
 * returned -2 MB across six editors: freezing stops CPU, it does not return
 * commit, and ~30 MB per editor stays resident for as long as the host keeps
 * the editor object alive -- which for a hidden editor in a session is
 * forever. Destroying the webview returns all of it, measured, and our pages
 * rebuild from C++ state by design (parameters, meters and MIDI state all
 * live on this side of the bridge), so the cost is ~half a second of blank
 * on the next open of an editor nobody had looked at for half a minute.
 *
 * 0 disables parking. The env override exists for tuning and for the bench,
 * which cannot wait thirty real seconds per measurement.
 */
inline int64_t webviewParkAfterMs() {
  static const int64_t value = [] {
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996) // read once, copied at once; nothing else writes it
#endif
    const char* env = std::getenv("SONORE_WEBVIEW_PARK_MS");
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
    if (!env || !env[0]) return (int64_t) 30000;
    return (int64_t) std::atoll(env);
  }();
  return value;
}

#if defined(SONORE_HAS_WEBVIEW_BACKEND)
/** Attach the webview to a host-provided parent. The one place the void* the
 *  shared code carries is cast back to the handle type this platform's
 *  backend declares -- used from set_parent, and again when a parked editor
 *  is shown. */
inline bool attachWebviewTo(Instance* inst, void* parent, uint32_t w, uint32_t h) {
#if defined(_WIN32)
  return inst->webview.create((HWND) parent, w, h, uiHtml(), bridgeScriptFor(*inst), kDesc.id);
#elif defined(__APPLE__)
  return inst->webview.create(parent, w, h, uiHtml(), bridgeScriptFor(*inst), kDesc.id);
#else
  return inst->webview.create((unsigned long) (uintptr_t) parent, w, h, uiHtml(),
                              bridgeScriptFor(*inst), kDesc.id);
#endif
}
#endif

/** Push whatever the page doesn't know yet. Runs on the main thread from the
 *  backend's ~30 Hz timer. */
inline void guiTick(Instance* inst) {
#if defined(SONORE_LICENSED_BUILD)
  // Asserted from the TICK, not once at create: eval() is dropped while the
  // webview is still starting or hidden, so a one-shot call at creation would
  // be lost exactly on the slow machines where it matters. Re-evaluated only
  // when the answer CHANGES, so the steady state costs one comparison.
  if (!inst->guiIsNative) {
    const int want = licenseGate().licensed() ? 1 : 0;
    if (inst->licenseUi != want) {
      inst->webview.eval(want ? license::overlayResultScript(true, "")
                              : license::overlayScript(licenseGate().machine()));
      inst->licenseUi = want;
    }
  }
#endif
  // The native editor pulls its own values and repaints itself; there is no
  // page to push a script into.
  if (inst->guiIsNative) {
    inst->nativeEditor.tick();
    return;
  }
#if defined(SONORE_HAS_WEBVIEW_BACKEND)
  // Hidden past the grace period: give the renderer back. This runs on the
  // webview's OWN timer, which dies with the window it ticks -- so parking is
  // also the last thing this tick path ever does, which is exactly right:
  // there is nothing left to tick.
  if (inst->guiHiddenAtMs != 0 && !inst->webviewParked) {
    const int64_t grace = webviewParkAfterMs();
    if (grace > 0 && steadyNowMs() - inst->guiHiddenAtMs >= grace) {
      inst->webview.destroy();
      inst->webviewParked = true;
      return;
    }
  }
  if (!inst->webview.ready()) return;
  // Nothing while hidden -- not even the script. eval() drops it then (it
  // must: ExecuteScript wakes a suspended renderer), but uiTickScript marks
  // every value it writes as delivered, so a host change made behind a
  // hidden editor was never sent when it came back: automation, a preset,
  // a file loaded meanwhile, all stale on screen. Held back here, the first
  // tick after show() sends everything that moved.
  if (inst->guiHiddenAtMs != 0) return;
  inst->webview.eval(uiTickScript(*inst));
#else
  (void) inst;
#endif
}

/**
 * [main-thread] Ask the host to show ITS menu for one parameter.
 *
 * MIDI learn, "remove automation", "show automation lane" -- a plugin cannot
 * offer any of them and the host cannot offer them if the plugin swallows the
 * right-click. So the click is handed straight back.
 *
 * Two things can honestly go wrong and neither is an error: a host may not
 * implement the extension at all, and one that does may still say it cannot
 * pop up a menu right now -- can_popup depends on the windowing system and is
 * explicitly invalidated when the plugin window is created. Both end in the
 * page's right-click doing nothing, which is what it did before.
 */
inline bool showHostContextMenu(Instance* inst, int paramIndex, int x, int y) {
  if (!inst->host || paramIndex < 0 || paramIndex >= SONORE_NUM_PARAMS) return false;
  const auto* ext = static_cast<const clap_host_context_menu_t*>(
      inst->host->get_extension(inst->host, CLAP_EXT_CONTEXT_MENU));
  if (!ext)
    ext = static_cast<const clap_host_context_menu_t*>(
        inst->host->get_extension(inst->host, CLAP_EXT_CONTEXT_MENU_COMPAT));
  if (!ext || !ext->popup) return false;
  if (ext->can_popup && !ext->can_popup(inst->host)) return false;

  clap_context_menu_target_t target{};
  target.kind = CLAP_CONTEXT_MENU_TARGET_KIND_PARAM;
  // The index IS the id here: the generated contract never reorders
  // parameters, which is what paramsGetInfo relies on too.
  target.id = (clap_id) paramIndex;
  // Screen index -1: the coordinates are relative to the plugin's own window,
  // which is what an embedded GUI reports and what the extension asks for.
  return ext->popup(inst->host, &target, -1, x, y);
}

/**
 * [main-thread] Show a native file browser and tell everyone what came back.
 *
 * The DSP first, then the page. That order matters on a reload: the page
 * usually reacts by showing the filename, and showing it before the DSP has
 * accepted the file would put a name on screen for a sample that is not
 * loaded yet.
 */
inline void chooseFileForPage(Instance* inst, const BridgeMessage& msg) {
#if defined(SONORE_HAS_WEBVIEW_BACKEND)
  void* parent = inst->webview.nativeWindow();
  const std::string path = FileDialog::byMode(msg.mode, parent);

  sendFile(inst->dsp, msg.purpose.c_str(), path.c_str());
  inst->uiStateDirty = true; // the DSP may have taken a file

  // A file the plugin loaded is exactly the change no host can see: it moved
  // no parameter and called nothing. Without this the DAW closes without
  // offering to save, and the user loses the sample they just picked.
  if (!path.empty()) {
    inst->stateDirty = true;
    markDirtyIfNeeded(inst);
  }

  inst->webview.eval(fileAnswerScript(msg.purpose, path));
#else
  (void) inst;
  (void) msg;
#endif
}

/** [main-thread] The page's bridge came up in a real document.
 *
 *  The tick pushes the state bag only when it is dirty, and parameters only
 *  when they differ from what it last pushed. The webview accepts scripts from
 *  the moment it exists, but the page's document is created later, by the
 *  navigation -- and a push made in between ran in the blank document that
 *  navigation replaced. The flag was cleared all the same, so the page never
 *  received the state at all: a sampler's slot drawn empty beside the file it
 *  holds, and an intermittent GUI-bridge test failure, worse under load. Every
 *  format answers this the same way: push it all again.
 *
 *  All of it, the held keys too. They are sent only when they change, and
 *  this list once left them out: an editor reopened over a held chord, or
 *  rebuilt after a park, drew its keyboard with every key up -- the new page
 *  had never been told, and nothing changed to tell it. */
inline void pageReady(Instance* inst) {
  inst->uiStateDirty = true;
  inst->uiEchoValid = false;
  inst->notesEchoValid = false;
  inst->uiLastMeter.clear();
  inst->uiLastLoad.clear();
#if defined(SONORE_LICENSED_BUILD)
  // And the licence overlay, for the same reason. The tick evaluated it into
  // the blank document before the navigation replaced it, then marked it
  // shown: an unlicensed build never showed its activation overlay -- the
  // only place to enter a code -- on any open. Forgotten here, the next tick
  // puts it into the page that now exists.
  inst->licenseUi = -1;
#endif
}

/** A page message arriving on the main thread. It never touches audio state
 *  directly: everything crosses to the audio thread through the lock-free
 *  queue, which is what keeps a UI event out of the audio callback's way. */
inline void guiOnMessage(Instance* inst, const BridgeMessage& msg) {
  // Nothing an interface may not write: out of range, or a parameter the
  // plugin REPORTS (readOnly). See uiMayWrite.
  if ((msg.kind == BridgeMessage::Kind::Set || msg.kind == BridgeMessage::Kind::GestureBegin ||
       msg.kind == BridgeMessage::Kind::GestureEnd) &&
      !uiMayWrite(kDesc, msg.index))
    return;
  UiEventQueue::Event e;
  switch (msg.kind) {
    case BridgeMessage::Kind::Ready:
      pageReady(inst);
      return;
    case BridgeMessage::Kind::Activate: {
#if defined(SONORE_LICENSED_BUILD)
      // On this thread: verifying a signature and writing one small file is
      // main-thread work, and the audio thread only ever reads the resulting
      // atomic flag.
      inst->webview.eval(license::handleActivate(licenseGate(), msg.text));
      inst->licenseUi = licenseGate().licensed() ? 1 : 0;
#endif
      return;
    }
    case BridgeMessage::Kind::CaptureKeys:
      // Straight onto the webview. Nothing crosses to audio: this decides who
      // receives a keystroke, which is a question only the UI thread has.
      inst->webview.captureKeys = msg.value >= 0.5;
      return;
    case BridgeMessage::Kind::ChooseFile:
      // On this thread and blocking, which is what a modal dialog is. The
      // audio thread keeps running throughout; nothing here touches it.
      chooseFileForPage(inst, msg);
      return;
    case BridgeMessage::Kind::ContextMenu:
      // Straight through, on this thread: the webview callback IS the main
      // thread, and the host's menu is modal -- queueing it for the audio
      // thread would be putting a popup menu in the audio callback.
      showHostContextMenu(inst, msg.index, msg.x, msg.y);
      return;
    case BridgeMessage::Kind::LoadPreset:
      // Every value goes through the same queue a knob uses, so a preset is
      // applied on the audio thread and recorded by the host like any edit.
      loadPresetByIndex(inst, msg.index);
      return;
    case BridgeMessage::Kind::Set: {
      if (msg.index < 0 || msg.index >= SONORE_NUM_PARAMS) return;
      const float clamped = clampToRange(kDesc.params[msg.index], (float) msg.value);
      e.kind = UiEventQueue::Event::Kind::ParamSet;
      e.index = msg.index;
      e.value = clamped;
      // Written here as well as queued, as the native editor's host does: the
      // host's get_value and the next state save read the parameter, not the
      // queue, and the timer pushes the parameter to the page whenever it
      // differs from the echo -- so with only the echo written the knob
      // flicked back on the next tick, and stayed back while nothing drained.
      inst->params[msg.index] = clamped;
      inst->uiEcho[msg.index] = clamped;
      break;
    }
    case BridgeMessage::Kind::GestureBegin:
    case BridgeMessage::Kind::GestureEnd:
      if (msg.index < 0 || msg.index >= SONORE_NUM_PARAMS) return;
      e.kind = msg.kind == BridgeMessage::Kind::GestureBegin
                   ? UiEventQueue::Event::Kind::GestureBegin
                   : UiEventQueue::Event::Kind::GestureEnd;
      e.index = msg.index;
      break;
    case BridgeMessage::Kind::NoteOn:
      if (msg.note < 0 || msg.note > 127) return;
      e.kind = UiEventQueue::Event::Kind::NoteOn;
      e.index = msg.note;
      e.value = (float) (msg.velocity < 1 ? 1 : (msg.velocity > 127 ? 127 : msg.velocity));
      break;
    case BridgeMessage::Kind::NoteOff:
      if (msg.note < 0 || msg.note > 127) return;
      e.kind = UiEventQueue::Event::Kind::NoteOff;
      e.index = msg.note;
      break;
    default:
      return;
  }
  inst->uiEvents.push(e);
  // A note needs process() to sound and gains nothing from a flush; an edit
  // has to reach the host even with nothing processing.
  if (e.kind != UiEventQueue::Event::Kind::NoteOn && e.kind != UiEventQueue::Event::Kind::NoteOff)
    requestParamFlush(inst);
}

// -- Driving the editor where the SDK cannot ------------------------------
//
// Windows delivers messages to a window procedure and generates WM_TIMER
// without being asked, so a Win32 editor drives itself. X11 has neither: the
// host owns the event loop, and a plugin that wants input has to be called.
//
// CLAP says so and provides both halves. posix-fd-support is the good one --
// hand over the X socket and the host calls back the moment something arrives,
// so a click is handled when it happens. timer-support is the fallback, at up
// to 33 ms of lag on every click, and better than a dead window.
//
// A host may offer neither. Then the editor draws once and never responds, so
// the wrapper does not open one at all: an editor that ignores the mouse is
// worse than a plugin that says it has no editor.

inline void editorOnFd(const clap_plugin_t* plugin, int fd, clap_posix_fd_flags_t) {
  Instance* inst = self(plugin);
  (void) fd;
  if (inst->guiIsNative) inst->nativeEditor.pumpEvents();
}

inline void editorOnTimer(const clap_plugin_t* plugin, clap_id timerId) {
  Instance* inst = self(plugin);
  if (!inst->guiIsNative || timerId != inst->editorTimerId) return;
  // pumpEvents drains input AND ticks; on a platform that needs no pumping it
  // does nothing, so tick() is still called for the editor's own clock.
  inst->nativeEditor.pumpEvents();
  inst->nativeEditor.tick();
}

inline const clap_plugin_posix_fd_support_t kPosixFdExt = {editorOnFd};
inline const clap_plugin_timer_support_t kTimerExt = {editorOnTimer};

/**
 * Ask the host to drive the editor, and say whether anyone will.
 *
 * Returns true when the SDK can drive it alone -- Windows -- or when a host
 * service was successfully registered.
 */
inline bool registerEditorPump(Instance* inst) {
  const int fd = inst->nativeEditor.connectionFd();
  if (fd < 0) return true; // this peer drives itself

  if (inst->host) {
    const auto* fdSupport = static_cast<const clap_host_posix_fd_support_t*>(
        inst->host->get_extension(inst->host, CLAP_EXT_POSIX_FD_SUPPORT));
    if (fdSupport && fdSupport->register_fd &&
        fdSupport->register_fd(inst->host, fd, CLAP_POSIX_FD_READ)) {
      inst->editorFd = fd;
      return true;
    }

    // No fd support: a timer still gets input through, late.
    const auto* timerSupport = static_cast<const clap_host_timer_support_t*>(
        inst->host->get_extension(inst->host, CLAP_EXT_TIMER_SUPPORT));
    clap_id id = 0;
    if (timerSupport && timerSupport->register_timer &&
        timerSupport->register_timer(inst->host, 33, &id)) {
      inst->editorTimerId = id;
      inst->editorTimerRunning = true;
      return true;
    }
  }
  return false;
}

inline void unregisterEditorPump(Instance* inst) {
  if (!inst->host) {
    inst->editorFd = -1;
    inst->editorTimerRunning = false;
    return;
  }
  if (inst->editorFd >= 0) {
    const auto* fdSupport = static_cast<const clap_host_posix_fd_support_t*>(
        inst->host->get_extension(inst->host, CLAP_EXT_POSIX_FD_SUPPORT));
    if (fdSupport && fdSupport->unregister_fd) fdSupport->unregister_fd(inst->host, inst->editorFd);
    inst->editorFd = -1;
  }
  if (inst->editorTimerRunning) {
    const auto* timerSupport = static_cast<const clap_host_timer_support_t*>(
        inst->host->get_extension(inst->host, CLAP_EXT_TIMER_SUPPORT));
    if (timerSupport && timerSupport->unregister_timer)
      timerSupport->unregister_timer(inst->host, inst->editorTimerId);
    inst->editorTimerRunning = false;
  }
}

inline bool guiCreate(const clap_plugin_t* plugin, const char* api, bool isFloating) {
  Instance* inst = self(plugin);
  if (isFloating || !api || std::strcmp(api, nativeWindowApi()) != 0) return false;
  inst->uiEchoValid = false;
  inst->guiCreated = true;
  // A fresh editor is a fresh document: whatever was pushed to the last one
  // is gone with it. The flag starts true and clears on the first tick, so
  // without this the SECOND editor of a session -- and any editor opened
  // after a preset load -- would never be sent the bag at all.
  inst->uiStateDirty = true;
  // Decided once per editor rather than once per build, because a host can
  // open and close an editor many times and the answer must not drift.
  const EditorChoice choice = editorChoice();
  if (choice.backend == EditorBackend::None) return false;
  inst->guiIsNative = choice.backend == EditorBackend::Native;
#if defined(SONORE_HAS_WEBVIEW_BACKEND)
  inst->webview.onMessage = [inst](const BridgeMessage& m) { guiOnMessage(inst, m); };
  inst->webview.onTick = [inst]() { guiTick(inst); };
#endif
  // The window itself is made in set_parent(): the host hands us the parent
  // there, and a child window has nothing to attach to before it.
  return true;
}

inline void guiDestroy(const clap_plugin_t* plugin) {
  Instance* inst = self(plugin);
  // Both, unconditionally. guiIsNative says which one was OPENED, and a
  // wrapper that only closed that one would leak the other after any path
  // that changed its mind -- which the webview fallback in set_parent does.
  unregisterEditorPump(inst);
  inst->nativeEditor.close();
#if defined(SONORE_HAS_WEBVIEW_BACKEND)
  inst->webview.destroy();
  // Park state dies with the GUI, or the NEXT editor this instance opens
  // would inherit a hidden-clock from the last one and park itself early.
  inst->webviewParked = false;
  inst->guiHiddenAtMs = 0;
  inst->guiParentHandle = nullptr;
#endif
  inst->guiCreated = false;
  inst->guiIsNative = false;
  inst->uiEchoValid = false;
}

/** Logical pixels to device pixels, rounded rather than truncated: at 150% a
 *  620-pixel editor is 930 and at 125% it is 775, and truncation loses a pixel
 *  off the edge on every odd multiple. */
inline uint32_t scaledPixels(uint32_t logical, double factor) {
  return (uint32_t) ((double) logical * factor + 0.5);
}

/**
 * The host telling us what a logical pixel is worth on this display.
 *
 * This used to return FALSE, meaning "I ignore your scale, I handle DPI
 * myself", with a comment saying the webview does it through the OS. Half
 * true, and the wrong half: the webview draws each CSS pixel as `scale`
 * DEVICE pixels, which is exactly why the window has to be `scale` times
 * bigger. It was already right in principle and still produced a 620-pixel
 * window showing the left two thirds of a 1240-pixel page at 200%.
 *
 * The VST3 side implements the same arithmetic through
 * IPlugViewContentScaleSupport; the two formats disagreeing about the same
 * webview would be a plugin that is sharp in one host and clipped in another.
 */
inline bool guiSetScale(const clap_plugin_t* plugin, double scale) {
  Instance* inst = self(plugin);
  if (!(scale > 0.0) || scale > 8.0) return false;
  inst->guiScale = scale;
  // Only if the window already exists. Before that the size is applied when
  // it is created, which is what gets the FIRST frame right rather than
  // correcting it visibly a moment later.
  if (inst->guiIsNative) {
    // The editor is told the SCALE, not a pre-multiplied size. It used to be
    // handed `logical * scale` as though that were a layout size, which laid
    // the component tree out in device pixels: at 200% the controls stayed the
    // same number of pixels across -- half their intended physical size -- and
    // twice as many fitted on screen. The window grew; the interface did not.
    if (inst->nativeEditor.isOpen()) inst->nativeEditor.setScale((float) scale);
    return true;
  }
#if defined(SONORE_HAS_WEBVIEW_BACKEND)
  if (inst->webview.handle())
    inst->webview.setSize(scaledPixels(inst->guiWidth, scale),
                          scaledPixels(inst->guiHeight, scale));
#endif
  return true;
}

/** In DEVICE pixels, which is what the extension asks for and what the host
 *  makes the window out of. */
inline bool guiGetSize(const clap_plugin_t* plugin, uint32_t* width, uint32_t* height) {
  Instance* inst = self(plugin);
  *width = scaledPixels(inst->guiWidth, inst->guiScale);
  *height = scaledPixels(inst->guiHeight, inst->guiScale);
  return true;
}

/** A fixed-size interface says so, and a host that asks stops drawing a grip
 *  the user cannot use. Either axis being resizable counts: CLAP asks the
 *  yes/no question here and the per-axis one in the hints below. */
inline bool guiCanResize(const clap_plugin_t*) {
  return kDesc.editorLimits.resizableHorizontally || kDesc.editorLimits.resizableVertically;
}

inline bool guiGetResizeHints(const clap_plugin_t*, clap_gui_resize_hints_t* hints) {
  const EditorConstraints& limits = kDesc.editorLimits;
  hints->can_resize_horizontally = limits.resizableHorizontally;
  hints->can_resize_vertically = limits.resizableVertically;

  // This said "HTML reflows; forcing the studio's aspect ratio on a resize
  // would fight the layout rather than help it" -- which is a good argument
  // for the DEFAULT and was being applied as a law. A plugin drawn as a fixed
  // skin has the opposite requirement and had no way to state it.
  hints->preserve_aspect_ratio = limits.aspectRatio > 0.0f;
  if (limits.aspectRatio > 0.0f) {
    // As a ratio of integers, which is what the extension asks for. Scaled by
    // a thousand rather than reduced: the host divides these, and 1778/1000 is
    // 16:9 to within a pixel at any size a screen has.
    hints->aspect_ratio_width = (uint32_t) (limits.aspectRatio * 1000.0f + 0.5f);
    hints->aspect_ratio_height = 1000;
  } else {
    hints->aspect_ratio_width = SONORE_UI_WIDTH;
    hints->aspect_ratio_height = SONORE_UI_HEIGHT;
  }
  return true;
}

inline bool guiAdjustSize(const clap_plugin_t* plugin, uint32_t* width, uint32_t* height) {
  if (!width || !height) return false;
  // DEVICE pixels in and out, as set_size takes them, while the limits are
  // LOGICAL -- so convert, clamp, convert back. Clamping the device numbers
  // directly approved 320x200 device pixels at 2x for a 320x200 minimum;
  // set_size then turned that into 160x100 logical, clamped it up to
  // 320x200, and the editor was 640x400 in a 320x200 window: clipped. A
  // dimension the clamp did not move is handed back exactly as asked, so a
  // size already in range never drifts by a rounding.
  Instance* inst = self(plugin);
  const double s = inst && inst->guiScale > 0.0 ? inst->guiScale : 1.0;
  const uint32_t logicalW = (uint32_t) ((double) *width / s + 0.5);
  const uint32_t logicalH = (uint32_t) ((double) *height / s + 0.5);
  uint32_t w = logicalW, h = logicalH;
  clampEditorSize(&w, &h);
  if (w != logicalW) *width = (uint32_t) ((double) w * s + 0.5);
  if (h != logicalH) *height = (uint32_t) ((double) h * s + 0.5);
  return true;
}

inline bool guiSetSize(const clap_plugin_t* plugin, uint32_t width, uint32_t height) {
  Instance* inst = self(plugin);
  // The host speaks DEVICE pixels here -- exactly what guiGetSize handed it --
  // while guiWidth/guiHeight are LOGICAL. Convert before storing, or the
  // logical field fills with device values and guiGetSize re-scales them: the
  // editor DOUBLED on every host round-trip at any non-1 scale, and the
  // inflated number was persisted into the state blob, so it infected saved
  // sessions and every other format's next editor. Invisible at 100%, which is
  // why REAPER validation on a 1x display never showed it.
  const double s = inst->guiScale > 0.0 ? inst->guiScale : 1.0;
  uint32_t logicalW = (uint32_t) ((double) width / s + 0.5);
  uint32_t logicalH = (uint32_t) ((double) height / s + 0.5);
  // Clamp in LOGICAL units -- the min/max bounds are logical -- also not only in
  // adjust_size: set_size is a separate entry point a host may use on its own.
  clampEditorSize(&logicalW, &logicalH);
  inst->guiWidth = logicalW;
  inst->guiHeight = logicalH;
  if (inst->guiIsNative) {
    // The native editor lays out in LOGICAL units and applies the scale itself
    // (see guiSetScale) -- hand it logical, not the host's device size, which
    // it would otherwise pass straight to setLogicalSize and lay out at double
    // size on a HiDPI display.
    inst->nativeEditor.setSize((int) logicalW, (int) logicalH);
    return true;
  }
#if defined(SONORE_HAS_WEBVIEW_BACKEND)
  // The webview is a device-pixel surface -- give it the device size back,
  // rebuilt from the CLAMPED logical size so a clamp is honoured.
  inst->webview.setSize(scaledPixels(logicalW, s), scaledPixels(logicalH, s));
#endif
  return true;
}

/** The handle a host hands over, as a void*. The one place platforms
 *  genuinely differ: the handle is a different type on each OS (HWND /
 *  NSView* / X window id). The three names are the same union member; keeping
 *  the branch here means nothing below it branches. */
inline void* parentHandle(const clap_window_t* window) {
#if defined(_WIN32)
  return (void*) window->win32;
#elif defined(__APPLE__)
  return window->cocoa;
#elif defined(__linux__)
  return (void*) (uintptr_t) window->x11;
#else
  (void) window;
  return nullptr;
#endif
}

/**
 * The user dragged an edge. ASK the host for that size.
 *
 * A plugin editor does not own its window and must never resize itself: the
 * frame belongs to the host, and a view that changed size on its own would sit
 * at one size inside a frame of another. So this asks, and nothing here
 * changes until the host answers -- which it may decline to do, and declining
 * is a legitimate answer.
 *
 * The answer is request_resize's own return value. True means the host has
 * taken the size and, the extension says, "doesn't have to call set_size()".
 * This waited for set_size all the same, so in a host that took the
 * extension at its word the frame grew and the editor did not: the window
 * bigger than what was drawn in it, get_size still reporting the old size,
 * and the session saving that one. An accepted size is applied here, through
 * set_size's own path; a host that calls set_size as well applies the same
 * size twice.
 *
 * Clamped before asking, through the same function every other size path uses.
 * The border already constrains the drag with the same numbers, so this is
 * belt and braces -- but it is the entry point a host sees, and an entry point
 * that trusts its caller because another one checks is an unchecked entry
 * point.
 *
 * In DEVICE pixels, because that is what the extension is defined in and what
 * the host builds the window out of.
 */
inline void requestHostResize(Instance* inst, int width, int height) {
  if (!inst->host) return;
  uint32_t w = width > 0 ? (uint32_t) width : 1u;
  uint32_t h = height > 0 ? (uint32_t) height : 1u;
  clampEditorSize(&w, &h);
  const auto* hostGui =
      (const clap_host_gui_t*) inst->host->get_extension(inst->host, CLAP_EXT_GUI);
  // A host is not required to offer this. One that does not simply gives no
  // way to resize from inside the plugin, and the drag does nothing -- which
  // is the honest outcome and not a failure to report.
  if (!hostGui || !hostGui->request_resize) return;
  const uint32_t deviceW = scaledPixels(w, inst->guiScale), deviceH = scaledPixels(h, inst->guiScale);
  if (hostGui->request_resize(inst->host, deviceW, deviceH)) guiSetSize(&inst->plugin, deviceW, deviceH);
}

namespace editordetail {
/** A DSP that draws its own editor declares
 *  `std::unique_ptr<gfx::AudioProcessorEditor> createEditor(const gfx::EditorHost&)`
 *  (a pointer to anything derived from it will do). */
template <typename T, typename = void>
struct HasOwnEditor : std::false_type {};
template <typename T>
struct HasOwnEditor<T, std::void_t<decltype(std::unique_ptr<gfx::AudioProcessorEditor>(
                           std::declval<T&>().createEditor(std::declval<const gfx::EditorHost&>())))>>
    : std::true_type {};
} // namespace editordetail

/**
 * [main-thread] The DSP's own editor, as what the native editor builds its
 * window's content from -- or nothing, and the generic editor it always had.
 *
 * Every format reaches it through the same Instance, so a plugin's face is
 * the same in CLAP, VST3 and AU. A template, as sendFile is, so a DSP without
 * the method never instantiates the call.
 */
template <typename T>
inline gfx::NativeEditor::ContentFactory customEditorFactory(T& dsp) {
  if constexpr (editordetail::HasOwnEditor<T>::value) {
    return [&dsp](const gfx::EditorHost& host) {
      return std::unique_ptr<gfx::AudioProcessorEditor>(dsp.createEditor(host));
    };
  } else {
    (void) dsp;
    return nullptr;
  }
}

/** Everything a native editor needs told before it opens. Two call sites --
 *  the ordinary one and the fallback when a webview fails to start -- and a
 *  helper so the second cannot quietly lack what the first has. */
inline void prepareNativeEditor(Instance* inst) {
  inst->nativeEditor.setResizeLimits(kDesc.editorLimits);
  inst->nativeEditor.onRequestResize = [inst](int w, int h) { requestHostResize(inst, w, h); };
  inst->nativeEditor.setContentFactory(customEditorFactory(inst->dsp));
}

inline bool guiSetParent(const clap_plugin_t* plugin, const clap_window_t* window) {
  Instance* inst = self(plugin);
  if (!window || !inst->guiCreated) return false;
  if (!window->api || std::strcmp(window->api, nativeWindowApi()) != 0) return false;
  void* parent = parentHandle(window);
  const uint32_t w = scaledPixels(inst->guiWidth, inst->guiScale);
  const uint32_t h = scaledPixels(inst->guiHeight, inst->guiScale);

  if (inst->guiIsNative) {
    // LOGICAL units, and the scale separately. The peer converts, because it is
    // the only thing that knows what display this window is about to appear on.
    // BEFORE open: the border is built there and reads the limits then.
    prepareNativeEditor(inst);
    if (!inst->nativeEditor.open(parent, kDesc.params, (int) kDesc.numParams, makeEditorHost(inst),
                                 (int) inst->guiWidth, (int) inst->guiHeight))
      return false;
    // Only when the host has actually said something. Left alone otherwise, so
    // the screen's own answer -- which the peer has already applied -- stands.
    if (inst->guiScale != 1.0) inst->nativeEditor.setScale((float) inst->guiScale);
    if (!registerEditorPump(inst)) {
      // Nothing will deliver input to this window. Closing it and answering no
      // lets the host fall back or report a real failure, rather than showing
      // a picture that never responds to anything.
      inst->nativeEditor.close();
      return false;
    }
    return true;
  }

#if defined(SONORE_HAS_WEBVIEW_BACKEND)
  inst->guiParentHandle = parent; // a parked editor re-attaches here on show
  inst->webviewParked = false;
  inst->guiHiddenAtMs = 0;
  const bool webOk = attachWebviewTo(inst, parent, w, h);
  if (webOk) return true;

  // The webview did not start. This is not hypothetical: WebView2 is absent on
  // a fresh Windows install until something deploys the runtime, and WebKitGTK
  // is a package a Linux user may simply not have. Until now that produced an
  // editor window with nothing in it -- the plugin played, automated and saved,
  // and not one control could be touched.
  //
  // The native editor does not need any of that, so use it. The plugin loses
  // its designed face and keeps every control, which is the right way round.
  //
  // Opened as the native branch above opens it: at the LOGICAL size, with
  // the host's scale applied apart from it. It was handed `w` and `h`, the
  // webview's DEVICE size, as its logical size and never told the scale, so
  // on a scaled display its layout was `scale` times too big -- and it kept
  // that size through every later set_scale: at 200% then 100%, a 1240-pixel
  // editor in the 620-pixel frame get_size reported. VST3's fallback was
  // right.
  prepareNativeEditor(inst);
  if (gfx::NativeEditor::isAvailable() &&
      inst->nativeEditor.open(parent, kDesc.params, (int) kDesc.numParams, makeEditorHost(inst),
                              (int) inst->guiWidth, (int) inst->guiHeight)) {
    if (inst->guiScale != 1.0) inst->nativeEditor.setScale((float) inst->guiScale);
    inst->guiIsNative = true;
    if (registerEditorPump(inst)) return true;
    inst->nativeEditor.close();
    inst->guiIsNative = false;
  }
  return false;
#else
  return false; // no webview backend in this build
#endif
}

inline bool guiSetTransient(const clap_plugin_t*, const clap_window_t*) {
  return false; // floating windows are not supported
}

inline void guiSuggestTitle(const clap_plugin_t*, const char*) {}

inline bool guiShow(const clap_plugin_t* plugin) {
  if (self(plugin)->guiIsNative) {
    if (!self(plugin)->nativeEditor.isOpen()) return false;
    self(plugin)->nativeEditor.setVisible(true);
    return true;
  }
#if defined(SONORE_HAS_WEBVIEW_BACKEND)
  Instance* inst = self(plugin);
  inst->guiHiddenAtMs = 0;
  // Parked while hidden: rebuild the webview on the parent the host gave us.
  // The page loads asynchronously exactly as it did on first open, and every
  // value it shows comes over the bridge from C++ state, so what the user
  // sees is a briefly blank editor that fills in -- not stale controls.
  if (inst->webviewParked) {
    inst->webviewParked = false;
    // DEVICE pixels, scaled from the logical size -- exactly as the first
    // attach in guiSetParent does. Passing raw guiWidth (logical) rebuilt the
    // webview at 1/scale of the frame: at 150% the page filled two thirds of
    // the window with dead space right and below.
    const uint32_t w = scaledPixels(inst->guiWidth, inst->guiScale);
    const uint32_t h = scaledPixels(inst->guiHeight, inst->guiScale);
    if (!inst->guiParentHandle || !attachWebviewTo(inst, inst->guiParentHandle, w, h))
      return false;
  }
  if (!inst->webview.handle()) return false;
  inst->webview.setVisible(true);
  return true;
#else
  (void) plugin;
  return false;
#endif
}

inline bool guiHide(const clap_plugin_t* plugin) {
  if (self(plugin)->guiIsNative) {
    if (!self(plugin)->nativeEditor.isOpen()) return false;
    self(plugin)->nativeEditor.setVisible(false);
    return true;
  }
#if defined(SONORE_HAS_WEBVIEW_BACKEND)
  Instance* inst = self(plugin);
  if (!inst->webview.handle()) return false;
  inst->webview.setVisible(false);
  // The clock the tick reads. Parking happens THERE rather than here so a
  // quick hide/show -- a user toggling between two editors -- never pays a
  // rebuild.
  inst->guiHiddenAtMs = steadyNowMs();
  return true;
#else
  (void) plugin;
  return false;
#endif
}

inline const clap_plugin_gui_t kGuiExt = {
    guiIsApiSupported, guiGetPreferredApi, guiCreate, guiDestroy,
    guiSetScale, guiGetSize, guiCanResize, guiGetResizeHints,
    guiAdjustSize, guiSetSize, guiSetParent, guiSetTransient,
    guiSuggestTitle, guiShow, guiHide,
};

inline const clap_plugin_preset_load_t kPresetLoadExt = {presetFromLocation};

inline const void* pluginGetExtension(const clap_plugin_t*, const char* id) {
  // Offered by every plugin, factory presets or not: a user preset is a file
  // any plugin can have, and the host's browser loads it through this.
  if (std::strcmp(id, CLAP_EXT_PRESET_LOAD) == 0 || std::strcmp(id, CLAP_EXT_PRESET_LOAD_COMPAT) == 0)
    return &kPresetLoadExt;
  // Offered unconditionally on the platforms that can need them. A host reads
  // these to know it MAY call us; publishing them without ever registering
  // anything costs nothing, and publishing them conditionally would depend on
  // whether an editor happens to be open when the host asks.
  if (std::strcmp(id, CLAP_EXT_POSIX_FD_SUPPORT) == 0) return &kPosixFdExt;
  if (std::strcmp(id, CLAP_EXT_TIMER_SUPPORT) == 0) return &kTimerExt;
  if (std::strcmp(id, CLAP_EXT_PARAMS) == 0) return &kParamsExt;
  if (std::strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0) return &kAudioPortsExt;
  if (std::strcmp(id, CLAP_EXT_NOTE_PORTS) == 0) return &kNotePortsExt;
  if (std::strcmp(id, CLAP_EXT_STATE) == 0) return &kStateExt;
  if (std::strcmp(id, CLAP_EXT_LATENCY) == 0) return &kLatencyExt;
  if (std::strcmp(id, CLAP_EXT_TAIL) == 0) return &kTailExt;
  // Offered only by a DSP that asked. A plugin that publishes this and
  // ignores what arrives makes a host do the work of gathering it for nobody.
  if ((std::strcmp(id, CLAP_EXT_TRACK_INFO) == 0 ||
       std::strcmp(id, CLAP_EXT_TRACK_INFO_COMPAT) == 0) &&
      WantsTrackInfo<SonoreDsp>::value)
    return &kTrackInfoExt;
  if (std::strcmp(id, CLAP_EXT_RENDER) == 0) return &kRenderExt;
  // Only where there is something to lay out. A plugin with no parameters has
  // no pages, and offering an extension that answers zero is noise.
  if (std::strcmp(id, CLAP_EXT_REMOTE_CONTROLS) == 0 && kDesc.numParams > 0)
    return &kRemoteControlsExt;
  // Offered only by an instrument that can actually answer. An effect has no
  // voices, and a DSP that never declared a capacity would be inventing one.
  if (std::strcmp(id, CLAP_EXT_VOICE_INFO) == 0 && kDesc.isInstrument &&
      HasVoiceCapacity<SonoreDsp>::value)
    return &kVoiceInfoExt;
  if (std::strcmp(id, CLAP_EXT_AUDIO_PORTS_ACTIVATION) == 0 ||
      std::strcmp(id, CLAP_EXT_AUDIO_PORTS_ACTIVATION_COMPAT) == 0)
    return &kAudioPortsActivationExt;
  // Offered only by a plugin that can genuinely be more than one width --
  // there is nothing to configure on a fixed-stereo effect, and answering
  // questions about it would be pretending otherwise.
  if ((std::strcmp(id, CLAP_EXT_CONFIGURABLE_AUDIO_PORTS) == 0 ||
       std::strcmp(id, CLAP_EXT_CONFIGURABLE_AUDIO_PORTS_COMPAT) == 0) &&
      minMainChannels() != maxMainChannels())
    return &kConfigurableAudioPortsExt;
  if (std::strcmp(id, CLAP_EXT_AUDIO_PORTS_CONFIG) == 0 &&
      minMainChannels() != maxMainChannels())
    return &kAudioPortsConfigExt;
  if (std::strcmp(id, CLAP_EXT_SURROUND) == 0 && maxMainChannels() > 2)
    return &kSurroundExt;
  if (std::strcmp(id, CLAP_EXT_GUI) == 0) return &kGuiExt;
  return nullptr;
}

inline void pluginOnMainThread(const clap_plugin_t* plugin) {
  // Where the audio thread's news gets delivered. It asked to be called here
  // because mark_dirty is [main-thread] and process() is not -- and neither
  // is a log, so the output guard's report comes the same way.
  markDirtyIfNeeded(self(plugin));
  reportOutputGuard(self(plugin));
  announceVoiceInfoIfChanged(self(plugin));
}

// ── Descriptor + factory ─────────────────────────────────────────────────────

inline const char* const* features() {
  static const char* fx[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_STEREO,
                             nullptr};
  static const char* synth[] = {CLAP_PLUGIN_FEATURE_INSTRUMENT,
                                CLAP_PLUGIN_FEATURE_SYNTHESIZER,
                                CLAP_PLUGIN_FEATURE_STEREO, nullptr};
  return kDesc.isInstrument ? synth : fx;
}

inline const clap_plugin_descriptor_t* descriptor() {
  static clap_plugin_descriptor_t d = {};
  static bool ready = false;
  if (!ready) {
    d.clap_version = CLAP_VERSION;
    d.id = kDesc.id;
    d.name = kDesc.name;
    d.vendor = kDesc.vendor;
    d.url = kDesc.url;
    d.manual_url = "";
    d.support_url = "";
    d.version = kDesc.version;
    d.description = kDesc.description;
    d.features = features();
    ready = true;
  }
  return &d;
}

inline uint32_t factoryCount(const clap_plugin_factory*) { return 1; }

inline const clap_plugin_descriptor_t* factoryGetDescriptor(const clap_plugin_factory*,
                                                            uint32_t index) {
  return index == 0 ? descriptor() : nullptr;
}

inline const clap_plugin_t* factoryCreate(const clap_plugin_factory*, const clap_host_t* host,
                                          const char* pluginId) {
  if (!pluginId || std::strcmp(pluginId, kDesc.id) != 0) return nullptr;
  void* mem = std::malloc(sizeof(Instance));
  if (!mem) return nullptr;
  Instance* inst = new (mem) Instance();
  inst->host = host;
  // CLAP is the format that simply hands it over: the host describes itself in
  // the struct it passes in. Copied rather than kept as pointers, because
  // those belong to the host and there is no promise about how long they live.
  if (host) {
    if (host->name) inst->hostInfo.name = host->name;
    if (host->vendor) inst->hostInfo.vendor = host->vendor;
    if (host->version) inst->hostInfo.version = host->version;
  }
  sendHostInfo(inst->dsp, inst->hostInfo);
  // NB: get_extension must NOT be called here. The header is explicit -- "You
  // must not call clap_host::get_extension() during
  // plugin-factory::create_plugin" -- so refreshTrackInfo(), which asks the host
  // for the track-info extension, waits for init() (the first place the host
  // promises get_extension is answerable). clap-validator flags a call here.
  inst->plugin.desc = descriptor();
  inst->plugin.plugin_data = inst;
  inst->plugin.init = pluginInit;
  inst->plugin.destroy = pluginDestroy;
  inst->plugin.activate = pluginActivate;
  inst->plugin.deactivate = pluginDeactivate;
  inst->plugin.start_processing = pluginStartProcessing;
  inst->plugin.stop_processing = pluginStopProcessing;
  inst->plugin.reset = pluginReset;
  inst->plugin.process = pluginProcess;
  inst->plugin.get_extension = pluginGetExtension;
  inst->plugin.on_main_thread = pluginOnMainThread;
  return &inst->plugin;
}

inline const clap_plugin_factory_t kFactory = {factoryCount, factoryGetDescriptor, factoryCreate};

// ── Preset discovery ─────────────────────────────────────────────────────────
//
// How a CLAP host's browser finds this plugin's presets without loading the
// plugin: the provider declares where they are, and the host asks it about
// each one. Two locations, both FILE:
//
//   factory  the compiled-in presets, written out as preset files in a cache
//            folder by syncFactoryPresetFiles (user_presets.h says why they
//            are files there and not only a "container" in the binary). When
//            that folder cannot be written, the presets are declared the old
//            way instead -- a PLUGIN location, load key = index -- so a
//            read-only home costs a validator's patience, not the presets.
//   user     the user's folder (user_presets.h), when it exists. Declaring a
//            folder that is not there is an error to a host, and making an
//            empty one in Documents for every plugin a host scans would be
//            rude, so a user who has never saved a preset gets the factory
//            location only -- and the folder at the next scan after the first
//            save.
//
// One preset per file, metadata from the file's header -- name, author,
// comment, category and tags as features, times -- and flags inherited from
// the location, so a file is factory or user by where it is.

inline const clap_preset_discovery_provider_descriptor_t* presetProviderDescriptor() {
  static char id[256];
  static bool built = false;
  if (!built) {
    std::snprintf(id, sizeof(id), "%s.presets", kDesc.id);
    built = true;
  }
  static clap_preset_discovery_provider_descriptor_t d = {};
  d.clap_version = CLAP_VERSION;
  d.id = id;
  d.name = kDesc.name;
  d.vendor = kDesc.vendor;
  return &d;
}

/** One preset-discovery provider the host created, as its provider_data:
 *  the clap object, the indexer it reports to, and the strings it handed the
 *  indexer, which must outlive the declare calls. Made by the preset
 *  discovery factory and freed by the provider's destroy, and never sees a
 *  plugin instance: a host indexes presets without loading the plugin. */
struct PresetProvider {
  clap_preset_discovery_provider_t provider{};
  const clap_preset_discovery_indexer_t* indexer = nullptr;
  std::string factoryFolder, userFolder, factoryName, userName, fileTypeName;
};

/** The folder the user's presets are in, or empty when it does not exist. */
inline std::string existingUserPresetFolder() {
  const std::string folder = defaultUserPresetFolder(kDesc.vendor ? kDesc.vendor : "", kDesc.name ? kDesc.name : "");
  return !folder.empty() && isDirectory(folder) ? folder : std::string();
}

inline bool hasPresetsToDiscover() {
  return (kDesc.presets && kDesc.numPresets > 0) || !existingUserPresetFolder().empty();
}

inline bool presetProviderInit(const clap_preset_discovery_provider_t* provider) {
  auto* self = static_cast<PresetProvider*>(provider->provider_data);
  if (!self || !self->indexer) return false;
  self->fileTypeName = std::string(kDesc.name ? kDesc.name : "Sonore") + " preset";
  clap_preset_discovery_filetype_t type{};
  type.name = self->fileTypeName.c_str();
  type.description = "A preset saved by the plugin: its whole state, with a name and metadata";
  type.file_extension = userPresetExtension() + 1; // without the dot
  if (!self->indexer->declare_filetype(self->indexer, &type)) return false;

  if (kDesc.presets && kDesc.numPresets > 0) {
    const std::string folder =
        defaultFactoryPresetFolder(kDesc.vendor ? kDesc.vendor : "", kDesc.name ? kDesc.name : "");
    PresetMetadata common;
    common.author = kDesc.vendor ? kDesc.vendor : "";
    common.pluginId = kDesc.id;
    common.pluginVersion = kDesc.version ? kDesc.version : "";
    std::vector<std::string> names;
    for (int i = 0; i < kDesc.numPresets; ++i) names.push_back(kDesc.presets[i].name ? kDesc.presets[i].name : "");
    clap_preset_discovery_location_t loc{};
    loc.flags = CLAP_PRESET_DISCOVERY_IS_FACTORY_CONTENT;
    self->factoryName = std::string(kDesc.name ? kDesc.name : "") + " factory presets";
    loc.name = self->factoryName.c_str();
    if (syncFactoryPresetFiles(folder, names, common, factoryPresetState)) {
      self->factoryFolder = folder;
      loc.kind = CLAP_PRESET_DISCOVERY_LOCATION_FILE;
      loc.location = self->factoryFolder.c_str();
    } else {
      loc.kind = CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN;
      loc.location = nullptr; // required to be null for PLUGIN locations
    }
    // A location the indexer refuses is left out, not the provider: the
    // other is still worth offering to a host that takes one and not both.
    self->indexer->declare_location(self->indexer, &loc);
  }

  self->userFolder = existingUserPresetFolder();
  if (!self->userFolder.empty()) {
    clap_preset_discovery_location_t loc{};
    loc.flags = CLAP_PRESET_DISCOVERY_IS_USER_CONTENT;
    self->userName = std::string(kDesc.name ? kDesc.name : "") + " user presets";
    loc.name = self->userName.c_str();
    loc.kind = CLAP_PRESET_DISCOVERY_LOCATION_FILE;
    loc.location = self->userFolder.c_str();
    self->indexer->declare_location(self->indexer, &loc);
  }
  return true;
}

inline void presetProviderDestroy(const clap_preset_discovery_provider_t* provider) {
  delete static_cast<PresetProvider*>(provider->provider_data);
}

/** The compiled-in presets as one container: the fallback location. */
inline bool presetProviderPluginMetadata(const clap_preset_discovery_metadata_receiver_t* receiver) {
  if (!kDesc.presets || kDesc.numPresets <= 0) return false;
  for (int i = 0; i < kDesc.numPresets; ++i) {
    char loadKey[32];
    std::snprintf(loadKey, sizeof(loadKey), "%d", i);
    if (!receiver->begin_preset(receiver, kDesc.presets[i].name, loadKey)) return false;
    clap_universal_plugin_id_t pluginId{};
    pluginId.abi = "clap";
    pluginId.id = descriptor()->id;
    receiver->add_plugin_id(receiver, &pluginId);
    receiver->set_flags(receiver, CLAP_PRESET_DISCOVERY_IS_FACTORY_CONTENT);
    if (kDesc.vendor && kDesc.vendor[0]) receiver->add_creator(receiver, kDesc.vendor);
  }
  return true;
}

/**
 * One preset file. Read from its header alone -- a host indexes thousands of
 * these and "get_metadata has to be fast" -- and a file that is not a preset
 * of this plugin is reported through on_error with the reason. A file for
 * ANOTHER plugin that happens to be in the folder declares nothing and is not
 * an error: it is simply not ours.
 */
inline bool presetProviderFileMetadata(const char* location,
                                       const clap_preset_discovery_metadata_receiver_t* receiver) {
  PresetMetadata meta;
  std::string why;
  if (!readPresetMetadata(location, &meta, &why)) {
    if (receiver->on_error) receiver->on_error(receiver, 0, why.c_str());
    return false;
  }
  if (meta.pluginId != kDesc.id) return true;
  // A file's name is not asked for: it is a single-preset file, which the
  // host names from the file -- unless the preset says otherwise.
  if (!receiver->begin_preset(receiver, meta.name.empty() ? nullptr : meta.name.c_str(), nullptr)) return true;
  clap_universal_plugin_id_t pluginId{};
  pluginId.abi = "clap";
  pluginId.id = descriptor()->id;
  receiver->add_plugin_id(receiver, &pluginId);
  if (!meta.author.empty()) receiver->add_creator(receiver, meta.author.c_str());
  if (!meta.comment.empty()) receiver->set_description(receiver, meta.comment.c_str());
  if (meta.created > 0 || meta.modified > 0)
    receiver->set_timestamps(receiver, meta.created > 0 ? (clap_timestamp) meta.created : CLAP_TIMESTAMP_UNKNOWN,
                             meta.modified > 0 ? (clap_timestamp) meta.modified : CLAP_TIMESTAMP_UNKNOWN);
  if (!meta.category.empty()) receiver->add_feature(receiver, meta.category.c_str());
  for (const std::string& tag : meta.tags) receiver->add_feature(receiver, tag.c_str());
  return true;
}

inline bool presetProviderGetMetadata(const clap_preset_discovery_provider_t*, uint32_t locationKind,
                                      const char* location,
                                      const clap_preset_discovery_metadata_receiver_t* receiver) {
  if (!receiver) return false;
  if (locationKind == CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN)
    return !location && presetProviderPluginMetadata(receiver);
  if (locationKind == CLAP_PRESET_DISCOVERY_LOCATION_FILE && location)
    return presetProviderFileMetadata(location, receiver);
  return false;
}

inline const void* presetProviderGetExtension(const clap_preset_discovery_provider_t*,
                                              const char*) {
  return nullptr;
}

/** Offered when there is something to browse: factory presets, or a user
 *  folder. A provider that indexes nothing is worse than no provider. */
inline uint32_t presetFactoryCount(const clap_preset_discovery_factory_t*) {
  return hasPresetsToDiscover() ? 1u : 0u;
}

inline const clap_preset_discovery_provider_descriptor_t* presetFactoryGetDescriptor(
    const clap_preset_discovery_factory_t*, uint32_t index) {
  if (index != 0 || !hasPresetsToDiscover()) return nullptr;
  return presetProviderDescriptor();
}

inline const clap_preset_discovery_provider_t* presetFactoryCreate(
    const clap_preset_discovery_factory_t*, const clap_preset_discovery_indexer_t* indexer,
    const char* providerId) {
  if (!indexer || !providerId) return nullptr;
  if (std::strcmp(providerId, presetProviderDescriptor()->id) != 0) return nullptr;
  auto* self = new PresetProvider();
  self->indexer = indexer;
  self->provider.desc = presetProviderDescriptor();
  self->provider.provider_data = self;
  self->provider.init = presetProviderInit;
  self->provider.destroy = presetProviderDestroy;
  self->provider.get_metadata = presetProviderGetMetadata;
  self->provider.get_extension = presetProviderGetExtension;
  return &self->provider;
}

inline const clap_preset_discovery_factory_t kPresetFactory = {
    presetFactoryCount, presetFactoryGetDescriptor, presetFactoryCreate};

/** What is wrong with kDesc, if anything: descriptorProblem(), plus the one
 *  thing only the wrapper can know -- that the table is the size every
 *  parameter array here was given. A user's build never asked: only the
 *  real-time test ran the check, so a duplicate id or an inverted channel
 *  range reached a host. */
inline const char* pluginDescriptorProblem(int* badParam = nullptr) {
  if (kDesc.numParams != SONORE_NUM_PARAMS) {
    if (badParam) *badParam = -1;
    return "a numParams that is not SONORE_NUM_PARAMS, the size of every parameter array in the wrappers";
  }
  return descriptorProblem(kDesc, badParam);
}

/** A debug build will not load with a malformed descriptor, and says why on
 *  stderr; a release build loads as it always did. The standalone's --verify
 *  fails on one in every build. */
inline bool entryInit(const char*) {
#if !defined(NDEBUG)
  int bad = -1;
  if (const char* why = pluginDescriptorProblem(&bad)) {
    if (bad >= 0)
      std::fprintf(stderr, "sonore: %s will not load: parameter %d of its descriptor has %s\n",
                   kDesc.name ? kDesc.name : "(unnamed)", bad, why);
    else
      std::fprintf(stderr, "sonore: %s will not load: its descriptor has %s\n",
                   kDesc.name ? kDesc.name : "(unnamed)", why);
    return false;
  }
#endif
  return true;
}
inline void entryDeinit() {}

inline const void* entryGetFactory(const char* factoryId) {
  if (!factoryId) return nullptr;
  if (std::strcmp(factoryId, CLAP_PLUGIN_FACTORY_ID) == 0) return &kFactory;
  // Only offered when there is something to browse: a provider that indexes
  // nothing is worse than no provider at all.
  if (hasPresetsToDiscover() &&
      (std::strcmp(factoryId, CLAP_PRESET_DISCOVERY_FACTORY_ID) == 0 ||
       std::strcmp(factoryId, CLAP_PRESET_DISCOVERY_FACTORY_ID_COMPAT) == 0))
    return &kPresetFactory;
  return nullptr;
}

} // namespace clapwrap
} // namespace sonore

// The DSO's one required symbol.
/** The descriptor this module was built from, for a test that compiles the
 *  plugin's source into itself (rt_safety_test) and wants to judge the
 *  declaration itself rather than what a format shows of it. Not exported
 *  from a built plugin: the version script and hidden visibility keep the
 *  entry points the only exports, and exports_test proves it. */
extern "C" const sonore::PluginDescriptor* sonore_test_descriptor() { return &kDesc; }

extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry = {
    CLAP_VERSION,
    sonore::clapwrap::entryInit,
    sonore::clapwrap::entryDeinit,
    sonore::clapwrap::entryGetFactory,
};
