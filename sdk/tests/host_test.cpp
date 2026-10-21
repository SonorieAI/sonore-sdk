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

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "temp_path.h"
#include <sonore/host.h>
#include <sonore/plugin_cache.h>

#include <atomic>
#include <new>
#include <thread>

#if defined(_WIN32)
#include <malloc.h> // _msize
#include <windows.h>
#else
#include <time.h> // clock_gettime, CLOCK_THREAD_CPUTIME_ID
#endif

// What this thread has spent on the CPU, in units that only a ratio of two
// readings gives meaning to (cycles on Windows, nanoseconds elsewhere). A
// wall clock counts the time another process held the core as well: on a
// busy machine the longer of two measured loads was preempted more often
// than the shorter, and their ratio crossed a bound the code met.
static double threadCpuNow() {
#if defined(_WIN32)
  ULONG64 cycles = 0;
  QueryThreadCycleTime(GetCurrentThread(), &cycles);
  return (double) cycles;
#else
  timespec t{};
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
  return (double) t.tv_sec * 1e9 + (double) t.tv_nsec;
#endif
}

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

// The bytes this test's operator new has handed out and not had back: what
// the HOST holds for the plugins it loaded. Windows only, where a module
// allocates through an operator new of its own, so none of a plugin's own
// bytes are in it; elsewhere a plugin's allocations can resolve to this one.
static std::atomic<long long> g_liveBytes{0};
static inline void countBytes(void* p, long long sign) {
#if defined(_WIN32)
  if (p) g_liveBytes.fetch_add(sign * (long long) _msize(p), std::memory_order_relaxed);
#else
  (void) p;
  (void) sign;
#endif
}

void* operator new(std::size_t n) {
  countIfArmed();
  void* p = std::malloc(n ? n : 1);
  if (!p) throw std::bad_alloc();
  countBytes(p, 1);
  return p;
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept {
  countBytes(p, -1);
  std::free(p);
}
void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete(void* p, std::size_t) noexcept { ::operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { ::operator delete(p); }

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

  // ── A bundle's binary, for this host's architecture ───────────────────────
  //
  // The folder inside Contents was x86_64 whatever the host was built for, so
  // a 32-bit or ARM64 host found nothing in a bundle that held its binary. A
  // bundle holding only this build's folder must resolve to it.
  {
    namespace fs = std::filesystem;
    std::error_code error;
    const fs::path bundle = fs::temp_directory_path(error) / "sonore_host_arch" / "Arch.vst3";
    fs::remove_all(bundle.parent_path(), error);
    fs::create_directories(bundle / "Contents" / sonore::host::vst3ArchFolder(), error);
#if defined(_WIN32)
    const char* leaf = "Arch.vst3";
#elif defined(__APPLE__)
    const char* leaf = "Arch";
#else
    const char* leaf = "Arch.so";
#endif
    std::ofstream(bundle / "Contents" / sonore::host::vst3ArchFolder() / leaf) << "x";
    const std::string binary = sonore::host::vst3BinaryPath(bundle.string());
    char line[400];
    std::snprintf(line, sizeof(line), "a bundle's binary is found in this host's own folder, %s (%s)",
                  sonore::host::vst3ArchFolder(), binary.c_str());
    check(fs::exists(fs::u8path(binary), error), line);
    fs::remove_all(bundle.parent_path(), error);
  }

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

  // An LV2 bundle's author is RDF too. Read the way lilv reads it, a list of
  // plugins shows the same vendor whatever the format.
  {
    int twins = 0, agree = 0;
    for (const auto& l : found) {
      if (l.format != "LV2") continue;
      for (const auto& c : found) {
        if (c.format != "CLAP" || c.name != l.name || c.vendor.empty()) continue;
        ++twins;
        if (c.vendor == l.vendor) ++agree;
        else std::printf("       %s: LV2 vendor \"%s\", CLAP \"%s\"\n", l.name.c_str(),
                         l.vendor.c_str(), c.vendor.c_str());
        break;
      }
    }
    std::printf("       %d of %d LV2 plugins name the vendor their CLAP does\n", agree, twins);
    check(twins > 0 && agree == twins, "an LV2 plugin's vendor is its doap:maintainer, as CLAP reports it");
    bool projectInFeatures = false;
    for (const auto& d : found)
      if (d.format == "LV2" && d.features.find("doap#Project") != std::string::npos)
        projectInFeatures = true;
    check(!projectInFeatures, "…and a doap:Project type is not taken for a category");
  }
  // What a value is called, LV2 edition: there is no call into the plugin for
  // its text, so the host builds it from the bundle -- a named step's label,
  // else the number and the unit's symbol. The saturator's table is fixed in
  // its source (x, Hz, %, dB); the showcase's Shape names its steps, and the
  // CLAP build of the same plugin says what each is called.
  {
    auto byName = [&](const char* format, const char* name) -> const sonore::host::PluginDescription* {
      for (const auto& d : found)
        if (d.format == format && d.name == name) return &d;
      return nullptr;
    };
    auto paramIndex = [](const sonore::host::HostedPlugin& p, const char* name) {
      for (int i = 0; i < p.numParameters(); ++i)
        if (p.parameter(i).name == name) return i;
      return -1;
    };
    if (const auto* d = byName("LV2", "Sonore Saturator")) {
      auto sat = sonore::host::loadPlugin(*d);
      if (sat && sat->isValid()) {
        const int drive = paramIndex(*sat, "Drive"), tone = paramIndex(*sat, "Tone");
        const int mix = paramIndex(*sat, "Mix"), out = paramIndex(*sat, "Output");
        const std::string t[4] = {drive >= 0 ? sat->parameterText(drive, 2.5) : "?",
                                  tone >= 0 ? sat->parameterText(tone, 6000.0) : "?",
                                  mix >= 0 ? sat->parameterText(mix, 60.0) : "?",
                                  out >= 0 ? sat->parameterText(out, -3.0) : "?"};
        std::printf("       LV2 saturator: \"%s\", \"%s\", \"%s\", \"%s\"\n", t[0].c_str(), t[1].c_str(),
                    t[2].c_str(), t[3].c_str());
        check(t[0] == "2.5 x" && t[1] == "6000 Hz" && t[2] == "60 %" && t[3] == "-3 dB",
              "an LV2 control's text carries its unit, LV2's own (Hz, %, dB) or the plugin's (x)");
      } else {
        check(false, "the LV2 saturator loads");
      }
    }
    const auto* clapShow = byName("CLAP", "Sonore Showcase");
    const auto* lv2Show = byName("LV2", "Sonore Showcase");
    if (clapShow && lv2Show) {
      auto c = sonore::host::loadPlugin(*clapShow);
      auto l = sonore::host::loadPlugin(*lv2Show);
      const int ci = c ? paramIndex(*c, "Shape") : -1, li = l ? paramIndex(*l, "Shape") : -1;
      bool same = ci >= 0 && li >= 0 && l->parameter(li).stepped;
      std::string seen;
      for (int v = 0; same && v < 4; ++v) {
        const std::string a = c->parameterText(ci, (double) v), b = l->parameterText(li, (double) v);
        seen += (v ? ", " : "") + b;
        same = same && a == b;
      }
      std::printf("       LV2 showcase Shape: %s\n", seen.c_str());
      check(same, "…and a named step reads as its name, as the CLAP build calls it, on a stepped control");
    }
  }

  // Somebody else's bundle may name the author on the plugin's lv2:project
  // instead -- the SDK's own did until REAPER showed nobody looks there but
  // lilv. Both are read.
  {
    const std::string bundle = sonoretest::tempPath("third-party.lv2");
    std::filesystem::create_directories(bundle);
    std::FILE* m = std::fopen((bundle + "/manifest.ttl").c_str(), "wb");
    check(m != nullptr, "a hand-written bundle can be written");
    if (m) {
      std::fputs("@prefix doap: <http://usefulinc.com/ns/doap#> .\n"
                 "@prefix foaf: <http://xmlns.com/foaf/0.1/> .\n"
                 "@prefix lv2:  <http://lv2plug.in/ns/lv2core#> .\n"
                 "<urn:example:nested>\n"
                 "    a lv2:Plugin , lv2:DelayPlugin ;\n"
                 "    doap:name \"Nested\" ;\n"
                 "    lv2:binary <nested.dll> ;\n"
                 "    lv2:project [\n"
                 "        a doap:Project ;\n"
                 "        doap:maintainer [ a foaf:Person ; foaf:name \"Project Author\" ] ;\n"
                 "    ] .\n"
                 "<urn:example:direct>\n"
                 "    a lv2:Plugin , doap:Project ;\n"
                 "    doap:name \"Direct\" ;\n"
                 "    lv2:binary <direct.dll> ;\n"
                 "    doap:maintainer [ a foaf:Person ; foaf:name \"Plugin Author\" ] .\n"
                 "<urn:example:nobody>\n"
                 "    a lv2:Plugin ;\n"
                 "    doap:name \"Nobody\" ;\n"
                 "    lv2:binary <nobody.dll> .\n",
                 m);
      std::fclose(m);
    }
    const auto described = sonore::host::describeFile(bundle);
    std::string nested = "?", direct = "?", nobody = "?", directFeatures = "?";
    for (const auto& d : described) {
      if (d.id == "urn:example:nested") nested = d.vendor;
      if (d.id == "urn:example:direct") { direct = d.vendor; directFeatures = d.features; }
      if (d.id == "urn:example:nobody") nobody = d.vendor;
    }
    std::printf("       nested \"%s\", direct \"%s\", nobody \"%s\"\n", nested.c_str(),
                direct.c_str(), nobody.c_str());
    check(nested == "Project Author", "a maintainer on the plugin's lv2:project is the vendor");
    check(direct == "Plugin Author", "…and so is one on the plugin itself");
    check(nobody.empty(), "…and a bundle that names nobody gets no invented vendor");
    check(directFeatures.empty(), "…and doap:Project is not listed as what the plugin is");
  }

  // An LV2 plugin with eight times the ports takes about eight times as long
  // to read, not sixty-four. Each port's every property, and each preset's
  // every port, was found by reading every triple of its document: a plugin
  // of 256 parameters and 16 presets took 180 ms per instance to load, and a
  // session of fifty of them nine seconds. A bundle of 48 ports and one of
  // 384, each with 8 presets setting every port, and no binary, so the load
  // ends once the Turtle is read: the larger took 30-48 times as long as the
  // smaller before, and takes 6-8 times now. Timed against each other, best
  // of three, so a loaded machine slows both.
  {
    auto writeWide = [](const char* leaf, int ports) {
      const std::string dir = sonoretest::tempPath(leaf);
      std::filesystem::create_directories(std::filesystem::u8path(dir));
      std::string manifest = "@prefix lv2: <http://lv2plug.in/ns/lv2core#> .\n"
                             "@prefix pset: <http://lv2plug.in/ns/ext/presets#> .\n"
                             "@prefix rdfs: <http://www.w3.org/2000/01/rdf-schema#> .\n"
                             "<urn:example:wide> a lv2:Plugin ; lv2:binary <absent.dll> ;"
                             " rdfs:seeAlso <plugin.ttl> .\n";
      std::string plugin = "@prefix lv2: <http://lv2plug.in/ns/lv2core#> .\n<urn:example:wide> lv2:port\n";
      std::string presets = "@prefix lv2: <http://lv2plug.in/ns/lv2core#> .\n"
                            "@prefix pset: <http://lv2plug.in/ns/ext/presets#> .\n";
      for (int i = 0; i < ports; ++i)
        plugin += std::string(i ? " ,\n" : "") + "  [ a lv2:InputPort , lv2:ControlPort ; lv2:index " +
                  std::to_string(i) + " ; lv2:symbol \"p" + std::to_string(i) + "\" ; lv2:name \"P " +
                  std::to_string(i) + "\" ; lv2:default 0.5 ; lv2:minimum 0.0 ; lv2:maximum 1.0 ]";
      plugin += " .\n";
      for (int k = 0; k < 8; ++k) {
        const std::string uri = "<urn:example:wide#preset" + std::to_string(k) + ">";
        manifest += uri + " a pset:Preset ; lv2:appliesTo <urn:example:wide> ; rdfs:label \"P" +
                    std::to_string(k) + "\" ; rdfs:seeAlso <presets.ttl> .\n";
        presets += uri + " lv2:port\n";
        for (int i = 0; i < ports; ++i)
          presets += std::string(i ? " ,\n" : "") + "  [ lv2:symbol \"p" + std::to_string(i) + "\" ; pset:value 0.25 ]";
        presets += " .\n";
      }
      const std::pair<const char*, const std::string*> files[] = {
          {"manifest.ttl", &manifest}, {"plugin.ttl", &plugin}, {"presets.ttl", &presets}};
      for (const auto& f : files) std::ofstream(std::filesystem::u8path(dir + "/" + f.first), std::ios::binary) << *f.second;
      return dir;
    };
    // Timed on this thread's own CPU clock, the two sizes in turn, best of
    // five each: by wall time, on a machine busy with other work (a parallel
    // ctest, a CI runner), the larger load was preempted in every run often
    // enough to cross the bound while the smaller one was not.
    auto timeLoad = [](const std::string& bundle) {
      sonore::host::PluginDescription d;
      d.format = "LV2";
      d.id = "urn:example:wide";
      d.path = bundle;
      const double t0 = threadCpuNow();
      const auto loaded = sonore::host::loadPlugin(d); // no binary: null, after the Turtle
      const double spent = threadCpuNow() - t0;
      return loaded ? 1e300 : spent;
    };
    const std::string smallBundle = writeWide("wide48.lv2", 48);
    const std::string largeBundle = writeWide("wide384.lv2", 384);
    double small = 1e300, large = 1e300;
    for (int k = 0; k < 5; ++k) {
      small = (std::min)(small, timeLoad(smallBundle));
      large = (std::min)(large, timeLoad(largeBundle));
    }
    std::printf("       LV2 load on the CPU, 384 ports against 48: %.1fx\n", large / small);
    check(small < 1e300 && large < 16.0 * small,
          "an LV2 bundle's load grows with its size, not with its size squared");
  }

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

  // ── The stream a VST3 writes its state into ───────────────────────────────
  //
  // A plugin may write a placeholder, write its data, seek back and fill the
  // placeholder in. The host's stream appended every write regardless of the
  // cursor, so the size landed at the end and the placeholder stayed: a state
  // the plugin itself could not read back.
  {
    std::vector<uint8_t> bytes;
    sonore::host::MemoryStream stream(&bytes);
    auto* s = reinterpret_cast<Steinberg_IBStream*>(&stream);
    Steinberg_int32 n = 0;
    Steinberg_int64 at = 0;
    char placeholder[4] = {'?', '?', '?', '?'};
    char body[4] = {'d', 'a', 't', 'a'};
    char size[4] = {'0', '0', '0', '4'};
    stream.lpVtbl->write(s, placeholder, 4, &n);
    stream.lpVtbl->write(s, body, 4, &n);
    stream.lpVtbl->seek(s, 0, Steinberg_IBStream_IStreamSeekMode_kIBSeekSet, &at);
    stream.lpVtbl->write(s, size, 4, &n);
    const std::string got(bytes.begin(), bytes.end());
    char line[160];
    std::snprintf(line, sizeof(line), "a VST3 state stream overwrites at its cursor (\"%s\", want "
                  "\"0004data\")", got.c_str());
    check(got == "0004data", line);
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

    // ...including the block that carries a preset's worth of parameter
    // events back from the plugin. Those are collected on the audio thread
    // into the host's edit list, which was never reserved: the first block
    // after a preset load grew it from empty, allocation by allocation, in
    // the one call that promises none.
    for (const char* wantFormat : {"CLAP", "VST3"}) {
      const sonore::host::PluginDescription* pick = nullptr;
      for (const auto& d : found)
        if (d.format == wantFormat && d.name.find("Saturator") != std::string::npos) pick = &d;
      if (!pick) continue;
      auto plugin = sonore::host::loadPlugin(*pick);
      if (!plugin || !plugin->prepare(48000.0, 256, 2) || plugin->numPresets() < 2) continue;
      std::vector<float> l(256, 0.0f), r(256, 0.0f);
      float* ch[2] = {l.data(), r.data()};
      { sonore::AudioBlock<float> io(ch, 2, 256); plugin->process(io, nullptr); }
      std::vector<sonore::host::ParamEdit> edits;
      plugin->drainParameterEdits(edits);
      edits.clear();
      plugin->loadPreset(plugin->numPresets() - 1);
      g_allocs.store(0);
      g_armed.store(true);
      { sonore::AudioBlock<float> io(ch, 2, 256); plugin->process(io, nullptr); }
      g_armed.store(false);
      const long carried = g_allocs.load();
      const size_t reported = plugin->drainParameterEdits(edits);
      char line[200];
      std::snprintf(line, sizeof(line), "%s: the block after a preset load allocates nothing "
                    "while collecting the plugin's edits (%ld allocations, %zu edits)",
                    wantFormat, carried, reported);
      check(carried == 0, line);
    }

    // A parameter set from the UI thread while the audio thread processes --
    // the way any host with a window uses this. The queue was a plain vector
    // that process() cleared at the END of the block, so whatever arrived
    // while the plugin was inside its process call was thrown away
    // undelivered, and that is most of the time on a busy audio thread. It
    // was also pushed and cleared from two threads with no synchronisation.
    {
      const sonore::host::PluginDescription* pick = nullptr;
      for (const auto& d : found)
        if (d.format == "CLAP" && d.name.find("Saturator") != std::string::npos) pick = &d;
      auto plugin = pick ? sonore::host::loadPlugin(*pick) : nullptr;
      if (plugin && plugin->prepare(48000.0, 256, 2) && plugin->numParameters() > 0) {
        const sonore::host::HostedParam p = plugin->parameter(0);
        std::atomic<bool> stop{false};
        std::atomic<unsigned long> blocks{0};
        std::thread audio([&] {
          std::vector<float> l(256, 0.0f), r(256, 0.0f);
          float* ch[2] = {l.data(), r.data()};
          while (!stop.load()) {
            sonore::AudioBlock<float> io(ch, 2, 256);
            plugin->process(io, nullptr);
            blocks.fetch_add(1);
          }
        });
        int lost = 0;
        const int trials = 100;
        for (int t = 0; t < trials; ++t) {
          const double v = p.minValue + (p.maxValue - p.minValue) * (double) ((t * 37) % 97 + 1) / 98.0;
          while (!plugin->setParameterValue(0, v)) std::this_thread::yield();
          const unsigned long from = blocks.load();
          while (blocks.load() < from + 3) std::this_thread::yield();
          if (std::fabs(plugin->parameterValue(0) - v) > 1e-4) ++lost;
        }
        stop.store(true);
        audio.join();
        char line[200];
        std::snprintf(line, sizeof(line), "CLAP: a parameter set from another thread while audio runs "
                      "always arrives (%d of %d lost)", lost, trials);
        check(lost == 0, line);
      }
    }

    // The musical clock a hosted VST3 is told. One second at 44.1 kHz and the
    // fixed 120 BPM is two beats; the context said 48 kHz and left the beat
    // position at zero while flagging it valid, so a tempo-synced plugin saw
    // its song frozen on the first beat. Read back from the GUI probe's own
    // state, where it records the position the host gave its last block.
    for (const auto& d : found) {
      if (d.format != "VST3" || d.name.find("GUI Probe") == std::string::npos) continue;
      auto probe = sonore::host::loadPlugin(d);
      if (!probe || !probe->prepare(44100.0, 441, 2)) break;
      std::vector<float> l(441, 0.0f), r(441, 0.0f);
      float* ch[2] = {l.data(), r.data()};
      for (int block = 0; block <= 100; ++block) { // the last block starts at 1.000 s
        sonore::AudioBlock<float> io(ch, 2, 441);
        probe->process(io, nullptr);
      }
      std::vector<uint8_t> state;
      probe->saveState(state);
      const std::string text(state.begin(), state.end());
      const size_t at = text.find("beat ");
      const std::string said = at == std::string::npos ? "nothing" : text.substr(at, 10);
      char line[200];
      std::snprintf(line, sizeof(line), "a hosted VST3 is told the music moves: after one second "
                    "at 44.1 kHz it hears \"%s\", want \"beat 2.000\"", said.c_str());
      check(said == "beat 2.000", line);
      break;
    }
  }

  // ── A CLAP plugin that asks for the main thread gets it ───────────────────
  //
  // request_callback was ignored, so on_main_thread never ran: the SDK's own
  // plugins ask for it to say their state changed, and that news never
  // reached the host. And latency.changed() during activate() -- where CLAP
  // allows it, and where the SDK's plugins announce their latency -- was
  // taken for a restart request, so every prepare asked for another.
  for (const auto& d : found) {
    if (d.format != "CLAP" || d.name.find("GUI Probe") == std::string::npos) continue;
    auto probe = sonore::host::loadPlugin(d);
    if (!probe || !probe->prepare(48000.0, 256, 2)) break;
    const bool restartAfterPrepare = probe->takeRestartRequest();
    // The probe's switch stands in for "the user loaded a sample": moving it
    // changes what the plugin would save.
    for (int i = 0; i < probe->numParameters(); ++i)
      if (std::string(probe->parameter(i).name).find("Latency") != std::string::npos ||
          std::string(probe->parameter(i).name).find("Switch") != std::string::npos)
        probe->setParameterValue(i, probe->parameter(i).maxValue);
    std::vector<float> l(256, 0.0f), r(256, 0.0f);
    float* ch[2] = {l.data(), r.data()};
    for (int block = 0; block < 4; ++block) {
      sonore::AudioBlock<float> io(ch, 2, 256);
      probe->process(io, nullptr);
    }
    std::vector<sonore::host::ParamEdit> edits;
    probe->drainParameterEdits(edits);
    const bool dirty = probe->takeStateDirty();
    char line[200];
    std::snprintf(line, sizeof(line),
                  "a CLAP plugin's state change reaches the host through on_main_thread (%s), and "
                  "announcing its latency in activate asks for no restart (%s)",
                  dirty ? "dirty" : "not told", restartAfterPrepare ? "restart asked" : "none");
    check(dirty && !restartAfterPrepare, line);
    break;
  }

  // ── An LV2 plugin's state survives the order a host restores in ─────────
  //
  // A session restores state and THEN prepares, and LV2 fixes the rate at
  // instantiate, so prepare() makes a new instance: loadState() before it
  // restored into nothing (and still returned true), and a later prepare
  // wiped what the instance held. The ducker keeps its key source in its
  // state, so a changed one has to come back through both.
  for (const auto& d : found) {
    if (d.format != "LV2" || d.name.find("Ducker") == std::string::npos) continue;
    auto source = sonore::host::loadPlugin(d);
    std::vector<uint8_t> blob;
    if (!source || !source->prepare(48000.0, 256, 2) || !source->saveState(blob)) break;
    const std::string from = "sidechain", to = "sidechaiN";
    auto at = std::search(blob.begin(), blob.end(), from.begin(), from.end());
    if (at == blob.end()) {
      check(false, "the ducker's LV2 state names its key source");
      break;
    }
    std::copy(to.begin(), to.end(), at);
    auto holds = [&](auto& plugin) {
      std::vector<uint8_t> now;
      return plugin.saveState(now) && std::search(now.begin(), now.end(), to.begin(), to.end()) != now.end();
    };
    auto fresh = sonore::host::loadPlugin(d);
    const bool restored = fresh && fresh->loadState(blob.data(), blob.size());
    const bool held = restored && holds(*fresh);
    const bool prepared = held && fresh->prepare(48000.0, 256, 2) && holds(*fresh);
    const bool reprepared = prepared && fresh->prepare(48000.0, 512, 2) && holds(*fresh);
    char line[220];
    std::snprintf(line, sizeof(line),
                  "an LV2 plugin's state, loaded before prepare, is still there after it (%s) and "
                  "after a second prepare (%s)", prepared ? "yes" : "no", reprepared ? "yes" : "no");
    check(held && prepared && reprepared, line);
    break;
  }

  // ── An LV2 plugin's aux outputs stay off the main bus ─────────────────────
  //
  // This host routes the main channels only, and connected every audio port
  // past them to the FIRST main channel: a splitter's aux bands were written
  // over the main left channel, and a key input heard the main signal. A
  // 12 kHz tone belongs entirely to the splitter's high band, so the main
  // output -- the low band -- must stay near silent on BOTH sides.
  for (const auto& d : found) {
    if (d.format != "LV2" || d.name.find("Splitter") == std::string::npos) continue;
    auto splitter = sonore::host::loadPlugin(d);
    if (!splitter || !splitter->prepare(48000.0, 256, 2)) break;
    std::vector<float> l(256), r(256);
    float* ch[2] = {l.data(), r.data()};
    double inEnergy = 0.0, leftEnergy = 0.0, rightEnergy = 0.0;
    int phase = 0;
    for (int block = 0; block < 60; ++block) {
      for (int i = 0; i < 256; ++i, ++phase)
        l[(size_t) i] = r[(size_t) i] = 0.5f * (float) std::sin(2.0 * 3.14159265358979 * 12000.0 * phase / 48000.0);
      if (block >= 20)
        for (int i = 0; i < 256; ++i) inEnergy += (double) l[(size_t) i] * l[(size_t) i];
      sonore::AudioBlock<float> io(ch, 2, 256);
      splitter->process(io, nullptr);
      if (block >= 20)
        for (int i = 0; i < 256; ++i) {
          leftEnergy += (double) l[(size_t) i] * l[(size_t) i];
          rightEnergy += (double) r[(size_t) i] * r[(size_t) i];
        }
    }
    char line[220];
    std::snprintf(line, sizeof(line), "an LV2 splitter's high band stays off the main bus (main "
                  "left %.1f dB, right %.1f dB, relative to the input)",
                  10.0 * std::log10(leftEnergy / inEnergy + 1e-30),
                  10.0 * std::log10(rightEnergy / inEnergy + 1e-30));
    check(leftEnergy < inEnergy * 0.01 && rightEnergy < inEnergy * 0.01, line);
    break;
  }

  // ── An arpeggiator's notes come back, in every format ────────────────────
  //
  // The LV2 build has a MIDI output and a UI notification output, both atom
  // ports, and this host connected every atom output to ONE buffer. The
  // notification port, claimed after the MIDI was written, reset the
  // sequence: the CLAP and VST3 arps handed on 43 notes in 2000 blocks and
  // the LV2 one none at all.
  for (const char* wantFormat : {"CLAP", "VST3", "LV2"}) {
    const sonore::host::PluginDescription* d = nullptr;
    for (const auto& candidate : found)
      if (candidate.format == wantFormat && candidate.name == "Sonore Arp") d = &candidate;
    if (!d) continue;
    auto arp = sonore::host::loadPlugin(*d);
    if (!arp || !arp->prepare(48000.0, 64, 2)) continue;
    std::vector<float> l(64, 0.0f), r(64, 0.0f);
    float* ch[2] = {l.data(), r.data()};
    sonore::MidiBuffer held;
    held.addEvent(sonore::MidiMessage::noteOn(0, 60, 100), 0);
    int notes = 0;
    for (int block = 0; block < 2000; ++block) {
      sonore::AudioBlock<float> io(ch, 2, 64);
      arp->process(io, block == 0 ? &held : nullptr);
      notes += arp->producedMidi().getNumEvents();
    }
    char line[160];
    std::snprintf(line, sizeof(line), "%s arpeggiator's notes come back through producedMidi() "
                  "(%d in 2000 blocks)", wantFormat, notes);
    check(notes > 0, line);
    arp->release();
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
  // kParamValuesChanged; this host had no handler at all, so a hosted
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

      // The caller's window dragged larger: the editor is asked to follow,
      // each format its own way -- CLAP's set_size(), VST3's onSize(),
      // LV2's ui:resize -- and the window the OS measures is the new size.
      {
        const uint32_t wantW = width + 40, wantH = height + 30;
        const bool took = plugin->setEditorSize(wantW, wantH);
        for (int i = 0; i < 20; ++i) {
          MSG pending;
          while (PeekMessageW(&pending, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&pending);
            DispatchMessageW(&pending);
          }
          plugin->idleEditor();
          Sleep(5);
        }
        uint32_t nowW = 0, nowH = 0;
        plugin->editorSize(nowW, nowH);
        RECT client{};
        if (child) GetClientRect(child, &client);
        char resized[200];
        std::snprintf(resized, sizeof(resized), "%s: the caller resizes the editor to %ux%u (%s; it reports "
                      "%ux%u, its window is %ldx%ld)", wantFormat, (unsigned) wantW, (unsigned) wantH,
                      took ? "taken" : "refused", (unsigned) nowW, (unsigned) nowH, client.right - client.left,
                      client.bottom - client.top);
        check(took && nowW == wantW && nowH == wantH && (uint32_t) (client.right - client.left) == wantW &&
                  (uint32_t) (client.bottom - client.top) == wantH,
              resized);
      }

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
    // faster": that is a stopwatch and a mood. Every file is UP TO DATE, so
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

    // ── …and does not rewrite the cache ──
    //
    // Every write of the cache is a flush to the disk, some 10 ms, and a scan
    // that found nothing changed paid one at its end to put back the bytes
    // that were there: more than everything else the scan of an unchanged
    // folder -- every start of a host -- costs. A file left alone keeps its
    // modification time; one written again does not.
    {
      std::error_code error;
      const auto writtenAt = std::filesystem::last_write_time(std::filesystem::u8path(kCachePath), error);
      sonore::host::PluginCache third;
      third.load(kCachePath);
      const size_t thirdFound = sonore::host::scanDirectoryCached(directory, third, kCachePath).size();
      const auto after = std::filesystem::last_write_time(std::filesystem::u8path(kCachePath), error);
      check(!error && after == writtenAt && thirdFound == firstPass.size(),
            "a scan that finds nothing changed leaves the cache file as it was");
    }

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

    // A BUNDLE -- every .lv2 -- is a folder, and on Windows a folder's own
    // stamp is a size of 0 and a time that moves when a file is added or
    // removed in it, not when one is rewritten. An update that wrote its
    // manifest and binary over the old ones in place left the stamp as it
    // was: the cache went on answering for the old bundle, its new plugins
    // never listed. macOS and Linux stamped a bundle by its files already.
    {
      namespace fs = std::filesystem;
      const fs::path bundle = fs::u8path(tempPath("sonore-cache-bundle.lv2"));
      std::error_code error;
      fs::create_directories(bundle, error);
      auto write = [&](const char* leaf, const char* text) {
        if (std::FILE* file = std::fopen((bundle / leaf).u8string().c_str(), "wb")) {
          std::fputs(text, file);
          std::fclose(file);
        }
      };
      write("manifest.ttl", "# one plugin\n");
      write("plugin.dll", "version 1\n");
      sonore::host::PluginCache bundles;
      bundles.remember(bundle.u8string(), {});
      const bool knownAtFirst = bundles.isUpToDate(bundle.u8string());
      write("manifest.ttl", "# one plugin, and now a second one beside it\n");
      write("plugin.dll", "version 2, with the second plugin in it\n");
      check(knownAtFirst && !bundles.isUpToDate(bundle.u8string()),
            "an LV2 bundle whose files were rewritten in place is scanned again");
      fs::remove_all(bundle, error);
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

    // A mark left by ANOTHER process that is still running is a scan in
    // progress, not a crash. A second host process started mid-scan read the
    // first one's mark as a death and blacklisted that plugin for good. The
    // mark now carries the writer's process id; a live writer is left alone.
    {
      const std::string victim = files[0];
#if defined(_WIN32)
      const unsigned long alive = 4; // System: always running, and not ours
#else
      const unsigned long alive = 1; // init: the same
#endif
      const unsigned long dead = 0x7FFFFFF0ul; // no such process
      auto writeMark = [&](unsigned long pid) {
        if (std::FILE* file = std::fopen(kCachePath, "wb")) {
          std::fprintf(file, "SONORE-PLUGIN-CACHE-1\nSCANNING\t%s\t%lu\n", victim.c_str(), pid);
          std::fclose(file);
        }
      };
      writeMark(alive);
      sonore::host::PluginCache whileScanning;
      check(whileScanning.load(kCachePath) && whileScanning.crashedOn().empty(),
            "a scan mark from a process that is still running is not read as a crash");
      writeMark(dead);
      sonore::host::PluginCache afterDeath;
      check(afterDeath.load(kCachePath) && afterDeath.crashedOn() == victim,
            "while one from a process that is gone still is");

      // Two processes scanning one cache, as two hosts started together do.
      // Each writes the whole file from its own memory, before and after
      // every plugin it opens -- so the second one's next write erased the
      // first one's mark. A plugin that killed the first process while the
      // second went on scanning left no trace, and killed it again on the
      // next start. Another process's mark is carried over now, until it is
      // acted on.
      auto fileText = [&]() {
        std::string text;
        if (std::FILE* file = std::fopen(kCachePath, "rb")) {
          char buffer[4096];
          size_t got = 0;
          while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0) text.append(buffer, got);
          std::fclose(file);
        }
        return text;
      };
      writeMark(alive);
      sonore::host::PluginCache beside;
      beside.load(kCachePath);
      beside.endScanning(kCachePath); // this process, done with a file of its own
      char mark[600];
      std::snprintf(mark, sizeof(mark), "SCANNING\t%s\t%lu", victim.c_str(), alive);
      check(fileText().find(mark) != std::string::npos,
            "a scan in a second process keeps the first one's mark while that one is scanning");

      sonore::host::PluginCache survivor;
      if (std::FILE* file = std::fopen(kCachePath, "wb")) {
        std::fprintf(file, "SONORE-PLUGIN-CACHE-1\n");
        std::fclose(file);
      }
      survivor.load(kCachePath); // before the other process reached the plugin
      writeMark(dead);           // which it then died inside
      survivor.endScanning(kCachePath);
      sonore::host::PluginCache nextStart;
      check(nextStart.load(kCachePath) && nextStart.crashedOn() == victim,
            "...and a plugin that killed one process while another scanned is found on the next start");
      sonore::host::scanFilesCached({}, nextStart, kCachePath);
      sonore::host::PluginCache acted;
      check(acted.load(kCachePath) && acted.crashedOn().empty() && acted.isBlacklisted(victim),
            "...blacklisted, and its mark gone once it has been acted on");
    }

    // A byte-order mark, which an editor may add when somebody edits the
    // blacklist by hand -- as the format invites. It was read as part of the
    // version line, and the whole cache, blacklist included, was thrown away.
    {
      if (std::FILE* file = std::fopen(kCachePath, "wb")) {
        std::fprintf(file, "\xEF\xBB\xBFSONORE-PLUGIN-CACHE-1\nBLACKLIST\t%s\n", files[0].c_str());
        std::fclose(file);
      }
      sonore::host::PluginCache edited;
      check(edited.load(kCachePath) && edited.isBlacklisted(files[0]),
            "a cache saved with a UTF-8 byte-order mark still loads, blacklist and all");
    }

    // Two stamps for one file -- a cache written twice over, or edited --
    // with the file's real stamp first and a stale one after it. isUpToDate
    // read the first and knownFor the last: the file was "up to date" and
    // known to hold nothing, so a cached scan hid its plugins for good. The
    // last line wins now, for every lookup, and the file is rescanned.
    {
      // The scan's whole cache again (the case above overwrote the file),
      // so the file's real stamp comes first.
      reloaded.save(kCachePath);
      std::string text;
      if (std::FILE* file = std::fopen(kCachePath, "rb")) {
        char buffer[4096];
        size_t got = 0;
        while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0) text.append(buffer, got);
        std::fclose(file);
      }
      std::string withPlugins;
      for (const auto& d : found)
        if (d.format == "CLAP") { withPlugins = d.path; break; }
      if (!withPlugins.empty()) {
        // A stamp no file has: 1 byte, modified at the epoch.
        text += "STAMP\t" + withPlugins + "\t1\t1\n";
        if (std::FILE* file = std::fopen(kCachePath, "wb")) {
          std::fwrite(text.data(), 1, text.size(), file);
          std::fclose(file);
        }
        sonore::host::PluginCache doubled;
        const bool doubledLoads = doubled.load(kCachePath);
        const bool current = doubled.isUpToDate(withPlugins);
        const bool known = !doubled.knownFor(withPlugins).empty();
        check(doubledLoads && current == known && !current,
              "a file with two stamps is judged by the last, the same way by every lookup, and rescanned");
      }
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
        check(v4.size() > 17 && v4[4] == 6, "…declaring version 6");

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
  // wrote and read it. This checks the bytes are the format other hosts
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
  // beyond its knobs (which sample a sampler loaded, which impulse a reverb
  // read) travels through the state: extension as typed key/value pairs, and
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

  // ── What the host holds for one plugin ────────────────────────────────────
  //
  // The bytes the host keeps for a plugin loaded, prepared at 48 kHz for 512
  // frames and run once: its own structures, none of the plugin's (see
  // g_liveBytes). Each bound sits between what is held now and what was
  // held when a structure outgrew its use:
  //   * a VST3 instrument's parameter list kept the room it was reserved for
  //     the 2080 MIDI controller parameters the instrument hides (515 KB
  //     held, 302 KB now);
  //   * an LV2 plugin with an editor had the 192 KB of queues between it and
  //     its interface from load, its editor opened or not (563 KB, 371 KB);
  //   * a VST3 effect kept room for a block of notes it has no input for
  //     (224 KB, 176 KB);
  //   * and so did a CLAP effect (187 KB, 131 KB).
#if defined(_WIN32)
  {
    struct Held {
      const char* format;
      const char* name;
      long long boundKb;
    };
    const Held held[] = {{"VST3", "Sonore Synth", 400},
                         {"LV2", "Sonore Synth", 470},
                         {"VST3", "Sonore Saturator", 205},
                         {"CLAP", "Sonore Saturator", 160}};
    for (const Held& want : held) {
      const sonore::host::PluginDescription* pick = nullptr;
      for (const auto& d : found)
        if (d.format == want.format && d.name == want.name) pick = &d;
      if (!pick) continue;
      std::vector<float> l(512, 0.0f), r(512, 0.0f);
      float* ch[2] = {l.data(), r.data()};
      const long long before = g_liveBytes.load();
      auto plugin = sonore::host::loadPlugin(*pick);
      const bool ready = plugin && plugin->prepare(48000.0, 512, 2);
      if (ready) {
        sonore::AudioBlock<float> io(ch, 2, 512);
        plugin->process(io, nullptr);
      }
      const long long bytes = g_liveBytes.load() - before;
      char line[200];
      std::snprintf(line, sizeof(line), "%s %s: the host holds %lld KB for it, loaded and prepared "
                    "(at most %lld KB)", want.format, want.name, bytes / 1024, want.boundKb);
      check(ready && bytes > 0 && bytes <= want.boundKb * 1024, line);
    }
  }
#endif

  std::printf("\n%d checks, %d failure(s)\n", g_checks, g_failures);
  if (g_failures == 0) std::printf("SONORE HOST PASSED\n");
  return g_failures == 0 ? 0 : 1;
}
