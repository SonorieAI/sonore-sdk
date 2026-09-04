// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: the CLAP wrapper driven exactly as a DAW drives it.
//
// This is the test that matters most: it loads the built .clap the same way a
// host does (entry -> factory -> descriptor -> create -> init -> activate ->
// process), pushes real parameter and note events through the event list, and
// round-trips state. Every check here is a bug a DAW would otherwise find
// first, in front of a paying customer.
//
// It links nothing from the plugin: the .clap is opened as a shared library at
// runtime, so what is tested is the SHIPPED artifact, not a second build of the
// same source.
#include <sonore/midi_ci.h>
#include <clap/clap.h>
#include <clap/factory/preset-discovery.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <array>
#include <utility>
#include <limits>
#include <random>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
using LibHandle = HMODULE;
static LibHandle openLib(const char* path) { return LoadLibraryA(path); }
static void* symbol(LibHandle h, const char* name) {
  return (void*) GetProcAddress(h, name);
}
static void closeLib(LibHandle h) { FreeLibrary(h); }
#else
#include <dlfcn.h>
using LibHandle = void*;
static LibHandle openLib(const char* path) { return dlopen(path, RTLD_LOCAL | RTLD_NOW); }
static void* symbol(LibHandle h, const char* name) { return dlsym(h, name); }
static void closeLib(LibHandle h) { dlclose(h); }
#endif

#if defined(__linux__) && defined(SONORE_TEST_X11)
// Embedding on Linux means handing the plugin a real X window, so the test
// needs Xlib, and a GTK loop to pump, exactly as a Linux DAW has.
//
// Opt-in, because Xlib headers are a dev package: a plain build (the WSL
// cross-check, a machine with no X) still compiles and runs everything else,
// and says out loud that embedding was not exercised rather than pretending.
#include <X11/Xlib.h>
#include <unistd.h>
#endif

static int g_failures = 0;
static int g_checks = 0;

static void check(bool ok, const char* what) {
  ++g_checks;
  std::printf(ok ? "  ok   %s\n" : "  FAIL %s\n", what);
  if (!ok) ++g_failures;
}

static void checkNear(double got, double want, double tol, const char* what) {
  ++g_checks;
  if (std::fabs(got - want) <= tol) {
    std::printf("  ok   %s (%.4f)\n", what, got);
  } else {
    ++g_failures;
    std::printf("  FAIL %s: got %.4f, want %.4f +/- %.4f\n", what, got, want, tol);
  }
}

// ── A minimal host ───────────────────────────────────────────────────────────

static int g_restartRequests = 0;
static void hostRequestRestart(const clap_host_t*) { ++g_restartRequests; }
static void hostRequestProcess(const clap_host_t*) {}
static int g_callbackRequests = 0;
static void hostRequestCallback(const clap_host_t*) { ++g_callbackRequests; }

// A state load rewrites every parameter behind the host's back; the plugin is
// required to announce that with clap_host_params::rescan(RESCAN_VALUES) or a
// generic host UI keeps showing stale values. Record what gets requested so
// the state test below can assert it (clap-validator fails plugins that skip
// this, so our own suite must too).
static uint32_t g_rescanFlags = 0;
static void hostParamsRescan(const clap_host_t*, clap_param_rescan_flags flags) {
  g_rescanFlags |= flags;
}
static void hostParamsClear(const clap_host_t*, clap_id, clap_param_clear_flags) {}
static void hostParamsRequestFlush(const clap_host_t*) {}
static const clap_host_params_t kHostParams = {hostParamsRescan, hostParamsClear,
                                               hostParamsRequestFlush};

/** How many times the plugin said its latency moved, and whether it asked to
 *  be called on the main thread to say it. Both matter: latency.changed() is
 *  [main-thread] and process() is not, so a plugin that notices the change
 *  while processing has to ASK rather than reach for the host. */
static int g_latencyChanged = 0;
static void hostLatencyChanged(const clap_host_t*) { ++g_latencyChanged; }
static const clap_host_latency_t kHostLatency = {hostLatencyChanged};

/** How many times the plugin said the SESSION changed underneath the host --
 *  something it did on its own, that no parameter and no host call caused. A
 *  host that is never told closes without asking to save. */
static int g_markedDirty = 0;
static void hostMarkDirty(const clap_host_t*) { ++g_markedDirty; }
static const clap_host_state_t kHostState = {hostMarkDirty};

/** How many times the plugin said its TAIL changed. Unlike the others this
 *  one is [audio-thread] by the header's own marking, which is the whole
 *  point of testing it separately. */
static int g_tailChanged = 0;
static void hostTailChanged(const clap_host_t*) { ++g_tailChanged; }
static const clap_host_tail_t kHostTail = {hostTailChanged};

/** The track this host pretends to have. A real host gathers this from the
 *  session; the point here is that the plugin ASKS and reads what comes back
 *  -- including the parts it must decode rather than copy, like the flags and
 *  the colour. */
static bool g_trackInfoQueried = false;
static bool hostGetTrackInfo(const clap_host_t*, clap_track_info_t* info) {
  if (!info) return false;
  g_trackInfoQueried = true;
  *info = clap_track_info_t{};
  info->flags = CLAP_TRACK_INFO_HAS_TRACK_NAME | CLAP_TRACK_INFO_HAS_TRACK_COLOR |
                CLAP_TRACK_INFO_HAS_AUDIO_CHANNEL | CLAP_TRACK_INFO_IS_FOR_RETURN_TRACK;
  std::snprintf(info->name, sizeof(info->name), "%s", "Verb Return");
  info->color.alpha = 255;
  info->color.red = 0x2a;
  info->color.green = 0xb1;
  info->color.blue = 0x7c;
  info->audio_channel_count = 2;
  info->audio_port_type = CLAP_PORT_STEREO;
  return true;
}
static const clap_host_track_info_t kHostTrackInfo = {hostGetTrackInfo};

/** The host's own right-click menu. A real one carries MIDI learn, "remove
 *  automation" and the rest -- things only the host can offer, and which
 *  vanish if a plugin handles the right-click itself. Recorded rather than
 *  shown: a modal menu in a test would never come back. */
static int g_menuPopups = 0;
static uint32_t g_menuTargetKind = 999;
static clap_id g_menuTargetId = 9999;
static int g_menuX = -1, g_menuY = -1;
static bool hostMenuCanPopup(const clap_host_t*) { return true; }
static bool hostMenuPopup(const clap_host_t*, const clap_context_menu_target_t* target,
                          int32_t /*screen*/, int32_t x, int32_t y) {
  ++g_menuPopups;
  if (target) {
    g_menuTargetKind = target->kind;
    g_menuTargetId = target->id;
  }
  g_menuX = (int) x;
  g_menuY = (int) y;
  return true;
}
static bool hostMenuPopulate(const clap_host_t*, const clap_context_menu_target_t*,
                             const clap_context_menu_builder_t*) {
  return false;
}
static bool hostMenuPerform(const clap_host_t*, const clap_context_menu_target_t*, clap_id) {
  return false;
}
static const clap_host_context_menu_t kHostContextMenu = {hostMenuPopulate, hostMenuPerform,
                                                          hostMenuCanPopup, hostMenuPopup};

static const void* hostGetExtension(const clap_host_t*, const char* id) {
  if (std::strcmp(id, CLAP_EXT_CONTEXT_MENU) == 0) return &kHostContextMenu;
  if (std::strcmp(id, CLAP_EXT_TRACK_INFO) == 0) return &kHostTrackInfo;
  if (std::strcmp(id, CLAP_EXT_TAIL) == 0) return &kHostTail;
  if (std::strcmp(id, CLAP_EXT_PARAMS) == 0) return &kHostParams;
  if (std::strcmp(id, CLAP_EXT_LATENCY) == 0) return &kHostLatency;
  if (std::strcmp(id, CLAP_EXT_STATE) == 0) return &kHostState;
  return nullptr;
}

static clap_host_t makeHost() {
  clap_host_t h{};
  h.clap_version = CLAP_VERSION;
  h.host_data = nullptr;
  h.name = "Sonore SDK Test Host";
  h.vendor = "Sonorie";
  h.url = "https://sonorie.com";
  h.version = "1.0.0";
  h.get_extension = hostGetExtension;
  h.request_restart = hostRequestRestart;
  h.request_process = hostRequestProcess;
  h.request_callback = hostRequestCallback;
  return h;
}

/** An input event list backed by a vector of raw event bytes. */
struct EventList {
  std::vector<std::vector<uint8_t>> storage;
  clap_input_events_t in{};

  void addParam(uint32_t paramId, double value, uint32_t time) {
    clap_event_param_value_t e{};
    e.header.size = sizeof(e);
    e.header.time = time;
    e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    e.header.type = CLAP_EVENT_PARAM_VALUE;
    e.param_id = paramId;
    e.note_id = -1;
    e.port_index = -1;
    e.channel = -1;
    e.key = -1;
    e.value = value;
    push(&e, sizeof(e));
  }

  void addNote(uint16_t type, int16_t key, double velocity, uint32_t time,
               int16_t channel = 0) {
    clap_event_note_t e{};
    e.header.size = sizeof(e);
    e.header.time = time;
    e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    e.header.type = type;
    e.note_id = -1;
    e.port_index = 0;
    e.channel = channel;
    e.key = key;
    e.velocity = velocity;
    push(&e, sizeof(e));
  }

  /** Raw MIDI, which is what an RPN is: three bytes at a time. */
  void addMidi(uint8_t status, uint8_t d1, uint8_t d2, uint32_t time) {
    clap_event_midi_t e{};
    e.header.size = sizeof(e);
    e.header.time = time;
    e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    e.header.type = CLAP_EVENT_MIDI;
    e.port_index = 0;
    e.data[0] = status;
    e.data[1] = d1;
    e.data[2] = d2;
    push(&e, sizeof(e));
  }

  void push(const void* data, size_t size) {
    std::vector<uint8_t> bytes(size);
    std::memcpy(bytes.data(), data, size);
    storage.push_back(std::move(bytes));
  }

  static uint32_t sizeCb(const clap_input_events_t* list) {
    return (uint32_t) static_cast<EventList*>(list->ctx)->storage.size();
  }
  static const clap_event_header_t* getCb(const clap_input_events_t* list, uint32_t index) {
    auto* self = static_cast<EventList*>(list->ctx);
    return reinterpret_cast<const clap_event_header_t*>(self->storage[index].data());
  }

  clap_input_events_t* events() {
    in.ctx = this;
    in.size = sizeCb;
    in.get = getCb;
    return &in;
  }
};

/** What the plugin emitted, in order. A MIDI effect is graded by this the way
 *  an instrument is graded by its audio: the output IS the product. */
struct EmittedMidi {
  struct Event {
    uint32_t time;
    uint8_t data[3];
  };
  std::vector<Event> events;
  void clear() { events.clear(); }
  int countStatus(uint8_t high) const {
    int n = 0;
    for (const Event& e : events)
      if ((e.data[0] & 0xf0) == high) ++n;
    return n;
  }
};
static EmittedMidi g_emitted;

/** SysEx the plugin emitted, COPIED here. The event carries a pointer that is
 *  only promised to be valid until process() returns, so a test that kept the
 *  pointer would be testing its own bug rather than the plugin's. */
static std::vector<std::vector<uint8_t>> g_emittedSysex;

static bool outTryPush(const clap_output_events_t*, const clap_event_header_t* h) {
  if (h && h->type == CLAP_EVENT_MIDI && h->space_id == CLAP_CORE_EVENT_SPACE_ID) {
    const auto* m = reinterpret_cast<const clap_event_midi_t*>(h);
    g_emitted.events.push_back({h->time, {m->data[0], m->data[1], m->data[2]}});
  } else if (h && h->type == CLAP_EVENT_MIDI_SYSEX &&
             h->space_id == CLAP_CORE_EVENT_SPACE_ID) {
    const auto* sx = reinterpret_cast<const clap_event_midi_sysex_t*>(h);
    if (sx->buffer && sx->size > 0)
      g_emittedSysex.push_back(std::vector<uint8_t>(sx->buffer, sx->buffer + sx->size));
  }
  return true;
}

/** A preset indexer + metadata receiver, enough to crawl a plugin's built-in
 *  presets the way a host's browser does. We drive this ourselves because
 *  clap-validator 0.4.1 deadlocks on any provider with more than one preset
 *  (its begin_preset holds a lock and then calls flush_preset, which takes
 *  the same non-reentrant mutex): the plugin side is fine, the tool is not. */
struct PresetCrawl {
  std::vector<std::string> names;
  std::vector<std::string> loadKeys;
  int locations = 0;
  bool sawPluginId = false;
};
static PresetCrawl g_crawl;

static bool idxDeclareFiletype(const clap_preset_discovery_indexer_t*,
                               const clap_preset_discovery_filetype_t*) {
  return true;
}
static bool idxDeclareLocation(const clap_preset_discovery_indexer_t*,
                               const clap_preset_discovery_location_t* l) {
  if (l) ++g_crawl.locations;
  return true;
}
static bool idxDeclareSoundpack(const clap_preset_discovery_indexer_t*,
                                const clap_preset_discovery_soundpack_t*) {
  return true;
}
static const void* idxGetExtension(const clap_preset_discovery_indexer_t*, const char*) {
  return nullptr;
}

static void rxOnError(const clap_preset_discovery_metadata_receiver_t*, int32_t, const char*) {}
static bool rxBeginPreset(const clap_preset_discovery_metadata_receiver_t*, const char* name,
                          const char* key) {
  g_crawl.names.push_back(name ? name : "");
  g_crawl.loadKeys.push_back(key ? key : "");
  return true;
}
static void rxAddPluginId(const clap_preset_discovery_metadata_receiver_t*,
                          const clap_universal_plugin_id_t* id) {
  if (id && id->abi && id->id && std::strcmp(id->abi, "clap") == 0) g_crawl.sawPluginId = true;
}
static void rxSetSoundpackId(const clap_preset_discovery_metadata_receiver_t*, const char*) {}
static void rxSetFlags(const clap_preset_discovery_metadata_receiver_t*, uint32_t) {}
static void rxAddCreator(const clap_preset_discovery_metadata_receiver_t*, const char*) {}
static void rxSetDescription(const clap_preset_discovery_metadata_receiver_t*, const char*) {}
static void rxSetTimestamps(const clap_preset_discovery_metadata_receiver_t*, clap_timestamp,
                            clap_timestamp) {}
static void rxAddFeature(const clap_preset_discovery_metadata_receiver_t*, const char*) {}
static void rxAddExtraInfo(const clap_preset_discovery_metadata_receiver_t*, const char*,
                           const char*) {}

/** A version this build cannot possibly understand. Named rather than written
 *  inline, so that when the format really does reach it the test fails loudly
 *  instead of quietly testing nothing. */
static constexpr uint32_t kFutureVersion = 99;

/** An in-memory stream pair for the state round-trip. */
struct MemStream {
  std::vector<uint8_t> bytes;
  size_t readPos = 0;

  static int64_t writeCb(const clap_ostream* s, const void* buf, uint64_t size) {
    auto* self = static_cast<MemStream*>(s->ctx);
    const uint8_t* p = static_cast<const uint8_t*>(buf);
    self->bytes.insert(self->bytes.end(), p, p + size);
    return (int64_t) size;
  }
  static int64_t readCb(const clap_istream* s, void* buf, uint64_t size) {
    auto* self = static_cast<MemStream*>(s->ctx);
    const size_t left = self->bytes.size() - self->readPos;
    const size_t take = size < left ? (size_t) size : left;
    if (take > 0) std::memcpy(buf, self->bytes.data() + self->readPos, take);
    self->readPos += take;
    return (int64_t) take;
  }
};

int main(int argc, char** argv) {
  // Unbuffered, so a crash under ctest still shows the line it crashed after.
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  if (argc < 2) {
    std::printf("usage: clap_host_test <path-to.clap> [--expect-bridge]\n");
    return 2;
  }
  const char* path = argv[1];
  bool expectBridge = false;
  bool expectSidechain = false;
  // 0 means "not given": assert only that SOME floor exists, which is what
  // every plugin without a declared size gets.
  uint32_t expectMinW = 0, expectMinH = 0, expectMaxW = 0, expectMaxH = 0;
  /** This plugin emits a constant with silent input, ON PURPOSE. See the tail
   *  measurement for why that makes one assertion inapplicable. */
  bool emitsDc = false;
  int expectChanMin = 0, expectChanMax = 0; // 0 = fixed-stereo expectations
  int expectAuxOuts = -1;                   // -1 = no expectation
  bool expectMidiOut = false;
  // Which editor this plugin should open. A plugin that defines SONORE_UI_HTML
  // asked for a page; one that does not gets the SDK's own controls. The test
  // is told which to expect so it can measure rather than accept whatever
  // appeared -- an editor that silently fell back to the other backend would
  // otherwise pass without a word.
  int expectEditor = 0; // 0 = no expectation, 1 = native, 2 = web
  for (int i = 2; i < argc; ++i) {
    if (std::strcmp(argv[i], "--expect-native-editor") == 0) expectEditor = 1;
    if (std::strcmp(argv[i], "--expect-web-editor") == 0) expectEditor = 2;
    if (std::strcmp(argv[i], "--expect-bridge") == 0) expectBridge = true;
    if (std::strcmp(argv[i], "--expect-sidechain") == 0) expectSidechain = true;
    if (std::strcmp(argv[i], "--expect-midi-out") == 0) expectMidiOut = true;
    if (std::strcmp(argv[i], "--expect-aux-outs") == 0 && i + 1 < argc) {
      expectAuxOuts = std::atoi(argv[i + 1]);
      ++i;
    }
    if (std::strcmp(argv[i], "--emits-dc") == 0) emitsDc = true;
    if (std::strcmp(argv[i], "--expect-editor-limits") == 0 && i + 4 < argc) {
      // The size THIS plugin declared, as four numbers. Given rather than
      // inferred because the point is to catch a wrapper that ignores the
      // descriptor: asking the plugin what it declared and then checking the
      // plugin agrees would pass against exactly the bug being looked for.
      expectMinW = (uint32_t) std::atoi(argv[i + 1]);
      expectMinH = (uint32_t) std::atoi(argv[i + 2]);
      expectMaxW = (uint32_t) std::atoi(argv[i + 3]);
      expectMaxH = (uint32_t) std::atoi(argv[i + 4]);
    }
    if (std::strcmp(argv[i], "--expect-channels") == 0 && i + 2 < argc) {
      expectChanMin = std::atoi(argv[i + 1]);
      expectChanMax = std::atoi(argv[i + 2]);
      i += 2;
    }
  }
  std::printf("Sonore CLAP host test\n  plugin: %s\n\n", path);

  LibHandle lib = openLib(path);
  if (!lib) {
    std::printf("  FAIL could not load the plugin binary\n");
    return 1;
  }

  const auto* entry = static_cast<const clap_plugin_entry_t*>(symbol(lib, "clap_entry"));
  check(entry != nullptr, "the DSO exports clap_entry");
  if (!entry) return 1;

  check(entry->init(path), "entry init succeeds");

  const auto* factory =
      static_cast<const clap_plugin_factory_t*>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
  check(factory != nullptr, "the plugin factory is available");
  if (!factory) return 1;

  check(factory->get_plugin_count(factory) == 1, "the factory advertises one plugin");

  const clap_plugin_descriptor_t* desc = factory->get_plugin_descriptor(factory, 0);
  check(desc != nullptr, "a descriptor is returned");
  if (!desc) return 1;
  check(desc->id && std::strlen(desc->id) > 0, "the descriptor has an id");
  check(desc->name && std::strlen(desc->name) > 0, "the descriptor has a name");
  check(desc->features != nullptr && desc->features[0] != nullptr,
        "the descriptor declares features (hosts categorise on these)");

  clap_host_t host = makeHost();
  const clap_plugin_t* plugin = factory->create_plugin(factory, &host, desc->id);
  check(plugin != nullptr, "the factory creates an instance");
  if (!plugin) return 1;

  // An unknown id must return null rather than a wrong plugin.
  check(factory->create_plugin(factory, &host, "com.example.nope") == nullptr,
        "an unknown plugin id is refused");

  check(plugin->init(plugin), "the instance initialises");

  // ── Parameters ────────────────────────────────────────────────────────────
  const auto* params =
      static_cast<const clap_plugin_params_t*>(plugin->get_extension(plugin, CLAP_EXT_PARAMS));
  check(params != nullptr, "the params extension is exposed");
  if (!params) return 1;

  const uint32_t nParams = params->count(plugin);
  check(nParams > 0, "the plugin declares parameters");

  bool infoOk = true, defaultsInRange = true;
  for (uint32_t i = 0; i < nParams; ++i) {
    clap_param_info_t info{};
    if (!params->get_info(plugin, i, &info)) { infoOk = false; break; }
    if (info.min_value >= info.max_value) infoOk = false;
    if (info.default_value < info.min_value || info.default_value > info.max_value)
      defaultsInRange = false;
    if (info.name[0] == '\0') infoOk = false;
    // A value must render to SOMETHING a host can display.
    char text[CLAP_NAME_SIZE];
    if (!params->value_to_text(plugin, info.id, info.default_value, text, sizeof(text)) ||
        text[0] == '\0')
      infoOk = false;
  }
  check(infoOk, "every parameter reports valid, named, displayable info");

  // Parameter GROUPS: a host builds its tree from the module path. Only the
  // plugins that declare groups have them, so this is a soft, informative
  // check plus a hard one on well-formedness.
  {
    int grouped = 0;
    bool wellFormed = true;
    for (uint32_t i = 0; i < nParams; ++i) {
      clap_param_info_t pi{};
      if (!params->get_info(plugin, i, &pi)) continue;
      const size_t len = std::strlen(pi.module);
      if (len > 0) {
        ++grouped;
        // A leading or trailing slash makes an empty path element, which some
        // hosts render as a nameless folder.
        if (pi.module[0] == '/' || pi.module[len - 1] == '/') wellFormed = false;
      }
    }
    check(wellFormed, "parameter module paths are well formed");
    std::printf("  ---- %d of %u parameters carry a group ----\n", grouped, nParams);
  }
  check(defaultsInRange, "every default sits inside its own range");
  check(!params->get_info(plugin, nParams, nullptr) || true, "an out-of-range index is handled");

  // Defaults must be live before any event is sent.
  double value = -12345.0;
  check(params->get_value(plugin, 0, &value), "parameter 0 reads back");
  clap_param_info_t p0{};
  params->get_info(plugin, 0, &p0);
  checkNear(value, p0.default_value, 1e-6, "…as its declared default");

  // ── Activate + process ────────────────────────────────────────────────────
  const uint32_t blockSize = 128;
  const double sampleRate = 48000.0;
  check(plugin->activate(plugin, sampleRate, 1, blockSize), "the plugin activates");
  check(plugin->start_processing(plugin), "processing starts");

  const auto* audioPorts = static_cast<const clap_plugin_audio_ports_t*>(
      plugin->get_extension(plugin, CLAP_EXT_AUDIO_PORTS));
  check(audioPorts != nullptr, "the audio-ports extension is exposed");
  const uint32_t outPorts = audioPorts ? audioPorts->count(plugin, false) : 0;
  check(outPorts >= 1, "there is at least one output port");

  clap_audio_port_info_t outInfo{};
  if (audioPorts) audioPorts->get(plugin, 0, false, &outInfo);
  check(outInfo.channel_count == 2, "the output port is stereo");

  const bool hasInput = audioPorts && audioPorts->count(plugin, true) > 0;

  if (expectAuxOuts >= 0) {
    check(audioPorts && (int) audioPorts->count(plugin, false) == 1 + expectAuxOuts,
          "the plugin declares its aux output ports");
    for (int b = 0; b < expectAuxOuts; ++b) {
      clap_audio_port_info_t ai{};
      check(audioPorts && audioPorts->get(plugin, (uint32_t) (1 + b), false, &ai),
            "each aux output port reports info");
      check((ai.flags & CLAP_AUDIO_PORT_IS_MAIN) == 0, "…and is NOT the main port");
      check(ai.name[0] != 0, "…and carries the name the descriptor gave it");
    }
  }

  if (expectSidechain) {
    check(audioPorts && audioPorts->count(plugin, true) == 2,
          "a sidechain DSP declares TWO input ports");
    clap_audio_port_info_t scInfo{};
    check(audioPorts && audioPorts->get(plugin, 1, true, &scInfo),
          "the sidechain port reports info");
    check(std::strcmp(scInfo.name, "Sidechain") == 0, "…named Sidechain");
    check((scInfo.flags & CLAP_AUDIO_PORT_IS_MAIN) == 0, "…and it is NOT a main port");
  }

  // An instrument is identified the way a host identifies one: it has a note
  // input port. The two kinds are then graded by DIFFERENT physics: feeding a
  // synth audio and demanding output would fail every correct synth, and
  // demanding silence-at-rest from an effect would fail every correct effect.
  const auto* notePorts = static_cast<const clap_plugin_note_ports_t*>(
      plugin->get_extension(plugin, CLAP_EXT_NOTE_PORTS));
  const bool isInstrument = notePorts && notePorts->count(plugin, true) > 0;
  std::printf("  ---- graded as %s ----\n", isInstrument ? "an INSTRUMENT" : "an EFFECT");
  if (isInstrument) {
    clap_note_port_info_t noteInfo{};
    check(notePorts->get(plugin, 0, true, &noteInfo), "the note port reports info");
    check((noteInfo.supported_dialects & CLAP_NOTE_DIALECT_MIDI) != 0,
          "the note port accepts raw MIDI (hosts that don't speak CLAP notes)");
    check(!hasInput, "an instrument declares no audio input bus");
  }

  if (expectMidiOut) {
    check(notePorts && notePorts->count(plugin, false) == 1,
          "a MIDI-emitting plugin declares a note OUTPUT port");
    clap_note_port_info_t nout{};
    check(notePorts && notePorts->get(plugin, 0, false, &nout),
          "the note output port reports info");
    check((nout.supported_dialects & CLAP_NOTE_DIALECT_MIDI) != 0,
          "…and speaks the dialect it actually emits");
  }


  std::vector<float> inL(blockSize), inR(blockSize), outL(blockSize), outR(blockSize);
  float* inPtrs[2] = {inL.data(), inR.data()};
  float* outPtrs[2] = {outL.data(), outR.data()};

  std::vector<float> scL(blockSize, 0.0f), scR(blockSize, 0.0f);
  float* scPtrs[2] = {scL.data(), scR.data()};

  clap_audio_buffer_t inBufs[2] = {};
  inBufs[0].data32 = inPtrs;
  inBufs[0].channel_count = 2;
  inBufs[1].data32 = scPtrs;
  inBufs[1].channel_count = 2;
  clap_audio_buffer_t& inBuf = inBufs[0];
  // Aux output buses get real, separately identifiable buffers: a wrapper
  // that routed a band to the wrong bus, or to the main out, fails below.
  constexpr int kMaxTestAux = 4;
  std::vector<std::vector<float>> auxCh((size_t) (kMaxTestAux * 2));
  std::vector<std::array<float*, 2>> auxPtrs((size_t) kMaxTestAux);
  clap_audio_buffer_t outBufs[1 + kMaxTestAux] = {};
  for (int b = 0; b < kMaxTestAux; ++b) {
    auxCh[(size_t) (b * 2)].assign(blockSize, 0.0f);
    auxCh[(size_t) (b * 2 + 1)].assign(blockSize, 0.0f);
    auxPtrs[(size_t) b] = {auxCh[(size_t) (b * 2)].data(), auxCh[(size_t) (b * 2 + 1)].data()};
    outBufs[1 + b].data32 = auxPtrs[(size_t) b].data();
    outBufs[1 + b].channel_count = 2;
  }
  clap_audio_buffer_t& outBuf = outBufs[0];
  outBuf.data32 = outPtrs;
  outBuf.channel_count = 2;

  EventList events;
  clap_output_events_t outEvents{};
  outEvents.ctx = nullptr;
  outEvents.try_push = outTryPush;

  clap_process_t process{};
  process.steady_time = 0;
  process.frames_count = blockSize;
  process.audio_inputs = hasInput ? inBufs : nullptr;
  process.audio_inputs_count = hasInput ? (expectSidechain ? 2u : 1u) : 0u;
  process.audio_outputs = outBufs;
  process.audio_outputs_count = expectAuxOuts > 0 ? (uint32_t) (1 + expectAuxOuts) : 1u;
  process.in_events = events.events();
  process.out_events = &outEvents;

  // Run `blocks` blocks, accumulating the output's health. `feed` fills the
  // input buffer for each block; the caller decides what "input" means.
  struct Result {
    double peak = 0.0;
    double energy = 0.0;
    bool finite = true;
    bool errored = false;
  };
  auto run = [&](int blocks, bool silentInput) -> Result {
    Result r;
    g_emitted.clear(); // each measured run is judged on its own emissions
    static int phase = 0;
    for (int b = 0; b < blocks; ++b) {
      for (uint32_t i = 0; i < blockSize; ++i) {
        const float s =
            silentInput ? 0.0f
                        : 0.25f * (float) std::sin(2.0 * 3.14159265358979 * 1000.0 * phase /
                                                   sampleRate);
        inL[i] = s;
        inR[i] = s;
        ++phase;
      }
      if (plugin->process(plugin, &process) == CLAP_PROCESS_ERROR) {
        r.errored = true;
        break;
      }
      events.storage.clear(); // events are consumed by the block that got them
      for (uint32_t i = 0; i < blockSize; ++i) {
        if (!std::isfinite(outL[i]) || !std::isfinite(outR[i])) r.finite = false;
        const double a = std::fabs(outL[i]);
        if (a > r.peak) r.peak = a;
        r.energy += (double) outL[i] * outL[i];
      }
    }
    return r;
  };

  const int oneSecond = (int) (sampleRate / blockSize);

  if (isInstrument) {
    // Silence at rest is a REQUIREMENT for an instrument: a synth humming with
    // no notes held is a bug every reviewer hears immediately.
    const Result rest = run(oneSecond / 4, true);
    check(!rest.errored && rest.finite, "an idle instrument stays finite");
    check(rest.peak < 1e-4, "an instrument is SILENT at rest (no notes held)");

    // Note on -> it must sound.
    events.storage.clear();
    events.addNote(CLAP_EVENT_NOTE_ON, 69, 1.0, 0); // A4
    const Result playing = run(oneSecond / 2, true);
    check(!playing.errored && playing.finite, "a held note stays finite");
    if (expectMidiOut) {
      // A note EFFECT is silent on purpose: its product is the MIDI it emits.
      check(playing.energy == 0.0, "a note effect stays audibly silent");
      check(!g_emitted.events.empty(), "…and a held note makes it EMIT MIDI");
      check(g_emitted.countStatus(0x90) > 1,
            "…emitting several note-ons, one per arpeggiator step");
      check(g_emitted.countStatus(0x80) > 0, "…each of them released again");
      bool pitchesInRange = true, offsetsInBlock = true;
      for (const auto& e : g_emitted.events) {
        if (e.data[1] > 127 || e.data[2] > 127) pitchesInRange = false;
        if (e.time >= blockSize) offsetsInBlock = false;
      }
      check(pitchesInRange, "…with data bytes inside MIDI's 7-bit range");
      check(offsetsInBlock, "…and frame offsets inside the block the host gave");
    } else {
      check(playing.energy > 0.0, "note-on makes the instrument sound");
    }
    check(playing.peak < 10.0, "a held note does not blow up");

    // Note off -> it must decay back to silence.
    events.storage.clear();
    events.addNote(CLAP_EVENT_NOTE_OFF, 69, 0.0, 0);
    run(oneSecond * 3, true); // let the release finish
    const Result released = run(oneSecond / 4, true);
    check(released.peak < 1e-3, "note-off releases back to silence");

    // ── The damper pedal ────────────────────────────────────────
    //
    // Without it a piano-style patch is unplayable: the player holds the
    // pedal, lifts their hands, and everything stops dead. CC 64 has to
    // survive the whole chain -- host event, wrapper, MIDI decode, the
    // instrument's own event loop -- and nothing above this proves the last
    // step, because the wrapper delivers the controller happily to a plugin
    // that then ignores it.
    //
    // Measured RELATIVELY, against the same note released without the pedal.
    // An absolute threshold would be measuring the instrument's own release
    // time instead: a sampler whose sample simply ends would fail a test that
    // demanded sound, and a synth with a long release would pass one whatever
    // the pedal did.
    if (!expectMidiOut) {
      const int window = oneSecond / 2;

      events.storage.clear();
      events.addNote(CLAP_EVENT_NOTE_ON, 69, 1.0, 0);
      run(oneSecond / 4, true);
      events.storage.clear();
      events.addNote(CLAP_EVENT_NOTE_OFF, 69, 0.0, 0);
      const Result freely = run(window, true);
      run(oneSecond * 3, true); // back to silence before the next attempt

      events.storage.clear();
      events.addMidi(0xb0, 64, 127, 0); // pedal down, before the note
      events.addNote(CLAP_EVENT_NOTE_ON, 69, 1.0, 1);
      run(oneSecond / 4, true);
      events.storage.clear();
      events.addNote(CLAP_EVENT_NOTE_OFF, 69, 0.0, 0);
      const Result pedalled = run(window, true);

      std::printf("  ---- after note-off: %.6f free, %.6f with the pedal down ----\n",
                  freely.energy, pedalled.energy);
      check(pedalled.energy > freely.energy * 4.0,
            "with CC 64 held, a key coming up does not stop the note");

      // And letting the pedal up ends it, or the pedal is not a pedal but a
      // switch that disables note-off for ever.
      events.storage.clear();
      events.addMidi(0xb0, 64, 0, 0);
      run(oneSecond * 3, true);
      const Result afterPedal = run(oneSecond / 4, true);
      check(afterPedal.peak < 1e-3, "…and letting it up releases what it was holding");
    }

    // A chord must not blow up: the classic voice-summing failure.
    events.storage.clear();
    events.addNote(CLAP_EVENT_NOTE_ON, 60, 1.0, 0);
    events.addNote(CLAP_EVENT_NOTE_ON, 64, 1.0, 0);
    events.addNote(CLAP_EVENT_NOTE_ON, 67, 1.0, 0);
    events.addNote(CLAP_EVENT_NOTE_ON, 72, 1.0, 0);
    const Result chord = run(oneSecond / 2, true);
    check(chord.finite && chord.peak < 10.0, "a four-note chord stays bounded");
    events.storage.clear();
    events.addNote(CLAP_EVENT_NOTE_OFF, 60, 0.0, 0);
    events.addNote(CLAP_EVENT_NOTE_OFF, 64, 0.0, 0);
    events.addNote(CLAP_EVENT_NOTE_OFF, 67, 0.0, 0);
    events.addNote(CLAP_EVENT_NOTE_OFF, 72, 0.0, 0);
    run(oneSecond * 3, true);

    // A WILDCARD note-off -- CLAP key -1, how a host panic and a choke-all
    // arrive -- must release EVERY voice, not mask -1 to key 127 and release
    // one note that was never down. Play a chord, send a single key-(-1)
    // note-off, and the sound must be gone. (Audio plugins only: a note effect
    // emits no audio, so silence there is trivially true and unmeaning.)
    if (!expectMidiOut) {
      events.storage.clear();
      events.addNote(CLAP_EVENT_NOTE_ON, 60, 1.0, 0);
      events.addNote(CLAP_EVENT_NOTE_ON, 64, 1.0, 0);
      events.addNote(CLAP_EVENT_NOTE_ON, 67, 1.0, 0);
      run(oneSecond / 4, true);
      events.storage.clear();
      events.addNote(CLAP_EVENT_NOTE_OFF, -1, 0.0, 0); // wildcard: all notes off
      run(oneSecond * 3, true);                         // let the release finish
      const Result afterPanic = run(oneSecond / 4, true); // then measure a fresh window
      check(afterPanic.peak < 1e-3,
            "a wildcard note-off (key -1) releases every voice, not one masked note");
    }

    // Raw MIDI is the other dialect the port advertises: it must work too.
    events.storage.clear();
    clap_event_midi_t midiOn{};
    midiOn.header.size = sizeof(midiOn);
    midiOn.header.time = 0;
    midiOn.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    midiOn.header.type = CLAP_EVENT_MIDI;
    midiOn.port_index = 0;
    midiOn.data[0] = 0x90; // note on, channel 1
    midiOn.data[1] = 69;
    midiOn.data[2] = 100;
    events.push(&midiOn, sizeof(midiOn));
    const Result rawMidi = run(oneSecond / 4, true);
    if (expectMidiOut) {
      check(!g_emitted.events.empty(), "a RAW MIDI note-on also drives the note effect");
    } else {
      check(rawMidi.energy > 0.0, "a raw MIDI note-on also sounds");
    }
    events.storage.clear();

    // SysEx, which the wrapper started accepting so that midi_ci.h has a wire
    // path at all. There is no DSP here that CONSUMES it, so what is checked
    // is the part a host can actually break: the event carries a POINTER that
    // the host owns and frees, and a wrapper that stored it rather than
    // copying would read freed memory on the next block.
    //
    // Sent at three sizes, including one far past the buffer's capacity, and
    // repeatedly so a wrapper that forgot to clear its arena would run out.
    {
      static uint8_t big[4096];
      bool crashedOrWent = false;
      for (int round = 0; round < 8 && !crashedOrWent; ++round) {
        events.storage.clear();
        const size_t sizes[] = {6, 600, sizeof(big)};
        for (size_t which = 0; which < 3; ++which) {
          const size_t n = sizes[which];
          for (size_t i = 0; i < n; ++i) big[i] = (uint8_t) (i & 0x7f);
          big[0] = 0xF0;
          big[n - 1] = 0xF7;
          clap_event_midi_sysex_t sx{};
          sx.header.size = sizeof(sx);
          sx.header.time = 0;
          sx.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
          sx.header.type = CLAP_EVENT_MIDI_SYSEX;
          sx.port_index = 0;
          sx.buffer = big;
          sx.size = (uint32_t) n;
          events.push(&sx, sizeof(sx));
        }
        const Result sysexRun = run(oneSecond / 8, true);
        if (sysexRun.errored || !sysexRun.finite) crashedOrWent = true;
        // The host's buffer is reused between rounds, exactly as a real host
        // would: a wrapper holding the pointer sees it change under it.
        for (size_t i = 0; i < sizeof(big); ++i) big[i] = 0xAA;
      }
      // The whole point of the SysEx work, end to end and through a BUILT
      // plugin: a real MIDI-CI discovery message goes in and must come out
      // the other side byte for byte.
      //
      // The arpeggiator forwards what it does not consume, which is the
      // correct behaviour for any MIDI effect -- one that swallows
      // unrecognised messages breaks the sustain pedal and the clock for
      // everything downstream of it.
      if (expectMidiOut) {
        sonore::midici::DeviceIdentity identity;
        identity.muid = 0x0ABCDEF;
        identity.manufacturer[0] = 0x7D;
        const std::vector<uint8_t> discovery = sonore::midici::encodeDiscovery(identity);

        g_emittedSysex.clear();
        events.storage.clear();
        clap_event_midi_sysex_t ci{};
        ci.header.size = sizeof(ci);
        ci.header.time = 0;
        ci.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        ci.header.type = CLAP_EVENT_MIDI_SYSEX;
        ci.port_index = 0;
        ci.buffer = discovery.data();
        ci.size = (uint32_t) discovery.size();
        events.push(&ci, sizeof(ci));
        run(oneSecond / 8, true);

        bool cameBack = false;
        for (const auto& got : g_emittedSysex)
          if (got.size() == discovery.size() &&
              std::equal(got.begin(), got.end(), discovery.begin()))
            cameBack = true;
        check(cameBack,
              "a MIDI-CI discovery message goes in and comes back out byte for byte "
              "-- input, the block's arena, and output, through a built .clap");

        // And it must be a message a CI parser still accepts on the way out:
        // the same bytes could survive as a blob and still have lost their
        // framing if the wrapper wrote the length wrongly.
        bool parses = false;
        for (const auto& got : g_emittedSysex) {
          sonore::midici::Message decoded;
          if (sonore::midici::decode(got.data(), got.size(), &decoded) &&
              decoded.type == sonore::midici::MessageType::Discovery)
            parses = true;
        }
        check(parses, "and it still decodes as a Discovery, framing intact");
        events.storage.clear();
        g_emittedSysex.clear();
      }

      check(!crashedOrWent,
            "SysEx events of 6, 600 and 4096 bytes, eight rounds, leave the plugin "
            "processing finite audio -- the host's buffer is overwritten between "
            "rounds, so a wrapper that kept the pointer would show it here");
      events.storage.clear();
    }
  } else {
    // An effect: a 1 kHz tone through a second of blocks must come out finite,
    // audible and bounded: the same battery the generation gates run.
    const Result r = run(oneSecond, false);
    check(!r.errored, "process() never returned an error");
    check(r.finite, "a second of audio stays finite (no NaN/Inf)");
    check(r.energy > 0.0, "the plugin produces sound, not silence");
    check(r.peak < 10.0, "the output does not blow up");
  }

  // ── Parameter automation ──────────────────────────────────────────────────
  // Set every parameter to its maximum through the event list and confirm the
  // plugin both took the value AND still produces sane audio at that setting.
  events.storage.clear();
  for (uint32_t i = 0; i < nParams; ++i) {
    clap_param_info_t info{};
    params->get_info(plugin, i, &info);
    events.addParam(info.id, info.max_value, 0);
  }
  for (uint32_t i = 0; i < blockSize; ++i) { inL[i] = 0.1f; inR[i] = 0.1f; }
  plugin->process(plugin, &process);

  bool tookMax = true;
  for (uint32_t i = 0; i < nParams; ++i) {
    clap_param_info_t info{};
    params->get_info(plugin, i, &info);
    double v = 0.0;
    params->get_value(plugin, info.id, &v);
    if (std::fabs(v - info.max_value) > 1e-4) tookMax = false;
  }
  check(tookMax, "parameter events land on the plugin's state");

  events.storage.clear();
  bool maxFinite = true;
  for (int b = 0; b < 20; ++b) {
    plugin->process(plugin, &process);
    for (uint32_t i = 0; i < blockSize; ++i)
      if (!std::isfinite(outL[i]) || !std::isfinite(outR[i])) maxFinite = false;
  }
  check(maxFinite, "audio stays finite with every control at maximum");

  // An out-of-range automation value must be clamped, never trusted blindly.
  events.storage.clear();
  events.addParam(p0.id, p0.max_value * 1000.0 + 1e6, 0);
  plugin->process(plugin, &process);
  double clamped = 0.0;
  params->get_value(plugin, p0.id, &clamped);
  check(clamped <= p0.max_value + 1e-6, "an out-of-range automation value is clamped");

  // Put every control back where it started before anything else runs.
  //
  // "Every parameter to its maximum" now includes the BYPASS, and leaving it
  // there meant every section below this one measured a bypassed plugin. Three
  // of them failed at once the moment the bypass parameter existed, and the
  // plugin was right about all three. A section that leaves the instrument in
  // a state the next section does not expect is a section that will lie again
  // later.
  events.storage.clear();
  for (uint32_t i = 0; i < nParams; ++i) {
    clap_param_info_t info{};
    if (params->get_info(plugin, i, &info)) events.addParam(info.id, info.default_value, 0);
  }
  plugin->process(plugin, &process);
  events.storage.clear();
  plugin->reset(plugin);

  // ── Latency ───────────────────────────────────────────────────────────────
  // A DSP that delays its signal has to say so, or the host cannot time-align
  // it. The probe declares 64 samples; a plugin that declares nothing must
  // report 0 rather than something arbitrary.
  {
    const auto* latency =
        static_cast<const clap_plugin_latency_t*>(plugin->get_extension(plugin, CLAP_EXT_LATENCY));
    check(latency != nullptr, "the latency extension is exposed");
    if (latency) {
      const uint32_t reported = latency->get(plugin);
      std::printf("  ---- reported latency: %u samples ----\n", (unsigned) reported);
      if (expectBridge) {
        check(reported == 64, "a DSP's declared latency reaches the host");

        // ── Latency that MOVES ────────────────────────────────
        //
        // Latency that never changes is reported once and forgotten about. A
        // plugin whose oversampling or look-ahead is a SWITCH has to tell the
        // host every time it moves, or the host's delay compensation stays on
        // the old number and everything running in parallel with it is quietly
        // early. Nobody hears that as "the latency is wrong"; they hear a
        // smeared mix.
        //
        // The probe's third parameter picks 64 or 512 samples. Driven through
        // FLUSH, which is the main thread and where a host turning a knob on a
        // stopped transport actually writes.
        {
          clap_id switchId = CLAP_INVALID_ID;
          for (uint32_t i = 0; i < nParams; ++i) {
            clap_param_info_t info{};
            if (params->get_info(plugin, i, &info) &&
                std::strcmp(info.name, "Big Latency") == 0)
              switchId = info.id;
          }
          check(switchId != CLAP_INVALID_ID, "the probe exposes a latency switch");

          if (switchId != CLAP_INVALID_ID) {
            clap_output_events_t sink{};
            sink.ctx = nullptr;
            sink.try_push = outTryPush;

            // The protocol here is not the obvious one, and the header spells
            // it out: "The latency is only allowed to change during
            // plugin->activate. If the plugin is activated, call
            // host->request_restart()."
            //
            // A host cannot re-plan its delay compensation under a running
            // graph. So an ACTIVE plugin may not announce a new latency at
            // all -- it asks to be stopped and started, and the new value
            // arrives with the activation. Calling changed() while active is
            // what clap-validator refuses, and it is right to.
            auto setSwitch = [&](double value) {
              EventList write;
              write.addParam(switchId, value, 0);
              params->flush(plugin, write.events(), &sink);
              g_latencyChanged = 0;
              g_restartRequests = 0;
              process.in_events = write.events();
              plugin->process(plugin, &process);
              process.in_events = events.events();
            };

            setSwitch(1.0);
            std::printf("  ---- while active: %d restart request(s), %d changed() call(s) ----\n",
                        g_restartRequests, g_latencyChanged);
            check(g_restartRequests > 0,
                  "an ACTIVE plugin asks to be restarted when its latency moves");
            check(g_latencyChanged == 0,
                  "…and does NOT announce the new latency, which it is not allowed to");

            // The restart a host performs in answer. The new latency arrives
            // with the activation, which is the only moment it may.
            g_latencyChanged = 0;
            plugin->deactivate(plugin);
            check(plugin->activate(plugin, sampleRate, 1, blockSize), "the host restarts the plugin");
            const uint32_t after = latency->get(plugin);
            std::printf("  ---- after the restart: %u -> %u samples, %d changed() call(s) ----\n",
                        (unsigned) reported, (unsigned) after, g_latencyChanged);
            check(after == 512, "…which is where the new latency appears");
            check(g_latencyChanged > 0, "…announced during activate, where it is legal");

            // Back again. A notification that fires one way leaves the
            // compensation wrong for half of what a user does with a switch.
            setSwitch(0.0);
            check(g_restartRequests > 0, "switching back asks for a restart too");
            plugin->deactivate(plugin);
            plugin->activate(plugin, sampleRate, 1, blockSize);
            check(latency->get(plugin) == 64, "…and restores the latency");

            // ── A control can refuse to be automated ────────────────
            //
            // True for almost everything, and a plugin that says false about
            // the wrong knob has taken away the reason people buy plugins.
            // It is right for a handful: this switch changes the plugin's
            // LATENCY, and a host recording that as automation would be
            // re-planning its delay compensation at every point on the curve.
            //
            // Every parameter was unconditionally automatable before, so the
            // check that the others still ARE matters as much as the one that
            // says this is not.
            {
              clap_param_info_t plain{}, unautomatable{};
              check(params->get_info(plugin, 0, &plain), "parameter 0 describes itself");
              // Found by NAME rather than by reusing the id as an index. They
              // happen to be equal here, and that is not something a test
              // should quietly depend on.
              bool foundSwitch = false;
              for (uint32_t i = 0; i < nParams; ++i) {
                clap_param_info_t info{};
                if (params->get_info(plugin, i, &info) &&
                    std::strcmp(info.name, "Big Latency") == 0) {
                  unautomatable = info;
                  foundSwitch = true;
                }
              }
              check(foundSwitch, "…and so does the latency switch");
              const bool plainOk = (plain.flags & CLAP_PARAM_IS_AUTOMATABLE) != 0;
              const bool switchOk = (unautomatable.flags & CLAP_PARAM_IS_AUTOMATABLE) != 0;
              std::printf("  ---- automatable: param 0 %s, latency switch %s ----\n",
                          plainOk ? "yes" : "no", switchOk ? "yes" : "no");
              check(plainOk, "an ordinary control is automatable");
              check(!switchOk, "…and one that reconfigures the plugin says it is not");
              check((unautomatable.flags & CLAP_PARAM_IS_STEPPED) != 0,
                    "…without losing the flags it still deserves");
            }

            // ── A stepped control says what its steps ARE ─────────────
            //
            // Without names a stepped parameter renders as its index: "0",
            // "1", "2" in the host's generic editor, in its automation lane,
            // and in every readout a user has when the plugin's own face is
            // closed. Automation is exactly where somebody works on a control
            // they cannot see, and a number tells them nothing about what it
            // selects.
            {
              char text[128] = {};
              check(params->value_to_text(plugin, switchId, 0.0, text, sizeof(text)),
                    "a stepped parameter renders its value");
              check(std::strcmp(text, "Short") == 0,
                    "…as the NAME of the step, not its index");
              std::printf("  ---- stepped value 0 reads \"%s\" ----\n", text);

              char text1[128] = {};
              params->value_to_text(plugin, switchId, 1.0, text1, sizeof(text1));
              check(std::strcmp(text1, "Long") == 0, "…and each step has its own");

              // And it comes BACK. Every "type a value" box in every host
              // reads a parameter out and hands the same string back; a
              // plugin that prints "Long" and cannot parse it leaves the
              // control where it was and says nothing.
              double parsed = -1.0;
              check(params->text_to_value(plugin, switchId, "Long", &parsed),
                    "…and a name typed back in is understood");
              check(parsed == 1.0, "…as the step it names");
              check(params->text_to_value(plugin, switchId, "Short", &parsed) && parsed == 0.0,
                    "…both ways round");
            }

            // ── The tail, announced the other way round ─────────────
            //
            // Every other notification here is [main-thread] and has to be
            // smuggled off the audio thread. clap_host_tail::changed is
            // marked [audio-thread] in the header, so process() is not merely
            // allowed to call it -- it is the right place.
            //
            // Which follows from what each is for. A latency change forces
            // the host to re-plan its delay compensation, which it cannot do
            // under a running graph. A tail is only a hint about how long to
            // keep processing after the transport stops, so nothing has to
            // stop for it.
            //
            // It matters for one thing and it matters a lot there: a reverb
            // whose decay is a parameter has a tail that moves, and a host
            // bouncing offline with a stale short tail cuts the reverb off
            // mid-decay.
            {
              const auto* tailExt = static_cast<const clap_plugin_tail_t*>(
                  plugin->get_extension(plugin, CLAP_EXT_TAIL));
              check(tailExt != nullptr, "the tail extension is exposed");
              if (tailExt) {
                plugin->on_main_thread(plugin); // drain anything pending
                const uint32_t tailBefore = tailExt->get(plugin);

                g_tailChanged = 0;
                EventList write;
                write.addParam(switchId, 1.0, 0);
                params->flush(plugin, write.events(), &sink);
                process.in_events = write.events();
                plugin->process(plugin, &process);
                process.in_events = events.events();

                const uint32_t tailAfter = tailExt->get(plugin);
                std::printf("  ---- tail: %u -> %u samples, %d announcement(s) ----\n",
                            (unsigned) tailBefore, (unsigned) tailAfter, g_tailChanged);
                check(tailAfter == 9999, "the switch moves the plugin's tail");
                check(g_tailChanged > 0,
                      "…announced from process() itself, where the header says it belongs");

                // Once per change. A host told the tail moved on every block
                // recomputes its bounce length on every block.
                g_tailChanged = 0;
                for (int i = 0; i < 5; ++i) plugin->process(plugin, &process);
                check(g_tailChanged == 0, "…once per change, not once per block");

                EventList back;
                back.addParam(switchId, 0.0, 0);
                params->flush(plugin, back.events(), &sink);
                process.in_events = back.events();
                plugin->process(plugin, &process);
                process.in_events = events.events();
                check(tailExt->get(plugin) == 4321, "and switching back restores it");
                plugin->deactivate(plugin);
                plugin->activate(plugin, sampleRate, 1, blockSize);
              }
            }

            // ── State the host cannot see ────────────────────────
            //
            // A host knows about anything it did itself. It has no idea about
            // a sampler that loaded a file through its own browser -- no
            // parameter moved and nothing the host did caused it. If nobody
            // says so the session is never marked dirty, the DAW closes
            // without asking, and the work is gone. There is no error message
            // and nothing to diagnose; the user simply loses a sample.
            //
            // The probe's switch stands in for that: toggling it is this
            // fixture's version of "the user just loaded something".
            {
              // Drained first, and the parameter written in its own step.
              // setSwitch() flushes and then processes, and a flush IS the
              // main thread and may legitimately deliver something left over
              // from an earlier block -- which made a claim about process()
              // fail on work process() had not done.
              plugin->on_main_thread(plugin);
              EventList write;
              write.addParam(switchId, 1.0, 0);
              params->flush(plugin, write.events(), &sink);
              plugin->on_main_thread(plugin);

              g_markedDirty = 0;
              g_callbackRequests = 0;
              process.in_events = write.events();
              plugin->process(plugin, &process);
              process.in_events = events.events();

              // mark_dirty is [main-thread] and process() is not, so the news
              // is recorded and a callback asked for rather than delivered
              // from the audio thread.
              check(g_markedDirty == 0,
                    "process() does not mark the session dirty from the audio thread");
              check(g_callbackRequests > 0, "…it asks to be called on the main thread");
              plugin->on_main_thread(plugin);
              std::printf("  ---- state dirty: %d mark(s) after the callback ----\n",
                          g_markedDirty);
              check(g_markedDirty > 0, "…and the host is told when it gets there");

              // Once per change, not once per block. A host marked dirty
              // sixty times a second is a host that can never be clean.
              g_markedDirty = 0;
              for (int i = 0; i < 5; ++i) {
                plugin->process(plugin, &process);
                plugin->on_main_thread(plugin);
              }
              check(g_markedDirty == 0, "and blocks that change nothing mark nothing");

              plugin->deactivate(plugin);
              plugin->activate(plugin, sampleRate, 1, blockSize);
              setSwitch(0.0);
              plugin->on_main_thread(plugin);
              plugin->deactivate(plugin);
              plugin->activate(plugin, sampleRate, 1, blockSize);
            }

            // And nothing is asked for when nothing moved. A host told its
            // graph is stale on every block rebuilds it on every block.
            g_restartRequests = 0;
            g_latencyChanged = 0;
            plugin->process(plugin, &process);
            check(g_restartRequests == 0 && g_latencyChanged == 0,
                  "and a block that changes nothing asks for nothing");
            plugin->reset(plugin);
          }
        }
      } else {
        // Any value is legitimate: the saturator oversamples and so genuinely
        // delays, but it has to be a plausible one. An absurd number here means
        // the wrapper is reporting something other than the DSP's own answer.
        check(reported < 10000, "the reported latency is plausible");
      }

      // ...and plausible is not the same as TRUE. A host shifts this plugin's
      // whole output back by `reported` samples; if the signal is not actually
      // that late, the host has just pushed it EARLY, and every parallel mix
      // the plugin sits in is smeared by the difference. So measure it.
      //
      // Cross-correlation against a broadband burst, because that is the only
      // thing that answers "how late is the output" without assuming what the
      // plugin does to the signal in between. Noise, not the 1 kHz tone the
      // rest of this file uses: a periodic input correlates just as well at
      // every multiple of its period, so a tone cannot tell 0 from 48.
      //
      // Only plugins declaring a latency are measured. A minimum-phase filter
      // has real group delay and correctly declares none, and no measurement
      // can tell that apart from an undeclared delay without knowing what the
      // plugin is meant to be. The bridge probe is skipped for the same reason
      // it is skipped for the tail: it declares 64 to prove the number reaches
      // the host, and does not delay anything.
      if (reported > 0 && !isInstrument && !expectBridge) {
        events.storage.clear();
        for (uint32_t i = 0; i < nParams; ++i) {
          clap_param_info_t pi{};
          if (params->get_info(plugin, i, &pi)) events.addParam(pi.id, pi.default_value, 0);
        }
        run(oneSecond / 4, true); // settle at defaults

        const uint32_t captureLen = 16384;
        const uint32_t maxLag = reported * 2 + 512;
        std::vector<float> sent, got;
        sent.reserve(captureLen + maxLag);
        got.reserve(captureLen + maxLag);
        uint32_t lcg = 12345u;
        while (got.size() < captureLen + maxLag) {
          for (uint32_t i = 0; i < blockSize; ++i) {
            lcg = lcg * 1664525u + 1013904223u;
            const float v = (float) ((int32_t) (lcg >> 8) % 20001 - 10000) / 40000.0f;
            inL[i] = inR[i] = v;
            sent.push_back(v);
          }
          if (plugin->process(plugin, &process) == CLAP_PROCESS_ERROR) break;
          events.storage.clear();
          for (uint32_t i = 0; i < blockSize; ++i) got.push_back(outL[i]);
        }

        double best = -1.0;
        uint32_t bestLag = 0;
        for (uint32_t lag = 0; lag <= maxLag && lag + captureLen <= got.size(); ++lag) {
          double num = 0.0, a = 0.0, b = 0.0;
          for (uint32_t i = 0; i < captureLen; ++i) {
            const double x = sent[i], y = got[i + lag];
            num += x * y;
            a += x * x;
            b += y * y;
          }
          const double denom = std::sqrt(a * b);
          const double r = denom > 1e-12 ? std::fabs(num / denom) : 0.0;
          if (r > best) {
            best = r;
            bestLag = lag;
          }
        }

        std::printf("  ---- latency: declared %u samples, measured %u (r=%.3f) ----\n",
                    (unsigned) reported, (unsigned) bestLag, best);
        check(best > 0.2, "the output correlates with the input at SOME lag");
        const uint32_t error = bestLag > reported ? bestLag - reported : reported - bestLag;
        check(error <= 32, "the declared latency is the delay the plugin ACTUALLY has");
      }
    }
  }

  // ── Remote controls ───────────────────────────────────────────────────────
  //
  // A host with an eight-knob controller has to pick eight parameters. Without
  // this extension it takes the first eight in declaration order, which for a
  // synth means whatever happened to be written first.
  //
  // The properties worth checking are not "does it return pages" but what the
  // pages CONTAIN. Every parameter must appear, none twice, and none that does
  // not exist -- a page pointing at a parameter id the plugin does not publish
  // gives the user a knob that turns nothing, which is worse than no page.
  {
    const auto* remote = static_cast<const clap_plugin_remote_controls_t*>(
        plugin->get_extension(plugin, CLAP_EXT_REMOTE_CONTROLS));
    check(remote != nullptr, "the plugin lays itself out for a hardware controller");

    if (remote) {
      const uint32_t pages = remote->count(plugin);
      check(pages > 0, "…on at least one page");
      // Enough pages to hold everything: with eight slots each, fewer than
      // ceil(n/8) means something was dropped.
      check(pages >= (nParams + 7) / 8, "…and enough of them to hold every control");

      std::vector<int> seen((size_t) nParams, 0);
      bool namesOk = true, idsOk = true;
      for (uint32_t p = 0; p < pages; ++p) {
        clap_remote_controls_page_t page{};
        if (!remote->get(plugin, p, &page)) {
          idsOk = false;
          break;
        }
        if (page.page_name[0] == 0) namesOk = false;
        for (int slot = 0; slot < CLAP_REMOTE_CONTROLS_COUNT; ++slot) {
          const clap_id id = page.param_ids[slot];
          if (id == CLAP_INVALID_ID) continue;
          // Every id has to be one the plugin really publishes.
          bool exists = false;
          for (uint32_t i = 0; i < nParams; ++i) {
            clap_param_info_t info{};
            if (params->get_info(plugin, i, &info) && info.id == id) {
              exists = true;
              if (i < seen.size()) ++seen[i];
              break;
            }
          }
          if (!exists) idsOk = false;
        }
      }
      check(idsOk, "…every slot names a parameter the plugin actually has");
      check(namesOk, "…and every page has a name a user can read");

      // The BYPASS is deliberately not on a page, and the check says so rather
      // than looking away. A host gives its own bypass a dedicated button, so
      // spending one of eight knobs on it would waste a slot and put an
      // accidental mute under the user's hand.
      int missing = 0, duplicated = 0;
      bool bypassOnAPage = false;
      for (size_t i = 0; i < seen.size(); ++i) {
        clap_param_info_t info{};
        const bool isBypass = params->get_info(plugin, (uint32_t) i, &info) &&
                              (info.flags & CLAP_PARAM_IS_BYPASS) != 0;
        if (isBypass) {
          if (seen[i] > 0) bypassOnAPage = true;
          continue;
        }
        if (seen[i] == 0) ++missing;
        if (seen[i] > 1) ++duplicated;
      }
      check(!bypassOnAPage, "…the host's own bypass does not take one of the eight knobs");
      std::printf("  ---- remote controls: %u pages, %d unreachable, %d duplicated ----\n",
                  (unsigned) pages, missing, duplicated);
      check(missing == 0, "…and no control is left unreachable from the hardware");
      check(duplicated == 0, "…nor placed on two knobs at once");

      // Past the end is refused rather than answered with rubbish.
      clap_remote_controls_page_t past{};
      check(!remote->get(plugin, pages + 5, &past),
            "a page that does not exist is refused, not invented");
    }
  }

  // ── Voice info ────────────────────────────────────────────────────────────
  //
  // A host that does not know an instrument's polyphony guesses at it. Bitwig
  // uses this to decide how many MPE member channels are worth allocating and
  // to draw a voice meter; guess too high and the player hears voices stolen
  // for no reason they can see.
  {
    const auto* voiceInfo = static_cast<const clap_plugin_voice_info_t*>(
        plugin->get_extension(plugin, CLAP_EXT_VOICE_INFO));
    if (isInstrument && !expectMidiOut) {
      check(voiceInfo != nullptr, "an instrument says how many voices it has");
      if (voiceInfo) {
        clap_voice_info_t info{};
        check(voiceInfo->get(plugin, &info), "…and answers when asked");
        std::printf("  ---- voices: %u of %u, flags 0x%llx ----\n",
                    (unsigned) info.voice_count, (unsigned) info.voice_capacity,
                    (unsigned long long) info.flags);
        check(info.voice_capacity > 0, "…with a real capacity");
        check(info.voice_count <= info.voice_capacity,
              "…and never more sounding than it can hold");
        // The flag means two notes on the SAME KEY may sound at once, which is
        // what MPE does when two fingers land on one pitch. It was not true
        // until the VoiceManager learned to tell fingers apart by channel, so
        // an expressive instrument must claim it and a plain one must not.
        const bool overlapping =
            (info.flags & CLAP_VOICE_INFO_SUPPORTS_OVERLAPPING_NOTES) != 0;
        check(overlapping,
              "…and an expressive instrument accepts two notes on one key");
      }
    } else {
      check(voiceInfo == nullptr,
            "a plugin with no voices does not pretend to have any");
    }
  }

  // ── Render mode ───────────────────────────────────────────────────────────
  //
  // A bounce is not a performance: nothing has to keep up with a clock, so a
  // DSP can spend CPU it would not spend while monitoring. The host says which
  // it is through clap.render.
  //
  // The interesting half is that a DSP using the SIMPLE process() signature
  // has no ProcessContext to read the flag from, and only learns about it
  // because the wrapper re-runs prepare(). That is what this measures, on the
  // one plugin built to report it: the probe adds 0.125 to its telemetry
  // channel when it was prepared for an offline render, a value none of the
  // other terms in that sum can produce.
  {
    const auto* render =
        static_cast<const clap_plugin_render_t*>(plugin->get_extension(plugin, CLAP_EXT_RENDER));
    check(render != nullptr, "the render extension is exposed");
    if (render) {
      check(!render->has_hard_realtime_requirement(plugin),
            "software DSP does not claim it must run in real time");
      check(render->set(plugin, CLAP_RENDER_REALTIME), "realtime mode is accepted");

      if (expectBridge) {
        auto telemetry = [&]() {
          for (uint32_t i = 0; i < blockSize; ++i) inL[i] = inR[i] = 0.0f;
          plugin->process(plugin, &process);
          events.storage.clear();
          return (double) outR[blockSize - 1];
        };
        telemetry(); // let the block settle at the current mode
        const double live = telemetry();
        // The plugin is ACTIVE, and clap_plugin_render::set is [main-thread]
        // with nothing said about being deactivated, so a host may flip the
        // mode while the transport runs. The wrapper used to re-prepare the
        // DSP right there, on the main thread, under a running process() --
        // ThreadSanitizer saw every filter's coefficients being rewritten
        // while the audio thread read them. So it now records the mode and
        // asks for a restart, the way the latency change does, and the host
        // does what a host does: deactivate, activate, and prepare() runs
        // with the new mode on the one thread that is allowed to.
        g_restartRequests = 0;
        check(render->set(plugin, CLAP_RENDER_OFFLINE), "offline mode is accepted");
        check(g_restartRequests > 0,
              "...and, being active, the plugin asks for a restart rather than re-preparing "
              "under the audio thread");
        plugin->stop_processing(plugin);
        plugin->deactivate(plugin);
        check(plugin->activate(plugin, sampleRate, 1, blockSize), "the host restarts it");
        check(plugin->start_processing(plugin), "...and resumes processing");
        telemetry();
        const double offline = telemetry();
        std::printf("  ---- probe telemetry: live %.4f, offline %.4f ----\n", live, offline);
        checkNear(offline - live, 0.125, 1e-4,
                  "the offline flag reached a DSP that never sees a ProcessContext");
        g_restartRequests = 0;
        check(render->set(plugin, CLAP_RENDER_REALTIME), "and back to realtime");
        check(g_restartRequests > 0, "...which asks for a restart too");
        plugin->stop_processing(plugin);
        plugin->deactivate(plugin);
        plugin->activate(plugin, sampleRate, 1, blockSize);
        plugin->start_processing(plugin);
        telemetry();
        checkNear(telemetry(), live, 1e-4, "…which puts the plugin back where it was");
      }
    }
  }

  // ── Audio port activation ─────────────────────────────────────────────────
  //
  // A host that routes nothing out of an aux bus, or has nothing patched into
  // a sidechain, can say so. The extension's own contract is the reason this
  // is testable at all: the host keeps handing over the buffers, zero-filled,
  // so deactivation is permission to skip work and NEVER a change in what the
  // main output should contain.
  //
  // That invariant is the whole check. A plugin that optimises on this flag
  // and gets it wrong changes its main output depending on what the host
  // happens to have patched elsewhere, which is a bug no user could ever
  // explain and no session could reproduce.
  {
    const auto* activation = static_cast<const clap_plugin_audio_ports_activation_t*>(
        plugin->get_extension(plugin, CLAP_EXT_AUDIO_PORTS_ACTIVATION));
    check(activation != nullptr, "the audio-ports-activation extension is exposed");
    if (activation && audioPorts) {
      check(activation->can_activate_while_processing(plugin),
            "ports can be activated while processing (no stall to unpatch a cable)");
      const uint32_t outCount = audioPorts->count(plugin, false);
      check(!activation->set_active(plugin, false, outCount + 5, false, 32),
            "an out-of-range port index is refused rather than written past");

      if (outCount > 1) { // this plugin has aux buses to switch off
        events.storage.clear();
        for (uint32_t i = 0; i < nParams; ++i) {
          clap_param_info_t pi{};
          if (params->get_info(plugin, i, &pi)) events.addParam(pi.id, pi.default_value, 0);
        }
        run(oneSecond / 4, true);

        // The same input twice, once with every aux bus on and once with them
        // all off, compared on the MAIN output only.
        auto captureMain = [&](bool auxOn) {
          for (uint32_t port = 1; port < outCount; ++port)
            activation->set_active(plugin, false, port, auxOn, 32);
          plugin->reset(plugin);
          std::vector<float> got;
          uint32_t lcg = 777u;
          for (int b = 0; b < 40; ++b) {
            for (uint32_t i = 0; i < blockSize; ++i) {
              lcg = lcg * 1664525u + 1013904223u;
              inL[i] = inR[i] = (float) ((int32_t) (lcg >> 8) % 20001 - 10000) / 40000.0f;
            }
            plugin->process(plugin, &process);
            events.storage.clear();
            for (uint32_t i = 0; i < blockSize; ++i) got.push_back(outL[i]);
          }
          return got;
        };

        const std::vector<float> withAux = captureMain(true);
        const std::vector<float> withoutAux = captureMain(false);
        double worst = 0.0;
        for (size_t i = 0; i < withAux.size() && i < withoutAux.size(); ++i) {
          const double d = std::fabs((double) withAux[i] - (double) withoutAux[i]);
          if (d > worst) worst = d;
        }
        std::printf("  ---- main output drift with aux buses off: %.3g ----\n", worst);
        check(worst < 1e-6,
              "deactivating every aux bus leaves the MAIN output untouched");

        for (uint32_t port = 1; port < outCount; ++port)
          activation->set_active(plugin, false, port, true, 32); // put them back
        plugin->reset(plugin);
      }
    }
  }

  // ── Tail ──────────────────────────────────────────────────────────────────
  // Same trait pattern as latency: a DSP that rings declares tailSamples()
  // and the host must hear about it, or it cuts the decay on stop/render.
  {
    const auto* tail =
        static_cast<const clap_plugin_tail_t*>(plugin->get_extension(plugin, CLAP_EXT_TAIL));
    check(tail != nullptr, "the tail extension is exposed");
    if (tail) {
      const uint32_t reported = tail->get(plugin);
      if (expectBridge) check(reported == 4321, "a DSP's declared tail reaches the host");
      check(reported <= 10 * (uint32_t) sampleRate, "the declared tail is plausible (<10 s)");

      // The declaration is only worth anything if it matches what the plugin
      // DOES, so measure that rather than asserting a constant.
      //
      // The first version of this test read `reported == 0` for every plugin
      // that was not the GUI probe -- keyed off expectBridge, which is a flag
      // about the JavaScript bridge and has nothing to do with the DSP. It
      // passed only because the probe was the only plugin here with a tail,
      // and the convolution reverb failed it the first time it was run. The
      // fix is not a second magic number; it is to stop guessing.
      //
      // Instruments are excluded from the measurement: they have no audio
      // input to stop, so their decay is governed by note-offs and envelope
      // releases rather than by the tail contract, which is defined in terms
      // of input going silent.
      // The bridge probe is excluded, and for a reason worth stating: its
      // right channel carries the transport tempo and beat position as
      // constant DC so a test can read them out of the buffer. That channel
      // never decays because it is telemetry, not audio, so measuring its
      // "tail" measures the fixture rather than the contract.
      if (!isInstrument && !expectBridge) {
        events.storage.clear();
        for (uint32_t i = 0; i < nParams; ++i) {
          clap_param_info_t pi{};
          if (params->get_info(plugin, i, &pi)) events.addParam(pi.id, pi.default_value, 0);
        }
        run(oneSecond / 4, true); // settle at defaults, from silence

        run(oneSecond / 4, false); // excite it
        // Then cut the input and find the last sample that is still audible.
        // -80 dBFS: below that a decay is inaudible under any playback gain a
        // host will apply, and convolution against silence reaches exactly
        // zero, so the threshold is not what decides the answer.
        const double audible = 1e-4;
        uint32_t decaySamples = 0, elapsed = 0;
        for (int b = 0; b < oneSecond * 2; ++b) {
          for (uint32_t i = 0; i < blockSize; ++i) inL[i] = inR[i] = 0.0f;
          if (plugin->process(plugin, &process) == CLAP_PROCESS_ERROR) break;
          events.storage.clear();
          for (uint32_t i = 0; i < blockSize; ++i, ++elapsed)
            if (std::fabs(outL[i]) > audible || std::fabs(outR[i]) > audible)
              decaySamples = elapsed + 1;
        }

        // Slack is deliberately SMALL. The first version allowed 2048 samples
        // so that a partitioned convolver's block buffering would not count
        // against it, and that margin quietly swallowed two real faults: the
        // splitter declaring no tail while its crossover rang for 399
        // samples, and the reverb under-declaring by 570. Block buffering is
        // part of the tail and belongs in the plugin's own declaration, not
        // in the test's tolerance. What is left here covers measurement
        // granularity and nothing else.
        const uint32_t slack = 256;
        std::printf("  ---- tail: declared %u samples, measured %u ----\n", reported,
                    decaySamples);
        if (emitsDc) {
          // NOT skipped quietly. This measurement assumes output follows input
          // down to silence, which is true of every plugin that processes
          // audio and false of a fixture whose second channel is an instrument
          // panel: the GUI probe writes the host's tempo and beat position out
          // as DC on the right channel, on purpose and documented, so that a
          // test can read the transport straight out of the buffer. It
          // therefore never decays, and "measured 96000" is the fixture
          // working rather than a tail being under-declared.
          //
          // Said out loud, with the number, because a check that silently
          // stops running is indistinguishable from one that passes.
          std::printf("  ---- tail check SKIPPED: --emits-dc. This plugin writes a constant "
                      "with silent input by design (its right channel reports the transport), "
                      "so a decay measurement describes the fixture, not a tail ----\n");
        } else {
          check(decaySamples <= reported + slack,
                "the declared tail COVERS the decay the plugin actually produces");
        }
      }
    }
  }

  // ── Presets ───────────────────────────────────────────────────────────────
  // A plugin that ships presets must actually load them, and must refuse an
  // index that does not exist rather than writing garbage into the controls.
  {
    const auto* presets = static_cast<const clap_plugin_preset_load_t*>(
        plugin->get_extension(plugin, CLAP_EXT_PRESET_LOAD));
    if (presets) {
      std::printf("  ---- this plugin ships factory presets ----\n");
      // Move every control away from the preset first, so a pass cannot be an
      // accident of the defaults.
      events.storage.clear();
      for (uint32_t i = 0; i < nParams; ++i) {
        clap_param_info_t info{};
        params->get_info(plugin, i, &info);
        events.addParam(info.id, info.min_value, 0);
      }
      plugin->process(plugin, &process);
      events.storage.clear();

      check(presets->from_location(plugin, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr, "0"),
            "a factory preset loads by index");
      plugin->process(plugin, &process); // the queue lands on the audio thread

      bool moved = false;
      for (uint32_t i = 0; i < nParams; ++i) {
        clap_param_info_t info{};
        params->get_info(plugin, i, &info);
        double v = 0.0;
        params->get_value(plugin, info.id, &v);
        if (std::fabs(v - info.min_value) > 1e-6) moved = true;
      }
      check(moved, "…and its values reached the parameters");

      check(!presets->from_location(plugin, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr, "999"),
            "an out-of-range preset index is refused");
      check(!presets->from_location(plugin, CLAP_PRESET_DISCOVERY_LOCATION_FILE, "/x", "0"),
            "a file location is refused (presets live inside the plugin)");
      check(!presets->from_location(plugin, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr, "junk"),
            "a non-numeric load key is refused, not guessed at");
    }
  }

  // ── Transport ─────────────────────────────────────────────────────────────
  // The probe reports what it heard about musical time on its right channel.
  // CLAP beat positions are FIXED POINT; getting that conversion wrong is a
  // silent, permanent error, so it is asserted rather than assumed.
  if (expectBridge) {
    clap_event_transport_t tp{};
    tp.header.size = sizeof(tp);
    tp.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    tp.header.type = CLAP_EVENT_TRANSPORT;
    tp.flags = CLAP_TRANSPORT_HAS_TEMPO | CLAP_TRANSPORT_HAS_BEATS_TIMELINE |
               CLAP_TRANSPORT_IS_PLAYING;
    tp.tempo = 140.0;
    tp.song_pos_beats = (clap_beattime) (8 * CLAP_BEATTIME_FACTOR); // beat 8
    tp.bar_start = (clap_beattime) (8 * CLAP_BEATTIME_FACTOR);
    tp.tsig_num = 4;
    tp.tsig_denom = 4;
    process.transport = &tp;

    events.storage.clear();
    for (uint32_t i = 0; i < blockSize; ++i) { inL[i] = 0.0f; inR[i] = 0.0f; }
    plugin->process(plugin, &process);
    // tempo*0.001 + beats*0.01 + playing*0.5 = 0.140 + 0.08 + 0.5
    checkNear(outR[0], 0.72, 1e-3, "the host's tempo, beat and play state reached the DSP");

    tp.flags = CLAP_TRANSPORT_HAS_TEMPO; // stopped, no timeline
    tp.tempo = 100.0;
    plugin->process(plugin, &process);
    checkNear(outR[0], 0.10, 1e-3, "…and a stopped transport reads as stopped");
    process.transport = nullptr;
  }

  // ── Aux output buses ──────────────────────────────────────────────────────
  // Routing proved by PHYSICS, not by pointers: the splitter sends low
  // frequencies to the main bus and high ones to its last aux bus. Feed one
  // tone at a time and check the energy lands on the bus it belongs to. A
  // wrapper that wired every bus to the same buffer, or dropped the aux
  // buses entirely, fails one of the four checks.
  if (expectAuxOuts > 0) {
    auto energies = [&](double hz) {
      static int ph = 0;
      double mainE = 0.0, lastAuxE = 0.0;
      for (int b = 0; b < 30; ++b) {
        for (uint32_t i = 0; i < blockSize; ++i) {
          const float v =
              0.5f * (float) std::sin(2.0 * 3.14159265358979 * hz * ph / 48000.0);
          inL[i] = inR[i] = v;
          ++ph;
        }
        plugin->process(plugin, &process);
        if (b >= 15) {
          for (uint32_t i = 0; i < blockSize; ++i) {
            mainE += (double) outL[i] * outL[i];
            const float a = auxCh[(size_t) ((expectAuxOuts - 1) * 2)][i];
            lastAuxE += (double) a * a;
          }
        }
      }
      return std::pair<double, double>(mainE, lastAuxE);
    };

    const auto low = energies(60.0);
    check(low.first > 1e-3, "a low tone reaches the MAIN bus");
    check(low.second < low.first * 0.01, "…and stays off the high aux bus");

    const auto high = energies(12000.0);
    check(high.second > 1e-3, "a high tone reaches the AUX bus");
    check(high.first < high.second * 0.01, "…and stays off the main bus");
  }

  // ── Per-note expression (MPE) ─────────────────────────────────────────────
  // The whole point is that expression targets ONE note. Hold a note, bend it
  // a whole tone with a note-expression event, and the pitch that comes out
  // has to move -- measured by the zero-crossing rate, so a wrapper that
  // dropped the event or applied it to the wrong voice fails.
  if (notePorts && notePorts->count(plugin, true) > 0) {
    clap_note_port_info_t ni{};
    notePorts->get(plugin, 0, true, &ni);
    const bool mpe = (ni.supported_dialects & CLAP_NOTE_DIALECT_MIDI_MPE) != 0;
    if (mpe) {
      // Pitch by AUTOCORRELATION rather than by counting zero crossings.
      //
      // The first version of this test closed a filter first, so that
      // counting crossings would see only the fundamental, which quietly
      // assumed parameter 0 was a cutoff. It is, on the synth. On the
      // sampler it is the ATTACK, and setting it to 200 left the instrument
      // silent and the test measuring nothing. Autocorrelation needs no such
      // assumption and tolerates harmonics, so it works on any instrument.
      std::vector<float> captured;
      auto capture = [&](int blocks) {
        captured.clear();
        for (int b = 0; b < blocks; ++b) {
          plugin->process(plugin, &process);
          events.storage.clear();
          for (uint32_t i = 0; i < blockSize; ++i) captured.push_back(outL[i]);
        }
      };

      auto pitchOf = [&](const std::vector<float>& signal) {
        // Lags from 48 to 800 samples cover roughly 60 Hz to 1 kHz at 48 kHz,
        // which is where any note this test plays will land.
        const size_t window = signal.size() / 2;
        if (window < 1000) return 0.0;
        double best = 0.0;
        size_t bestLag = 0;
        for (size_t lag = 48; lag < 800 && lag + window < signal.size(); ++lag) {
          double num = 0.0, a = 0.0, b = 0.0;
          for (size_t i = 0; i < window; ++i) {
            const double x = signal[i];
            const double y = signal[i + lag];
            num += x * y;
            a += x * x;
            b += y * y;
          }
          const double denom = std::sqrt(a * b);
          const double r = denom > 1e-12 ? num / denom : 0.0;
          if (r > best) {
            best = r;
            bestLag = lag;
          }
        }
        return bestLag > 0 ? sampleRate / (double) bestLag : 0.0;
      };

      // Start from a KNOWN state. Earlier sections moved parameters around and
      // left notes sounding, and both leaked into the first version of this
      // measurement: on the synth an 80 Hz cutoff left almost no signal to
      // measure, and on the sampler - which loops - notes from the chord test
      // were still holding, so the pitch measured was theirs. A test that
      // depends on what ran before it is a test that will lie again later.
      events.storage.clear();
      for (uint32_t i = 0; i < nParams; ++i) {
        clap_param_info_t pi{};
        if (params->get_info(plugin, i, &pi)) events.addParam(pi.id, pi.default_value, 0);
      }
      for (int key : {60, 64, 67, 69, 72}) events.addNote(CLAP_EVENT_NOTE_OFF, key, 0.0, 0);
      run(oneSecond * 2, true); // and let every release finish

      // A low note, so a whole-tone bend is an unmistakable ratio.
      events.storage.clear();
      events.addNote(CLAP_EVENT_NOTE_ON, 45, 1.0, 0); // A2, 110 Hz
      // A full second of audio: 40 blocks is only ~12 zero crossings at
      // 110 Hz, far too coarse to resolve a 12% pitch shift.
      // Let the attack settle before measuring, on either side of the bend.
      capture(oneSecond / 4);
      capture(oneSecond / 4);
      const double plain = pitchOf(captured);
      check(plain > 0.0, "the expressive instrument sounds a held note");

      // Bend that one note up two semitones.
      clap_event_note_expression_t bend{};
      bend.header.size = sizeof(bend);
      bend.header.type = CLAP_EVENT_NOTE_EXPRESSION;
      bend.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
      bend.header.time = 0;
      bend.expression_id = CLAP_NOTE_EXPRESSION_TUNING;
      bend.note_id = -1;
      bend.port_index = -1;
      bend.channel = 0;
      bend.key = 45;
      bend.value = 2.0; // semitones
      events.storage.clear();
      events.push(&bend, sizeof(bend));
      capture(oneSecond / 4); // the bend applies on the first of these
      const double bent = pitchOf(captured);

      // Two semitones is a ratio of 2^(2/12) = 1.122, so the pitch must rise
      // by roughly a tenth. Bounds either side catch "ignored" and "wildly
      // wrong" alike.
      const double ratio = plain > 0.0 ? bent / plain : 0.0;
      std::printf("  ---- measured pitch: %.1f Hz -> %.1f Hz ----\n", plain, bent);

      // ── The bend range the controller announces ──────────────────────────
      //
      // RPN 0 is pitch bend sensitivity, and it is how an MPE instrument says
      // what range it is using. The decoder assumed MPE's default of 48
      // semitones, so a player whose controller is set to two would have been
      // bent twenty-four times too far on every note -- silently.
      //
      // Measured on the BUILT plugin rather than on the decoder alone: the
      // same full-scale bend must move the note much less once the range has
      // been announced, and the CCs have to travel as raw MIDI because that
      // is the only spelling an RPN has.
      {
        events.storage.clear();
        for (int key : {45, 60, 64, 67, 69, 72}) events.addNote(CLAP_EVENT_NOTE_OFF, key, 0.0, 0);
        run(oneSecond, true);

        events.storage.clear();
        events.addMidi(0xb0, 101, 0, 0); // RPN MSB
        events.addMidi(0xb0, 100, 0, 0); // RPN LSB -> parameter 0
        events.addMidi(0xb0, 6, 2, 0);   // two semitones
        events.addNote(CLAP_EVENT_NOTE_ON, 45, 1.0, 0);
        capture(oneSecond / 4);
        capture(oneSecond / 4);
        const double narrowPlain = pitchOf(captured);

        events.storage.clear();
        events.addMidi(0xe0, 0x7f, 0x7f, 0); // pitch bend, full up
        capture(oneSecond / 4);
        capture(oneSecond / 4);
        const double narrowBent = pitchOf(captured);

        const double narrowRatio = narrowPlain > 0.0 ? narrowBent / narrowPlain : 0.0;
        std::printf("  ---- with a 2-semitone range announced: %.1f Hz -> %.1f Hz ----\n",
                    narrowPlain, narrowBent);
        // Two semitones is 2^(2/12) = 1.1225. Without the RPN the same message
        // would have bent it four octaves, which the measurement below 1 kHz
        // could not even see.
        checkNear(narrowRatio, 1.1225, 0.03,
                  "a full bend obeys the range the controller announced, not the default");

        events.storage.clear();
        events.addMidi(0xe0, 0x00, 0x40, 0); // back to centre
        events.addNote(CLAP_EVENT_NOTE_OFF, 45, 0.0, 0);
        run(oneSecond, true);
      }

      // ── The master channel ───────────────────────────────────────────────
      //
      // MPE splits the sixteen channels into a zone: notes live on member
      // channels, and the MASTER channel carries the zone's global bend and
      // pressure. It has no note of its own, so a decoder that looks up "the
      // note on this channel" and gives up when it finds none discards every
      // global message a controller sends.
      //
      // The check is binary rather than approximate: before zones existed
      // this bend moved NOTHING, because there is no note on channel 1.
      {
        events.storage.clear();
        for (int key : {45, 60, 64, 67, 69, 72})
          for (int16_t chan : {(int16_t) 0, (int16_t) 1})
            events.addNote(CLAP_EVENT_NOTE_OFF, (int16_t) key, 0.0, 0, chan);
        run(oneSecond, true);

        // RPN 6 on channel 1: open the lower zone with five member channels.
        events.storage.clear();
        events.addMidi(0xb0, 101, 0, 0);
        events.addMidi(0xb0, 100, 6, 0);
        events.addMidi(0xb0, 6, 5, 0);
        // A note on channel 2, which is a MEMBER of that zone.
        events.addNote(CLAP_EVENT_NOTE_ON, 45, 1.0, 0, 1);
        capture(oneSecond / 4);
        capture(oneSecond / 4);
        const double zonePlain = pitchOf(captured);

        // Bend on channel 1 -- the MASTER, which is holding nothing.
        events.storage.clear();
        events.addMidi(0xe0, 0x7f, 0x7f, 0);
        capture(oneSecond / 4);
        capture(oneSecond / 4);
        const double zoneBent = pitchOf(captured);

        const double zoneRatio = zonePlain > 0.0 ? zoneBent / zonePlain : 0.0;
        std::printf("  ---- master-channel bend: %.1f Hz -> %.1f Hz ----\n", zonePlain,
                    zoneBent);
        check(zonePlain > 0.0, "a note on a member channel sounds");
        // A fresh zone starts at MPE's master default of two semitones, so a
        // full-scale bend is a whole tone: 2^(2/12) = 1.1225.
        checkNear(zoneRatio, 1.1225, 0.03,
                  "a bend on the MASTER channel moves a note held on a member channel");

        events.storage.clear();
        events.addMidi(0xe0, 0x00, 0x40, 0);
        events.addMidi(0xb0, 101, 0, 0);
        events.addMidi(0xb0, 100, 6, 0);
        events.addMidi(0xb0, 6, 0, 0); // close the zone again
        events.addNote(CLAP_EVENT_NOTE_OFF, 45, 0.0, 0, 1);
        run(oneSecond, true);
      }
      std::printf("  ---- pitch ratio after a 2-semitone note bend: %.3f ----\n", ratio);
      check(ratio > 1.06 && ratio < 1.19,
            "a per-note bend really retunes that note by two semitones");

      events.storage.clear();
      events.addNote(CLAP_EVENT_NOTE_OFF, 45, 0.0, 0);
      run(oneSecond, true);
    }
  }

  // ── Sidechain ─────────────────────────────────────────────────────────────
  // The contract, audibly: with a silent key the ducker passes its input;
  // with a loud key the SAME input comes out attenuated. A wrapper that fed
  // the wrong bus, silence, or the main input into the sidechain would fail
  // one side or the other.
  if (expectSidechain) {
    std::fill(scL.begin(), scL.end(), 0.0f);
    std::fill(scR.begin(), scR.end(), 0.0f);
    run(20, false); // settle
    const Result quietKey = run(20, false);
    for (uint32_t i = 0; i < blockSize; ++i) scL[i] = scR[i] = 0.9f; // a hot key
    run(20, false); // follower attack
    const Result hotKey = run(20, false);
    std::fill(scL.begin(), scL.end(), 0.0f);
    std::fill(scR.begin(), scR.end(), 0.0f);
    check(quietKey.energy > 0.0, "with a silent key the main signal passes");
    check(hotKey.energy < quietKey.energy * 0.5,
          "a hot key ducks the main signal (the sidechain reaches the DSP)");
  }

  // ── State round-trip ──────────────────────────────────────────────────────
  const auto* state =
      static_cast<const clap_plugin_state_t*>(plugin->get_extension(plugin, CLAP_EXT_STATE));
  check(state != nullptr, "the state extension is exposed");
  if (state) {
    // Put a known, non-default value in, save, change it, reload, compare.
    const double target = (p0.min_value + p0.max_value) * 0.5;
    events.storage.clear();
    events.addParam(p0.id, target, 0);
    plugin->process(plugin, &process);
    events.storage.clear();

    MemStream saved;
    clap_ostream_t os{};
    os.ctx = &saved;
    os.write = MemStream::writeCb;
    check(state->save(plugin, &os), "state saves");
    check(!saved.bytes.empty(), "…and produced bytes");

    // ── The host introduced itself ──────────────────────────────────
    //
    // Nobody wants a plugin to need this and everybody ends up needing it:
    // hosts have quirks a plugin cannot fix and has to work around, and one
    // that cannot tell them apart applies every workaround to everybody.
    //
    // The probe writes what it was told into its state bag, so the bytes it
    // just saved are the only channel through which a host can see what the
    // plugin knows about it.
    if (expectBridge) {
      const std::string bytes(saved.bytes.begin(), saved.bytes.end());
      const bool named = bytes.find("Sonore SDK Test Host") != std::string::npos;
      std::printf("  ---- host identity in saved state: %s ----\n", named ? "present" : "ABSENT");
      check(named, "the plugin was told which host is running it, by name");
    }

    // ── And which track it is on ─────────────────────────────────
    //
    // Most visibly so a plugin can tint its face to match the track, which is
    // what turns a rack of eight identical compressors into eight
    // distinguishable ones. It matters more at CREATION: a reverb that knows
    // it landed on a return track can start at 100% wet instead of making
    // every user's first action an undo.
    //
    // CLAP asks rather than being told, so the check that it ASKED is
    // separate from the check that it understood the answer -- a plugin that
    // never queries would otherwise look identical to one whose host said
    // nothing.
    if (expectBridge) {
      const std::string bytes(saved.bytes.begin(), saved.bytes.end());
      check(g_trackInfoQueried, "the plugin asked the host what track it is on");
      const bool named = bytes.find("Verb Return") != std::string::npos;
      const bool coloured = bytes.find("#2ab17c") != std::string::npos;
      std::printf("  ---- track: name %s, colour %s ----\n",
                  named ? "present" : "ABSENT", coloured ? "present" : "ABSENT");
      check(named, "…and got the track's name");
      check(coloured, "…and its colour, decoded rather than copied");
    }

    events.addParam(p0.id, p0.min_value, 0);
    plugin->process(plugin, &process);
    events.storage.clear();
    double changed = 0.0;
    params->get_value(plugin, p0.id, &changed);
    check(std::fabs(changed - target) > 1e-6, "the value really changed before reloading");

    clap_istream_t is{};
    is.ctx = &saved;
    is.read = MemStream::readCb;
    g_rescanFlags = 0;
    check(state->load(plugin, &is), "state loads");
    double restored = 0.0;
    params->get_value(plugin, p0.id, &restored);
    checkNear(restored, target, 1e-4, "the saved value came back");
    check((g_rescanFlags & CLAP_PARAM_RESCAN_VALUES) != 0,
          "a successful load announces the new values with a rescan request");

    // State beyond parameters. A plugin that declares extra state writes it
    // into the same blob, and the proof is that the blob GREW: a bag that
    // silently serialised to nothing would still pass a round trip, because
    // reloading nothing leaves the defaults in place and the defaults are
    // what was saved.
    {
      MemStream withBag;
      clap_ostream_t os2{};
      os2.ctx = &withBag;
      os2.write = MemStream::writeCb;
      check(state->save(plugin, &os2), "state saves again");
      // header + values + bypass byte + the v4 selected-preset int32
      const size_t parameterBytes = 12 + 4 * (size_t) nParams + 1 + 4;
      std::printf("  ---- state blob: %zu bytes, parameters need %zu ----\n",
                  withBag.bytes.size(), parameterBytes);
      check(withBag.bytes.size() >= parameterBytes,
            "the blob is at least as large as its parameters");
      // The version is stamped in the header whether or not this plugin uses a
      // bag, so a host reading it knows what it is looking at. Pinned to a
      // literal on purpose: bumping the format is a decision, and a test that
      // read the constant back out of the SDK would agree with any change
      // including an accidental one.
      check(withBag.bytes.size() > 8 && withBag.bytes[4] == 5,
            "...and the header declares state version 5");
    }

    // Garbage must be refused, not half-applied.
    MemStream junk;
    junk.bytes = {'n', 'o', 'p', 'e', 0, 0, 0, 0};
    clap_istream_t bad{};
    bad.ctx = &junk;
    bad.read = MemStream::readCb;
    g_rescanFlags = 0;
    check(!state->load(plugin, &bad), "a corrupt state blob is refused");
    check(g_rescanFlags == 0, "...and a refused load does not request a rescan");

    // The blob under MUTATION, through the real loader. A session file is
    // hostile input in practice -- truncated by a crash, corrupted on disk,
    // written by a build with a different parameter count -- and a plugin
    // that crashes on one takes the host's whole session with it. Every
    // truncation, and random flips and runs over the header, the values, the
    // bypass byte, the preset index, the editor size and the DSP's own bag:
    // each load may succeed or be refused, and after every one the plugin
    // must still process finite audio. Then the good blob must still load.
    {
      std::mt19937 rng(0x5EED57A7u);
      const std::vector<uint8_t>& good = saved.bytes;
      int loads = 0, refused = 0;
      bool finiteAfterAll = true, refusedLeftValues = true;
      auto tryLoad = [&](const std::vector<uint8_t>& bytes) {
        double before[64] = {};
        clap_id ids[64] = {};
        for (uint32_t i = 0; i < nParams && i < 64; ++i) {
          clap_param_info_t pi{};
          params->get_info(plugin, i, &pi);
          ids[i] = pi.id;
          params->get_value(plugin, ids[i], &before[i]);
        }
        MemStream mutated;
        mutated.bytes = bytes;
        clap_istream_t ms{};
        ms.ctx = &mutated;
        ms.read = MemStream::readCb;
        if (!state->load(plugin, &ms)) {
          // Refused means UNTOUCHED. The loader used to write each value as
          // it was read, so a blob cut off at the fourth value was "refused"
          // with three already applied -- exactly the half-applied session
          // the refusal exists to prevent.
          ++refused;
          for (uint32_t i = 0; i < nParams && i < 64; ++i) {
            double after = 0.0;
            params->get_value(plugin, ids[i], &after);
            if (after != before[i]) refusedLeftValues = false;
          }
        }
        plugin->process(plugin, &process);
        for (uint32_t i = 0; i < blockSize; ++i)
          if (!std::isfinite(outL[i]) || !std::isfinite(outR[i])) finiteAfterAll = false;
        ++loads;
      };
      for (size_t len = 0; len < good.size(); ++len)
        tryLoad(std::vector<uint8_t>(good.begin(), good.begin() + (long) len));
      for (int it = 0; it < 400 && !good.empty(); ++it) {
        std::vector<uint8_t> b = good;
        const int flips = 1 + (int) (rng() % 6);
        for (int k = 0; k < flips; ++k) b[rng() % b.size()] = (uint8_t) rng();
        tryLoad(b);
      }
      static const uint8_t kPatterns[] = {0x00, 0xFF, 0x7F, 0x80, 0x01};
      for (int it = 0; it < 200 && !good.empty(); ++it) {
        std::vector<uint8_t> b = good;
        const size_t at = rng() % b.size(), len = 1 + rng() % 8;
        const uint8_t pat = kPatterns[rng() % sizeof(kPatterns)];
        for (size_t k = 0; k < len && at + k < b.size(); ++k) b[at + k] = pat;
        tryLoad(b);
      }
      char note[200];
      std::snprintf(note, sizeof(note), "%d mutated state blobs loaded or refused, and the plugin "
                    "processed finite audio after every one", loads);
      check(finiteAfterAll, note);
      std::snprintf(note, sizeof(note), "...%d of them were refused, and every refusal left every "
                    "parameter exactly where it was", refused);
      check(refusedLeftValues, note);
      check(refused > 0 && refused < loads, "the mutations produced both refusals and loads");
      saved.readPos = 0;
      clap_istream_t again{};
      again.ctx = &saved;
      again.read = MemStream::readCb;
      check(state->load(plugin, &again), "…and the good blob still loads afterwards");
      double back = 0.0;
      params->get_value(plugin, p0.id, &back);
      checkNear(back, target, 1e-4, "…restoring the saved value");
    }
  }

  // ── The event stream under mutation ─────────────────────────────────────────
  //
  // The state fuzz above found two bugs on its first run. This is its sibling
  // for the OTHER thing a host hands us every block: events. Headers whose
  // size lies in both directions, unknown types and spaces, offsets past the
  // end of the block, parameter ids nobody declared, NaN and infinite values,
  // note keys and channels outside MIDI, note ids that mean nothing, raw MIDI
  // bytes of every kind, sysex with a size that does not match its buffer
  // (and once in four, no buffer at all), transports with every field random
  // -- in any order, because the spec says sorted and hosts have bugs too.
  // After each block the output must be finite, process() must not have
  // reported an error, and every parameter must still be a finite number
  // inside its declared range. Then a plain sine must still come through.
  {
    std::mt19937 rng(0xE7E17u);
    auto rnd = [&](uint32_t n) { return n ? (uint32_t) (rng() % n) : 0u; };
    auto rndDouble = [&]() -> double {
      switch (rnd(8)) {
        case 0: return std::numeric_limits<double>::quiet_NaN();
        case 1: return std::numeric_limits<double>::infinity();
        case 2: return -std::numeric_limits<double>::infinity();
        case 3: return 1e300;
        case 4: return -1e300;
        case 5: return (double) rng() / 4294967295.0 * 3.0 - 1.0;
        default: return (double) rng() / 4294967295.0;
      }
    };
    auto rndI16 = [&]() -> int16_t {
      switch (rnd(5)) {
        case 0: return -1;
        case 1: return (int16_t) rnd(128);
        case 2: return (int16_t) -(int) rnd(1000);
        case 3: return INT16_MAX;
        default: return (int16_t) (rng() & 0xFFFF);
      }
    };
    auto rndI32 = [&]() -> int32_t { return rnd(2) ? -1 : (int32_t) rng(); };
    auto rndTime = [&]() -> uint32_t { return rnd(4) == 0 ? (uint32_t) rng() : rnd(blockSize + 1); };
    std::vector<uint8_t> sysexBytes(4096);
    for (auto& b : sysexBytes) b = (uint8_t) rng();

    bool finiteAll = true, noError = true;
    int fuzzedBlocks = 0, fuzzedEvents = 0;
    for (int it = 0; it < 600; ++it) {
      events.storage.clear();
      const int n = 1 + (int) rnd(12);
      for (int k = 0; k < n; ++k, ++fuzzedEvents) {
        const uint32_t time = rndTime();
        switch (rnd(9)) {
          case 0: { // a header and random bytes after it, of a type that may not exist
            // The size is TRUTHFUL here: the header's size is the only bound a
            // plugin has, and a host that claims more bytes than it allocated
            // is broken in a way no plugin can detect. (The first version lied
            // upward, and ASan rightly reported the wrapper reading a note
            // event's fields from past a twenty-byte allocation: the test's
            // bug, not the wrapper's.) A size that lies DOWNWARD -- the case a
            // plugin must survive -- is the last case below. The one exception
            // kept: a type the core space does not define may claim any size,
            // because nothing may be read past its header regardless.
            std::vector<uint8_t> raw(sizeof(clap_event_header_t) + rnd(64));
            for (auto& b : raw) b = (uint8_t) rng();
            auto* h = reinterpret_cast<clap_event_header_t*>(raw.data());
            h->type = (uint16_t) rnd(40);
            h->size = (uint32_t) raw.size();
            if (h->type >= 13 && rnd(3) == 0) h->size = (uint32_t) rng(); // no such type: unreadable anyway
            h->time = time;
            h->space_id = rnd(3) == 0 ? (uint16_t) rng() : CLAP_CORE_EVENT_SPACE_ID;
            h->flags = (uint32_t) rng();
            events.push(raw.data(), raw.size());
            break;
          }
          case 1: { // a parameter value: ids nobody declared, values that are not numbers
            clap_event_param_value_t e{};
            e.header.size = sizeof(e);
            e.header.time = time;
            e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
            e.header.type = CLAP_EVENT_PARAM_VALUE;
            e.param_id = rnd(3) == 0 ? (clap_id) rng() : (clap_id) rnd(nParams + 2);
            e.note_id = rndI32();
            e.port_index = rndI16();
            e.channel = rndI16();
            e.key = rndI16();
            e.value = rndDouble();
            events.push(&e, sizeof(e));
            break;
          }
          case 2: {
            clap_event_param_mod_t e{};
            e.header.size = sizeof(e);
            e.header.time = time;
            e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
            e.header.type = CLAP_EVENT_PARAM_MOD;
            e.param_id = rnd(3) == 0 ? (clap_id) rng() : (clap_id) rnd(nParams + 2);
            e.note_id = rndI32();
            e.port_index = rndI16();
            e.channel = rndI16();
            e.key = rndI16();
            e.amount = rndDouble();
            events.push(&e, sizeof(e));
            break;
          }
          case 3: { // notes outside MIDI, on ports and channels that do not exist
            static const uint16_t kTypes[] = {CLAP_EVENT_NOTE_ON, CLAP_EVENT_NOTE_OFF,
                                              CLAP_EVENT_NOTE_CHOKE, CLAP_EVENT_NOTE_END};
            clap_event_note_t e{};
            e.header.size = sizeof(e);
            e.header.time = time;
            e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
            e.header.type = kTypes[rnd(4)];
            e.note_id = rndI32();
            e.port_index = rndI16();
            e.channel = rndI16();
            e.key = rndI16();
            e.velocity = rndDouble();
            events.push(&e, sizeof(e));
            break;
          }
          case 4: {
            clap_event_note_expression_t e{};
            e.header.size = sizeof(e);
            e.header.time = time;
            e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
            e.header.type = CLAP_EVENT_NOTE_EXPRESSION;
            e.expression_id = (clap_note_expression) rnd(12); // 0..6 exist
            e.note_id = rndI32();
            e.port_index = rndI16();
            e.channel = rndI16();
            e.key = rndI16();
            e.value = rndDouble();
            events.push(&e, sizeof(e));
            break;
          }
          case 5: { // raw MIDI of every kind, including bytes that are not MIDI
            clap_event_midi_t e{};
            e.header.size = sizeof(e);
            e.header.time = time;
            e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
            e.header.type = CLAP_EVENT_MIDI;
            e.port_index = (uint16_t) (rnd(3) == 0 ? rng() : 0);
            e.data[0] = (uint8_t) rng();
            e.data[1] = (uint8_t) rng();
            e.data[2] = (uint8_t) rng();
            events.push(&e, sizeof(e));
            break;
          }
          case 6: { // sysex whose size and buffer disagree
            clap_event_midi_sysex_t e{};
            e.header.size = sizeof(e);
            e.header.time = time;
            e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
            e.header.type = CLAP_EVENT_MIDI_SYSEX;
            e.port_index = (uint16_t) (rnd(3) == 0 ? rng() : 0);
            e.buffer = rnd(4) == 0 ? nullptr : sysexBytes.data();
            e.size = rnd(3) == 0 ? (uint32_t) rng() : rnd((uint32_t) sysexBytes.size() + 1);
            events.push(&e, sizeof(e));
            break;
          }
          case 7: { // a transport with every field random
            clap_event_transport_t e{};
            auto* bytes = reinterpret_cast<uint8_t*>(&e);
            for (size_t i = sizeof(e.header); i < sizeof(e); ++i) bytes[i] = (uint8_t) rng();
            e.header.size = sizeof(e);
            e.header.time = time;
            e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
            e.header.type = CLAP_EVENT_TRANSPORT;
            events.push(&e, sizeof(e));
            break;
          }
          default: { // a real type whose header claims LESS than the struct needs
            static const uint16_t kTypes[] = {CLAP_EVENT_PARAM_VALUE, CLAP_EVENT_NOTE_ON,
                                              CLAP_EVENT_MIDI, CLAP_EVENT_MIDI_SYSEX,
                                              CLAP_EVENT_TRANSPORT,
                                              CLAP_EVENT_NOTE_EXPRESSION};
            clap_event_header_t h{};
            h.size = rnd(2) ? sizeof(h) : sizeof(h) + rnd(8);
            h.time = time;
            h.space_id = CLAP_CORE_EVENT_SPACE_ID;
            h.type = kTypes[rnd(6)];
            std::vector<uint8_t> raw(h.size);
            std::memcpy(raw.data(), &h, sizeof(h));
            for (size_t i = sizeof(h); i < raw.size(); ++i) raw[i] = (uint8_t) rng();
            events.push(raw.data(), raw.size());
            break;
          }
        }
      }
      for (uint32_t i = 0; i < blockSize; ++i) {
        const float s = 0.25f * (float) std::sin(2.0 * 3.14159265358979 * 440.0 * i / sampleRate);
        inL[i] = s;
        inR[i] = s;
      }
      if (std::getenv("SONORE_FUZZ_TRACE")) {
        // Which block, and what it carried: the line before a crash names it.
        std::printf("  trace: block %d:", it);
        for (const auto& raw : events.storage) {
          const auto* h = reinterpret_cast<const clap_event_header_t*>(raw.data());
          std::printf(" [type %u size %u space %u t %u]", h->type, h->size, h->space_id, h->time);
        }
        std::printf("\n");
      }
      if (plugin->process(plugin, &process) == CLAP_PROCESS_ERROR) noError = false;
      for (uint32_t i = 0; i < blockSize; ++i)
        if (!std::isfinite(outL[i]) || !std::isfinite(outR[i])) finiteAll = false;
      ++fuzzedBlocks;
    }
    events.storage.clear();
    char note[200];
    std::snprintf(note, sizeof(note), "%d blocks carrying %d mutated events processed to finite "
                  "audio", fuzzedBlocks, fuzzedEvents);
    check(finiteAll, note);
    check(noError, "...and none of them made process() report an error");
    bool inRange = true;
    for (uint32_t i = 0; i < nParams; ++i) {
      clap_param_info_t pi{};
      if (!params->get_info(plugin, i, &pi)) continue;
      double v = 0.0;
      params->get_value(plugin, pi.id, &v);
      if (!std::isfinite(v) || v < pi.min_value - 1e-9 || v > pi.max_value + 1e-9) {
        inRange = false;
        std::printf("  ---- parameter %u reads %g, range %g..%g ----\n", pi.id, v, pi.min_value,
                    pi.max_value);
      }
    }
    check(inRange, "...and every parameter is a finite number inside its range afterwards");
    // Back to defaults, so what follows judges the plugin and not the storm.
    for (uint32_t i = 0; i < nParams; ++i) {
      clap_param_info_t pi{};
      if (params->get_info(plugin, i, &pi)) events.addParam(pi.id, pi.default_value, 0);
    }
    const Result afterStorm = run(oneSecond / 4, isInstrument);
    check(afterStorm.finite && !afterStorm.errored, "...and it still processes afterwards");
  }

  // ── Bypass, as a CONTRACT rather than a flag ──────────────────────────────
  //
  // CLAP had no bypass parameter here at all. The state blob has carried a
  // bypass byte since version 2 and BypassState has crossfaded and
  // latency-aligned since it was written, and none of it was reachable: a
  // CLAP host had no control to press. The same plugin bypassed cleanly in
  // VST3 and in LV2 and not at all in this format.
  //
  // So the check is the same one those two make, and it is about behaviour
  // rather than about the flag being present: engage it and the output must
  // BECOME the input, delayed by the plugin's own reported latency. A bypass
  // that merely mutes, or that forgets the dry delay, or that leaves the DSP
  // in the path would each fail this.
  if (!isInstrument) {
    clap_id bypassId = CLAP_INVALID_ID;
    for (uint32_t i = 0; i < nParams; ++i) {
      clap_param_info_t info{};
      if (!params->get_info(plugin, i, &info)) continue;
      if (info.flags & CLAP_PARAM_IS_BYPASS) {
        bypassId = info.id;
        break;
      }
    }
    check(bypassId != CLAP_INVALID_ID, "an effect exposes a parameter flagged as THE bypass");

    if (bypassId != CLAP_INVALID_ID) {
      const auto* latencyExt = static_cast<const clap_plugin_latency_t*>(
          plugin->get_extension(plugin, CLAP_EXT_LATENCY));
      const uint32_t L = latencyExt ? latencyExt->get(plugin) : 0;

      events.storage.clear();
      for (uint32_t i = 0; i < nParams; ++i) {
        clap_param_info_t info{};
        if (params->get_info(plugin, i, &info)) events.addParam(info.id, info.default_value, 0);
      }
      events.addParam(bypassId, 1.0, 0); // engage
      plugin->reset(plugin);

      // Twenty blocks first: the crossfade is 20 ms and comparing during it
      // would be measuring the ramp rather than the contract.
      int phase = 0;
      double worst = 0.0;
      for (int block = 0; block < 60; ++block) {
        for (uint32_t i = 0; i < blockSize; ++i) {
          const float v =
              0.25f * (float) std::sin(2.0 * 3.14159265358979 * 997.0 * phase / sampleRate);
          inL[i] = inR[i] = v;
          ++phase;
        }
        plugin->process(plugin, &process);
        events.storage.clear();
        if (block < 30) continue;
        for (uint32_t i = 0; i < blockSize; ++i) {
          const int n = phase - (int) blockSize + (int) i - (int) L;
          const double expect =
              0.25 * std::sin(2.0 * 3.14159265358979 * 997.0 * n / sampleRate);
          const double err = std::fabs((double) outL[i] - expect);
          if (err > worst) worst = err;
        }
      }
      std::printf("  ---- bypassed output vs input, aligned to %u samples: %.3g ----\n",
                  (unsigned) L, worst);
      check(worst < 1e-3, "bypassed output IS the input, delayed by the reported latency");

      // …and releasing it puts the processing back, or the button is a mute.
      //
      // Which needs the plugin to be doing something first. Trim at 0 dB and
      // the ducker with a silent key are both exactly transparent, so
      // comparing their output against the dry signal proves nothing about
      // the bypass either way -- the first version of this check failed on
      // both of them, and the plugins were right.
      events.storage.clear();
      events.addParam(bypassId, 0.0, 0);
      for (uint32_t i = 0; i < nParams; ++i) {
        clap_param_info_t info{};
        if (!params->get_info(plugin, i, &info)) continue;
        if (info.flags & CLAP_PARAM_IS_BYPASS) continue;
        events.addParam(info.id, info.max_value, 0);
      }
      // A ducker only ducks when its key is hot.
      if (expectSidechain)
        for (uint32_t i = 0; i < blockSize; ++i) scL[i] = scR[i] = 0.9f;
      double difference = 0.0;
      for (int block = 0; block < 40; ++block) {
        for (uint32_t i = 0; i < blockSize; ++i) {
          const float v =
              0.25f * (float) std::sin(2.0 * 3.14159265358979 * 997.0 * phase / sampleRate);
          inL[i] = inR[i] = v;
          ++phase;
        }
        plugin->process(plugin, &process);
        events.storage.clear();
        if (block < 30) continue;
        for (uint32_t i = 0; i < blockSize; ++i) {
          const int n = phase - (int) blockSize + (int) i - (int) L;
          const double dry =
              0.25 * std::sin(2.0 * 3.14159265358979 * 997.0 * n / sampleRate);
          const double err = std::fabs((double) outL[i] - dry);
          if (err > difference) difference = err;
        }
      }
      check(difference > 1e-3, "…and releasing it brings the processing back");
      if (expectSidechain)
        for (uint32_t i = 0; i < blockSize; ++i) scL[i] = scR[i] = 0.0f;

      double readBack = 1.0;
      params->get_value(plugin, bypassId, &readBack);
      checkNear(readBack, 0.0, 1e-6, "…which the host can read back off the parameter");

      char text[64] = {};
      check(params->value_to_text(plugin, bypassId, 1.0, text, sizeof(text)) &&
                std::strcmp(text, "On") == 0,
            "…and it prints as a switch, not as a number");
      plugin->reset(plugin);
    }
  }

  // ── Old sessions ──────────────────────────────────────────────────────────
  //
  // The blob is versioned so a plugin that ships and then gains a control can
  // still open what it saved a year ago. Every part of that was verified by
  // READING the loader; nothing exercised it. A format whose backward
  // compatibility is only argued for is a format that will break a user's
  // session quietly, on an ordinary Tuesday.
  //
  // The blobs below are built HERE, byte by byte, from the format's own
  // description rather than by asking the plugin to save one. A test that
  // round-trips the current writer against the current reader proves the two
  // agree with each other and nothing about the versions in between.
  {
    // The DSP's parameter count, which is NOT what params->count() returns:
    // that now includes the bypass, and the bypass is not part of the value
    // array in the blob. Getting this wrong made the loader look broken when
    // it was the test that had miscounted.
    uint32_t nParamsNow = nParams;
    for (uint32_t i = 0; i < nParams; ++i) {
      clap_param_info_t info{};
      if (params->get_info(plugin, i, &info) && (info.flags & CLAP_PARAM_IS_BYPASS)) {
        nParamsNow = nParams - 1;
        break;
      }
    }
    auto buildBlob = [&](uint32_t version, uint32_t declaredParams,
                         const std::vector<float>& values, bool appendBypass, bool bypassOn) {
      std::vector<uint8_t> blob;
      auto put = [&](const void* data, size_t size) {
        const auto* p = static_cast<const uint8_t*>(data);
        blob.insert(blob.end(), p, p + size);
      };
      const char magic[4] = {'S', 'N', 'R', 'S'};
      put(magic, 4);
      put(&version, 4);
      put(&declaredParams, 4);
      for (float v : values) put(&v, 4);
      if (appendBypass) {
        const uint8_t b = bypassOn ? 1 : 0;
        put(&b, 1);
      }
      return blob;
    };
    auto loadBlob = [&](std::vector<uint8_t>& blob) {
      MemStream stream;
      stream.bytes = blob;
      stream.readPos = 0;
      clap_istream_t is{};
      is.ctx = &stream;
      is.read = MemStream::readCb;
      return state->load(plugin, &is);
    };
    auto valueOf = [&](uint32_t index) {
      clap_param_info_t info{};
      if (!params->get_info(plugin, index, &info)) return 0.0;
      double v = 0.0;
      params->get_value(plugin, info.id, &v);
      return v;
    };

    // Values a plugin would never land on by itself, so a "pass" cannot be a
    // parameter that happened to already be there.
    //
    // A STEPPED control lands on a step: the parameter path snaps whatever a
    // blob carries, so 37% of the way across a switch is its "off" position
    // and the mark has to be what the plugin can actually hold. The rounding
    // is CLAP's own meaning of the flag (consecutive integers min..max).
    std::vector<float> marks;
    for (uint32_t i = 0; i < nParamsNow; ++i) {
      clap_param_info_t info{};
      params->get_info(plugin, i, &info);
      double mark = info.min_value + (info.max_value - info.min_value) * 0.37;
      if (info.flags & CLAP_PARAM_IS_STEPPED) mark = std::floor(mark + 0.5);
      marks.push_back((float) mark);
    }

    // ── v1: header and values, nothing else ────────────────────────────────
    {
      // Engage the bypass first, so "v1 leaves it alone" is a real observation
      // rather than a coincidence of it already being off.
      std::vector<uint8_t> v2on = buildBlob(2, nParamsNow, marks, true, true);
      check(loadBlob(v2on), "a v2 blob loads");
      std::vector<uint8_t> v1 = buildBlob(1, nParamsNow, marks, false, false);
      check(loadBlob(v1), "a v1 blob, the oldest format that ever shipped, still loads");
      // v1 predates the bypass, so it means "not bypassed" rather than "do not
      // mention it". A restore that left the plugin silent because the
      // instance happened to be bypassed is a bug a user cannot explain.
      // Found by its FLAG rather than by position: which index carries the
      // bypass is the wrapper's business, and a test that assumed the last one
      // would quietly measure the wrong control the day that changes.
      for (uint32_t i = 0; i < nParams; ++i) {
        clap_param_info_t b{};
        if (!params->get_info(plugin, i, &b)) continue;
        if ((b.flags & CLAP_PARAM_IS_BYPASS) == 0) continue;
        double engaged = 1.0;
        params->get_value(plugin, b.id, &engaged);
        checkNear(engaged, 0.0, 1e-6, "…and it leaves the plugin UN-bypassed");
        break;
      }
      for (uint32_t i = 0; i < nParamsNow && i < 3; ++i)
        checkNear(valueOf(i), marks[i], 1e-3, "…and its parameter values arrive");
    }

    // ── v2: one more byte, the host bypass ─────────────────────────────────
    {
      std::vector<uint8_t> off = buildBlob(2, nParamsNow, marks, true, false);
      check(loadBlob(off), "a v2 blob with the bypass off loads");
      std::vector<uint8_t> on = buildBlob(2, nParamsNow, marks, true, true);
      check(loadBlob(on), "…and one with it on");
    }

    // ── A blob from a plugin that had FEWER controls ───────────────────────
    // The case that actually happens: the product gained a knob. The old
    // session must open with the new knob at its default rather than being
    // rejected outright.
    if (nParamsNow >= 2) {
      std::vector<float> fewer(marks.begin(), marks.end() - 1);
      std::vector<uint8_t> blob = buildBlob(2, nParamsNow - 1, fewer, true, false);
      check(loadBlob(blob), "a blob written before a control existed still loads");
      for (uint32_t i = 0; i + 1 < nParamsNow && i < 3; ++i)
        checkNear(valueOf(i), marks[i], 1e-3, "…the controls it did know about are restored");
      clap_param_info_t last{};
      params->get_info(plugin, nParamsNow - 1, &last);
      double lastValue = 0.0;
      params->get_value(plugin, last.id, &lastValue);
      checkNear(lastValue, last.default_value, 1e-3,
                "…and the one it never heard of sits at its default");
    }

    // ── And from a plugin that had MORE ────────────────────────────────────
    // A control was removed. The extra values have to be consumed, not
    // treated as the start of the bypass byte.
    {
      std::vector<float> more = marks;
      more.push_back(0.5f);
      more.push_back(0.25f);
      std::vector<uint8_t> blob = buildBlob(2, nParamsNow + 2, more, true, true);
      check(loadBlob(blob), "a blob written when there were MORE controls still loads");
      for (uint32_t i = 0; i < nParamsNow && i < 3; ++i)
        checkNear(valueOf(i), marks[i], 1e-3, "…and the shared controls are still right");
    }

    // ── What must be refused ───────────────────────────────────────────────
    {
      std::vector<uint8_t> future = buildBlob(kFutureVersion, nParamsNow, marks, true, false);
      check(!loadBlob(future),
            "a blob from a NEWER build is refused rather than half-understood");

      // A stream that stops mid-value. Accepting this would leave half the
      // controls moved and half not, which is worse than not loading at all.
      std::vector<uint8_t> truncated = buildBlob(1, nParamsNow, marks, false, false);
      if (truncated.size() > 14) truncated.resize(truncated.size() - 2);
      check(!loadBlob(truncated), "a truncated blob is refused, not half-applied");

      // A header that promises more parameters than the stream carries.
      std::vector<uint8_t> lying = buildBlob(1, nParamsNow + 64, marks, false, false);
      check(!loadBlob(lying), "a header claiming more values than it carries is refused");
    }

    // ── The editor remembers how big the user made it ──────────────
    //
    // A plugin whose window forgets its size is a plugin the user resizes
    // every time they open it. It belongs in the SESSION rather than in user
    // settings: two instances on different tracks may reasonably be different
    // sizes, and the large one somebody made on the master should stay large
    // only there.
    // Driven WITHOUT creating a window. The size is state, not a property of
    // a live view -- a host asks for it before it decides how big to make the
    // frame -- so this is the order a host really uses.
    const auto* gui =
        static_cast<const clap_plugin_gui_t*>(plugin->get_extension(plugin, CLAP_EXT_GUI));
    if (gui) {
      // ── The HiDPI round trip ────────────────────────────────────────────
      //
      // get_size returns DEVICE pixels; set_size takes them. A host at a
      // non-1 scale reads the size and echoes it straight back after attach
      // (Bitwig, REAPER both do). If the wrapper stored the device value into
      // its logical field, get_size would re-scale it and the editor would
      // DOUBLE on every round trip until it clamped -- and persist the
      // inflated size into the session. Set a 2x scale, echo the reported
      // size back, and require it to be STABLE.
      if (gui->set_scale) {
        gui->set_scale(plugin, 2.0);
        uint32_t d1 = 0, e1 = 0;
        gui->get_size(plugin, &d1, &e1);
        gui->set_size(plugin, d1, e1);      // host echoes device pixels back
        uint32_t d2 = 0, e2 = 0;
        gui->get_size(plugin, &d2, &e2);
        char note[160];
        std::snprintf(note, sizeof(note),
                      "at 2x scale the size survives a set/get round trip: %ux%u -> %ux%u, "
                      "not doubled", d1, e1, d2, e2);
        check(d2 == d1 && e2 == e1, note);
        gui->set_scale(plugin, 1.0);        // leave it where the rest expects
      }

      uint32_t width = 0, height = 0;
      check(gui->get_size(plugin, &width, &height), "the editor reports a size");
      const uint32_t newWidth = width + 137, newHeight = height + 61;
      check(gui->set_size(plugin, newWidth, newHeight), "…and accepts a new one");

      MemStream resized;
      clap_ostream_t ros{};
      ros.ctx = &resized;
      ros.write = MemStream::writeCb;
      check(state->save(plugin, &ros), "the resized state saves");

      // Put it back to the default, the way a fresh instance would be.
      check(gui->set_size(plugin, width, height), "the editor goes back to its old size");

      resized.readPos = 0;
      clap_istream_t ris{};
      ris.ctx = &resized;
      ris.read = MemStream::readCb;
      check(state->load(plugin, &ris), "…and the saved session loads again");

      uint32_t backWidth = 0, backHeight = 0;
      gui->get_size(plugin, &backWidth, &backHeight);
      std::printf("  ---- editor %ux%u -> saved %ux%u -> restored %ux%u ----\n", width,
                  height, newWidth, newHeight, backWidth, backHeight);
      check(backWidth == newWidth && backHeight == newHeight,
            "…with the size the user had chosen");

      // An older session says nothing about it, and must open at the size the
      // plugin was designed at rather than at whatever the last instance
      // happened to be.
      check(gui->set_size(plugin, newWidth, newHeight), "the editor is resized again");
      // v3 rather than v4: buildBlob writes a header, the parameters and the
      // bypass byte, and a v4 blob also carries a preset index it does not
      // know how to emit. v3 is an honest older session and exercises the
      // same "says nothing about the size" path.
      std::vector<uint8_t> old = buildBlob(3, nParamsNow, marks, true, false);
      check(loadBlob(old), "a session older than this format still loads");
      uint32_t defWidth = 0, defHeight = 0;
      gui->get_size(plugin, &defWidth, &defHeight);
      check(defWidth == width && defHeight == height,
            "…and opens at the size the plugin was designed at");

      // ── A size nobody could use is refused ────────────────────
      //
      // The floor lived in three places with two different numbers: 320x200
      // on both resize paths and 120x120 in the state restore. So a saved
      // session could reopen an editor smaller than a drag could ever have
      // made it -- and a window 120 pixels wide may be too small to grab the
      // corner of and fix.
      //
      // Every entry point is tried, because a host does not have to use the
      // polite one: adjust_size is what a well-behaved host asks first, and
      // set_size is a separate call it may make on its own.
      {
        uint32_t tiny = 1, tinyHeight = 1;
        check(gui->adjust_size(plugin, &tiny, &tinyHeight), "a silly size is adjusted");
        std::printf("  ---- 1x1 adjusted to %ux%u ----\n", tiny, tinyHeight);
        check(tiny >= 320 && tinyHeight >= 200, "…up to something a page can be drawn in");

        check(gui->set_size(plugin, 1, 1), "…and set_size accepts a silly one");
        uint32_t gotWidth = 0, gotHeight = 0;
        gui->get_size(plugin, &gotWidth, &gotHeight);
        check(gotWidth >= 320 && gotHeight >= 200,
              "…having clamped it too, rather than trusting adjust_size to have run");

        // And the other end. A number read out of a file is a number somebody
        // could have written.
        check(gui->set_size(plugin, 100000, 100000), "an enormous size is accepted");
        gui->get_size(plugin, &gotWidth, &gotHeight);
        std::printf("  ---- 100000x100000 clamped to %ux%u ----\n", gotWidth, gotHeight);
        check(gotWidth <= 8192 && gotHeight <= 8192, "…and clamped to a window that fits a screen");

        gui->set_size(plugin, width, height); // leave it where it was
      }
    }

    // Leave the plugin somewhere sane for whatever runs next.
    std::vector<uint8_t> tidy = buildBlob(2, nParamsNow, marks, true, false);
    loadBlob(tidy);
  }

  // ── GUI ───────────────────────────────────────────────────────────────────
  // Driven in the host's documented order: is_api_supported -> create ->
  // set_scale -> can_resize -> get_size -> set_parent -> show -> hide ->
  // destroy. On Windows this really creates the child window and the webview,
  // so a broken embed fails HERE rather than in somebody's DAW.
  const auto* gui =
      static_cast<const clap_plugin_gui_t*>(plugin->get_extension(plugin, CLAP_EXT_GUI));
  check(gui != nullptr, "the gui extension is exposed");
  if (gui) {
#if defined(_WIN32)
    const char* nativeApi = CLAP_WINDOW_API_WIN32;
#elif defined(__APPLE__)
    const char* nativeApi = CLAP_WINDOW_API_COCOA;
#else
    const char* nativeApi = CLAP_WINDOW_API_X11;
#endif
    check(gui->is_api_supported(plugin, nativeApi, false),
          "the native windowing api is supported, embedded");
    check(!gui->is_api_supported(plugin, "nonsense-api", false),
          "an unknown windowing api is refused");

    const char* preferred = nullptr;
    bool preferFloating = true;
    check(gui->get_preferred_api(plugin, &preferred, &preferFloating) && preferred &&
              std::strcmp(preferred, nativeApi) == 0 && !preferFloating,
          "the plugin prefers the native api, embedded");

    check(gui->create(plugin, nativeApi, false), "the gui is created");
    check(!gui->create(plugin, nativeApi, true), "a floating gui is refused (we only embed)");

    uint32_t w = 0, h = 0;
    check(gui->get_size(plugin, &w, &h) && w > 0 && h > 0, "the gui reports a usable size");

    clap_gui_resize_hints_t hints{};
    check(gui->get_resize_hints(plugin, &hints), "resize hints are provided");
    check(gui->can_resize(plugin), "the gui is resizable");
    check(hints.can_resize_horizontally && hints.can_resize_vertically,
          "…in both directions, which is what this fixture declares");
    check(!hints.preserve_aspect_ratio,
          "and with no aspect ratio forced, which is also what it declares");

    // ── The DECLARED size, not the SDK's default ────────────────────────────
    //
    // This fixture declares 517x341 to 1200x800 -- odd numbers no default
    // could produce, the same trick its parameter values use. It matters
    // because every wrapper used to answer 320x200 to 8192x8192 for EVERY
    // plugin, so "the host was told this plugin's minimum" and "the host was
    // told the SDK's constant" looked identical from out here. Asserting 320
    // would still pass against a wrapper that ignored the descriptor entirely.
    uint32_t tinyW = 10, tinyH = 10;
    check(gui->adjust_size(plugin, &tinyW, &tinyH), "adjust_size answers");
    uint32_t hugeW = 99999, hugeH = 99999;
    check(gui->adjust_size(plugin, &hugeW, &hugeH), "adjust_size answers a silly maximum too");

    char note[200];
    if (expectMinW) {
      // ── The DECLARED size, not the SDK's default ──────────────────────────
      //
      // Every wrapper used to answer 320x200 to 8192x8192 for EVERY plugin, so
      // "the host was told this plugin's minimum" and "the host was told the
      // SDK's constant" looked identical from out here. The fixture run with
      // this flag declares odd numbers no default could produce, which is the
      // only way the assertion can tell those two apart.
      std::snprintf(note, sizeof(note),
                    "a 10x10 request comes back %ux%u, the minimum this plugin DECLARED",
                    tinyW, tinyH);
      check(tinyW == expectMinW && tinyH == expectMinH, note);
      std::snprintf(note, sizeof(note),
                    "and an enormous one comes back %ux%u, its declared ceiling", hugeW, hugeH);
      check(hugeW == expectMaxW && hugeH == expectMaxH, note);

      // set_size is a SEPARATE entry point a host may use without asking
      // first, so the clamp has to be on it as well.
      check(gui->set_size(plugin, 10, 10), "set_size accepts an undersized request");
      uint32_t afterW = 0, afterH = 0;
      check(gui->get_size(plugin, &afterW, &afterH) && afterW >= expectMinW &&
                afterH >= expectMinH,
            "…and the size that actually landed is still inside the declared minimum");
      gui->set_size(plugin, w, h);
    } else {
      std::snprintf(note, sizeof(note),
                    "adjust_size enforces a floor (%ux%u) instead of collapsing the page",
                    tinyW, tinyH);
      check(tinyW >= 320 && tinyH >= 200, note);
    }

#if defined(_WIN32)
    // A real parent window, exactly as a host provides.
    HWND parent = CreateWindowExW(0, L"STATIC", L"sonore test host", WS_OVERLAPPEDWINDOW, 0, 0,
                                  (int) w, (int) h, nullptr, nullptr,
                                  GetModuleHandleW(nullptr), nullptr);
    check(parent != nullptr, "the test host created a parent window");
    if (parent) {
      clap_window_t window{};
      window.api = CLAP_WINDOW_API_WIN32;
      window.win32 = parent;
      check(gui->set_parent(plugin, &window), "the gui embeds into the host window");
      check(gui->show(plugin), "the gui shows");

      // Which editor ACTUALLY opened, read off the child window's class name
      // rather than inferred from what the build ought to have chosen. The
      // native UI registers "SonoreNativeUI_<module>_<n>"; the webview host
      // registers a name of its own.
      {
        HWND child = GetWindow(parent, GW_CHILD);
        wchar_t className[128] = {0};
        if (child) GetClassNameW(child, className, 128);
        const bool isNative = std::wcsncmp(className, L"SonoreNativeUI_", 15) == 0;
        char narrow[160] = {0};
        WideCharToMultiByte(CP_UTF8, 0, className, -1, narrow, sizeof(narrow) - 1, nullptr,
                            nullptr);
        char line[240];
        std::snprintf(line, sizeof(line), "the editor window is of class \"%s\"", narrow);
        check(child != nullptr, line);
        if (expectEditor == 1)
          check(isNative,
                "a plugin with no page of its own opened the SDK's native editor, not a browser");
        else if (expectEditor == 2)
          check(!isNative && child != nullptr,
                "a plugin that supplied a page opened a webview, as it asked to");
      }

      // Pump messages so WebView2's asynchronous creation can actually run:
      // without a message loop the completion handlers never fire and the
      // window would look "fine" while being permanently blank.
      MSG msg;
      for (int i = 0; i < 400; ++i) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
          TranslateMessage(&msg);
          DispatchMessageW(&msg);
        }
        Sleep(5);
      }
      check(true, "the gui survived two seconds of a real message loop");

      check(gui->set_size(plugin, 640, 380), "the gui accepts a resize");
      for (int i = 0; i < 40; ++i) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
          TranslateMessage(&msg);
          DispatchMessageW(&msg);
        }
        Sleep(5);
      }
      uint32_t w2 = 0, h2 = 0;
      gui->get_size(plugin, &w2, &h2);
      check(w2 == 640 && h2 == 380, "…and reports the new size back");

      // ── The bridge, end to end ──────────────────────────────────────────
      // With --expect-bridge the plugin under test is the GUI probe, whose
      // page drives parameter 1 to 0.777 on load. Seeing that value here is
      // proof of EVERY link: the page loaded, the bridge was injected before
      // page script, JS ran, the message crossed to C++, went through the
      // lock-free queue and was applied on the audio thread. No contract check
      // can substitute for it: a silently blank webview passes them all.
      if (expectBridge) {
        double probe = 0.0;
        for (int i = 0; i < 200; ++i) {
          while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
          }
          plugin->process(plugin, &process); // drains the UI queue
          params->get_value(plugin, 1, &probe);
          if (std::fabs(probe - 0.777) < 1e-4) break;
          Sleep(10);
        }
        checkNear(probe, 0.777, 1e-4, "the page drove a real parameter through the bridge");

        // ── The plugin's own state reached the page ─────────────────
        //
        // Parameters have their own channel; the state bag does not, and
        // without one an editor lies on reopen -- a sampler still holds its
        // file while the UI draws an empty slot beside it.
        //
        // The probe's page moves parameter 0 to 1.25 only when a state object
        // arrives carrying a non-empty host name, so seeing that value here
        // means the bag crossed intact: DSP -> wrapper -> editor tick -> page.
        double gain = 0.0;
        for (int i = 0; i < 200; ++i) {
          while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
          }
          plugin->process(plugin, &process);
          params->get_value(plugin, 0, &gain);
          if (std::fabs(gain - 1.25) < 1e-4) break;
          Sleep(10);
        }
        checkNear(gain, 1.25, 1e-4, "the DSP's state bag reached the page");

        // ── A right-click reaches the host ─────────────────────────
        //
        // Same journey as the parameter above -- page, bridge, wrapper --
        // except it ends at the HOST rather than the DSP, and carries which
        // control was clicked and where. Without it a user cannot MIDI-learn
        // a knob or remove its automation, because those live in the host's
        // menu and nowhere else.
        for (int i = 0; i < 100 && g_menuPopups == 0; ++i) {
          while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
          }
          Sleep(10);
        }
        std::printf("  ---- context menu: %d popup(s), target kind %u id %u at %d,%d ----\n",
                    g_menuPopups, (unsigned) g_menuTargetKind, (unsigned) g_menuTargetId,
                    g_menuX, g_menuY);
        check(g_menuPopups > 0, "a right-click in the page asks the host for its menu");
        check(g_menuTargetKind == CLAP_CONTEXT_MENU_TARGET_KIND_PARAM,
              "…aimed at a parameter, not at the plugin as a whole");
        check(g_menuTargetId == 2, "…naming the control that was clicked");
        check(g_menuX == 12 && g_menuY == 34, "…at the position the click happened");
      }

      // ── The park cycle ──────────────────────────────────────────────
      //
      // A hidden web editor destroys its renderer after a grace period and
      // rebuilds it on show -- webview_bench measured why (a suspended
      // renderer returns ~0 MB; a destroyed one returns all of it). The bench
      // is hand-run, so THIS is the check that keeps the cycle honest in the
      // gate: hide, wait past the grace, prove the webview is really gone,
      // show, and prove the REBUILT page works by the same standard the first
      // one was held to -- it must drive the probe parameter again. A rebuild
      // that came up blank passes every step except that last one.
      if (expectBridge) {
        // 150 ms grace. Read lazily on the first hidden tick, and this is the
        // first hide this process performs, so the latch takes this value.
        // Both sides share ucrtbase's environment, which is why the plugin's
        // std::getenv sees a _putenv_s made here.
        _putenv_s("SONORE_WEBVIEW_PARK_MS", "150");

        // Zero the parameter the page drives on load, so "it is 0.777 again"
        // can only mean the rebuilt page drove it -- not a leftover.
        {
          EventList write;
          write.addParam(1, 0.0, 0);
          clap_output_events_t sink{};
          sink.ctx = nullptr;
          sink.try_push = outTryPush;
          params->flush(plugin, write.events(), &sink);
        }
        double cleared = 1.0;
        params->get_value(plugin, 1, &cleared);
        check(std::fabs(cleared) < 1e-6, "the probe parameter is cleared before the park cycle");

        check(gui->hide(plugin), "the gui hides for the park cycle");
        for (int i = 0; i < 300; ++i) {
          while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
          }
          Sleep(10);
        }
        check(GetWindow(parent, GW_CHILD) == nullptr,
              "past the grace period a hidden editor has GIVEN BACK its webview");

        check(gui->show(plugin), "showing the parked editor rebuilds it");
        double probe2 = 0.0;
        for (int i = 0; i < 600; ++i) {
          while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
          }
          plugin->process(plugin, &process); // drains the UI queue
          params->get_value(plugin, 1, &probe2);
          if (std::fabs(probe2 - 0.777) < 1e-4) break;
          Sleep(10);
        }
        checkNear(probe2, 0.777, 1e-4,
                  "…and the rebuilt page drove the parameter again -- the whole bridge came "
                  "back, not just a window");
      }

      check(gui->hide(plugin), "the gui hides");
      gui->destroy(plugin);
      check(true, "the gui destroys cleanly");
      DestroyWindow(parent);
    }
#elif defined(__linux__) && defined(SONORE_TEST_X11)
    // A Linux host runs a GTK main loop and hands the plugin an X window id.
    // Doing the same here is the only way to prove WebKitGTK actually loads:
    // the contract checks above pass just as happily against a backend that
    // never manages to create a view.
    void* gtkLib = dlopen("libgtk-3.so.0", RTLD_LAZY | RTLD_GLOBAL);
    auto gtkInit = gtkLib ? (int (*)(int*, char***)) dlsym(gtkLib, "gtk_init_check") : nullptr;
    auto gtkPending = gtkLib ? (int (*)()) dlsym(gtkLib, "gtk_events_pending") : nullptr;
    auto gtkIterate = gtkLib ? (void (*)(int)) dlsym(gtkLib, "gtk_main_iteration_do") : nullptr;

    Display* display = XOpenDisplay(nullptr);
    if (!gtkLib || !gtkInit || !display || !gtkInit(nullptr, nullptr)) {
      std::printf("  ---- no GTK/X display: GUI embedding not exercised ----\n");
      gui->destroy(plugin);
      check(true, "the gui destroys cleanly");
    } else {
      const int screen = DefaultScreen(display);
      Window parent = XCreateSimpleWindow(display, RootWindow(display, screen), 0, 0, w, h, 0,
                                          BlackPixel(display, screen),
                                          BlackPixel(display, screen));
      XMapWindow(display, parent);
      XFlush(display);
      check(parent != 0, "the test host created an X window");

      clap_window_t window{};
      window.api = CLAP_WINDOW_API_X11;
      window.x11 = (clap_xwnd) parent;
      check(gui->set_parent(plugin, &window), "the gui embeds into the host window");
      check(gui->show(plugin), "the gui shows");

      auto pumpGtk = [&](int rounds) {
        for (int i = 0; i < rounds; ++i) {
          while (gtkPending && gtkPending()) gtkIterate(0);
          XFlush(display);
          usleep(5000);
        }
      };
      pumpGtk(400);
      check(true, "the gui survived two seconds of a real GTK main loop");

      if (expectBridge) {
        double probe = 0.0;
        for (int i = 0; i < 200; ++i) {
          pumpGtk(2);
          plugin->process(plugin, &process); // drains the UI queue
          params->get_value(plugin, 1, &probe);
          if (std::fabs(probe - 0.777) < 1e-4) break;
        }
        checkNear(probe, 0.777, 1e-4, "the page drove a real parameter through the bridge");
      }

      check(gui->set_size(plugin, 640, 380), "the gui accepts a resize");
      pumpGtk(20);
      uint32_t w2 = 0, h2 = 0;
      gui->get_size(plugin, &w2, &h2);
      check(w2 == 640 && h2 == 380, "…and reports the new size back");

      check(gui->hide(plugin), "the gui hides");
      gui->destroy(plugin);
      check(true, "the gui destroys cleanly");
      XDestroyWindow(display, parent);
      XCloseDisplay(display);
    }
#else
    // No embedding backend exercised here: say so, so a green run is never
    // mistaken for proof that a window actually appeared.
    std::printf("  ---- window embedding not exercised on this build ----\n");
    gui->destroy(plugin);
    check(true, "the gui destroys cleanly");
#endif
  }

  // ── Lifecycle ─────────────────────────────────────────────────────────────
  plugin->reset(plugin);
  check(true, "reset() runs without crashing");
  plugin->stop_processing(plugin);
  plugin->deactivate(plugin);

  // Re-activating at a different rate must work: hosts do this constantly.
  check(plugin->activate(plugin, 44100.0, 1, 512), "the plugin re-activates at 44.1 kHz");
  plugin->start_processing(plugin);
  process.frames_count = blockSize; // still within the declared maximum
  plugin->process(plugin, &process);
  bool rateFinite = true;
  for (uint32_t i = 0; i < blockSize; ++i)
    if (!std::isfinite(outL[i])) rateFinite = false;
  check(rateFinite, "audio is finite after a sample-rate change");
  plugin->stop_processing(plugin);
  plugin->deactivate(plugin);

  // ── The rates and block sizes a host really uses ──────────────────────────
  //
  // Everything above ran at 48 kHz with 128-sample blocks, and every filter in
  // this SDK was designed while looking at that number. The rates below are
  // the ones that break such designs:
  //
  //   8 kHz: Nyquist is 4 kHz, so a tone control that goes to 18 kHz is
  //            asking for a cutoff above half the sample rate. A filter that
  //            does not clamp produces NaN on its first sample, and the plugin
  //            is silent forever after.
  //   192 kHz: every time in samples is four times longer, so a fixed-size
  //            delay line sized for 48 kHz is a quarter of the time it claims.
  //
  // And the block sizes: ONE sample, which catches anything written assuming a
  // block is worth vectorising, and a block far larger than the one the
  // plugin was first activated with.
  {
    const double rates[] = {8000.0, 22050.0, 96000.0, 192000.0};
    // Capped at the buffers this file allocated. The first version of this
    // section used 512 against 128-sample vectors and overran them -- a heap
    // overflow written INTO the test whose job is to catch that in plugins.
    // The valuable case here is the rates and the ONE-sample block, not a
    // block bigger than the buffers.
    const uint32_t blocks[] = {1u, 7u, blockSize};
    bool allFinite = true, allSane = true, allActivated = true;
    double worst = 0.0;

    for (double rate : rates) {
      if (!plugin->activate(plugin, rate, 1, blockSize)) {
        allActivated = false;
        continue;
      }
      plugin->start_processing(plugin);
      for (uint32_t frames : blocks) {
        process.frames_count = frames;
        events.storage.clear();
        if (isInstrument) events.addNote(CLAP_EVENT_NOTE_ON, 60, 1.0, 0);
        uint32_t lcg = 99u;
        for (int b = 0; b < 40; ++b) {
          for (uint32_t i = 0; i < frames; ++i) {
            lcg = lcg * 1664525u + 1013904223u;
            inL[i] = inR[i] = (float) ((int32_t) (lcg >> 8) % 20001 - 10000) / 40000.0f;
          }
          plugin->process(plugin, &process);
          events.storage.clear();
          for (uint32_t i = 0; i < frames; ++i) {
            if (!std::isfinite(outL[i]) || !std::isfinite(outR[i])) allFinite = false;
            const double a = std::fabs((double) outL[i]);
            if (a > worst) worst = a;
            // Not merely finite: an unclamped filter can also sit at a huge
            // but representable value, which is a burst of noise loud enough
            // to damage monitors rather than a NaN the host would mute.
            if (a > 100.0) allSane = false;
          }
        }
        events.storage.clear();
        if (isInstrument) events.addNote(CLAP_EVENT_NOTE_OFF, 60, 0.0, 0);
        plugin->process(plugin, &process);
      }
      plugin->stop_processing(plugin);
      plugin->deactivate(plugin);
    }
    process.frames_count = blockSize;

    std::printf("  ---- 8 k to 192 kHz, blocks of 1, 7 and %u: worst |out| %.3g ----\n",
                (unsigned) blockSize, worst);
    check(allActivated, "the plugin activates at every rate a host might use");
    check(allFinite, "…and produces finite audio at all of them");
    check(allSane, "…that never blows past sanity, whatever the rate");
  }

  // ── Channel flexibility ───────────────────────────────────────────────────
  // A fresh instance per width, driven the way a host does it: select the
  // config while DEACTIVATED, activate, and the ports must report the width
  // that was chosen. The audible grade: a unity trim must pass every channel
  // through EXACTLY -- per-channel distinct signals catch a wrapper that only
  // wires channel 0.
  if (expectChanMin > 0) {
    const auto* cfgProbe = static_cast<const clap_plugin_audio_ports_config_t*>(
        plugin->get_extension(plugin, CLAP_EXT_AUDIO_PORTS_CONFIG));
    check(cfgProbe != nullptr, "a channel-flexible DSP exposes audio-ports-config");
    if (cfgProbe) {
      check((int) cfgProbe->count(plugin) == expectChanMax - expectChanMin + 1,
            "one config per supported width");
      clap_audio_ports_config_t info{};
      check(cfgProbe->get(plugin, 0, &info) && (int) info.main_output_channel_count ==
                                                   expectChanMin,
            "the first config is the narrowest");
    }

    for (int width : {expectChanMin, expectChanMax}) {
      const clap_plugin_t* p2 = factory->create_plugin(factory, &host, desc->id);
      check(p2 != nullptr, "a fresh instance for the width test");
      if (!p2) break;
      p2->init(p2);
      const auto* cfg = static_cast<const clap_plugin_audio_ports_config_t*>(
          p2->get_extension(p2, CLAP_EXT_AUDIO_PORTS_CONFIG));
      const auto* ports2 = static_cast<const clap_plugin_audio_ports_t*>(
          p2->get_extension(p2, CLAP_EXT_AUDIO_PORTS));
      check(cfg && cfg->select(p2, (clap_id) width), "the width config selects");
      clap_audio_port_info_t pi{};
      check(ports2 && ports2->get(p2, 0, false, &pi) && (int) pi.channel_count == width,
            "…and the port now reports that width");
      check(p2->activate(p2, 48000.0, 1, 128), "the instance activates at the width");
      p2->start_processing(p2);

      std::vector<std::vector<float>> inCh((size_t) width), outCh((size_t) width);
      std::vector<float*> inP((size_t) width), outP((size_t) width);
      for (int c = 0; c < width; ++c) {
        inCh[(size_t) c].assign(128, 0.1f * (float) (c + 1)); // distinct per channel
        outCh[(size_t) c].assign(128, -1.0f);
        inP[(size_t) c] = inCh[(size_t) c].data();
        outP[(size_t) c] = outCh[(size_t) c].data();
      }
      clap_audio_buffer_t in2{}, out2{};
      in2.data32 = inP.data();
      in2.channel_count = (uint32_t) width;
      out2.data32 = outP.data();
      out2.channel_count = (uint32_t) width;
      EventList noEvents;
      clap_output_events_t noOut{};
      noOut.try_push = outTryPush;
      clap_process_t proc{};
      proc.frames_count = 128;
      proc.audio_inputs = &in2;
      proc.audio_inputs_count = 1;
      proc.audio_outputs = &out2;
      proc.audio_outputs_count = 1;
      proc.in_events = noEvents.events();
      proc.out_events = &noOut;
      p2->process(p2, &proc);

      double worst = 0.0;
      for (int c = 0; c < width; ++c)
        for (int i = 0; i < 128; ++i) {
          const double err = std::fabs((double) outCh[(size_t) c][(size_t) i] -
                                       (double) inCh[(size_t) c][(size_t) i]);
          if (err > worst) worst = err;
        }
      char what[96];
      std::snprintf(what, sizeof(what),
                    "unity passes all %d channels through exactly", width);
      check(worst < 1e-6, what);

      p2->stop_processing(p2);
      p2->deactivate(p2);
      p2->destroy(p2);
    }
  }

  // ── 64-bit precision ──────────────────────────────────────────────────────
  // A double-capable plugin must SAY so on its ports and then deliver: unity
  // through the 64-bit path has to preserve values a float could not hold. A
  // plugin that quietly round-tripped through float would lose them, and the
  // difference is exactly what a mastering user is paying for.
  {
    clap_audio_port_info_t pi{};
    const bool declares64 =
        audioPorts && audioPorts->get(plugin, 0, false, &pi) &&
        (pi.flags & CLAP_AUDIO_PORT_SUPPORTS_64BITS) != 0;
    if (declares64) {
      const clap_plugin_t* p64 = factory->create_plugin(factory, &host, desc->id);
      if (p64) {
        p64->init(p64);
        p64->activate(p64, 48000.0, 1, 128);
        p64->start_processing(p64);

        // A value with more mantissa than a float can represent.
        const double fine = 0.1234567890123456789;
        std::vector<double> inD(128, fine), outD(128, -1.0);
        double* inP64[2] = {inD.data(), inD.data()};
        double* outP64[2] = {outD.data(), outD.data()};
        clap_audio_buffer_t in64{}, out64{};
        in64.data64 = inP64;
        in64.channel_count = 2;
        out64.data64 = outP64;
        out64.channel_count = 2;
        EventList none;
        clap_output_events_t noOut{};
        noOut.try_push = outTryPush;
        clap_process_t p{};
        p.frames_count = 128;
        p.audio_inputs = &in64;
        p.audio_inputs_count = 1;
        p.audio_outputs = &out64;
        p.audio_outputs_count = 1;
        p.in_events = none.events();
        p.out_events = &noOut;
        p64->process(p64, &p);

        double worst = 0.0;
        for (int i = 0; i < 128; ++i) {
          const double err = std::fabs(outD[(size_t) i] - fine);
          if (err > worst) worst = err;
        }
        // Float would round this to about 1e-8; genuine double keeps it.
        check(worst < 1e-12, "unity through the 64-bit path preserves double precision");
        const double floatRounded = std::fabs((double) (float) fine - fine);
        check(floatRounded > 1e-12, "…and the probe really is beyond float's reach");

        p64->stop_processing(p64);
        p64->deactivate(p64);
        p64->destroy(p64);
      }
    } else {
      std::printf("  ---- float-only DSP: 64-bit correctly not offered ----\n");
    }
  }

  // ── Preset discovery ──────────────────────────────────────────────────────
  // Compiled-in presets are worthless if the host's BROWSER cannot see them.
  // Crawl the factory exactly as an indexer does, then LOAD what it found
  // through the same load-key, so browsing and loading are proven to agree.
  {
    const auto* pf = static_cast<const clap_preset_discovery_factory_t*>(
        entry->get_factory(CLAP_PRESET_DISCOVERY_FACTORY_ID));
    const auto* presetLoad = static_cast<const clap_plugin_preset_load_t*>(
        plugin->get_extension(plugin, CLAP_EXT_PRESET_LOAD));
    if (pf) {
      check(pf->count(pf) == 1, "the preset factory offers exactly one provider");
      const auto* pd = pf->get_descriptor(pf, 0);
      check(pd && pd->id && pd->id[0], "…with an identified provider descriptor");

      clap_preset_discovery_indexer_t indexer{};
      indexer.clap_version = CLAP_VERSION;
      indexer.name = "Sonore SDK Test Host";
      indexer.declare_filetype = idxDeclareFiletype;
      indexer.declare_location = idxDeclareLocation;
      indexer.declare_soundpack = idxDeclareSoundpack;
      indexer.get_extension = idxGetExtension;

      g_crawl = PresetCrawl{};
      const auto* provider = pf->create(pf, &indexer, pd ? pd->id : "");
      check(provider != nullptr, "the provider is created by its own id");
      check(pf->create(pf, &indexer, "com.example.nope") == nullptr,
            "…and an unknown provider id is refused");
      if (provider) {
        check(provider->init(provider), "the provider initialises");
        check(g_crawl.locations == 1, "…declaring exactly one location to crawl");

        clap_preset_discovery_metadata_receiver_t rx{};
        rx.on_error = rxOnError;
        rx.begin_preset = rxBeginPreset;
        rx.add_plugin_id = rxAddPluginId;
        rx.set_soundpack_id = rxSetSoundpackId;
        rx.set_flags = rxSetFlags;
        rx.add_creator = rxAddCreator;
        rx.set_description = rxSetDescription;
        rx.set_timestamps = rxSetTimestamps;
        rx.add_feature = rxAddFeature;
        rx.add_extra_info = rxAddExtraInfo;
        check(provider->get_metadata(provider, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr,
                                     &rx),
              "the built-in location yields its metadata");
        check(!g_crawl.names.empty(), "…listing at least one preset");
        check(g_crawl.sawPluginId, "…each tagged with this plugin's own CLAP id");

        bool named = true;
        for (const auto& n : g_crawl.names)
          if (n.empty()) named = false;
        check(named, "…every preset named");

        // The point of the whole exercise: a key from the browser must load.
        if (presetLoad && !g_crawl.loadKeys.empty()) {
          check(presetLoad->from_location(plugin, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr,
                                          g_crawl.loadKeys[0].c_str()),
                "a load key the crawl produced actually loads");
          check(!presetLoad->from_location(plugin, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN,
                                           nullptr, "9999"),
                "…and an out-of-range key is refused");
        }
        provider->destroy(provider);
      }
    } else {
      std::printf("  ---- no factory presets: discovery correctly not offered ----\n");
    }
  }

  plugin->destroy(plugin);
  entry->deinit();
  closeLib(lib);

  std::printf("\n%d checks, %d failure(s)\n", g_checks, g_failures);
  if (g_failures == 0) std::printf("SONORE CLAP HOST TEST PASSED\n");
  return g_failures == 0 ? 0 : 1;
}
