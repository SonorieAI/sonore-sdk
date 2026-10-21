// SPDX-License-Identifier: Apache-2.0
// Sonore SDK test fixture: CLAP plugins with many parameters, all reported.
//
// A plugin reports what it did to its own parameters as output events, and
// sonore/host.h turns each one's id into a position in the parameter list.
// That lookup was a linear scan, run once per event on the audio thread: a
// plugin with a few thousand parameters loading a preset -- every one of them
// reported in one block -- cost a few million comparisons inside process().
//
// Written against the raw API so the event pattern is exactly that. Two
// plugins in one file, alike but for their parameter count, each reporting
// every parameter once per block with the parameter's own index as its value.
// The ids are scattered rather than 0..n-1, and not in declaration order, as
// a plugin that assigns them by hashing names has them.
//
// A third, com.sonorie.test.params.unterminated, fills its fixed-size strings
// to the last byte with no terminator, as strncpy does with a name as long
// as the field: parameter 0's name, parameter 1's module (followed by three
// doubles with no zero byte in them), and every value's text.
//
// A fourth, com.sonorie.test.params.garbage-count, has three parameters and
// says it has 4,294,967,295 -- a count read from memory it never set.
//
// A fifth, com.sonorie.test.params.shrinks, has 250 parameters until a state
// is loaded into it, and 10 after, which it announces with
// CLAP_PARAM_RESCAN_ALL -- legal, as it is not active then -- as a plugin
// whose parameters are its modules' does when a session swaps them.

#include <clap/clap.h>

#include <cstdio>
#include <cstring>

namespace {

struct Variant {
  clap_plugin_descriptor_t descriptor;
  uint32_t count;
  /** What params.count() says, when that is not `count`. */
  uint32_t claimed = 0;
};

/** A bijection on 32 bits: odd multiplier, then xor. Distinct ids, no order. */
uint32_t idOf(uint32_t index) { return (index * 2654435761u) ^ 0x5bd1e995u; }

struct Fixture {
  clap_plugin_t plugin{};
  const clap_host_t* host = nullptr;
  const Variant* variant = nullptr;
  uint32_t live = 0; // the parameters it has now
};

Fixture* self(const clap_plugin_t* p) { return static_cast<Fixture*>(p->plugin_data); }

bool unterminated(const clap_plugin_t* p) {
  return std::strcmp(self(p)->variant->descriptor.id, "com.sonorie.test.params.unterminated") == 0;
}

uint32_t paramsCount(const clap_plugin_t* p) {
  const Variant* v = self(p)->variant;
  return v->claimed ? v->claimed : self(p)->live;
}
bool paramsGetInfo(const clap_plugin_t* p, uint32_t index, clap_param_info_t* info) {
  const uint32_t count = self(p)->live;
  if (index >= count) return false;
  std::memset(info, 0, sizeof(*info));
  info->id = idOf(index);
  info->flags = CLAP_PARAM_IS_AUTOMATABLE;
  std::snprintf(info->name, sizeof(info->name), "Param %u", index);
  info->min_value = 0.0;
  info->max_value = (double) count;
  info->default_value = 0.0;
  if (unterminated(p) && index == 0) {
    std::memset(info->name, 'N', sizeof(info->name));
    std::snprintf(info->module, sizeof(info->module), "Group");
  } else if (unterminated(p) && index == 1) {
    std::memset(info->module, 'M', sizeof(info->module));
    info->min_value = 1.1; // the struct's last fields, none with a zero byte
    info->max_value = 2.2;
    info->default_value = 1.7;
  }
  return true;
}
bool paramsGetValue(const clap_plugin_t*, clap_id, double* out) {
  *out = 0.0;
  return true;
}
bool paramsValueToText(const clap_plugin_t* p, clap_id, double v, char* out, uint32_t size) {
  if (unterminated(p)) std::memset(out, 'T', size);
  else std::snprintf(out, size, "%.0f", v);
  return true;
}
bool paramsTextToValue(const clap_plugin_t*, clap_id, const char*, double*) { return false; }
void paramsFlush(const clap_plugin_t*, const clap_input_events_t*, const clap_output_events_t*) {}
const clap_plugin_params_t kParams = {paramsCount,       paramsGetInfo,     paramsGetValue,
                                      paramsValueToText, paramsTextToValue, paramsFlush};

const char* const kFeatures[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, nullptr};
const Variant kVariants[] = {
    {{CLAP_VERSION_INIT, "com.sonorie.test.params.small", "Sonore Params Fixture (small)", "Sonorie",
      "https://sonorie.com", "", "", "1.0.0", "Reports all 250 of its parameters every block.",
      kFeatures},
     250},
    {{CLAP_VERSION_INIT, "com.sonorie.test.params.large", "Sonore Params Fixture (large)", "Sonorie",
      "https://sonorie.com", "", "", "1.0.0", "Reports all 4000 of its parameters every block.",
      kFeatures},
     4000},
    {{CLAP_VERSION_INIT, "com.sonorie.test.params.unterminated", "Sonore Params Fixture (unterminated)",
      "Sonorie", "https://sonorie.com", "", "", "1.0.0",
      "Fills its fixed-size strings with no terminator.", kFeatures},
     3},
    {{CLAP_VERSION_INIT, "com.sonorie.test.params.garbage-count", "Sonore Params Fixture (garbage count)",
      "Sonorie", "https://sonorie.com", "", "", "1.0.0",
      "Has three parameters and says it has four billion.", kFeatures},
     3,
     0xFFFFFFFFu},
    {{CLAP_VERSION_INIT, "com.sonorie.test.params.shrinks", "Sonore Params Fixture (shrinks)",
      "Sonorie", "https://sonorie.com", "", "", "1.0.0",
      "Has 250 parameters until a state is loaded, and 10 after.", kFeatures},
     250},
};

bool pluginInit(const clap_plugin_t*) { return true; }
void pluginDestroy(const clap_plugin_t* p) { delete self(p); }
bool pluginActivate(const clap_plugin_t*, double, uint32_t, uint32_t) { return true; }
void pluginDeactivate(const clap_plugin_t*) {}
bool pluginStartProcessing(const clap_plugin_t*) { return true; }
void pluginStopProcessing(const clap_plugin_t*) {}
void pluginReset(const clap_plugin_t*) {}
clap_process_status pluginProcess(const clap_plugin_t* p, const clap_process_t* process) {
  const Fixture* f = self(p);
  if (!process->out_events) return CLAP_PROCESS_CONTINUE;
  // What a preset load looks like from the host's side: every parameter at
  // once, in the plugin's own order.
  clap_event_param_value_t e{};
  e.header.size = sizeof(e);
  e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
  e.header.type = CLAP_EVENT_PARAM_VALUE;
  e.note_id = -1;
  e.port_index = -1;
  e.channel = -1;
  e.key = -1;
  for (uint32_t i = 0; i < f->live; ++i) {
    e.param_id = idOf(i);
    e.value = (double) i;
    process->out_events->try_push(process->out_events, &e.header);
  }
  return CLAP_PROCESS_CONTINUE;
}
bool stateSave(const clap_plugin_t*, const clap_ostream_t* stream) {
  const uint8_t byte = 1;
  return stream->write(stream, &byte, 1) == 1;
}
bool stateLoad(const clap_plugin_t* p, const clap_istream_t*) {
  Fixture* f = self(p);
  f->live = 10;
  const auto* params =
      static_cast<const clap_host_params_t*>(f->host->get_extension(f->host, CLAP_EXT_PARAMS));
  if (params && params->rescan) params->rescan(f->host, CLAP_PARAM_RESCAN_ALL);
  return true;
}
const clap_plugin_state_t kState = {stateSave, stateLoad};

const void* pluginGetExtension(const clap_plugin_t* p, const char* id) {
  if (std::strcmp(id, CLAP_EXT_STATE) == 0 &&
      std::strcmp(self(p)->variant->descriptor.id, "com.sonorie.test.params.shrinks") == 0)
    return &kState;
  return std::strcmp(id, CLAP_EXT_PARAMS) == 0 ? &kParams : nullptr;
}
void pluginOnMainThread(const clap_plugin_t*) {}

uint32_t factoryCount(const clap_plugin_factory_t*) { return 5; }
const clap_plugin_descriptor_t* factoryDescriptor(const clap_plugin_factory_t*, uint32_t index) {
  return index < 5 ? &kVariants[index].descriptor : nullptr;
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
  f->live = variant->count;
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
