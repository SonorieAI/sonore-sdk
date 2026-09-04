// SPDX-License-Identifier: Apache-2.0
// The two threads a plugin lives on, running against each other.
//
// A host calls process() on its audio thread and everything else -- state
// loads, preset loads, parameter reads, the render mode, the interface's
// edits -- on its main thread, at the same time. Every one of those crosses
// into memory the audio thread is reading. The wrappers share parameters as
// relaxed atomics snapshotted once per block, flags the same way, and the
// interface's edits through a lock-free queue; this test is what proves
// that, by doing all of it at once for a few seconds against the saturator
// example, whose plugin.cpp is INCLUDED here so that its entry point, its
// instance type and its interface queue are all in this translation unit
// (the sampler stress test does the same with its DSP).
//
// In plain ctest a torn pointer is a crash and a torn value is a NaN in the
// output; under ThreadSanitizer (scripts/sdk-sanitize.mjs builds it that way)
// a missing atomic is a report with two stacks. Both are the finding.
//
//   concurrency_test [seconds]
#include "../examples/saturator/plugin.cpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

namespace {

const void* hostGetExtension(const clap_host_t*, const char*) { return nullptr; }
void hostRequestRestart(const clap_host_t*) {}
void hostRequestProcess(const clap_host_t*) {}
void hostRequestCallback(const clap_host_t*) {}
clap_host_t g_host = {CLAP_VERSION_INIT,   nullptr,          "com.sonorie.concurrency",
                      "Sonorie",           "",               "1.0",
                      hostGetExtension,    hostRequestRestart, hostRequestProcess,
                      hostRequestCallback};

// The audio thread's event lists: one parameter event per block, and a sink
// that accepts whatever the plugin emits (the interface's edits come back out
// as parameter events; that path is part of what is being exercised).
struct AudioEvents {
  clap_event_param_value_t param{};
  bool haveParam = false;
  static uint32_t size(const clap_input_events_t* list) {
    return static_cast<const AudioEvents*>(list->ctx)->haveParam ? 1u : 0u;
  }
  static const clap_event_header_t* get(const clap_input_events_t* list, uint32_t i) {
    const auto* self = static_cast<const AudioEvents*>(list->ctx);
    return (i == 0 && self->haveParam) ? &self->param.header : nullptr;
  }
  static bool push(const clap_output_events_t*, const clap_event_header_t* e) { return e != nullptr; }
};

struct MemStream {
  std::vector<uint8_t> bytes;
  size_t readPos = 0;
  static int64_t write(const clap_ostream* s, const void* buf, uint64_t size) {
    auto* self = static_cast<MemStream*>(s->ctx);
    const auto* p = static_cast<const uint8_t*>(buf);
    self->bytes.insert(self->bytes.end(), p, p + size);
    return (int64_t) size;
  }
  static int64_t read(const clap_istream* s, void* buf, uint64_t size) {
    auto* self = static_cast<MemStream*>(s->ctx);
    const size_t left = self->bytes.size() - self->readPos;
    const size_t take = size < left ? (size_t) size : left;
    if (take) std::memcpy(buf, self->bytes.data() + self->readPos, take);
    self->readPos += take;
    return (int64_t) take;
  }
};

int g_failures = 0;
void check(bool ok, const char* what) {
  std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) ++g_failures;
}

} // namespace

int main(int argc, char** argv) {
  double seconds = 3.0;
  if (argc > 1) seconds = std::atof(argv[1]);
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::printf("── two threads against one plugin, %.1f s ──\n", seconds);

  if (!clap_entry.init("")) {
    check(false, "the entry initialises");
    return 1;
  }
  const auto* factory =
      static_cast<const clap_plugin_factory_t*>(clap_entry.get_factory(CLAP_PLUGIN_FACTORY_ID));
  const clap_plugin_descriptor_t* desc = factory ? factory->get_plugin_descriptor(factory, 0) : nullptr;
  const clap_plugin_t* plugin = desc ? factory->create_plugin(factory, &g_host, desc->id) : nullptr;
  if (!plugin || !plugin->init(plugin)) {
    check(false, "the plugin is created and initialised");
    return 1;
  }
  const auto* params =
      static_cast<const clap_plugin_params_t*>(plugin->get_extension(plugin, CLAP_EXT_PARAMS));
  const auto* state =
      static_cast<const clap_plugin_state_t*>(plugin->get_extension(plugin, CLAP_EXT_STATE));
  const auto* render =
      static_cast<const clap_plugin_render_t*>(plugin->get_extension(plugin, CLAP_EXT_RENDER));
  const auto* presets = static_cast<const clap_plugin_preset_load_t*>(
      plugin->get_extension(plugin, CLAP_EXT_PRESET_LOAD));
  check(params && state, "the parameter and state extensions are exposed");
  if (!params || !state) return 1;
  const uint32_t numParams = params->count(plugin);
  std::vector<clap_param_info_t> infos(numParams);
  for (uint32_t i = 0; i < numParams; ++i) params->get_info(plugin, i, &infos[i]);

  constexpr uint32_t kFrames = 128;
  std::vector<float> inL(kFrames), inR(kFrames), outL(kFrames), outR(kFrames);
  float* inPtrs[2] = {inL.data(), inR.data()};
  float* outPtrs[2] = {outL.data(), outR.data()};
  clap_audio_buffer_t inBuf{}, outBuf{};
  inBuf.data32 = inPtrs;
  inBuf.channel_count = 2;
  outBuf.data32 = outPtrs;
  outBuf.channel_count = 2;

  AudioEvents audioEvents;
  clap_input_events_t inEvents{&audioEvents, AudioEvents::size, AudioEvents::get};
  clap_output_events_t outEvents{&audioEvents, AudioEvents::push};
  clap_process_t process{};
  process.frames_count = kFrames;
  process.audio_inputs = &inBuf;
  process.audio_inputs_count = 1;
  process.audio_outputs = &outBuf;
  process.audio_outputs_count = 1;
  process.in_events = &inEvents;
  process.out_events = &outEvents;

  check(plugin->activate(plugin, 48000.0, kFrames, kFrames), "the plugin activates");
  check(plugin->start_processing(plugin), "…and starts processing");

  // A state blob to reload from, taken before the threads start.
  MemStream saved;
  clap_ostream_t os{&saved, MemStream::write};
  check(state->save(plugin, &os), "state saves for the main thread to reload");

  std::atomic<bool> stop{false};
  // 64-bit on purpose: a thirty-minute soak on Windows, where long is 32 bits,
  // wrapped the main-thread turn counter past two billion and reported a
  // negative number of turns as a failure of the plugin.
  std::atomic<long long> blocks{0}, mainTurns{0};
  std::atomic<bool> bad{false};

  std::thread audio([&] {
    uint32_t phase = 0;
    long n = 0;
    while (!stop.load(std::memory_order_acquire)) {
      for (uint32_t i = 0; i < kFrames; ++i) {
        const float v = 0.5f * std::sin(0.02f * (float) (phase++));
        inL[i] = inR[i] = v;
      }
      // One parameter event per block from the HOST side too, on the audio
      // thread as the contract says, so the main thread's writes and the
      // host's automation land on the same values at once.
      audioEvents.haveParam = numParams > 0;
      if (numParams > 0) {
        const uint32_t which = (uint32_t) (n % numParams);
        clap_event_param_value_t& e = audioEvents.param;
        e.header.size = sizeof(e);
        e.header.time = 0;
        e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        e.header.type = CLAP_EVENT_PARAM_VALUE;
        e.header.flags = 0;
        e.param_id = infos[which].id;
        e.note_id = -1;
        e.port_index = -1;
        e.channel = -1;
        e.key = -1;
        const double t = (double) (n % 100) / 100.0;
        e.value = infos[which].min_value + t * (infos[which].max_value - infos[which].min_value);
      }
      plugin->process(plugin, &process);
      for (uint32_t i = 0; i < kFrames; ++i)
        if (!std::isfinite(outL[i]) || !std::isfinite(outR[i]) || std::fabs(outL[i]) > 100.0f)
          bad.store(true);
      ++n;
      blocks.fetch_add(1);
    }
  });

  // The main thread: everything a host does while the transport runs.
  sonore::clapwrap::Instance* inst = sonore::clapwrap::self(plugin);
  const auto until = std::chrono::steady_clock::now() +
                     std::chrono::milliseconds((long long) (seconds * 1000.0));
  long k = 0;
  while (std::chrono::steady_clock::now() < until) {
    // A session restore, mid-flight.
    saved.readPos = 0;
    clap_istream_t is{&saved, MemStream::read};
    state->load(plugin, &is);
    // The interface turning a knob: through the same queue the webview uses.
    if (numParams > 0) {
      sonore::UiEventQueue::Event e;
      e.kind = sonore::UiEventQueue::Event::Kind::ParamSet;
      e.index = (int32_t) (k % numParams);
      e.value = (float) infos[(size_t) e.index].default_value;
      inst->uiEvents.push(e);
      sonore::UiEventQueue::Event g;
      g.kind = (k & 1) ? sonore::UiEventQueue::Event::Kind::GestureBegin
                       : sonore::UiEventQueue::Event::Kind::GestureEnd;
      g.index = e.index;
      inst->uiEvents.push(g);
    }
    // The host reading values back for its own display.
    for (uint32_t i = 0; i < numParams; ++i) {
      double v = 0.0;
      params->get_value(plugin, infos[i].id, &v);
      if (!std::isfinite(v)) bad.store(true);
    }
    // A preset from the factory list, and the render mode flipping.
    if (presets && (k % 7) == 0) presets->from_location(plugin, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr, "0");
    if (render && (k % 11) == 0) render->set(plugin, (k % 22) == 0 ? CLAP_RENDER_OFFLINE : CLAP_RENDER_REALTIME);
    ++k;
    mainTurns.fetch_add(1);
  }
  stop.store(true, std::memory_order_release);
  audio.join();

  plugin->stop_processing(plugin);
  plugin->deactivate(plugin);
  plugin->destroy(plugin);
  clap_entry.deinit();

  std::printf("  ---- %lld blocks rendered against %lld main-thread turns ----\n", blocks.load(),
              mainTurns.load());
  check(blocks.load() > 100, "the audio thread rendered throughout");
  check(mainTurns.load() > 20, "the main thread kept loading, editing and reading throughout");
  check(!bad.load(), "every rendered sample and every value read back stayed finite");
  if (g_failures == 0) std::printf("SONORE CONCURRENCY TEST PASSED\n");
  return g_failures == 0 ? 0 : 1;
}
