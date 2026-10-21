// SPDX-License-Identifier: Apache-2.0
// A plugin's saved state, as a host hands it back.
//
// The blob comes from a session file, and a session file may be truncated by
// a full disk, damaged in a sync, edited by hand, written by another build or
// sent by somebody else. fuzz_parsers covers the StateBag on its own; this is
// the whole loader the formats share (clapwrap::loadStateBody: the header,
// the parameter values, the bypass, the preset, the editor size, then the
// bag), reached through the CLAP state extension with an example compiled in
// -- the ducker, which keeps a bag of its own beside its parameters and has a
// sidechain to process.
//
// Mutations of a real saved blob, and three properties for each:
//   * a blob that loads leaves a plugin whose next block is finite and sane;
//   * a blob that is refused leaves the plugin exactly as it was -- its own
//     saved state, byte for byte, is what it was before;
//   * a blob that loads is a fixed point after one round: saved, loaded and
//     saved again, the bytes do not move (clamping and defaults settle once).
//
//   fuzz_state                    a quick pass (ctest)
//   fuzz_state --iterations 50000 the deep pass, under ASan
#include "../examples/ducker/plugin.cpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

namespace {

const void* hostGetExtension(const clap_host_t*, const char*) { return nullptr; }
void hostRequest(const clap_host_t*) {}
const clap_host_t kHost = {CLAP_VERSION_INIT, nullptr, "com.sonorie.fuzz-state", "Sonorie", "",
                           "1.0", hostGetExtension, hostRequest, hostRequest, hostRequest};

struct Reader {
  clap_istream_t stream{};
  const std::vector<uint8_t>* bytes = nullptr;
  size_t pos = 0;
  size_t chunk = 0; // when non-zero, short reads of at most this many bytes
};
int64_t readFrom(const clap_istream_t* s, void* buffer, uint64_t size) {
  auto* r = static_cast<Reader*>(s->ctx);
  size_t take = (size_t) size;
  if (r->chunk && take > r->chunk) take = r->chunk;
  const size_t left = r->bytes->size() - r->pos;
  if (take > left) take = left;
  if (take) std::memcpy(buffer, r->bytes->data() + r->pos, take);
  r->pos += take;
  return (int64_t) take;
}
int64_t writeTo(const clap_ostream_t* s, const void* buffer, uint64_t size) {
  auto* out = static_cast<std::vector<uint8_t>*>(s->ctx);
  const auto* p = static_cast<const uint8_t*>(buffer);
  out->insert(out->end(), p, p + size);
  return (int64_t) size;
}

std::vector<uint8_t> save(const clap_plugin_t* plugin, const clap_plugin_state_t* state) {
  std::vector<uint8_t> out;
  clap_ostream_t os{&out, writeTo};
  if (!state->save(plugin, &os)) out.clear();
  return out;
}
bool load(const clap_plugin_t* plugin, const clap_plugin_state_t* state,
          const std::vector<uint8_t>& bytes, size_t chunk) {
  Reader r;
  r.bytes = &bytes;
  r.chunk = chunk;
  r.stream.ctx = &r;
  r.stream.read = readFrom;
  return state->load(plugin, &r.stream);
}

// One block through every port the plugin declares, the input a steady
// signal, and what came out checked.
struct Rig {
  std::vector<std::vector<float>> storage;
  std::vector<std::vector<float*>> pointers;
  std::vector<clap_audio_buffer_t> ins, outs;
};

bool runBlock(const clap_plugin_t* plugin, const clap_plugin_audio_ports_t* ports, Rig& rig,
              float* worst) {
  const uint32_t frames = 128;
  rig.storage.clear();
  rig.pointers.clear();
  rig.ins.clear();
  rig.outs.clear();
  for (int dir = 0; dir < 2; ++dir) {
    const bool isInput = dir == 0;
    const uint32_t count = ports ? ports->count(plugin, isInput) : 0;
    for (uint32_t i = 0; i < count; ++i) {
      clap_audio_port_info_t info{};
      ports->get(plugin, i, isInput, &info);
      std::vector<float*> chans;
      for (uint32_t c = 0; c < info.channel_count; ++c) {
        rig.storage.emplace_back(frames, isInput ? 0.25f : 0.0f);
        chans.push_back(nullptr);
      }
      rig.pointers.push_back(chans);
    }
  }
  // Point the channel arrays at the storage only now that both have stopped
  // growing.
  size_t s = 0;
  for (auto& chans : rig.pointers)
    for (auto& p : chans) p = rig.storage[s++].data();
  size_t b = 0;
  for (int dir = 0; dir < 2; ++dir) {
    const bool isInput = dir == 0;
    const uint32_t count = ports ? ports->count(plugin, isInput) : 0;
    for (uint32_t i = 0; i < count; ++i, ++b) {
      clap_audio_buffer_t buf{};
      buf.data32 = rig.pointers[b].data();
      buf.channel_count = (uint32_t) rig.pointers[b].size();
      (isInput ? rig.ins : rig.outs).push_back(buf);
    }
  }
  clap_input_events_t inEvents{nullptr, [](const clap_input_events_t*) -> uint32_t { return 0; },
                               [](const clap_input_events_t*, uint32_t) -> const clap_event_header_t* {
                                 return nullptr;
                               }};
  clap_output_events_t outEvents{nullptr, [](const clap_output_events_t*, const clap_event_header_t*) {
                                   return true;
                                 }};
  clap_process_t process{};
  process.steady_time = -1;
  process.frames_count = frames;
  process.audio_inputs = rig.ins.data();
  process.audio_inputs_count = (uint32_t) rig.ins.size();
  process.audio_outputs = rig.outs.data();
  process.audio_outputs_count = (uint32_t) rig.outs.size();
  process.in_events = &inEvents;
  process.out_events = &outEvents;
  plugin->process(plugin, &process);
  *worst = 0.0f;
  for (const auto& out : rig.outs)
    for (uint32_t c = 0; c < out.channel_count; ++c)
      for (uint32_t i = 0; i < frames; ++i) {
        const float v = out.data32[c][i];
        if (!std::isfinite(v)) return false;
        if (std::fabs(v) > *worst) *worst = std::fabs(v);
      }
  return true;
}

} // namespace

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  size_t iterations = 3000;
  for (int i = 1; i < argc; ++i)
    if (std::strcmp(argv[i], "--iterations") == 0 && i + 1 < argc)
      iterations = (size_t) std::strtoull(argv[++i], nullptr, 10);

  std::printf("Sonore SDK plugin state fuzz -- %zu mutations\n", iterations);
  if (!clap_entry.init("")) return 2;
  const auto* factory =
      static_cast<const clap_plugin_factory_t*>(clap_entry.get_factory(CLAP_PLUGIN_FACTORY_ID));
  const clap_plugin_descriptor_t* desc = factory ? factory->get_plugin_descriptor(factory, 0) : nullptr;
  const clap_plugin_t* plugin = desc ? factory->create_plugin(factory, &kHost, desc->id) : nullptr;
  if (!plugin || !plugin->init(plugin)) {
    std::printf("  FAIL the ducker did not instantiate\n");
    return 1;
  }
  const auto* state =
      static_cast<const clap_plugin_state_t*>(plugin->get_extension(plugin, CLAP_EXT_STATE));
  const auto* params =
      static_cast<const clap_plugin_params_t*>(plugin->get_extension(plugin, CLAP_EXT_PARAMS));
  const auto* ports =
      static_cast<const clap_plugin_audio_ports_t*>(plugin->get_extension(plugin, CLAP_EXT_AUDIO_PORTS));
  if (!state || !params || !plugin->activate(plugin, 48000.0, 1, 128) ||
      !plugin->start_processing(plugin)) {
    std::printf("  FAIL the ducker has no state extension or did not activate\n");
    return 1;
  }

  // The seed: a session as a user would have saved it -- every parameter
  // somewhere other than its default, and the DSP's own bag filled in.
  std::vector<uint8_t> seed = save(plugin, state);
  if (seed.size() < 16) {
    std::printf("  FAIL the ducker saved %zu bytes\n", seed.size());
    return 1;
  }
  // Move every parameter off its default inside the blob itself: the values
  // follow the 12-byte header (magic, version, count) as floats.
  {
    uint32_t count = 0;
    std::memcpy(&count, seed.data() + 8, 4);
    for (uint32_t i = 0; i < count && 12 + 4 * (size_t) (i + 1) <= seed.size(); ++i) {
      clap_param_info_t info{};
      params->get_info(plugin, i, &info);
      const float v = (float) (info.min_value + (info.max_value - info.min_value) * 0.3);
      std::memcpy(seed.data() + 12 + 4 * (size_t) i, &v, 4);
    }
    if (!load(plugin, state, seed, 0)) {
      std::printf("  FAIL the ducker refused its own blob with the values moved\n");
      return 1;
    }
    seed = save(plugin, state);
  }

  // The loader's own 32-bit fields, by offset in the seed: version, count,
  // each value, then -- past the one-byte bypass, so none of these is
  // 4-aligned -- preset, editor width and height, and the bag's first words.
  // Half the 32-bit edits land on one of them. Aligned-only edits reached the
  // fields after the bypass about once in ten thousand cases.
  std::vector<size_t> fields = {4, 8};
  {
    uint32_t count = 0;
    std::memcpy(&count, seed.data() + 8, 4);
    for (uint32_t i = 0; i < count; ++i) fields.push_back(12 + 4 * (size_t) i);
    const size_t afterBypass = 12 + 4 * (size_t) count + 1;
    for (size_t k = 0; k < 8; ++k) fields.push_back(afterBypass + 4 * k);
  }

  std::mt19937 rng(0x5EEDF00Du);
  auto pick = [&](size_t n) { return n ? (size_t) (rng() % n) : 0; };
  const uint32_t interesting[] = {0u,          1u,          0x7Fu,       0x80u,       0xFFu,
                                  0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu, 0x7FC00000u, // NaN
                                  0x7F800000u, 0xFF800000u, // +-inf
                                  0x00000001u, // a denormal
                                  0x4E6E6B28u, // 1e9
                                  2u,          3u,          4u,          5u,          6u};

  size_t loaded = 0, refused = 0, failures = 0;
  Rig rig;
  for (size_t it = 0; it < iterations; ++it) {
    std::vector<uint8_t> blob = seed;
    const int edits = 1 + (int) pick(4);
    for (int e = 0; e < edits; ++e) {
      switch (pick(7)) {
        case 0: // flip a bit
          if (!blob.empty()) blob[pick(blob.size())] ^= (uint8_t) (1u << pick(8));
          break;
        case 1: // a byte to an edge value
          if (!blob.empty()) blob[pick(blob.size())] = (uint8_t) interesting[pick(5)];
          break;
        case 2: { // a 32-bit field to an edge value (counts, versions, floats)
          const size_t at = (rng() & 1) ? fields[pick(fields.size())] : pick(blob.size());
          if (at + 4 <= blob.size())
            std::memcpy(blob.data() + at, &interesting[pick(sizeof(interesting) / 4)], 4);
          break;
        }
        case 3: // truncate
          blob.resize(pick(blob.size() + 1));
          break;
        case 4: { // extend with noise
          const size_t more = 1 + pick(64);
          for (size_t i = 0; i < more; ++i) blob.push_back((uint8_t) rng());
          break;
        }
        case 5: // cut a run out of the middle
          if (blob.size() > 2) {
            const size_t at = pick(blob.size() - 1), len = 1 + pick(blob.size() - at - 1);
            blob.erase(blob.begin() + (long) at, blob.begin() + (long) (at + len));
          }
          break;
        default: // duplicate a run
          if (!blob.empty()) {
            const size_t at = pick(blob.size()), len = 1 + pick(blob.size() - at);
            std::vector<uint8_t> run(blob.begin() + (long) at, blob.begin() + (long) (at + len));
            blob.insert(blob.begin() + (long) pick(blob.size() + 1), run.begin(), run.end());
          }
          break;
      }
    }

    const std::vector<uint8_t> before = save(plugin, state);
    // Every few cases the host reads in short pieces, as a stream may.
    const bool ok = load(plugin, state, blob, (it % 5 == 0) ? 1 + pick(7) : 0);
    if (!ok) {
      ++refused;
      if (save(plugin, state) != before) {
        ++failures;
        if (failures <= 5) std::printf("  FAIL case %zu: a refused blob changed the plugin\n", it);
      }
      continue;
    }
    ++loaded;
    float worst = 0.0f;
    if (!runBlock(plugin, ports, rig, &worst) || worst > 64.0f) {
      ++failures;
      if (failures <= 5)
        std::printf("  FAIL case %zu: after a load the next block is not sane (peak %g)\n", it,
                    (double) worst);
    }
    const std::vector<uint8_t> once = save(plugin, state);
    if (!load(plugin, state, once, 0) || save(plugin, state) != once) {
      ++failures;
      if (failures <= 5)
        std::printf("  FAIL case %zu: a loaded state does not survive its own round trip\n", it);
    }
    // Back to the seed for the next case, so every mutation starts from the
    // same saved session.
    load(plugin, state, seed, 0);
  }

  plugin->stop_processing(plugin);
  plugin->deactivate(plugin);
  plugin->destroy(plugin);
  clap_entry.deinit();

  std::printf("  %zu loaded, %zu refused, %zu broke a property\n", loaded, refused, failures);
  if (loaded == 0 || refused == 0) {
    std::printf("  FAIL the mutations never %s -- the fuzz measured nothing\n",
                loaded == 0 ? "loaded" : "were refused");
    return 1;
  }
  std::printf(failures == 0 ? "SONORE STATE FUZZ PASSED\n" : "SONORE STATE FUZZ FAILED\n");
  return failures == 0 ? 0 : 1;
}
