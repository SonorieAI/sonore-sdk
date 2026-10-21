// SPDX-License-Identifier: Apache-2.0
// The plugin-state parsers, fed what a session file, a plugin binary or a
// process table can hand them.
//
//   * AudioProcessorValueTreeState::replaceState, and the undo path that
//     restores a serialised state, with mutations of a real saved state. A
//     state that loads must leave every parameter finite, in range and on a
//     legal position, and be a fixed point after one round trip.
//   * LadspaPlugin::decodeState -- a LADSPA plugin's saved controls, from a
//     session file -- with mutations of a real blob.
//   * LadspaPlugin::describePort with arbitrary hint words and bounds,
//     NaN and infinity included: they are read out of somebody else's binary,
//     and the range that comes back must still be a range.
//   * PluginHostType's classifier and version reader, and the plugin list's
//     category reader, on arbitrary text.
//
//   fuzz_plugin_state                     a quick pass (ctest)
//   fuzz_plugin_state --iterations 50000  the deep pass, under ASan
#include <sonore/gfx/plugin_list.h>
#include <sonore/host.h>
#include <sonore/plugin_host_type.h>
#include <sonore/value_tree_state.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

using namespace sonore;

namespace {

int g_failures = 0;

void fail(const char* what, long iteration) {
  if (g_failures < 20) std::printf("  FAIL %s (iteration %ld)\n", what, iteration);
  ++g_failures;
}

AudioProcessorValueTreeState::ParameterLayout layout() {
  AudioProcessorValueTreeState::ParameterLayout l;
  NormalisableRange<float> cutoff(20.0f, 20000.0f, 0.0f);
  cutoff.setSkewForCentre(1000.0f);
  l.add(std::make_unique<AudioParameterFloat>("gain", "Gain", NormalisableRange<float>(-60.0f, 12.0f, 0.1f), 0.0f),
        std::make_unique<AudioParameterFloat>("cutoff", "Cutoff", cutoff, 1000.0f),
        std::make_unique<AudioParameterInt>("voices", "Voices", 1, 16, 8),
        std::make_unique<AudioParameterBool>("bypass", "Bypass", false),
        std::make_unique<AudioParameterChoice>("mode", "Mode", std::vector<std::string>{"A", "B", "C"}, 1));
  return l;
}

std::vector<uint8_t> bytesOf(const StateBag& bag) {
  std::vector<uint8_t> out;
  bag.serialise(out);
  return out;
}

std::vector<uint8_t> mutate(std::vector<uint8_t> b, std::mt19937& rng) {
  std::uniform_int_distribution<int> kind(0, 7);
  const int rounds = 1 + (int) (rng() % 4);
  for (int r = 0; r < rounds; ++r) {
    const size_t n = b.size();
    switch (kind(rng)) {
      case 0:
        if (n) b[rng() % n] ^= (uint8_t) (1u << (rng() % 8));
        break;
      case 1:
        if (n) b.resize(rng() % n);
        break;
      case 2:
        b.insert(b.begin() + (long) (n ? rng() % (n + 1) : 0), (uint8_t) (rng() & 0xff));
        break;
      case 3:
        if (n) b[rng() % n] = (uint8_t) (rng() & 0xff);
        break;
      case 4: { // a length field made enormous
        if (n >= 4) {
          const size_t at = rng() % (n - 3);
          b[at] = b[at + 1] = b[at + 2] = 0xff;
          b[at + 3] = (uint8_t) (rng() & 0xff);
        }
        break;
      }
      case 5: { // a value replaced by a number nobody should have saved
        static const char* const kWords[] = {"nan", "-inf", "inf", "1e999", "-1e38", "abc", "", "0x10"};
        const std::string w = kWords[rng() % 8];
        if (n) {
          const size_t at = rng() % n;
          for (size_t i = 0; i < w.size() && at + i < b.size(); ++i) b[at + i] = (uint8_t) w[i];
        }
        break;
      }
      case 6:
        if (n > 1) b.erase(b.begin() + (long) (rng() % n));
        break;
      default:
        for (int i = 0; i < 8; ++i) b.push_back((uint8_t) (rng() & 0xff));
        break;
    }
  }
  return b;
}

bool sane(const AudioProcessorValueTreeState& s) {
  for (int i = 0; i < s.numParameters(); ++i) {
    const RangedAudioParameter* p = s.parameterAt(i);
    const float v = p->rawValue().load();
    if (!(v == v) || v < p->info().minValue || v > p->info().maxValue) return false;
    if (p->legalise(v) != v) return false;
  }
  return true;
}

std::string randomText(std::mt19937& rng) {
  static const char* const kPieces[] = {"reaper", "Bitwig", ".vst3/", ".clap", "\\", "/", ".exe",
                                        "Cubase", "12", "9999999999", ".component/", "live",
                                        "com.apple.logic", "", " ", "\xC3\xA9", ".lv2/", "fl"};
  std::string s;
  const int n = (int) (rng() % 6);
  for (int i = 0; i < n; ++i) {
    if (rng() % 3 == 0) s.push_back((char) (rng() & 0xff));
    else s += kPieces[rng() % (sizeof(kPieces) / sizeof(kPieces[0]))];
  }
  return s;
}

float wildFloat(std::mt19937& rng) {
  switch (rng() % 6) {
    case 0: return std::nanf("");
    case 1: return INFINITY;
    case 2: return -INFINITY;
    case 3: return (float) ((int) (rng() % 2000) - 1000);
    case 4: return 3.0e38f;
    default: {
      uint32_t bits = (uint32_t) rng();
      float f;
      std::memcpy(&f, &bits, sizeof(f));
      return f;
    }
  }
}

} // namespace

int main(int argc, char** argv) {
  long iterations = 2000;
  for (int i = 1; i + 1 < argc; ++i)
    if (std::strcmp(argv[i], "--iterations") == 0) iterations = std::atol(argv[i + 1]);
  std::printf("fuzz_plugin_state: %ld iterations per target\n", iterations);
  std::mt19937 rng(20261013u);

  // ── APVTS state ──
  AudioProcessorValueTreeState source(nullptr, "PARAMETERS", layout());
  source.parameterAt(0)->setValueNotifyingHost(0.3f);
  source.parameterAt(4)->setValueNotifyingHost(1.0f);
  source.state.setString("sample", "kick.wav");
  source.state.setDouble("tempo", 128.5);
  const std::vector<uint8_t> seed = bytesOf(source.copyState());
  UndoHistory history;
  AudioProcessorValueTreeState target(&history, "PARAMETERS", layout());
  long loaded = 0;
  for (long it = 0; it < iterations; ++it) {
    const std::vector<uint8_t> blob = mutate(seed, rng);
    StateBag bag;
    if (!bag.deserialise(blob.data(), blob.size())) continue;
    ++loaded;
    target.replaceState(bag);
    if (!sane(target)) fail("replaceState left a parameter out of range or off its steps", it);
    const std::vector<uint8_t> once = bytesOf(target.copyState());
    StateBag again;
    again.deserialise(once.data(), once.size());
    target.replaceState(again);
    if (bytesOf(target.copyState()) != once) fail("a loaded state is not a fixed point", it);

    // The undo path restores a serialised state too: hand it the raw mutation.
    history.push(std::string(blob.begin(), blob.end()), "fuzz");
    target.undo();
    if (!sane(target)) fail("undo into a damaged state left a parameter insane", it);
  }
  std::printf("  APVTS: %ld of %ld mutated states parsed and were applied\n", loaded, iterations);

  // ── LADSPA state ──
  std::vector<uint8_t> ladspaSeed = {'L', 'D', 'S', 'P', 1, 0, 0, 0, 3, 0, 0, 0};
  for (uint32_t port = 0; port < 3; ++port) {
    const float v = 0.5f * (float) port;
    uint32_t bits;
    std::memcpy(&bits, &v, 4);
    for (int k = 0; k < 4; ++k) ladspaSeed.push_back((uint8_t) (port >> (8 * k)));
    for (int k = 0; k < 4; ++k) ladspaSeed.push_back((uint8_t) (bits >> (8 * k)));
  }
  long accepted = 0;
  for (long it = 0; it < iterations; ++it) {
    const std::vector<uint8_t> blob = mutate(ladspaSeed, rng);
    std::vector<std::pair<uint32_t, float>> out;
    if (!host::LadspaPlugin::decodeState(blob.data(), blob.size(), &out)) continue;
    ++accepted;
    if (blob.size() != 12 + out.size() * 8) fail("decodeState accepted a blob of the wrong length", it);
    for (const auto& e : out)
      if (!(e.second == e.second)) fail("decodeState accepted a NaN", it);
  }
  std::printf("  LADSPA state: %ld of %ld mutated blobs accepted, each exactly well formed\n", accepted,
              iterations);

  // ── LADSPA port hints ──
  for (long it = 0; it < iterations; ++it) {
    ladspa::PortRangeHint hint{(int) (rng() & 0x3ff), wildFloat(rng), wildFloat(rng)};
    const double rate = (rng() % 4 == 0) ? 0.0 : (double) (8000 + rng() % 184000);
    const host::HostedParam p = host::LadspaPlugin::describePort(hint, rate);
    const bool finite = std::isfinite(p.minValue) && std::isfinite(p.maxValue) && std::isfinite(p.defaultValue);
    if (!finite || !(p.maxValue > p.minValue) || p.defaultValue < p.minValue || p.defaultValue > p.maxValue)
      fail("describePort produced something that is not a range with a default inside it", it);
  }
  std::printf("  LADSPA hints: %ld arbitrary hints, every one a finite range\n", iterations);

  // ── Host type and categories ──
  for (long it = 0; it < iterations; ++it) {
    const std::string a = randomText(rng), b = randomText(rng), c = randomText(rng);
    const PluginHostType::HostType t = PluginHostType::classify(a, b, c);
    if (std::strlen(PluginHostType::describe(t)) == 0) fail("a host with no description", it);
    const int major = PluginHostType::majorVersionOf(a);
    if (major < 0 || major > 999) fail("a major version out of its range", it);
    (void) PluginHostType::pluginFormatFromModulePath(a, b);
    const PluginHostType h = PluginHostType::fromProcess(a, b, c, randomText(rng));
    if (!h.getHostDescription() || !h.getHostDescription()[0]) fail("an empty host description", it);
    host::PluginDescription d;
    d.features = randomText(rng) + ";" + randomText(rng) + "|" + randomText(rng);
    if (gfx::PluginListComponent::categoryOf(d).empty()) fail("an empty category", it);
  }
  std::printf("  host type and category: %ld random texts classified\n", iterations);

  std::printf(g_failures == 0 ? "FUZZ PLUGIN STATE PASSED\n" : "FUZZ PLUGIN STATE FAILED (%d)\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}
