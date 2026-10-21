// SPDX-License-Identifier: Apache-2.0
// The standalone's audio path allocates nothing -- on a fresh audio thread.
//
// Every device switch and driver reset starts a new audio thread, and the
// standalone kept its scratch buffers (key silence, aux storage) in
// thread_local vectors grown on first use: the first block on every new
// thread allocated, inside the callback that must not. Those buffers now
// belong to the app and are sized by prepare().
//
// The splitter is the example with aux buses, so it takes the path that
// needs the most scratch. Its standalone build is compiled in here without
// its entry point; the app is prepared on this thread and run on a new one,
// the way a device switch does it, under a counting operator new.
#define SONORE_BUILD_STANDALONE 1
#define SONORE_STANDALONE_NO_MAIN 1
#include "../examples/splitter/plugin.cpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <new>
#include <thread>
#include <vector>

static std::atomic<bool> g_armed{false};
static std::atomic<long> g_allocs{0};

void* operator new(std::size_t n) {
  if (g_armed.load(std::memory_order_relaxed)) g_allocs.fetch_add(1, std::memory_order_relaxed);
  void* p = std::malloc(n ? n : 1);
  if (!p) throw std::bad_alloc();
  return p;
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

int main() {
  using namespace sonore::standalone;
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  int failures = 0;
  auto check = [&](bool ok, const char* what) {
    std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
  };

  // Self-check: a counter that cannot see an allocation proves nothing.
  g_armed.store(true);
  { std::vector<double> deliberate(512); if (deliberate[0] != 0.0) return 9; }
  g_armed.store(false);
  check(g_allocs.load() > 0, "the allocation counter sees an allocation (self-check)");

  static App app;
  app.testSignal.store(true);
  prepare(app, 48000.0, 256);

  long allocations = -1;
  std::vector<float> left(256, 0.0f), right(256, 0.0f);
  std::thread audio([&] {
    // A new thread, as after a device switch; counted from its first block.
    g_allocs.store(0);
    g_armed.store(true);
    for (int block = 0; block < 16; ++block) processBlock(app, left.data(), right.data(), 256);
    g_armed.store(false);
    allocations = g_allocs.load();
  });
  audio.join();

  char line[200];
  std::snprintf(line, sizeof(line), "the first blocks on a new audio thread allocate nothing "
                "(%ld allocations)", allocations);
  check(allocations == 0, line);

  // And nothing non-finite reaches the device. Here the SDK is the host: a
  // NaN fed in (as a capture device or an ASIO input can deliver) poisons
  // the splitter's filters, and what came out went to the speakers as is.
  {
    std::vector<float> nanIn(256 * 2, std::nanf(""));
    std::vector<float> quiet(256 * 2, 0.0f);
    bool allFinite = true;
    for (int block = 0; block < 8; ++block) {
      processBlock(app, left.data(), right.data(), 256, block == 0 ? nanIn.data() : quiet.data(),
                   2);
      for (int i = 0; i < 256; ++i)
        if (!std::isfinite(left[(size_t) i]) || !std::isfinite(right[(size_t) i])) allFinite = false;
    }
    check(allFinite, "a NaN fed in never reaches the device: what would have been sent is silence");
  }

  // The editor's load meter and scope, which the standalone shows through
  // guiTick like every format. It fed the level meter only: the CPU read 0
  // and the scope never moved. And the load is judged against a deadline at
  // the device's rate, which nothing told it.
  {
    prepare(app, 96000.0, 256);
    app.instance.scope.clear();
    const uint64_t before = app.instance.load.blocks();
    for (int block = 0; block < 32; ++block) processBlock(app, left.data(), right.data(), 256);
    const uint64_t timed = app.instance.load.blocks() - before;
    bool moved = false;
    for (int c = 0; c < app.instance.scope.columns(); ++c) {
      float lo = 0.0f, hi = 0.0f;
      app.instance.scope.columnAt(c, &lo, &hi);
      if (hi > 0.01f) moved = true;
    }
    char text[200];
    std::snprintf(text, sizeof(text), "every block is timed for the load meter (%llu of 32) and "
                  "the scope moves (%s)", (unsigned long long) timed, moved ? "yes" : "no");
    check(timed == 32 && moved, text);
    // A hundred deadlines, not one: the peak is the worst block since the
    // reset, the 32 real blocks above among them, and on a busy machine one
    // of those can take longer than its own deadline.
    app.instance.load.record(100.0 * 256.0 / 96000.0, 256);
    std::snprintf(text, sizeof(text), "the load deadline is the device's 96 kHz (a block of 100 "
                  "deadlines reads %.2f, want 100.00)", (double) app.instance.load.peakLoad());
    check(std::fabs(app.instance.load.peakLoad() - 100.0f) < 1.0f, text);
  }
  // An endless tail -- a frozen reverb reports INT_MAX -- is cut to 30 s in
  // an offline render. Added in full, --render --input reserved 17 GB.
  {
    using sonore::standalone::offlineTailFrames;
    char text[200];
    std::snprintf(text, sizeof(text), "an offline render plays a tail in full (%llu of 9600 frames) "
                  "and an endless one for 30 s (%llu frames)",
                  (unsigned long long) offlineTailFrames(9600u, 48000.0),
                  (unsigned long long) offlineTailFrames(0x7FFFFFFFu, 48000.0));
    check(offlineTailFrames(9600u, 48000.0) == 9600u &&
              offlineTailFrames(0x7FFFFFFFu, 48000.0) == 1440000u &&
              offlineTailFrames(0u, 48000.0) == 0u,
          text);
  }
  // The keys a page shows as held: sonore.notes, isNoteDown and onNotes read
  // the instance's note state, which every plugin format keeps from the
  // block's MIDI. The standalone never kept it, so a keyboard drawn on the
  // page stayed dark under every key played -- on the MIDI keyboard, from a
  // MIDI file, or on the page itself -- while the plugin sounded them.
  {
    using sonore::UiEventQueue;
    app.midiOpen = true; // a keyboard is open: its queue is drained
    app.midiQueue.push(0x90, 60, 100);
    UiEventQueue::Event on;
    on.kind = UiEventQueue::Event::Kind::NoteOn;
    on.index = 64;
    on.value = 90.0f;
    app.instance.uiEvents.push(on);
    processBlock(app, left.data(), right.data(), 256);
    const bool held = app.instance.notes.isDown(60) && app.instance.notes.isDown(64);
    app.midiQueue.push(0x80, 60, 0);
    UiEventQueue::Event off;
    off.kind = UiEventQueue::Event::Kind::NoteOff;
    off.index = 64;
    app.instance.uiEvents.push(off);
    processBlock(app, left.data(), right.data(), 256);
    const bool released = !app.instance.notes.isDown(60) && !app.instance.notes.isDown(64);
    // A device switch prepares the DSP, which forgets its voices: no key is
    // left lit for a note nothing sounds.
    app.midiQueue.push(0x90, 67, 100);
    processBlock(app, left.data(), right.data(), 256);
    const bool heldBeforeSwitch = app.instance.notes.isDown(67);
    prepare(app, 48000.0, 256);
    const bool clearedBySwitch = !app.instance.notes.isDown(67);
    app.midiOpen = false;
    char text[240];
    std::snprintf(text, sizeof(text), "the page is told which keys are held (a keyboard's and its "
                  "own: %s), let go (%s), and none after a device switch (%s)",
                  held && heldBeforeSwitch ? "yes" : "no", released ? "yes" : "no",
                  clearedBySwitch ? "yes" : "no");
    check(held && released && heldBeforeSwitch && clearedBySwitch, text);
  }
  // What the picker is told is playing, after a switch that failed. The hub
  // stops the old device first, so a refused one leaves nothing open -- and
  // the page was still handed the old device as the current one, at its old
  // rate: "Speakers, 48000 Hz" over silence, the reason only on stderr.
  {
    PlatformAudio system; // never opened, as after stopAll()
#if defined(_WIN32)
    sonore::asio::AsioOutput asio;
    DeviceHub hub{app, system, asio};
#else
    DeviceHub hub{app, system};
#endif
    hub.backend = "system";
    hub.index = 0;
    hub.name = "Speakers";
    hub.open = true;
#if defined(_WIN32)
    // An interface that is not plugged in: the commonest refusal there is.
    const bool switched = hub.select("asio", -1, "No Such ASIO Driver (test)");
#else
    hub.stopAll(); // what every refused switch leaves behind
    const bool switched = false;
#endif
    const std::string told = hub.json();
    const bool claimsNothing = told.find("current:{backend:'',index:-1,name:''}") != std::string::npos &&
                               told.find("sampleRate:0,bufferFrames:0") != std::string::npos;
    std::printf("  ---- after the refused switch the page is told: ...%s ----\n",
                told.substr(told.find("current:")).c_str());
    check(!switched && !hub.open && claimsNothing,
          "a refused switch leaves the picker saying nothing plays, not the old device");
  }
#if defined(_WIN32)
  // The window is the editor's size times the display's scale. Windows asks
  // for the size limits while CreateWindowEx is still running, before the
  // window knows its instance, and holds the new window to the answer: the
  // limits were taken unscaled. A fixed-size editor at 125% got a 620x300
  // window for a 775x375 page, cut off, with no edge to drag. The splitter is
  // resizable up to 8192 logical pixels, so the unscaled limit shows only on
  // an editor wider than that in device pixels: 4400 logical at 200%.
  {
    const uint32_t w = app.instance.guiWidth, h = app.instance.guiHeight;
    app.instance.guiWidth = 4400;
    app.instance.guiHeight = 300;
    HWND window = createAppWindow(app, 2.0);
    RECT client{};
    if (window) GetClientRect(window, &client);
    if (window) DestroyWindow(window);
    app.instance.guiWidth = w;
    app.instance.guiHeight = h;
    app.instance.guiScale = 1.0;
    char text[200];
    std::snprintf(text, sizeof(text), "the window holds the editor at the display's scale "
                  "(%ldx%ld, want 8800x600)", (long) client.right, (long) client.bottom);
    check(window && client.right == 8800 && client.bottom == 600, text);
  }
#endif
  // The MIDI picker's "None". It called startMidi(-1), and -1 there means
  // "the device remembered from last time": choosing no keyboard reopened the
  // keyboard being turned off. Now it closes it, opens nothing, and is
  // remembered so the next launch opens none either. Counted by midiOpens,
  // which counts open attempts whether or not a device is connected.
  // Settings go to a scratch folder, never the real user's.
  {
    char scratch[512] = {0};
#if defined(_WIN32)
    GetTempPathA(sizeof(scratch), scratch);
    std::strcat(scratch, "sonore-standalone-rt-settings");
    _putenv_s("APPDATA", scratch);
#else
    std::snprintf(scratch, sizeof(scratch), "/tmp/sonore-standalone-rt-settings");
    setenv("XDG_CONFIG_HOME", scratch, 1);
    setenv("HOME", scratch, 1);
#endif
    const int before = app.midiOpens;
    selectMidi(app, -1);
    const bool closedAndRemembered =
        app.midiOpens == before && !app.midiOpen && loadRememberedMidi() == kMidiNone;
    startMidi(app, -1); // what the next launch does
    char text[200];
    std::snprintf(text, sizeof(text), "choosing no keyboard opens none, now or at the next launch "
                  "(%d open attempt(s))", app.midiOpens - before);
    check(closedAndRemembered && app.midiOpens == before && !app.midiOpen, text);
    std::error_code ec;
    std::filesystem::remove_all(scratch, ec);
  }
  // The aux buses the splitter is handed live in the app's scratch now, not
  // on the stack of every block: each of its buses is its own run of
  // auxStore at this block's length, set up again when the length changes,
  // and every bus past its count stays an empty block with no channels.
  {
    namespace cw = sonore::clapwrap;
    bool busesRight = cw::numAuxOutputs() > 0;
    for (uint32_t frames : {256u, 100u, 7u}) {
      processBlock(app, left.data(), right.data(), frames);
      const StandaloneScratch& s = app.scratch;
      size_t flat = 0;
      for (uint32_t b = 0; b < cw::kMaxAuxOutputs; ++b) {
        const sonore::AudioBlock<float>& bus = s.aux.blocks[b];
        if (b >= cw::numAuxOutputs()) {
          if (bus.getNumChannels() != 0) busesRight = false;
          continue;
        }
        const uint32_t width = cw::auxBusChannels(b);
        if ((uint32_t) bus.getNumChannels() != width || bus.getNumSamples() != frames)
          busesRight = false;
        for (uint32_t c = 0; c < width; ++c, ++flat)
          if (bus.getChannelPointer(c) != s.auxStore.data() + flat * frames) busesRight = false;
        for (uint32_t c = width; c < cw::kMaxAudioChannels; ++c)
          if (s.aux.ptrs[b][c] != nullptr) busesRight = false;
      }
    }
    char text[200];
    std::snprintf(text, sizeof(text), "the splitter's %u aux buses are its own storage at each "
                  "block's length, and the rest stay empty", cw::numAuxOutputs());
    check(busesRight, text);
  }
  // What a device of each width is handed (renderInterleaved): the plugin's
  // two sides in its first two channels, a mono device both sides at half
  // each, a device wider than stereo silence past the two. Three apps run in
  // lockstep on the test signal, one per device, with periods that cross the
  // 256-frame slice every way; the stereo one is the reference for the rest.
  {
    static App stereo, mono, wide;
    for (App* each : {&stereo, &mono, &wide}) {
      each->testSignal.store(true);
      prepare(*each, 48000.0, 256);
    }
    std::vector<float> two(1000 * 2), one(1000), eight(1000 * 8);
    bool audible = false, monoMixes = true, wideMatches = true;
    for (uint32_t period : {1u, 255u, 256u, 257u, 480u, 1000u}) {
      renderInterleaved(stereo, two.data(), period, 2);
      renderInterleaved(mono, one.data(), period, 1);
      renderInterleaved(wide, eight.data(), period, 8);
      for (uint32_t i = 0; i < period; ++i) {
        const float l = two[(size_t) i * 2], r = two[(size_t) i * 2 + 1];
        if (l != 0.0f && r != 0.0f) audible = true;
        if (one[i] != 0.5f * (l + r)) monoMixes = false;
        if (eight[(size_t) i * 8] != l || eight[(size_t) i * 8 + 1] != r) wideMatches = false;
        for (uint32_t c = 2; c < 8; ++c)
          if (eight[(size_t) i * 8 + c] != 0.0f) wideMatches = false;
      }
    }
    char text[240];
    std::snprintf(text, sizeof(text), "a device gets the plugin's two sides (%s), a mono one "
                  "both at half (%s), a wider one silence past the two (%s)",
                  audible ? "yes" : "no", monoMixes ? "yes" : "no", wideMatches ? "yes" : "no");
    check(audible && monoMixes && wideMatches, text);
  }
  if (failures == 0) std::printf("SONORE STANDALONE RT TEST PASSED\n");
  return failures == 0 ? 0 : 1;
}
