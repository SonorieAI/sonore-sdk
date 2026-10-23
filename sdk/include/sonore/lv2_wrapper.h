// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: the LV2 format wrapper.
//
// LV2 is the open standard (ISC-licensed headers, vendored) that Linux DAWs
// (Ardour, Reaper on Linux, Carla, Zrythm) treat as native. The plugin is a
// shared library plus a bundle of Turtle (.ttl) metadata describing every port;
// hosts read the TTL to know the plugin's shape BEFORE loading any code.
//
// Include it AFTER clap_wrapper.h, like the other formats: the CLAP wrapper
// owns the shared machinery (Instance, parameters, the UI queue) and this file
// adapts it, so one source builds .clap, .vst3 and .lv2 that cannot drift.
//
// The TTL is GENERATED, never hand-written: compiling the same source with
// SONORE_LV2_TTLGEN yields a tiny program that emits manifest.ttl and the
// plugin's .ttl from the descriptor. Metadata written by hand drifts from the
// code the first time anyone adds a parameter: generating both from one table
// is the whole reason the descriptor exists.
//
// Port layout (index order is the contract, and the TTL states it):
//   effect:      [0..P-1] control in | [P] audio in L | [P+1] in R
//                | [P+2] out L | [P+3] out R
//   instrument:  [0..P-1] control in | [P] atom MIDI in | [P+1] out L | [P+2] out R
//
// LV2's control ports are one float* each, connected by the host and read per
// run(): parameters land at block granularity, which the toolkit's smoothers
// exist to make inaudible (same story as every other format here).
#pragma once

#include <lv2/atom/atom.h>
#include <lv2/core/lv2.h>
#include <lv2/midi/midi.h>
#include <lv2/state/state.h>
#include <lv2/urid/urid.h>

#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <clocale>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>

#include "audio.h"
#include "gui.h"
#include "plugin.h"
#include "lv2_atom.h"
#include "number_text.h"
#include "lv2_ui.h"
#include "presets.h"
#include "transport.h"

#ifndef SONORE_NUM_PARAMS
#error "Define SONORE_NUM_PARAMS (and struct SonoreDsp) before including lv2_wrapper.h"
#endif

namespace sonore {
namespace lv2 {

// ── The worker extension ─────────────────────────────────────
//
// Declared here rather than vendored, the same way lv2_ui.h declares the UI
// ABI: the SDK vendors the headers it uses heavily and spells out the small
// ones, and a file of ours claiming to BE upstream's would be worse than one
// that says it mirrors the spec.
//
// What it is for: LV2's plugin side has no main thread of its own. run() is
// the audio callback, so reading a file there is a dropout -- and the host is
// the only thing that owns a thread to do it on. The worker extension is that
// thread. run() schedules, the host calls work() somewhere else, and the
// answer comes back through work_response() before the next run().
#define SONORE_LV2_WORKER__schedule "http://lv2plug.in/ns/ext/worker#schedule"
#define SONORE_LV2_WORKER__interface "http://lv2plug.in/ns/ext/worker#interface"
// The time extension is URIs only -- time:Position is an atom object whose
// properties a host fills in -- so its header is not vendored for one prefix.
#define SONORE_LV2_TIME "http://lv2plug.in/ns/ext/time#"

typedef void* Lv2WorkerRespondHandle;
typedef void* Lv2WorkerScheduleHandle;
enum Lv2WorkerStatus { kLv2WorkerSuccess = 0, kLv2WorkerErrUnknown = 1, kLv2WorkerErrNoSpace = 2 };
typedef Lv2WorkerStatus (*Lv2WorkerRespondFunction)(Lv2WorkerRespondHandle, uint32_t, const void*);

/** LV2_Worker_Schedule, the host's half: schedule_work() hands a job to the
 *  worker thread. Spelled here for its layout; see above. */
struct Lv2WorkerSchedule {
  Lv2WorkerScheduleHandle handle;
  Lv2WorkerStatus (*schedule_work)(Lv2WorkerScheduleHandle, uint32_t, const void*);
};

/** LV2_Worker_Interface, the plugin's half: work() runs on the worker thread,
 *  work_response() back on the audio thread before the next run(), and
 *  end_run() after each run(). */
struct Lv2WorkerInterface {
  Lv2WorkerStatus (*work)(LV2_Handle, Lv2WorkerRespondFunction, Lv2WorkerRespondHandle, uint32_t,
                          const void*);
  Lv2WorkerStatus (*work_response)(LV2_Handle, uint32_t, const void*);
  Lv2WorkerStatus (*end_run)(LV2_Handle);
};

// ── The log extension ────────────────────────────────────────
//
// Declared the same way, for one message: the output guard's report, which
// goes to the host's log as a warning where the host offers one. The spec
// allows its printf from any NON-realtime context, so it is called from the
// worker or from deactivate()/cleanup(), never from run().
#define SONORE_LV2_LOG__log "http://lv2plug.in/ns/ext/log#log"
#define SONORE_LV2_LOG__Warning "http://lv2plug.in/ns/ext/log#Warning"

/** LV2_Log_Log: the host's log, for non-realtime contexts only. */
struct Lv2Log {
  void* handle;
  // printf and vprintf upstream; renamed because a C library may define
  // printf as a macro, and only the layout is the ABI.
  int (*print)(void* handle, LV2_URID type, const char* format, ...);
  int (*vprint)(void* handle, LV2_URID type, const char* format, va_list args);
};

/** What run() hands the worker to have the output guard's report made: an
 *  exact byte string, which no file-load payload can equal (a payload ends
 *  with a path after the purpose's terminator, and this ends at its own). */
inline const char* outputGuardWork() { return "sonore:output-guard"; }

using clapwrap::Instance;

// ── Port arithmetic, shared by the wrapper and the TTL generator ─────────────

#include "lv2_ports.h"
#include "path_io.h"

/** The plugin URI: derived from the reverse-DNS id, which is already unique
 *  per product. LV2 identifies plugins by URI exactly as VST3 does by UID. */
inline void pluginUri(char* out, size_t capacity) {
  std::snprintf(out, capacity, "urn:sonorie:%s", kDesc.id);
}

// ── The instance ─────────────────────────────────────────────────────────────

/** One LV2 instance: the Instance every format wrapper shares, plus what LV2
 *  adds -- the host's port pointers, the URIDs it mapped, the worker and the
 *  log it offered, and the MIDI and transport of the current run(). */
struct Plugin {
  Instance shared;
  /** A long run()'s MIDI, held here while it is handed out slice by slice.
   *  A member, so nothing in run() allocates for it. */
  MidiBuffer runMidi;
  double sampleRate = 48000.0;

  /** Host-connected port locations. Controls may be null until connected. */
  const float* controls[SONORE_NUM_PARAMS > 0 ? SONORE_NUM_PARAMS : 1]{};
  /** One per channel of the main bus, which is lv2Width() wide -- up to
   *  kMaxAudioChannels, not two. These were two-wide, so a quad-only effect's
   *  third and fourth channels were read through slots that did not exist. */
  const float* audioIn[clapwrap::kMaxAudioChannels]{};
  float* audioOut[clapwrap::kMaxAudioChannels]{};
  /** lv2:enabled port location, or null when the host never connected it
   *  (it is connectionOptional): null means enabled. */
  const float* enabled = nullptr;
  /** lv2:latency output, written every run(); null when never connected. */
  float* latencyOut = nullptr;
  /** The block's level, for a UI that is a separate module and can only be
   *  reached through ports. */
  float* meterPeakOut = nullptr;
  float* meterRmsOut = nullptr;
  /** The DSP's gain reduction, when it declares one (grPorts()). */
  float* meterGrOut = nullptr;

  /** The interface's two atom ports. */
  const LV2_Atom_Sequence* uiControlIn = nullptr;
  LV2_Atom_Sequence* uiNotifyOut = nullptr;

  /** time:Position objects from the host, when the DSP takes the transport. */
  const LV2_Atom_Sequence* timeIn = nullptr;
  /** Where the host's transport is, as of the start of the next run(): set
   *  from each time:Position, carried forward by every run in between --
   *  a host sends one only when the transport jumps. */
  TransportInfo transport;
  double transportSpeed = 0.0;
  LV2_URID uridAtomObject = 0, uridAtomBlank = 0, uridAtomFloat = 0, uridAtomDouble = 0,
           uridAtomInt = 0, uridAtomLong = 0;
  LV2_URID uridTimePosition = 0, uridTimeBar = 0, uridTimeBarBeat = 0, uridTimeBeat = 0,
           uridTimeBeatUnit = 0, uridTimeBeatsPerBar = 0, uridTimeBeatsPerMinute = 0,
           uridTimeFrame = 0, uridTimeSpeed = 0;
  LV2_URID uridStateRequest = 0;
  LV2_URID uridStateJson = 0;
  LV2_URID uridLoadFile = 0;
  LV2_URID uridNotes = 0;
  /** The keyboard as the interface last saw it. Compared, not recomputed:
   *  four 32-bit words is two 64-bit loads, and sending them every block
   *  would put an eval on the interface's clock for ever. */
  uint64_t notesSentLow = 0, notesSentHigh = 0;
  bool notesSentValid = false;
  /** The host's worker thread, if it offered one. Null is legal and means
   *  this plugin cannot be handed a file -- which is better than loading one
   *  in run() and calling it a feature. */
  const Lv2WorkerSchedule* worker = nullptr;
  /** The host's log, if it offered one, and its warning type: where the
   *  output guard's report goes. Null means stderr. */
  const Lv2Log* log = nullptr;
  LV2_URID uridLogWarning = 0;
  /** LV2 runs the DSP's [main-thread] calls -- loadFile, saveState,
   *  loadState, prepare -- on THREE threads: the host's (activate, save,
   *  restore), the worker's (work), and the audio thread's when a freewheel
   *  flip re-prepares (updateFreeWheel). The DSP was promised one, as CLAP
   *  and VST3 give it, so they take turns here. run() only ever try_locks
   *  it, and puts the flip off to the next run when it is held. */
  std::mutex mainThread;
  /** An interface state the worker built, on its way to run(). work() hands
   *  it over by pointer in its response, so each response carries its own;
   *  one shared string, written by the next work() while the previous
   *  response was still queued, ended on the state before both loads.
   *  work_response() swaps the text in -- two pointers, no allocation -- and
   *  pushes the husk onto uiRetired for the worker to free. */
  struct UiText {
    std::string json;
    /** Which build of the state this is: see uiBuilt. */
    uint32_t build = 0;
    UiText* next = nullptr;
    /** The next in uiTexts. */
    UiText* older = nullptr;
  };
  std::atomic<UiText*> uiRetired{nullptr};
  /** Every UiText work() has made and nothing has freed yet, delivered or
   *  not, under mainThread. A response the host never delivers -- host.h
   *  drops its queue with the instance on every prepare(), jalv at exit --
   *  never reaches uiRetired, and its text was lost with the instance: two
   *  heap blocks per dropped load. cleanup() frees what is left here. */
  UiText* uiTexts = nullptr;
  /** Every interface state is numbered as it is built, under mainThread --
   *  by work(), and by activate() and restore() -- and uiShown is the number
   *  of the one uiStateJson holds. A response is swapped in only when it is
   *  newer: one the worker built BEFORE a restore, still queued when the
   *  host restored between two runs, put the old file back on the interface
   *  while the DSP held the restored one. uiShown is written by restore()
   *  and activate(), which LV2 never runs beside work_response(). */
  uint32_t uiBuilt = 0;
  uint32_t uiShown = 0;
  /**
   * The bag as an object literal, built OFF the audio thread.
   *
   * run() only copies bytes out of it. Building the JSON there would mean a
   * string allocation per block, and the whole reason this port exists is to
   * carry something that cannot be computed cheaply.
   */
  std::string uiStateJson;
  /** Bumped whenever uiStateJson is rebuilt, and compared against what was
   *  last put on the wire. A counter rather than a flag because run() and the
   *  rebuild are not the only two things involved -- a UI can ask again at
   *  any time, and it must get the CURRENT answer either way. */
  uint32_t uiStateSeq = 0;
  uint32_t uiStateSent = 0;
  /** True once a request has arrived and until it has been answered. */
  bool uiStateRequested = false;
  /** Frames run since the state was last put on the notify port. At a second
   *  of audio it is sent again whether or not anything asked: see
   *  writeUiNotify. */
  uint64_t framesSinceUiState = 0;
  /** lv2:freeWheeling input, or null when the host never connected it: null
   *  means real time, which is what a host that does not know about the port
   *  is doing. */
  const float* freeWheel = nullptr;
  /** Sidechain inputs; null when the host routed nothing (they are optional). */
  const float* scIn[2]{};
  /** Silence for an unrouted sidechain, sized at activate. */
  std::vector<float> scSilence;
  /** Aux output port locations, flattened in declaration order. */
  float* auxOut[clapwrap::kMaxAuxOutputs * clapwrap::kMaxAudioChannels]{};
  /** The atom:Sequence the host gave us to write emitted MIDI into. */
  LV2_Atom_Sequence* midiOut = nullptr;
  LV2_URID uridAtomSequence = 0;
  uint32_t midiOutCapacity = 0;
  const LV2_Atom_Sequence* midiIn = nullptr;

  /** URIDs, mapped once at instantiate through the host's urid:map. */
  LV2_URID uridMidiEvent = 0;
  LV2_URID uridStateBlob = 0;
  LV2_URID uridAtomChunk = 0;
};

inline LV2_Handle instantiate(const LV2_Descriptor*, double sampleRate, const char*,
                              const LV2_Feature* const* features) {
  // urid:map is a requiredFeature in our TTL, so a host that reaches this far
  // has promised it. Verify anyway: a null map here would be a crash later.
  LV2_URID_Map* map = nullptr;
  const Lv2WorkerSchedule* worker = nullptr;
  const Lv2Log* log = nullptr;
  for (int i = 0; features && features[i]; ++i) {
    if (std::strcmp(features[i]->URI, LV2_URID__map) == 0)
      map = (LV2_URID_Map*) features[i]->data;
    // OPTIONAL, deliberately. A host without a worker thread can still run
    // every plugin here; it just cannot hand one a file, and refusing to load
    // at all over that would be a worse trade than the missing feature.
    else if (std::strcmp(features[i]->URI, SONORE_LV2_WORKER__schedule) == 0)
      worker = (const Lv2WorkerSchedule*) features[i]->data;
    // Optional too: without it the output guard's report goes to stderr.
    else if (std::strcmp(features[i]->URI, SONORE_LV2_LOG__log) == 0)
      log = (const Lv2Log*) features[i]->data;
  }
  if (!map) return nullptr;

  void* memory = std::malloc(sizeof(Plugin));
  if (!memory) return nullptr;
  Plugin* self = new (memory) Plugin();
  self->worker = worker;
  self->log = log;
  if (log) self->uridLogWarning = map->map(map->handle, SONORE_LV2_LOG__Warning);
  self->sampleRate = sampleRate;
  self->uridMidiEvent = map->map(map->handle, LV2_MIDI__MidiEvent);
  self->uridAtomSequence = map->map(map->handle, LV2_ATOM__Sequence);
  self->uridAtomChunk = map->map(map->handle, LV2_ATOM__Chunk);
  self->uridStateBlob = map->map(map->handle, "urn:sonorie:state:blob");
  self->uridStateRequest = map->map(map->handle, "urn:sonorie:ui:stateRequest");
  self->uridStateJson = map->map(map->handle, "urn:sonorie:ui:stateJson");
  self->uridLoadFile = map->map(map->handle, "urn:sonorie:ui:loadFile");
  self->uridNotes = map->map(map->handle, "urn:sonorie:ui:notes");
  self->uridAtomObject = map->map(map->handle, LV2_ATOM__Object);
  self->uridAtomBlank = map->map(map->handle, LV2_ATOM__Blank);
  self->uridAtomFloat = map->map(map->handle, LV2_ATOM__Float);
  self->uridAtomDouble = map->map(map->handle, LV2_ATOM__Double);
  self->uridAtomInt = map->map(map->handle, LV2_ATOM__Int);
  self->uridAtomLong = map->map(map->handle, LV2_ATOM__Long);
  self->uridTimePosition = map->map(map->handle, SONORE_LV2_TIME "Position");
  self->uridTimeBar = map->map(map->handle, SONORE_LV2_TIME "bar");
  self->uridTimeBarBeat = map->map(map->handle, SONORE_LV2_TIME "barBeat");
  self->uridTimeBeat = map->map(map->handle, SONORE_LV2_TIME "beat");
  self->uridTimeBeatUnit = map->map(map->handle, SONORE_LV2_TIME "beatUnit");
  self->uridTimeBeatsPerBar = map->map(map->handle, SONORE_LV2_TIME "beatsPerBar");
  self->uridTimeBeatsPerMinute = map->map(map->handle, SONORE_LV2_TIME "beatsPerMinute");
  self->uridTimeFrame = map->map(map->handle, SONORE_LV2_TIME "frame");
  self->uridTimeSpeed = map->map(map->handle, SONORE_LV2_TIME "speed");
  for (int i = 0; i < SONORE_NUM_PARAMS && i < kDesc.numParams; ++i)
    self->shared.params[i] = kDesc.params[i].defaultValue;
  return (LV2_Handle) self;
}

/** Re-run prepare() when the host starts or stops freewheeling.
 *
 *  Same reasoning as the CLAP side: a DSP taking the simple process()
 *  signature has no ProcessContext to read the flag from, so re-preparing is
 *  the only way it ever hears about the change. Called from run(), which is
 *  the audio thread -- but only on the block where the flag actually MOVES,
 *  which happens twice per bounce rather than per block, and a host that is
 *  freewheeling is by definition not keeping up with a clock.
 */
inline void updateFreeWheel(Plugin* self) {
  const bool offline = self->freeWheel && *self->freeWheel >= 0.5f;
  if (offline == self->shared.offline) return;
  // prepare() is a [main-thread] call, and the worker's loadFile() and the
  // host's save() -- which LV2 lets run beside run() -- take mainThread
  // before they call the DSP. This did not, and a flip arriving while a file
  // loaded ran prepare() inside loadFile(). Taken here too, but never waited
  // for: a run that finds it held leaves the flag unmoved, and the next run
  // makes the flip.
  std::unique_lock<std::mutex> turn(self->mainThread, std::try_to_lock);
  if (!turn.owns_lock()) return;
  self->shared.offline = offline;
  clapwrap::prepareDsp(&self->shared);
}

inline void connectPort(LV2_Handle handle, uint32_t port, void* data) {
  auto* self = (Plugin*) handle;
  const uint32_t controls = (uint32_t) numControlPorts();
  if (port < controls) {
    self->controls[port] = (const float*) data;
    return;
  }
  if (midiOutPorts() && port == (uint32_t) portMidiOut()) {
    self->midiOut = (LV2_Atom_Sequence*) data;
    return;
  }
  if (latencyPorts() && port == (uint32_t) portLatency()) {
    self->latencyOut = (float*) data;
    return;
  }
  if (port == (uint32_t) portUiControl()) {
    self->uiControlIn = (const LV2_Atom_Sequence*) data;
    return;
  }
  if (port == (uint32_t) portUiNotify()) {
    self->uiNotifyOut = (LV2_Atom_Sequence*) data;
    return;
  }
  if (timePorts() && port == (uint32_t) portTimeIn()) {
    self->timeIn = (const LV2_Atom_Sequence*) data;
    return;
  }
  if (port == (uint32_t) portMeterPeak()) {
    self->meterPeakOut = (float*) data;
    return;
  }
  if (port == (uint32_t) portMeterRms()) {
    self->meterRmsOut = (float*) data;
    return;
  }
  if (grPorts() && port == (uint32_t) portMeterGr()) {
    self->meterGrOut = (float*) data;
    return;
  }
  // Ports BOTH kinds of plugin have come before the instrument branch, which
  // returns. The MIDI-out port was once below it and an instrument's MIDI out
  // was never connected; the aux outputs and the freewheel switch were still
  // below it, so a synth emitting stems left its aux ports pointing nowhere
  // (the host played its own stale buffer) and ignored an offline bounce.
  if (auxOutChannels() > 0 && port >= (uint32_t) portAuxOut0() &&
      port < (uint32_t) (portAuxOut0() + auxOutChannels())) {
    self->auxOut[port - (uint32_t) portAuxOut0()] = (float*) data;
    return;
  }
  if (port == (uint32_t) portFreeWheel()) {
    self->freeWheel = (const float*) data;
    return;
  }
  // The main bus by RANGE, lv2Width() ports each way. It was matched as
  // "the first port" and "the one after it" whatever the width -- and at
  // width 1 the one after the input IS the output, so the output port was
  // taken for a right input and never connected: a mono-only effect's run()
  // returned on its first line and the host heard its own stale buffer.
  const uint32_t width = (uint32_t) lv2Width();
  const uint32_t outFirst = (uint32_t) portAudioOutL();
  if (port >= outFirst && port < outFirst + width) {
    self->audioOut[port - outFirst] = (float*) data;
    return;
  }
  if (midiPorts() && port == (uint32_t) portMidiIn()) {
    self->midiIn = (const LV2_Atom_Sequence*) data;
    return;
  }
  if (isInstrument()) return;
  const uint32_t inFirst = (uint32_t) portAudioInL();
  if (port == (uint32_t) portEnabled()) self->enabled = (const float*) data;
  else if (scPorts() && port == (uint32_t) portScInL()) self->scIn[0] = (const float*) data;
  else if (scPorts() && port == (uint32_t) portScInL() + 1) self->scIn[1] = (const float*) data;
  else if (port >= inFirst && port < inFirst + width) self->audioIn[port - inFirst] = (const float*) data;
}

/**
 * Rebuild the answer an interface gets when it asks what the plugin has.
 *
 * Called from activate() and from state restore -- both [main-thread], both
 * places where allocating a string is fine. run() only ever COPIES this, and
 * that is the whole arrangement: the expensive half happens where there is
 * time for it, and the audio thread does a memcpy.
 */
inline void refreshUiState(Plugin* self) {
  StateBag bag;
  clapwrap::saveDspState(self->shared.dsp, bag);
  self->uiStateJson = bagToJson(bag);
  self->uiShown = ++self->uiBuilt; // newer than any response still queued
  ++self->uiStateSeq;
}

/** [worker-thread or main-thread] Free what work_response() has finished
 *  with. Never on the audio thread, which only pushes. */
inline void freeRetiredUiText(Plugin* self) {
  Plugin::UiText* r = self->uiRetired.exchange(nullptr, std::memory_order_acquire);
  while (r) {
    Plugin::UiText* next = r->next;
    for (Plugin::UiText** link = &self->uiTexts; *link; link = &(*link)->older)
      if (*link == r) {
        *link = r->older;
        break;
      }
    delete r;
    r = next;
  }
}

inline void activate(LV2_Handle handle) {
  auto* self = (Plugin*) handle;
  std::lock_guard<std::mutex> turn(self->mainThread);
  refreshUiState(self);
  // Into the SHARED instance, and prepared through prepareDsp, because that
  // is what updateFreeWheel calls when an export starts -- and it reads these
  // three fields. They were never written here: activate built its own spec
  // and handed it to the DSP directly, so the shared ones kept the Instance
  // defaults, and the first freewheel flip re-prepared every DSP at 48 kHz,
  // 128 frames and two channels whatever the session was. A 44.1 kHz bounce
  // came out 8.8% sharp in every filter and delay, and stayed that way after
  // the bounce, because turning freewheel off re-prepared with the same.
  self->shared.sampleRate = self->sampleRate;
  // LV2 gives no maximum block size up front; 8192 covers every host's real
  // behaviour, and run() slices anything larger.
  self->shared.maxFrames = 8192;
  self->shared.mainChannels = (uint32_t) lv2Width();
  clapwrap::prepareDsp(&self->shared);
  // With room for the latency to move: LV2 has no restart (see follow()).
  const uint32_t latency = clapwrap::dspLatency(self->shared.dsp);
  self->shared.bypass.prepare(self->sampleRate, 8192, latency, (uint32_t) lv2Width(),
                              clapwrap::HasLatency<SonoreDsp>::value
                                  ? clapwrap::BypassState::roomFor(latency, self->sampleRate)
                                  : 0);
  if (scPorts()) self->shared.scSilence.assign(8192, 0.0f);
}

/** Claim the MIDI output buffer for this run(): note the host's capacity and
 *  leave an empty sequence, whatever follows. First thing in run(), because
 *  a host reads atom.size back as how much was written -- and a run() of
 *  zero frames, or one that returned early, claimed it nowhere and left the
 *  capacity there, which reads as a buffer full of events. */
inline void claimMidiOut(Plugin* self) {
  if (!midiOutPorts() || !self->midiOut) return;
  LV2_Atom_Sequence* seq = self->midiOut;
  self->midiOutCapacity = seq->atom.size;
  seq->atom.type = self->uridAtomSequence;
  seq->atom.size = (uint32_t) sizeof(LV2_Atom_Sequence_Body);
  seq->body.unit = 0;
  seq->body.pad = 0;
}

/** Append this slice's emitted MIDI to the host's output sequence.
 *
 *  LV2 hands us a buffer whose atom.size the HOST set to the capacity; the
 *  plugin overwrites it with what it actually wrote. Getting that backwards
 *  is how a host reads gigabytes of garbage, so the capacity is captured
 *  before the first write of the block and every event is bounds-checked. */
inline void writeMidiOut(Plugin* self, uint32_t frameOffset, uint32_t frames) {
  if (!midiOutPorts() || !self->midiOut) return;
  LV2_Atom_Sequence* seq = self->midiOut;
  auto* base = (uint8_t*) &seq->body;
  // One event: the body padded to eight, which is part of what the next
  // event's offset is measured from -- wrong, it does not truncate a message,
  // it misaligns everything after it. False when the buffer is full: drop,
  // never overrun.
  auto put = [&](int position, const uint8_t* data, uint32_t size) {
    const uint32_t need = (uint32_t) sizeof(LV2_Atom_Event) + ((size + 7u) & ~7u);
    if (seq->atom.size + need > self->midiOutCapacity) return false;
    auto* ev = (LV2_Atom_Event*) (base + seq->atom.size);
    int64_t t = position < 0 ? 0 : position;
    if (t >= (int64_t) frames) t = (int64_t) frames - 1;
    ev->time.frames = (int64_t) frameOffset + t;
    ev->body.size = size;
    ev->body.type = self->uridMidiEvent;
    std::memcpy((uint8_t*) ev + sizeof(LV2_Atom_Event), data, size);
    seq->atom.size += need;
    return true;
  };

  // Short messages and SysEx MERGED by time, as CLAP's finishProcess merges
  // them: a sequence is read in order, and all the short ones first put a
  // note at frame 20 ahead of SysEx at frame 0, which JACK refuses. Each
  // short message at its real length -- a clock is one byte, a program
  // change two -- not padded out to three, which a receiver using running
  // status reads as extra events.
  const MidiBuffer& out = self->shared.midiOut;
  const auto* e = out.begin();
  const auto* x = out.sysexBegin();
  while (e != out.end() || x != out.sysexEnd()) {
    const bool shortNext =
        x == out.sysexEnd() || (e != out.end() && e->samplePosition <= x->samplePosition);
    if (shortNext) {
      const int status = e->message.getRawStatus();
      const int length = shortMessageLength(status);
      const uint8_t bytes[3] = {(uint8_t) status, (uint8_t) e->message.getRawData1(),
                                (uint8_t) e->message.getRawData2()};
      if (length > 0 && !put(e->samplePosition, bytes, (uint32_t) length)) break;
      ++e;
    } else {
      if (!put(x->samplePosition, out.sysexData(*x), x->length)) break;
      ++x;
    }
  }
}

/** What the notify port asks a host for (rsz:minimumSize): room for the
 *  state as one JSON message. */
constexpr uint32_t kUiNotifyMinimumBytes = 65536;

/**
 * [audio-thread] Put the state on the notify port, if anything is owed.
 *
 * Owed means a UI asked, or the state moved since the last time it was sent,
 * or a second of audio has run since then. The second keeps an already-open
 * interface current; the first lets one opened later catch up -- where the
 * host carries its request. REAPER does not, dependably: of an interface's
 * requests, sent at every idle, the plugin saw one per editor opened at best
 * and in some sessions none, and that interface's page never had the state.
 * The third is what does not depend on the host: an interface opened at any
 * time has the state within a second of audio. It costs a copy of the JSON
 * into the host's buffer once a second, and the interface passes on only a
 * state that differs from the one its page has.
 */
inline void writeUiNotify(Plugin* self) {
  LV2_Atom_Sequence* seq = self->uiNotifyOut;
  if (!seq) return;
  const uint32_t capacity = seq->atom.size;
  // Claim the buffer, whether or not anything goes in it: a host reads
  // atom.size to find out how much we wrote, and leaving its incoming
  // capacity there says the whole buffer is a message.
  seq->atom.type = self->uridAtomSequence;
  seq->atom.size = (uint32_t) sizeof(LV2_Atom_Sequence_Body);
  seq->body.unit = 0;
  seq->body.pad = 0;

  // ── Which keys are sounding ─────────────────────────────────
  //
  // The last thing an LV2 interface could not see that the others could. Sent
  // only when it CHANGED, which for a keyboard is a few times a second where
  // the meters move thirty -- and unlike the meters this cannot travel on a
  // control port, because a 32-bit mask does not survive a float's 24-bit
  // mantissa.
  const uint64_t low = self->shared.notes.low(), high = self->shared.notes.high();
  if (!self->notesSentValid || low != self->notesSentLow || high != self->notesSentHigh) {
    const uint32_t words[4] = {self->shared.notes.word(0), self->shared.notes.word(1),
                               self->shared.notes.word(2), self->shared.notes.word(3)};
    const uint32_t need = (uint32_t) sizeof(LV2_Atom_Event) + (uint32_t) sizeof(words);
    if (seq->atom.size + need <= capacity) {
      auto* ev = (LV2_Atom_Event*) ((uint8_t*) &seq->body + seq->atom.size);
      ev->time.frames = 0;
      ev->body.size = (uint32_t) sizeof(words);
      ev->body.type = self->uridNotes;
      std::memcpy((uint8_t*) ev + sizeof(LV2_Atom_Event), words, sizeof(words));
      seq->atom.size += need;
      self->notesSentLow = low;
      self->notesSentHigh = high;
      self->notesSentValid = true;
    }
  }

  const bool owed = self->uiStateRequested || self->uiStateSent != self->uiStateSeq ||
                    self->framesSinceUiState >= (uint64_t) self->sampleRate;
  if (!owed) return;

  const uint32_t payload = (uint32_t) self->uiStateJson.size();
  const uint32_t need = (uint32_t) sizeof(LV2_Atom_Event) + atomPad(payload);
  // Too big for the buffer the host gave: keep the debt rather than sending
  // half a message. A page that receives truncated JSON does something much
  // worse than a page that is told nothing yet.
  if (seq->atom.size + need > capacity) return;

  auto* ev = (LV2_Atom_Event*) ((uint8_t*) &seq->body + seq->atom.size);
  ev->time.frames = 0;
  ev->body.size = payload;
  ev->body.type = self->uridStateJson;
  if (payload)
    std::memcpy((uint8_t*) ev + sizeof(LV2_Atom_Event), self->uiStateJson.data(), payload);
  seq->atom.size += need;

  self->uiStateRequested = false;
  self->uiStateSent = self->uiStateSeq;
  self->framesSinceUiState = 0;
}

/** A number out of an atom of any of the four types a host may send one in. */
inline bool atomNumber(const Plugin* self, const LV2_Atom* a, double* out) {
  const void* v = a + 1;
  if (a->type == self->uridAtomFloat && a->size >= 4) *out = (double) *(const float*) v;
  else if (a->type == self->uridAtomDouble && a->size >= 8) *out = *(const double*) v;
  else if (a->type == self->uridAtomInt && a->size >= 4) *out = (double) *(const int32_t*) v;
  else if (a->type == self->uridAtomLong && a->size >= 8) *out = (double) *(const int64_t*) v;
  else return false;
  return std::isfinite(*out);
}

/**
 * [audio-thread] Take a time:Position into the transport.
 *
 * Any property may be missing -- a host sends what changed -- so each one
 * that is there overwrites its part and the rest stand. LV2 counts in the
 * meter's own beat (an eighth in 6/8, at that many a minute); every other
 * format, and TransportInfo, counts quarter notes, so beats and tempo are
 * converted by 4 / beatUnit.
 */
inline bool readTimePosition(Plugin* self, const LV2_Atom* obj) {
  if (obj->size < sizeof(LV2_Atom_Object_Body)) return false;
  if (((const LV2_Atom_Object_Body*) (obj + 1))->otype != self->uridTimePosition) return false;
  double bpm = -1.0, barBeat = -1.0, beat = -1.0, beatsPerBar = -1.0, beatUnit = -1.0;
  double frame = -1.0, bar = -1.0, speed = 0.0;
  bool haveSpeed = false;
  forEachAtomProperty(obj, [&](const LV2_Atom_Property_Body* p) {
    double v = 0.0;
    if (!atomNumber(self, &p->value, &v)) return;
    if (p->key == self->uridTimeBeatsPerMinute) bpm = v;
    else if (p->key == self->uridTimeBarBeat) barBeat = v;
    else if (p->key == self->uridTimeBeat) beat = v;
    else if (p->key == self->uridTimeBeatsPerBar) beatsPerBar = v;
    else if (p->key == self->uridTimeBeatUnit) beatUnit = v;
    else if (p->key == self->uridTimeFrame) frame = v;
    else if (p->key == self->uridTimeBar) bar = v;
    else if (p->key == self->uridTimeSpeed) speed = v, haveSpeed = true;
  });
  TransportInfo& t = self->transport;
  if (haveSpeed) {
    self->transportSpeed = speed;
    t.isPlaying = speed != 0.0;
  }
  if (beatsPerBar > 0.0 && beatUnit >= 1.0) {
    t.timeSigNumerator = (int) (beatsPerBar + 0.5);
    t.timeSigDenominator = (int) (beatUnit + 0.5);
  }
  const double quarter = t.timeSigDenominator > 0 ? 4.0 / (double) t.timeSigDenominator : 1.0;
  if (bpm > 0.0) {
    t.tempo = bpm * quarter;
    t.hasTempo = true;
  }
  if (frame >= 0.0 && self->sampleRate > 0.0) {
    t.positionSeconds = frame / self->sampleRate;
    t.hasSeconds = true;
  }
  if (bar >= 0.0 && barBeat >= 0.0) {
    const double barLength = beatsPerBar > 0.0 ? beatsPerBar : (double) t.timeSigNumerator;
    t.barNumber = (int32_t) bar;
    t.barStartBeats = bar * barLength * quarter;
    t.positionBeats = t.barStartBeats + barBeat * quarter;
    t.hasBeats = true;
  } else if (beat >= 0.0) {
    t.positionBeats = beat * quarter;
    t.hasBeats = true;
  }
  return true;
}

/** [audio-thread] Carry the transport over `frames` at the host's speed, so
 *  the next run() starts where this one ended. */
inline void advanceTransport(Plugin* self, uint32_t frames) {
  TransportInfo& t = self->transport;
  if (!t.isPlaying || !(self->sampleRate > 0.0)) return;
  const double seconds = (double) frames / self->sampleRate * self->transportSpeed;
  if (t.hasSeconds) t.positionSeconds += seconds;
  if (t.hasBeats && t.hasTempo) {
    t.positionBeats += seconds * t.tempo / 60.0;
    const double barLength = t.beatsPerBar();
    if (barLength > 0.0) {
      const double bars = std::floor((t.positionBeats - t.barStartBeats) / barLength);
      if (bars != 0.0 && std::isfinite(bars)) {
        t.barStartBeats += bars * barLength;
        t.barNumber += (int32_t) bars;
      }
    }
  }
}

/** A float as Turtle: the fewest significant digits that read back as the
 *  same float, with a full stop whatever the locale. %g gave six, so a
 *  default of 0.70710678 was published as 0.707107 -- a different float, and
 *  a host that resets a control to its default put it somewhere the plugin
 *  never did. Nine is always enough; 0.1 still prints as 0.1. */
inline void ttlNumber(char* out, size_t capacity, float value) {
  char conversion[8];
  for (int digits = 6; digits <= 9; ++digits) {
    std::snprintf(conversion, sizeof(conversion), "%%.%dg", digits);
    numtext::format(out, capacity, (double) value, conversion);
    if ((float) numtext::parseOr(out, 0.0) == value) return;
  }
}

/** An atom event's frame, clamped into a run of `sampleCount`: at or past the
 *  end a note-off was never reached and the note hung, and a frame past what
 *  an int holds came out negative (see clapwrap::eventTime). */
inline int frameInRun(int64_t frame, uint32_t sampleCount) {
  if (sampleCount == 0 || frame < 0) return 0;
  return frame < (int64_t) sampleCount ? (int) frame : (int) sampleCount - 1;
}

inline void run(LV2_Handle handle, uint32_t sampleCount) {
  auto* self = (Plugin*) handle;
  claimMidiOut(self);
  if (!self->audioOut[0]) {
    // Nothing to render into -- but the notify port is still the host's to
    // read back, and an unclaimed one reads as its whole capacity.
    if (LV2_Atom_Sequence* seq = self->uiNotifyOut) {
      seq->atom.type = self->uridAtomSequence;
      seq->atom.size = (uint32_t) sizeof(LV2_Atom_Sequence_Body);
    }
    return;
  }
  const uint32_t width = (uint32_t) lv2Width();
  // Every channel of the main bus, each its own port. An output a host left
  // unconnected falls back to the first, as the right one always did.
  float* chans[clapwrap::kMaxAudioChannels];
  for (uint32_t c = 0; c < width; ++c)
    chans[c] = self->audioOut[c] ? self->audioOut[c] : self->audioOut[0];

  // Is the host bouncing? Checked before anything else in the block, because a
  // change re-prepares the DSP and everything below should run against the
  // settings it was just prepared with.
  updateFreeWheel(self);

  // Controls: one float each, read at block rate and clamped: the host wrote
  // whatever its UI or automation held, and trusting it blindly is how an
  // out-of-range value reaches a filter coefficient.
  for (int i = 0; i < SONORE_NUM_PARAMS && i < kDesc.numParams; ++i)
    if (self->controls[i])
      self->shared.params[i] = clampToRange(kDesc.params[i], *self->controls[i]);

  // lv2:enabled reads inverted from bypass: > 0 means processing. A host that
  // never connected the (connectionOptional) port gets a plugin that is
  // simply always enabled.
  if (!isInstrument()) self->shared.bypass.engaged = self->enabled && *self->enabled <= 0.0f;
  if (self->latencyOut) *self->latencyOut = (float) clapwrap::dspLatency(self->shared.dsp);

  // MIDI arrives as an atom:Sequence of events with frame timestamps, walked
  // by the same bounds-checked walk as every other sequence here. This one
  // had its own copy, whose pointer sum could wrap on a 32-bit build and
  // whose step padded a host's size in 32 bits.
  //
  // ADDED to what is already waiting, not cleared first: every slice below
  // clears what it played, so anything here is what a run of ZERO frames
  // read and never played. It was cleared at this line, unplayed -- a
  // note-off in such a run left its note sounding. It plays at frame 0 of
  // this run instead, as a VST3 zero-sample call's events do.
  if (self->midiIn && midiPorts()) {
    forEachAtomEvent(self->midiIn, [self, sampleCount](const LV2_Atom_Event* event) {
      const uint8_t* body = (const uint8_t*) event + sizeof(LV2_Atom_Event);
      const int at = frameInRun(event->time.frames, sampleCount);
      if (event->body.type == self->uridMidiEvent && event->body.size >= 1) {
        const int status = body[0];
        // >= 1, not >= 3. Clock, start, stop and continue are a SINGLE byte,
        // so the old length test threw them away a second time even where the
        // status test would have let them through.
        if (isSysexStart(status)) {
          // One atom event carries the WHOLE message -- the atom has a size,
          // so unlike a serial stream there is nothing to reassemble.
          self->shared.midi.addSysex(body, (size_t) event->body.size, at);
        } else if (deliverableToDsp(status)) {
          const int d1 = event->body.size >= 2 ? body[1] : 0;
          const int d2 = event->body.size >= 3 ? body[2] : 0;
          self->shared.midi.addEvent(MidiMessage(status, d1, d2), at);
        }
      }
    });
  }

  for (uint32_t c = 0; c < width; ++c) {
    if (c > 0 && chans[c] == chans[0]) continue; // a fallback: already written
    if (isInstrument()) {
      std::memset(chans[c], 0, sampleCount * sizeof(float));
    } else {
      const float* in = self->audioIn[c] ? self->audioIn[c] : self->audioIn[0];
      if (in && in != chans[c]) std::memcpy(chans[c], in, sampleCount * sizeof(float));
    }
  }

  // The host's timeline. A time:Position arrives when the transport jumps --
  // start, stop, locate, a tempo change -- and between them the position is
  // carried forward here. The DSP was handed an empty one, always: no tempo,
  // no position, no play state, in every LV2 host.
  //
  // A position describes the song AT ITS FRAME. The DSP hears of it for the
  // whole run (a transport is block-granular in every format), but it is
  // carried forward only from where it was stamped: carried over the whole
  // run, a locate stamped at frame 300 left the position 300 frames ahead
  // for good.
  if (clapwrap::WantsTransport<SonoreDsp>::value) {
    uint32_t stampedAt = 0;
    if (self->timeIn && self->timeIn->atom.type == self->uridAtomSequence)
      forEachAtomEvent(self->timeIn, [self, sampleCount, &stampedAt](const LV2_Atom_Event* ev) {
        if (ev->body.type == self->uridAtomObject || ev->body.type == self->uridAtomBlank)
          if (readTimePosition(self, &ev->body))
            stampedAt = (uint32_t) frameInRun(ev->time.frames, sampleCount);
      });
    clapwrap::sendTransport(self->shared.dsp, self->transport);
    advanceTransport(self, sampleCount - stampedAt);
  }
  // Slice: prepare() promised the DSP at most 8192 frames per call, and LV2
  // hosts are allowed to exceed any expectation you didn't write down.
  //
  // The MIDI is sliced WITH the audio: each slice gets the events inside it,
  // at times counted from its own start. All of it went to the first slice,
  // at times counted from the start of the run -- so a note at frame 9000 of
  // a 12000-frame run reached a DSP rendering 8192 frames and was never
  // played, and a note-off lost that way left a note stuck.
  const bool sliced = sampleCount > 8192;
  if (sliced) {
    self->runMidi.clear();
    for (const auto& e : self->shared.midi) self->runMidi.addEvent(e.message, e.samplePosition);
    forwardSysex(self->shared.midi, self->runMidi);
  }
  uint32_t done = 0;
  while (done < sampleCount) {
    const uint32_t n = (sampleCount - done) > 8192 ? 8192 : (sampleCount - done);
    if (sliced) {
      self->shared.midi.clear();
      const int lo = (int) done, hi = (int) (done + n);
      for (const auto& e : self->runMidi)
        if (e.samplePosition >= lo && e.samplePosition < hi)
          self->shared.midi.addEvent(e.message, e.samplePosition - lo);
      for (auto it = self->runMidi.sysexBegin(); it != self->runMidi.sysexEnd(); ++it)
        if (it->samplePosition >= lo && it->samplePosition < hi)
          self->shared.midi.addSysex(self->runMidi.sysexData(*it), it->length,
                                     it->samplePosition - lo);
    }
    float* slice[clapwrap::kMaxAudioChannels];
    for (uint32_t c = 0; c < width; ++c) slice[c] = chans[c] + done;
    float* zero = self->shared.scSilence.empty() ? nullptr : self->shared.scSilence.data();
    float* scSlice[2] = {zero, zero};
    if (self->scIn[0]) {
      scSlice[0] = (float*) self->scIn[0] + done;
      scSlice[1] = (float*) (self->scIn[1] ? self->scIn[1] : self->scIn[0]) + done;
    }
    AudioBlock<float> scBlock(scSlice, 2, scSlice[0] ? n : 0);
    if (clapwrap::HasLatency<SonoreDsp>::value)
      self->shared.bypass.follow(clapwrap::dspLatency(self->shared.dsp));
    clapwrap::bypassCapture(self->shared.bypass, slice, width, n);
    AudioBlock<float> block(slice, width, n);

    // Aux buses: our flattened ports, re-grouped into blocks for the context.
    // Held in the instance, not zeroed on the stack for every slice (see
    // clapwrap::processdetail::AuxScratch).
    AudioBlock<float>* const auxBlocks = self->shared.aux32.blocks;
    const uint32_t nAux = clapwrap::numAuxOutputs();
    uint32_t flat = 0;
    for (uint32_t b = 0; b < nAux; ++b) {
      float** const auxRow = self->shared.aux32.row(b);
      const uint32_t want = clapwrap::auxBusChannels(b);
      uint32_t have = 0;
      for (uint32_t c = 0; c < want; ++c, ++flat)
        if (self->auxOut[flat]) auxRow[have++] = self->auxOut[flat] + done;
      auxBlocks[b] = AudioBlock<float>(auxRow, have, have ? n : 0);
    }

    self->shared.midiOut.clear();
    uint8_t roles[clapwrap::kMaxAudioChannels];
    const uint32_t numRoles =
        rolesFromMask(defaultChannelMask(width), roles, clapwrap::kMaxAudioChannels);
    self->shared.expression.clear();
    if (kDesc.supportsMpe) self->shared.mpe.process(self->shared.midi, self->shared.expression);
    // ctx.offline too: it was left false, so a DSP reading it -- as the
    // other formats let it -- never heard that the host was freewheeling.
    ProcessContext ctx{block,
                       auxBlocks,
                       nAux,
                       scBlock,
                       self->shared.midi,
                       self->shared.midiOut,
                       numRoles ? roles : nullptr,
                       &self->shared.expression,
                       nullptr,
                       true,
                       self->shared.offline};
    trackNotes(self->shared.notes, ctx.midi);
    clapwrap::snapshotParams(&self->shared);
    clapwrap::runDspCtx(self->shared.dsp, ctx, self->shared.paramsBlock);
    writeMidiOut(self, done, n);
    clapwrap::bypassApply(self->shared.bypass, slice, width, n);
    // NaN or infinity on any output port leaves as silence (output_guard.h),
    // each slice as the block the DSP ran. LV2 has no main thread to ask for:
    // the first trip goes to the worker, which may write a log, and a host
    // with no worker hears of it at deactivate().
    if (clapwrap::guardOutputs(&self->shared, slice, width, auxBlocks, nAux, n) && self->worker &&
        self->worker->schedule_work)
      self->worker->schedule_work(self->worker->handle,
                                  (uint32_t) std::strlen(outputGuardWork()) + 1,
                                  outputGuardWork());
    self->shared.midi.clear(); // this slice's events are spent
    done += n;
  }

  // ── The interface's atom ports ────────────────────────────────
  //
  // Read the request, then answer it. Nothing here allocates: the answer was
  // built in activate() or in state restore, and this copies bytes into a
  // buffer the host provided.
  if (self->uiControlIn && self->uiControlIn->atom.type == self->uridAtomSequence) {
    forEachAtomEvent(self->uiControlIn, [self](const LV2_Atom_Event* ev) {
      if (ev->body.type == self->uridStateRequest) {
        self->uiStateRequested = true;
      } else if (ev->body.type == self->uridLoadFile && self->worker &&
                 self->worker->schedule_work) {
        // Handed straight to the host's thread. Nothing is read here, and
        // nothing about the DSP is touched -- run() has no business opening a
        // file and no thread to do it on.
        self->worker->schedule_work(self->worker->handle, ev->body.size,
                                    (const uint8_t*) ev + sizeof(LV2_Atom_Event));
      }
    });
  }
  self->framesSinceUiState += sampleCount;
  writeUiNotify(self);

  // The whole run() call, not the last slice: a host that splits a block at
  // every MIDI event would otherwise have its meter show the level of the two
  // samples after the final note.
  if (self->meterPeakOut || self->meterRmsOut) {
    const BlockLevel level =
        measureBlock(self->audioOut[0], (size_t) sampleCount);
    if (self->meterPeakOut) *self->meterPeakOut = level.peak;
    if (self->meterRmsOut) *self->meterRmsOut = level.rms;
  }
  // Not a measurement of the output: the DSP is the only thing that knows how
  // much it took off, so it reports it or there is no port.
  if (self->meterGrOut) *self->meterGrOut = clapwrap::dspGainReduction(self->shared.dsp);
}

/** [worker-thread or main-thread] The output guard's report, once: a warning
 *  in the host's log where it gave one, stderr otherwise. Never run(): the
 *  log's printf is for non-realtime contexts only. */
inline void reportOutputGuard(Plugin* self) {
  char message[400];
  if (!self->shared.outputGuard.takeReport(kDesc.name, message, sizeof(message))) return;
  if (self->log && self->log->print) {
    self->log->print(self->log->handle, self->uridLogWarning, "%s\n", message);
    return;
  }
  clapwrap::writeOutputGuardReport(message);
}

/**
 * [worker-thread] Read the file the interface picked.
 *
 * The host calls this off the audio thread, which is the entire point: this
 * is where a sampler opens a WAV, and doing that in run() is a dropout with
 * a plausible-sounding excuse.
 *
 * The payload is "purpose\0path" -- two C strings in one blob, because the
 * schedule call takes one buffer and inventing a struct for two strings would
 * mean agreeing on padding across a boundary that does not need it. The one
 * other payload is run()'s own: the output guard has something to report.
 */
inline Lv2WorkerStatus workerWork(LV2_Handle handle, Lv2WorkerRespondFunction respond,
                                  Lv2WorkerRespondHandle responder, uint32_t size,
                                  const void* data) {
  auto* self = (Plugin*) handle;
  if (data && size == std::strlen(outputGuardWork()) + 1 &&
      std::memcmp(data, outputGuardWork(), size) == 0) {
    reportOutputGuard(self);
    return kLv2WorkerSuccess;
  }
  if (!data || size < 2) return kLv2WorkerErrUnknown;
  const char* purpose = (const char*) data;
  const size_t purposeLength = strnlen(purpose, size);
  if (purposeLength + 1 >= size) return kLv2WorkerErrUnknown;
  // The path is whatever follows the purpose's terminator, up to the END OF
  // THE ATOM: the UI sends it with no terminator of its own, so it is copied
  // out with the atom's length as the bound rather than handed over as a C
  // string that would read on into whatever follows. This is the worker
  // thread, so the copy may allocate.
  const size_t remaining = size - purposeLength - 1;
  const std::string pathCopy(purpose + purposeLength + 1,
                             strnlen(purpose + purposeLength + 1, remaining));
  const char* path = pathCopy.c_str();

  // The host may be saving or restoring on its own thread right now, and the
  // DSP's file and its state are one thing to it.
  std::lock_guard<std::mutex> turn(self->mainThread);
  freeRetiredUiText(self);
  clapwrap::sendFile(self->shared.dsp, purpose, path);

  // Rebuilt HERE, where allocating is allowed, into a buffer of its own that
  // travels in the response. run() gets it by swapping two pointers.
  StateBag bag;
  clapwrap::saveDspState(self->shared.dsp, bag);
  auto* fresh = new Plugin::UiText;
  fresh->json = bagToJson(bag);
  fresh->build = ++self->uiBuilt;
  // Listed before it is handed over, so a host that never delivers it
  // leaves it to cleanup() rather than to nobody.
  fresh->older = self->uiTexts;
  self->uiTexts = fresh;
  if (!respond || respond(responder, (uint32_t) sizeof(fresh), &fresh) != kLv2WorkerSuccess) {
    self->uiTexts = fresh->older; // nothing will deliver it
    delete fresh;
  }
  return kLv2WorkerSuccess;
}

/** [audio-thread] The worker finished. Swapping two std::strings moves two
 *  pointers and allocates nothing, and pushing the husk is a compare-and-swap,
 *  which is what makes this legal here. */
inline Lv2WorkerStatus workerResponse(LV2_Handle handle, uint32_t size, const void* data) {
  auto* self = (Plugin*) handle;
  Plugin::UiText* fresh = nullptr;
  if (!data || size != sizeof(fresh)) return kLv2WorkerErrUnknown;
  std::memcpy(&fresh, data, sizeof(fresh));
  if (!fresh) return kLv2WorkerErrUnknown;
  // Only a state newer than the one shown. Older is a load the host has
  // since restored over: its text goes to be freed unread. Compared as a
  // signed difference, so the numbering may wrap.
  if ((int32_t) (fresh->build - self->uiShown) > 0) {
    self->uiStateJson.swap(fresh->json);
    self->uiShown = fresh->build;
    ++self->uiStateSeq;
  }
  fresh->next = self->uiRetired.load(std::memory_order_relaxed);
  while (!self->uiRetired.compare_exchange_weak(fresh->next, fresh, std::memory_order_release,
                                                std::memory_order_relaxed)) {
  }
  return kLv2WorkerSuccess;
}

/** Instantiation class, so not realtime: the output guard's report for a host
 *  with no worker, or one whose worker has not got to it yet. */
inline void deactivate(LV2_Handle handle) { reportOutputGuard((Plugin*) handle); }

inline void cleanup(LV2_Handle handle) {
  auto* self = (Plugin*) handle;
  reportOutputGuard(self); // a host that never deactivated
  freeRetiredUiText(self);
  // And the answers the host dropped: no work_response() comes after this.
  while (Plugin::UiText* t = self->uiTexts) {
    self->uiTexts = t->older;
    delete t;
  }
  self->~Plugin();
  std::free(self);
}

// ── State: the same versioned SNRS blob as every other format ────────────────

inline LV2_State_Status stateSave(LV2_Handle handle, LV2_State_Store_Function store,
                                  LV2_State_Handle host, uint32_t,
                                  const LV2_Feature* const*) {
  auto* self = (Plugin*) handle;
  // Not while the worker is loading a file into the DSP being saved.
  std::lock_guard<std::mutex> turn(self->mainThread);
  // The FULL versioned SNRS blob, built by the one shared serializer in
  // clap_wrapper.h -- params, bypass, selected preset, editor size AND the
  // DSP's StateBag. This file used to write only header+params while stamping
  // the CURRENT version number, so an LV2 host (Ardour, Carla) silently dropped
  // a sampler's loaded file, the bypass state, the chosen preset and the window
  // size on every reload -- exactly the drift the VST3 wrapper already had
  // fixed by routing through this same function. LV2 stores a blob at once
  // rather than a stream, so the put callback appends to a buffer and we hand
  // the whole thing to store() below.
  std::vector<uint8_t> blob;
  const bool ok = clapwrap::saveStateBody(&self->shared,
                                          [&blob](const void* data, size_t size) -> size_t {
                                            const auto* p = (const uint8_t*) data;
                                            blob.insert(blob.end(), p, p + size);
                                            return size;
                                          });
  if (!ok || blob.empty()) return LV2_STATE_ERR_UNKNOWN;
  // POD, not PORTABLE. state.h's portable values "MUST NOT depend on ...
  // endianness or alignment" and "MUST NOT contain filenames"; this is a
  // native struct dump, and the DSP's bag in it may carry an absolute path
  // (the sampler's sample). Declared portable, a host may move it between
  // machines as if it were neither. Mapping such paths through state:mapPath
  // needs the bag to know which entries ARE paths, which it does not yet.
  return store(host, self->uridStateBlob, blob.data(), blob.size(), self->uridAtomChunk,
               LV2_STATE_IS_POD);
}

inline LV2_State_Status stateRestore(LV2_Handle handle, LV2_State_Retrieve_Function retrieve,
                                     LV2_State_Handle host, uint32_t,
                                     const LV2_Feature* const*) {
  auto* self = (Plugin*) handle;
  std::lock_guard<std::mutex> turn(self->mainThread);
  size_t size = 0;
  uint32_t type = 0, flags = 0;
  const auto* blob = (const uint8_t*) retrieve(host, self->uridStateBlob, &size, &type, &flags);
  if (!blob || size < sizeof(clapwrap::StateHeader)) return LV2_STATE_ERR_NO_PROPERTY;

  // The same shared parser every other format uses: reads params, bypass,
  // preset, editor size and the StateBag, tolerates an older or newer param
  // count, and hands an empty bag to a DSP whose blob predates bags. LV2 hands
  // back the whole blob at once, so the get callback walks it by offset and
  // returns 0 (EOF) once the bag has consumed the rest -- which is exactly how
  // the serializer knows the bag has ended.
  size_t offset = 0;
  const bool ok = clapwrap::loadStateBody(
      &self->shared, [blob, size, &offset](void* dst, size_t want) -> size_t {
        const size_t remaining = size - offset;
        const size_t n = want < remaining ? want : remaining;
        if (n) std::memcpy(dst, blob + offset, n);
        offset += n;
        return n;
      });
  // The interface, if one is open, is now showing the previous session.
  refreshUiState(self);
  return ok ? LV2_STATE_SUCCESS : LV2_STATE_ERR_BAD_TYPE;
}

inline const void* extensionData(const char* uri) {
  if (std::strcmp(uri, LV2_STATE__interface) == 0) {
    // Not named `interface`: on Windows that word is a COM macro (objbase.h
    // defines it to `struct`), and the CLAP wrapper's webview chain pulls
    // windows.h into this translation unit.
    static const LV2_State_Interface stateInterface = {stateSave, stateRestore};
    return &stateInterface;
  }
  if (std::strcmp(uri, SONORE_LV2_WORKER__interface) == 0) {
    static const Lv2WorkerInterface workerInterface = {workerWork, workerResponse, nullptr};
    return &workerInterface;
  }
  return nullptr;
}

inline const LV2_Descriptor* descriptor() {
  static char uri[256];
  static LV2_Descriptor d = {};
  static bool ready = false;
  if (!ready) {
    pluginUri(uri, sizeof(uri));
    d.URI = uri;
    d.instantiate = instantiate;
    d.connect_port = connectPort;
    d.activate = activate;
    d.run = run;
    d.deactivate = deactivate;
    d.cleanup = cleanup;
    d.extension_data = extensionData;
    ready = true;
  }
  return &d;
}

// ── The TTL generator ────────────────────────────────────────────────────────
/** The suffix of main-bus audio port `c`'s symbol and name. "l"/"r" for the
 *  first two, which every existing session's port symbols already say and
 *  which must not change; the channel's number after that. It used to be
 *  `c ? 'r' : 'l'` for every channel, so a quad bus declared four ports and
 *  three of them were "in_r" -- symbols LV2 requires to be unique. */
inline const char* audioPortSymbol(int c) {
  static const char* const kSymbols[] = {"l", "r", "3", "4", "5", "6", "7", "8"};
  return (c >= 0 && c < 8) ? kSymbols[c] : "x";
}
inline const char* audioPortName(int c) {
  static const char* const kNames[] = {"L", "R", "3", "4", "5", "6", "7", "8"};
  return (c >= 0 && c < 8) ? kNames[c] : "X";
}

/** Escape a string for a Turtle literal.
 *
 *  A preset called `Bass "Heavy"` would otherwise close the literal early and
 *  turn the rest of the file into a parse error -- which a host reports as a
 *  plugin that does not exist, because a bundle whose metadata will not parse
 *  is a bundle it skips entirely. */
inline void writeTurtleString(std::FILE* out, const char* text) {
  for (const char* p = text; p && *p; ++p) {
    switch (*p) {
      case '"': std::fputs("\\\"", out); break;
      case '\\': std::fputs("\\\\", out); break;
      case '\n': std::fputs("\\n", out); break;
      case '\r': std::fputs("\\r", out); break;
      case '\t': std::fputs("\\t", out); break;
      default: std::fputc(*p, out); break;
    }
  }
}

/** An IRI inside <...>: the characters Turtle forbids there are
 *  percent-encoded, so a URL with a space or a stray quote yields a file
 *  every host can still parse rather than one none can. */
inline void writeTurtleIri(std::FILE* out, const char* text) {
  for (const char* p = text; p && *p; ++p) {
    const unsigned char c = (unsigned char) *p;
    if (c <= 0x20 || c == '<' || c == '>' || c == '"' || c == '{' || c == '}' || c == '|' ||
        c == '^' || c == '`' || c == '\\')
      std::fprintf(out, "%%%02X", (unsigned) c);
    else
      std::fputc(*p, out);
  }
}

/** LV2's own unit for a parameter's unit string -- the units extension's
 *  spelling of the ones the SDK uses (the AU wrapper maps the same set) -- or
 *  null for one it has no word for, which the TTL then declares itself. */
inline const char* lv2UnitFor(const char* unit) {
  if (!unit || !unit[0]) return nullptr;
  static const char* const kUnits[][2] = {
      {"dB", "units:db"},   {"Hz", "units:hz"},     {"kHz", "units:khz"},
      {"ms", "units:ms"},   {"s", "units:s"},       {"min", "units:min"},
      {"%", "units:pc"},    {"ct", "units:cent"},   {"cents", "units:cent"},
      {"st", "units:semitone12TET"},                {"semitones", "units:semitone12TET"},
      {"bpm", "units:bpm"}, {"BPM", "units:bpm"},   {"oct", "units:oct"},
  };
  for (const auto& u : kUnits)
    if (std::strcmp(unit, u[0]) == 0) return u[1];
  return nullptr;
}

/** lv2:symbol is [_a-zA-Z][_a-zA-Z0-9]*. A parameter id outside that -- a
 *  hyphen, a leading digit -- is mapped onto it, and the SAME mapping is used
 *  for the presets file, so the two always name the same port. */
inline void lv2Symbol(const char* id, char* out, size_t capacity) {
  size_t n = 0;
  if (!id || !id[0]) id = "port";
  const unsigned char first = (unsigned char) id[0];
  if (first >= '0' && first <= '9' && n + 1 < capacity) out[n++] = '_';
  for (const char* p = id; *p && n + 1 < capacity; ++p) {
    const unsigned char c = (unsigned char) *p;
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '_';
    out[n++] = ok ? (char) c : '_';
  }
  out[n] = '\0';
}

/** Whether `symbol` is one the wrapper may give a port of its own: the
 *  designated controls, the meters, the atom ports, and every audio port
 *  name it can write (in_/out_ l, r, 3..8 and x, sc_l and sc_r, auxB_C). */
inline bool lv2ReservedSymbol(const char* symbol) {
  static const char* const kFixed[] = {"enabled", "freewheel", "latency",  "meterPeak", "meterRms",
                                       "meterGr", "midi_in",   "midi_out", "time_in",   "uiControl",
                                       "uiNotify", "sc_l",     "sc_r"};
  for (const char* fixed : kFixed)
    if (std::strcmp(symbol, fixed) == 0) return true;
  static const char* const kChannels[] = {"l", "r", "3", "4", "5", "6", "7", "8", "x"};
  for (const char* c : kChannels) {
    char name[16];
    std::snprintf(name, sizeof(name), "in_%s", c);
    if (std::strcmp(symbol, name) == 0) return true;
    std::snprintf(name, sizeof(name), "out_%s", c);
    if (std::strcmp(symbol, name) == 0) return true;
  }
  // "aux", digits, "_", digits, and nothing after: an aux bus's channel.
  // Walked by hand -- sscanf is C4996 under MSVC, an error where warnings are.
  if (std::strncmp(symbol, "aux", 3) != 0) return false;
  const char* p = symbol + 3;
  const auto digits = [&p] {
    const char* start = p;
    while (*p >= '0' && *p <= '9') ++p;
    return p != start;
  };
  if (!digits() || *p++ != '_' || !digits()) return false;
  return *p == '\0';
}

/** Every parameter's lv2:symbol, in table order.
 *
 *  lv2Symbol() of the id, made unique. LV2 requires a plugin's port symbols
 *  to be, and hosts look ports up by them -- a parameter whose id was
 *  "enabled" or "latency" shared a symbol with the wrapper's own port of that
 *  name, and two ids that map onto one symbol ("gain-db", "gain_db") shared
 *  it with each other: a host saving or automating one of them by symbol got
 *  the other. A clash now gets "_2", "_3"... until it is free. Every id that
 *  was already unique keeps its symbol, so no existing session's or preset's
 *  port moves. The TTL and the presets file both take their symbols from
 *  here. Built at TTL generation, which is allowed to allocate. */
inline std::vector<std::string> lv2ParamSymbols() {
  std::vector<std::string> symbols;
  symbols.reserve((size_t) (kDesc.numParams > 0 ? kDesc.numParams : 0));
  for (int i = 0; i < kDesc.numParams; ++i) {
    char base[96];
    lv2Symbol(kDesc.params[i].id, base, sizeof(base));
    std::string symbol = base;
    auto taken = [&](const std::string& candidate) {
      if (lv2ReservedSymbol(candidate.c_str())) return true;
      for (const std::string& earlier : symbols)
        if (earlier == candidate) return true;
      return false;
    };
    for (int n = 2; taken(symbol); ++n) symbol = std::string(base) + "_" + std::to_string(n);
    symbols.push_back(symbol);
  }
  return symbols;
}

/** How many factory presets are genuinely usable.
 *
 *  A preset whose value count does not match the parameter contract is
 *  skipped rather than written out half-applied -- the same rule applyPreset()
 *  follows at runtime, so a host never sees a preset the plugin would refuse. */
inline int numFactoryPresets() {
  if (!kDesc.presets || kDesc.numPresets <= 0) return 0;
  return kDesc.numPresets;
}

/** Factory presets in LV2's own spelling: one pset:Preset per preset, naming
 *  each port by the symbol the plugin published and the value it should take.
 *
 *  This existed in CLAP (preset discovery) and in VST3 (program lists) and
 *  nowhere here, so a plugin shipping four presets showed none of them in an
 *  LV2 host. */
inline void writePresetsTtl(std::FILE* out, const char* uri) {
  std::fprintf(out,
               "@prefix lv2:  <http://lv2plug.in/ns/lv2core#> .\n"
               "@prefix pset: <http://lv2plug.in/ns/ext/presets#> .\n"
               "@prefix rdfs: <http://www.w3.org/2000/01/rdf-schema#> .\n");

  const std::vector<std::string> symbols = lv2ParamSymbols();
  for (int i = 0; i < numFactoryPresets(); ++i) {
    const Preset& preset = kDesc.presets[i];
    std::fprintf(out,
                 "\n<%s#preset%d>\n"
                 "    a pset:Preset ;\n"
                 "    lv2:appliesTo <%s> ;\n"
                 "    rdfs:label \"",
                 uri, i, uri);
    writeTurtleString(out, preset.name);
    std::fprintf(out, "\" ;\n");

    // Only the ports this preset actually carries a value for. A preset
    // written before a control existed simply says nothing about it, and the
    // host leaves it wherever it was -- which is what LV2 does with any port
    // a preset omits.
    const int n = preset.numValues < kDesc.numParams ? preset.numValues : kDesc.numParams;
    if (n <= 0) {
      std::fprintf(out, "    rdfs:comment \"no values\" .\n");
      continue;
    }
    std::fprintf(out, "    lv2:port ");
    for (int v = 0; v < n; ++v) {
      char value[32];
      ttlNumber(value, sizeof(value), preset.values[v]);
      std::fprintf(out, "%s[\n        lv2:symbol \"%s\" ;\n        pset:value %s\n    ]",
                   v == 0 ? "" : " ,\n             ", symbols[(size_t) v].c_str(), value);
    }
    std::fprintf(out, " .\n");
  }
}

// Compiled from the SAME source with SONORE_LV2_TTLGEN: prints manifest.ttl and
// <name>.ttl for the bundle. Hosts read these before loading any code, so a
// port list that disagrees with connect_port() is a crash: generating both
// from one descriptor is what makes the disagreement impossible.

/** Map the descriptor's plain-word category onto lv2core's plugin taxonomy.
 *  Hosts (and lv2lint) treat the bare base class as "uncategorised", so a
 *  known word buys real browser placement; an unknown one falls back rather
 *  than guessing.
 *
 *  An INSTRUMENT is lv2:InstrumentPlugin whatever its category says. The
 *  class is what a host files a plugin by -- REAPER lists an LV2 as "LV2i:"
 *  only for this one -- and isInstrument is what CLAP's instrument feature
 *  and VST3's "Instrument" subcategory are built from. The category used to
 *  win here, so the arpeggiator (isInstrument, "utility") was an instrument
 *  in REAPER as CLAP and VST3 and an effect as LV2. */
inline const char* lv2ClassFor(const PluginDescriptor& d) {
  if (d.isInstrument) return "lv2:InstrumentPlugin";
  struct Pair { const char* key; const char* cls; };
  static const Pair kMap[] = {
      {"distortion", "lv2:DistortionPlugin"}, {"saturator", "lv2:DistortionPlugin"},
      {"waveshaper", "lv2:WaveshaperPlugin"}, {"amplifier", "lv2:AmplifierPlugin"},
      {"reverb", "lv2:ReverbPlugin"},         {"delay", "lv2:DelayPlugin"},
      {"echo", "lv2:DelayPlugin"},            {"compressor", "lv2:CompressorPlugin"},
      {"limiter", "lv2:LimiterPlugin"},       {"gate", "lv2:GatePlugin"},
      {"expander", "lv2:ExpanderPlugin"},     {"dynamics", "lv2:DynamicsPlugin"},
      {"eq", "lv2:EQPlugin"},                 {"equalizer", "lv2:EQPlugin"},
      {"filter", "lv2:FilterPlugin"},         {"chorus", "lv2:ChorusPlugin"},
      {"flanger", "lv2:FlangerPlugin"},       {"phaser", "lv2:PhaserPlugin"},
      {"modulation", "lv2:ModulatorPlugin"},  {"pitch", "lv2:PitchPlugin"},
      {"spectral", "lv2:SpectralPlugin"},     {"utility", "lv2:UtilityPlugin"},
      {"analyzer", "lv2:AnalyserPlugin"},     {"analyser", "lv2:AnalyserPlugin"},
      {"spatial", "lv2:SpatialPlugin"},       {"simulator", "lv2:SimulatorPlugin"},
      {"synth", "lv2:InstrumentPlugin"},      {"instrument", "lv2:InstrumentPlugin"},
      {"oscillator", "lv2:OscillatorPlugin"}, {"generator", "lv2:GeneratorPlugin"},
  };
  if (d.category)
    for (const Pair& m : kMap)
      if (std::strcmp(d.category, m.key) == 0) return m.cls;
  return nullptr;
}

/** LV2's version pair is its own numbering domain: even minor >= 2 announces a
 *  stable public release, 0 or odd reads as pre-release/development to every
 *  host (and lv2lint flags it). Copying the product's semver digits verbatim
 *  would brand 1.0.1 unstable, so map monotonically onto the convention:
 *  doubling keeps ordering and keeps everything even. */
inline void lv2VersionFromProduct(const char* version, int* outMinor, int* outMicro) {
  int maj = 0, min = 0, mic = 0;
  // Parsed by hand: sscanf is the one call in this file MSVC flags (C4996),
  // which a project with /sdl -- Visual Studio's default -- turns into an error.
  if (version) {
    char* end = nullptr;
    maj = (int) std::strtol(version, &end, 10);
    if (end && *end == '.') {
      min = (int) std::strtol(end + 1, &end, 10);
      if (end && *end == '.') mic = (int) std::strtol(end + 1, &end, 10);
    }
  }
  *outMinor = maj * 2;
  *outMicro = (min * 100 + mic) * 2;
}

inline void writeTtl(std::FILE* out, const char* binaryName) {
  char uri[256];
  pluginUri(uri, sizeof(uri));

  std::fprintf(out,
               "@prefix atom:  <http://lv2plug.in/ns/ext/atom#> .\n"
               "@prefix doap:  <http://usefulinc.com/ns/doap#> .\n"
               "@prefix lv2:   <http://lv2plug.in/ns/lv2core#> .\n"
               "@prefix foaf:  <http://xmlns.com/foaf/0.1/> .\n"
               "@prefix log:   <http://lv2plug.in/ns/ext/log#> .\n"
               "@prefix midi:  <http://lv2plug.in/ns/ext/midi#> .\n"
               "@prefix pg:    <http://lv2plug.in/ns/ext/port-groups#> .\n"
               // port-props, for notOnGUI. Declared here because a turtle file
               // that USES a prefix it never declared is not a turtle file --
               // lv2lint rejects the whole bundle, not just the line.
               "@prefix pprops: <http://lv2plug.in/ns/ext/port-props#> .\n"
               // resize-port, for the notify port's rsz:minimumSize.
               "@prefix rsz:   <http://lv2plug.in/ns/ext/resize-port#> .\n"
               "@prefix rdf:   <http://www.w3.org/1999/02/22-rdf-syntax-ns#> .\n"
               "@prefix rdfs:  <http://www.w3.org/2000/01/rdf-schema#> .\n"
               "@prefix state: <http://lv2plug.in/ns/ext/state#> .\n"
               "@prefix time:  <http://lv2plug.in/ns/ext/time#> .\n"
               "@prefix work:  <http://lv2plug.in/ns/ext/worker#> .\n"
               "@prefix units: <http://lv2plug.in/ns/extensions/units#> .\n"
               "@prefix urid:  <http://lv2plug.in/ns/ext/urid#> .\n\n");
  const char* lv2Class = lv2ClassFor(kDesc);
  // The plugin is a doap:Project as well -- the class doap:maintainer belongs
  // to -- so the author below can sit on the plugin itself and strict RDF
  // validation stays clean.
  std::fprintf(out, "<%s>\n    a lv2:Plugin%s%s , doap:Project ;\n    doap:name \"", uri,
               lv2Class ? " , " : "", lv2Class ? lv2Class : "");
  // Through the escaper, like the preset names always were: a name with a
  // quote in it (12" Kick) is a plausible name, and unescaped it made a file
  // no host could parse -- the plugin simply did not exist in LV2.
  writeTurtleString(out, kDesc.name);
  std::fprintf(out, "\" ;\n");
  int verMinor = 0, verMicro = 0;
  lv2VersionFromProduct(kDesc.version, &verMinor, &verMicro);
  std::fprintf(out, "    lv2:minorVersion %d ;\n    lv2:microVersion %d ;\n", verMinor,
               verMicro);
  // Licence and maintainer are emitted from what the descriptor really says --
  // the licence falls back to the vendor URL (where a seller's terms live),
  // and absent contact details stay absent rather than invented.
  const char* license = kDesc.license && kDesc.license[0] ? kDesc.license
                        : kDesc.url && kDesc.url[0]      ? kDesc.url
                                                         : nullptr;
  if (license) {
    std::fprintf(out, "    doap:license <");
    writeTurtleIri(out, license);
    std::fprintf(out, "> ;\n");
  }
  if ((kDesc.vendor && kDesc.vendor[0]) || (kDesc.url && kDesc.url[0]) ||
      (kDesc.email && kDesc.email[0])) {
    // On the plugin, not nested under an lv2:project: lilv looks at both, but
    // REAPER reads the vendor only from here -- with the author one level down
    // every Sonore LV2 was listed with no vendor while its CLAP and VST3 had one.
    std::fprintf(out, "    doap:maintainer [\n        a foaf:Person ;\n");
    if (kDesc.vendor && kDesc.vendor[0]) {
      std::fprintf(out, "        foaf:name \"");
      writeTurtleString(out, kDesc.vendor);
      std::fprintf(out, "\" ;\n");
    }
    if (kDesc.url && kDesc.url[0]) {
      std::fprintf(out, "        foaf:homepage <");
      writeTurtleIri(out, kDesc.url);
      std::fprintf(out, "> ;\n");
    }
    if (kDesc.email && kDesc.email[0]) {
      std::fprintf(out, "        foaf:mbox <mailto:");
      writeTurtleIri(out, kDesc.email);
      std::fprintf(out, "> ;\n");
    }
    std::fprintf(out, "    ] ;\n");
  }
  std::fprintf(out, "    lv2:binary <%s> ;\n", binaryName);
  std::fprintf(out, "    lv2:requiredFeature urid:map ;\n");
  // The SDK's processing contract IS hard-realtime (allocation only inside
  // prepare()), so say so -- session-restore hosts prefer plugins that do.
  std::fprintf(out, "    lv2:optionalFeature lv2:hardRTCapable ;\n");
  // The worker is how a plugin gets a thread it is allowed to read a file
  // on. OPTIONAL: a host without one runs everything here, it just cannot
  // hand a plugin a sample, and refusing to load over that would be a worse
  // trade than the missing feature.
  std::fprintf(out, "    lv2:optionalFeature work:schedule ;\n");
  // Where the output guard's one warning goes; stderr without it.
  std::fprintf(out, "    lv2:optionalFeature log:log ;\n");
  std::fprintf(out, "    lv2:extensionData state:interface , work:interface ;\n");

  std::fprintf(out, "    lv2:port [\n");
  bool first = true;
  auto separator = [&] {
    if (!first) std::fprintf(out, "    ] , [\n");
    first = false;
  };

  const std::vector<std::string> paramSymbols = lv2ParamSymbols();
  for (int i = 0; i < kDesc.numParams; ++i) {
    const ParamInfo& p = kDesc.params[i];
    separator();
    const char* symbol = paramSymbols[(size_t) i].c_str();
    std::fprintf(out,
                 "        a lv2:InputPort , lv2:ControlPort ;\n"
                 "        lv2:index %d ;\n"
                 "        lv2:symbol \"%s\" ;\n"
                 "        lv2:name \"",
                 i, symbol);
    writeTurtleString(out, p.label);
    char def[32], lo[32], hi[32];
    ttlNumber(def, sizeof(def), p.defaultValue);
    ttlNumber(lo, sizeof(lo), p.minValue);
    ttlNumber(hi, sizeof(hi), p.maxValue);
    std::fprintf(out,
                 "\" ;\n"
                 "        lv2:default %s ;\n"
                 "        lv2:minimum %s ;\n"
                 "        lv2:maximum %s ;\n",
                 def, lo, hi);
    // What the number is IN. LV2 has no call that turns a value into the
    // plugin's text, so this is all a host has to say "6000 Hz" rather than
    // "6000": the units extension's own unit where it has one, else a unit
    // declared here with the plugin's symbol. Not on a control whose steps
    // are named -- the names say what they are.
    if (p.unit && p.unit[0] && !(p.stepCount > 0 && paramValueName(p, 0) != nullptr)) {
      if (const char* known = lv2UnitFor(p.unit)) {
        std::fprintf(out, "        units:unit %s ;\n", known);
      } else {
        // units:render is a printf format: a '%' in the symbol is doubled.
        char render[128];
        size_t n = 0;
        for (const char* c = "%f "; *c; ++c) render[n++] = *c;
        for (const char* c = p.unit; *c && n + 2 < sizeof(render); ++c) {
          if (*c == '%') render[n++] = '%';
          render[n++] = *c;
        }
        render[n] = '\0';
        std::fprintf(out, "        units:unit [\n            a units:Unit ;\n            rdfs:label \"");
        writeTurtleString(out, p.unit);
        std::fprintf(out, "\" ;\n            units:symbol \"");
        writeTurtleString(out, p.unit);
        std::fprintf(out, "\" ;\n            units:render \"");
        writeTurtleString(out, render);
        std::fprintf(out, "\"\n        ] ;\n");
      }
    }
    // What the port is FOR, beyond its range.
    //
    // Only `hidden` has a standard spelling here. pprops:notOnGUI is an
    // official LV2 extension and means exactly what it says. The other two
    // do not exist in LV2:
    //
    //   automatable  Every control port in LV2 is automatable, full stop.
    //                There is no property that says otherwise, and inventing
    //                one from a host's private vocabulary would tie the
    //                bundle to that host.
    //   readOnly     LV2's honest equivalent is an OUTPUT control port, which
    //                is a different port with a different direction -- not a
    //                property on this one. Changing direction would move the
    //                parameter out of the state a host saves, so the value
    //                stays writable here and the flag is dropped.
    //
    // Both degrade to an ordinary control port, which is what they were
    // before this existed.
    if (p.hidden)
      std::fprintf(out, "        lv2:portProperty pprops:notOnGUI ;\n");
    // A HINT, which is all LV2 has. pprops:logarithmic tells a host to spread
    // the low end of the range across more of the control -- it does not
    // carry our exponent, so a host draws its own idea of logarithmic rather
    // than the exact curve. That is still enormously better than linear for a
    // frequency, and the plugin's own interface has the real curve anyway.
    if (p.skew != 1.0f && p.skew > 0.0f && p.skew < 1.0f)
      std::fprintf(out, "        lv2:portProperty pprops:logarithmic ;\n");
    if (p.stepCount > 0) {
      // integer only when every step IS a whole number: the property is a
      // promise to the host, and a control stepping 0, 0.5, 1 cannot keep it.
      // enumeration ON TOP of it when the steps are named: integer says
      // "whole numbers only", enumeration says "and only these ones", which
      // is what makes a host draw a dropdown rather than a stepped knob.
      const bool integers = stepsAreIntegers(p);
      const bool named = paramValueName(p, 0) != nullptr;
      if (integers && named)
        std::fprintf(out, "        lv2:portProperty lv2:integer , lv2:enumeration ;\n");
      else if (integers)
        std::fprintf(out, "        lv2:portProperty lv2:integer ;\n");
      else if (named)
        std::fprintf(out, "        lv2:portProperty lv2:enumeration ;\n");

      // The names themselves. Without them an LV2 host shows the index, the
      // same way CLAP and VST3 did before they were given the table -- a
      // filter-type switch reading "2" in an automation lane, which is the
      // one place a user works on a control they cannot see.
      for (int v = 0; v < p.stepCount; ++v) {
        const char* name = paramValueName(p, v);
        if (!name) continue;
        std::fprintf(out, "        lv2:scalePoint [\n            rdfs:label \"");
        writeTurtleString(out, name);
        // The step's VALUE, not its index: a control declared 2..8 in four
        // steps names 2, 4, 6 and 8, and a host that jumped to "index 2"
        // would land between two of them.
        char value[32];
        ttlNumber(value, sizeof(value), stepValueOf(p, v));
        std::fprintf(out, "\" ;\n            rdf:value %s\n        ] ;\n", value);
      }
    }
    // LV2 groups ports by pointing them at a pg:Group resource, declared
    // below the plugin. Hosts that know port-groups build a tree from it.
    const GroupTable groups = collectGroups(kDesc.params, kDesc.numParams);
    const int g = groups.indexOf(p.group);
    if (g >= 0) std::fprintf(out, "        pg:group <%s#group%d> ;\n", uri, g);
  }

  if (!isInstrument()) {
    // Host bypass: hosts that know lv2:enabled (Ardour, MOD) wire their
    // bypass button to it; everyone else sees an optional control they may
    // ignore, which is why it is connectionOptional.
    separator();
    std::fprintf(out,
                 "        a lv2:InputPort , lv2:ControlPort ;\n"
                 "        lv2:index %d ;\n"
                 "        lv2:symbol \"enabled\" ;\n"
                 "        lv2:name \"Enabled\" ;\n"
                 "        lv2:default 1 ;\n"
                 "        lv2:minimum 0 ;\n"
                 "        lv2:maximum 1 ;\n"
                 "        lv2:portProperty lv2:integer , lv2:toggled , lv2:connectionOptional ;\n"
                 "        lv2:designation lv2:enabled ;\n",
                 portEnabled());
  }

  {
    separator();
    std::fprintf(out,
                 "        a lv2:OutputPort , lv2:ControlPort ;\n"
                 "        lv2:index %d ;\n"
                 "        lv2:symbol \"meterPeak\" ;\n"
                 "        lv2:name \"Peak\" ;\n"
                 "        lv2:minimum 0.0 ;\n"
                 "        lv2:maximum 4.0 ;\n"
                 "        lv2:default 0.0 ;\n"
                 "        lv2:portProperty lv2:connectionOptional ;\n",
                 portMeterPeak());
    separator();
    std::fprintf(out,
                 "        a lv2:OutputPort , lv2:ControlPort ;\n"
                 "        lv2:index %d ;\n"
                 "        lv2:symbol \"meterRms\" ;\n"
                 "        lv2:name \"RMS\" ;\n"
                 "        lv2:minimum 0.0 ;\n"
                 "        lv2:maximum 4.0 ;\n"
                 "        lv2:default 0.0 ;\n"
                 "        lv2:portProperty lv2:connectionOptional ;\n",
                 portMeterRms());
  }
  // The interface's two atom ports. An interface that is not running leaves
  // them unconnected, which is why both are connectionOptional: a host with
  // no UI must not be made to allocate atom buffers for nobody.
  {
    separator();
    std::fprintf(out,
                 "        a lv2:InputPort , atom:AtomPort ;\n"
                 "        atom:bufferType atom:Sequence ;\n"
                 "        lv2:index %d ;\n"
                 "        lv2:symbol \"uiControl\" ;\n"
                 "        lv2:name \"UI Control\" ;\n"
                 "        lv2:portProperty lv2:connectionOptional ;\n",
                 portUiControl());
    separator();
    std::fprintf(out,
                 "        a lv2:OutputPort , atom:AtomPort ;\n"
                 "        atom:bufferType atom:Sequence ;\n"
                 "        lv2:index %d ;\n"
                 "        lv2:symbol \"uiNotify\" ;\n"
                 "        lv2:name \"UI Notify\" ;\n"
                 "        lv2:portProperty lv2:connectionOptional ;\n"
                 // The state goes to the interface as one JSON message, never
                 // in pieces: one bigger than the buffer the host gave waits,
                 // and with no size asked for a host picks its own -- 4 KB in
                 // jalv, 8 KB in Ardour -- and a plugin with enough
                 // parameters never reached its own page. Hosts that know
                 // resize-port allocate at least this much.
                 "        rsz:minimumSize %u ;\n",
                 portUiNotify(), (unsigned) kUiNotifyMinimumBytes);
  }
  if (timePorts()) {
    separator();
    std::fprintf(out,
                 "        a lv2:InputPort , atom:AtomPort ;\n"
                 "        atom:bufferType atom:Sequence ;\n"
                 "        atom:supports time:Position ;\n"
                 "        lv2:index %d ;\n"
                 "        lv2:symbol \"time_in\" ;\n"
                 "        lv2:name \"Transport\" ;\n"
                 "        lv2:portProperty lv2:connectionOptional ;\n",
                 portTimeIn());
  }
  if (grPorts()) {
    separator();
    std::fprintf(out,
                 "        a lv2:OutputPort , lv2:ControlPort ;\n"
                 "        lv2:index %d ;\n"
                 "        lv2:symbol \"meterGr\" ;\n"
                 "        lv2:name \"Gain Reduction\" ;\n"
                 "        lv2:minimum 0.0 ;\n"
                 "        lv2:maximum 60.0 ;\n"
                 "        lv2:default 0.0 ;\n"
                 "        lv2:portProperty lv2:connectionOptional ;\n",
                 portMeterGr());
  }
  if (latencyPorts()) {
    separator();
    std::fprintf(out,
                 "        a lv2:OutputPort , lv2:ControlPort ;\n"
                 "        lv2:index %d ;\n"
                 "        lv2:symbol \"latency\" ;\n"
                 "        lv2:name \"Latency\" ;\n"
                 "        lv2:minimum 0 ;\n"
                 "        lv2:maximum 192000 ;\n"
                 "        lv2:portProperty lv2:reportsLatency , lv2:integer , "
                 "lv2:connectionOptional ;\n"
                 "        lv2:designation lv2:latency ;\n",
                 portLatency());
  }

  {
    // The host's switch, not the user's: notOnGUI and notAutomatic keep it out
    // of a generic editor and its automation lanes, where REAPER showed it as
    // a control anyone could move -- and automate -- mid-song.
    separator();
    std::fprintf(out,
                 "        a lv2:InputPort , lv2:ControlPort ;\n"
                 "        lv2:index %d ;\n"
                 "        lv2:symbol \"freewheel\" ;\n"
                 "        lv2:name \"Freewheel\" ;\n"
                 "        lv2:default 0 ;\n"
                 "        lv2:minimum 0 ;\n"
                 "        lv2:maximum 1 ;\n"
                 "        lv2:portProperty lv2:integer , lv2:toggled , lv2:connectionOptional ,\n"
                 "            pprops:notOnGUI , pprops:notAutomatic ;\n"
                 "        lv2:designation lv2:freeWheeling ;\n",
                 portFreeWheel());
  }

  if (midiPorts()) {
    separator();
    std::fprintf(out,
                 "        a lv2:InputPort , atom:AtomPort ;\n"
                 "        lv2:index %d ;\n"
                 "        lv2:symbol \"midi_in\" ;\n"
                 "        lv2:name \"MIDI In\" ;\n"
                 "        atom:bufferType atom:Sequence ;\n"
                 "        atom:supports midi:MidiEvent ;\n",
                 portMidiIn());
  }
  if (!isInstrument()) {
    for (int c = 0; c < audioInPorts(); ++c) {
      separator();
      std::fprintf(out,
                   "        a lv2:InputPort , lv2:AudioPort ;\n"
                   "        lv2:index %d ;\n"
                   "        lv2:symbol \"in_%s\" ;\n"
                   "        lv2:name \"In %s\" ;\n",
                   portAudioInL() + c, audioPortSymbol(c), audioPortName(c));
    }
  }

  if (scPorts()) {
    for (int c = 0; c < 2; ++c) {
      separator();
      std::fprintf(out,
                   "        a lv2:InputPort , lv2:AudioPort ;\n"
                   "        lv2:index %d ;\n"
                   "        lv2:symbol \"sc_%c\" ;\n"
                   "        lv2:name \"Sidechain %c\" ;\n"
                   "        lv2:portProperty lv2:isSideChain , lv2:connectionOptional ;\n",
                   portScInL() + c, c ? 'r' : 'l', c ? 'R' : 'L');
    }
  }
  for (int c = 0; c < lv2Width(); ++c) {
    separator();
    std::fprintf(out,
                 "        a lv2:OutputPort , lv2:AudioPort ;\n"
                 "        lv2:index %d ;\n"
                 "        lv2:symbol \"out_%s\" ;\n"
                 "        lv2:name \"Out %s\" ;\n",
                 portAudioOutL() + c, audioPortSymbol(c), audioPortName(c));
  }

  if (midiOutPorts()) {
    separator();
    std::fprintf(out,
                 "        a lv2:OutputPort , atom:AtomPort ;\n"
                 "        lv2:index %d ;\n"
                 "        lv2:symbol \"midi_out\" ;\n"
                 "        lv2:name \"MIDI Out\" ;\n"
                 "        atom:bufferType atom:Sequence ;\n"
                 "        atom:supports midi:MidiEvent ;\n",
                 portMidiOut());
  }

  // Aux output buses, flattened: LV2 hosts route ports, not buses, so each
  // aux channel is a port named after its bus.
  {
    int flat = 0;
    for (uint32_t b = 0; b < clapwrap::numAuxOutputs(); ++b) {
      const uint32_t width = clapwrap::auxBusChannels(b);
      for (uint32_t c = 0; c < width; ++c, ++flat) {
        separator();
        char symbol[64], label[80];
        std::snprintf(symbol, sizeof(symbol), "aux%u_%u", (unsigned) b, (unsigned) c);
        std::snprintf(label, sizeof(label), "%s %u", kDesc.auxOutputs[b].name,
                      (unsigned) (c + 1));
        std::fprintf(out,
                     "        a lv2:OutputPort , lv2:AudioPort ;\n"
                     "        lv2:index %d ;\n"
                     "        lv2:symbol \"%s\" ;\n"
                     "        lv2:name \"",
                     portAuxOut0() + flat, symbol);
        // Escaped, as every other name in this file: a bus called Stem "A"
        // closed the literal early, and a bundle with one line of broken
        // Turtle is a bundle a host skips whole.
        writeTurtleString(out, label);
        std::fprintf(out, "\" ;\n");
      }
    }
  }
  std::fprintf(out, "    ] .\n");

  // The group resources the ports referenced. A dangling pg:group would make
  // a port-group-aware host show an unnamed box.
  //
  // lv2:symbol is REQUIRED on a group and was missing. sord_validate said so
  // every run -- "has 0 != 1 values" -- and the gate walked past it, because
  // sord_validate reports the error and then exits zero. The gate now greps
  // its output instead of trusting that status.
  {
    const GroupTable groups = collectGroups(kDesc.params, kDesc.numParams);
    for (int g = 0; g < groups.count; ++g) {
      // A symbol is not a name: it must match [A-Za-z_][A-Za-z0-9_]*, so a
      // group called "Low/Mid" or "3-Band" cannot use its own label. Anything
      // outside the set becomes an underscore, a leading digit gets one in
      // front, and a label with nothing usable in it falls back to the index
      // -- which is unique by construction and always valid.
      char symbol[64];
      int n = 0;
      const char* name = groups.names[g] ? groups.names[g] : "";
      for (int i = 0; name[i] && n < (int) sizeof(symbol) - 1; ++i) {
        const char c = name[i];
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_';
        if (!ok) {
          // One underscore per run of junk, and never a trailing one.
          if (n > 0 && symbol[n - 1] != '_') symbol[n++] = '_';
          continue;
        }
        if (n == 0 && c >= '0' && c <= '9') symbol[n++] = '_';
        symbol[n++] = c;
      }
      while (n > 0 && symbol[n - 1] == '_') --n;
      symbol[n] = 0;
      if (n == 0) std::snprintf(symbol, sizeof(symbol), "group%d", g);

      // The name through the escaper, twice: a group called Amp "Clean" or
      // In\Out was written raw, and its quote or backslash made the whole
      // plugin.ttl unreadable -- the plugin did not exist in LV2.
      std::fprintf(out, "\n<%s#group%d>\n    a pg:Group ;\n    lv2:name \"", uri, g);
      writeTurtleString(out, name);
      std::fprintf(out, "\" ;\n    lv2:symbol \"%s\" ;\n    rdfs:label \"", symbol);
      writeTurtleString(out, name);
      std::fprintf(out, "\" .\n");
    }
  }
}

inline int ttlGeneratorMain(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: %s <bundle-dir> <binary-name>\n", argv[0]);
    return 2;
  }
  // Turtle numbers use a full stop, always. fprintf's %g follows the process
  // locale, and a machine set to a comma-decimal one would write "lv2:default
  // 0,5" -- which is not a number, and which a host reports as a broken
  // bundle rather than as a locale problem. The rest of this SDK is careful
  // about this (see jsNumber); the generator has to be too.
  std::setlocale(LC_NUMERIC, "C");

  const char* dir = argv[1];
  const char* binary = argv[2];
  char uri[256], path[1024];
  pluginUri(uri, sizeof(uri));

  std::snprintf(path, sizeof(path), "%s/manifest.ttl", dir);
  std::FILE* manifest = ::sonore::pathio::open(path, "w");
  if (!manifest) {
    std::fprintf(stderr, "cannot write %s\n", path);
    return 1;
  }
  std::fprintf(manifest,
               "@prefix lv2:  <http://lv2plug.in/ns/lv2core#> .\n"
               "@prefix pset: <http://lv2plug.in/ns/ext/presets#> .\n"
               "@prefix rdfs: <http://www.w3.org/2000/01/rdf-schema#> .\n"
               "@prefix ui:   <http://lv2plug.in/ns/extensions/ui#> .\n\n"
               // portNotification names atom:eventTransfer, so the manifest
               // needs the atom prefix too -- a turtle file using one it never
               // declared is not a turtle file, and lv2lint rejects the whole
               // bundle rather than the line.
               "@prefix atom: <http://lv2plug.in/ns/ext/atom#> .\n"
               "<%s>\n    a lv2:Plugin ;\n    lv2:binary <%s> ;\n    rdfs:seeAlso <plugin.ttl> .\n",
               uri, binary);

  // The interface.
  //
  // Without this an LV2 host draws a column of generic sliders and the page
  // the seller designed never appears -- while the same source shows it in
  // CLAP, in VST3 and in AU.
  //
  // Declared only for the platform this bundle was BUILT for. A ui:X11UI
  // announced on a Windows binary is a promise the file cannot keep, and a
  // host that believes it opens nothing and blames the plugin.
#if defined(SONORE_LV2_UI)
  {
    const char* uiType = "ui:WindowsUI";
    // portNotification is what makes a host FORWARD the notify port to the
    // interface. Without it the plugin writes atoms nobody reads: the port
    // exists, the host runs it, and the UI is simply never told. That is the
    // whole difference between this working and looking like it should.
    std::fprintf(manifest,
                 "\n<%s#ui>\n"
                 "    a %s ;\n"
                 "    ui:binary <%s> ;\n"
                 "    lv2:extensionData ui:idleInterface , ui:resize ;\n"
                 "    lv2:requiredFeature ui:idleInterface , ui:parent ;\n"
                 "    ui:portNotification [\n"
                 "        ui:plugin <%s> ;\n"
                 "        lv2:symbol \"uiNotify\" ;\n"
                 "        ui:protocol atom:eventTransfer\n"
                 "    ] .\n"
                 "\n<%s>\n    ui:ui <%s#ui> .\n",
                 uri, uiType, binary, uri, uri, uri);
  }
#endif
  // Presets are announced in the MANIFEST and defined in presets.ttl. That
  // split is the convention for a reason: a host scanning a folder of a
  // hundred plugins reads every manifest and only opens the definitions when
  // a user actually goes looking. Putting the values in the manifest would
  // make every scan read all of them.
  for (int i = 0; i < numFactoryPresets(); ++i) {
    std::fprintf(manifest,
                 "\n<%s#preset%d>\n"
                 "    a pset:Preset ;\n"
                 "    lv2:appliesTo <%s> ;\n"
                 "    rdfs:label \"",
                 uri, i, uri);
    writeTurtleString(manifest, kDesc.presets[i].name);
    std::fprintf(manifest, "\" ;\n    rdfs:seeAlso <presets.ttl> .\n");
  }
  std::fclose(manifest);

  if (numFactoryPresets() > 0) {
    std::snprintf(path, sizeof(path), "%s/presets.ttl", dir);
    std::FILE* presets = ::sonore::pathio::open(path, "w");
    if (!presets) {
      std::fprintf(stderr, "cannot write %s\n", path);
      return 1;
    }
    writePresetsTtl(presets, uri);
    std::fclose(presets);
  }

  std::snprintf(path, sizeof(path), "%s/plugin.ttl", dir);
  std::FILE* plugin = ::sonore::pathio::open(path, "w");
  if (!plugin) {
    std::fprintf(stderr, "cannot write %s\n", path);
    return 1;
  }
  writeTtl(plugin, binary);
  std::fclose(plugin);
  std::printf("wrote %s/manifest.ttl and plugin.ttl for <%s>\n", dir, uri);
  return 0;
}

} // namespace lv2
} // namespace sonore

#if defined(SONORE_LV2_UI)
/** The interface entry point, beside the plugin's own.
 *
 *  Shipped in the SAME binary. LV2 allows a separate one and many plugins use
 *  that so a headless host never loads a GUI toolkit -- but this source
 *  already links the WebView for its CLAP and VST3 builds, so a second binary
 *  would duplicate it rather than avoid it. */
LV2_SYMBOL_EXPORT const sonore::lv2ui::Lv2UiDescriptor* lv2ui_descriptor(
    uint32_t index) {
  return sonore::lv2ui::descriptor(index);
}
#endif

#if defined(SONORE_LV2_TTLGEN)
int main(int argc, char** argv) { return sonore::lv2::ttlGeneratorMain(argc, argv); }
#else
/** The host's entry point. */
LV2_SYMBOL_EXPORT const LV2_Descriptor* lv2_descriptor(uint32_t index) {
  return index == 0 ? sonore::lv2::descriptor() : nullptr;
}

/** The generator as a plain function, for a plugin compiled ONCE into an
 *  object that every format links (sonore_add_plugin): the ttlgen executable
 *  is then cmake/lv2_ttlgen_main.cpp -- a main() and nothing else -- linked
 *  against that object, instead of a second full compile of the plugin that
 *  existed to print two text files. Hidden, because a host must never see it
 *  in the module; not inline, because an object nobody calls it from would
 *  not contain it and the link would fail on the one symbol it needs. */
#if defined(_MSC_VER)
extern "C" int sonore_lv2_ttlgen_main(int argc, char** argv) {
#else
extern "C" __attribute__((visibility("hidden"))) int sonore_lv2_ttlgen_main(int argc, char** argv) {
#endif
  return sonore::lv2::ttlGeneratorMain(argc, argv);
}
#endif
