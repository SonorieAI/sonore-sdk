// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: the WebAssembly ABI glue (GENERATED, never authored against).
//
// Appended by the compiler after the model's DSP so the SAME `SonoreDsp` that
// ships inside the native CLAP also drives the browser preview. The exported C
// ABI is byte-for-byte the one the AudioWorklet host already speaks
// (src/lib/worklet.ts → buildWasmWorkletModule): planar f32, channel stride =
// SONORE_MAX_FRAMES, 2 channels, params in a flat array, MIDI through a
// [frameOffset, status, data1, data2] × 64 inlet.
//
// Keeping the export names (`sonore_*`) identical to the previous ABI is
// deliberate: cached wasm, stored revision sources and the compiler service
// contract all key off them, and there is no user-visible gain in renaming.
#pragma once
#ifndef SONORE_NUM_PARAMS
#error "Define SONORE_NUM_PARAMS (and struct SonoreDsp) before the ABI glue."
#endif

#include <type_traits>
#include "audio.h"
#include "output_guard.h"
#include "transport.h"

#define SONORE_MAX_FRAMES 128
#define SONORE_CHANNELS   2
#define SONORE_MAX_EVENTS 64

// The output guard (output_guard.h), as every plugin format runs it: a block
// with a NaN or an infinity in it leaves as silence, so the preview plays what
// the shipped plugin plays. On unless the build says otherwise. The descriptor
// is not part of this build -- the glue sees only the DSP -- so a plugin whose
// descriptor sets guardOutput = false is compiled with SONORE_GUARD_OUTPUT=0
// by whoever composes the build, or the preview guards what the plugin will
// not.
#ifndef SONORE_GUARD_OUTPUT
#define SONORE_GUARD_OUTPUT 1
#endif

static float sonore_in_buf[SONORE_MAX_FRAMES * SONORE_CHANNELS];
static float sonore_out_buf[SONORE_MAX_FRAMES * SONORE_CHANNELS];
static float sonore_param_buf[(SONORE_NUM_PARAMS > 0) ? SONORE_NUM_PARAMS : 1];
static float sonore_midi_buf[SONORE_MAX_EVENTS * 4];
static int sonore_midi_n = 0;
/** How many blocks the guard has silenced, for the host to read and report:
 *  this module has no log of its own to write to. */
static sonore::detail::OutputGuard sonore_guard;

/** [audio-thread] One instance's block through the guard: the whole block
 *  silenced and counted if any sample of it is not finite. */
static inline void sonore_guard_block(sonore::detail::OutputGuard& guard, float* const* chans,
                                      int n) {
#if SONORE_GUARD_OUTPUT
  guard.check<float>(chans, (uint32_t) SONORE_CHANNELS, nullptr, 0, (size_t) n);
#else
  (void) guard;
  (void) chans;
  (void) n;
#endif
}
// VALUE-initialised with `{}`, matching clap_wrapper.h's `SonoreDsp dsp{}`
// EXACTLY, and the braces are the whole point of this line.
//
// `static SonoreDsp x;` default-initialises; `SonoreDsp x{}` value-initialises,
// and on an aggregate that copy-initialises every member from `{}`. A member
// whose default constructor is `explicit` is well-formed under the first and
// ILL-FORMED under the second. So the preview compiled plugins the shipped
// binary cannot, and the difference surfaced as a macOS farm build dying on
// `clap_wrapper.h: chosen constructor is explicit in copy-initialization` for a
// plugin whose preview was perfectly healthy -- after it had been published and
// sold with only a Linux binary. The runtime result is identical either way;
// what changes is that the preview now refuses what the plugin would refuse.
//
// This is the "preview is bit-identical to the shipped plugin" promise being
// load-bearing rather than decorative: every construct the native wrappers use
// has to appear here in the SAME FORM, or the studio quietly accepts code the
// farm will reject.
static SonoreDsp sonore_dsp_instance{};
static sonore::MidiBuffer sonore_midi_events;

// Which process() signature does SonoreDsp have? The SAME three the native
// wrappers detect, so the preview runs every plugin shape the shipped binary
// does, not just the two simplest. The dispatch lives in a TEMPLATE so
// `if constexpr` genuinely discards the untaken branch; in a plain function
// both branches would still type-check, which is exactly the mismatch this
// detection exists to avoid.
//
//   effect:      process(AudioBlock&, const float*)
//   instrument:  process(AudioBlock&, const float*, MidiBuffer&)
//   rich:        process(ProcessContext&, const float*)   -- aux/sidechain/MPE
/** The preview's copy of clapwrap::TakesMidi: true when SonoreDsp declares
 *  the instrument signature, `process(AudioBlock<float>&, const float*,
 *  MidiBuffer&)`. A copy rather than the wrapper's own because this glue is
 *  built without clap_wrapper.h; the two must detect the same shapes. */
template <typename T, typename = void>
struct SonoreTakesMidi : std::false_type {};
/** The match for SonoreTakesMidi: T declares the MIDI-taking process(). */
template <typename T>
struct SonoreTakesMidi<T, std::void_t<decltype(std::declval<T&>().process(
                              std::declval<sonore::AudioBlock<float>&>(),
                              static_cast<const float*>(nullptr),
                              std::declval<sonore::MidiBuffer&>()))>> : std::true_type {};

// The sidechain form -- process(main, key, params) -- is the FOURTH shape the
// native wrappers accept, and the first version of this glue had no branch for
// it: a ducker compiled and shipped as a plugin and failed to compile for the
// preview with "too few arguments". Found by putting every example through
// this file. The key is silent here, as it is in a DAW with nothing routed.
/** The preview's copy of clapwrap::TakesSidechain: true when SonoreDsp
 *  declares `process(AudioBlock<float>& main, AudioBlock<float>& key,
 *  const float*)`. The key is what the host wrote through
 *  sonore_sidechain_ptr(), and silence when it writes nothing. */
template <typename T, typename = void>
struct SonoreTakesSidechain : std::false_type {};
/** The match for SonoreTakesSidechain: T declares the two-block process(). */
template <typename T>
struct SonoreTakesSidechain<T, std::void_t<decltype(std::declval<T&>().process(
                                   std::declval<sonore::AudioBlock<float>&>(),
                                   std::declval<sonore::AudioBlock<float>&>(),
                                   static_cast<const float*>(nullptr)))>> : std::true_type {};

/** Does this build want a key at all?
 *
 *  Two signatures read one: the sidechain form takes it as an argument, and the
 *  rich context form finds it on the context. Everything else must pay nothing
 *  -- which is why the buffer lives on a TEMPLATE. A static member of a
 *  template is emitted only if it is used, and the `if constexpr` in
 *  `sonore_sidechain_ptr` below does not instantiate the branch it does not
 *  take, so a plugin with no key input carries no key buffer, no extra bss and
 *  no change to where its heap starts. That property is load-bearing: the
 *  pooling round proved itself by hashing a plugin built before and after the
 *  change and getting the same bytes, and a header that quietly grew every
 *  binary would take that proof away from whoever needs it next. */
template <bool Want>
struct SonoreKeyStore {
  static float buf[SONORE_MAX_FRAMES * SONORE_CHANNELS];
};
template <bool Want>
float SonoreKeyStore<Want>::buf[SONORE_MAX_FRAMES * SONORE_CHANNELS];

/** The preview's copy of clapwrap::TakesContext: true when SonoreDsp declares
 *  `process(ProcessContext&, const float*)`. Checked first, as the wrappers
 *  do. The preview fills the context's main block, MIDI and key, and leaves
 *  aux outputs and per-note expression empty. */
template <typename T, typename = void>
struct SonoreTakesContext : std::false_type {};
/** The match for SonoreTakesContext: T declares the context-taking process(). */
template <typename T>
struct SonoreTakesContext<T, std::void_t<decltype(std::declval<T&>().process(
                                 std::declval<sonore::ProcessContext&>(),
                                 static_cast<const float*>(nullptr)))>> : std::true_type {};

// MIDI the DSP emits (context path); the preview has no MIDI-out sink, so it is
// cleared each block and discarded. Kept in static storage so the reference the
// context holds stays valid for the whole call.
static sonore::MidiBuffer sonore_midi_out_events;

// What the host says about time. The browser has no timeline, so the studio
// SUPPLIES one -- a tempo the user sets and a position that advances while
// "playing" -- because a DSP that syncs to the beat (SyncedLfo, a dotted
// eighth delay) was previewing at its 120 BPM fallback and shipping into a
// DAW at whatever the song runs at. The one thing the preview promises is
// that it sounds like the plugin; without this it did not, for exactly the
// plugins where tempo is the point.
static sonore::TransportInfo sonore_transport;
/** The bar, as the host last stated it. Four four until told otherwise, which
 *  is the right default and was until now the only answer this ABI had. */
static int sonore_sig_num = 4, sonore_sig_den = 4;

/** The preview's copy of clapwrap::WantsTransport: true when SonoreDsp
 *  declares `void setTransport(const TransportInfo&)`, which the glue then
 *  calls before every block with the studio's tempo and position. */
template <typename T, typename = void>
struct SonoreWantsTransport : std::false_type {};
/** The match for SonoreWantsTransport: T declares setTransport(). */
template <typename T>
struct SonoreWantsTransport<T, std::void_t<decltype(std::declval<T&>().setTransport(
                                   std::declval<const sonore::TransportInfo&>()))>>
    : std::true_type {};

/** Does this DSP delay its signal, and by how much? Same compile-time
 *  detection as the MIDI and transport hooks, and the same one the CLAP
 *  wrapper already uses -- a DSP that does not delay pays nothing for the ones
 *  that do. */
template <typename T, typename = void>
struct SonoreHasLatency : std::false_type {};
/** The match for SonoreHasLatency: T declares `latencySamples() const`. */
template <typename T>
struct SonoreHasLatency<T, std::void_t<decltype(std::declval<const T&>().latencySamples())>>
    : std::true_type {};

/** INSIDE A TEMPLATE, for the same reason sonore_run_dsp is: `if constexpr` in
 *  a PLAIN function still type-checks the dead branch, so a DSP with no
 *  latencySamples() fails to compile on a call it never makes. The first
 *  version of this was a plain extern "C" function and broke every example in
 *  the SDK that does not delay -- which is most of them, and which the wasm
 *  build test said immediately. */
template <typename T>
static int sonore_latency_of(const T& dsp) {
  if constexpr (SonoreHasLatency<T>::value) return (int) dsp.latencySamples();
  else { (void) dsp; return 0; }
}

// Turn a host's inlet into sonore_midi_events. Shared by the instrument and
// the context paths; consuming it (count -> 0) means a stale block can't replay.
// The inlet is a parameter, not the module's: in a pooled module each handle
// has its own, and handle 0's IS the module's.
// Inline, so a build whose DSP reads no MIDI has no unused static to warn on.
static inline void sonore_decode_midi(const float* inlet, int& pending, int n) {
  sonore_midi_events.clear();
  const int count = (pending < SONORE_MAX_EVENTS) ? pending : SONORE_MAX_EVENTS;
  for (int e = 0; e < count; ++e) {
    const float* ev = inlet + e * 4;
    int ofs = (int) ev[0];
    if (ofs < 0) ofs = 0;
    if (ofs >= n) ofs = (n > 0) ? n - 1 : 0;
    const int status = (int) ev[1] & 0xff;
    // Only 3-byte channel messages come through the inlet, never a malformed
    // 1-byte message handed to the 3-byte constructor.
    if (status < 0x80 || status >= 0xf0) continue;
    sonore_midi_events.addEvent(
        sonore::MidiMessage(status, (int) ev[2] & 0x7f, (int) ev[3] & 0x7f), ofs);
  }
  pending = 0; // consumed
}

/** The module's own key buffer, or null for a build that reads none. A
 *  template, so the buffer is emitted only where a signature reads it. */
template <typename T>
static float* sonore_module_key() {
  if constexpr (SonoreTakesSidechain<T>::value || SonoreTakesContext<T>::value)
    return SonoreKeyStore<true>::buf;
  else
    return nullptr;
}

/** One block of one DSP, from the MIDI inlet and key buffer it is handed.
 *  They used to be the module's, read for every handle -- so a pooled handle
 *  had its events and key copied over handle 0's, which live there, and
 *  rendering handle 1 before handle 0 lost handle 0's notes or played 1's. */
template <typename T>
static void sonore_run_dsp(T& dsp, sonore::AudioBlock<float>& block, const float* params, int n,
                           const float* midiInlet, int& midiPending, float* key) {
  // Each signature reads some of these and not others.
  (void) midiInlet;
  (void) key;
  // Inside the template on purpose: `if constexpr` in a plain function still
  // type-checks the dead branch, and a DSP with no setTransport would fail
  // to compile on a call it never makes.
  if constexpr (SonoreWantsTransport<T>::value) dsp.setTransport(sonore_transport);
  if constexpr (SonoreTakesContext<T>::value) {
    // The rich signature (aux buses, sidechain, MPE). The preview routes none of
    // those, so main + MIDI are filled and everything else is empty/default: no
    // aux (numAux 0), a zero-width sidechain, an empty MIDI-out, no expression.
    sonore_decode_midi(midiInlet, midiPending, n);
    sonore_midi_out_events.clear();
    // THE KEY, from the host's own inlet. It was a zero-width block with a
    // comment saying the preview routes none of those, and that stopped being
    // true the moment a host had a track to route: what a studio could not
    // express, a DAW can.
    float* keyChans[SONORE_CHANNELS];
    for (int c = 0; c < SONORE_CHANNELS; ++c) keyChans[c] = key + c * SONORE_MAX_FRAMES;
    sonore::AudioBlock<float> sidechain(keyChans, (size_t) SONORE_CHANNELS, (size_t) n);
    sonore::ProcessContext ctx{block, nullptr, 0, sidechain,
                               sonore_midi_events, sonore_midi_out_events};
    dsp.process(ctx, params);
  } else if constexpr (SonoreTakesMidi<T>::value) {
    sonore_decode_midi(midiInlet, midiPending, n);
    dsp.process(block, params, sonore_midi_events);
  } else if constexpr (SonoreTakesSidechain<T>::value) {
    midiPending = 0;
    // The host's key, of the block's own length. The same non-const block type
    // the native wrappers pass (read-only by contract, not by type), so a DSP
    // that compiles as a plugin compiles here. It used to be a buffer that was
    // silent by construction, which made every sidechain plugin a plugin that
    // could not be tested through this ABI -- the host writes it now, and a
    // host that writes nothing still gets the silence it used to.
    float* keyChans[SONORE_CHANNELS];
    for (int c = 0; c < SONORE_CHANNELS; ++c) keyChans[c] = key + c * SONORE_MAX_FRAMES;
    sonore::AudioBlock<float> keyBlock(keyChans, (size_t) SONORE_CHANNELS, (size_t) n);
    dsp.process(block, keyBlock, params);
  } else {
    midiPending = 0; // effects ignore the inlet, but never let it go stale
    dsp.process(block, params);
  }
}

extern "C" {

void sonore_init(float sampleRate) {
  sonore::ProcessSpec spec;
  spec.sampleRate = (double) sampleRate;
  spec.maximumBlockSize = (uint32_t) SONORE_MAX_FRAMES;
  spec.numChannels = (uint32_t) SONORE_CHANNELS;
  sonore_midi_n = 0;
  sonore_dsp_instance.prepare(spec);
}

void sonore_process(int n) {
  if (n < 0) n = 0;
  if (n > SONORE_MAX_FRAMES) n = SONORE_MAX_FRAMES;
  // Process in place over the OUT buffer (host wrote IN; we copy then filter).
  for (int c = 0; c < SONORE_CHANNELS; ++c)
    for (int i = 0; i < n; ++i)
      sonore_out_buf[c * SONORE_MAX_FRAMES + i] = sonore_in_buf[c * SONORE_MAX_FRAMES + i];

  float* chans[SONORE_CHANNELS];
  for (int c = 0; c < SONORE_CHANNELS; ++c) chans[c] = sonore_out_buf + c * SONORE_MAX_FRAMES;

  sonore::AudioBlock<float> block(chans, (size_t) SONORE_CHANNELS, (size_t) n);
  sonore_run_dsp(sonore_dsp_instance, block, sonore_param_buf, n, sonore_midi_buf, sonore_midi_n,
                 sonore_module_key<SonoreDsp>());
  sonore_guard_block(sonore_guard, chans, n);
}

/** Blocks the output guard has played as silence since the module started:
 *  a host that sees this move says once, in its own console, that the DSP
 *  made NaN or infinity -- the report every plugin format makes. */
int sonore_output_guard_trips() {
  return (int) sonore_guard.trips.load(std::memory_order_relaxed);
}

float* sonore_in_ptr() { return sonore_in_buf; }
float* sonore_out_ptr() { return sonore_out_buf; }
float* sonore_params_ptr() { return sonore_param_buf; }
int sonore_num_params() { return SONORE_NUM_PARAMS; }
int sonore_max_frames() { return SONORE_MAX_FRAMES; }
int sonore_channels() { return SONORE_CHANNELS; }
float* sonore_midi_ptr() { return sonore_midi_buf; }
/** Where the host writes the KEY for this block: two channels of
 *  SONORE_MAX_FRAMES, the same planar layout as the in and out buffers.
 *
 *  Null when this build has no signature that reads one, which is the answer a
 *  host needs rather than a buffer it would fill for nothing. Its contents are
 *  whatever was written last -- the ABI never clears it, because a host that
 *  routes a key writes it every block and a host that does not should not pay
 *  a memset per block for a buffer nobody reads. */
float* sonore_sidechain_ptr() {
  if constexpr (SonoreTakesSidechain<SonoreDsp>::value ||
                SonoreTakesContext<SonoreDsp>::value)
    return SonoreKeyStore<true>::buf;
  else
    return nullptr;
}
/** 1 = this build reads a key. Declared separately from the pointer because a
 *  host asking "should I offer this plugin a sidechain control" is asking a
 *  question about the PLUGIN, and answering it by testing a pointer for null
 *  works until somebody makes the pointer always valid. */
int sonore_wants_sidechain() {
  return (SonoreTakesSidechain<SonoreDsp>::value ||
          SonoreTakesContext<SonoreDsp>::value) ? 1 : 0;
}
void sonore_midi_count(int n) {
  sonore_midi_n = (n < 0) ? 0 : (n > SONORE_MAX_EVENTS ? SONORE_MAX_EVENTS : n);
}
/** The studio's timeline, once per block, ahead of sonore_process. Beats are
 *  quarter notes from the start; a host that is not playing still reports a
 *  tempo, which is what a synced delay needs to size itself while stopped. */
void sonore_set_transport(double bpm, double positionBeats, int playing) {
  sonore::TransportInfo& t = sonore_transport;
  t.hasTempo = bpm > 1.0 && bpm < 1000.0;
  t.tempo = t.hasTempo ? bpm : 120.0;
  t.isPlaying = playing != 0;
  t.hasBeats = positionBeats >= 0.0;
  t.positionBeats = t.hasBeats ? positionBeats : 0.0;
  t.hasSeconds = t.hasBeats && t.hasTempo;
  t.positionSeconds = t.hasSeconds ? positionBeats * 60.0 / t.tempo : 0.0;
  t.timeSigNumerator = sonore_sig_num;
  t.timeSigDenominator = sonore_sig_den;
  // THE RULE LIVES ON TransportInfo, beside barPhase() which already needed it
  // -- a bar in QUARTER NOTES, which is what positionBeats counts. A second
  // copy here is how the two would come to disagree.
  const double perBar = t.beatsPerBar();
  const double bar = t.hasBeats && perBar > 0.0 ? positionBeats / perBar : 0.0;
  t.barNumber = (int32_t) bar;
  t.barStartBeats = (double) t.barNumber * perBar;
}

/** The bar this song is counted in.
 *
 *  Separate from `sonore_set_transport` rather than two more arguments to it,
 *  because that entry point is in every plugin ever built against this SDK and
 *  in cached wasm nobody is going to rebuild: changing its signature would
 *  break each of them at the call, and the host cannot tell which it has. A
 *  host that never calls this gets four four, which is what it got before.
 *
 *  The ABI used to answer 4/4 whatever the song was, so a plugin that synced to
 *  a BAR was a third out in three four and twice out in six eight -- not
 *  missing, wrong, and silently. */
void sonore_set_time_sig(int numerator, int denominator) {
  if (numerator > 0 && numerator < 64) sonore_sig_num = numerator;
  // A power of two, because that is what a note value IS -- and a denominator
  // of, say, 5 has no note to name it.
  if (denominator > 0 && denominator <= 64 && (denominator & (denominator - 1)) == 0)
    sonore_sig_den = denominator;
}

/** How many samples this instance delays its output by, right now.
 *
 *  A LOOK-AHEAD LIMITER DELAYS THE MIX, and a host that does not know it has a
 *  track sliding against the others by exactly that much. The native wrappers
 *  have reported this since `Limiter` and `Convolver` landed; the browser ABI
 *  never had anywhere to put it, so the DAW read a number out of a manifest
 *  instead -- which is wrong twice over. It is fixed at BUILD time, so a
 *  5 ms look-ahead is 240 samples at 48 kHz and 480 at 96 and the manifest can
 *  only say one of them; and it cannot move, so a plugin whose look-ahead is a
 *  CONTROL reports the latency it had when somebody else set that control.
 *
 *  Asked AFTER `sonore_init` and after any parameter change, because both are
 *  moments the answer can change: latency in SAMPLES depends on the rate, and a
 *  look-ahead in milliseconds depends on the knob.
 *
 *  Zero for a DSP that does not declare `latencySamples()`, which is the honest
 *  answer rather than a guess -- and what every effect that does not delay
 *  reports anyway. */
int sonore_latency() { return sonore_latency_of(sonore_dsp_instance); }

/** 1 = this build consumes MIDI (an instrument/midi-aware effect). A context
 *  DSP reads MIDI through ctx.midi, so it counts too -- without a descriptor
 *  here the preview cannot tell a context instrument from a context effect, and
 *  feeding an unwanted note to an effect that ignores it is harmless, whereas
 *  hiding the keyboard from a synth is not. */
int sonore_wants_midi() {
#if defined(SONORE_WANTS_MIDI)
  // The compiler that appended this glue knew the answer -- the studio's spec
  // says whether a plugin is an instrument -- and said so.
  return SONORE_WANTS_MIDI ? 1 : 0;
#else
  return (SonoreTakesMidi<SonoreDsp>::value || SonoreTakesContext<SonoreDsp>::value) ? 1 : 0;
#endif
}

} // extern "C"

// ── many instances in one module ───────────────────────────────────────────
//
// WHY THIS EXISTS, measured rather than assumed. A browser gives a tab about
// 125 WebAssembly memories and no more: process-wide, indifferent to how big
// they are (a 512 KB build hits the same wall as a 2 MB one), and a Worker
// does not get a budget of its own -- the first one to ask takes the whole
// thing. So a host that instantiates a module per plugin INSTANCE runs out at
// 125 instances no matter how little each one costs, and it runs out with the
// CPU almost idle: the wall arrives at 2.3% of a core with a light plugin and
// 48% with a heavy one, which is a wall that does not move when the work does.
//
// One module holding N DSPs is one reservation. The ceiling becomes 125 plugin
// TYPES, which nobody reaches, instead of 125 instances, which a serious
// session does.
//
// DEFAULT OFF, AND BYTE-IDENTICAL WHEN IT IS. Every plugin this SDK has ever
// built compiles against this header, including the ones in the marketplace,
// so the whole of this is inside an `#if` that is false unless a host asks for
// it -- and the build proves it: a plugin compiled before and after this
// change, with the default, hashes the same.
//
// HANDLE 0 IS THE EXISTING SINGLETON. That looks like a special case and it is
// the reason the rest is provably additive: `sonore_init`/`sonore_process` and
// the buffers they use are untouched, and a host that knows nothing about
// handles keeps working exactly as it did.
#if defined(SONORE_MAX_INSTANCES) && SONORE_MAX_INSTANCES > 1

#define SONORE_EXTRA (SONORE_MAX_INSTANCES - 1)

// Braced for the same reason as sonore_dsp_instance above: the multi-instance
// array must be initialised the way the wrapper initialises its own member, or
// this path reopens the hole the single instance just closed.
static SonoreDsp sonore_dsp_extra[SONORE_EXTRA]{};
static float sonore_in_extra[SONORE_EXTRA][SONORE_MAX_FRAMES * SONORE_CHANNELS];
static float sonore_out_extra[SONORE_EXTRA][SONORE_MAX_FRAMES * SONORE_CHANNELS];
static float sonore_param_extra[SONORE_EXTRA][(SONORE_NUM_PARAMS > 0) ? SONORE_NUM_PARAMS : 1];
static float sonore_midi_extra[SONORE_EXTRA][SONORE_MAX_EVENTS * 4];
static int sonore_midi_n_extra[SONORE_EXTRA];
static bool sonore_slot_used[SONORE_MAX_INSTANCES];
static sonore::detail::OutputGuard sonore_guard_extra[SONORE_EXTRA];

static inline bool sonore_handle_ok(int h) {
  return h >= 0 && h < SONORE_MAX_INSTANCES;
}
static inline SonoreDsp& sonore_dsp_at(int h) {
  return h == 0 ? sonore_dsp_instance : sonore_dsp_extra[h - 1];
}
static inline float* sonore_in_at(int h) {
  return h == 0 ? sonore_in_buf : sonore_in_extra[h - 1];
}
static inline float* sonore_out_at(int h) {
  return h == 0 ? sonore_out_buf : sonore_out_extra[h - 1];
}
static inline float* sonore_params_at(int h) {
  return h == 0 ? sonore_param_buf : sonore_param_extra[h - 1];
}
static inline float* sonore_midi_at(int h) {
  return h == 0 ? sonore_midi_buf : sonore_midi_extra[h - 1];
}
static inline sonore::detail::OutputGuard& sonore_guard_at(int h) {
  return h == 0 ? sonore_guard : sonore_guard_extra[h - 1];
}

/** A key per handle, on a template for the same reason the module's is: a
 *  pooled build with no sidechain signature must carry none of this. Eight
 *  slots of two channels is 128 KB, which is not nothing in a module that
 *  exists to be cheap. */
template <bool Want>
struct SonoreKeyPool {
  static float buf[SONORE_EXTRA][SONORE_MAX_FRAMES * SONORE_CHANNELS];
};
template <bool Want>
float SonoreKeyPool<Want>::buf[SONORE_EXTRA][SONORE_MAX_FRAMES * SONORE_CHANNELS];

static constexpr bool sonore_pool_wants_key =
    SonoreTakesSidechain<SonoreDsp>::value || SonoreTakesContext<SonoreDsp>::value;

static inline float* sonore_key_at(int h) {
  if constexpr (sonore_pool_wants_key)
    return h == 0 ? SonoreKeyStore<true>::buf : SonoreKeyPool<true>::buf[h - 1];
  else
    return nullptr;
}

extern "C" {

/** Claim a slot. -1 when the module is full, which the host must handle:
 *  running out of slots in a pooled module is the same event as running out of
 *  memories was, and it is the one this design exists to make rare. */
int sonore_new() {
  for (int h = 0; h < SONORE_MAX_INSTANCES; ++h) {
    if (sonore_slot_used[h]) continue;
    sonore_slot_used[h] = true;
    // A slot handed out again is a new instance: the guard's count starts
    // over, or one plugin's broken filter is reported against the next.
    sonore_guard_at(h).trips.store(0, std::memory_order_relaxed);
    sonore_guard_at(h).reported.store(false, std::memory_order_relaxed);
    return h;
  }
  return -1;
}

/** Give a slot back. The DSP is left as it is -- `sonore_init_at` is what
 *  prepares one, and a host that reuses a slot without preparing it is a host
 *  that would have heard the last plugin's reverb tail either way. */
void sonore_delete(int h) {
  if (sonore_handle_ok(h)) sonore_slot_used[h] = false;
}

int sonore_max_instances() { return SONORE_MAX_INSTANCES; }

void sonore_init_at(int h, float sampleRate) {
  if (!sonore_handle_ok(h)) return;
  sonore::ProcessSpec spec;
  spec.sampleRate = (double) sampleRate;
  spec.maximumBlockSize = (uint32_t) SONORE_MAX_FRAMES;
  spec.numChannels = (uint32_t) SONORE_CHANNELS;
  if (h == 0) sonore_midi_n = 0;
  else sonore_midi_n_extra[h - 1] = 0;
  sonore_dsp_at(h).prepare(spec);
}

void sonore_process_at(int h, int n) {
  if (!sonore_handle_ok(h)) return;
  if (n < 0) n = 0;
  if (n > SONORE_MAX_FRAMES) n = SONORE_MAX_FRAMES;
  float* in = sonore_in_at(h);
  float* out = sonore_out_at(h);
  for (int c = 0; c < SONORE_CHANNELS; ++c)
    for (int i = 0; i < n; ++i)
      out[c * SONORE_MAX_FRAMES + i] = in[c * SONORE_MAX_FRAMES + i];

  // Each handle's own MIDI inlet and key, handed to the dispatcher. Handle 0's
  // are the module's; copying another handle's over them to reach the
  // dispatcher clobbered whatever the host had queued for handle 0.
  float* chans[SONORE_CHANNELS];
  for (int c = 0; c < SONORE_CHANNELS; ++c) chans[c] = out + c * SONORE_MAX_FRAMES;
  sonore::AudioBlock<float> block(chans, (size_t) SONORE_CHANNELS, (size_t) n);
  int& pending = h == 0 ? sonore_midi_n : sonore_midi_n_extra[h - 1];
  sonore_run_dsp(sonore_dsp_at(h), block, sonore_params_at(h), n, sonore_midi_at(h), pending,
                 sonore_key_at(h));
  sonore_guard_block(sonore_guard_at(h), chans, n);
}

/** The output guard's count for one pooled instance; -1 for a handle that is
 *  not one. */
int sonore_output_guard_trips_at(int h) {
  if (!sonore_handle_ok(h)) return -1;
  return (int) sonore_guard_at(h).trips.load(std::memory_order_relaxed);
}

float* sonore_in_ptr_at(int h) { return sonore_handle_ok(h) ? sonore_in_at(h) : nullptr; }
float* sonore_out_ptr_at(int h) { return sonore_handle_ok(h) ? sonore_out_at(h) : nullptr; }
float* sonore_params_ptr_at(int h) { return sonore_handle_ok(h) ? sonore_params_at(h) : nullptr; }
float* sonore_midi_ptr_at(int h) { return sonore_handle_ok(h) ? sonore_midi_at(h) : nullptr; }
float* sonore_sidechain_ptr_at(int h) { return sonore_handle_ok(h) ? sonore_key_at(h) : nullptr; }

/** The same, per pooled instance: eight delays in one module are eight
 *  different numbers the moment one of them has its look-ahead turned. */
int sonore_latency_at(int h) {
  if (!sonore_handle_ok(h)) return 0;
  return sonore_latency_of(sonore_dsp_at(h));
}

void sonore_midi_count_at(int h, int n) {
  if (!sonore_handle_ok(h)) return;
  const int m = (n < 0) ? 0 : (n > SONORE_MAX_EVENTS ? SONORE_MAX_EVENTS : n);
  if (h == 0) sonore_midi_n = m;
  else sonore_midi_n_extra[h - 1] = m;
}

} // extern "C"

#endif // SONORE_MAX_INSTANCES > 1
