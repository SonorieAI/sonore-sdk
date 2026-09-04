// SPDX-License-Identifier: Apache-2.0
// Sonore SDK, the WebAssembly ABI glue (GENERATED, never authored against).
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

#define SONORE_MAX_FRAMES 128
#define SONORE_CHANNELS   2
#define SONORE_MAX_EVENTS 64

static float sonore_in_buf[SONORE_MAX_FRAMES * SONORE_CHANNELS];
static float sonore_out_buf[SONORE_MAX_FRAMES * SONORE_CHANNELS];
static float sonore_param_buf[(SONORE_NUM_PARAMS > 0) ? SONORE_NUM_PARAMS : 1];
static float sonore_midi_buf[SONORE_MAX_EVENTS * 4];
static int sonore_midi_n = 0;
static SonoreDsp sonore_dsp_instance;
static sonore::MidiBuffer sonore_midi_events;

// Which process() signature does SonoreDsp have? The SAME three the native
// wrappers detect, so the preview runs every plugin shape the shipped binary
// does: not just the two simplest. The dispatch lives in a TEMPLATE so
// `if constexpr` genuinely discards the untaken branch; in a plain function
// both branches would still type-check, which is exactly the mismatch this
// detection exists to avoid.
//
//   effect:      process(AudioBlock&, const float*)
//   instrument:  process(AudioBlock&, const float*, MidiBuffer&)
//   rich:        process(ProcessContext&, const float*)   -- aux/sidechain/MPE
template <typename T, typename = void>
struct SonoreTakesMidi : std::false_type {};
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
template <typename T, typename = void>
struct SonoreTakesSidechain : std::false_type {};
template <typename T>
struct SonoreTakesSidechain<T, std::void_t<decltype(std::declval<T&>().process(
                                   std::declval<sonore::AudioBlock<float>&>(),
                                   std::declval<sonore::AudioBlock<float>&>(),
                                   static_cast<const float*>(nullptr)))>> : std::true_type {};

template <typename T, typename = void>
struct SonoreTakesContext : std::false_type {};
template <typename T>
struct SonoreTakesContext<T, std::void_t<decltype(std::declval<T&>().process(
                                 std::declval<sonore::ProcessContext&>(),
                                 static_cast<const float*>(nullptr)))>> : std::true_type {};

// MIDI the DSP emits (context path); the preview has no MIDI-out sink, so it is
// cleared each block and discarded. Kept in static storage so the reference the
// context holds stays valid for the whole call.
static sonore::MidiBuffer sonore_midi_out_events;

// Turn the host's inlet into sonore_midi_events. Shared by the instrument and
// the context paths; consuming it (count -> 0) means a stale block can't replay.
static void sonore_decode_midi(int n) {
  sonore_midi_events.clear();
  const int count = (sonore_midi_n < SONORE_MAX_EVENTS) ? sonore_midi_n : SONORE_MAX_EVENTS;
  for (int e = 0; e < count; ++e) {
    const float* ev = sonore_midi_buf + e * 4;
    int ofs = (int) ev[0];
    if (ofs < 0) ofs = 0;
    if (ofs >= n) ofs = (n > 0) ? n - 1 : 0;
    const int status = (int) ev[1] & 0xff;
    // Only 3-byte channel messages come through the inlet: never a malformed
    // 1-byte message handed to the 3-byte constructor.
    if (status < 0x80 || status >= 0xf0) continue;
    sonore_midi_events.addEvent(
        sonore::MidiMessage(status, (int) ev[2] & 0x7f, (int) ev[3] & 0x7f), ofs);
  }
  sonore_midi_n = 0; // consumed
}

template <typename T>
static void sonore_run_dsp(T& dsp, sonore::AudioBlock<float>& block, const float* params, int n) {
  if constexpr (SonoreTakesContext<T>::value) {
    // The rich signature (aux buses, sidechain, MPE). The preview routes none of
    // those, so main + MIDI are filled and everything else is empty/default: no
    // aux (numAux 0), a zero-width sidechain, an empty MIDI-out, no expression.
    sonore_decode_midi(n);
    sonore_midi_out_events.clear();
    sonore::AudioBlock<float> sidechain; // empty: no key input in the preview
    sonore::ProcessContext ctx{block, nullptr, 0, sidechain,
                               sonore_midi_events, sonore_midi_out_events};
    dsp.process(ctx, params);
  } else if constexpr (SonoreTakesMidi<T>::value) {
    sonore_decode_midi(n);
    dsp.process(block, params, sonore_midi_events);
  } else if constexpr (SonoreTakesSidechain<T>::value) {
    sonore_midi_n = 0;
    // A silent key of the block's own length. The same non-const block type the
    // native wrappers pass (read-only by contract, not by type), so a DSP that
    // compiles as a plugin compiles here.
    static float sonore_key_buf[SONORE_MAX_FRAMES * SONORE_CHANNELS] = {};
    float* keyChans[SONORE_CHANNELS];
    for (int c = 0; c < SONORE_CHANNELS; ++c) keyChans[c] = sonore_key_buf + c * SONORE_MAX_FRAMES;
    sonore::AudioBlock<float> key(keyChans, (size_t) SONORE_CHANNELS, (size_t) n);
    dsp.process(block, key, params);
  } else {
    sonore_midi_n = 0; // effects ignore the inlet, but never let it go stale
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
  sonore_run_dsp(sonore_dsp_instance, block, sonore_param_buf, n);
}

float* sonore_in_ptr() { return sonore_in_buf; }
float* sonore_out_ptr() { return sonore_out_buf; }
float* sonore_params_ptr() { return sonore_param_buf; }
int sonore_num_params() { return SONORE_NUM_PARAMS; }
int sonore_max_frames() { return SONORE_MAX_FRAMES; }
int sonore_channels() { return SONORE_CHANNELS; }
float* sonore_midi_ptr() { return sonore_midi_buf; }
void sonore_midi_count(int n) {
  sonore_midi_n = (n < 0) ? 0 : (n > SONORE_MAX_EVENTS ? SONORE_MAX_EVENTS : n);
}
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
