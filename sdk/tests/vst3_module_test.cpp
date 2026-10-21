// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: sonore/host.h pairs a VST3 module's exit with its entry.
//
// The fixture counts its module entries and exits and refuses the entry on
// request. Whatever the host does -- scan it, try to load it -- every exit
// must answer an entry that succeeded: none after a refusal, one after each
// acceptance.

#include <cstdio>
#include <cstdlib>
#include <string>

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

static int counter(const char* name) {
  char text[32] = {0};
#if defined(_WIN32)
  GetEnvironmentVariableA(name, text, (DWORD) sizeof(text));
#else
  if (const char* v = std::getenv(name)) std::snprintf(text, sizeof(text), "%s", v);
#endif
  return std::atoi(text);
}

static void set(const char* name, const char* value) {
#if defined(_WIN32)
  SetEnvironmentVariableA(name, value);
#else
  setenv(name, value, 1);
#endif
}

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  if (argc < 2) {
    std::printf("usage: vst3_module_test <SonoreModuleFixture.vst3>\n");
    return 2;
  }
  std::printf("── VST3 module entry and exit: %s ──\n", argv[1]);
  sonore::host::PluginDescription asLoaded;
  asLoaded.path = argv[1];
  asLoaded.format = "VST3";
  asLoaded.id = "00000000000000000000000000000000";
  char line[200];

  for (const char* refuse : {"0", "1"}) {
    const bool refusing = refuse[0] == '1';
    set("SONORE_MODULE_FIXTURE_REFUSE", refuse);
    set("SONORE_MODULE_FIXTURE_ENTRIES", "0");
    set("SONORE_MODULE_FIXTURE_EXITS", "0");
    (void) sonore::host::describeFile(argv[1]);
    (void) sonore::host::loadPlugin(asLoaded); // no factory: it loads nothing
    const int entries = counter("SONORE_MODULE_FIXTURE_ENTRIES");
    const int exits = counter("SONORE_MODULE_FIXTURE_EXITS");
    std::snprintf(line, sizeof(line), "%s: scanned and loaded, %d entr%s and %d exit%s, want %d",
                  refusing ? "entry refused" : "entry accepted", entries, entries == 1 ? "y" : "ies",
                  exits, exits == 1 ? "" : "s", refusing ? 0 : entries);
    check(entries == 2 && exits == (refusing ? 0 : 2), line);
  }
#if !defined(_WIN32) && !defined(__APPLE__)
  check(counter("SONORE_MODULE_FIXTURE_NULL_HANDLE") == 0,
        "ModuleEntry is given the library's own handle, not null");
#endif

  std::printf("\n%d checks, %d failure(s)\n", g_checks, g_failures);
  if (g_failures == 0) std::printf("SONORE VST3 MODULE PASSED\n");
  return g_failures == 0 ? 0 : 1;
}
