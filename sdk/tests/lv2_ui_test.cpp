// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: the LV2 interface, driven the way a host drives one.
//
// LV2 hands the interface a parent widget and a function to call when a
// control moves, and then never speaks to it again except through
// port_event(). So this is the whole contract, and every part of it is
// exercised against the BUILT bundle:
//
//   * the manifest declares a ui:ui the host can find
//   * lv2ui_descriptor() is exported and its URI matches what the TTL says
//   * instantiate() refuses a host that skips the ui:parent it demanded
//   * …and embeds into a real window when given one
//   * the page loads, runs, and writes a parameter back through the host's
//     write_function, which is the whole point and the part that no amount
//     of interface-shape checking can stand in for
//   * port_event() reaches the page, and does NOT echo back what the page
//     itself just said
//
// The probe fixture is the plugin used, for the same reason it is used
// everywhere else: its page talks the moment it loads, so a bridge that is
// broken anywhere along the chain shows up as a parameter that never moves.

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

static int g_checks = 0;
static int g_failures = 0;

static void check(bool ok, const char* what) {
  ++g_checks;
  if (!ok) ++g_failures;
  std::printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
}

#if !defined(_WIN32)
int main() {
  std::printf("lv2_ui_test: no window system here; skipped\n");
  return 0;
}
#else

// ── The ABI, as a host sees it ───────────────────────────────────────────────
using Lv2UiWidget = void*;
using Lv2UiHandle = void*;
using Lv2UiController = void*;
using Lv2UiWriteFunction = void (*)(Lv2UiController, uint32_t, uint32_t, uint32_t, const void*);

struct Lv2Feature {
  const char* URI;
  void* data;
};
struct Lv2UiDescriptor {
  const char* URI;
  Lv2UiHandle (*instantiate)(const Lv2UiDescriptor*, const char*, const char*,
                             Lv2UiWriteFunction, Lv2UiController, Lv2UiWidget*,
                             const Lv2Feature* const*);
  void (*cleanup)(Lv2UiHandle);
  void (*port_event)(Lv2UiHandle, uint32_t, uint32_t, uint32_t, const void*);
  const void* (*extension_data)(const char*);
};
struct Lv2UiIdleInterface {
  int (*idle)(Lv2UiHandle);
};

// What the "host" was told, by the interface.
struct Written {
  uint32_t port = 0;
  float value = 0.0f;
};
static std::vector<Written> g_written;

// The atom ABI, spelled out rather than included, for the same reason the UI
// descriptor above is: this file is a HOST. Reaching into the SDK's headers
// for the layout would make it agree with whatever those headers say, which
// is the one thing a host test must not do.
using LV2_URID = uint32_t;
using LV2_URID_Map_Handle = void*;
struct LV2_URID_Map {
  LV2_URID_Map_Handle handle;
  LV2_URID (*map)(LV2_URID_Map_Handle, const char*);
};
struct TestAtom {
  uint32_t size;
  uint32_t type;
};
struct TestAtomEvent {
  int64_t frames;
  TestAtom body;
};
struct TestAtomSequenceBody {
  uint32_t unit;
  uint32_t pad;
};
struct TestAtomSequence {
  TestAtom atom;
  TestAtomSequenceBody body;
};

/** Atom writes the interface made, counted by port. A request for state
 *  arrives here rather than as a float, and dropping it silently is how the
 *  first version of this test proved nothing. */
static int g_atomWrites = 0;
static uint32_t g_atomPort = 0xffffffffu;
/** The type and size of the last atom the interface wrote. eventTransfer
 *  carries one bare atom; a Sequence here is the shape no host turns into
 *  the event the plugin looks for. */
static uint32_t g_atomType = 0, g_atomSize = 0, g_atomBody = 0;

static void writeFunction(Lv2UiController, uint32_t port, uint32_t size, uint32_t format,
                          const void* buffer) {
  if (!buffer) return;
  if (format != 0) {
    ++g_atomWrites;
    g_atomPort = port;
    if (size >= sizeof(TestAtom)) {
      g_atomType = ((const TestAtom*) buffer)->type;
      g_atomBody = ((const TestAtom*) buffer)->size;
      g_atomSize = size;
    }
    return;
  }
  if (size != sizeof(float)) return;
  g_written.push_back({port, *(const float*) buffer});
}

/** The smallest urid:map that works: a growing table of strings. A real host
 *  keeps one per session; nothing here needs more than that they are stable
 *  and unique. */
static std::vector<std::string> g_urids;
static LV2_URID mapUri(LV2_URID_Map_Handle, const char* uri) {
  for (size_t i = 0; i < g_urids.size(); ++i)
    if (g_urids[i] == uri) return (LV2_URID) (i + 1);
  g_urids.push_back(uri);
  return (LV2_URID) g_urids.size();
}

/**
 * The index of a port, looked up by SYMBOL in the bundle's ttl.
 *
 * Which is how a host finds one. Hardcoding a number here would pass until
 * somebody added a port before it and then point at whatever had moved into
 * its place -- and the failure would look like an interface that stopped
 * receiving messages.
 */
static int portIndexBySymbol(const std::string& ttl, const char* symbol) {
  const std::string needle = std::string("lv2:symbol \"") + symbol + "\"";
  const size_t at = ttl.find(needle);
  if (at == std::string::npos) return -1;
  const size_t indexAt = ttl.rfind("lv2:index", at);
  if (indexAt == std::string::npos) return -1;
  return std::atoi(ttl.c_str() + indexAt + 9);
}

static std::string readFile(const std::string& path) {
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return "";
  std::string text;
  char buffer[4096];
  size_t got = 0;
  while ((got = std::fread(buffer, 1, sizeof(buffer), f)) > 0) text.append(buffer, got);
  std::fclose(f);
  return text;
}

static void pump(int milliseconds, Lv2UiHandle ui, const Lv2UiIdleInterface* idle) {
  const DWORD until = GetTickCount() + (DWORD) milliseconds;
  MSG msg;
  while (GetTickCount() < until) {
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    if (idle && idle->idle) idle->idle(ui);
    Sleep(5);
  }
}

int main(int argc, char** argv) {
  if (argc < 2) {
    std::printf("usage: lv2_ui_test <bundle.lv2>\n");
    return 2;
  }
  const std::string bundle = argv[1];
  std::printf("── LV2 interface: %s ─────────────────────────\n", bundle.c_str());

  // ── What the bundle promises ──────────────────────────────────────────────
  const std::string manifest = readFile(bundle + "/manifest.ttl");
  check(!manifest.empty(), "the bundle has a manifest");
  check(manifest.find("ui:ui") != std::string::npos,
        "…which points the host at an interface");
  check(manifest.find("ui:WindowsUI") != std::string::npos,
        "…of the type this platform can actually open");
  check(manifest.find("ui:idleInterface") != std::string::npos,
        "…and asks the host to idle it, without which nothing it does is heard");

  // ── What the binary delivers ──────────────────────────────────────────────
  std::string dll = bundle;
  const size_t slash = dll.find_last_of("/\\");
  const std::string leaf = slash == std::string::npos ? dll : dll.substr(slash + 1);
  std::string base = leaf;
  if (base.size() > 4 && base.compare(base.size() - 4, 4, ".lv2") == 0)
    base = base.substr(0, base.size() - 4);
  dll = bundle + "/" + base + ".dll";

  HMODULE lib = LoadLibraryA(dll.c_str());
  check(lib != nullptr, "the bundle's binary loads");
  if (!lib) {
    std::printf("\n%d checks, %d failure(s)\n", g_checks, g_failures + 1);
    return 1;
  }

  auto entry = (const Lv2UiDescriptor* (*) (uint32_t)) GetProcAddress(lib, "lv2ui_descriptor");
  check(entry != nullptr, "…and exports lv2ui_descriptor");
  if (!entry) {
    std::printf("\n%d checks, %d failure(s)\n", g_checks, g_failures + 1);
    return 1;
  }

  const Lv2UiDescriptor* d = entry(0);
  check(d != nullptr && d->URI != nullptr, "the descriptor exists and names itself");
  check(entry(1) == nullptr, "…and there is exactly one of them");
  if (!d) {
    std::printf("\n%d checks, %d failure(s)\n", g_checks, g_failures + 1);
    return 1;
  }
  // The URI in the binary and the URI in the metadata have to be the same
  // string, or a host looks up an interface that does not answer.
  check(manifest.find(d->URI) != std::string::npos,
        "…by the same URI the manifest advertises");

  // ── A host that ignores what the bundle asked for ─────────────────────────
  {
    Lv2UiWidget widget = (Lv2UiWidget) 1; // deliberately not null, to see it cleared
    const Lv2Feature* const none[] = {nullptr};
    Lv2UiHandle ui = d->instantiate(d, "urn:test", bundle.c_str(), writeFunction, nullptr,
                                    &widget, none);
    check(ui == nullptr, "a host that skips the REQUIRED ui:parent is refused");
    check(widget == nullptr, "…and gets no widget rather than a stale pointer");
  }

  // ── A host that behaves ───────────────────────────────────────────────────
  HWND parent = CreateWindowExW(0, L"STATIC", L"sonore lv2 ui test", WS_OVERLAPPEDWINDOW, 0, 0,
                                760, 520, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
  check(parent != nullptr, "the test host created a parent window");
  if (!parent) {
    std::printf("\n%d checks, %d failure(s)\n", g_checks, g_failures + 1);
    return 1;
  }

  Lv2Feature parentFeature{"http://lv2plug.in/ns/extensions/ui#parent", (void*) parent};
  Lv2Feature idleFeature{"http://lv2plug.in/ns/extensions/ui#idleInterface", nullptr};
  LV2_URID_Map uridMap{nullptr, mapUri};
  Lv2Feature mapFeature{"http://lv2plug.in/ns/ext/urid#map", &uridMap};
  const Lv2Feature* features[] = {&parentFeature, &idleFeature, &mapFeature, nullptr};

  Lv2UiWidget widget = nullptr;
  Lv2UiHandle ui = d->instantiate(d, "urn:test", bundle.c_str(), writeFunction, nullptr, &widget,
                                  features);
  check(ui != nullptr, "the interface instantiates");
  check(widget != nullptr, "…and hands back a widget");
  if (widget) {
    check(IsWindow((HWND) widget) != 0, "…which is a real window");
    check(GetParent((HWND) widget) == parent, "…embedded in the one the host provided");
  }

  const auto* idle = (const Lv2UiIdleInterface*) d->extension_data(
      "http://lv2plug.in/ns/extensions/ui#idleInterface");
  check(idle != nullptr && idle->idle != nullptr,
        "the idle interface it advertised is really there");

  if (ui) {
    // ── The chain that matters ──────────────────────────────────────────────
    // Page loaded -> bridge injected -> script ran -> message posted -> queue
    // -> idle() -> write_function. Every link has to work or nothing arrives.
    g_written.clear();
    ShowWindow(parent, SW_HIDE);
    pump(4000, ui, idle);
    // Four seconds is what the page takes on a quiet machine, and a fixed
    // window failed on a busy one: under a parallel ASan run the page had not
    // loaded yet, and the chain was reported broken when it was only slow.
    // So the four seconds stay (what is checked below also happens in them)
    // and a page that has not spoken yet is given until thirty.
    for (int waited = 4000; g_written.empty() && waited < 30000; waited += 250) pump(250, ui, idle);

    check(!g_written.empty(),
          "the page drove a parameter back through the host's write_function");
    if (!g_written.empty()) {
      const Written& w = g_written.back();
      std::printf("  ---- the page wrote port %u = %.4f ----\n", (unsigned) w.port,
                  (double) w.value);
      check(w.port < 64, "…on a port index a control could actually occupy");
    }

    // \u2500\u2500 The atom protocol, both directions \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500
    //
    // An LV2 interface is a separate module: it shares no memory with the
    // plugin and reaches it only through ports. So it ASKS what the plugin
    // has, over an atom input port, and the plugin answers on an atom output
    // one. An interface opened after the plugin has missed every change and
    // cannot wait to be told; one already open has to be told when something
    // moves. Neither direction covers the other.
    //
    // The test host plays the plugin's part here. It has already seen the
    // request by now -- the interface sends it from idle().
    std::printf("  ---- atom writes from the interface: %d, on port %u ----\n",
                g_atomWrites, (unsigned) g_atomPort);
    check(g_atomWrites > 0, "the interface asks the plugin what it has");
    // One bare atom, the shape eventTransfer carries -- a Sequence was never
    // read as a request -- and with a body: REAPER drops an empty one.
    check(g_atomType == mapUri(nullptr, "urn:sonorie:ui:stateRequest") && g_atomBody > 0 &&
              g_atomSize == sizeof(TestAtom) + g_atomBody,
          "\u2026with one bare atom that has a body, the shape every host delivers");

    // ...and the answer, written the way the plugin writes it.
    {
      g_written.clear();
      const LV2_URID uridSequence = mapUri(nullptr, "http://lv2plug.in/ns/ext/atom#Sequence");
      const LV2_URID uridJson = mapUri(nullptr, "urn:sonorie:ui:stateJson");
      const LV2_URID uridTransfer =
          mapUri(nullptr, "http://lv2plug.in/ns/ext/atom#eventTransfer");
      const int notifyPort = portIndexBySymbol(readFile(bundle + "/plugin.ttl"), "uiNotify");
      check(notifyPort >= 0, "the bundle declares a notify port to answer on");
      const std::string json = "{'host':'Sonore LV2 Test Host'}";

      alignas(8) uint8_t buffer[256] = {};
      auto* seq = (TestAtomSequence*) buffer;
      seq->atom.type = uridSequence;
      seq->body.unit = 0;
      seq->body.pad = 0;
      auto* ev = (TestAtomEvent*) ((uint8_t*) &seq->body + sizeof(TestAtomSequenceBody));
      ev->frames = 0;
      ev->body.size = (uint32_t) json.size();
      ev->body.type = uridJson;
      std::memcpy((uint8_t*) ev + sizeof(TestAtomEvent), json.data(), json.size());
      seq->atom.size = (uint32_t) (sizeof(TestAtomSequenceBody) + sizeof(TestAtomEvent) +
                                   ((json.size() + 7u) & ~7u));

      // One bare atom -- the state JSON as its own event's body -- which is
      // how jalv, Carla, Ardour and REAPER deliver eventTransfer. The page
      // read only whole Sequences and never had the state.
      alignas(8) uint8_t bare[256] = {};
      auto* bareAtom = (TestAtom*) bare;
      bareAtom->size = (uint32_t) json.size();
      bareAtom->type = uridJson;
      std::memcpy(bare + sizeof(TestAtom), json.data(), json.size());
      if (notifyPort >= 0)
        d->port_event(ui, (uint32_t) notifyPort, (uint32_t) (sizeof(TestAtom) + json.size()),
                      uridTransfer, bareAtom);
      (void) seq; // the Sequence form is the second page's, below

      // The probe's page moves parameter 0 to 1.25 when a state object
      // arrives with a host name in it -- and only then. Seeing that value
      // come back through write_function is the whole path: plugin ->
      // port_event -> page -> bridge -> queue -> idle -> write_function.
      // Until the page answers, up to fifteen seconds: three were not enough
      // for a page on a machine busy compiling.
      bool sawGain = false;
      for (int slice = 0; slice < 150 && !sawGain; ++slice) {
        pump(100, ui, idle);
        for (const Written& w : g_written)
          if (w.port == 0 && std::fabs(w.value - 1.25f) < 1e-3f) sawGain = true;
      }
      std::printf("  ---- writes after the answer: %zu ----\n", g_written.size());
      check(sawGain, "…and the answer reaches the page, which acts on it");

      // The plugin sends its state again every second of audio, asked or
      // not (a host may not carry the interface's request). The page acts on
      // every state it is handed -- the probe's sets Gain each time -- so the
      // interface hands it a state only when it differs from the last one.
      auto gainWrites = [] {
        int n = 0;
        for (const Written& w : g_written)
          if (w.port == 0 && std::fabs(w.value - 1.25f) < 1e-3f) ++n;
        return n;
      };
      const int before = gainWrites();
      if (notifyPort >= 0)
        d->port_event(ui, (uint32_t) notifyPort, (uint32_t) (sizeof(TestAtom) + json.size()),
                      uridTransfer, bareAtom);
      pump(1500, ui, idle);
      const int afterRepeat = gainWrites();
      const std::string moved = "{'host':'Sonore LV2 Test Host','preset':'B'}";
      alignas(8) uint8_t bare2[256] = {};
      auto* movedAtom = (TestAtom*) bare2;
      movedAtom->size = (uint32_t) moved.size();
      movedAtom->type = uridJson;
      std::memcpy(bare2 + sizeof(TestAtom), moved.data(), moved.size());
      if (notifyPort >= 0)
        d->port_event(ui, (uint32_t) notifyPort, (uint32_t) (sizeof(TestAtom) + moved.size()),
                      uridTransfer, movedAtom);
      int afterMoved = afterRepeat;
      for (int slice = 0; slice < 150 && afterMoved == afterRepeat; ++slice) {
        pump(100, ui, idle);
        afterMoved = gainWrites();
      }
      char repeatLine[200];
      std::snprintf(repeatLine, sizeof(repeatLine),
                    "the same state again is not handed to the page, a changed one is (Gain set "
                    "%d time(s), then %d, then %d)", before, afterRepeat, afterMoved);
      check(afterRepeat == before && afterMoved == before + 1, repeatLine);
    }

    // ── port_event, and the echo it must not fight ──────────────────────────
    if (idle && idle->idle) {
      const size_t before = g_written.size();
      // The host echoing back exactly what the page just said. A UI that
      // pushed this into the page would move the control under the user's
      // mouse, and a UI that then wrote it BACK would loop with the host
      // forever.
      if (!g_written.empty()) {
        const Written& w = g_written.back();
        d->port_event(ui, w.port, sizeof(float), 0, &w.value);
      }
      pump(300, ui, idle);
      check(g_written.size() == before,
            "an echo of the page's own value does not come back out again");

      // And a value the page has NOT seen must reach it without complaint.
      const float fresh = 0.4321f;
      d->port_event(ui, 0, sizeof(float), 0, &fresh);
      // Nonsense a careless host might send: wrong size, wrong format, a port
      // that is not a control. None of it may crash.
      const double wrongSize = 1.0;
      d->port_event(ui, 0, sizeof(double), 0, &wrongSize);
      d->port_event(ui, 0, sizeof(float), 7, &fresh);
      d->port_event(ui, 9999, sizeof(float), 0, &fresh);
      d->port_event(ui, 0, sizeof(float), 0, nullptr);
      pump(300, ui, idle);
      check(true, "malformed port events are survived rather than trusted");
    }

    d->cleanup(ui);
    check(true, "the interface cleans up");
  }

  // ── A host that answers at once ────────────────────────────────────
  //
  // A real host sends every control's value right after instantiate(), and a
  // plugin answers the first state request within a run() or two -- both long
  // before WebView2 has started. Both were evaluated into the blank document
  // the navigation then replaced: the page drew defaults and an empty state
  // until the user touched something. Here the test host does exactly that,
  // and the probe page answers each one it receives: Gain 0.4321 moves the
  // probe parameter to 0.5, and a state object moves Gain to 1.25.
  if (idle && idle->idle) {
    g_written.clear();
    Lv2UiWidget early = nullptr;
    Lv2UiHandle second = d->instantiate(d, "urn:test", bundle.c_str(), writeFunction, nullptr,
                                        &early, features);
    check(second != nullptr, "a second interface instantiates");
    if (second) {
      const float gain = 0.4321f;
      d->port_event(second, 0, sizeof(float), 0, &gain);
      idle->idle(second); // its first state request...
      const LV2_URID uridSequence = mapUri(nullptr, "http://lv2plug.in/ns/ext/atom#Sequence");
      const LV2_URID uridJson = mapUri(nullptr, "urn:sonorie:ui:stateJson");
      const LV2_URID uridTransfer = mapUri(nullptr, "http://lv2plug.in/ns/ext/atom#eventTransfer");
      const int notifyPort = portIndexBySymbol(readFile(bundle + "/plugin.ttl"), "uiNotify");
      const std::string json = "{'host':'Sonore LV2 Test Host'}";
      alignas(8) uint8_t buffer[256] = {};
      auto* seq = (TestAtomSequence*) buffer;
      seq->atom.type = uridSequence;
      auto* ev = (TestAtomEvent*) ((uint8_t*) &seq->body + sizeof(TestAtomSequenceBody));
      ev->body.size = (uint32_t) json.size();
      ev->body.type = uridJson;
      std::memcpy((uint8_t*) ev + sizeof(TestAtomEvent), json.data(), json.size());
      seq->atom.size = (uint32_t) (sizeof(TestAtomSequenceBody) + sizeof(TestAtomEvent) +
                                   ((json.size() + 7u) & ~7u));
      if (notifyPort >= 0) // ...answered before the page exists
        d->port_event(second, (uint32_t) notifyPort,
                      (uint32_t) (sizeof(TestAtom) + seq->atom.size), uridTransfer, seq);
      // The same allowance as the first page: five seconds, then until
      // thirty for a page that has not answered yet. A fixed five failed
      // five runs in a row on a machine just out of a build, and passed in
      // 250 ms to 3.3 s on the same machine idle -- slow, not broken.
      bool sawValue = false, sawState = false;
      auto seen = [&] {
        for (const Written& w : g_written) {
          if (w.port == 1 && std::fabs(w.value - 0.5f) < 1e-3f) sawValue = true;
          if (w.port == 0 && std::fabs(w.value - 1.25f) < 1e-3f) sawState = true;
        }
        return sawValue && sawState;
      };
      pump(5000, second, idle);
      for (int waited = 5000; !seen() && waited < 30000; waited += 250) pump(250, second, idle);
      check(sawValue, "a control value the host sent before the page existed reaches it");
      check(sawState, "…and so does a state answer that arrived before it");
      d->cleanup(second);
    }
  }

  // ── The size it opens at ───────────────────────────────────────────
  //
  // The plugin's own -- the probe declares none, so the SDK's 700x420, inside
  // its 517x341..1200x800 limits, which is what CLAP and VST3 open it at --
  // times the host's ui:scaleFactor, because the webview draws each CSS pixel
  // as that many device pixels. It was a fixed 720x480 device pixels for
  // every plugin and every display: in REAPER at 125% the showcase opened at
  // 780x500 as CLAP and VST3, and at 576x384 as LV2.
  if (idle && idle->idle) {
    struct Resize {
      void* handle;
      int (*ui_resize)(void*, int, int);
    };
    static int askedW = 0, askedH = 0;
    Resize resize{nullptr, [](void*, int w, int h) {
                    askedW = w;
                    askedH = h;
                    return 0;
                  }};
    Lv2Feature resizeFeature{"http://lv2plug.in/ns/extensions/ui#resize", &resize};
    // LV2_Options_Option, spelled out like the rest of the ABI here.
    struct Option {
      uint32_t context, subject;
      LV2_URID key;
      uint32_t size;
      LV2_URID type;
      const void* value;
    };
    const float two = 2.0f;
    const Option options[] = {
        {0, 0, mapUri(nullptr, "http://lv2plug.in/ns/extensions/ui#scaleFactor"), sizeof(float),
         mapUri(nullptr, "http://lv2plug.in/ns/ext/atom#Float"), &two},
        {0, 0, 0, 0, 0, nullptr}};
    Lv2Feature optionsFeature{"http://lv2plug.in/ns/ext/options#options", (void*) options};
    for (int scale = 1; scale <= 2; ++scale) {
      const Lv2Feature* sized[] = {&parentFeature, &idleFeature, &mapFeature, &resizeFeature,
                                   scale == 2 ? &optionsFeature : nullptr, nullptr};
      askedW = askedH = 0;
      Lv2UiWidget w = nullptr;
      Lv2UiHandle h = d->instantiate(d, "urn:test", bundle.c_str(), writeFunction, nullptr, &w,
                                     sized);
      RECT r{};
      if (w) GetClientRect((HWND) w, &r);
      char line[200];
      std::snprintf(line, sizeof(line),
                    "%s: asks the host for %dx%d and is %ldx%ld (want %dx%d)",
                    scale == 2 ? "at ui:scaleFactor 2" : "with no scale given", askedW, askedH,
                    (long) r.right, (long) r.bottom, 700 * scale, 420 * scale);
      check(h != nullptr && askedW == 700 * scale && askedH == 420 * scale &&
                r.right == 700 * scale && r.bottom == 420 * scale,
            line);
      if (h) {
        pump(500, h, idle);
        d->cleanup(h);
      }
    }
  }

  DestroyWindow(parent);
  FreeLibrary(lib);

  std::printf("\n%d checks, %d failure(s)\n", g_checks, g_failures);
  if (g_failures == 0) std::printf("SONORE LV2 UI PASSED\n");
  return g_failures == 0 ? 0 : 1;
}
#endif
