// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: a VST3 plugin loaded by sonore/host.h is destroyed when it is
// unloaded.
//
// VST3 objects are reference counted, and queryInterface hands out a
// reference the caller owes back. host.h took two it never returned: the
// IEditController of a plugin whose component is also its controller, and the
// controller's IMidiMapping. So an SDK plugin -- both, always -- was never
// destroyed: its DSP's destructor never ran, and ExitDll and FreeLibrary then
// unloaded the code beneath an object still alive, along with anything its
// destructor would have stopped.
//
// The fixture counts its live DSPs in the process environment, which a plugin
// and its host share. Loaded, it must read 1; unloaded, 0.

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <sonore/host.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

static int g_checks = 0;
static int g_failures = 0;

static void check(bool ok, const char* what) {
  ++g_checks;
  if (!ok) ++g_failures;
  std::printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
}

static int liveFixtures() {
  char text[32] = {0};
#if defined(_WIN32)
  GetEnvironmentVariableA("SONORE_LIFETIME_FIXTURE_LIVE", text, (DWORD) sizeof(text));
#else
  if (const char* v = std::getenv("SONORE_LIFETIME_FIXTURE_LIVE"))
    std::snprintf(text, sizeof(text), "%s", v);
#endif
  return std::atoi(text);
}

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  if (argc < 2) {
    std::printf("usage: vst3_lifetime_test <SonoreLifetimeFixture.vst3>\n");
    return 2;
  }
  std::printf("── VST3 lifetime: %s ──\n", argv[1]);

  const auto found = sonore::host::describeFile(argv[1]);
  const sonore::host::PluginDescription* fixture = nullptr;
  for (const auto& d : found)
    if (d.format == "VST3") fixture = &d;
  check(fixture != nullptr, "the fixture is described as a VST3 plugin");
  if (!fixture) return 1;

  char line[200];
  for (int round = 1; round <= 3; ++round) {
    {
      auto plugin = sonore::host::loadPlugin(*fixture);
      check(plugin != nullptr, "the fixture loads");
      if (!plugin) return 1;
      std::snprintf(line, sizeof(line), "round %d: loaded, %d live DSP(s), want 1", round,
                    liveFixtures());
      check(liveFixtures() == 1, line);
      if (round == 1) {
        // The fixture's one parameter is called "Größe". A VST3 name crosses as
        // UTF-16; narrowed one unit at a time it came out as Latin-1 bytes,
        // which is not UTF-8 and is not the name.
        const std::string name = plugin->numParameters() > 0 ? plugin->parameter(0).name : "";
        std::snprintf(line, sizeof(line), "a parameter named outside ASCII arrives as UTF-8 "
                      "(%zu bytes, want 7)", name.size());
        check(name == "Gr\xC3\xB6\xC3\x9F" "e", line);
      }

      // Used, not just loaded: prepared, a note through the MIDI path, a block.
      check(plugin->prepare(48000.0, 256, 2), "…prepares");
      std::vector<float> l(256, 0.0f), r(256, 0.0f);
      float* channels[2] = {l.data(), r.data()};
      sonore::AudioBlock<float> io(channels, 2, 256);
      sonore::MidiBuffer midi;
      midi.addEvent(sonore::MidiMessage::noteOn(1, 60, 100), 0);
      plugin->process(io, &midi);
      plugin->release();
    }
    std::snprintf(line, sizeof(line), "round %d: unloaded, %d live DSP(s), want 0 -- the host "
                  "returned every reference it took", round, liveFixtures());
    check(liveFixtures() == 0, line);
    if (liveFixtures() != 0) break; // the count only grows from here
  }

  std::printf("\n%d checks, %d failure(s)\n", g_checks, g_failures);
  if (g_failures == 0) std::printf("SONORE VST3 LIFETIME PASSED\n");
  return g_failures == 0 ? 0 : 1;
}
