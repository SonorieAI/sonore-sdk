// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: the CLAP wrapper, driven through its own C API with its
// Instance in reach.
//
// The trim example is compiled into this test, as vst3_feeds_test does with
// the VST3 wrapper, so what activate() left behind can be read directly.
//
// activate() prepares the DSP through prepareDsp(), as every other format
// does: that is where the load meter learns the session's rate, held notes
// are let go, and a licensed build's gate is told the rate. CLAP called the
// DSP's prepare() itself and skipped the rest, so after an activation at
// 96 kHz the editor's CPU meter judged blocks against a 48 kHz deadline.
//
// And a size the editor asked the host for, which the host accepted, is the
// size the editor then has -- whether or not the host also calls set_size().
#include "../examples/trim/plugin.cpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>

static int g_checks = 0;
static int g_failures = 0;

static void check(bool ok, const char* what) {
  ++g_checks;
  if (!ok) ++g_failures;
  std::printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
}

namespace {
const void* hostGetExtension(const clap_host_t*, const char*) { return nullptr; }
void hostRequest(const clap_host_t*) {}
const clap_host_t kHost = {CLAP_VERSION_INIT, nullptr, "com.sonorie.clap-wrapper-test", "Sonorie",
                           "",                "1.0",   hostGetExtension,              hostRequest,
                           hostRequest,       hostRequest};

// A host that takes every size the plugin asks for, and -- as clap.gui allows
// -- does not call set_size() after saying yes.
int g_resizeRequests = 0;
uint32_t g_requestedWidth = 0, g_requestedHeight = 0;
bool hostRequestResize(const clap_host_t*, uint32_t width, uint32_t height) {
  ++g_resizeRequests;
  g_requestedWidth = width;
  g_requestedHeight = height;
  return true;
}
bool hostGuiNo(const clap_host_t*) { return false; }
void hostGuiHints(const clap_host_t*) {}
void hostGuiClosed(const clap_host_t*, bool) {}
const clap_host_gui_t kHostGui = {hostGuiHints, hostRequestResize, hostGuiNo, hostGuiNo, hostGuiClosed};
const void* guiHostGetExtension(const clap_host_t*, const char* id) {
  return std::strcmp(id, CLAP_EXT_GUI) == 0 ? &kHostGui : nullptr;
}
const clap_host_t kGuiHost = {CLAP_VERSION_INIT, nullptr, "com.sonorie.clap-wrapper-test.gui", "Sonorie",
                              "",                "1.0",   guiHostGetExtension,                 hostRequest,
                              hostRequest,       hostRequest};
} // namespace

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::printf("── the CLAP wrapper, from the inside ──\n");
  check(sonore::clapwrap::pluginDescriptorProblem() == nullptr,
        "the trim example's descriptor is well-formed, as the wrapper checks it");
  check(clap_entry.init(""), "the entry initialises");
  const auto* factory =
      static_cast<const clap_plugin_factory_t*>(clap_entry.get_factory(CLAP_PLUGIN_FACTORY_ID));
  const clap_plugin_descriptor_t* desc = factory ? factory->get_plugin_descriptor(factory, 0) : nullptr;
  const clap_plugin_t* plugin = desc ? factory->create_plugin(factory, &kHost, desc->id) : nullptr;
  check(plugin != nullptr && plugin->init(plugin), "the trim example instantiates");
  if (!plugin) return 1;

  check(plugin->activate(plugin, 96000.0, 1, 256), "and activates at 96 kHz");
  sonore::clapwrap::Instance* inst = sonore::clapwrap::self(plugin);
  // A block that takes exactly its own deadline at 96 kHz is a load of 1.
  inst->load.record(256.0 / 96000.0, 256);
  char line[200];
  std::snprintf(line, sizeof(line), "the load meter's deadline is the 96 kHz one activate() was "
                "given (a block of exactly 256/96000 s reads %.2f, want 1.00)",
                (double) inst->load.peakLoad());
  check(std::fabs(inst->load.peakLoad() - 1.0f) < 0.01f, line);

  // An input buffer with NO channels. The copy into the work buffer took
  // channel min(c, count - 1), which for a count of zero is channel
  // 4294967295 -- a read far past the host's array. Nothing to copy is
  // silence.
  {
    plugin->start_processing(plugin);
    float inL[64], inR[64], outL[64], outR[64];
    for (int i = 0; i < 64; ++i) inL[i] = inR[i] = outL[i] = outR[i] = 0.5f;
    float* ins[2] = {inL, inR};
    float* outs[2] = {outL, outR};
    clap_audio_buffer_t inBuf{}, outBuf{};
    inBuf.data32 = ins;
    inBuf.channel_count = 0;
    outBuf.data32 = outs;
    outBuf.channel_count = 2;
    clap_process_t p{};
    p.frames_count = 64;
    p.steady_time = -1;
    p.audio_inputs = &inBuf;
    p.audio_inputs_count = 1;
    p.audio_outputs = &outBuf;
    p.audio_outputs_count = 1;
    static const clap_input_events_t noEvents = {
        nullptr, [](const clap_input_events_t*) -> uint32_t { return 0; },
        [](const clap_input_events_t*, uint32_t) -> const clap_event_header_t* { return nullptr; }};
    static const clap_output_events_t sink = {
        nullptr, [](const clap_output_events_t*, const clap_event_header_t*) { return true; }};
    p.in_events = &noEvents;
    p.out_events = &sink;
    plugin->process(plugin, &p);
    float peak = 0.0f;
    for (int i = 0; i < 64; ++i) peak = std::fmax(peak, std::fabs(outL[i]));
    std::snprintf(line, sizeof(line), "an input with no channels is processed as silence "
                  "(output peak %.3f)", (double) peak);
    check(peak == 0.0f, line);
    plugin->stop_processing(plugin);
  }

  // A page that comes up -- a reopened editor, one rebuilt after a park --
  // has seen nothing, and says so with 'ready'. Everything the tick pushes
  // only on a change has to be pushed again then, the held keys included:
  // they were not, so a page opened over a held chord drew its keyboard
  // with every key up until a note changed. Shown in a real WebView2 with
  // the synth before this was written; here it is the tick that is held to
  // it, which needs no window.
  {
    const std::string first = sonore::clapwrap::uiTickScript(*inst);
    const std::string quiet = sonore::clapwrap::uiTickScript(*inst);
    sonore::clapwrap::pageReady(inst);
    const std::string afterReady = sonore::clapwrap::uiTickScript(*inst);
    check(first.find("__notes(") != std::string::npos && quiet.find("__notes(") == std::string::npos,
          "the tick tells a page the held keys once, and again only when they change");
    check(afterReady.find("__notes(") != std::string::npos,
          "...and tells a page that has just come up, which has seen none");
  }

  // The meter's ballistics run on the time that passed, not on the time a
  // tick was meant to take. Every editor advanced them by 33 ms a tick, and
  // a Win32 timer asked for 33 ms fires every 47 (the system's 15.6 ms
  // tick): the peak fell at 70% of its speed, a second after a transient
  // reading -10 dB where the 0.6 s decay puts it at -14.5. Here a quarter of
  // a second passes between two ticks.
  {
    inst->meter.push(1.0f, 0.0f);
    sonore::clapwrap::uiTickScript(*inst); // the peak lands at once
    inst->meter.push(0.0f, 0.0f);
    const auto before = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    sonore::clapwrap::uiTickScript(*inst);
    const double waited =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - before).count();
    // Between the two ticks at least the quarter second passed, and at most
    // what was measured around them: the reading has to lie between the two
    // decays, wherever a busy machine put the ticks.
    const double atMost = std::exp(-0.25 / 0.6), atLeast = std::exp(-waited / 0.6);
    std::snprintf(line, sizeof(line),
                  "a peak falls by the time that passed (%.3f s: reads %.3f, the 0.6 s decay says "
                  "%.3f to %.3f)",
                  waited, (double) inst->meter.level(), atLeast, atMost);
    check(inst->meter.level() <= atMost + 0.005 && inst->meter.level() >= atLeast - 0.005, line);
  }

  plugin->deactivate(plugin);
  plugin->destroy(plugin);

  // The editor's border asks the host for a size, and the host says yes.
  // "The host doesn't have to call set_size()" after that, clap.gui says, and
  // the wrapper waited for set_size anyway: the frame took the new size while
  // the plugin kept drawing -- and reporting, and saving -- the old one.
  {
    const clap_plugin_t* sized = factory->create_plugin(factory, &kGuiHost, desc->id);
    check(sized != nullptr && sized->init(sized), "a second instance, in a host that resizes on request");
    const auto* gui =
        sized ? static_cast<const clap_plugin_gui_t*>(sized->get_extension(sized, CLAP_EXT_GUI)) : nullptr;
    const bool anyEditor = sonore::clapwrap::editorChoice().backend != sonore::EditorBackend::None;
    if (!anyEditor) {
      std::printf("  ---- no editor in this build: the resize request is not reachable ----\n");
    } else if (gui && gui->create(sized, sonore::clapwrap::nativeWindowApi(), false)) {
      uint32_t w = 0, h = 0;
      gui->get_size(sized, &w, &h);
      // What the border does at the end of a drag.
      sonore::clapwrap::requestHostResize(sonore::clapwrap::self(sized), (int) w + 200, (int) h + 100);
      uint32_t after = 0, afterH = 0;
      gui->get_size(sized, &after, &afterH);
      std::snprintf(line, sizeof(line),
                    "a size the host accepted is the editor's size (asked %ux%u, get_size %ux%u, was %ux%u)",
                    g_requestedWidth, g_requestedHeight, after, afterH, w, h);
      check(g_resizeRequests == 1 && after == g_requestedWidth && afterH == g_requestedHeight &&
                after == w + 200 && afterH == h + 100,
            line);
      gui->destroy(sized);
    } else {
      check(false, "...and its editor can be created");
    }
    if (sized) sized->destroy(sized);
  }

  // What every instance of every plugin holds before its DSP: the trim's DSP
  // is one smoother, so this is the wrapper's own Instance. Its two
  // MidiBuffers were 37 KB of the 66 while a MidiMessage was three ints; a
  // bound with room to spare, so it fails on a regrowth and not on a field.
  // 60 KB since the host's notes joined it -- their names in and out
  // (NoteIdBuffer), their modulation (ParamModBuffer) and its totals, about
  // 5 KB in all -- with the same room left for the next field.
  {
    char sized[160];
    std::snprintf(sized, sizeof(sized), "the wrapper's Instance is %zu bytes (MidiBuffer %zu), under 60 KB",
                  sizeof(sonore::clapwrap::Instance), sizeof(sonore::MidiBuffer));
    check(sizeof(sonore::clapwrap::Instance) < 60 * 1024 && sizeof(sonore::MidiBuffer::Entry) == 8,
          sized);
  }
  clap_entry.deinit();
  std::printf("\n%d checks, %d failure(s)\n", g_checks, g_failures);
  if (g_failures == 0) std::printf("SONORE CLAP WRAPPER PASSED\n");
  return g_failures == 0 ? 0 : 1;
}
