// SPDX-License-Identifier: Apache-2.0
// Sonore SDK test fixture: a CLAP plugin that notices flush() beside process().
//
// clap_plugin_params::flush is "[active ? audio-thread : main-thread]", and
// whatever thread calls it, never at the same time as process(). sonore/
// host.h flushed from drainParameterEdits() on the caller's main thread
// whenever the plugin had asked, active or not -- so with audio running, a
// plugin asking for a flush got one on the UI thread in the middle of a block.
//
// Written against the raw API so it can watch for exactly that. process()
// takes a few hundred microseconds, asks for a flush every block (as a plugin
// whose own editor keeps changing things does), and flush() counts every
// call that finds process() still inside. The count is parameter 1, read-only,
// which the test reads back through the host.
//
// A second plugin, com.sonorie.test.flush-early, is the same but for one
// thing: it asks for a flush in init(), before the host has prepared it, as a
// plugin does whose state moved while it was being set up.

#include <clap/clap.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>

namespace {

struct Fixture {
  clap_plugin_t plugin{};
  const clap_host_t* host = nullptr;
  std::atomic<int> inProcess{0};
  std::atomic<int> overlaps{0};
  std::atomic<int> flushes{0};
  std::atomic<double> value{0.0};
  // The notes plugin's counts.
  std::atomic<int> notesOn{0}, notesOff{0}, midiEvents{0};
};

Fixture* self(const clap_plugin_t* p) { return static_cast<Fixture*>(p->plugin_data); }

void takeEvents(Fixture* f, const clap_input_events_t* in) {
  if (!in) return;
  const uint32_t n = in->size(in);
  for (uint32_t i = 0; i < n; ++i) {
    const clap_event_header_t* h = in->get(in, i);
    if (h->space_id != CLAP_CORE_EVENT_SPACE_ID) continue;
    if (h->type == CLAP_EVENT_NOTE_ON) f->notesOn.fetch_add(1);
    if (h->type == CLAP_EVENT_NOTE_OFF) f->notesOff.fetch_add(1);
    if (h->type == CLAP_EVENT_MIDI) f->midiEvents.fetch_add(1);
    if (h->type != CLAP_EVENT_PARAM_VALUE) continue;
    const auto* e = reinterpret_cast<const clap_event_param_value_t*>(h);
    if (e->param_id == 0) f->value.store(e->value);
  }
}

void askForFlush(Fixture* f) {
  const auto* params = static_cast<const clap_host_params_t*>(
      f->host->get_extension(f->host, CLAP_EXT_PARAMS));
  if (params && params->request_flush) params->request_flush(f->host);
}

uint32_t paramsCount(const clap_plugin_t*) { return 2; }
bool paramsGetInfo(const clap_plugin_t*, uint32_t index, clap_param_info_t* info) {
  if (index > 1) return false;
  std::memset(info, 0, sizeof(*info));
  info->id = index;
  info->flags = index == 0 ? CLAP_PARAM_IS_AUTOMATABLE : CLAP_PARAM_IS_READONLY;
  std::snprintf(info->name, sizeof(info->name), "%s", index == 0 ? "Value" : "Overlaps");
  info->min_value = 0.0;
  info->max_value = index == 0 ? 1.0 : 1e9;
  info->default_value = 0.0;
  return true;
}
bool paramsGetValue(const clap_plugin_t* p, clap_id id, double* out) {
  if (id == 0) *out = self(p)->value.load();
  else if (id == 1) *out = (double) self(p)->overlaps.load();
  else return false;
  return true;
}
bool paramsValueToText(const clap_plugin_t*, clap_id, double v, char* out, uint32_t size) {
  std::snprintf(out, size, "%.3f", v);
  return true;
}
bool paramsTextToValue(const clap_plugin_t*, clap_id, const char*, double*) { return false; }
void paramsFlush(const clap_plugin_t* p, const clap_input_events_t* in, const clap_output_events_t*) {
  Fixture* f = self(p);
  if (f->inProcess.load() > 0) f->overlaps.fetch_add(1);
  f->flushes.fetch_add(1);
  takeEvents(f, in);
}
const clap_plugin_params_t kParams = {paramsCount,       paramsGetInfo,     paramsGetValue,
                                      paramsValueToText, paramsTextToValue, paramsFlush};

// The third plugin, com.sonorie.test.clap-notes: a note port that speaks
// ONLY CLAP's dialect, and three read-only parameters counting what arrived
// -- note-ons, note-offs, MIDI events. A host must not send it MIDI.
uint32_t noteParamsCount(const clap_plugin_t*) { return 3; }
bool noteParamsGetInfo(const clap_plugin_t*, uint32_t index, clap_param_info_t* info) {
  if (index > 2) return false;
  std::memset(info, 0, sizeof(*info));
  info->id = index;
  info->flags = CLAP_PARAM_IS_READONLY;
  static const char* const kNames[] = {"Notes on", "Notes off", "MIDI events"};
  std::snprintf(info->name, sizeof(info->name), "%s", kNames[index]);
  info->max_value = 1e9;
  return true;
}
bool noteParamsGetValue(const clap_plugin_t* p, clap_id id, double* out) {
  const Fixture* f = self(p);
  if (id == 0) *out = (double) f->notesOn.load();
  else if (id == 1) *out = (double) f->notesOff.load();
  else if (id == 2) *out = (double) f->midiEvents.load();
  else return false;
  return true;
}
void noteParamsFlush(const clap_plugin_t*, const clap_input_events_t*, const clap_output_events_t*) {}
const clap_plugin_params_t kNoteParams = {noteParamsCount,   noteParamsGetInfo, noteParamsGetValue,
                                          paramsValueToText, paramsTextToValue, noteParamsFlush};
uint32_t notePortsCount(const clap_plugin_t*, bool isInput) { return isInput ? 1 : 0; }
bool notePortsGet(const clap_plugin_t*, uint32_t index, bool isInput, clap_note_port_info_t* info) {
  if (!isInput || index != 0) return false;
  std::memset(info, 0, sizeof(*info));
  info->id = 0;
  info->supported_dialects = CLAP_NOTE_DIALECT_CLAP;
  info->preferred_dialect = CLAP_NOTE_DIALECT_CLAP;
  std::snprintf(info->name, sizeof(info->name), "Notes");
  return true;
}
const clap_plugin_note_ports_t kNotePorts = {notePortsCount, notePortsGet};

const char* const kFeatures[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, nullptr};
const char* const kInstrumentFeatures[] = {CLAP_PLUGIN_FEATURE_INSTRUMENT, nullptr};
const clap_plugin_descriptor_t kDescriptor = {
    CLAP_VERSION_INIT,
    "com.sonorie.test.flush",
    "Sonore Flush Fixture",
    "Sonorie",
    "https://sonorie.com",
    "",
    "",
    "1.0.0",
    "Counts every flush() that arrives while process() is running.",
    kFeatures,
};
const clap_plugin_descriptor_t kEarlyDescriptor = {
    CLAP_VERSION_INIT,
    "com.sonorie.test.flush-early",
    "Sonore Flush Fixture (asks in init)",
    "Sonorie",
    "https://sonorie.com",
    "",
    "",
    "1.0.0",
    "Asks for a flush before it is ever activated.",
    kFeatures,
};

const clap_plugin_descriptor_t kNotesDescriptor = {
    CLAP_VERSION_INIT,
    "com.sonorie.test.clap-notes",
    "Sonore CLAP-dialect Notes Fixture",
    "Sonorie",
    "https://sonorie.com",
    "",
    "",
    "1.0.0",
    "A note port that speaks only CLAP's note dialect.",
    kInstrumentFeatures,
};

bool pluginInit(const clap_plugin_t*) { return true; }
bool pluginInitEarly(const clap_plugin_t* p) {
  askForFlush(self(p));
  return true;
}
void pluginDestroy(const clap_plugin_t* p) { delete self(p); }
bool pluginActivate(const clap_plugin_t*, double, uint32_t, uint32_t) { return true; }
void pluginDeactivate(const clap_plugin_t*) {}
bool pluginStartProcessing(const clap_plugin_t*) { return true; }
void pluginStopProcessing(const clap_plugin_t*) {}
void pluginReset(const clap_plugin_t*) {}
clap_process_status pluginProcess(const clap_plugin_t* p, const clap_process_t* process) {
  Fixture* f = self(p);
  f->inProcess.fetch_add(1);
  takeEvents(f, process->in_events);
  // A block that takes real time, so a flush arriving from another thread
  // has somewhere to land.
  const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(300);
  while (std::chrono::steady_clock::now() < until) {
  }
  askForFlush(f);
  f->inProcess.fetch_sub(1);
  return CLAP_PROCESS_CONTINUE;
}
const void* pluginGetExtension(const clap_plugin_t*, const char* id) {
  return std::strcmp(id, CLAP_EXT_PARAMS) == 0 ? &kParams : nullptr;
}
clap_process_status notesProcess(const clap_plugin_t* p, const clap_process_t* process) {
  takeEvents(self(p), process->in_events);
  for (uint32_t o = 0; o < process->audio_outputs_count; ++o)
    for (uint32_t c = 0; c < process->audio_outputs[o].channel_count; ++c)
      if (process->audio_outputs[o].data32 && process->audio_outputs[o].data32[c])
        std::memset(process->audio_outputs[o].data32[c], 0, process->frames_count * sizeof(float));
  return CLAP_PROCESS_CONTINUE;
}
const void* notesGetExtension(const clap_plugin_t*, const char* id) {
  if (std::strcmp(id, CLAP_EXT_PARAMS) == 0) return &kNoteParams;
  if (std::strcmp(id, CLAP_EXT_NOTE_PORTS) == 0) return &kNotePorts;
  return nullptr;
}
void pluginOnMainThread(const clap_plugin_t*) {}

uint32_t factoryCount(const clap_plugin_factory_t*) { return 3; }
const clap_plugin_descriptor_t* factoryDescriptor(const clap_plugin_factory_t*, uint32_t index) {
  return index == 0 ? &kDescriptor
                    : (index == 1 ? &kEarlyDescriptor : (index == 2 ? &kNotesDescriptor : nullptr));
}
const clap_plugin_t* factoryCreate(const clap_plugin_factory_t*, const clap_host_t* host,
                                   const char* id) {
  const bool early = id && std::strcmp(id, kEarlyDescriptor.id) == 0;
  const bool notes = id && std::strcmp(id, kNotesDescriptor.id) == 0;
  if (!id || (!early && !notes && std::strcmp(id, kDescriptor.id) != 0)) return nullptr;
  auto* f = new Fixture();
  f->host = host;
  f->plugin.desc = notes ? &kNotesDescriptor : (early ? &kEarlyDescriptor : &kDescriptor);
  f->plugin.plugin_data = f;
  f->plugin.init = early ? pluginInitEarly : pluginInit;
  f->plugin.destroy = pluginDestroy;
  f->plugin.activate = pluginActivate;
  f->plugin.deactivate = pluginDeactivate;
  f->plugin.start_processing = pluginStartProcessing;
  f->plugin.stop_processing = pluginStopProcessing;
  f->plugin.reset = pluginReset;
  f->plugin.process = notes ? notesProcess : pluginProcess;
  f->plugin.get_extension = notes ? notesGetExtension : pluginGetExtension;
  f->plugin.on_main_thread = pluginOnMainThread;
  return &f->plugin;
}
const clap_plugin_factory_t kFactory = {factoryCount, factoryDescriptor, factoryCreate};

bool entryInit(const char*) { return true; }
void entryDeinit() {}
const void* entryGetFactory(const char* id) {
  return std::strcmp(id, CLAP_PLUGIN_FACTORY_ID) == 0 ? &kFactory : nullptr;
}

} // namespace

extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry = {
    CLAP_VERSION_INIT,
    entryInit,
    entryDeinit,
    entryGetFactory,
};
