// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: the editor, opened and closed until a leak would show.
//
// Every editor open creates a WebView2 environment, a controller, a webview
// and a script-message handler, all of them COM objects with a reference
// count, and the first version of the bridge leaked every one of them on
// every open: the API takes its own reference and the code kept ours too.
// That was found by READING. This is the measurement: the guiprobe's editor
// is opened into a real window, pumped until it has really been created,
// closed, and the process's handle count and private bytes are read after
// each cycle. Growth over the warm-up is allowed -- WebView2 caches, the CRT
// grows its heap -- and growth that keeps going is a leak.
//
//   editor_soak_test <plugin.clap> [cycles]        (SONORE_SOAK_CYCLES also)
//
// Windows only: the other backends have no display on the machines that run
// this, and a test that cannot open a window has nothing to measure.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>

#if defined(_WIN32)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#endif

#include <sonore/host.h>

static int g_checks = 0, g_failures = 0;
static void check(bool ok, const char* what) {
  ++g_checks;
  if (!ok) ++g_failures;
  std::printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
}

#if defined(_WIN32)
static void pump(int ms) {
  const ULONGLONG until = GetTickCount64() + (ULONGLONG) ms;
  while (GetTickCount64() < until) {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    Sleep(5);
  }
}
static DWORD handles() {
  DWORD n = 0;
  GetProcessHandleCount(GetCurrentProcess(), &n);
  return n;
}
static size_t privateBytes() {
  PROCESS_MEMORY_COUNTERS_EX pmc{};
  pmc.cb = sizeof(pmc);
  GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*) &pmc, sizeof(pmc));
  return (size_t) pmc.PrivateUsage;
}
#endif

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::printf("── the editor, opened and closed until a leak would show ──────────\n");
#if !defined(_WIN32)
  (void) argc;
  (void) argv;
  std::printf("  SKIPPED: window embedding is measured on Windows; this platform has no "
              "display in the harness\n");
  return 0;
#else
  if (argc < 2) {
    std::printf("usage: editor_soak_test <plugin.clap> [cycles]\n");
    return 2;
  }
  int cycles = 12;
  if (argc > 2) cycles = std::atoi(argv[2]);
  if (const char* env = std::getenv("SONORE_SOAK_CYCLES")) cycles = std::atoi(env);
  if (cycles < 8) cycles = 8;

  const std::vector<sonore::host::PluginDescription> found =
      sonore::host::describeFile(argv[1]);
  check(!found.empty(), "the plugin file describes at least one plugin");
  if (found.empty()) return 1;
  std::unique_ptr<sonore::host::HostedPlugin> plugin = sonore::host::loadPlugin(found[0]);
  check(plugin && plugin->isValid(), "it loads");
  if (!plugin || !plugin->isValid()) return 1;
  check(plugin->prepare(48000.0, 512), "it prepares");
  check(plugin->hasEditor(), "it has an editor");
  if (!plugin->hasEditor()) return 1;

  HWND parent = CreateWindowExW(0, L"STATIC", L"sonore editor soak", WS_OVERLAPPEDWINDOW, 0, 0,
                                800, 600, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
  check(parent != nullptr, "a host window exists to embed into");
  if (!parent) return 1;
  ShowWindow(parent, SW_SHOWNOACTIVATE);

  // A cycle counts only once the editor has REALLY come up. WebView2 creates
  // itself through COM callbacks after set_parent returns, so an open followed
  // by a close a few hundred milliseconds later can measure a webview that
  // never existed. The guiprobe's page drives parameter 1 to 0.777 the moment
  // it loads; that arriving through the bridge is the proof the whole chain
  // was built, and only then is it torn down.
  std::vector<float> l(512, 0.0f), r(512, 0.0f);
  float* chans[2] = {l.data(), r.data()};
  auto processOne = [&]() {
    sonore::AudioBlock<float> io(chans, 2, 512);
    plugin->process(io);
  };
  const int warm = cycles / 4 > 3 ? cycles / 4 : 3;
  DWORD handlesAtWarm = 0;
  size_t bytesAtWarm = 0;
  int opened = 0, created = 0;
  for (int c = 0; c < cycles; ++c) {
    plugin->setParameterValue(1, 0.0);
    processOne();
    const bool ok = plugin->openEditor(parent);
    if (ok) ++opened;
    bool up = false;
    const ULONGLONG deadline = GetTickCount64() + 6000;
    while (GetTickCount64() < deadline) {
      pump(20);
      processOne(); // the page's edit crosses on the audio thread
      if (std::fabs(plugin->parameterValue(1) - 0.777) < 1e-3) {
        up = true;
        break;
      }
    }
    if (up) ++created;
    plugin->closeEditor();
    pump(120); // ...and let it go away
    const DWORD h = handles();
    const size_t b = privateBytes();
    std::printf("  cycle %2d: %s, %s  handles %5lu  private %7.1f MB\n", c + 1,
                ok ? "opened" : "REFUSED", up ? "page up" : "PAGE NEVER CAME UP",
                (unsigned long) h, (double) b / (1024.0 * 1024.0));
    if (c + 1 == warm) {
      handlesAtWarm = h;
      bytesAtWarm = b;
    }
  }
  const DWORD handlesEnd = handles();
  const size_t bytesEnd = privateBytes();
  const int measured = cycles - warm;
  const double handlesPerCycle = ((double) handlesEnd - (double) handlesAtWarm) / measured;
  const double bytesPerCycle = ((double) bytesEnd - (double) bytesAtWarm) / measured;
  std::printf("  ---- after warm-up: %+.2f handles and %+.1f KB per open/close cycle over %d "
              "cycles ----\n", handlesPerCycle, bytesPerCycle / 1024.0, measured);
  check(opened == cycles, "every open succeeded");
  check(created == cycles, "...and every editor really came up (the page drove a parameter) "
                           "before it was closed");
  // A leaked editor is several handles (the environment, the controller, the
  // webview, its message handler, a window) and hundreds of KB, every cycle.
  check(handlesPerCycle < 2.0, "handle count does not keep growing (under 2 per cycle)");
  check(bytesPerCycle < 256.0 * 1024.0, "private bytes do not keep growing (under 256 KB per cycle)");

  plugin.reset();
  DestroyWindow(parent);
  std::printf("\n%d checks, %d failure(s)\n", g_checks, g_failures);
  if (g_failures == 0) std::printf("SONORE EDITOR SOAK PASSED\n");
  return g_failures == 0 ? 0 : 1;
#endif
}
