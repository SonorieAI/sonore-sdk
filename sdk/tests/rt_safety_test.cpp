// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: the real-time safety audit.
//
// Every audio SDK's documentation says the same thing: do not allocate, lock,
// or touch the filesystem on the audio thread. Ours says it too, in comments,
// in several files. Nothing checked it. A comment is not a test, and the whole
// point of this suite is that the difference matters.
//
// This binary is built DIFFERENTLY from the other host tests, and the
// difference is the entire idea. clap_host_test loads a .clap and cannot see
// inside it: replacing operator new in the host does not touch the allocator
// the plugin module uses. So this one compiles the plugin's own source into
// itself. One module, one heap, one operator new, and that operator new is
// ours, so every allocation the wrapper or the DSP makes is counted.
//
// What it cannot see, stated plainly rather than left to be assumed:
//
//   * malloc() called directly, bypassing operator new. Nothing in the SDK
//     does this, but a third-party decoder might.
//   * locks. Counting mutex acquisitions portably needs a platform hook this
//     does not have; the SDK takes no locks in process() by construction, and
//     that remains an argument rather than a measurement.
//   * the GUI thread, which is main-thread by contract and allowed to allocate.
//
// Allocation during activate() and prepare() is CORRECT and expected, that is
// what those calls are for. The counter is armed only around the calls CLAP
// marks [audio-thread].

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>

#include <clap/clap.h>
#include <sonore/plugin.h>

// Defined by clap_wrapper.h inside the plugin's own translation unit, which
// this binary compiles into itself. A built plugin does not export it.
extern "C" const sonore::PluginDescriptor* sonore_test_descriptor();

// ── The counter ──────────────────────────────────────────────────────────────
namespace {
std::atomic<bool> g_armed{false};
std::atomic<long> g_allocs{0};
std::atomic<long> g_bytes{0};
int g_checks = 0;
int g_failures = 0;

void check(bool ok, const char* what) {
  ++g_checks;
  if (!ok) ++g_failures;
  std::printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
}

inline void countIfArmed(std::size_t n) {
  if (g_armed.load(std::memory_order_relaxed)) {
    g_allocs.fetch_add(1, std::memory_order_relaxed);
    g_bytes.fetch_add((long) n, std::memory_order_relaxed);
  }
}
} // namespace

void* operator new(std::size_t n) {
  countIfArmed(n);
  void* p = std::malloc(n ? n : 1);
  if (!p) throw std::bad_alloc();
  return p;
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
  countIfArmed(n);
  return std::malloc(n ? n : 1);
}
void* operator new[](std::size_t n, const std::nothrow_t& t) noexcept {
  return ::operator new(n, t);
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }

#if defined(__cpp_aligned_new)
// The aligned forms are separate replaceable functions: leaving them at their
// defaults would let an over-aligned allocation slip past the counter without
// anyone noticing the hole.
static void* alignedAlloc(std::size_t n, std::size_t a) {
#if defined(_MSC_VER)
  return _aligned_malloc(n ? n : 1, a);
#else
  void* p = nullptr;
  if (a < sizeof(void*)) a = sizeof(void*);
  if (posix_memalign(&p, a, n ? n : 1) != 0) return nullptr;
  return p;
#endif
}
static void alignedFree(void* p) {
#if defined(_MSC_VER)
  _aligned_free(p);
#else
  std::free(p);
#endif
}
void* operator new(std::size_t n, std::align_val_t a) {
  countIfArmed(n);
  void* p = alignedAlloc(n, (std::size_t) a);
  if (!p) throw std::bad_alloc();
  return p;
}
void* operator new[](std::size_t n, std::align_val_t a) { return ::operator new(n, a); }
void operator delete(void* p, std::align_val_t) noexcept { alignedFree(p); }
void operator delete[](void* p, std::align_val_t) noexcept { alignedFree(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { alignedFree(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { alignedFree(p); }
#endif

// ── A host that does nothing, so anything measured is the plugin's ───────────
namespace {
const void* hostGetExtension(const clap_host_t*, const char*) { return nullptr; }
void hostRequestRestart(const clap_host_t*) {}
void hostRequestProcess(const clap_host_t*) {}
void hostRequestCallback(const clap_host_t*) {}

clap_host_t g_host = {CLAP_VERSION_INIT,       nullptr,          "com.sonorie.rt-audit",
                      "Sonorie",               "",               "1.0",
                      hostGetExtension,        hostRequestRestart, hostRequestProcess,
                      hostRequestCallback};

// Every event this test will ever send is built BEFORE the counter is armed.
// Building them inside the guarded region would measure the test rather than
// the plugin, which is the classic way a harness like this quietly lies.
struct Events {
  std::vector<clap_event_param_value_t> params;
  std::vector<clap_event_note_t> notes;
  std::vector<clap_event_note_expression_t> expressions;
  std::vector<const clap_event_header_t*> current; // reserved up front
  std::vector<uint8_t> sink;                       // for anything pushed back

  void reserveAll(size_t maxEvents) {
    current.reserve(maxEvents);
    sink.reserve(64 * 1024);
  }
};
Events g_events;

uint32_t inSize(const clap_input_events_t*) { return (uint32_t) g_events.current.size(); }
const clap_event_header_t* inGet(const clap_input_events_t*, uint32_t i) {
  return i < g_events.current.size() ? g_events.current[i] : nullptr;
}
clap_input_events_t g_in = {nullptr, inSize, inGet};

// The output sink never grows while armed: its capacity is reserved up front
// and anything past it is dropped, which is what a real host's fixed-size
// event queue does anyway.
bool outPush(const clap_output_events_t*, const clap_event_header_t* e) {
  if (!e) return false;
  if (g_events.sink.size() + e->size > g_events.sink.capacity()) return false;
  g_events.sink.insert(g_events.sink.end(), (const uint8_t*) e, (const uint8_t*) e + e->size);
  return true;
}
clap_output_events_t g_out = {nullptr, outPush};
} // namespace

extern "C" const clap_plugin_entry_t clap_entry;

int main() {
  // Unbuffered, so a crash under ctest still shows the line it crashed after.
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::printf("── real-time safety audit ──────────────────────────────────────────────\n");

  // Prove the instrument works before trusting what it says.
  //
  // A counter that silently stopped counting -- a toolchain that stops
  // honouring operator new replacement, a CRT linked so that std::vector
  // routes somewhere else -- would make every plugin below look perfectly
  // clean. That failure mode is invisible and permanent, so the harness
  // allocates on purpose first and refuses to report anything if it cannot
  // see its own allocation.
  {
    g_armed.store(true);
    const long before = g_allocs.load();
    {
      std::vector<double> deliberate;
      deliberate.resize(4096); // unmistakably a heap allocation
      if (deliberate[0] != 0.0) std::printf(" ");
    }
    const long seen = g_allocs.load() - before;
    g_armed.store(false);
    g_allocs.store(0);
    g_bytes.store(0);
    check(seen > 0, "the allocation counter can SEE an allocation (harness self-check)");
    if (seen == 0) {
      std::printf("  the audit below would be meaningless; refusing to report a pass\n");
      return 1;
    }
  }

  // The declaration itself, before anything is asked of the plugin. A
  // default that is not on one of its control's steps, a table naming half
  // the positions, two parameters sharing an id: none of it fails to
  // compile, none of it crashes, and every format shows a host something
  // slightly different about it. This is where the SDK's own examples --
  // and any project that runs this audit -- are held to the contract
  // plugin.h states.
  {
    int which = -1;
    const char* why = sonore::descriptorProblem(*sonore_test_descriptor(), &which);
    char line[256];
    if (why && which >= 0)
      std::snprintf(line, sizeof(line), "the descriptor is well-formed (parameter %d has %s)",
                    which, why);
    else if (why)
      std::snprintf(line, sizeof(line), "the descriptor is well-formed (it has %s)", why);
    else
      std::snprintf(line, sizeof(line), "the descriptor is well-formed");
    check(why == nullptr, line);
  }

  if (!clap_entry.init("")) {
    std::printf("  FAIL entry init\n");
    return 1;
  }
  const auto* factory =
      static_cast<const clap_plugin_factory_t*>(clap_entry.get_factory(CLAP_PLUGIN_FACTORY_ID));
  if (!factory || factory->get_plugin_count(factory) == 0) {
    std::printf("  FAIL no factory\n");
    return 1;
  }
  const clap_plugin_descriptor_t* desc = factory->get_plugin_descriptor(factory, 0);
  std::printf("  plugin: %s\n", desc ? desc->name : "?");

  const clap_plugin_t* plugin = factory->create_plugin(factory, &g_host, desc->id);
  if (!plugin || !plugin->init(plugin)) {
    std::printf("  FAIL create/init\n");
    return 1;
  }

  const auto* params =
      static_cast<const clap_plugin_params_t*>(plugin->get_extension(plugin, CLAP_EXT_PARAMS));
  const auto* notePorts = static_cast<const clap_plugin_note_ports_t*>(
      plugin->get_extension(plugin, CLAP_EXT_NOTE_PORTS));
  const auto* audioPorts = static_cast<const clap_plugin_audio_ports_t*>(
      plugin->get_extension(plugin, CLAP_EXT_AUDIO_PORTS));
  const uint32_t nParams = params ? params->count(plugin) : 0;
  const bool takesNotes = notePorts && notePorts->count(plugin, true) > 0;

  // ── Build every event up front ────────────────────────────────────────────
  // Eight different value sets per parameter, so the plugin sees genuinely
  // changing automation rather than the same number repeated: a DSP that
  // only rebuilds coefficients when a control moves would otherwise never
  // take the branch that allocates, if one did.
  const int kVariants = 8;
  for (uint32_t p = 0; p < nParams; ++p) {
    clap_param_info_t info{};
    if (!params->get_info(plugin, p, &info)) continue;
    for (int v = 0; v < kVariants; ++v) {
      clap_event_param_value_t e{};
      e.header.size = sizeof(e);
      e.header.type = CLAP_EVENT_PARAM_VALUE;
      e.header.time = 0;
      e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
      e.param_id = info.id;
      e.port_index = -1;
      e.key = -1;
      e.channel = -1;
      e.note_id = -1;
      const double t = (double) v / (double) (kVariants - 1);
      e.value = info.min_value + t * (info.max_value - info.min_value);
      g_events.params.push_back(e);
    }
  }
  // Note expression, because MPE is a separate path through the wrapper and a
  // far less travelled one than plain notes. An allocation hiding there would
  // only ever fire for players using an expressive controller, which is the
  // worst possible audience to discover it.
  for (int k = 0; k < 16; ++k) {
    clap_event_note_expression_t e{};
    e.header.size = sizeof(e);
    e.header.type = CLAP_EVENT_NOTE_EXPRESSION;
    e.header.time = 0;
    e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    e.port_index = 0;
    e.channel = k % 4;
    e.key = 48 + (k * 5) % 36;
    e.note_id = -1;
    static const int32_t kIds[] = {CLAP_NOTE_EXPRESSION_TUNING, CLAP_NOTE_EXPRESSION_PRESSURE,
                                   CLAP_NOTE_EXPRESSION_BRIGHTNESS,
                                   CLAP_NOTE_EXPRESSION_VOLUME};
    e.expression_id = kIds[k % 4];
    e.value = (k % 2) ? 0.75 : -0.5;
    g_events.expressions.push_back(e);
  }

  for (int k = 0; k < 24; ++k) {
    clap_event_note_t e{};
    e.header.size = sizeof(e);
    e.header.type = (k % 2 == 0) ? CLAP_EVENT_NOTE_ON : CLAP_EVENT_NOTE_OFF;
    e.header.time = 0;
    e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    e.port_index = 0;
    e.channel = k % 4; // several MIDI channels, which is what MPE looks like
    e.key = 48 + (k * 5) % 36;
    e.note_id = -1;
    e.velocity = 0.8;
    g_events.notes.push_back(e);
  }
  g_events.reserveAll(nParams * 2 + 16);

  // ── Audio buffers, allocated now ──────────────────────────────────────────
  const uint32_t kMaxFrames = 512;
  const uint32_t kChans = 2;
  const uint32_t nInPorts = audioPorts ? audioPorts->count(plugin, true) : 0;
  const uint32_t nOutPorts = audioPorts ? audioPorts->count(plugin, false) : 1;

  std::vector<std::vector<float>> storage;
  std::vector<std::vector<float*>> ptrs;
  const uint32_t totalPorts = nInPorts + nOutPorts;
  storage.resize(totalPorts * kChans);
  ptrs.resize(totalPorts);
  for (uint32_t port = 0; port < totalPorts; ++port) {
    ptrs[port].resize(kChans);
    for (uint32_t c = 0; c < kChans; ++c) {
      storage[port * kChans + c].assign(kMaxFrames, 0.0f);
      ptrs[port][c] = storage[port * kChans + c].data();
    }
  }
  std::vector<std::vector<double>> storage64(totalPorts * kChans);
  std::vector<std::vector<double*>> ptrs64(totalPorts);
  for (uint32_t port = 0; port < totalPorts; ++port) {
    ptrs64[port].resize(kChans);
    for (uint32_t c = 0; c < kChans; ++c) {
      storage64[port * kChans + c].assign(kMaxFrames, 0.0);
      ptrs64[port][c] = storage64[port * kChans + c].data();
    }
  }

  std::vector<clap_audio_buffer_t> inBuf(nInPorts), outBuf(nOutPorts);
  for (uint32_t i = 0; i < nInPorts; ++i)
    inBuf[i] = {ptrs[i].data(), nullptr, kChans, 0, 0};
  for (uint32_t i = 0; i < nOutPorts; ++i)
    outBuf[i] = {ptrs[nInPorts + i].data(), nullptr, kChans, 0, 0};

  // A real transport rather than nullptr: the wrapper takes a different branch
  // for each, and the one a DAW actually uses is the one with a transport.
  clap_event_transport_t transport{};
  transport.header.size = sizeof(transport);
  transport.header.type = CLAP_EVENT_TRANSPORT;
  transport.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
  transport.flags = CLAP_TRANSPORT_HAS_TEMPO | CLAP_TRANSPORT_HAS_BEATS_TIMELINE |
                    CLAP_TRANSPORT_HAS_TIME_SIGNATURE | CLAP_TRANSPORT_IS_PLAYING;
  transport.tempo = 128.0;
  transport.tsig_num = 7; // not 4/4, so a plugin that assumes it is shows up
  transport.tsig_denom = 8;

  clap_process_t process{};
  process.steady_time = 0;
  process.frames_count = kMaxFrames;
  process.transport = &transport;
  process.audio_inputs = nInPorts ? inBuf.data() : nullptr;
  process.audio_outputs = outBuf.data();
  process.audio_inputs_count = nInPorts;
  process.audio_outputs_count = nOutPorts;
  process.in_events = &g_in;
  process.out_events = &g_out;

  // Activating allocates, and should: that is what activate() is FOR.
  if (!plugin->activate(plugin, 48000.0, 1, kMaxFrames)) {
    std::printf("  FAIL activate\n");
    return 1;
  }

  // A warm-up pass outside the guard. The very first call through any code
  // path can allocate one-time statics -- a locale, a lazily built table --
  // and counting those would be blaming the plugin for something that happens
  // once in a process's life rather than once per block.
  uint32_t seed = 1u;
  auto fillEvents = [&](uint32_t block) {
    g_events.current.clear();
    if (nParams > 0) {
      const size_t base = (size_t) (block % kVariants);
      for (uint32_t p = 0; p < nParams; ++p) {
        const size_t idx = p * (size_t) kVariants + base;
        if (idx < g_events.params.size())
          g_events.current.push_back(&g_events.params[idx].header);
      }
    }
    if (takesNotes && !g_events.notes.empty()) {
      g_events.current.push_back(&g_events.notes[block % g_events.notes.size()].header);
      if (!g_events.expressions.empty())
        g_events.current.push_back(
            &g_events.expressions[block % g_events.expressions.size()].header);
    }
  };
  auto fillAudio = [&](uint32_t frames) {
    for (uint32_t port = 0; port < nInPorts; ++port)
      for (uint32_t c = 0; c < kChans; ++c) {
        float* d = ptrs[port][c];
        for (uint32_t i = 0; i < frames; ++i) {
          seed = seed * 1664525u + 1013904223u;
          d[i] = (float) ((int32_t) (seed >> 8) % 20001 - 10000) / 40000.0f;
        }
      }
  };

  plugin->start_processing(plugin);
  for (uint32_t b = 0; b < 64; ++b) {
    fillEvents(b);
    fillAudio(kMaxFrames);
    plugin->process(plugin, &process);
    g_events.sink.clear();
  }
  plugin->stop_processing(plugin);

  // ── Armed ─────────────────────────────────────────────────────────────────
  g_allocs.store(0);
  g_bytes.store(0);
  g_armed.store(true);

  plugin->start_processing(plugin);
  const long afterStart = g_allocs.load();

  // Block sizes that are not the maximum and not powers of two, because a
  // buffer sized for exactly one of those is a buffer that gets resized for
  // the others.
  const uint32_t sizes[] = {512, 64, 129, 480, 1, 333, 512, 7};
  for (uint32_t b = 0; b < 400; ++b) {
    const uint32_t frames = sizes[b % (sizeof(sizes) / sizeof(sizes[0]))];
    process.frames_count = frames;
    fillEvents(b);
    fillAudio(frames);
    plugin->process(plugin, &process);
    process.steady_time += frames;
    transport.song_pos_beats = (clap_beattime) (b * CLAP_BEATTIME_FACTOR / 4);
    g_events.sink.clear();
    if (b % 97 == 0) plugin->reset(plugin);          // [audio-thread]
    if (params && b % 53 == 0) {
      g_events.current.clear();
      params->flush(plugin, &g_in, &g_out);          // [audio-thread] while active
    }
  }
  const long afterProcess = g_allocs.load();

  // The 64-bit path is an entirely separate route through the wrapper and gets
  // a fraction of the traffic the 32-bit one does, which makes it exactly the
  // place an allocation would sit unnoticed. Run it only where the plugin
  // actually offers double precision -- asking for it otherwise measures the
  // wrapper refusing, which proves nothing.
  bool ran64 = false;
  if (audioPorts && nOutPorts > 0) {
    clap_audio_port_info_t pi{};
    if (audioPorts->get(plugin, 0, false, &pi) && (pi.flags & CLAP_AUDIO_PORT_SUPPORTS_64BITS)) {
      for (uint32_t i = 0; i < nInPorts; ++i) {
        inBuf[i].data32 = nullptr;
        inBuf[i].data64 = ptrs64[i].data();
      }
      for (uint32_t i = 0; i < nOutPorts; ++i) {
        outBuf[i].data32 = nullptr;
        outBuf[i].data64 = ptrs64[nInPorts + i].data();
      }
      for (uint32_t b = 0; b < 120; ++b) {
        const uint32_t frames = sizes[b % (sizeof(sizes) / sizeof(sizes[0]))];
        process.frames_count = frames;
        fillEvents(b);
        for (uint32_t port = 0; port < nInPorts; ++port)
          for (uint32_t c = 0; c < kChans; ++c) {
            double* d = ptrs64[port][c];
            for (uint32_t i = 0; i < frames; ++i) {
              seed = seed * 1664525u + 1013904223u;
              d[i] = (double) ((int32_t) (seed >> 8) % 20001 - 10000) / 40000.0;
            }
          }
        plugin->process(plugin, &process);
        process.steady_time += frames;
        g_events.sink.clear();
      }
      ran64 = true;
    }
  }
  const long after64 = g_allocs.load();
  plugin->stop_processing(plugin);
  const long afterStop = g_allocs.load() - after64;

  g_armed.store(false);
  const long total = g_allocs.load();
  const long bytes = g_bytes.load();
  // ──────────────────────────────────────────────────────────────────────────

  std::printf("  ---- allocations on the audio thread: %ld (%ld bytes) ----\n", total, bytes);
  check(afterStart == 0, "start_processing() allocates nothing");
  check(afterProcess == 0, "400 blocks of process(), reset() and flush() allocate nothing");
  if (ran64)
    check(after64 == 0, "...and 120 more in 64-bit, which is a separate path entirely");
  else
    std::printf("  ---- float-only DSP: no 64-bit path to audit ----\n");
  check(afterStop == 0, "stop_processing() allocates nothing");
  check(total == 0, "nothing at all was allocated on the audio thread");

  // ── every rate a host runs at, every block size it hands over ─────────────
  //
  // Everything above ran at 48 kHz. The 192 kHz sizing bugs -- reverb combs,
  // chorus lines and a limiter's look-ahead all silently clamped at high
  // rates -- were found by READING, which is the wrong way to find a bug that
  // a loop can find. So: nine rates from 8 kHz to 384 kHz, five block sizes
  // from a single sample to 8192, and at every combination the plugin must
  // activate, stay finite, stay bounded, report a latency that fits inside a
  // second, and allocate NOTHING while processing -- because a buffer that
  // turned out too small for the rate is exactly what a lazy allocation on
  // the audio thread looks like.
  {
    const auto* latencyExt = static_cast<const clap_plugin_latency_t*>(
        plugin->get_extension(plugin, CLAP_EXT_LATENCY));
    static const double kRates[] = {8000.0,  22050.0,  44100.0,  48000.0, 88200.0,
                                    96000.0, 176400.0, 192000.0, 384000.0};
    static const uint32_t kBlocks[] = {1, 7, 64, 4096, 8192};
    const uint32_t kBig = 8192;

    std::vector<std::vector<float>> bigStorage(totalPorts * kChans);
    std::vector<std::vector<float*>> bigPtrs(totalPorts);
    for (uint32_t port = 0; port < totalPorts; ++port) {
      bigPtrs[port].resize(kChans);
      for (uint32_t c = 0; c < kChans; ++c) {
        bigStorage[port * kChans + c].assign(kBig, 0.0f);
        bigPtrs[port][c] = bigStorage[port * kChans + c].data();
      }
    }
    std::vector<clap_audio_buffer_t> bigIn(nInPorts), bigOut(nOutPorts);
    for (uint32_t i = 0; i < nInPorts; ++i)
      bigIn[i] = {bigPtrs[i].data(), nullptr, kChans, 0, 0};
    for (uint32_t i = 0; i < nOutPorts; ++i)
      bigOut[i] = {bigPtrs[nInPorts + i].data(), nullptr, kChans, 0, 0};
    clap_process_t big = process;
    big.audio_inputs = nInPorts ? bigIn.data() : nullptr;
    big.audio_outputs = bigOut.data();

    plugin->deactivate(plugin);
    int combos = 0;
    long allocsWhileProcessing = 0;
    bool activatedAll = true, finiteAll = true, latencyFits = true;
    double worstPeak = 0.0;
    char worst[160] = "";
    for (double rate : kRates) {
      for (uint32_t block : kBlocks) {
        if (!plugin->activate(plugin, rate, 1, block)) {
          activatedAll = false;
          std::printf("  FAIL activate(%g Hz, max %u) refused\n", rate, block);
          continue;
        }
        if (latencyExt) {
          const uint32_t lat = latencyExt->get(plugin);
          if ((double) lat > rate) {
            latencyFits = false;
            std::printf("  FAIL latency %u at %g Hz is more than a second\n", lat, rate);
          }
        }
        g_allocs.store(0);
        g_armed.store(true);
        plugin->start_processing(plugin);
        double peak = 0.0;
        bool finite = true;
        for (uint32_t b = 0; b < 24; ++b) {
          // The declared maximum, and sizes below it: a host is allowed both.
          const uint32_t frames = (b % 3 == 0) ? block : 1 + (b * 37) % block;
          big.frames_count = frames;
          fillEvents(b);
          for (uint32_t port = 0; port < nInPorts; ++port)
            for (uint32_t c = 0; c < kChans; ++c) {
              float* d = bigPtrs[port][c];
              for (uint32_t i = 0; i < frames; ++i) {
                seed = seed * 1664525u + 1013904223u;
                d[i] = (float) ((int32_t) (seed >> 8) % 20001 - 10000) / 40000.0f;
              }
            }
          plugin->process(plugin, &big);
          big.steady_time += frames;
          g_events.sink.clear();
          for (uint32_t port = 0; port < nOutPorts; ++port)
            for (uint32_t c = 0; c < kChans; ++c) {
              const float* d = bigPtrs[nInPorts + port][c];
              for (uint32_t i = 0; i < frames; ++i) {
                if (!(d[i] == d[i]) || d[i] > 1e30f || d[i] < -1e30f) finite = false;
                const double a = d[i] < 0.0f ? -(double) d[i] : (double) d[i];
                if (a > peak) peak = a;
              }
            }
        }
        plugin->stop_processing(plugin);
        g_armed.store(false);
        const long allocs = g_allocs.load();
        if (allocs != 0)
          std::printf("  FAIL %ld allocation(s) while processing at %g Hz, max %u\n", allocs,
                      rate, block);
        allocsWhileProcessing += allocs;
        if (!finite) {
          finiteAll = false;
          std::printf("  FAIL non-finite output at %g Hz, max %u\n", rate, block);
        }
        if (peak > worstPeak) {
          worstPeak = peak;
          std::snprintf(worst, sizeof(worst), "%g Hz, max %u", rate, block);
        }
        plugin->deactivate(plugin);
        ++combos;
      }
    }
    std::printf("  ---- %d rate/block combinations, worst peak %.3f at %s ----\n", combos,
                worstPeak, worst);
    check(activatedAll, "the plugin activates at every rate from 8 kHz to 384 kHz with every "
                        "block size from 1 to 8192");
    check(finiteAll, "...and its output is finite at every one of them");
    check(worstPeak < 100.0, "...and bounded");
    check(latencyFits, "...and the latency it reports fits inside a second at every rate");
    check(allocsWhileProcessing == 0,
          "...and nothing was allocated while processing at any of them");
    plugin->activate(plugin, 48000.0, 1, kMaxFrames); // back to what the teardown expects
  }

  plugin->deactivate(plugin);
  plugin->destroy(plugin);
  clap_entry.deinit();

  std::printf("\n%d checks, %d failure(s)\n", g_checks, g_failures);
  if (g_failures == 0) std::printf("SONORE RT SAFETY PASSED\n");
  return g_failures == 0 ? 0 : 1;
}
