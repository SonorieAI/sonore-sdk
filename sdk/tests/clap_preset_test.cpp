// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: a CLAP factory preset lands on every parameter, at once.
//
// clap_plugin_params: after "loading a preset" the plugin calls the host's
// params->rescan(CLAP_PARAM_RESCAN_VALUES), and get_value() answers for the
// new values from then on. The wrapper pushed each value through the UI
// event queue instead -- 255 events deep, its push unchecked -- so a plugin
// with more parameters than that lost the rest, and get_value() kept
// answering with the old values until something drained the queue.
//
// A probe with 300 parameters and one preset, compiled in so its CLAP entry
// can be driven directly.
#define SONORE_NUM_PARAMS 300

#include <cstdio>
#include <cstdlib>

#include <sonore/dsp.h>

struct SonoreDsp {
  void prepare(const sonore::ProcessSpec&) {}
  void process(sonore::AudioBlock<float>&, const float*) {}
};

#include <sonore/plugin.h>
#include <sonore/presets.h>

static sonore::ParamInfo kParamTable[SONORE_NUM_PARAMS];
static char g_ids[SONORE_NUM_PARAMS][8];
static float g_presetValues[SONORE_NUM_PARAMS];
static const sonore::Preset kPresets[1] = {{"All up", g_presetValues, SONORE_NUM_PARAMS}};
static const bool g_tableBuilt = [] {
  for (int i = 0; i < SONORE_NUM_PARAMS; ++i) {
    std::snprintf(g_ids[i], sizeof(g_ids[i]), "p%d", i);
    kParamTable[i].id = g_ids[i];
    kParamTable[i].label = g_ids[i];
    kParamTable[i].minValue = 0.0f;
    kParamTable[i].maxValue = 1.0f;
    kParamTable[i].defaultValue = 0.0f;
    g_presetValues[i] = 0.5f + 0.001f * (float) (i % 100);
  }
  return true;
}();

static const sonore::PluginDescriptor kDesc = {
    "com.sonorie.test.clappreset",
    "Sonore CLAP Preset Probe",
    "Sonorie",
    "1.0.0",
    "Three hundred parameters and one preset.",
    "https://sonorie.com",
    false,
    kParamTable,
    SONORE_NUM_PARAMS,
    kPresets,
    1,
    "utility",
};

#include <sonore/clap_wrapper.h>

static int g_checks = 0;
static int g_failures = 0;

static void check(bool ok, const char* what) {
  ++g_checks;
  if (!ok) ++g_failures;
  std::printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
}

namespace {
int g_rescans = 0;
void hostRescan(const clap_host_t*, clap_param_rescan_flags flags) {
  if (flags & CLAP_PARAM_RESCAN_VALUES) ++g_rescans;
}
void hostClear(const clap_host_t*, clap_id, clap_param_clear_flags) {}
void hostRequestFlush(const clap_host_t*) {}
const clap_host_params_t kHostParams = {hostRescan, hostClear, hostRequestFlush};
const void* hostGetExtension(const clap_host_t*, const char* id) {
  return std::strcmp(id, CLAP_EXT_PARAMS) == 0 ? &kHostParams : nullptr;
}
void hostRequest(const clap_host_t*) {}
const clap_host_t kHost = {CLAP_VERSION_INIT, nullptr, "com.sonorie.clap-preset-test", "Sonorie",
                           "",                "1.0",   hostGetExtension,             hostRequest,
                           hostRequest,       hostRequest};
} // namespace

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::printf("── a CLAP factory preset with 300 parameters ──\n");
  (void) g_tableBuilt;
  check(clap_entry.init(""), "the entry initialises");
  const auto* factory =
      static_cast<const clap_plugin_factory_t*>(clap_entry.get_factory(CLAP_PLUGIN_FACTORY_ID));
  const clap_plugin_descriptor_t* desc = factory->get_plugin_descriptor(factory, 0);
  const clap_plugin_t* plugin = factory->create_plugin(factory, &kHost, desc->id);
  check(plugin && plugin->init(plugin), "the probe instantiates");
  if (!plugin) return 1;
  plugin->activate(plugin, 48000.0, 1, 256);

  const auto* params = static_cast<const clap_plugin_params_t*>(plugin->get_extension(plugin, CLAP_EXT_PARAMS));
  const auto* presets =
      static_cast<const clap_plugin_preset_load_t*>(plugin->get_extension(plugin, CLAP_EXT_PRESET_LOAD));
  // 300 of its own, and the bypass the wrapper adds.
  check(params && presets && params->count(plugin) >= 300, "it has 300 parameters and preset-load");
  if (!params || !presets) return 1;
  g_rescans = 0;
  check(presets->from_location(plugin, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr, "0"),
        "the factory preset loads");

  // Read back straight away: no flush, no process() in between.
  int right = 0;
  for (uint32_t i = 0; i < params->count(plugin); ++i) {
    clap_param_info_t info{};
    double value = -1.0;
    if (!params->get_info(plugin, i, &info) || info.name[0] != 'p') continue; // the bypass
    const int k = std::atoi(info.name + 1);
    if (k >= 0 && k < SONORE_NUM_PARAMS && params->get_value(plugin, info.id, &value) &&
        std::fabs(value - (double) g_presetValues[k]) < 1e-6)
      ++right;
  }
  char line[200];
  std::snprintf(line, sizeof(line), "get_value answers with the preset straight away, for every "
                "parameter (%d of 300)", right);
  check(right == 300, line);
  std::snprintf(line, sizeof(line), "and the host is asked to rescan the values (%d time(s))",
                g_rescans);
  check(g_rescans >= 1, line);

  // A descriptor that declares more parameters than the arrays hold -- which
  // a release build loads. applyPreset writes one value per DECLARED
  // parameter, and every wrapper array is SONORE_NUM_PARAMS long: loading one
  // of its presets wrote past the end, in the discovery provider and in
  // VST3's program change. The helper every one of them now goes through
  // refuses it, and writes nothing.
  {
    sonore::PluginDescriptor declared = kDesc;
    declared.numParams = 6; // the first six rows of the table
    static const float kSix[6] = {0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f};
    static const sonore::Preset kSixPreset[1] = {{"Six", kSix, 6}};
    declared.presets = kSixPreset;
    declared.numPresets = 1;
    float room[2 + 4] = {-1.0f, -1.0f, -7.0f, -7.0f, -7.0f, -7.0f}; // two to fill, four past the end
    const bool refused = !sonore::clapwrap::factoryPresetValues(declared, 0, room, 2);
    check(refused && room[0] == -1.0f && room[1] == -1.0f && room[2] == -7.0f && room[5] == -7.0f,
          "a preset for more parameters than the array holds is refused, and nothing is written");
    float whole[6] = {};
    check(sonore::clapwrap::factoryPresetValues(declared, 0, whole, 6) && whole[5] == 0.6f,
          "...and the same preset into an array that holds it is applied");
  }

  plugin->deactivate(plugin);
  plugin->destroy(plugin);
  clap_entry.deinit();
  std::printf("\n%d checks, %d failure(s)\n", g_checks, g_failures);
  if (g_failures == 0) std::printf("SONORE CLAP PRESET PASSED\n");
  return g_failures == 0 ? 0 : 1;
}
