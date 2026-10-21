// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: a licensed build's web editor shows its activation overlay.
//
// A marketplace build that is not activated on this machine has one way to
// take a code: the overlay its page shows. The editor's tick evaluated the
// overlay into the webview before the page's document existed, marked it
// shown, and never tried again -- so the overlay never appeared, on any open.
//
// The trim example, built here as a licensed product with a key no licence
// can match, so it is never activated. Its page waits three seconds after it
// is ready and reports on the Phase switch whether the overlay ran (1) or not
// (0), then moves Gain to -30 to say it has reported. The editor is opened
// twice: the second open is a fresh page in the same instance.

#define SONORE_LICENSE_PRODUCT "sonore-licence-overlay-test"
#define SONORE_LICENSE_PUBKEY "MCowBQYDK2VwAyEAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA="
#define SONORE_UI_HTML                                                                      \
  "<!doctype html><html><body style='background:#123'><div id=x>page</div><script>"        \
  "window.addEventListener('sonore:ready',function(){setTimeout(function(){"               \
  "sonore.set('phase',window.__sonoreLicense?1:0);sonore.set('gain',-30);},3000);});"      \
  "</script></body></html>"
#include "../examples/trim/plugin.cpp"

#include <cstdio>
#include <cstring>

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool ok, const char* what) {
  ++g_checks;
  if (!ok) ++g_failures;
  std::printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
}

const void* hostGetExtension(const clap_host_t*, const char*) { return nullptr; }
void hostRequest(const clap_host_t*) {}
const clap_host_t kHost = {CLAP_VERSION_INIT, nullptr, "licence overlay test", "sonore", "", "1.0",
                           hostGetExtension,  hostRequest, hostRequest, hostRequest};

void pump(int ms) {
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

/** One editor open: the Phase the page reported, or -1 if it never did. */
double openOnce(const clap_plugin_t* plugin, const clap_plugin_gui_t* gui,
                const clap_plugin_params_t* params) {
  HWND parent = CreateWindowExW(0, L"STATIC", L"licence overlay test", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                0, 0, 760, 520, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
  sonore::clapwrap::self(plugin)->params[0] = 0.0f;
  sonore::clapwrap::self(plugin)->params[1] = 0.5f;
  gui->create(plugin, CLAP_WINDOW_API_WIN32, false);
  clap_window_t w{};
  w.api = CLAP_WINDOW_API_WIN32;
  w.win32 = parent;
  gui->set_parent(plugin, &w);
  gui->show(plugin);
  double gain = 0.0, phase = -1.0;
  // A minute: a WebView2 cold start on a machine busy compiling is slow, and
  // this measures whether the overlay comes, not how soon.
  const ULONGLONG deadline = GetTickCount64() + 60000;
  while (GetTickCount64() < deadline) {
    pump(50);
    params->get_value(plugin, 0, &gain);
    if (gain < -29.0) break;
  }
  if (gain < -29.0) params->get_value(plugin, 1, &phase);
  gui->hide(plugin);
  gui->destroy(plugin);
  DestroyWindow(parent);
  pump(200);
  return phase;
}

} // namespace

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::printf("── a licensed build's activation overlay ──\n");
  clap_entry.init("");
  const auto* factory =
      static_cast<const clap_plugin_factory_t*>(clap_entry.get_factory(CLAP_PLUGIN_FACTORY_ID));
  const clap_plugin_descriptor_t* desc = factory->get_plugin_descriptor(factory, 0);
  const clap_plugin_t* plugin = factory->create_plugin(factory, &kHost, desc->id);
  plugin->init(plugin);
  const auto* gui = static_cast<const clap_plugin_gui_t*>(plugin->get_extension(plugin, CLAP_EXT_GUI));
  const auto* params =
      static_cast<const clap_plugin_params_t*>(plugin->get_extension(plugin, CLAP_EXT_PARAMS));
  check(!sonore::clapwrap::licenseGate().licensed(), "the product is not activated on this machine");
  char line[160];
  const double first = openOnce(plugin, gui, params);
  std::snprintf(line, sizeof(line), "the first open shows the activation overlay (page reported %g)", first);
  check(first == 1.0, line);
  const double second = openOnce(plugin, gui, params);
  std::snprintf(line, sizeof(line), "...and so does the second, a fresh page (page reported %g)", second);
  check(second == 1.0, line);
  plugin->destroy(plugin);
  clap_entry.deinit();
  std::printf("\n%d checks, %d failure(s)\n", g_checks, g_failures);
  std::printf(g_failures ? "LICENCE OVERLAY TEST FAILED\n" : "LICENCE OVERLAY TEST PASSED\n");
  return g_failures ? 1 : 0;
}
