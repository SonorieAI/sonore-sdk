// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: the hosting API, exercised against the plugins this SDK builds.
//
// sonore/host.h is the other direction: not being a plugin, but loading one.
// The obvious way to test it is also the strongest one available here: point
// it at our own build output and make it scan, load, drive, automate and
// round-trip state on every .clap in the folder.
//
// That is a real test rather than a circular one, because the two sides were
// written against the SPEC rather than against each other: the wrapper answers
// what CLAP says a plugin must answer, and the host asks what CLAP says a host
// may ask. Where they disagree, one of them is wrong about the spec.

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "temp_path.h"
#include <sonore/host.h>
#include <sonore/plugin_cache.h>

#include <atomic>
#include <new>

#if defined(_WIN32)
#include <windows.h>
#endif

// The header says process() allocates nothing. That is a claim about the
// audio thread, and claims about the audio thread are worth exactly what they
// are measured to be worth -- so the same counter the RT audit uses is armed
// around the hosted process() calls below.
static std::atomic<bool> g_armed{false};
static std::atomic<long> g_allocs{0};

static inline void countIfArmed() {
  if (g_armed.load(std::memory_order_relaxed)) g_allocs.fetch_add(1, std::memory_order_relaxed);
}

using sonoretest::tempPath; // every file a test writes goes under ONE temp
                            // directory, swept at exit -- see temp_path.h

static int g_checks = 0;
static int g_failures = 0;

static void check(bool ok, const char* what) {
  ++g_checks;
  if (!ok) ++g_failures;
  std::printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
}

static void checkNear(double got, double want, double tolerance, const char* what) {
  const bool ok = std::fabs(got - want) <= tolerance;
  ++g_checks;
  if (!ok) ++g_failures;
  std::printf("  %-4s %s (%.4f)\n", ok ? "ok" : "FAIL", what, got);
}

void* operator new(std::size_t n) {
  countIfArmed();
  void* p = std::malloc(n ? n : 1);
  if (!p) throw std::bad_alloc();
  return p;
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

int main(int argc, char** argv) {
  // Unbuffered, because this test loads other people's code into its own
  // process and a crash must still show how far it got. Block-buffered stdout
  // throws away the last few hundred lines exactly when they matter most.
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  if (argc < 2) {
    std::printf("usage: host_test <directory-of-clap-plugins>\n");
    return 2;
  }
  const std::string directory = argv[1];
  std::printf("── hosting: %s ──────────────────────────────────────\n", directory.c_str());

  // ── Scan ──────────────────────────────────────────────────────────────────
  const std::vector<sonore::host::PluginDescription> found =
      sonore::host::scanDirectory(directory);
  std::printf("  ---- %d plugins found ----\n", (int) found.size());
  check(!found.empty(), "the scan finds plugins in the build output");

  bool sawClap = false, sawVst3 = false, sawLv2 = false;
  for (const auto& d : found) {
    if (d.format == "CLAP") sawClap = true;
    if (d.format == "VST3") sawVst3 = true;
    if (d.format == "LV2") sawLv2 = true;
  }
  check(sawClap, "the scan finds CLAP plugins");
  check(sawVst3, "…and VST3 plugins, which are a different shape entirely");
  // LV2 is the one that cannot be described without reading RDF: its name,
  // its ports and its binary all live in Turtle beside the code.
  check(sawLv2, "…and LV2 bundles, whose metadata had to be PARSED to find at all");

  bool sawInstrument = false, sawEffect = false;
  for (const auto& d : found) {
    if (d.isInstrument) sawInstrument = true;
    else sawEffect = true;
    if (d.id.empty() || d.name.empty()) {
      check(false, "every description carries an id and a name");
      break;
    }
  }
  check(sawInstrument, "at least one is recognised as an instrument");
  check(sawEffect, "…and at least one as an effect");

  // A scan must not need the plugin loaded to say what it is, so nothing above
  // this line has instantiated anything.
  {
    const auto missing = sonore::host::scanDirectory(directory + "/does-not-exist");
    check(missing.empty(), "scanning a directory that is not there returns nothing, not a crash");
  }

  // ── Load and drive every one of them ──────────────────────────────────────
  int loaded = 0, withParams = 0, stateRoundTrips = 0, automated = 0;
  for (const auto& description : found) {
    auto plugin = sonore::host::loadPlugin(description);
    if (!plugin || !plugin->isValid()) {
      std::printf("  FAIL could not load %s\n", description.name.c_str());
      ++g_failures;
      ++g_checks;
      continue;
    }
    ++loaded;

    if (!plugin->prepare(48000.0, 256, 2)) {
      std::printf("  FAIL could not prepare %s\n", description.name.c_str());
      ++g_failures;
      ++g_checks;
      continue;
    }

    // Audio through it. A NaN or an infinity coming back means the host handed
    // the plugin something malformed, which is the failure this catches.
    std::vector<float> left(256, 0.0f), right(256, 0.0f);
    float* channels[2] = {left.data(), right.data()};
    bool finite = true;
    uint32_t lcg = 22222u;
    for (int block = 0; block < 32; ++block) {
      for (int i = 0; i < 256; ++i) {
        lcg = lcg * 1664525u + 1013904223u;
        const float v = (float) ((int32_t) (lcg >> 8) % 20001 - 10000) / 40000.0f;
        left[(size_t) i] = right[(size_t) i] = v;
      }
      sonore::AudioBlock<float> io(channels, 2, 256);
      sonore::MidiBuffer midi;
      if (plugin->acceptsMidi() && block == 0)
        midi.addEvent(sonore::MidiMessage::noteOn(0, 60, 100), 0);
      plugin->process(io, &midi);
      for (int i = 0; i < 256; ++i)
        if (!std::isfinite(left[(size_t) i]) || !std::isfinite(right[(size_t) i])) finite = false;
    }
    if (!finite) {
      std::printf("  FAIL %s produced non-finite audio when hosted\n", description.name.c_str());
      ++g_failures;
      ++g_checks;
    }

    // Parameters, read back through the host's own view of them.
    if (plugin->numParameters() > 0) {
      ++withParams;
      const sonore::host::HostedParam& p = plugin->parameter(0);
      const bool sane = p.maxValue > p.minValue && p.defaultValue >= p.minValue &&
                        p.defaultValue <= p.maxValue && !p.name.empty();
      if (!sane) {
        std::printf("  FAIL %s parameter 0 is not describable\n", description.name.c_str());
        ++g_failures;
        ++g_checks;
      }

      // Automation. The value must be there on the NEXT block, not this one:
      // the host queues it as an event, which is the only race-free way.
      const double target = p.minValue + (p.maxValue - p.minValue) * 0.75;
      plugin->setParameterValue(0, target);
      sonore::AudioBlock<float> io(channels, 2, 256);
      plugin->process(io, nullptr);
      if (std::fabs(plugin->parameterValue(0) - target) < 1e-4) ++automated;
      else {
        std::printf("  FAIL %s did not take the queued parameter change (%f vs %f)\n",
                    description.name.c_str(), plugin->parameterValue(0), target);
        ++g_failures;
        ++g_checks;
      }

      // …and the plugin's own rendering of it, which is what a host displays.
      const std::string text = plugin->parameterText(0, target);
      if (text.empty()) {
        std::printf("  FAIL %s cannot print its own parameter value\n", description.name.c_str());
        ++g_failures;
        ++g_checks;
      }
    }

    // State, round-tripped through the host: save, move a control, reload, and
    // the control must come back. This is the operation a session depends on.
    std::vector<uint8_t> saved;
    if (plugin->saveState(saved) && !saved.empty() && plugin->numParameters() > 0) {
      const double before = plugin->parameterValue(0);
      const sonore::host::HostedParam& p = plugin->parameter(0);
      const double moved = (std::fabs(before - p.maxValue) > 1e-6) ? p.maxValue : p.minValue;
      plugin->setParameterValue(0, moved);
      sonore::AudioBlock<float> io(channels, 2, 256);
      plugin->process(io, nullptr);
      if (plugin->loadState(saved.data(), saved.size()) &&
          std::fabs(plugin->parameterValue(0) - before) < 1e-4)
        ++stateRoundTrips;
      else {
        std::printf("  FAIL %s state did not round-trip through the host\n",
                    description.name.c_str());
        ++g_failures;
        ++g_checks;
      }
    }

    // Latency and tail are what a host needs to place the plugin in time.
    if (plugin->latencySamples() > 100000) {
      std::printf("  FAIL %s reports an absurd latency\n", description.name.c_str());
      ++g_failures;
      ++g_checks;
    }

    plugin->release();
  }

  std::printf("  ---- loaded %d, %d with parameters, %d automated, %d state round-trips ----\n",
              loaded, withParams, automated, stateRoundTrips);
  check(loaded == (int) found.size(), "every plugin the scan described actually loads");
  check(automated == withParams, "every parameter takes a queued change");
  check(stateRoundTrips == withParams, "every plugin's state survives a host round-trip");

  // ── Failure modes a real session hits ─────────────────────────────────────
  {
    sonore::host::PluginDescription bogus;
    bogus.format = "CLAP";
    bogus.path = directory + "/not-a-plugin.clap";
    bogus.id = "nothing";
    check(sonore::host::loadPlugin(bogus) == nullptr,
          "a file that is not there fails to load rather than crashing");

    if (!found.empty()) {
      sonore::host::PluginDescription wrongId = found[0];
      wrongId.id = "com.example.does-not-exist";
      check(sonore::host::loadPlugin(wrongId) == nullptr,
            "an id the file does not contain fails to load");

      // "AU", not "VST3": VST3 is supported now, so naming it here would make
      // this check pass because the CLAP file has no VST3 factory rather than
      // because the format was refused. A check that passes for the wrong
      // reason is worse than no check.
      sonore::host::PluginDescription wrongFormat = found[0];
      wrongFormat.format = "AU";
      check(sonore::host::loadPlugin(wrongFormat) == nullptr,
            "a format this host does not do is refused, not guessed at");
    }
  }

  // ── Two at once, and one outliving the other ──────────────────────────────
  // Each hosted plugin owns its module, so a second load of the same file must
  // not disturb the first, and destroying one must not unload the other's code
  // out from under it.
  if (found.size() >= 1) {
    auto first = sonore::host::loadPlugin(found[0]);
    check(first && first->isValid(), "a plugin loads");
    {
      auto second = sonore::host::loadPlugin(found[0]);
      check(second && second->isValid(), "…and the same file loads a second time");
      if (second) second->prepare(48000.0, 128, 2);
    } // second is destroyed here

    if (first) {
      check(first->prepare(48000.0, 128, 2),
            "…and the first still works after the second is gone");
      std::vector<float> l(128, 0.1f), r(128, 0.1f);
      float* ch[2] = {l.data(), r.data()};
      sonore::AudioBlock<float> io(ch, 2, 128);
      first->process(io, nullptr);
      bool ok = true;
      for (int i = 0; i < 128; ++i)
        if (!std::isfinite(l[(size_t) i])) ok = false;
      check(ok, "…and still produces finite audio");
    }
  }

  // ── The promise process() makes ───────────────────────────────────────────
  if (!found.empty()) {
    // Prove the counter works before trusting it: a counter that silently
    // stopped counting would make this section pass forever.
    g_armed.store(true);
    { std::vector<double> deliberate; deliberate.resize(2048); if (deliberate[0] != 0.0) return 9; }
    const bool counterWorks = g_allocs.load() > 0;
    g_armed.store(false);
    g_allocs.store(0);
    check(counterWorks, "the allocation counter can see an allocation (self-check)");

    for (const char* wantFormat : {"CLAP", "VST3", "LV2"}) {
    const sonore::host::PluginDescription* pick = nullptr;
    for (const auto& d : found)
      if (d.format == wantFormat) { pick = &d; break; }
    if (!pick) continue;
    auto plugin = sonore::host::loadPlugin(*pick);
    if (plugin && plugin->prepare(48000.0, 256, 2)) {
      std::vector<float> l(256, 0.0f), r(256, 0.0f);
      float* ch[2] = {l.data(), r.data()};
      sonore::MidiBuffer midi;
      midi.addEvent(sonore::MidiMessage::noteOn(0, 60, 100), 0);
      midi.addEvent(sonore::MidiMessage::controlChange(0, 1, 64), 32);

      // One warm-up pass outside the guard: the first call through any path
      // can build a one-time static, and counting that would blame the host
      // for something that happens once in a process's life.
      { sonore::AudioBlock<float> io(ch, 2, 256); plugin->process(io, &midi); }

      g_allocs.store(0);
      g_armed.store(true);
      for (int block = 0; block < 200; ++block) {
        for (int i = 0; i < plugin->numParameters() && i < 8; ++i)
          plugin->setParameterValue(i, plugin->parameter(i).defaultValue);
        // Block sizes that are not the maximum, because a buffer sized for
        // exactly one length is a buffer that gets resized for the others.
        const int frames = (block % 3 == 0) ? 256 : (block % 3 == 1 ? 129 : 64);
        sonore::AudioBlock<float> io(ch, 2, (size_t) frames);
        plugin->process(io, &midi);
      }
      g_armed.store(false);
      const long n = g_allocs.load();
      std::printf("  ---- %s: allocations inside 200 hosted process() calls: %ld ----\n",
                  wantFormat, n);
      check(n == 0, "process() allocates nothing, as the header promises");
    }
    }
  }

  // ── Bypass, through one call in three formats ─────────────────────────────
  //
  // Every format has a bypass and no two spell it alike: CLAP flags a
  // parameter, VST3 flags a different one, and LV2 designates a control port
  // whose sense is INVERTED. An application hosting a rack needs to switch a
  // plugin off without knowing which of those it has.
  //
  // The check is the CONTRACT, not the flag: engage it and the output must
  // BECOME the input, delayed by the plugin's own reported latency. A bypass
  // that merely mutes, or that forgets the dry delay, or that leaves the DSP
  // in the path would each fail this, and the inverted LV2 sense would fail
  // it in the most obvious way of all, by bypassing when asked not to.
  {
    int checkedFormats = 0;
    for (const char* wantFormat : {"CLAP", "VST3", "LV2"}) {
      const sonore::host::PluginDescription* pick = nullptr;
      for (const auto& d : found) {
        if (d.format != wantFormat || d.isInstrument) continue;
        // The reverb: an effect with real latency, so "delayed by its own
        // reported amount" is a claim with something in it.
        if (d.name.find("Reverb") != std::string::npos) pick = &d;
      }
      if (!pick) continue;

      auto plugin = sonore::host::loadPlugin(*pick);
      if (!plugin || !plugin->prepare(48000.0, 256, 2)) continue;
      if (!plugin->hasBypass()) {
        check(false, "an effect exposes a bypass through the hosting API");
        continue;
      }
      ++checkedFormats;
      check(!plugin->isBypassed(), "…which starts disengaged");
      check(plugin->setBypassed(true), "…and engages");

      const uint32_t latency = plugin->latencySamples();
      std::vector<float> l(256), r(256);
      float* channels[2] = {l.data(), r.data()};

      // The crossfade is 20 ms; comparing during it measures the ramp.
      std::vector<float> sent, got;
      int phase = 0;
      for (int b = 0; b < 60; ++b) {
        for (int i = 0; i < 256; ++i) {
          const float v =
              0.25f * (float) std::sin(2.0 * 3.14159265358979 * 997.0 * phase / 48000.0);
          l[(size_t) i] = r[(size_t) i] = v;
          sent.push_back(v);
          ++phase;
        }
        sonore::AudioBlock<float> io(channels, 2, 256);
        plugin->process(io, nullptr);
        for (int i = 0; i < 256; ++i) got.push_back(l[(size_t) i]);
      }

      double worst = 0.0;
      size_t compared = 0;
      for (size_t i = 30 * 256; i + latency < got.size() && i < sent.size(); ++i) {
        const double err = std::fabs((double) got[i + latency] - (double) sent[i]);
        if (err > worst) worst = err;
        ++compared;
      }
      std::printf("  ---- %s bypass: %zu samples compared at %u latency, worst %.4g ----\n",
                  wantFormat, compared, (unsigned) latency, worst);
      check(compared > 1000, "there is enough settled audio to judge");
      check(worst < 1e-3, "…bypassed output IS the input, delayed by the reported latency");

      check(plugin->isBypassed(), "…and the host can read the state back");
      check(plugin->setBypassed(false), "…and disengage it again");
      sonore::AudioBlock<float> io(channels, 2, 256);
      plugin->process(io, nullptr);
      check(!plugin->isBypassed(), "…which reads back as disengaged");
    }
    check(checkedFormats == 3, "all three formats bypass through the same call");
  }

  // ── Factory presets, through one call for three unrelated mechanisms ────
  //
  // The plugin ships three presets from one compiled-in table. CLAP publishes
  // them through a preset-discovery FACTORY that a host must crawl before it
  // knows any exist; VST3 hangs a program list off a unit and selects with a
  // parameter flagged kIsProgramChange; LV2 writes pset:Preset into the
  // bundle's Turtle, so finding them means reading RDF. Three formats, three
  // mechanisms, and an application showing a preset menu should not have to
  // know which one it is holding.
  //
  // The check is not that a name comes back. It is that the SOUND changes,
  // and that the same preset gives the SAME sound in all three formats,
  // which is the claim that fails if an index is off by one, if a Turtle
  // symbol does not match a port, or if a program change moves the editor
  // and leaves the processor where it was.
  {
    struct FormatResult {
      std::string format;
      std::vector<std::string> names;
      std::vector<double> rms;
    };
    std::vector<FormatResult> results;

    for (const char* wantFormat : {"CLAP", "VST3", "LV2"}) {
      const sonore::host::PluginDescription* pick = nullptr;
      for (const auto& d : found)
        if (d.format == wantFormat && d.name.find("Saturator") != std::string::npos) pick = &d;
      if (!pick) continue;

      auto plugin = sonore::host::loadPlugin(*pick);
      if (!plugin || !plugin->prepare(48000.0, 256, 2)) {
        check(false, "the plugin that ships presets loads");
        continue;
      }

      FormatResult result;
      result.format = wantFormat;
      const int n = plugin->numPresets();
      if (n <= 0) {
        std::printf("  ---- %s: no presets found ----\n", wantFormat);
        check(false, "a plugin with factory presets publishes them to a host");
        continue;
      }

      for (int i = 0; i < n; ++i) result.names.push_back(plugin->presetName(i));

      // Out of range is a normal question with a normal answer, not a crash.
      check(plugin->presetName(-1).empty() && plugin->presetName(n).empty(),
            "asking for a preset that is not there gives nothing rather than misbehaving");
      check(!plugin->loadPreset(-1) && !plugin->loadPreset(n), "…and loading one is refused");

      for (int i = 0; i < n; ++i) {
        check(plugin->loadPreset(i), "every published preset loads");

        // Long enough that any parameter smoothing has arrived: what is being
        // compared is the settled sound of the preset, not the ramp into it.
        std::vector<float> l(256), r(256);
        float* channels[2] = {l.data(), r.data()};
        double sum = 0.0;
        size_t counted = 0;
        int phase = 0;
        for (int b = 0; b < 80; ++b) {
          for (int k = 0; k < 256; ++k) {
            const float v =
                0.25f * (float) std::sin(2.0 * 3.14159265358979 * 220.0 * phase / 48000.0);
            l[(size_t) k] = r[(size_t) k] = v;
            ++phase;
          }
          sonore::AudioBlock<float> io(channels, 2, 256);
          plugin->process(io, nullptr);
          if (b < 60) continue; // settle
          for (int k = 0; k < 256; ++k) {
            sum += (double) l[(size_t) k] * (double) l[(size_t) k];
            ++counted;
          }
        }
        result.rms.push_back(std::sqrt(sum / (double) counted));
      }

      std::printf("  ---- %s presets:", wantFormat);
      for (size_t i = 0; i < result.names.size(); ++i)
        std::printf(" \"%s\"=%.5f", result.names[i].c_str(), result.rms[i]);
      std::printf(" ----\n");
      results.push_back(result);
    }

    check(results.size() == 3, "all three formats publish the same plugin's presets");

    if (results.size() == 3) {
      for (const FormatResult& r : results) {
        check(r.names.size() == 3, "…all three of them");
        bool named = true;
        for (const std::string& name : r.names)
          if (name.empty()) named = false;
        check(named, "…each with a name of its own");

        // A preset that changes nothing is worse than no preset, because the
        // user believes it loaded. Drive 2.5 into 18 through a saturator is
        // not a subtle difference and must not read as one.
        bool distinct = true;
        for (size_t i = 0; i < r.rms.size(); ++i)
          for (size_t k = i + 1; k < r.rms.size(); ++k)
            if (std::fabs(r.rms[i] - r.rms[k]) < 1e-4) distinct = false;
        check(distinct, "…and each one actually changes the sound");
      }

      // The cross-format claim: same table, same preset, same audio. This is
      // what an off-by-one in a program parameter or an unmatched Turtle
      // symbol breaks, and neither is visible from one format alone.
      double worstName = 0.0, worstAudio = 0.0;
      bool namesAgree = true;
      for (size_t i = 1; i < results.size(); ++i) {
        for (size_t k = 0; k < results[0].names.size() && k < results[i].names.size(); ++k) {
          if (results[i].names[k] != results[0].names[k]) namesAgree = false;
          const double diff = std::fabs(results[i].rms[k] - results[0].rms[k]);
          if (diff > worstAudio) worstAudio = diff;
        }
      }
      (void) worstName;
      std::printf("  ---- cross-format: names %s, worst RMS difference %.3g ----\n",
                  namesAgree ? "identical" : "DIFFER", worstAudio);
      check(namesAgree, "the same preset has the same name in every format");
      check(worstAudio < 2e-3, "…and produces the same audio in every format");
    }
  }

  // ── What the plugin says back about its own parameters ────────────────
  //
  // Automation is not one-way. When a preset moves four knobs, the plugin has
  // to say so, or the host records nothing and keeps drawing the values from
  // before: the preset is heard and not seen, which a user reads as a preset
  // that did not load.
  //
  // Each format says it differently. CLAP emits CLAP_EVENT_PARAM_VALUE into
  // the output event list, and asks for a flush when it is idle so an edit
  // made with the transport stopped does not wait for playback. VST3 has the
  // host implement IComponentHandler and calls restartComponent with
  // kParamValuesChanged: this host had no handler at all, so a hosted
  // plugin had nowhere to report anything. LV2 has no channel for it, because
  // a control port is a float the HOST owns and the plugin only reads.
  //
  // The check is that the host learns the SAME values it can read back, one
  // per parameter, without processing a single block first.
  {
    int reportingFormats = 0;
    for (const char* wantFormat : {"CLAP", "VST3", "LV2"}) {
      const sonore::host::PluginDescription* pick = nullptr;
      for (const auto& d : found)
        if (d.format == wantFormat && d.name.find("Saturator") != std::string::npos) pick = &d;
      if (!pick) continue;

      auto plugin = sonore::host::loadPlugin(*pick);
      if (!plugin || !plugin->prepare(48000.0, 256, 2)) continue;

      // Anything said during load is water under the bridge; what is being
      // measured is what THIS preset load reports.
      std::vector<sonore::host::ParamEdit> edits;
      plugin->drainParameterEdits(edits);
      edits.clear();

      std::vector<double> before;
      for (int i = 0; i < plugin->numParameters(); ++i) before.push_back(plugin->parameterValue(i));

      const int last = plugin->numPresets() - 1;
      check(last > 0, "there is more than one preset to move between");
      check(plugin->loadPreset(last), "the last preset loads");

      // Deliberately no process() call in between. A host with the transport
      // stopped is the case that breaks if a plugin only reports through the
      // audio thread, and it is the common one: nobody loads presets while
      // the tape is rolling.
      const size_t reported = plugin->drainParameterEdits(edits);

      if (std::string(wantFormat) == "LV2") {
        // Not a gap in this host: the format has no such channel, and
        // inventing edits to make three formats look alike would be a lie
        // that a caller building an automation lane would believe.
        check(reported == 0 && edits.empty(),
              "LV2 reports nothing, because a control port is the host's own float");
        std::printf("  ---- LV2: no parameter-edit channel exists in the format ----\n");
        continue;
      }

      ++reportingFormats;
      check(reported > 0, "the plugin says its parameters moved");

      // Every parameter the plugin named must carry the value the host can
      // read back independently. A report that says "something changed" with
      // the wrong number in it is worse than no report.
      double worst = 0.0;
      int valueEdits = 0, badIndex = 0;
      for (const auto& e : edits) {
        if (e.kind != sonore::host::ParamEdit::Kind::kValue) continue;
        ++valueEdits;
        if (e.index < 0 || e.index >= plugin->numParameters()) {
          ++badIndex;
          continue;
        }
        const double diff = std::fabs(e.value - plugin->parameterValue(e.index));
        if (diff > worst) worst = diff;
      }
      std::printf("  ---- %s: %d parameter edits reported, worst disagreement %.4g ----\n",
                  wantFormat, valueEdits, worst);
      check(badIndex == 0, "…each naming a parameter that exists");

      // The claim with teeth: every parameter that MOVED was named. A preset
      // does not touch bypass, so demanding one report per parameter would be
      // demanding a lie; demanding one per parameter that actually changed is
      // the thing a host needs to write an automation lane correctly. A
      // report that quietly omits one knob leaves that lane stale forever.
      int moved = 0, unreported = 0;
      for (int i = 0; i < plugin->numParameters() && i < (int) before.size(); ++i) {
        if (std::fabs(plugin->parameterValue(i) - before[i]) < 1e-9) continue;
        ++moved;
        bool named = false;
        for (const auto& e : edits)
          if (e.kind == sonore::host::ParamEdit::Kind::kValue && e.index == i) named = true;
        if (!named) ++unreported;
      }
      std::printf("  ---- %s: %d parameters moved, %d of them unreported ----\n", wantFormat,
                  moved, unreported);
      check(moved > 0, "…the preset moved something to report in the first place");
      check(unreported == 0, "…and every parameter that moved was named");

      // Plain units, not normalised: 8x of drive has to arrive as 8, not as
      // 0.37. This is the conversion VST3 needs and CLAP does not, and
      // getting it wrong is invisible until two formats are compared.
      check(worst < 1e-4, "…and every reported value is the one the host reads back");

      // The values must also be the PRESET's, not the defaults: a report
      // that faithfully echoes what the plugin was already at proves nothing.
      bool movedFromDefault = false;
      for (const auto& e : edits) {
        if (e.kind != sonore::host::ParamEdit::Kind::kValue) continue;
        if (e.index < 0 || e.index >= plugin->numParameters()) continue;
        const auto& info = plugin->parameter(e.index);
        if (std::fabs(e.value - info.defaultValue) > 1e-6) movedFromDefault = true;
      }
      check(movedFromDefault, "…and they are the preset's values, not the defaults");
    }
    check(reportingFormats == 2,
          "both formats that have a channel for it report parameter edits");
  }

#if defined(_WIN32)
  // ── The plugin's own face, in a window this test owns ─────────────────
  //
  // A host that can load a plugin and not show it is half a host. CLAP hands
  // over a clap_window_t and the plugin parents itself into it; VST3 makes the
  // host create an IPlugView, give it an IPlugFrame, and attach it to a
  // platform handle. Both are easy to get wrong in a way that returns true
  // and shows nothing.
  //
  // So the check is not the return value. A real window is created here, the
  // editor is opened into it, and the test looks for a CHILD window that was
  // not there before, which is the operating system agreeing that something
  // was embedded. Then it closes and the child has to be gone, because an
  // editor that leaks its window leaks it into the caller's.
  {
    int embedded = 0;
    for (const char* wantFormat : {"CLAP", "VST3", "LV2"}) {
      const sonore::host::PluginDescription* pick = nullptr;
      for (const auto& d : found)
        if (d.format == wantFormat && d.name.find("Saturator") != std::string::npos) pick = &d;
      if (!pick) continue;

      auto plugin = sonore::host::loadPlugin(*pick);
      if (!plugin) continue;
      if (!plugin->hasEditor()) {
        std::printf("  ---- %s: reports no editor on this platform ----\n", wantFormat);
        continue;
      }

      HWND parent = CreateWindowExW(0, L"STATIC", L"sonore editor host", WS_OVERLAPPEDWINDOW, 0, 0,
                                    900, 600, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
      check(parent != nullptr, "the test can make a window to embed into");
      if (!parent) continue;

      check(GetWindow(parent, GW_CHILD) == nullptr, "which starts with nothing in it");
      const bool opened = plugin->openEditor((void*) parent);
      check(opened, "the plugin's editor opens into it");
      if (!opened) {
        DestroyWindow(parent);
        continue;
      }

      // A webview takes its time coming up, so this waits for the OS rather
      // than assuming a duration. Reporting how long it took keeps a
      // regression from hiding behind a generous timeout.
      HWND child = nullptr;
      DWORD waited = 0;
      const DWORD started = GetTickCount();
      for (int i = 0; i < 600 && !child; ++i) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
          TranslateMessage(&msg);
          DispatchMessageW(&msg);
        }
        // LV2 has no loop of its own: the bundle asks for ui:idleInterface
        // and this is the host driving it. The other two ignore the call.
        plugin->idleEditor();
        child = GetWindow(parent, GW_CHILD);
        if (!child) Sleep(10);
      }
      waited = GetTickCount() - started;

      uint32_t width = 0, height = 0;
      const bool sized = plugin->editorSize(width, height);
      std::printf("  ---- %s editor: child %s after %ums, size %ux%u ----\n", wantFormat,
                  child ? "embedded" : "NEVER APPEARED", (unsigned) waited, (unsigned) width,
                  (unsigned) height);

      check(child != nullptr, "…and the window it made is a child of ours");
      check(sized, "…and it says what size it wants");
      check(width > 0 && height > 0, "…which is a size rather than nothing");

      plugin->closeEditor();
      MSG msg;
      while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
      }
      check(GetWindow(parent, GW_CHILD) == nullptr,
            "…and closing it leaves the caller's window as empty as it found it");
      check(!plugin->editorSize(width, height), "…with no size to report once closed");

      DestroyWindow(parent);
      ++embedded;
    }
    check(embedded == 3, "all three formats embed an editor");
  }

  // ── A scaled display ────────────────────────────────────
  //
  // Most Windows laptops ship at 125% or 150%. The page is laid out in
  // logical pixels whatever the monitor does, and the webview draws each of
  // them as `scale` device pixels, so a window built at the LOGICAL size
  // shows the top-left corner of a correctly drawn page and cuts off the
  // rest. VST3 has a whole separate interface for this and we implemented
  // none of it, which told every host we were not DPI aware.
  //
  // The check is the WINDOW, measured through the OS. Not the number the
  // plugin reports about itself, which is what it would say either way.
  {
    // BOTH formats, held to the same claim. They share one webview, so a
    // difference between them is a plugin that is sharp in one host and
    // clipped in another.
    for (const char* wantFormat : {"VST3", "CLAP"}) {
    const sonore::host::PluginDescription* pick = nullptr;
    for (const auto& d : found)
      if (d.format == wantFormat && d.name.find("Saturator") != std::string::npos) pick = &d;

    if (pick) {
      uint32_t base[2] = {0, 0};
      uint32_t doubled[2] = {0, 0};
      LONG childWidth[2] = {0, 0};

      for (int pass = 0; pass < 2; ++pass) {
        auto plugin = sonore::host::loadPlugin(*pick);
        if (!plugin || !plugin->hasEditor()) break;
        if (pass == 1)
          check(plugin->setEditorScale(2.0), "the plugin accepts a content scale from the host");

        HWND parent = CreateWindowExW(0, L"STATIC", L"sonore dpi", WS_OVERLAPPEDWINDOW, 0, 0, 2000,
                                      1400, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!parent) break;
        if (!plugin->openEditor((void*) parent)) {
          DestroyWindow(parent);
          break;
        }
        HWND child = nullptr;
        for (int i = 0; i < 400 && !child; ++i) {
          MSG msg;
          while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
          }
          child = GetWindow(parent, GW_CHILD);
          if (!child) Sleep(10);
        }
        uint32_t* into = pass == 0 ? base : doubled;
        plugin->editorSize(into[0], into[1]);
        if (child) {
          RECT rect{};
          GetClientRect(child, &rect);
          childWidth[pass] = rect.right - rect.left;
        }
        plugin->closeEditor();
        DestroyWindow(parent);
      }

      std::printf("  ---- %s at 100%%: %ux%u reported, %ld px window | at 200%%: %ux%u, %ld px "
                  "----\n",
                  wantFormat, base[0], base[1], (long) childWidth[0], doubled[0], doubled[1],
                  (long) childWidth[1]);
      check(base[0] > 0 && doubled[0] > 0, "the editor reports a size at both scales");
      check(doubled[0] == base[0] * 2 && doubled[1] == base[1] * 2,
            "…and asks for twice the pixels when a logical one is worth two");
      // The one that actually matters. A view can report whatever it likes;
      // the window is what the user sees the page drawn into.
      check(childWidth[0] > 0 && childWidth[1] == childWidth[0] * 2,
            "…and the window it makes is twice as wide, not the same size cut in half");
    }
    }
  }

  // A scale that cannot be drawn is refused rather than clamped quietly.
  {
    const sonore::host::PluginDescription* pick = nullptr;
    for (const auto& d : found)
      if (d.format == "VST3" && d.name.find("Saturator") != std::string::npos) pick = &d;
    if (pick) {
      auto plugin = sonore::host::loadPlugin(*pick);
      if (plugin) {
        check(!plugin->setEditorScale(0.0), "a scale of zero is refused");
        check(!plugin->setEditorScale(-1.5), "and so is a negative one");
      }
    }
  }

  // ── Two of the same plugin, both editors open ───────────────────────
  //
  // The commonest thing anyone does with a plugin: put it on two tracks and
  // open both. It is also where per-MODULE state stops being harmless, because
  // the two instances share every static in the binary: one window class, one
  // webview user-data folder, one COM apartment.
  //
  // Opened at the SAME TIME rather than one after the other, which is the case
  // that a shared static breaks and a sequential test would miss entirely.
  {
    const sonore::host::PluginDescription* pick = nullptr;
    for (const auto& d : found)
      if (d.format == "CLAP" && d.name.find("Saturator") != std::string::npos) pick = &d;

    if (pick) {
      auto first = sonore::host::loadPlugin(*pick);
      auto second = sonore::host::loadPlugin(*pick);
      check(first && second, "the same plugin loads twice");

      if (first && second) {
        HWND windows[2] = {};
        sonore::host::HostedPlugin* both[2] = {first.get(), second.get()};
        for (int i = 0; i < 2; ++i)
          windows[i] = CreateWindowExW(0, L"STATIC", L"sonore two editors", WS_OVERLAPPEDWINDOW,
                                       0, 0, 900, 600, nullptr, nullptr,
                                       GetModuleHandleW(nullptr), nullptr);

        int opened = 0;
        for (int i = 0; i < 2; ++i)
          if (windows[i] && both[i]->openEditor((void*) windows[i])) ++opened;
        check(opened == 2, "both instances open an editor of their own");

        HWND children[2] = {};
        for (int spin = 0; spin < 600 && !(children[0] && children[1]); ++spin) {
          MSG msg;
          while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
          }
          for (int i = 0; i < 2; ++i) {
            both[i]->idleEditor();
            if (windows[i]) children[i] = GetWindow(windows[i], GW_CHILD);
          }
          if (!(children[0] && children[1])) Sleep(10);
        }

        std::printf("  ---- two instances: first %s, second %s ----\n",
                    children[0] ? "embedded" : "EMPTY", children[1] ? "embedded" : "EMPTY");
        check(children[0] && children[1], "…and both of them draw into their own window");
        // Distinct windows, not one editor moved from the first parent to the
        // second: a shared static that hands back the same window would pass
        // the check above and be catastrophic in a session.
        check(children[0] != children[1], "…which are two windows, not one shared between them");

        for (int i = 0; i < 2; ++i) both[i]->closeEditor();
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
          TranslateMessage(&msg);
          DispatchMessageW(&msg);
        }
        // Closing ONE must not take the other's window with it, which is what
        // a shared static would do. Both are closed here, so both are empty;
        // the interesting half is that neither crashed doing it.
        for (int i = 0; i < 2; ++i)
          if (windows[i]) DestroyWindow(windows[i]);
      }
    }
  }
#endif

  // ── Remembering a scan, and what killed one ────────────────────
  //
  // Scanning means LOADING SOMEBODY ELSE'S CODE: every file in the folder
  // gets opened and its entry point called. Slow, and dangerous: one plugin
  // that faults on load takes the host down every single launch, and the user
  // has no way past it.
  //
  // The cache answers both. The second answer is a dead man's switch: the
  // path is written and FLUSHED before the file is opened, so a plugin that
  // was mid-scan when the process died is a plugin that killed it.
  {
    const char* kCachePath = tempPath("sonore-scan-cache.txt");
    std::remove(kCachePath);

    sonore::host::PluginCache cache;
    check(!cache.load(kCachePath), "a cache that is not there does not load");

    const std::vector<std::string> files = sonore::host::pluginFilesIn(directory);
    check(!files.empty(), "the folder has candidate files in it");

    const std::vector<sonore::host::PluginDescription> firstPass =
        sonore::host::scanDirectoryCached(directory, cache, kCachePath);
    std::printf("  ---- first scan: %zu candidate files, %zu plugins ----\n",
                files.size(), firstPass.size());
    check(firstPass.size() == found.size(),
          "a cached scan finds exactly what the plain scan finds");

    // ── Persisted, and read back ──
    sonore::host::PluginCache reloaded;
    check(reloaded.load(kCachePath), "the cache was written and reads back");
    check(reloaded.plugins().size() == firstPass.size(),
          "…with every plugin the scan described");
    check(reloaded.crashedOn().empty(),
          "…and no file marked as in progress, because none crashed");

    // ── The second scan opens nothing ──
    //
    // The measurable claim, and the reason the cache exists. Not "it is
    // faster", that is a stopwatch and a mood. Every file is UP TO DATE, so
    // none of them is a candidate for loading.
    size_t upToDate = 0;
    for (const std::string& file : files)
      if (reloaded.isUpToDate(file)) ++upToDate;
    std::printf("  ---- second scan: %zu of %zu files already known ----\n", upToDate,
                files.size());
    check(upToDate == files.size(), "a second scan finds every file already known");

    const std::vector<sonore::host::PluginDescription> secondPass =
        sonore::host::scanDirectoryCached(directory, reloaded, kCachePath);
    check(secondPass.size() == firstPass.size(), "…and describes the same plugins");

    // ── A file that changed is scanned again ──
    //
    // The stamp is size AND time, because either alone is fooled by something
    // ordinary: a rebuild landing on the same size, or an installer that
    // preserves timestamps.
    {
      sonore::host::PluginCache stale;
      stale.load(kCachePath);
      // A path the cache has never heard of stands in for a changed file: the
      // question either way is "does the stamp match", and a file that is not
      // in the cache has no stamp to match.
      check(!stale.isUpToDate(directory + "/something-new.clap"),
            "a file the cache has not seen is not up to date");
    }

    // ── The dead man's switch ──
    //
    // Written by hand, because the honest way to produce one is to crash the
    // process inside a plugin load, and a test that does that has no way to
    // report what it found. What is checked is the RECOVERY: given a cache
    // with a file marked in progress, the next scan must blacklist it and
    // must not open it.
    {
      const std::string victim = files[0];
      {
        std::FILE* file = std::fopen(kCachePath, "wb");
        check(file != nullptr, "a cache can be written by hand");
        if (file) {
          std::fprintf(file, "SONORE-PLUGIN-CACHE-1\nSCANNING\t%s\n", victim.c_str());
          std::fclose(file);
        }
      }

      sonore::host::PluginCache afterCrash;
      check(afterCrash.load(kCachePath), "the cache from a run that died loads");
      check(afterCrash.crashedOn() == victim, "…and names the file it died inside");
      check(!afterCrash.isBlacklisted(victim), "…which is not blacklisted yet");

      const std::vector<sonore::host::PluginDescription> afterPass =
          sonore::host::scanDirectoryCached(directory, afterCrash, kCachePath);
      check(afterCrash.isBlacklisted(victim),
            "a file that was mid-scan when the host died is blacklisted");

      // And it is genuinely SKIPPED, not merely listed. Whatever was in that
      // file must be missing from the results.
      size_t fromVictim = 0;
      for (const auto& d : afterPass)
        if (d.path == victim) ++fromVictim;
      std::printf("  ---- after a crash: %zu blacklisted, %zu plugins from the blamed file ----\n",
                  afterCrash.blacklist().size(), fromVictim);
      check(fromVictim == 0, "…and nothing from it is loaded again");
      check(afterPass.size() < firstPass.size(),
            "…so the scan returns fewer plugins than before");

      // A user who updated the plugin can give it another chance.
      check(afterCrash.forgive(victim), "a blacklisted plugin can be forgiven");
      check(!afterCrash.isBlacklisted(victim), "…and is not blacklisted afterwards");
      const std::vector<sonore::host::PluginDescription> forgiven =
          sonore::host::scanDirectoryCached(directory, afterCrash, kCachePath);
      check(forgiven.size() == firstPass.size(), "…and is scanned again");
    }

    // A cache from a future version is thrown away rather than misread. A
    // host that will not start because of its own cache is worse than one
    // that rescans.
    {
      std::FILE* file = std::fopen(kCachePath, "wb");
      if (file) {
        std::fprintf(file, "SONORE-PLUGIN-CACHE-99\nPLUGIN\tnonsense\n");
        std::fclose(file);
      }
      sonore::host::PluginCache future;
      check(!future.load(kCachePath), "a cache written by a future version is refused");
      check(future.plugins().empty(), "…and leaves nothing half-read behind");
    }

    std::remove(kCachePath);
  }

  // ── Presets as FILES ───────────────────────────────────
  //
  // A plugin's factory presets live inside it. That is not where most presets
  // are: people have folders of .vstpreset files, bought and downloaded and
  // saved from another host years ago. A host that cannot open one starts
  // empty however much the user already owns.
  //
  // The claim is a round trip through a FILE ON DISK and a SECOND INSTANCE.
  // Saving and reloading into the same object proves almost nothing: the
  // plugin could be returning what it already had.
  {
    const char* kPresetPath = tempPath("sonore-test.vstpreset");
    std::remove(kPresetPath);

    int checkedFormats = 0;
    for (const char* wantFormat : {"VST3", "CLAP", "LV2"}) {
      const sonore::host::PluginDescription* pick = nullptr;
      for (const auto& d : found)
        if (d.format == wantFormat && d.name.find("Saturator") != std::string::npos) pick = &d;
      if (!pick) continue;

      auto first = sonore::host::loadPlugin(*pick);
      if (!first || !first->prepare(48000.0, 256, 2)) continue;

      // Move the controls somewhere they would not be by default, so a preset
      // that silently did nothing would be indistinguishable from a working
      // one only if the defaults happened to match.
      for (int i = 0; i < first->numParameters(); ++i) {
        const auto& info = first->parameter(i);
        first->setParameterValue(i, info.minValue + (info.maxValue - info.minValue) * 0.31);
      }
      // A BLOCK, before reading anything back. CLAP queues parameter writes
      // and the plugin only sees them on the next process or flush -- reading
      // straight after setting returns the values from before, which is
      // correct of CLAP and made this test compare defaults against defaults
      // and call it a pass. LV2 writes ports directly and VST3 tells its
      // controller at once, so only one of the three needed it and only one
      // of the three would have shown it.
      {
        std::vector<float> l(256, 0.0f), r(256, 0.0f);
        float* channels[2] = {l.data(), r.data()};
        sonore::AudioBlock<float> io(channels, 2, 256);
        first->process(io, nullptr);
      }

      // Read back in a SECOND pass, after every write has landed. VST3
      // publishes a program-change parameter alongside the real ones, and
      // moving it loads a preset -- which overwrites the parameters set
      // before it in the same loop. Recording each value as it was written
      // captured numbers that no longer existed by the end.
      std::vector<double> written;
      for (int i = 0; i < first->numParameters(); ++i)
        written.push_back(first->parameterValue(i));

      check(sonore::host::savePresetFile(*first, *pick, kPresetPath),
            "a plugin's state saves to a preset file");

      // A SECOND instance, at its defaults, which the preset has to move.
      auto second = sonore::host::loadPlugin(*pick);
      if (!second || !second->prepare(48000.0, 256, 2)) continue;
      bool differsBefore = false;
      for (int i = 0; i < second->numParameters() && i < (int) written.size(); ++i)
        if (std::fabs(second->parameterValue(i) - written[(size_t) i]) > 1e-6) differsBefore = true;
      check(differsBefore, "…and a fresh instance does not already match it");

      check(sonore::host::loadPresetFile(*second, *pick, kPresetPath),
            "…the file loads into the fresh instance");

      double worst = 0.0;
      for (int i = 0; i < second->numParameters() && i < (int) written.size(); ++i) {
        const double err = std::fabs(second->parameterValue(i) - written[(size_t) i]);
        if (err > worst) worst = err;
      }
      std::printf("  ---- %s preset file: %d parameters, worst difference %.3g ----\n",
                  wantFormat, second->numParameters(), worst);
      check(worst < 1e-6, "…and every parameter comes back where it was");
      ++checkedFormats;
      std::remove(kPresetPath);
    }
    check(checkedFormats == 3, "every format round-trips a preset through a file");
  }

  // ── An older state blob still loads ─────────────────────────
  //
  // The version byte is only worth carrying if it is honoured. v4 added the
  // selected preset between the bypass byte and the DSP bag; a v3 blob has
  // the bag straight after the bypass, and reading four bytes of bag as a
  // preset index would restore a session with a preset nobody chose and a
  // bag that starts four bytes late.
  //
  // Built by editing a real v4 blob rather than by hand, so it is exactly
  // what the previous version wrote.
  {
    const sonore::host::PluginDescription* pick = nullptr;
    for (const auto& d : found)
      if (d.format == "CLAP" && d.name.find("Saturator") != std::string::npos) pick = &d;
    if (pick) {
      auto plugin = sonore::host::loadPlugin(*pick);
      if (plugin && plugin->prepare(48000.0, 256, 2)) {
        for (int i = 0; i < plugin->numParameters(); ++i) {
          const auto& info = plugin->parameter(i);
          plugin->setParameterValue(i, info.minValue + (info.maxValue - info.minValue) * 0.42);
        }
        std::vector<float> l(256, 0.0f), r(256, 0.0f);
        float* channels[2] = {l.data(), r.data()};
        sonore::AudioBlock<float> io(channels, 2, 256);
        plugin->process(io, nullptr);

        std::vector<double> written;
        for (int i = 0; i < plugin->numParameters(); ++i)
          written.push_back(plugin->parameterValue(i));

        std::vector<uint8_t> v4;
        check(plugin->saveState(v4), "a current state blob saves");
        check(v4.size() > 17 && v4[4] == 5, "…declaring version 5");

        // v3: the version byte back, and the four preset bytes taken out.
        std::vector<uint8_t> v3 = v4;
        v3[4] = 3;
        const size_t presetAt = 12 + 4 * (size_t) plugin->numParameters() + 1;
        if (presetAt + 4 <= v3.size())
          v3.erase(v3.begin() + (ptrdiff_t) presetAt, v3.begin() + (ptrdiff_t) presetAt + 4);

        auto fresh = sonore::host::loadPlugin(*pick);
        if (fresh && fresh->prepare(48000.0, 256, 2)) {
          check(fresh->loadState(v3.data(), v3.size()), "a version 3 blob still loads");
          double worst = 0.0;
          for (int i = 0; i < fresh->numParameters() && i < (int) written.size(); ++i) {
            const double err = std::fabs(fresh->parameterValue(i) - written[(size_t) i]);
            if (err > worst) worst = err;
          }
          std::printf("  ---- v3 blob into a v4 build: worst difference %.3g ----\n", worst);
          check(worst < 1e-6, "…with every parameter where the older version put it");
        }
      }
    }
  }

  // The .vstpreset container itself, read back field by field. The round trip
  // above would pass even if the header were nonsense, because the same code
  // wrote and read it: this checks the bytes are the format other hosts
  // expect.
  {
    const char* kPath = tempPath("sonore-container.vstpreset");
    const std::string classId = "0123456789ABCDEF0123456789ABCDEF";
    std::vector<uint8_t> state;
    for (int i = 0; i < 500; ++i) state.push_back((uint8_t) (i * 7));

    check(sonore::writeVstPreset(kPath, classId, state), "a preset container writes");

    // The header, by hand: "VST3", version 1, the class id as 32 ASCII
    // characters at offset 8.
    {
      std::FILE* file = std::fopen(kPath, "rb");
      check(file != nullptr, "…and can be opened as bytes");
      if (file) {
        uint8_t header[48] = {};
        const size_t got = std::fread(header, 1, sizeof(header), file);
        std::fclose(file);
        check(got == sizeof(header), "…with a full header in it");
        check(std::memcmp(header, "VST3", 4) == 0, "…beginning VST3, as the format says");
        check(std::memcmp(header + 8, classId.data(), 32) == 0,
              "…and naming the plugin it belongs to");
      }
    }

    sonore::Vst3PresetFile read;
    check(sonore::readVstPreset(kPath, &read), "the container reads back");
    check(read.classId == classId, "…with the same class id");
    check(read.componentState == state, "…and the same bytes, exactly");
    std::remove(kPath);
  }

  // A preset for a DIFFERENT plugin is refused. Nothing else in the file says
  // what it is for, and the plugin cannot tell: a state blob is opaque bytes
  // and it will happily swallow another plugin's.
  {
    const char* kPath = tempPath("sonore-wrong.vstpreset");
    const sonore::host::PluginDescription* vst3 = nullptr;
    for (const auto& d : found)
      if (d.format == "VST3" && d.name.find("Saturator") != std::string::npos) vst3 = &d;
    if (vst3) {
      auto plugin = sonore::host::loadPlugin(*vst3);
      if (plugin && plugin->prepare(48000.0, 256, 2)) {
        std::vector<uint8_t> state;
        plugin->saveState(state);
        check(sonore::writeVstPreset(kPath, "FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF", state),
              "a preset can be written naming another plugin");
        check(!sonore::host::loadPresetFile(*plugin, *vst3, kPath),
              "…and loading it into the wrong plugin is refused");
      }
    }
    std::remove(kPath);
  }

  // Corrupt files are refused rather than half applied. A preset half applied
  // is worse than one not applied, because the user cannot tell which half.
  {
    const char* kPath = tempPath("sonore-corrupt.vstpreset");
    sonore::Vst3PresetFile read;

    check(!sonore::readVstPreset("no-such-preset.vstpreset", &read), "a missing preset is refused");
    check(!sonore::readVstPreset(nullptr, &read), "and so is a null path");

    auto writeBytes = [&](const std::vector<uint8_t>& bytes) {
      std::FILE* file = std::fopen(kPath, "wb");
      if (!file) return false;
      std::fwrite(bytes.data(), 1, bytes.size(), file);
      std::fclose(file);
      return true;
    };

    writeBytes({'N', 'O', 'P', 'E'});
    check(!sonore::readVstPreset(kPath, &read), "a file that is not a preset at all is refused");

    // A valid header whose list offset points past the end. The shape a
    // truncated download has.
    {
      std::vector<uint8_t> bytes(48, 0);
      std::memcpy(bytes.data(), "VST3", 4);
      for (int i = 0; i < 32; ++i) bytes[(size_t) (8 + i)] = 'A';
      bytes[40] = 0xff;
      bytes[41] = 0xff;
      writeBytes(bytes);
      check(!sonore::readVstPreset(kPath, &read),
            "a chunk list pointing past the end of the file is refused");
    }

    // A list claiming more entries than the file could hold. The shape that
    // would allocate gigabytes if it were believed.
    {
      std::vector<uint8_t> bytes(60, 0);
      std::memcpy(bytes.data(), "VST3", 4);
      for (int i = 0; i < 32; ++i) bytes[(size_t) (8 + i)] = 'A';
      bytes[40] = 48; // list at offset 48
      std::memcpy(bytes.data() + 48, "List", 4);
      bytes[52] = 0xff;
      bytes[53] = 0xff;
      bytes[54] = 0xff;
      bytes[55] = 0x0f;
      writeBytes(bytes);
      check(!sonore::readVstPreset(kPath, &read),
            "a count that cannot fit in the file is refused rather than allocated");
    }
    std::remove(kPath);
  }

  // ── The state an LV2 plugin keeps outside its ports ───────────────────────
  //
  // A control port is a float and nothing else. Everything a plugin knows
  // beyond its knobs, which sample a sampler loaded, which impulse a reverb
  // read: travels through the state: extension as typed key/value pairs, and
  // a host that saved only the ports would restore a session with the knobs
  // right and the sound wrong.
  //
  // This host used to save only the ports and say so. The check is that the
  // extension is now actually CONSULTED: a plugin that has one produces a
  // bigger blob than its ports account for, and one that does not produces
  // exactly its ports and nothing more.
  {
    int withExtension = 0, total = 0;
    bool roundTripped = false;
    for (const auto& description : found) {
      if (description.format != "LV2") continue;
      auto plugin = sonore::host::loadPlugin(description);
      if (!plugin || !plugin->prepare(48000.0, 256, 2)) continue;
      ++total;

      std::vector<uint8_t> saved;
      if (!plugin->saveState(saved)) {
        check(false, "an LV2 plugin's state saves");
        continue;
      }
      // The layout: a count of ports, their floats, then a count of
      // properties. Anything past that is the extension having stored
      // something.
      const size_t portBytes = 4 + (size_t) plugin->numParameters() * sizeof(float) + 4;
      if (saved.size() > portBytes) ++withExtension;

      if (roundTripped) continue; // one full round trip is enough
      roundTripped = true;

      const double before = plugin->numParameters() > 0 ? plugin->parameterValue(0) : 0.0;
      if (plugin->numParameters() > 0) {
        const auto& p = plugin->parameter(0);
        plugin->setParameterValue(0, std::fabs(before - p.maxValue) > 1e-6 ? p.maxValue
                                                                          : p.minValue);
      }
      const bool restored = plugin->loadState(saved.data(), saved.size());
      check(restored, "…and restores");
      if (restored && plugin->numParameters() > 0)
        check(std::fabs(plugin->parameterValue(0) - before) < 1e-4,
              "…bringing the port values back with it");

      // A blob claiming a different port count is refused rather than read as
      // if the floats happened to line up.
      std::vector<uint8_t> wrong = saved;
      wrong[0] = (uint8_t) (wrong[0] + 7);
      check(!plugin->loadState(wrong.data(), wrong.size()),
            "…while a blob claiming a different port count is refused");

      // A blob that stops after the ports is a blob written before this host
      // understood the extension. Restoring what it DOES say is right;
      // refusing it would orphan every session saved by the older build.
      std::vector<uint8_t> older(saved.begin(), saved.begin() + (long) portBytes - 4);
      check(plugin->loadState(older.data(), older.size()),
            "…and one written before the extension was understood still loads");
    }

    std::printf("  ---- LV2 state: %d of %d plugins store beyond their ports ----\n",
                withExtension, total);
    check(total > 0, "there are LV2 plugins to save state from");
    // Every plugin this SDK builds carries a state blob, so all of them.
    // Asserting "and some do not" would be asserting something about a plugin
    // set that does not exist here.
    check(withExtension == total,
          "every one of them had its state: extension consulted, not just its ports");
  }

  // ── The MIDI a VST3 does not deliver as MIDI ──────────────────────────────
  //
  // Pitch bend has no event form in VST3: it travels as a parameter change on
  // an id the plugin publishes through IMidiMapping. So a host that takes a
  // MidiBuffer has to do that translation itself, and this is the check that
  // it does: the note must actually bend.
  //
  // A caller of this API writes one MidiBuffer and gets the same behaviour
  // from both formats, which is the entire point of there being an interface.
  for (const auto& description : found) {
    if (!description.isInstrument) continue;
    auto plugin = sonore::host::loadPlugin(description);
    if (!plugin || !plugin->acceptsMidi() || !plugin->prepare(48000.0, 256, 2)) continue;

    std::vector<float> l(256, 0.0f), r(256, 0.0f);
    float* ch[2] = {l.data(), r.data()};
    std::vector<float> captured;
    auto capture = [&](int blocks, const sonore::MidiBuffer* midi) {
      captured.clear();
      for (int b = 0; b < blocks; ++b) {
        sonore::AudioBlock<float> io(ch, 2, 256);
        plugin->process(io, b == 0 ? midi : nullptr);
        for (int i = 0; i < 256; ++i) captured.push_back(l[(size_t) i]);
      }
    };
    auto pitchOf = [&](const std::vector<float>& sig) {
      const size_t window = sig.size() / 2;
      if (window < 1000) return 0.0;
      double best = 0.0;
      size_t bestLag = 0;
      for (size_t lag = 48; lag < 800 && lag + window < sig.size(); ++lag) {
        double num = 0.0, a = 0.0, b = 0.0;
        for (size_t i = 0; i < window; ++i) {
          const double x = sig[i], y = sig[i + lag];
          num += x * y;
          a += x * x;
          b += y * y;
        }
        const double d = std::sqrt(a * b);
        const double rr = d > 1e-12 ? num / d : 0.0;
        if (rr > best) { best = rr; bestLag = lag; }
      }
      return bestLag > 0 ? 48000.0 / (double) bestLag : 0.0;
    };

    sonore::MidiBuffer on;
    on.addEvent(sonore::MidiMessage::noteOn(0, 45, 100), 0); // A2, 110 Hz
    capture(94, &on);
    capture(94, nullptr);
    const double plain = pitchOf(captured);

    // A whole tone up. 8192 is centre; MPE's default range is 48 semitones,
    // so +2 of those is 8192 + 8191 * 2/48.
    sonore::MidiBuffer bend;
    bend.addEvent(sonore::MidiMessage::pitchBend(0, 8192 + (int) (8191.0 * 2.0 / 48.0)), 0);
    capture(94, &bend);
    capture(94, nullptr);
    const double bent = pitchOf(captured);

    std::printf("  ---- %s %s: %.1f Hz -> %.1f Hz ----\n", description.format.c_str(),
                description.name.c_str(), plain, bent);
    if (plain > 0.0)
      checkNear(bent / plain, 1.1225, 0.03,
                "a pitch bend written to the host API bends the note, in either format");

    sonore::MidiBuffer off;
    off.addEvent(sonore::MidiMessage::noteOff(0, 45), 0);
    capture(4, &off);
  }

  std::printf("\n%d checks, %d failure(s)\n", g_checks, g_failures);
  if (g_failures == 0) std::printf("SONORE HOST PASSED\n");
  return g_failures == 0 ? 0 : 1;
}
