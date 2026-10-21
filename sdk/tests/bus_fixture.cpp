// SPDX-License-Identifier: Apache-2.0
// Sonore SDK test fixture: CLAP plugins whose audio ports are not "one
// stereo bus each way".
//
// clap_process_t's audio buffers "must have the same count as specified by
// clap_plugin_audio_ports->count()", each as wide as its port says. A plugin
// is entitled to rely on that -- a compressor reads its sidechain from
// audio_inputs[1] without checking the count first -- and sonore/host.h gave
// every plugin one input and one output, as wide as the CALLER asked: a
// sidechain read past the end of the host's array, and a stereo plugin hosted
// for a mono device read a second channel that was not there.
//
// Written against the raw API so it can check instead of crashing. Each
// plugin compares what process() was handed with what it declared, counts
// every block that does not match in read-only parameter 0, and leaves such
// a block untouched. A block that matches comes out at twice its input on
// every main channel, and the sidechain is added in, so the test can see the
// routing as well as the shape.
//
// Each also keeps CLAP's thread rules for start_processing() and reset(),
// which are the audio thread's: the next process() counts in parameter 1 a
// call that came from any other thread. Parameter 2 counts resets.
//
//   sidechain   in: main stereo + aux mono   out: main stereo + aux mono
//   stereo      in: main stereo              out: main stereo
//   mono        in: main mono                out: main mono
//   reluctant   stereo each way, and refuses start_processing() twice

#include <clap/clap.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>

namespace {

struct Port {
  const char* name;
  uint32_t channels;
  bool main;
};

struct Variant {
  clap_plugin_descriptor_t descriptor;
  Port inputs[2];
  uint32_t inputCount;
  Port outputs[2];
  uint32_t outputCount;
  int refuseStarts = 0; // how many start_processing() calls fail first
};

const char* const kFeatures[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, nullptr};
const Variant kVariants[] = {
    {{CLAP_VERSION_INIT, "com.sonorie.test.bus.sidechain", "Sonore Bus Fixture (sidechain)",
      "Sonorie", "https://sonorie.com", "", "", "1.0.0",
      "Stereo main ports plus a mono sidechain in and a mono aux out.", kFeatures},
     {{"Main", 2, true}, {"Sidechain", 1, false}},
     2,
     {{"Main", 2, true}, {"Aux", 1, false}},
     2},
    {{CLAP_VERSION_INIT, "com.sonorie.test.bus.stereo", "Sonore Bus Fixture (stereo)", "Sonorie",
      "https://sonorie.com", "", "", "1.0.0", "One stereo port each way, and nothing else.",
      kFeatures},
     {{"Main", 2, true}, {}},
     1,
     {{"Main", 2, true}, {}},
     1},
    {{CLAP_VERSION_INIT, "com.sonorie.test.bus.mono", "Sonore Bus Fixture (mono)", "Sonorie",
      "https://sonorie.com", "", "", "1.0.0", "One mono port each way, and nothing else.",
      kFeatures},
     {{"Main", 1, true}, {}},
     1,
     {{"Main", 1, true}, {}},
     1},
    {{CLAP_VERSION_INIT, "com.sonorie.test.bus.reluctant", "Sonore Bus Fixture (reluctant)",
      "Sonorie", "https://sonorie.com", "", "", "1.0.0",
      "Stereo each way; start_processing() fails twice before it succeeds.", kFeatures},
     {{"Main", 2, true}, {}},
     1,
     {{"Main", 2, true}, {}},
     1,
     2},
};

struct Fixture {
  clap_plugin_t plugin{};
  const clap_host_t* host = nullptr;
  const Variant* variant = nullptr;
  std::atomic<int> violations{0};
  std::atomic<int> wrongThread{0};
  std::atomic<int> resets{0};
  int refusalsLeft = 0;
  // Where the last start_processing() and reset() ran, checked by the next
  // process(). Written before the flag is raised, read after it is lowered.
  std::thread::id startedOn, resetOn;
  std::atomic<bool> checkStart{false}, checkReset{false};
};

Fixture* self(const clap_plugin_t* p) { return static_cast<Fixture*>(p->plugin_data); }

uint32_t portsCount(const clap_plugin_t* p, bool isInput) {
  return isInput ? self(p)->variant->inputCount : self(p)->variant->outputCount;
}
bool portsGet(const clap_plugin_t* p, uint32_t index, bool isInput, clap_audio_port_info_t* info) {
  const Variant* v = self(p)->variant;
  if (index >= (isInput ? v->inputCount : v->outputCount)) return false;
  const Port& port = isInput ? v->inputs[index] : v->outputs[index];
  std::memset(info, 0, sizeof(*info));
  info->id = index;
  std::snprintf(info->name, sizeof(info->name), "%s", port.name);
  info->flags = port.main ? CLAP_AUDIO_PORT_IS_MAIN : 0;
  info->channel_count = port.channels;
  info->port_type = port.channels == 1 ? CLAP_PORT_MONO : port.channels == 2 ? CLAP_PORT_STEREO
                                                                             : nullptr;
  info->in_place_pair = CLAP_INVALID_ID;
  return true;
}
const clap_plugin_audio_ports_t kPorts = {portsCount, portsGet};

const char* const kParamNames[] = {"Violations", "Wrong thread", "Resets"};
uint32_t paramsCount(const clap_plugin_t*) { return 3; }
bool paramsGetInfo(const clap_plugin_t*, uint32_t index, clap_param_info_t* info) {
  if (index >= 3) return false;
  std::memset(info, 0, sizeof(*info));
  info->id = index;
  info->flags = CLAP_PARAM_IS_READONLY;
  std::snprintf(info->name, sizeof(info->name), "%s", kParamNames[index]);
  info->max_value = 1e9;
  return true;
}
bool paramsGetValue(const clap_plugin_t* p, clap_id id, double* out) {
  const Fixture* f = self(p);
  if (id == 0) *out = (double) f->violations.load();
  else if (id == 1) *out = (double) f->wrongThread.load();
  else if (id == 2) *out = (double) f->resets.load();
  else return false;
  return true;
}
bool paramsValueToText(const clap_plugin_t*, clap_id, double v, char* out, uint32_t size) {
  std::snprintf(out, size, "%.0f", v);
  return true;
}
bool paramsTextToValue(const clap_plugin_t*, clap_id, const char*, double*) { return false; }
void paramsFlush(const clap_plugin_t*, const clap_input_events_t*, const clap_output_events_t*) {}
const clap_plugin_params_t kParams = {paramsCount,       paramsGetInfo,     paramsGetValue,
                                      paramsValueToText, paramsTextToValue, paramsFlush};

/** What the host handed over, against what was declared. */
bool shapeMatches(const clap_audio_buffer_t* buffers, uint32_t count, const Port* declared,
                  uint32_t declaredCount) {
  if (count != declaredCount || (count > 0 && !buffers)) return false;
  for (uint32_t i = 0; i < count; ++i) {
    if (buffers[i].channel_count != declared[i].channels || !buffers[i].data32) return false;
    for (uint32_t c = 0; c < declared[i].channels; ++c)
      if (!buffers[i].data32[c]) return false;
  }
  return true;
}

bool pluginInit(const clap_plugin_t*) { return true; }
void pluginDestroy(const clap_plugin_t* p) { delete self(p); }
bool pluginActivate(const clap_plugin_t*, double, uint32_t, uint32_t) { return true; }
void pluginDeactivate(const clap_plugin_t*) {}
bool pluginStartProcessing(const clap_plugin_t* p) {
  Fixture* f = self(p);
  f->startedOn = std::this_thread::get_id();
  f->checkStart.store(true);
  if (f->refusalsLeft > 0) {
    --f->refusalsLeft;
    return false;
  }
  return true;
}
void pluginStopProcessing(const clap_plugin_t*) {}
void pluginReset(const clap_plugin_t* p) {
  Fixture* f = self(p);
  f->resets.fetch_add(1);
  f->resetOn = std::this_thread::get_id();
  f->checkReset.store(true);
}
clap_process_status pluginProcess(const clap_plugin_t* p, const clap_process_t* process) {
  Fixture* f = self(p);
  const Variant* v = f->variant;
  const std::thread::id here = std::this_thread::get_id();
  if (f->checkStart.exchange(false) && f->startedOn != here) f->wrongThread.fetch_add(1);
  if (f->checkReset.exchange(false) && f->resetOn != here) f->wrongThread.fetch_add(1);
  if (!shapeMatches(process->audio_inputs, process->audio_inputs_count, v->inputs,
                    v->inputCount) ||
      !shapeMatches(process->audio_outputs, process->audio_outputs_count, v->outputs,
                    v->outputCount)) {
    f->violations.fetch_add(1);
    return CLAP_PROCESS_CONTINUE;
  }
  const uint32_t frames = process->frames_count;
  const clap_audio_buffer_t& in = process->audio_inputs[0];
  const clap_audio_buffer_t& out = process->audio_outputs[0];
  const float* side = v->inputCount > 1 ? process->audio_inputs[1].data32[0] : nullptr;
  for (uint32_t c = 0; c < out.channel_count; ++c)
    for (uint32_t i = 0; i < frames; ++i)
      out.data32[c][i] = 2.0f * in.data32[c][i] + (side ? side[i] : 0.0f);
  if (v->outputCount > 1)
    for (uint32_t i = 0; i < frames; ++i) process->audio_outputs[1].data32[0][i] = 1.0f;
  return CLAP_PROCESS_CONTINUE;
}
const void* pluginGetExtension(const clap_plugin_t*, const char* id) {
  if (std::strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0) return &kPorts;
  if (std::strcmp(id, CLAP_EXT_PARAMS) == 0) return &kParams;
  return nullptr;
}
void pluginOnMainThread(const clap_plugin_t*) {}

constexpr uint32_t kVariantCount = sizeof(kVariants) / sizeof(kVariants[0]);
uint32_t factoryCount(const clap_plugin_factory_t*) { return kVariantCount; }
const clap_plugin_descriptor_t* factoryDescriptor(const clap_plugin_factory_t*, uint32_t index) {
  return index < kVariantCount ? &kVariants[index].descriptor : nullptr;
}
const clap_plugin_t* factoryCreate(const clap_plugin_factory_t*, const clap_host_t* host,
                                   const char* id) {
  const Variant* variant = nullptr;
  for (const Variant& v : kVariants)
    if (id && std::strcmp(id, v.descriptor.id) == 0) variant = &v;
  if (!variant) return nullptr;
  auto* f = new Fixture();
  f->host = host;
  f->variant = variant;
  f->refusalsLeft = variant->refuseStarts;
  f->plugin.desc = &variant->descriptor;
  f->plugin.plugin_data = f;
  f->plugin.init = pluginInit;
  f->plugin.destroy = pluginDestroy;
  f->plugin.activate = pluginActivate;
  f->plugin.deactivate = pluginDeactivate;
  f->plugin.start_processing = pluginStartProcessing;
  f->plugin.stop_processing = pluginStopProcessing;
  f->plugin.reset = pluginReset;
  f->plugin.process = pluginProcess;
  f->plugin.get_extension = pluginGetExtension;
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
