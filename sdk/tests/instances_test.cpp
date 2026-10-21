// SPDX-License-Identifier: Apache-2.0
// Two instances of one plugin share a binary, and so every static in it.
//
// The commonest arrangement there is -- the same plugin on two tracks, set
// differently -- and the one a plugin tested a single instance at a time can
// never fail: a scratch buffer, a cache, a filter table or a "first block"
// flag kept in a static is shared by both, and one track's sound leaks into
// the other's. host_test opens two editors at once; this does the same for
// the audio.
//
// For each plugin given -- files, or a folder scanned for every plugin in it,
// as ctest passes the build's -- instance A at its defaults, instance B with
// every parameter moved, fed the same input in interleaved blocks -- A, B, A,
// B -- and each compared, sample for sample, with an instance holding the
// same settings that rendered on its own before the pair was loaded. They
// must be identical: the same
// code on the same input with the same settings, so any difference is state
// one instance took from the other. Instruments get the same notes as input.
//
//   instances_test <plugin file or folder>...
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <sonore/host.h>

namespace {

int g_checks = 0, g_failures = 0;
void check(bool ok, const char* what) {
  ++g_checks;
  if (!ok) ++g_failures;
  std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
}

constexpr double kRate = 48000.0;
constexpr uint32_t kBlock = 256;
constexpr int kBlocks = 64;

// Every parameter a third of the way from wherever its default sits toward
// whichever end is farther, so a stepped control steps and nothing is left at
// its default.
void moveEverything(sonore::host::HostedPlugin& p) {
  for (int i = 0; i < p.numParameters(); ++i) {
    const sonore::host::HostedParam& info = p.parameter(i);
    const double toMax = info.maxValue - info.defaultValue, toMin = info.defaultValue - info.minValue;
    const double target = toMax >= toMin ? info.defaultValue + toMax * 0.66 : info.defaultValue - toMin * 0.66;
    p.setParameterValue(i, target);
  }
  // Except the bypass. CLAP and VST3 list it among the parameters, and two
  // thirds of the way along it is ENGAGED: the moved instance played its dry
  // input, "sounds as it does alone" compared dry with dry, and the check
  // that the two settings sound different was met by wet against dry for
  // every effect, whatever its own controls did.
  if (p.hasBypass()) p.setBypassed(false);
}

// One block of input: a tone with some noise, different in every block so a
// block that came out of the wrong instance cannot match by accident.
void fillInput(std::vector<float>& l, std::vector<float>& r, int block) {
  uint32_t seed = 0x9E3779B9u ^ (uint32_t) block * 2654435761u;
  for (uint32_t i = 0; i < kBlock; ++i) {
    seed = seed * 1664525u + 1013904223u;
    const double t = ((double) block * kBlock + i) / kRate;
    const float noise = (float) ((double) seed / 4294967296.0 - 0.5) * 0.05f;
    l[i] = 0.3f * (float) std::sin(2.0 * 3.14159265358979 * 220.0 * t) + noise;
    r[i] = 0.25f * (float) std::sin(2.0 * 3.14159265358979 * 330.0 * t) - noise;
  }
}

void fillNotes(sonore::MidiBuffer& midi, int block) {
  midi.clear();
  if (block % 8 == 0) midi.addEvent(sonore::MidiMessage::noteOn(0, 48 + (block / 8) % 24, 100), 17);
  if (block % 8 == 5) midi.addEvent(sonore::MidiMessage::noteOff(0, 48 + (block / 8) % 24), 101);
}

struct Run {
  std::unique_ptr<sonore::host::HostedPlugin> plugin;
  std::vector<float> out; // every block, interleaved stereo
  void block(int index, bool instrument) {
    std::vector<float> l(kBlock), r(kBlock);
    if (instrument) {
      std::fill(l.begin(), l.end(), 0.0f);
      std::fill(r.begin(), r.end(), 0.0f);
    } else {
      fillInput(l, r, index);
    }
    sonore::MidiBuffer midi;
    fillNotes(midi, index);
    float* chans[2] = {l.data(), r.data()};
    sonore::AudioBlock<float> io(chans, 2, kBlock);
    plugin->process(io, &midi);
    for (uint32_t i = 0; i < kBlock; ++i) {
      out.push_back(l[i]);
      out.push_back(r[i]);
    }
  }
};

bool prepared(Run& run, const sonore::host::PluginDescription& d, bool moved) {
  run.plugin = sonore::host::loadPlugin(d);
  if (!run.plugin || !run.plugin->prepare(kRate, kBlock, 2)) return false;
  if (moved) moveEverything(*run.plugin);
  return true;
}

double worstDifference(const std::vector<float>& a, const std::vector<float>& b) {
  if (a.size() != b.size()) return INFINITY;
  double worst = 0.0;
  for (size_t i = 0; i < a.size(); ++i) {
    const double d = std::fabs((double) a[i] - (double) b[i]);
    if (!(d <= worst)) worst = d; // NaN counts as the worst
  }
  return worst;
}

double energy(const std::vector<float>& a) {
  double e = 0.0;
  for (float v : a) e += (double) v * v;
  return e;
}

} // namespace

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  if (argc < 2) {
    std::printf("usage: instances_test <plugin file or folder>...\n");
    return 2;
  }
  std::vector<sonore::host::PluginDescription> plugins;
  int sounding = 0, distinct = 0;
  for (int arg = 1; arg < argc; ++arg) {
    const auto inFile = sonore::host::describeFile(argv[arg]);
    const auto found = inFile.empty() ? sonore::host::scanDirectory(argv[arg]) : inFile;
    if (found.empty()) check(false, (std::string("finds a plugin in ") + argv[arg]).c_str());
    plugins.insert(plugins.end(), found.begin(), found.end());
  }
  for (const sonore::host::PluginDescription& d : plugins) {
    std::printf("\n%s (%s)\n", d.name.c_str(), d.format.c_str());
    const bool instrument = d.isInstrument;

    // Each alone first.
    Run soloA, soloB;
    if (!prepared(soloA, d, false) || !prepared(soloB, d, true)) {
      check(false, "loads and prepares");
      continue;
    }
    for (int b = 0; b < kBlocks; ++b) soloA.block(b, instrument);
    for (int b = 0; b < kBlocks; ++b) soloB.block(b, instrument);
    soloA.plugin.reset();
    soloB.plugin.reset();

    // Then side by side, interleaved, as two tracks are.
    Run a, b;
    if (!prepared(a, d, false) || !prepared(b, d, true)) {
      check(false, "two instances load and prepare side by side");
      continue;
    }
    for (int k = 0; k < kBlocks; ++k) {
      a.block(k, instrument);
      b.block(k, instrument);
    }

    char line[240];
    const double dA = worstDifference(a.out, soloA.out), dB = worstDifference(b.out, soloB.out);
    std::snprintf(line, sizeof(line), "the default instance beside a moved one sounds as it does alone "
                  "(worst difference %.3g)", dA);
    check(dA == 0.0, line);
    std::snprintf(line, sizeof(line), "the moved instance beside a default one sounds as it does alone "
                  "(worst difference %.3g)", dB);
    check(dB == 0.0, line);
    // Whether the two settings really are two sounds: if they are not, the
    // comparison above could not have seen one leak into the other. Counted
    // rather than required per plugin -- an arpeggiator's audio is silence
    // either way, and a test fixture's controls may not touch its sound --
    // and required over the whole run below, so a host that stopped moving
    // parameters could not turn every check here into a comparison of equals.
    const double apart = worstDifference(soloA.out, soloB.out);
    if (energy(soloA.out) + energy(soloB.out) > 0.0) {
      ++sounding;
      if (apart > 1e-4) ++distinct;
      else std::printf("  ---- its two settings sound the same: nothing here to leak ----\n");
    } else {
      std::printf("  ---- silent at both settings: nothing to tell apart ----\n");
    }
  }
  char line[200];
  std::snprintf(line, sizeof(line), "the two settings sounded different in %d of the %d plugins "
                "that make sound -- the comparisons measured something", distinct, sounding);
  check(sounding > 0 && distinct * 2 >= sounding, line);
  std::printf("\n%d checks, %d failure(s)\n", g_checks, g_failures);
  std::printf(g_failures == 0 ? "SONORE INSTANCE ISOLATION PASSED\n" : "SONORE INSTANCE ISOLATION FAILED\n");
  return g_failures == 0 ? 0 : 1;
}
