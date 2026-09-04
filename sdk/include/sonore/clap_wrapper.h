// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: the CLAP format wrapper.
//
// One header that turns a `SonoreDsp` + a `PluginDescriptor` into a complete
// CLAP plugin: factory, entry point, params, audio/note ports, state, latency,
// and sample-accurate event handling. Include it exactly once in a translation
// unit that has already defined the DSP and the descriptor:
//
//   #define SONORE_NUM_PARAMS 4
//   #include <sonore/dsp.h>
//   struct SonoreDsp { ... };                       // the generated DSP
//   static const sonore::ParamInfo kParams[] = { ... };
//   static const sonore::PluginDescriptor kDesc = { ... };
//   #include <sonore/clap_wrapper.h>                 // defines clap_entry
//
// Why CLAP is the native format rather than a shim over something else: it is
// MIT, it is a C ABI (no framework runtime to link, no licence to negotiate),
// and its process() model, planar float buffers plus a sorted event list, is
// already the shape our ABI has used since the wasm preview. The mapping is
// nearly one-to-one, which is what keeps preview and shipped binary identical.
//
// Real-time discipline: process() allocates nothing, takes no locks and does no
// IO. Parameter values live in a plain array written on the audio thread from
// events and read by the main thread for get_value; a torn float read is worth
// less than a lock on the audio thread, and CLAP hosts flush params rather than
// racing them.
#pragma once

#include <clap/clap.h>
#include <clap/ext/posix-fd-support.h>
#include <clap/ext/surround.h>
#include <clap/ext/timer-support.h>
#include <clap/factory/preset-discovery.h>
#include <atomic>
#include <cstring>
#include <vector>
#include <chrono>
#include <cstdlib>
#include <new>
#include <type_traits>

#include "denormals.h"
#include "load.h"
#include "audio.h"
#include "plugin.h"
#include "state_bag.h"
#include "presets.h"
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

// The plugin's face. A generated project defines SONORE_UI_HTML to its own
// `uihtml`; without one the wrapper shows a plain generated panel, so a plugin
// is never faceless.
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
template <typename T, typename = void>
struct TakesMidi : std::false_type {};
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
template <typename T, typename = void>
struct TakesSidechain : std::false_type {};
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
template <typename T, typename = void>
struct TakesContext : std::false_type {};
template <typename T>
struct TakesContext<T, std::void_t<decltype(std::declval<T&>().process(
                           std::declval<ProcessContext&>(),
                           static_cast<const float*>(nullptr)))>> : std::true_type {};

// ── Can this DSP process in DOUBLE precision? ────────────────────────────────
// A DSP written as a template over the sample type answers yes for free; one
// written in plain float answers no, and every wrapper then DECLINES the
// host's 64-bit path rather than downcasting behind its back.
template <typename T, typename = void>
struct TakesDouble : std::false_type {};
template <typename T>
struct TakesDouble<T, std::void_t<decltype(std::declval<T&>().process(
                          std::declval<AudioBlock<double>&>(),
                          static_cast<const float*>(nullptr)))>> : std::true_type {};
template <typename T, typename = void>
struct TakesDoubleCtx : std::false_type {};
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
  if (!TakesContext<SonoreDsp>::value || !kDesc.auxOutputs) return 0;
  const int n = kDesc.numAuxOutputs;
  return (uint32_t) (n < 0 ? 0 : (n > 32 ? 32 : n));
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
template <typename T, typename = void>
struct WantsTransport : std::false_type {};
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
template <typename T>
struct WantsHostInfo<
    T, std::void_t<decltype(std::declval<T&>().setHostInfo(std::declval<const HostInfo&>()))>>
    : std::true_type {};

/** Told ONCE, when the plugin is created. A host does not change identity
 *  mid-session, and telling a DSP every block something that cannot change
 *  would be a per-block cost for a constant. */
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
  return info;
}

/** The persisted state blob. Versioned from day one: a plugin that ships and
 *  then gains a parameter must still load its old sessions. */
struct StateHeader {
  char magic[4];      // 'S','N','R','S'
  uint32_t version;   // 3
  uint32_t numParams; // params that follow, as float32
};
// v1: header + values.
// v2: appends one byte, the host bypass flag.
// v3: appends a StateBag: the key/value state a plugin has beyond its
//     parameters, which is the only way a sampler can remember WHICH FILE it
//     loaded. Loaders accept all three, and an older blob simply leaves the
//     newer fields at their defaults, so no saved session is ever orphaned.
constexpr uint32_t kStateVersion = 5;

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
template <typename T>
struct HasVoiceCapacity<T, std::void_t<decltype(std::declval<const T&>().voiceCapacity())>>
    : std::true_type {};

/** …and how many are sounding right now, which a host shows as a voice meter.
 *  Optional on top of the capacity: a DSP may know its size without counting
 *  what is active. */
template <typename T, typename = void>
struct HasActiveVoices : std::false_type {};
template <typename T>
struct HasActiveVoices<T, std::void_t<decltype(std::declval<const T&>().activeVoices())>>
    : std::true_type {};

template <typename T, typename = void>
struct HasStateBag : std::false_type {};
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

/** Host-driven bypass with a click-free crossfade and a latency-aligned dry
 *  path. VST3 (kIsBypass parameter), AU (BypassEffect property) and LV2
 *  (lv2:enabled port) all drive this; CLAP hosts bypass plugins themselves,
 *  so the CLAP path never engages it. The dry signal is delayed by the DSP's
 *  reported latency -- mixing an undelayed dry against latency-delayed wet
 *  during the ramp would comb-filter every bypass toggle. */
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

struct BypassState {
  SharedFlag engaged;   // what the host asked for, from whichever thread asked
  float mix = 0.0f;     // crossfade position: 0 = processing, 1 = fully dry
  float step = 0.001f;  // per-sample ramp increment (~20 ms)
  uint32_t delay = 0;   // dry-path delay = reported DSP latency
  uint32_t pos = 0;     // ring write cursor
  uint32_t channels = 2;
  // Stored as double so the SAME ring serves both precisions exactly: every
  // float is representable as a double and converts back unchanged, so a
  // bypassed 64-bit path is bit-exact rather than quietly rounded.
  std::vector<double> ring[kMaxAudioChannels];

  void prepare(double sampleRate, uint32_t maxFrames, uint32_t latencySamples,
               uint32_t numChannels = 2) {
    delay = latencySamples;
    channels = numChannels < 1 ? 1
               : numChannels > kMaxAudioChannels ? kMaxAudioChannels
                                                 : numChannels;
    step = sampleRate > 0.0 ? (float) (1.0 / (0.020 * sampleRate)) : 0.05f;
    const size_t n = (size_t) delay + (size_t) (maxFrames > 0 ? maxFrames : 1);
    for (uint32_t c = 0; c < kMaxAudioChannels; ++c)
      if (c < channels) ring[c].assign(n, 0.0);
      else ring[c].clear();
    pos = 0;
    mix = engaged ? 1.0f : 0.0f;
  }

  bool idle() const { return !engaged && mix <= 0.0f; }
};

/** Stash the dry input BEFORE the DSP runs (the wrappers process in place).
 *  With zero latency this may be skipped while idle -- the block an engage
 *  arrives in still captures in time. With latency it runs every block, or
 *  the first `delay` samples after an engage would crossfade against stale
 *  history. */
template <typename Sample>
inline void bypassCapture(BypassState& b, Sample* const* chans, uint32_t frames) {
  if (kDesc.isInstrument || b.ring[0].empty()) return;
  if (b.idle() && b.delay == 0) return;
  const size_t size = b.ring[0].size();
  for (uint32_t c = 0; c < b.channels; ++c) {
    if (c > 0 && chans[c] == chans[0]) break; // mono fed through aliased pointers
    const Sample* src = chans[c];
    for (uint32_t i = 0; i < frames; ++i) b.ring[c][(b.pos + i) % size] = (double) src[i];
  }
}

/** Crossfade the delayed dry path over the wet output and advance the ring.
 *  Instruments have no dry signal, so bypass fades them to silence. */
template <typename Sample>
inline void bypassApply(BypassState& b, Sample* const* chans, uint32_t frames) {
  const bool haveRing = !b.ring[0].empty();
  if (b.idle()) {
    if (haveRing) b.pos = (uint32_t) ((b.pos + frames) % b.ring[0].size());
    return;
  }
  const float target = b.engaged ? 1.0f : 0.0f;
  float mix = b.mix;
  if (kDesc.isInstrument || !haveRing) {
    for (uint32_t i = 0; i < frames; ++i) {
      mix += (target > mix) ? b.step : -b.step;
      mix = mix < 0.0f ? 0.0f : (mix > 1.0f ? 1.0f : mix);
      const Sample gain = (Sample) (1.0f - mix);
      for (uint32_t c = 0; c < b.channels; ++c) {
        if (c > 0 && chans[c] == chans[0]) break;
        chans[c][i] *= gain;
      }
    }
    b.mix = mix;
    return;
  }
  const size_t size = b.ring[0].size();
  for (uint32_t i = 0; i < frames; ++i) {
    mix += (target > mix) ? b.step : -b.step;
    mix = mix < 0.0f ? 0.0f : (mix > 1.0f ? 1.0f : mix);
    const size_t read = (b.pos + size + i - b.delay) % size;
    for (uint32_t c = 0; c < b.channels; ++c) {
      if (c > 0 && chans[c] == chans[0]) break;
      chans[c][i] = (Sample) (chans[c][i] * (1.0f - mix) + b.ring[c][read] * mix);
    }
  }
  b.mix = mix;
  b.pos = (uint32_t) ((b.pos + frames) % size);
}

struct Instance {
  clap_plugin_t plugin{};
  const clap_host_t* host = nullptr;

  SonoreDsp dsp{};
  /** The parameters as the host last set them, readable from any thread. */
  SharedParam params[SONORE_NUM_PARAMS > 0 ? SONORE_NUM_PARAMS : 1]{};
  /** The audio thread's copy for the block in hand -- what the DSP is given.
   *  Taken at the top of every process call by snapshotParams(). */
  float paramsBlock[SONORE_NUM_PARAMS > 0 ? SONORE_NUM_PARAMS : 1]{};
  MidiBuffer midi;
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
   *  which preset it was on. */
  int32_t selectedPreset = 0;
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
};

/** [audio-thread] Copy the shared parameters into the block's own array.
 *  One relaxed load per parameter, once per block; the DSP then reads plain
 *  floats that cannot move under it for the rest of the call. */
inline void snapshotParams(Instance* inst) {
  for (int i = 0; i < SONORE_NUM_PARAMS; ++i) inst->paramsBlock[i] = inst->params[i];
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

/** The bypass sits one past the DSP's own parameters, so every real parameter
 *  keeps the id it has always had and no saved automation is disturbed. */
constexpr clap_id kClapBypassParamId = (clap_id) SONORE_NUM_PARAMS;

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
  // CLAP's stepped flag promises the CONSECUTIVE integers min..max -- a host
  // steps such a control by one. A control stepping 2, 4, 6, 8 (or 0, 0.5,
  // 1) cannot keep that promise, so it is declared continuous here and the
  // parameter path snaps whatever arrives: the DSP still only sees a step,
  // and value_to_text still shows the step's name.
  if (stepsAreConsecutiveIntegers(p)) info->flags |= CLAP_PARAM_IS_STEPPED;
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
    *out = (text && (text[0] == 'O' || text[0] == 'o') && (text[1] == 'n' || text[1] == 'N')) ||
                   (text && text[0] == '1')
               ? 1.0
               : 0.0;
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

/** Apply one event to our state. Shared by process() and flush() so a parameter
 *  can never mean two different things depending on which path delivered it. */
inline void applyEvent(Instance* inst, const clap_event_header_t* hdr) {
  if (hdr->space_id != CLAP_CORE_EVENT_SPACE_ID) return;
  if (hdr->size < sizeof(clap_event_header_t)) return;
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
    case CLAP_EVENT_NOTE_ON: {
      const auto* e = eventAs<clap_event_note_t>(hdr);
      if (!e) break;
      const int ch = e->channel < 0 ? 0 : e->channel;
      const int vel = (int) (e->velocity * 127.0 + 0.5);
      inst->midi.addEvent(MidiMessage::noteOn(ch, e->key, vel < 1 ? 1 : vel), (int) hdr->time);
      break;
    }
    case CLAP_EVENT_NOTE_OFF:
    case CLAP_EVENT_NOTE_CHOKE: {
      const auto* e = eventAs<clap_event_note_t>(hdr);
      if (!e) break;
      const int ch = e->channel < 0 ? 0 : e->channel;
      if (e->key < 0) {
        // CLAP's wildcard: key -1 means "every note" -- how a host panic and a
        // choke-all arrive. Turned into the MIDI that says exactly that: All
        // Notes Off (CC 123), or All Sound Off (CC 120) for the harder choke,
        // both of which the toolkit's applyPedals() maps to allNotesOff(). A
        // raw noteOff would mask -1 to key 127 (data1 & 0x7f) and release the
        // ONE note nobody was playing, leaving the whole chord stuck on.
        const int cc = hdr->type == CLAP_EVENT_NOTE_CHOKE ? 120 : 123;
        inst->midi.addEvent(MidiMessage::controlChange(ch, cc, 0), (int) hdr->time);
      } else {
        inst->midi.addEvent(MidiMessage::noteOff(ch, e->key), (int) hdr->time);
      }
      break;
    }
    case CLAP_EVENT_NOTE_EXPRESSION: {
      // The native spelling: the host already resolved which note is meant.
      if (!kDesc.supportsMpe) break;
      const auto* e = eventAs<clap_event_note_expression_t>(hdr);
      if (!e) break;
      NoteExpressionBuffer::Entry entry;
      entry.noteId = e->note_id;
      entry.key = e->key;
      entry.channel = e->channel;
      entry.expression = (uint8_t) e->expression_id;
      entry.value = (float) e->value;
      entry.samplePosition = (int) hdr->time;
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
      inst->midi.addEvent(MidiMessage(status, e->data[1], e->data[2]), (int) hdr->time);
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
        inst->midi.addSysex(e->buffer, (size_t) e->size, (int) hdr->time);
      break;
    }
    default:
      break;
  }
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
        ev.header.type = e.kind == UiEventQueue::Event::Kind::GestureBegin
                             ? CLAP_EVENT_PARAM_GESTURE_BEGIN
                             : CLAP_EVENT_PARAM_GESTURE_END;
        ev.param_id = (clap_id) e.index;
        out->try_push(out, &ev.header);
        break;
      }
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
  for (uint32_t i = 0; i < n; ++i) applyEvent(inst, in->get(in, i));
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
  // be turning our knobs: without this, an edit made with transport stopped
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
// trades quality for CPU while monitoring can stop trading. Changing the mode
// re-runs prepare(), which is what lets a DSP using the simple process()
// signature -- the one that never sees a ProcessContext -- hear about it.

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
#if defined(SONORE_LICENSED_BUILD)
  // Every format prepares through this function, so this is the whole surface.
  licenseGate().setSampleRate(inst->sampleRate);
#endif
}

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
  // recorded (an atomic, read per block through ctx.offline) and the host is
  // asked for a restart -- which is [thread-safe] -- so that activate() runs
  // prepare() with the new mode, on the one thread and at the one moment
  // that is allowed to. A simple-signature DSP learns of the change there.
  if (inst->activated && inst->host) inst->host->request_restart(inst->host);
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
// CLAP_VOICE_INFO_SUPPORTS_OVERLAPPING_NOTES is the interesting half. It means
// two notes on the SAME KEY can sound at once, which is exactly what MPE does
// when two fingers land on one pitch. Claiming it was not possible until this
// commit: VoiceManager matched on key alone, so lifting either finger released
// both. The flag is set now because the behaviour is there now, and not one
// commit earlier.

/** Templated so the branch that is not taken is never INSTANTIATED.
 *
 *  `if constexpr` inside a plain function still requires both branches to
 *  compile, so asking a monophonic DSP for a voiceCapacity() it does not have
 *  is an error even when the condition is false. Making the DSP type a
 *  template parameter is what actually discards it. */
template <typename T>
inline bool fillVoiceInfo(const T& dsp, clap_voice_info_t* info) {
  if constexpr (HasVoiceCapacity<T>::value) {
    const int capacity = dsp.voiceCapacity();
    if (capacity <= 0) return false;
    info->voice_capacity = (uint32_t) capacity;
    if constexpr (HasActiveVoices<T>::value) {
      const int active = dsp.activeVoices();
      info->voice_count = (uint32_t) (active < 0 ? 0 : (active > capacity ? capacity : active));
    } else {
      // A DSP that does not count says its capacity is what is available,
      // which is what CLAP asks for when the plugin has no per-voice model to
      // report. Reporting zero would read as an instrument with no voices.
      info->voice_count = (uint32_t) capacity;
    }
    // Only where the descriptor says the DSP plays expressively. A synth that
    // is not built for per-note control has no reason to be handed two notes
    // on one key, and saying it can is how a host sends something the plugin
    // will mishandle.
    info->flags = kDesc.supportsMpe ? CLAP_VOICE_INFO_SUPPORTS_OVERLAPPING_NOTES : 0;
    return true;
  } else {
    (void) dsp;
    (void) info;
    return false;
  }
}

inline bool voiceInfoGet(const clap_plugin_t* plugin, clap_voice_info_t* info) {
  if (!info || !kDesc.isInstrument) return false;
  return fillVoiceInfo(self(plugin)->dsp, info);
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

/** Which parameters belong on page `pageIndex`, and what to call it.
 *  Returns false when the page does not exist. */
inline bool remoteControlsPageAt(uint32_t pageIndex, clap_remote_controls_page_t* page) {
  if (!page) return false;
  const GroupTable groups = collectGroups(kDesc.params, kDesc.numParams);

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
        if (groups.indexOf(kDesc.params[i].group) == g) ++inGroup;
      const int pagesHere = (inGroup + CLAP_REMOTE_CONTROLS_COUNT - 1) / CLAP_REMOTE_CONTROLS_COUNT;
      for (int sub = 0; sub < pagesHere; ++sub) {
        if (page_ == pageIndex) {
          int n = 0, seen = 0;
          for (int i = 0; i < kDesc.numParams && n < CLAP_REMOTE_CONTROLS_COUNT; ++i) {
            if (groups.indexOf(kDesc.params[i].group) != g) continue;
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
      if (groups.indexOf(kDesc.params[i].group) < 0) ++ungrouped;
    const int extra = (ungrouped + CLAP_REMOTE_CONTROLS_COUNT - 1) / CLAP_REMOTE_CONTROLS_COUNT;
    for (int sub = 0; sub < extra; ++sub) {
      if (page_ == pageIndex) {
        int n = 0, seen = 0;
        for (int i = 0; i < kDesc.numParams && n < CLAP_REMOTE_CONTROLS_COUNT; ++i) {
          if (groups.indexOf(kDesc.params[i].group) >= 0) continue;
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
 * The state format itself, told nothing about streams.
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
  float values[SONORE_NUM_PARAMS > 0 ? SONORE_NUM_PARAMS : 1];
  for (int i = 0; i < SONORE_NUM_PARAMS; ++i) values[i] = inst->params[i];
  if (!write(values, sizeof(float) * (size_t) SONORE_NUM_PARAMS)) return false;

  const uint8_t bypassByte = inst->bypass.engaged ? 1 : 0;
  if (!write(&bypassByte, 1)) return false;

  // v4: which preset is selected. BEFORE the bag, not after, because the bag
  // is read until the stream stops giving -- anything written past it is
  // swallowed by it and never seen again.
  const int32_t preset = inst->selectedPreset;
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
  const int32_t editorSize[2] = {(int32_t) inst->guiWidth, (int32_t) inst->guiHeight};
  if (!write(editorSize, sizeof(editorSize))) return false;

  // v3: whatever the DSP itself wants to remember.
  StateBag bag;
  saveDspState(inst->dsp, bag);
  std::vector<uint8_t> bagBytes;
  bag.serialise(bagBytes);
  if (!bagBytes.empty() && !write(bagBytes.data(), bagBytes.size())) return false;
  return true;
}

template <typename Get>
inline bool loadStateBody(Instance* inst, Get&& get) {
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

  StateBag bag;
  bool haveBag = false;
  if (h.version >= 3 && HasStateBag<SonoreDsp>::value) {
    // The bag runs to the end of the stream, so it is read until the stream
    // stops giving rather than by a length we would have to keep in sync.
    std::vector<uint8_t> bagBytes;
    uint8_t chunk[512];
    for (;;) {
      const size_t got = get(chunk, sizeof(chunk));
      if (got == 0) break;
      bagBytes.insert(bagBytes.end(), chunk, chunk + got);
    }
    haveBag = !bagBytes.empty() && bag.deserialise(bagBytes.data(), bagBytes.size());
  }

  // Nothing above touched the instance. From here on nothing can fail.
  for (int i = 0; i < SONORE_NUM_PARAMS; ++i) inst->params[i] = staged[i];
  inst->bypass.engaged = bypassEngaged;
  inst->selectedPreset = selectedPreset;
  inst->guiWidth = guiWidth;
  inst->guiHeight = guiHeight;
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

// ── Latency ──────────────────────────────────────────────────────────────────
//
// A DSP that delays its signal MUST say so, or the host cannot time-align it
// against the rest of the session: a look-ahead limiter or a convolution
// reverb that reports zero smears every parallel mix it sits in, silently.
//
// So this is not a constant: a DSP declares `int latencySamples() const` and
// the wrapper reports it. Detected like the other optional hooks, so anything
// zero-latency (most designs) neither declares nor pays for it.
template <typename T, typename = void>
struct HasLatency : std::false_type {};
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
template <typename T, typename = void>
struct HasStateDirty : std::false_type {};
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
  if (!HasLatency<SonoreDsp>::value || !inst->host) return;
  if (dspLatency(inst->dsp) == inst->reportedLatency) return;
  inst->host->request_restart(inst->host);
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
template <typename T, typename = void>
struct HasTail : std::false_type {};
template <typename T>
struct HasTail<T, std::void_t<decltype(std::declval<const T&>().tailSamples())>>
    : std::true_type {};

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
  if (!HasTail<SonoreDsp>::value || !inst->host) return;
  const uint32_t tail = dspTail(inst->dsp);
  if (inst->tailReported && tail == inst->reportedTail) return;
  inst->reportedTail = tail;
  inst->tailReported = true;
  if (const auto* ext =
          static_cast<const clap_host_tail_t*>(inst->host->get_extension(inst->host,
                                                                        CLAP_EXT_TAIL)))
    ext->changed(inst->host);
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
  ProcessSpec spec;
  spec.offline = inst->offline;
  spec.sampleRate = sampleRate;
  spec.maximumBlockSize = maxFrames;
  spec.numChannels = inst->mainChannels;
  inst->dsp.prepare(spec); // the one place allocation is allowed
  inst->bypass.prepare(sampleRate, maxFrames, dspLatency(inst->dsp), inst->mainChannels);
  if (TakesSidechain<SonoreDsp>::value) inst->scSilence.assign(maxFrames > 0 ? maxFrames : 1, 0.0f);
  if (TakesSidechain<SonoreDsp>::value && supportsDouble())
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
  inst->midi.clear();
  // A reset means the plugin is not in the middle of ANYTHING, and a bypass
  // crossfade in flight is something. Snapping it to its target rather than
  // letting it keep ramping is what makes a reset deterministic: the host
  // test caught the difference as telemetry that drifted by a few percent for
  // several blocks after every reset, which is the sort of wrongness that
  // never quite looks like a bug.
  inst->bypass.mix = inst->bypass.engaged ? 1.0f : 0.0f;
  inst->bypass.pos = 0;
  for (uint32_t c = 0; c < kMaxAudioChannels; ++c)
    for (double& v : inst->bypass.ring[c]) v = 0.0;
  // prepare() IS our reset: it clears every state variable the DSP owns.
  prepareDsp(inst);
}

/** constant_mask is an OUTPUT field on an output buffer: the plugin says
 *  which channels it left constant. The main port always said 0; the aux
 *  ports said whatever the host's memory held, and a host that reads a set
 *  bit skips a channel the DSP wrote. */
inline void clearAuxConstantMasks(const clap_process_t* process, uint32_t numAux) {
  for (uint32_t b = 0; b < numAux; ++b)
    if (1 + b < process->audio_outputs_count) process->audio_outputs[1 + b].constant_mask = 0;
}

inline clap_process_status pluginProcess(const clap_plugin_t* plugin,
                                         const clap_process_t* process) {
  Instance* inst = self(plugin);
  // The clock starts before anything in the block, so what is measured is
  // what the HOST waited for: event handling and bus setup, not just the DSP.
  const auto blockStart = std::chrono::steady_clock::now();
  const uint32_t frames = process->frames_count;

  // Drain the whole event list up front. Notes keep their sample offsets (the
  // DSP applies them per-frame), so a synth stays sample-accurate; parameters
  // land at block granularity, which is what smoothing is for.
  inst->midi.clear();
  inst->expression.clear();
  if (process->in_events) {
    const uint32_t n = process->in_events->size(process->in_events);
    for (uint32_t i = 0; i < n; ++i)
      applyEvent(inst, process->in_events->get(process->in_events, i));
  }
  // Raw-MIDI hosts express per note through MPE's channel layout instead of
  // through typed events, so the same block is read both ways.
  if (kDesc.supportsMpe) inst->mpe.process(inst->midi, inst->expression);

  // Then whatever the user did to the INTERFACE since the last block. These go
  // back out to the host as live events too, which is what makes turning a knob
  // on our webview recordable as automation exactly like a host-side control.
  drainUiEvents(inst, process->out_events);

  if (process->audio_outputs_count < 1 || frames == 0) return CLAP_PROCESS_CONTINUE;

  clap_audio_buffer_t* out = &process->audio_outputs[0];

  // ── 64-bit path ───────────────────────────────────────────────────────────
  // Taken only when the host handed us doubles AND the DSP genuinely speaks
  // them. Everything the float path does happens here too -- bypass (through
  // the shared exact ring), aux buses, meters -- at the host's precision.
  if constexpr (supportsDouble()) {
    if (!out->data32 && out->data64 && out->channel_count >= 1) {
      uint32_t nch64 = inst->mainChannels;
      if (out->channel_count < nch64) nch64 = out->channel_count;
      if (nch64 > kMaxAudioChannels) nch64 = kMaxAudioChannels;
      double* chans64[kMaxAudioChannels];
      for (uint32_t c = 0; c < nch64; ++c) chans64[c] = out->data64[c];

      if (kDesc.isInstrument) {
        for (uint32_t c = 0; c < nch64; ++c)
          std::memset(chans64[c], 0, sizeof(double) * frames);
      } else if (process->audio_inputs_count > 0 && process->audio_inputs[0].data64) {
        const clap_audio_buffer_t* in = &process->audio_inputs[0];
        for (uint32_t c = 0; c < nch64; ++c) {
          const double* src = in->data64[c < in->channel_count ? c : in->channel_count - 1];
          if (src && src != chans64[c]) std::memcpy(chans64[c], src, sizeof(double) * frames);
        }
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

      AudioBlock<double> auxBlocks64[kMaxAuxOutputs] = {};
      double* auxPtrs64[kMaxAuxOutputs][kMaxAudioChannels] = {};
      const uint32_t nAux64 = numAuxOutputs();
      for (uint32_t b = 0; b < nAux64; ++b) {
        uint32_t width = 0;
        const uint32_t portIndex = 1 + b;
        if (portIndex < process->audio_outputs_count) {
          const clap_audio_buffer_t& ab = process->audio_outputs[portIndex];
          if (ab.data64) {
            width = ab.channel_count;
            const uint32_t want = auxBusChannels(b);
            if (width > want) width = want;
            if (width > kMaxAudioChannels) width = kMaxAudioChannels;
            for (uint32_t c = 0; c < width; ++c) auxPtrs64[b][c] = ab.data64[c];
          }
        }
        auxBlocks64[b] = AudioBlock<double>(auxPtrs64[b], width, width ? frames : 0);
      }

      uint8_t roles64[kMaxAudioChannels];
      const uint32_t numRoles64 =
          inst->mainRoles(roles64, kMaxAudioChannels);
      AudioBlock<double> block64(chans64, nch64, frames);
      inst->midiOut.clear();
      ProcessContextT<double> ctx64{block64,   auxBlocks64, nAux64,
                                    scBlock64, inst->midi,  inst->midiOut,
                                    numRoles64 ? roles64 : nullptr, &inst->expression,
                                    &inst->outputPortActive.flags[1],
                                    inst->inputPortActive[1],
                                    inst->offline};
      bypassCapture(inst->bypass, chans64, frames);
      trackNotes(inst->notes, ctx64.midi);
      snapshotParams(inst);
      runDspCtx64(inst->dsp, ctx64, inst->paramsBlock);
      bypassApply(inst->bypass, chans64, frames);

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
      return CLAP_PROCESS_CONTINUE;
    }
  }

  if (!out->data32 || out->channel_count < 1) return CLAP_PROCESS_CONTINUE;

  // The block is as wide as BOTH sides agree on: the negotiated width, capped
  // by what the host actually delivered this call.
  uint32_t nch = inst->mainChannels;
  if (out->channel_count < nch) nch = out->channel_count;
  if (nch > kMaxAudioChannels) nch = kMaxAudioChannels;
  float* chans[kMaxAudioChannels];
  for (uint32_t c = 0; c < nch; ++c) chans[c] = out->data32[c];

  if (kDesc.isInstrument) {
    // A synth writes; it never reads an input that doesn't exist.
    for (uint32_t c = 0; c < nch; ++c) std::memset(chans[c], 0, sizeof(float) * frames);
  } else if (process->audio_inputs_count > 0 && process->audio_inputs[0].data32) {
    // Copy input to output when the host didn't give us in-place buffers.
    const clap_audio_buffer_t* in = &process->audio_inputs[0];
    for (uint32_t c = 0; c < nch; ++c) {
      const float* src = in->data32[c < in->channel_count ? c : in->channel_count - 1];
      if (src && src != chans[c]) std::memcpy(chans[c], src, sizeof(float) * frames);
    }
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
  AudioBlock<float> auxBlocks[kMaxAuxOutputs] = {};
  float* auxPtrs[kMaxAuxOutputs][kMaxAudioChannels] = {};
  const uint32_t nAux = numAuxOutputs();
  for (uint32_t b = 0; b < nAux; ++b) {
    const uint32_t portIndex = 1 + b;
    uint32_t width = 0;
    if (portIndex < process->audio_outputs_count) {
      const clap_audio_buffer_t& ab = process->audio_outputs[portIndex];
      if (ab.data32) {
        width = ab.channel_count;
        const uint32_t want = auxBusChannels(b);
        if (width > want) width = want;
        if (width > kMaxAudioChannels) width = kMaxAudioChannels;
        for (uint32_t c = 0; c < width; ++c) auxPtrs[b][c] = ab.data32[c];
      }
    }
    auxBlocks[b] = AudioBlock<float>(auxPtrs[b], width, width ? frames : 0);
  }

  inst->midiOut.clear();
  uint8_t roles[kMaxAudioChannels];
  const uint32_t numRoles = inst->mainRoles(roles, kMaxAudioChannels);
  // Aux flags start at index 1 of the output array: index 0 is the MAIN output,
  // which a DSP cannot skip and which ctx.aux does not include.
  ProcessContext ctx{block,      auxBlocks,     nAux,
                     scBlock,    inst->midi,    inst->midiOut,
                     numRoles ? roles : nullptr, &inst->expression,
                     &inst->outputPortActive.flags[1],
                     inst->inputPortActive[1],
                     inst->offline};
  // Stash the dry input, run the DSP, then crossfade back to the dry if the
  // host has engaged the bypass.
  //
  // These two calls existed only in the 64-bit path, which is the one almost
  // no host takes. So in CLAP the bypass has never worked: the state blob has
  // carried the flag since version 2 and BypassState has crossfaded and
  // latency-aligned all along, and none of it was ever reached at 32 bits.
  // Adding the parameter that lets a host press it is what finally made that
  // visible -- the check that measures the contract failed on the first run.
  bypassCapture(inst->bypass, chans, frames);
  trackNotes(inst->notes, ctx.midi);
  snapshotParams(inst);
  runDspCtx(inst->dsp, ctx, inst->paramsBlock);
  bypassApply(inst->bypass, chans, frames);

  // Emitted MIDI goes out as raw MIDI events, the dialect our output port
  // declares. Offsets are clamped into the block: an event scheduled past the
  // end is a host-visible protocol error, not something to pass along.
  if (kDesc.producesMidi && process->out_events) {
    for (const auto& e : inst->midiOut) {
      clap_event_midi_t ev{};
      ev.header.size = sizeof(ev);
      ev.header.type = CLAP_EVENT_MIDI;
      ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
      uint32_t t = (uint32_t) (e.samplePosition < 0 ? 0 : e.samplePosition);
      if (t >= frames) t = frames - 1;
      ev.header.time = t;
      ev.port_index = 0;
      ev.data[0] = (uint8_t) e.message.getRawStatus();
      ev.data[1] = (uint8_t) e.message.getRawData1();
      ev.data[2] = (uint8_t) e.message.getRawData2();
      process->out_events->try_push(process->out_events, &ev.header);
    }

    // SysEx out. The event carries a POINTER, and CLAP requires it to stay
    // valid until process() returns -- which is exactly why these bytes live
    // in the Instance's own arena rather than in a temporary here. Pointing
    // at a local would be a dangling read the moment this loop ended.
    for (auto it = inst->midiOut.sysexBegin(); it != inst->midiOut.sysexEnd(); ++it) {
      clap_event_midi_sysex_t ev{};
      ev.header.size = sizeof(ev);
      ev.header.type = CLAP_EVENT_MIDI_SYSEX;
      ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
      uint32_t t = (uint32_t) (it->samplePosition < 0 ? 0 : it->samplePosition);
      if (t >= frames) t = frames - 1;
      ev.header.time = t;
      ev.port_index = 0;
      ev.buffer = inst->midiOut.sysexData(*it);
      ev.size = it->length;
      process->out_events->try_push(process->out_events, &ev.header);
    }
  }

  // Publish what the meters should show. Two atomic stores: the interface can
  // be closed, opening, or absent and this costs the same either way.
  {
    // Vectorised: one pass each for peak and energy. Measured 1.7x native
    // and 1.4x in WebAssembly over the hand-rolled loop, and the meters run
    // on every block whether or not an editor is open.
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
  // The latency may have moved this block. A plugin cannot announce that from
  // here -- it asks to be restarted, which is thread-safe and therefore legal
  // from the audio thread.
  requestRestartIfLatencyChanged(inst);

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

  return CLAP_PROCESS_CONTINUE;
}

// ── Presets ──────────────────────────────────────────────────────────────────
// Factory presets are compiled in (see presets.h), so the load key is simply
// the preset's index. Loading one pushes every value through the SAME UI queue
// a knob uses, which is what makes a preset land on the audio thread safely AND
// arrive at the host as recordable parameter changes.

inline bool loadPresetByIndex(Instance* inst, int index) {
  if (index < 0 || index >= kDesc.numPresets || !kDesc.presets) return false;
  inst->uiStateDirty = true; // a preset can move anything, including the bag
  // Through applyPreset(), not around it.
  //
  // The rule -- refuse a preset whose value count does not match the contract,
  // clamp the ones that do -- lived in THREE places: presets.h, here, and the
  // VST3 wrapper. The copy in presets.h was the one with a test and the only
  // one with no callers, so a change to the rule would have been proved
  // correct and then not applied anywhere.
  float values[SONORE_NUM_PARAMS > 0 ? SONORE_NUM_PARAMS : 1];
  if (!applyPreset(kDesc.presets[index], kDesc.params, kDesc.numParams, values)) return false;
  for (int i = 0; i < kDesc.numParams; ++i) {
    UiEventQueue::Event e;
    e.kind = UiEventQueue::Event::Kind::ParamSet;
    e.index = i;
    e.value = values[i];
    inst->uiEvents.push(e);
    inst->uiEcho[i] = e.value;
  }
  inst->selectedPreset = index;
  // The events above reach the host on the next process() call, which is fine
  // while audio is running and useless while it is not: a user loading a
  // preset with the transport stopped would watch the host's generic editor
  // keep showing the old numbers until they pressed play. Asking for a flush
  // is how CLAP says "there is something waiting even though I am idle".
  if (inst->host) {
    if (const auto* hostParams = static_cast<const clap_host_params_t*>(
            inst->host->get_extension(inst->host, CLAP_EXT_PARAMS)))
      hostParams->request_flush(inst->host);
  }
  return true;
}

inline bool presetFromLocation(const clap_plugin_t* plugin, uint32_t locationKind,
                               const char* /*location*/, const char* loadKey) {
  // Only presets that live INSIDE the plugin exist here; there is no file
  // format to read, by design.
  if (locationKind != CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN || !loadKey) return false;
  char* end = nullptr;
  const long index = std::strtol(loadKey, &end, 10);
  if (end == loadKey) return false;
  return loadPresetByIndex(self(plugin), (int) index);
}

// ── GUI ──────────────────────────────────────────────────────────────────────
// The plugin's face is the same HTML the studio preview renders, in an OS
// webview, wired through the `window.sonore` bridge. Only the EMBEDDED case is
// supported: it is what every host implements, and a floating window would give
// us a second lifetime to get wrong for no user-visible gain.

/** The page source: the generated `uihtml` when the project defines one, else a
 *  plain generated panel: a plugin is never faceless. */
inline const std::string& uiHtml() {
  static const std::string html =
#if defined(SONORE_UI_HTML)
      std::string(SONORE_UI_HTML);
#else
      fallbackHtml(kDesc);
#endif
  return html;
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

/**
 * [main-thread] The four things the native editor needs from the plugin.
 *
 * Every edit goes down the SAME queue the page uses, so a knob turned here is
 * automation the host records, an undo step the host can collapse, and a value
 * the DSP receives on the audio thread -- not a private number the editor keeps
 * to itself.
 */
/** Defined below, next to the rest of the context-menu plumbing. Declared here
 *  because makeEditorHost binds it into the editor and comes first. */
inline bool showHostContextMenu(Instance* inst, int paramIndex, int x, int y);

inline gfx::EditorHost makeEditorHost(Instance* inst) {
  gfx::EditorHost host;
  host.getParameter = [inst](int index) -> float {
    if (index < 0 || index >= SONORE_NUM_PARAMS) return 0.0f;
    return inst->params[index];
  };
  host.setParameter = [inst](int index, float value) {
    if (index < 0 || index >= SONORE_NUM_PARAMS) return;
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
  };
  host.beginGesture = [inst](int index) {
    if (index < 0 || index >= SONORE_NUM_PARAMS) return;
    UiEventQueue::Event e;
    e.kind = UiEventQueue::Event::Kind::GestureBegin;
    e.index = index;
    inst->uiEvents.push(e);
  };
  host.endGesture = [inst](int index) {
    if (index < 0 || index >= SONORE_NUM_PARAMS) return;
    UiEventQueue::Event e;
    e.kind = UiEventQueue::Event::Kind::GestureEnd;
    e.index = index;
    inst->uiEvents.push(e);
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

  inst.meter.tick(0.033);
  char lv[40], db[40], vu[40], meterCall[160];
  jsNumber(lv, sizeof(lv), (double) inst.meter.level());
  jsNumber(db, sizeof(db), (double) inst.meter.db());
  jsNumber(vu, sizeof(vu), (double) inst.meter.vu());
  std::snprintf(meterCall, sizeof(meterCall), "window.sonore.__meter(%s,%s,%s);", lv, db, vu);
  js += meterCall;
  char cpu[40], peak[40], cpuCall[200];
  jsNumber(cpu, sizeof(cpu), (double) inst.load.load());
  jsNumber(peak, sizeof(peak), (double) inst.load.peakLoad());
  std::snprintf(cpuCall, sizeof(cpuCall),
                "if(window.sonore.__cpu)window.sonore.__cpu(%s,%s,%llu);", cpu, peak,
                (unsigned long long) inst.load.xruns());
  js += cpuCall;
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
    const char* env = std::getenv("SONORE_WEBVIEW_PARK_MS");
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
  return inst->webview.create((HWND) parent, w, h, uiHtml(), bridgeScript(kDesc), kDesc.id);
#elif defined(__APPLE__)
  return inst->webview.create(parent, w, h, uiHtml(), bridgeScript(kDesc), kDesc.id);
#else
  return inst->webview.create((unsigned long) (uintptr_t) parent, w, h, uiHtml(),
                              bridgeScript(kDesc), kDesc.id);
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
  inst->webview.eval(uiTickScript(*inst));
#else
  (void) inst;
#endif
}

/** A page message arriving on the main thread. It never touches audio state
 *  directly: everything crosses to the audio thread through the lock-free
 *  queue, which is what keeps a UI event out of the audio callback's way. */
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

inline void guiOnMessage(Instance* inst, const BridgeMessage& msg) {
  UiEventQueue::Event e;
  switch (msg.kind) {
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
    case BridgeMessage::Kind::Set:
      if (msg.index < 0 || msg.index >= SONORE_NUM_PARAMS) return;
      e.kind = UiEventQueue::Event::Kind::ParamSet;
      e.index = msg.index;
      e.value = (float) msg.value;
      // Echo locally so the timer doesn't immediately fight the user's knob.
      inst->uiEcho[msg.index] = clampToRange(kDesc.params[msg.index], (float) msg.value);
      break;
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

inline bool guiAdjustSize(const clap_plugin_t*, uint32_t* width, uint32_t* height) {
  clampEditorSize(width, height);
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

/** The one place platforms genuinely differ: the handle a host hands over is a
 *  different type on each OS (HWND / NSView* / X window id). */
/** The handle a host hands over, as a void*. The three names are the same
 *  union member; keeping the branch here means nothing below it branches. */
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
 * changes until the host answers through gui_set_size -- which it may decline
 * to do, and declining is a legitimate answer.
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
  hostGui->request_resize(inst->host, scaledPixels(w, inst->guiScale),
                          scaledPixels(h, inst->guiScale));
}

/** Everything a native editor needs told before it opens. Two call sites --
 *  the ordinary one and the fallback when a webview fails to start -- and a
 *  helper so the second cannot quietly lack what the first has. */
inline void prepareNativeEditor(Instance* inst) {
  inst->nativeEditor.setResizeLimits(kDesc.editorLimits);
  inst->nativeEditor.onRequestResize = [inst](int w, int h) { requestHostResize(inst, w, h); };
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
  prepareNativeEditor(inst);
  if (gfx::NativeEditor::isAvailable() &&
      inst->nativeEditor.open(parent, kDesc.params, (int) kDesc.numParams, makeEditorHost(inst),
                              (int) w, (int) h)) {
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
  if (kDesc.numPresets > 0 &&
      (std::strcmp(id, CLAP_EXT_PRESET_LOAD) == 0 ||
       std::strcmp(id, CLAP_EXT_PRESET_LOAD_COMPAT) == 0))
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
  // because mark_dirty is [main-thread] and process() is not.
  markDirtyIfNeeded(self(plugin));
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
// Our factory presets are compiled into the binary, which CLAP models as a
// location of kind PLUGIN: nothing on disk to crawl, the plugin simply
// declares what it carries. Without this the host's preset BROWSER never sees
// them -- loadPresetByIndex only answers once something already picked one.
//
// The load-key is the preset INDEX as text, matching what state::load-preset
// already accepts, so browsing and loading agree by construction.

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

struct PresetProvider {
  clap_preset_discovery_provider_t provider{};
  const clap_preset_discovery_indexer_t* indexer = nullptr;
};

inline bool presetProviderInit(const clap_preset_discovery_provider_t* provider) {
  auto* self = static_cast<PresetProvider*>(provider->provider_data);
  if (!self || !self->indexer) return false;
  clap_preset_discovery_location_t loc{};
  loc.flags = CLAP_PRESET_DISCOVERY_IS_FACTORY_CONTENT;
  loc.name = kDesc.name;
  loc.kind = CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN;
  loc.location = nullptr; // required to be null for PLUGIN locations
  return self->indexer->declare_location(self->indexer, &loc);
}

inline void presetProviderDestroy(const clap_preset_discovery_provider_t* provider) {
  delete static_cast<PresetProvider*>(provider->provider_data);
}

inline bool presetProviderGetMetadata(
    const clap_preset_discovery_provider_t*, uint32_t locationKind, const char* location,
    const clap_preset_discovery_metadata_receiver_t* receiver) {
  if (locationKind != CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN || location || !receiver)
    return false;
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

inline const void* presetProviderGetExtension(const clap_preset_discovery_provider_t*,
                                              const char*) {
  return nullptr;
}

inline uint32_t presetFactoryCount(const clap_preset_discovery_factory_t*) {
  return (kDesc.presets && kDesc.numPresets > 0) ? 1u : 0u;
}

inline const clap_preset_discovery_provider_descriptor_t* presetFactoryGetDescriptor(
    const clap_preset_discovery_factory_t*, uint32_t index) {
  if (index != 0 || !kDesc.presets || kDesc.numPresets <= 0) return nullptr;
  return presetProviderDescriptor();
}

inline const clap_preset_discovery_provider_t* presetFactoryCreate(
    const clap_preset_discovery_factory_t*, const clap_preset_discovery_indexer_t* indexer,
    const char* providerId) {
  if (!indexer || !providerId) return nullptr;
  if (std::strcmp(providerId, presetProviderDescriptor()->id) != 0) return nullptr;
  if (!kDesc.presets || kDesc.numPresets <= 0) return nullptr;
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

inline bool entryInit(const char*) { return true; }
inline void entryDeinit() {}

inline const void* entryGetFactory(const char* factoryId) {
  if (!factoryId) return nullptr;
  if (std::strcmp(factoryId, CLAP_PLUGIN_FACTORY_ID) == 0) return &kFactory;
  // Only offered when there is something to browse: a provider that indexes
  // nothing is worse than no provider at all.
  if (kDesc.presets && kDesc.numPresets > 0 &&
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
