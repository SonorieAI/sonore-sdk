// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: a pooled wasm module keeps each instance's MIDI and key apart.
//
// With SONORE_MAX_INSTANCES > 1 one module holds several DSPs, and handle 0
// is the module's original instance: its MIDI inlet and key buffer ARE the
// module's, which is what keeps a host that never learned about handles
// working. The dispatcher read those module buffers for every handle, so
// rendering handle 1 copied its events and key over handle 0's -- or, with
// nothing queued for handle 1, zeroed handle 0's event count. A host that
// queued MIDI for both and rendered 1 before 0 lost 0's notes, or played 1's.
//
// The ABI is plain C++ until it is compiled for wasm, so this drives it
// natively: a context DSP that records what each block brought it.
//
// And each instance has its own output guard: a block with a NaN in it leaves
// a pooled handle as silence, as it leaves every plugin format, counted for
// that handle alone and started over when the slot is handed out again.

#include <cstdio>
#include <limits>

#include <sonore/audio.h>

#define SONORE_NUM_PARAMS 1
#define SONORE_MAX_INSTANCES 2

struct SonoreDsp {
  int events = -1, note = -1;
  float key = -1.0f;
  bool poison = false; // the next block's first sample is a NaN
  void prepare(const sonore::ProcessSpec&) {}
  void process(sonore::ProcessContext& ctx, const float*) {
    events = 0;
    for (const auto& e : ctx.midi) {
      ++events;
      note = e.message.getNoteNumber();
    }
    key = ctx.sidechain.getNumChannels() > 0 && ctx.sidechain.getNumSamples() > 0
              ? ctx.sidechain.getChannelPointer(0)[0]
              : -1.0f;
    if (poison && ctx.main.getNumSamples() > 0)
      ctx.main.getChannelPointer(0)[0] = std::numeric_limits<float>::quiet_NaN();
  }
};

#include <sonore/wasm_abi.h>

static int g_checks = 0;
static int g_failures = 0;

static void check(bool ok, const char* what) {
  ++g_checks;
  if (!ok) ++g_failures;
  std::printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
}

static void queueNote(int h, int note) {
  float* m = sonore_midi_ptr_at(h);
  m[0] = 0.0f;
  m[1] = (float) 0x90;
  m[2] = (float) note;
  m[3] = 100.0f;
  sonore_midi_count_at(h, 1);
}

static void writeKey(int h, float value) {
  float* key = sonore_sidechain_ptr_at(h);
  for (int i = 0; i < SONORE_MAX_FRAMES * SONORE_CHANNELS; ++i) key[i] = value;
}

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::printf("── wasm ABI, two instances in one module ──\n");
  const int a = sonore_new(), b = sonore_new();
  check(a == 0 && b == 1, "two slots, the first being the module's own instance");
  sonore_init_at(a, 48000.0f);
  sonore_init_at(b, 48000.0f);
  const SonoreDsp& dspA = sonore_dsp_instance;
  const SonoreDsp& dspB = sonore_dsp_extra[0];

  char line[200];
  queueNote(a, 60);
  queueNote(b, 72);
  writeKey(a, 0.25f);
  writeKey(b, 0.75f);
  sonore_process_at(b, 128);
  sonore_process_at(a, 128);
  std::snprintf(line, sizeof(line), "rendered second, handle 0 hears its own note (%d event(s), "
                "note %d)", dspA.events, dspA.note);
  check(dspA.events == 1 && dspA.note == 60, line);
  std::snprintf(line, sizeof(line), "...and its own key (%.2f), not the one rendered before it",
                dspA.key);
  check(dspA.key == 0.25f, line);
  std::snprintf(line, sizeof(line), "handle 1 heard its own note and key (note %d, key %.2f)",
                dspB.note, dspB.key);
  check(dspB.events == 1 && dspB.note == 72 && dspB.key == 0.75f, line);

  queueNote(a, 61);
  sonore_process_at(b, 128);
  sonore_process_at(a, 128);
  std::snprintf(line, sizeof(line), "with nothing queued for handle 1, rendering it leaves handle "
                "0's note alone (%d event(s), note %d)", dspA.events, dspA.note);
  check(dspA.events == 1 && dspA.note == 61, line);
  check(dspB.events == 0, "...and handle 1 hears nothing");

  // The module's original entry points are handle 0's, as before.
  queueNote(a, 62);
  sonore_process(128);
  check(dspA.events == 1 && dspA.note == 62, "the original sonore_process still drives handle 0");
  sonore_process(128);
  check(dspA.events == 0, "...and consumes its inlet, so a block is never replayed");

  // ── The output guard, per instance ──
  {
    auto fill = [](int h, float value) {
      float* in = sonore_in_ptr_at(h);
      for (int i = 0; i < SONORE_MAX_FRAMES * SONORE_CHANNELS; ++i) in[i] = value;
    };
    auto silent = [](int h, int frames = SONORE_MAX_FRAMES) {
      const float* out = sonore_out_ptr_at(h);
      for (int c = 0; c < SONORE_CHANNELS; ++c)
        for (int i = 0; i < frames; ++i)
          if (out[c * SONORE_MAX_FRAMES + i] != 0.0f) return false;
      return true;
    };
    SonoreDsp& poisonB = sonore_dsp_extra[0];
    fill(a, 0.5f);
    fill(b, 0.5f);
    poisonB.poison = true;
    sonore_process_at(b, 128);
    sonore_process_at(a, 128);
    std::snprintf(line, sizeof(line), "a NaN in handle 1's block leaves it as silence, counted for "
                  "handle 1 alone (%d, %d)", sonore_output_guard_trips_at(b),
                  sonore_output_guard_trips_at(a));
    check(silent(b) && sonore_output_guard_trips_at(b) == 1 && sonore_output_guard_trips_at(a) == 0,
          line);
    check(sonore_out_ptr_at(a)[0] == 0.5f && sonore_out_ptr_at(a)[SONORE_MAX_FRAMES] == 0.5f,
          "...and handle 0's clean block passes untouched");
    poisonB.poison = false;
    sonore_process_at(b, 128);
    check(sonore_out_ptr_at(b)[0] == 0.5f, "...and handle 1's next clean block plays as normal");

    SonoreDsp& poisonA = sonore_dsp_instance;
    poisonA.poison = true;
    sonore_process(64);
    check(silent(a, 64), "the module's own sonore_process is guarded, both channels of its block");
    check(sonore_output_guard_trips() == 1 && sonore_output_guard_trips_at(a) == 1,
          "...and sonore_output_guard_trips() is handle 0's count");
    poisonA.poison = false;

    sonore_delete(b);
    const int again = sonore_new();
    check(again == b && sonore_output_guard_trips_at(again) == 0,
          "a slot handed out again starts its count over");
    check(sonore_output_guard_trips_at(7) == -1, "a handle that is not one reads -1");
  }

  std::printf("\n%d checks, %d failure(s)\n", g_checks, g_failures);
  if (g_failures == 0) std::printf("SONORE WASM POOL PASSED\n");
  return g_failures == 0 ? 0 : 1;
}
