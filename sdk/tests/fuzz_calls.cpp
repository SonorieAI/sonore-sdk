// SPDX-License-Identifier: Apache-2.0
// A plugin driven the way real hosts drive it: in every order the CLAP
// specification allows, not just the one a test happens to write.
//
// clap_host_test checks each call on its own, in a sensible sequence. A DAW
// is less tidy. It activates at one rate and then another, processes a block
// of zero frames, resets in the middle of a note, loads a session while the
// transport runs, flushes parameters while idle, sends events at the last
// frame of a block and transport that jumps backwards. This is a random walk
// over that state machine -- every step one the specification permits from
// the state the plugin is in -- with a plugin compiled in (SONORE_FUZZ_PLUGIN
// names its source), and two properties held throughout:
//   * nothing crashes or reads out of bounds (run it under ASan for that);
//   * every block that went in finite comes out finite and below 100.
//
//   fuzz_calls                    a quick walk (ctest)
//   fuzz_calls --steps 200000     a long one, under ASan
#ifndef SONORE_FUZZ_PLUGIN
#define SONORE_FUZZ_PLUGIN "../examples/synth/plugin.cpp"
#endif
#include SONORE_FUZZ_PLUGIN

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

namespace {

const void* hostGetExtension(const clap_host_t*, const char*) { return nullptr; }
void hostRequest(const clap_host_t*) {}
const clap_host_t kHost = {CLAP_VERSION_INIT, nullptr, "com.sonorie.fuzz-calls", "Sonorie", "",
                           "1.0", hostGetExtension, hostRequest, hostRequest, hostRequest};

// ── Events ───────────────────────────────────────────────────────────────────
// Kept as raw bytes of each event, in time order, as a host's list is.
struct EventList {
  std::vector<std::vector<uint8_t>> events;
  clap_input_events_t in{this, size, get};
  static uint32_t size(const clap_input_events_t* l) {
    return (uint32_t) static_cast<const EventList*>(l->ctx)->events.size();
  }
  static const clap_event_header_t* get(const clap_input_events_t* l, uint32_t i) {
    const auto* self = static_cast<const EventList*>(l->ctx);
    return i < self->events.size() ? (const clap_event_header_t*) self->events[i].data() : nullptr;
  }
  template <typename T>
  void add(const T& e) {
    std::vector<uint8_t> bytes(sizeof(T));
    std::memcpy(bytes.data(), &e, sizeof(T));
    events.push_back(std::move(bytes));
  }
  void sortByTime() {
    std::stable_sort(events.begin(), events.end(), [](const auto& a, const auto& b) {
      return ((const clap_event_header_t*) a.data())->time <
             ((const clap_event_header_t*) b.data())->time;
    });
  }
};
bool pushOut(const clap_output_events_t*, const clap_event_header_t*) { return true; }
const clap_output_events_t kOut{nullptr, pushOut};

std::vector<uint8_t> g_saved;
int64_t writeTo(const clap_ostream_t*, const void* buffer, uint64_t size) {
  const auto* p = static_cast<const uint8_t*>(buffer);
  g_saved.insert(g_saved.end(), p, p + size);
  return (int64_t) size;
}
struct Reader {
  const std::vector<uint8_t>* bytes;
  size_t pos;
};
int64_t readFrom(const clap_istream_t* s, void* buffer, uint64_t size) {
  auto* r = static_cast<Reader*>(s->ctx);
  const size_t take = std::min((size_t) size, r->bytes->size() - r->pos);
  if (take) std::memcpy(buffer, r->bytes->data() + r->pos, take);
  r->pos += take;
  return (int64_t) take;
}

} // namespace

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  size_t steps = 20000;
  uint32_t seed = 0xC0FFEEu;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--steps") == 0 && i + 1 < argc) steps = std::strtoull(argv[++i], nullptr, 10);
    if (std::strcmp(argv[i], "--seed") == 0 && i + 1 < argc) seed = (uint32_t) std::strtoul(argv[++i], nullptr, 10);
  }
  if (!clap_entry.init("")) return 2;
  const auto* factory =
      static_cast<const clap_plugin_factory_t*>(clap_entry.get_factory(CLAP_PLUGIN_FACTORY_ID));
  const clap_plugin_descriptor_t* desc = factory ? factory->get_plugin_descriptor(factory, 0) : nullptr;
  const clap_plugin_t* plugin = desc ? factory->create_plugin(factory, &kHost, desc->id) : nullptr;
  if (!plugin || !plugin->init(plugin)) {
    std::printf("  FAIL the plugin did not instantiate\n");
    return 1;
  }
  std::printf("Sonore SDK host-call fuzz -- %s, %zu steps, seed %u\n", desc->name, steps, seed);
  const auto* params = static_cast<const clap_plugin_params_t*>(plugin->get_extension(plugin, CLAP_EXT_PARAMS));
  const auto* state = static_cast<const clap_plugin_state_t*>(plugin->get_extension(plugin, CLAP_EXT_STATE));
  const auto* ports = static_cast<const clap_plugin_audio_ports_t*>(plugin->get_extension(plugin, CLAP_EXT_AUDIO_PORTS));
  const auto* latency = static_cast<const clap_plugin_latency_t*>(plugin->get_extension(plugin, CLAP_EXT_LATENCY));
  const auto* tail = static_cast<const clap_plugin_tail_t*>(plugin->get_extension(plugin, CLAP_EXT_TAIL));
  const bool wantsNotes = plugin->get_extension(plugin, CLAP_EXT_NOTE_PORTS) != nullptr;

  std::vector<clap_param_info_t> infos;
  for (uint32_t i = 0; params && i < params->count(plugin); ++i) {
    clap_param_info_t info{};
    params->get_info(plugin, i, &info);
    infos.push_back(info);
  }

  std::mt19937 rng(seed);
  auto pick = [&](uint32_t n) { return n ? rng() % n : 0u; };
  auto chance = [&](int percent) { return (int) pick(100) < percent; };
  const double rates[] = {22050.0, 44100.0, 48000.0, 88200.0, 96000.0, 192000.0};

  bool active = false, processing = false;
  uint32_t maxFrames = 0;
  double rate = 48000.0;
  int64_t steady = 0;
  double beats = 0.0;
  bool heldNote[128] = {};
  std::vector<std::vector<float>> storage;
  std::vector<std::vector<float*>> pointers;
  std::vector<clap_audio_buffer_t> ins, outs;
  size_t blocks = 0, failures = 0, calls = 0;
  const char* lastStep = "";

  auto buildBuffers = [&]() {
    storage.clear();
    pointers.clear();
    ins.clear();
    outs.clear();
    for (int dir = 0; dir < 2; ++dir) {
      const uint32_t count = ports ? ports->count(plugin, dir == 0) : 0;
      for (uint32_t i = 0; i < count; ++i) {
        clap_audio_port_info_t info{};
        ports->get(plugin, i, dir == 0, &info);
        std::vector<float*> chans(info.channel_count, nullptr);
        for (uint32_t c = 0; c < info.channel_count; ++c) storage.emplace_back(maxFrames, 0.0f);
        pointers.push_back(chans);
      }
    }
    size_t s = 0;
    for (auto& chans : pointers)
      for (auto& p : chans) p = storage[s++].data();
    size_t b = 0;
    for (int dir = 0; dir < 2; ++dir) {
      const uint32_t count = ports ? ports->count(plugin, dir == 0) : 0;
      for (uint32_t i = 0; i < count; ++i, ++b) {
        clap_audio_buffer_t buf{};
        buf.data32 = pointers[b].data();
        buf.channel_count = (uint32_t) pointers[b].size();
        (dir == 0 ? ins : outs).push_back(buf);
      }
    }
  };

  auto paramEvent = [&](EventList& list, uint32_t at) {
    if (infos.empty()) return;
    const clap_param_info_t& info = infos[pick((uint32_t) infos.size())];
    clap_event_param_value_t e{};
    e.header = {sizeof(e), at, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0};
    e.param_id = info.id;
    e.cookie = info.cookie;
    e.note_id = -1;
    e.port_index = -1;
    e.channel = -1;
    e.key = -1;
    const double u = (double) pick(1001) / 1000.0;
    // Mostly inside the range; sometimes at or past its ends, which a host
    // with a careless automation lane does send.
    e.value = chance(90) ? info.min_value + u * (info.max_value - info.min_value)
                         : (chance(50) ? info.max_value * 2.0 + 1.0 : info.min_value - 7.0);
    list.add(e);
  };
  auto noteEvent = [&](EventList& list, uint32_t at) {
    const int key = 36 + (int) pick(48);
    clap_event_note_t e{};
    const bool on = !heldNote[key] || chance(20); // sometimes a second on for a held key
    e.header = {sizeof(e), at, CLAP_CORE_EVENT_SPACE_ID,
                (uint16_t) (on ? CLAP_EVENT_NOTE_ON : CLAP_EVENT_NOTE_OFF), 0};
    e.note_id = chance(50) ? (int32_t) pick(1000) : -1;
    e.port_index = 0;
    e.channel = (int16_t) pick(16);
    e.key = (int16_t) key;
    e.velocity = (double) pick(101) / 100.0;
    heldNote[key] = on;
    list.add(e);
  };

  for (size_t step = 0; step < steps; ++step) {
    const uint32_t roll = pick(100);
    ++calls;
    if (!active) {
      if (roll < 60) {
        rate = rates[pick(6)];
        // 16 .. 4096 mostly; one time in ten 8192 .. 65536, the blocks an
        // offline render asks for.
        maxFrames = chance(10) ? 1u << (13 + pick(4)) : 1u << (4 + pick(9));
        lastStep = "activate";
        if (plugin->activate(plugin, rate, 1, maxFrames)) {
          active = true;
          buildBuffers();
        }
      } else if (roll < 75 && params) {
        EventList list;
        for (uint32_t n = pick(6); n > 0; --n) paramEvent(list, 0);
        lastStep = "flush (idle)";
        params->flush(plugin, &list.in, &kOut);
      } else if (roll < 90 && state) {
        lastStep = "state round trip (idle)";
        g_saved.clear();
        clap_ostream_t os{nullptr, writeTo};
        if (state->save(plugin, &os)) {
          Reader r{&g_saved, 0};
          clap_istream_t is{&r, readFrom};
          state->load(plugin, &is);
        }
      } else {
        lastStep = "main thread callback";
        plugin->on_main_thread(plugin);
      }
      continue;
    }
    if (!processing) {
      if (roll < 55) {
        lastStep = "start_processing";
        processing = plugin->start_processing(plugin);
      } else if (roll < 70) {
        lastStep = "deactivate";
        plugin->deactivate(plugin);
        active = false;
      } else if (roll < 80 && params) {
        EventList list;
        for (uint32_t n = pick(6); n > 0; --n) paramEvent(list, 0);
        lastStep = "flush (active, stopped)";
        params->flush(plugin, &list.in, &kOut);
      } else if (roll < 88) {
        lastStep = "reset (stopped)";
        plugin->reset(plugin);
      } else {
        lastStep = "latency/tail/main thread";
        if (latency) (void) latency->get(plugin);
        if (tail) (void) tail->get(plugin);
        plugin->on_main_thread(plugin);
      }
      continue;
    }
    // Processing.
    if (roll < 75) {
      // A block: any length up to the maximum, zero included.
      const uint32_t frames = chance(5) ? 0 : (chance(10) ? maxFrames : 1 + pick(maxFrames));
      EventList list;
      const uint32_t nEvents = chance(40) ? pick(8) : 0;
      for (uint32_t n = 0; n < nEvents; ++n) {
        const uint32_t at = frames ? (chance(20) ? frames - 1 : pick(frames)) : 0;
        if (wantsNotes && chance(50)) noteEvent(list, at);
        else paramEvent(list, at);
      }
      list.sortByTime();
      // Input: silence, a signal, a loud one, or tiny values near denormal.
      const int kind = (int) pick(4);
      uint32_t noise = rng();
      for (auto& buf : ins)
        for (uint32_t c = 0; c < buf.channel_count; ++c)
          for (uint32_t i = 0; i < frames; ++i) {
            noise = noise * 1664525u + 1013904223u;
            const float r = (float) ((double) noise / 4294967296.0 - 0.5);
            buf.data32[c][i] = kind == 0 ? 0.0f : kind == 1 ? 0.5f * r : kind == 2 ? 4.0f * r : 1e-38f * r;
          }
      for (auto& buf : outs)
        for (uint32_t c = 0; c < buf.channel_count; ++c)
          std::fill(buf.data32[c], buf.data32[c] + frames, 0.0f);
      clap_event_transport_t transport{};
      const bool sendTransport = chance(70);
      if (sendTransport) {
        if (chance(10)) beats = (double) pick(64) - 4.0; // a jump, backwards too
        transport.header = {sizeof(transport), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_TRANSPORT, 0};
        // Drawn one statement at a time: the operands of | are unsequenced,
        // and a seed has to replay the same walk under any compiler.
        const bool playing = chance(80);
        const bool looping = chance(20);
        transport.flags = CLAP_TRANSPORT_HAS_TEMPO | CLAP_TRANSPORT_HAS_BEATS_TIMELINE |
                          CLAP_TRANSPORT_HAS_TIME_SIGNATURE | (playing ? CLAP_TRANSPORT_IS_PLAYING : 0) |
                          (looping ? CLAP_TRANSPORT_IS_LOOP_ACTIVE : 0);
        transport.tempo = 40.0 + (double) pick(200);
        transport.song_pos_beats = (clap_beattime) std::llround(beats * (double) CLAP_BEATTIME_FACTOR);
        transport.loop_start_beats = 0;
        transport.loop_end_beats = (clap_beattime) (8 * CLAP_BEATTIME_FACTOR);
        transport.tsig_num = (uint16_t) (2 + pick(6));
        transport.tsig_denom = 4;
        beats += (double) frames / rate * transport.tempo / 60.0;
      }
      clap_process_t process{};
      process.steady_time = steady;
      process.frames_count = frames;
      process.transport = sendTransport ? &transport : nullptr;
      process.audio_inputs = ins.data();
      process.audio_inputs_count = (uint32_t) ins.size();
      process.audio_outputs = outs.data();
      process.audio_outputs_count = (uint32_t) outs.size();
      process.in_events = &list.in;
      process.out_events = &kOut;
      lastStep = "process";
      plugin->process(plugin, &process);
      steady += frames;
      ++blocks;
      float worst = 0.0f;
      bool finite = true;
      for (const auto& buf : outs)
        for (uint32_t c = 0; c < buf.channel_count; ++c)
          for (uint32_t i = 0; i < frames; ++i) {
            const float v = buf.data32[c][i];
            if (!std::isfinite(v)) finite = false;
            else if (std::fabs(v) > worst) worst = std::fabs(v);
          }
      if (!finite || worst > 100.0f) {
        ++failures;
        if (failures <= 5)
          std::printf("  FAIL step %zu: a block of %u frames at %g Hz came out %s (peak %g)\n", step,
                      frames, rate, finite ? "too loud" : "NOT FINITE", (double) worst);
      }
    } else if (roll < 85) {
      lastStep = "stop_processing";
      plugin->stop_processing(plugin);
      processing = false;
    } else if (roll < 92) {
      lastStep = "reset (processing)";
      plugin->reset(plugin);
    } else if (state) {
      // A session loaded while the transport runs: on the main thread, with
      // the audio thread idle between blocks, as a single-threaded host does.
      lastStep = "state round trip (processing)";
      g_saved.clear();
      clap_ostream_t os{nullptr, writeTo};
      if (state->save(plugin, &os)) {
        Reader r{&g_saved, 0};
        clap_istream_t is{&r, readFrom};
        state->load(plugin, &is);
      }
    }
  }
  (void) lastStep;

  if (processing) plugin->stop_processing(plugin);
  if (active) plugin->deactivate(plugin);
  plugin->destroy(plugin);
  clap_entry.deinit();

  std::printf("  %zu calls, %zu blocks processed, %zu broke a property\n", calls, blocks, failures);
  if (blocks == 0) {
    std::printf("  FAIL the walk never processed a block\n");
    return 1;
  }
  std::printf(failures == 0 ? "SONORE HOST-CALL FUZZ PASSED\n" : "SONORE HOST-CALL FUZZ FAILED\n");
  return failures == 0 ? 0 : 1;
}
