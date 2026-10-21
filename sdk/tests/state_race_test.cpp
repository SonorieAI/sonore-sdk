// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: a state loaded while audio runs reaches a block whole or not at
// all.
//
// Every format lets a host load a state, or a preset, on its main thread
// while the audio thread is processing. The wrapper wrote the new values one
// at a time and the audio thread copied them once per block, so a copy taken
// in between was half one session and half the other: a parameter set nobody
// ever saved, played for a block. A review probe counted 7206 such blocks in
// 5.27 million.
//
// Here the probe DSP has 32 parameters, and the two sessions set all 32 to
// 0.25 or all to 0.75. Any block whose parameters are not all equal is a torn
// load.
//
//   state_race_test [seconds]
#define SONORE_NUM_PARAMS 32

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#include <sonore/dsp.h>

static std::atomic<long long> g_blocks{0}, g_torn{0};

struct SonoreDsp {
  void prepare(const sonore::ProcessSpec&) {}
  void process(sonore::AudioBlock<float>& io, const float* params) {
    for (int i = 1; i < SONORE_NUM_PARAMS; ++i)
      if (params[i] != params[0]) {
        g_torn.fetch_add(1, std::memory_order_relaxed);
        break;
      }
    g_blocks.fetch_add(1, std::memory_order_relaxed);
    io.clear();
  }
};

#include <sonore/plugin.h>

static sonore::ParamInfo makeParam(int i) {
  static char ids[SONORE_NUM_PARAMS][8];
  std::snprintf(ids[i], sizeof(ids[i]), "p%d", i);
  return {ids[i], ids[i], "", 0.0f, 1.0f, 0.5f, 0};
}
static const sonore::ParamInfo* paramTable() {
  static sonore::ParamInfo table[SONORE_NUM_PARAMS];
  for (int i = 0; i < SONORE_NUM_PARAMS; ++i) table[i] = makeParam(i);
  return table;
}
static sonore::PluginDescriptor makeDesc() {
  sonore::PluginDescriptor d;
  d.id = "com.sonorie.test.staterace";
  d.name = "Sonore State Race Probe";
  d.params = paramTable();
  d.numParams = SONORE_NUM_PARAMS;
  return d;
}
static const sonore::PluginDescriptor kDesc = makeDesc();

#include <sonore/clap_wrapper.h>

namespace {
int g_checks = 0, g_failures = 0;
void check(bool ok, const char* what) {
  ++g_checks;
  if (!ok) ++g_failures;
  std::printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
}

const void* hostGetExtension(const clap_host_t*, const char*) { return nullptr; }
void hostRequest(const clap_host_t*) {}
const clap_host_t kHost = {CLAP_VERSION_INIT, nullptr, "com.sonorie.state-race", "Sonorie", "", "1.0",
                           hostGetExtension, hostRequest, hostRequest, hostRequest};

int64_t writeCb(const clap_ostream_t* s, const void* data, uint64_t n) {
  auto* out = static_cast<std::vector<uint8_t>*>(s->ctx);
  out->insert(out->end(), (const uint8_t*) data, (const uint8_t*) data + n);
  return (int64_t) n;
}
struct Reader {
  const std::vector<uint8_t>* bytes;
  size_t pos;
};
int64_t readCb(const clap_istream_t* s, void* data, uint64_t n) {
  auto* r = static_cast<Reader*>(s->ctx);
  const size_t left = r->bytes->size() - r->pos;
  const size_t take = n < left ? (size_t) n : left;
  std::memcpy(data, r->bytes->data() + r->pos, take);
  r->pos += take;
  return (int64_t) take;
}
} // namespace

int main(int argc, char** argv) {
  const double seconds = argc > 1 ? std::atof(argv[1]) : 1.0;
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::printf("── a state loaded while audio runs, %.1f s ──\n", seconds);
  clap_entry.init("");
  const auto* factory =
      static_cast<const clap_plugin_factory_t*>(clap_entry.get_factory(CLAP_PLUGIN_FACTORY_ID));
  const clap_plugin_t* plugin =
      factory->create_plugin(factory, &kHost, factory->get_plugin_descriptor(factory, 0)->id);
  check(plugin && plugin->init(plugin), "the probe initialises");
  if (!plugin) return 1;
  const auto* state =
      static_cast<const clap_plugin_state_t*>(plugin->get_extension(plugin, CLAP_EXT_STATE));
  sonore::clapwrap::Instance* inst = sonore::clapwrap::self(plugin);

  auto sessionAt = [&](float value) {
    for (int i = 0; i < SONORE_NUM_PARAMS; ++i) inst->params[i] = value;
    std::vector<uint8_t> bytes;
    clap_ostream_t out{&bytes, writeCb};
    state->save(plugin, &out);
    return bytes;
  };
  const std::vector<uint8_t> low = sessionAt(0.25f), high = sessionAt(0.75f);

  constexpr uint32_t kFrames = 32;
  float inL[kFrames] = {}, inR[kFrames] = {}, outL[kFrames], outR[kFrames];
  float* inPtrs[2] = {inL, inR};
  float* outPtrs[2] = {outL, outR};
  clap_audio_buffer_t inBuf{}, outBuf{};
  inBuf.data32 = inPtrs;
  inBuf.channel_count = 2;
  outBuf.data32 = outPtrs;
  outBuf.channel_count = 2;
  clap_process_t process{};
  process.frames_count = kFrames;
  process.steady_time = -1;
  process.audio_inputs = &inBuf;
  process.audio_inputs_count = 1;
  process.audio_outputs = &outBuf;
  process.audio_outputs_count = 1;
  check(plugin->activate(plugin, 48000.0, 1, kFrames) && plugin->start_processing(plugin),
        "it activates");

  std::atomic<bool> stop{false};
  std::thread audio([&] {
    while (!stop.load(std::memory_order_acquire)) plugin->process(plugin, &process);
  });
  long long loads = 0;
  const auto until =
      std::chrono::steady_clock::now() + std::chrono::milliseconds((long long) (seconds * 1000.0));
  while (std::chrono::steady_clock::now() < until) {
    Reader r{(loads & 1) ? &high : &low, 0};
    clap_istream_t in{&r, readCb};
    state->load(plugin, &in);
    ++loads;
  }
  stop.store(true, std::memory_order_release);
  audio.join();

  plugin->stop_processing(plugin);
  plugin->deactivate(plugin);
  plugin->destroy(plugin);
  clap_entry.deinit();

  char line[200];
  std::snprintf(line, sizeof(line), "%lld blocks against %lld state loads", g_blocks.load(), loads);
  check(g_blocks.load() > 1000 && loads > 100, line);
  std::snprintf(line, sizeof(line), "...and no block ran on a mixture of the two sessions (%lld did)",
                g_torn.load());
  check(g_torn.load() == 0, line);
  std::printf("\n%d checks, %d failure(s)\n", g_checks, g_failures);
  if (g_failures == 0) std::printf("SONORE STATE RACE PASSED\n");
  return g_failures == 0 ? 0 : 1;
}
