// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: the output guard, in every format that hands a block back.
//
// A DSP that makes NaN or infinity must not hand it to a host: a block with
// one non-finite sample on any output port leaves as silence on every port,
// the DSP keeps its state, and the first such block is reported once, from
// the main thread -- the host's log where it has one, stderr otherwise.
//
// The probe below emits NaN, +inf or -inf on demand (parameter 0), on the
// main bus's left channel, its right channel or the aux bus (parameter 1). It
// is compiled with the CLAP, VST3 and LV2 wrappers and the standalone into
// this one binary, as rt_safety_test does it, so each format's own Instance
// can be read after driving it through its C API -- and global operator new
// is this file's, so a guard that allocated on the audio thread would be
// counted. Built twice: SONORE_TEST_GUARD_OFF builds the same probe with
// guardOutput = false, and then the NaN has to come through untouched.
#define SONORE_BUILD_VST3 1
#define SONORE_BUILD_LV2 1
#define SONORE_BUILD_STANDALONE 1
#define SONORE_STANDALONE_NO_MAIN 1
#define SONORE_NUM_PARAMS 2

#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <string>
#include <thread>
#include <vector>

#include <sonore/dsp.h>

// ── The probe ────────────────────────────────────────────────────────────────

struct SonoreDsp {
  /** Blocks run since prepare(). The guard must never reset a DSP: this keeps
   *  counting across a silenced block. */
  uint64_t blocks = 0;
  int prepares = 0;
  /** What the last prepare() and the blocks since said about rendering
   *  offline: the standalone's --render is a bounce, and says so. */
  bool preparedOffline = false;
  bool ranOffline = false;

  /** A delay on the main bus that the probe applies and reports, as a
   *  look-ahead limiter does. 0 -- none -- except while the standalone's
   *  offline render is checked for compensating it. */
  static int& delay() {
    static int samples = 0;
    return samples;
  }
  int latencySamples() const { return delay(); }
  double line[2][64] = {};
  size_t lineAt = 0;

  void prepare(const sonore::ProcessSpec& spec) {
    ++prepares;
    blocks = 0;
    preparedOffline = spec.offline;
    ranOffline = false;
    for (auto& channel : line)
      for (double& v : channel) v = 0.0;
    lineAt = 0;
  }

  /** The aux bus's content: finite, known, and the same expression the test
   *  computes, so a clean block can be compared bit for bit. */
  template <typename S>
  static S auxSample(size_t channel, size_t i) {
    return (S) (0.001 * (double) (i + 1) * (double) (channel + 1));
  }

  template <typename S>
  void process(sonore::ProcessContextT<S>& ctx, const float* params) {
    ++blocks;
    if (ctx.offline) ranOffline = true;
    sonore::AudioBlock<S>& main = ctx.main;
    const size_t n = main.getNumSamples();
    for (size_t c = 0; c < main.getNumChannels(); ++c) {
      S* x = main.getChannelPointer(c);
      for (size_t i = 0; i < n; ++i) x[i] = x[i] * (S) 0.5;
    }
    const int d = delay();
    if (d > 0 && d <= 64)
      for (size_t i = 0; i < n; ++i, ++lineAt)
        for (size_t c = 0; c < main.getNumChannels() && c < 2; ++c) {
          S* x = main.getChannelPointer(c);
          double& slot = line[c][lineAt % (size_t) d];
          const double out = slot;
          slot = (double) x[i];
          x[i] = (S) out;
        }
    for (size_t b = 0; b < ctx.numAux; ++b)
      for (size_t c = 0; c < ctx.aux[b].getNumChannels(); ++c) {
        S* x = ctx.aux[b].getChannelPointer(c);
        for (size_t i = 0; i < ctx.aux[b].getNumSamples(); ++i) x[i] = auxSample<S>(c, i);
      }
    const int poison = (int) params[0];
    if (poison == 0 || n == 0) return;
    const S bad = poison == 1   ? std::numeric_limits<S>::quiet_NaN()
                  : poison == 2 ? std::numeric_limits<S>::infinity()
                                : -std::numeric_limits<S>::infinity();
    const int where = (int) params[1];
    if (where == 2 && ctx.numAux > 0 && ctx.aux[0].getNumChannels() > 1 &&
        ctx.aux[0].getNumSamples() > 0)
      ctx.aux[0].getChannelPointer(1)[ctx.aux[0].getNumSamples() / 2] = bad;
    else if (where == 1 && main.getNumChannels() > 1)
      main.getChannelPointer(1)[n - 1] = bad;
    else
      main.getChannelPointer(0)[0] = bad;
  }
};

#include <sonore/plugin.h>

static const char* const kPoisonNames[] = {"none", "NaN", "+inf", "-inf"};
static const char* const kWhereNames[] = {"main left", "main right", "aux"};

static const sonore::ParamInfo kParamTable[SONORE_NUM_PARAMS] = {
    {"poison", "Poison", "", 0.0f, 3.0f, 0.0f, 4, nullptr, kPoisonNames, 4},
    {"where", "Where", "", 0.0f, 2.0f, 0.0f, 3, nullptr, kWhereNames, 3},
};

static const sonore::AuxBusInfo kAux[1] = {{"Aux", 2}};

// Positional, as every example writes one -- which is the point of putting
// guardOutput LAST: a descriptor written before it existed still compiles and
// gets the guard. The guard-off build names it.
static const sonore::PluginDescriptor kDesc = {
    "com.sonorie.test.outputguard",
    "Sonore Guard Probe",
    "Sonorie",
    "1.0.0",
    "Emits NaN or infinity on demand.",
    "https://sonorie.com",
    false,
    kParamTable,
    SONORE_NUM_PARAMS,
    nullptr,
    0,
    "utility",
    nullptr,
    nullptr,
    2,
    2,
    kAux,
    1,
    false,
    false,
    sonore::EditorKind::Auto,
    sonore::EditorConstraints{},
#if defined(SONORE_TEST_GUARD_OFF)
    false,
#endif
};

#include <sonore/formats.h>

#if defined(_WIN32)
#include <io.h>
#define SONORE_DUP _dup
#define SONORE_DUP2 _dup2
#define SONORE_CLOSE _close
#define SONORE_FILENO _fileno
#else
#include <unistd.h>
#define SONORE_DUP dup
#define SONORE_DUP2 dup2
#define SONORE_CLOSE close
#define SONORE_FILENO fileno
#endif

#if defined(SONORE_TEST_GUARD_OFF)
static constexpr bool kGuard = false;
#else
static constexpr bool kGuard = true;
#endif

// ── The allocation counter (rt_safety_test's) ────────────────────────────────
namespace {
std::atomic<bool> g_armed{false};
std::atomic<long> g_allocs{0};
int g_checks = 0;
int g_failures = 0;

void check(bool ok, const char* what) {
  ++g_checks;
  if (!ok) ++g_failures;
  std::printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
}

inline void countIfArmed() {
  if (g_armed.load(std::memory_order_relaxed)) g_allocs.fetch_add(1, std::memory_order_relaxed);
}
} // namespace

void* operator new(std::size_t n) {
  countIfArmed();
  void* p = std::malloc(n ? n : 1);
  if (!p) throw std::bad_alloc();
  return p;
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
  countIfArmed();
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

namespace {

std::thread::id g_mainThread;
/** True on a thread onAudioThread() made: what a host callback checks to know
 *  it was called from the audio thread. */
thread_local bool t_audioThread = false;

/** Run `work` on a thread of its own, as a host's audio thread, with the
 *  allocation counter armed for exactly that. */
template <typename Work>
void onAudioThread(Work work) {
  std::thread audio([&work] {
    t_audioThread = true;
    g_armed.store(true);
    work();
    g_armed.store(false);
  });
  audio.join();
}

/** stderr, captured to a file for the length of a call. The formats with no
 *  host log report there, and a report nobody can see is not tested. */
std::string captureStderr(void (*call)(void*), void* context) {
  const char* path =
      kGuard ? "sonore-output-guard-stderr.txt" : "sonore-output-guard-off-stderr.txt";
  std::fflush(stderr);
  const int saved = SONORE_DUP(SONORE_FILENO(stderr));
  if (FILE* f = std::fopen(path, "wb")) {
    SONORE_DUP2(SONORE_FILENO(f), SONORE_FILENO(stderr));
    std::fclose(f);
  }
  call(context);
  std::fflush(stderr);
  SONORE_DUP2(saved, SONORE_FILENO(stderr));
  SONORE_CLOSE(saved);
  std::string text;
  if (FILE* f = std::fopen(path, "rb")) {
    char buffer[1024];
    size_t got = 0;
    while ((got = std::fread(buffer, 1, sizeof(buffer), f)) > 0) text.append(buffer, got);
    std::fclose(f);
  }
  std::remove(path);
  return text;
}

bool mentions(const std::string& text, const char* what) {
  return text.find(what) != std::string::npos;
}

/** One block's buffers: a stereo input, the main output and the aux output,
 *  the input a known signal and the outputs pre-filled with garbage. */
template <typename S>
struct Buffers {
  std::vector<S> in[2], out[2], aux[2];
  explicit Buffers(size_t frames) {
    for (int c = 0; c < 2; ++c) {
      in[c].resize(frames);
      out[c].assign(frames, (S) 7.0);
      aux[c].assign(frames, (S) 7.0);
      for (size_t i = 0; i < frames; ++i)
        in[c][i] = (S) (0.5 * std::sin(0.01 * (double) (i + 1) + (double) c));
    }
  }
  void scribble() {
    for (int c = 0; c < 2; ++c) {
      std::fill(out[c].begin(), out[c].end(), (S) 7.0);
      std::fill(aux[c].begin(), aux[c].end(), (S) 7.0);
    }
  }
  /** Every output sample exactly what the probe makes of the input: bit for
   *  bit, not near it. `piece` is the block the DSP ran, when a wrapper split
   *  the host's: the aux ramp starts over in each. */
  bool exact(size_t piece = 0) const {
    for (int c = 0; c < 2; ++c)
      for (size_t i = 0; i < in[c].size(); ++i) {
        const S main = in[c][i] * (S) 0.5;
        const S side = SonoreDsp::auxSample<S>((size_t) c, piece ? i % piece : i);
        if (std::memcmp(&out[c][i], &main, sizeof(S)) != 0) return false;
        if (std::memcmp(&aux[c][i], &side, sizeof(S)) != 0) return false;
      }
    return true;
  }
  /** Every sample of every port +0.0. */
  bool silent() const {
    for (int c = 0; c < 2; ++c)
      for (size_t i = 0; i < out[c].size(); ++i) {
        const S zero = (S) 0;
        if (std::memcmp(&out[c][i], &zero, sizeof(S)) != 0) return false;
        if (std::memcmp(&aux[c][i], &zero, sizeof(S)) != 0) return false;
      }
    return true;
  }
  /** Exactly the probe's output with its one poisoned sample left in place:
   *  what a plugin with the guard off must hand back. */
  bool poisonedOnly(int where) const {
    size_t bad = 0;
    for (int c = 0; c < 2; ++c)
      for (size_t i = 0; i < in[c].size(); ++i) {
        const bool mainBad = !std::isfinite((double) out[c][i]);
        const bool auxBad = !std::isfinite((double) aux[c][i]);
        if (mainBad || auxBad) {
          ++bad;
          const bool expected = where == 2 ? (auxBad && c == 1 && i == in[c].size() / 2)
                                : where == 1 ? (mainBad && c == 1 && i == in[c].size() - 1)
                                             : (mainBad && c == 0 && i == 0);
          if (!expected) return false;
          continue;
        }
        const S main = in[c][i] * (S) 0.5;
        const S side = SonoreDsp::auxSample<S>((size_t) c, i);
        if (std::memcmp(&out[c][i], &main, sizeof(S)) != 0) return false;
        if (std::memcmp(&aux[c][i], &side, sizeof(S)) != 0) return false;
      }
    return bad == 1;
  }
};

// ── CLAP ─────────────────────────────────────────────────────────────────────

struct MockHost {
  clap_host_t host{};
  bool offersLog = true;
  std::atomic<int> callbacks{0};
  bool callbackOnAudioThread = false;
  int logs = 0;
  int severity = -1;
  std::string message;
  std::thread::id logThread;
};

void hostLog(const clap_host_t* h, clap_log_severity severity, const char* msg) {
  auto* m = (MockHost*) h->host_data;
  ++m->logs;
  m->severity = severity;
  m->message = msg ? msg : "";
  m->logThread = std::this_thread::get_id();
}
const clap_host_log_t kHostLog = {hostLog};

const void* hostGetExtension(const clap_host_t* h, const char* id) {
  auto* m = (MockHost*) h->host_data;
  if (m->offersLog && std::strcmp(id, CLAP_EXT_LOG) == 0) return &kHostLog;
  return nullptr;
}
void hostRequestCallback(const clap_host_t* h) {
  auto* m = (MockHost*) h->host_data;
  if (m->callbacks.fetch_add(1) == 0) m->callbackOnAudioThread = t_audioThread;
}
void hostRequestNothing(const clap_host_t*) {}

void initHost(MockHost& m, bool offersLog) {
  m.offersLog = offersLog;
  m.host.clap_version = CLAP_VERSION;
  m.host.host_data = &m;
  m.host.name = "com.sonorie.output-guard-test";
  m.host.vendor = "Sonorie";
  m.host.url = "";
  m.host.version = "1.0";
  m.host.get_extension = hostGetExtension;
  m.host.request_restart = hostRequestNothing;
  m.host.request_process = hostRequestNothing;
  m.host.request_callback = hostRequestCallback;
}

const clap_input_events_t kNoEvents = {
    nullptr, [](const clap_input_events_t*) -> uint32_t { return 0; },
    [](const clap_input_events_t*, uint32_t) -> const clap_event_header_t* { return nullptr; }};
const clap_output_events_t kEventSink = {
    nullptr, [](const clap_output_events_t*, const clap_event_header_t*) { return true; }};

const clap_plugin_t* createClap(MockHost& host) {
  const auto* factory =
      static_cast<const clap_plugin_factory_t*>(clap_entry.get_factory(CLAP_PLUGIN_FACTORY_ID));
  const clap_plugin_descriptor_t* desc = factory ? factory->get_plugin_descriptor(factory, 0) : nullptr;
  const clap_plugin_t* plugin = desc ? factory->create_plugin(factory, &host.host, desc->id) : nullptr;
  if (plugin && !plugin->init(plugin)) {
    plugin->destroy(plugin);
    return nullptr;
  }
  return plugin;
}

template <typename S>
void clapBlock(const clap_plugin_t* plugin, Buffers<S>& b, uint32_t frames) {
  S* ins[2] = {b.in[0].data(), b.in[1].data()};
  S* outs[2] = {b.out[0].data(), b.out[1].data()};
  S* auxes[2] = {b.aux[0].data(), b.aux[1].data()};
  clap_audio_buffer_t inBuf{}, outBufs[2]{};
  inBuf.channel_count = 2;
  outBufs[0].channel_count = 2;
  outBufs[1].channel_count = 2;
  if constexpr (sizeof(S) == 4) {
    inBuf.data32 = (float**) ins;
    outBufs[0].data32 = (float**) outs;
    outBufs[1].data32 = (float**) auxes;
  } else {
    inBuf.data64 = (double**) ins;
    outBufs[0].data64 = (double**) outs;
    outBufs[1].data64 = (double**) auxes;
  }
  clap_process_t p{};
  p.frames_count = frames;
  p.steady_time = -1;
  p.audio_inputs = &inBuf;
  p.audio_inputs_count = 1;
  p.audio_outputs = outBufs;
  p.audio_outputs_count = 2;
  p.in_events = &kNoEvents;
  p.out_events = &kEventSink;
  plugin->process(plugin, &p);
}

bool meterShowsSilence(sonore::MeterState& meter) {
  meter.tick(10.0); // long enough for the last clean block's peak to have decayed away
  return meter.level() < 1e-3f && std::isfinite(meter.vu()) && std::isfinite(meter.db());
}

template <typename S>
void clapPath(const char* label) {
  std::printf("\n── CLAP, %s ──\n", label);
  MockHost host;
  initHost(host, true);
  const clap_plugin_t* plugin = createClap(host);
  char line[400];
  std::snprintf(line, sizeof(line), "%s: the probe instantiates", label);
  check(plugin != nullptr, line);
  if (!plugin) return;
  constexpr uint32_t kMax = 512;
  plugin->activate(plugin, 48000.0, 1, kMax);
  plugin->start_processing(plugin);
  sonore::clapwrap::Instance* inst = sonore::clapwrap::self(plugin);
  const int preparesBefore = inst->dsp.prepares;
  g_allocs.store(0);

  Buffers<S> b(256);
  const auto run = [&](int poison, int where) {
    inst->params[0] = (float) poison;
    inst->params[1] = (float) where;
    b.scribble();
    onAudioThread([&] { clapBlock(plugin, b, 256); });
  };

  run(0, 0);
  std::snprintf(line, sizeof(line), "%s: a clean block comes out bit for bit, main and aux", label);
  check(b.exact(), line);

  struct Case {
    int poison, where;
  } cases[] = {{1, 0}, {2, 1}, {3, 2}};
  int expectedTrips = 0;
  for (const Case& k : cases) {
    run(k.poison, k.where);
    if (kGuard) {
      ++expectedTrips;
      std::snprintf(line, sizeof(line), "%s: %s on the %s -> every port of the block is silence",
                    label, kPoisonNames[k.poison], kWhereNames[k.where]);
      check(b.silent(), line);
      std::snprintf(line, sizeof(line), "%s: ...and the meter shows that silence, not the %s",
                    label, kPoisonNames[k.poison]);
      check(meterShowsSilence(inst->meter), line);
    } else {
      std::snprintf(line, sizeof(line),
                    "%s: guard off, %s on the %s comes through untouched, the rest bit for bit",
                    label, kPoisonNames[k.poison], kWhereNames[k.where]);
      check(b.poisonedOnly(k.where), line);
    }
    run(0, 0);
    std::snprintf(line, sizeof(line), "%s: ...and the next clean block plays as normal, bit for bit",
                  label);
    check(b.exact(), line);
  }

  std::snprintf(line, sizeof(line),
                "%s: the DSP was not reset (%llu blocks counted since prepare, %d prepare(s) "
                "during the run)",
                label, (unsigned long long) inst->dsp.blocks, inst->dsp.prepares - preparesBefore);
  check(inst->dsp.blocks == 7 && inst->dsp.prepares == preparesBefore, line);

  // A block past what activate() promised is run in pieces, each guarded.
  {
    Buffers<S> big(1024);
    inst->params[0] = 1.0f;
    inst->params[1] = 1.0f;
    onAudioThread([&] { clapBlock(plugin, big, 1024); });
    if (kGuard) {
      expectedTrips += 2;
      std::snprintf(line, sizeof(line), "%s: a block of twice the maximum, poisoned, is silence "
                    "throughout", label);
      check(big.silent(), line);
    }
    inst->params[0] = 0.0f;
    big.scribble();
    onAudioThread([&] { clapBlock(plugin, big, 1024); });
    std::snprintf(line, sizeof(line), "%s: ...and clean, it is bit for bit", label);
    check(big.exact(kMax), line);
  }

  const uint32_t trips = inst->outputGuard.trips.load();
  std::snprintf(line, sizeof(line), "%s: the guard counted %u silenced block(s), want %d", label,
                (unsigned) trips, expectedTrips);
  check((int) trips == expectedTrips, line);
  std::snprintf(line, sizeof(line), "%s: no allocation on the audio thread (%ld)", label,
                g_allocs.load());
  check(g_allocs.load() == 0, line);

  if (kGuard) {
    std::snprintf(line, sizeof(line), "%s: request_callback was asked once, from the audio "
                  "thread (%d time(s))", label, host.callbacks.load());
    check(host.callbacks.load() == 1 && host.callbackOnAudioThread, line);
    std::snprintf(line, sizeof(line), "%s: ...and nothing was logged before the main thread ran",
                  label);
    check(host.logs == 0, line);
    plugin->on_main_thread(plugin);
    plugin->on_main_thread(plugin);
    std::snprintf(line, sizeof(line), "%s: on_main_thread logs exactly one warning, on the main "
                  "thread (%d log(s), severity %d)", label, host.logs, host.severity);
    check(host.logs == 1 && host.severity == CLAP_LOG_WARNING && host.logThread == g_mainThread,
          line);
    std::printf("       \"%s\"\n", host.message.c_str());
    std::snprintf(line, sizeof(line), "%s: ...naming the plugin and the count (%u blocks)", label,
                  (unsigned) expectedTrips);
    char count[32];
    std::snprintf(count, sizeof(count), "%d blocks", expectedTrips);
    check(mentions(host.message, kDesc.name) && mentions(host.message, count), line);
    // More trips after the report: counted, never said again.
    run(1, 0);
    plugin->on_main_thread(plugin);
    std::snprintf(line, sizeof(line), "%s: a later trip is counted and says nothing more "
                  "(%u trips, %d callback(s), %d log(s))", label,
                  (unsigned) inst->outputGuard.trips.load(), host.callbacks.load(), host.logs);
    check(inst->outputGuard.trips.load() == (uint32_t) expectedTrips + 1 &&
              host.callbacks.load() == 1 && host.logs == 1,
          line);
  } else {
    plugin->on_main_thread(plugin);
    std::snprintf(line, sizeof(line), "%s: guard off: no callback asked, nothing logged", label);
    check(host.callbacks.load() == 0 && host.logs == 0, line);
  }

  plugin->stop_processing(plugin);
  plugin->deactivate(plugin);
  plugin->destroy(plugin);
}

/** A host with no log: the report goes to stderr. */
void clapNoLog() {
  std::printf("\n── CLAP, a host with no log ──\n");
  MockHost host;
  initHost(host, false);
  const clap_plugin_t* plugin = createClap(host);
  if (!plugin) {
    check(false, "the probe instantiates");
    return;
  }
  plugin->activate(plugin, 48000.0, 1, 512);
  plugin->start_processing(plugin);
  sonore::clapwrap::Instance* inst = sonore::clapwrap::self(plugin);
  inst->params[0] = 2.0f;
  Buffers<float> b(128);
  onAudioThread([&] { clapBlock(plugin, b, 128); });
  struct Ctx {
    const clap_plugin_t* plugin;
  } ctx{plugin};
  const std::string err = captureStderr(
      [](void* c) {
        const clap_plugin_t* p = ((Ctx*) c)->plugin;
        p->on_main_thread(p);
        p->on_main_thread(p);
      },
      &ctx);
  if (kGuard) {
    check(host.callbacks.load() == 1 && mentions(err, kDesc.name) && mentions(err, "1 block"),
          "the report goes to stderr, naming the plugin and the count");
    size_t lines = 0;
    for (size_t at = err.find(kDesc.name); at != std::string::npos;
         at = err.find(kDesc.name, at + 1))
      ++lines;
    check(lines == 1, "...once");
  } else {
    check(err.empty() && host.callbacks.load() == 0, "guard off: nothing on stderr");
  }
  plugin->stop_processing(plugin);
  plugin->deactivate(plugin);
  plugin->destroy(plugin);
}

// ── VST3 ─────────────────────────────────────────────────────────────────────

template <typename S>
void vst3Block(Steinberg_Vst_IAudioProcessor* proc, Buffers<S>& b, Steinberg_int32 frames,
               Steinberg_int32 sampleSize) {
  S* ins[2] = {b.in[0].data(), b.in[1].data()};
  S* outs[2] = {b.out[0].data(), b.out[1].data()};
  S* auxes[2] = {b.aux[0].data(), b.aux[1].data()};
  Steinberg_Vst_AudioBusBuffers inBus{}, outBus[2]{};
  inBus.numChannels = 2;
  outBus[0].numChannels = 2;
  outBus[1].numChannels = 2;
  if constexpr (sizeof(S) == 4) {
    inBus.Steinberg_Vst_AudioBusBuffers_channelBuffers32 = (float**) ins;
    outBus[0].Steinberg_Vst_AudioBusBuffers_channelBuffers32 = (float**) outs;
    outBus[1].Steinberg_Vst_AudioBusBuffers_channelBuffers32 = (float**) auxes;
  } else {
    inBus.Steinberg_Vst_AudioBusBuffers_channelBuffers64 = (double**) ins;
    outBus[0].Steinberg_Vst_AudioBusBuffers_channelBuffers64 = (double**) outs;
    outBus[1].Steinberg_Vst_AudioBusBuffers_channelBuffers64 = (double**) auxes;
  }
  Steinberg_Vst_ProcessData data{};
  data.processMode = Steinberg_Vst_ProcessModes_kRealtime;
  data.symbolicSampleSize = sampleSize;
  data.numSamples = frames;
  data.numInputs = 1;
  data.numOutputs = 2;
  data.inputs = &inBus;
  data.outputs = outBus;
  proc->lpVtbl->process(proc, &data);
}

template <typename S>
void vst3Path(const char* label) {
  std::printf("\n── VST3, %s ──\n", label);
  const Steinberg_int32 size = sizeof(S) == 4 ? Steinberg_Vst_SymbolicSampleSizes_kSample32
                                              : Steinberg_Vst_SymbolicSampleSizes_kSample64;
  Steinberg_IPluginFactory* factory = GetPluginFactory();
  Steinberg_PClassInfo info{};
  bool found = false;
  for (Steinberg_int32 i = 0; i < factory->lpVtbl->countClasses(factory) && !found; ++i)
    found = factory->lpVtbl->getClassInfo(factory, i, &info) == Steinberg_kResultOk &&
            std::strcmp(info.category, "Audio Module Class") == 0;
  void* object = nullptr;
  char line[400];
  std::snprintf(line, sizeof(line), "%s: the probe is created through its own factory", label);
  check(found && factory->lpVtbl->createInstance(factory, (Steinberg_FIDString) info.cid,
                                                 (Steinberg_FIDString) Steinberg_Vst_IComponent_iid,
                                                 &object) == Steinberg_kResultOk,
        line);
  if (!object) return;
  auto* component = (Steinberg_Vst_IComponent*) object;
  component->lpVtbl->initialize(component, nullptr);
  Steinberg_Vst_IAudioProcessor* proc = nullptr;
  Steinberg_Vst_IEditController* controller = nullptr;
  component->lpVtbl->queryInterface(component, Steinberg_Vst_IAudioProcessor_iid, (void**) &proc);
  component->lpVtbl->queryInterface(component, Steinberg_Vst_IEditController_iid,
                                    (void**) &controller);
  sonore::vst3::Plugin* plugin = sonore::vst3::ownerOfComponent(component);
  std::snprintf(line, sizeof(line), "%s: it processes this sample size", label);
  check(proc && controller && proc->lpVtbl->canProcessSampleSize(proc, size) == Steinberg_kResultOk,
        line);
  if (!proc || !controller) return;
  Steinberg_Vst_ProcessSetup setup{};
  setup.processMode = Steinberg_Vst_ProcessModes_kRealtime;
  setup.symbolicSampleSize = size;
  setup.maxSamplesPerBlock = 512;
  setup.sampleRate = 48000.0;
  proc->lpVtbl->setupProcessing(proc, &setup);
  component->lpVtbl->setActive(component, 1);
  proc->lpVtbl->setProcessing(proc, 1);
  g_allocs.store(0);

  Buffers<S> b(256);
  const auto run = [&](int poison, int where) {
    plugin->shared.params[0] = (float) poison;
    plugin->shared.params[1] = (float) where;
    b.scribble();
    onAudioThread([&] { vst3Block(proc, b, 256, size); });
  };
  run(0, 0);
  std::snprintf(line, sizeof(line), "%s: a clean block comes out bit for bit, main and aux", label);
  check(b.exact(), line);
  run(1, 2);
  if (kGuard) {
    std::snprintf(line, sizeof(line), "%s: NaN on the aux bus -> both buses are silence", label);
    check(b.silent(), line);
    std::snprintf(line, sizeof(line), "%s: ...and the meter shows silence", label);
    check(meterShowsSilence(plugin->shared.meter), line);
  } else {
    std::snprintf(line, sizeof(line), "%s: guard off, the NaN on the aux bus comes through", label);
    check(b.poisonedOnly(2), line);
  }
  run(3, 0);
  if (kGuard) {
    std::snprintf(line, sizeof(line), "%s: -inf on the main bus -> silence", label);
    check(b.silent(), line);
  }
  run(0, 0);
  std::snprintf(line, sizeof(line), "%s: ...and the next clean block is bit for bit", label);
  check(b.exact(), line);
  std::snprintf(line, sizeof(line), "%s: %u trip(s) counted, no allocation on the audio thread "
                "(%ld)", label, (unsigned) plugin->shared.outputGuard.trips.load(), g_allocs.load());
  check(plugin->shared.outputGuard.trips.load() == (kGuard ? 2u : 0u) && g_allocs.load() == 0,
        line);

  // VST3 cannot ask for the main thread; the parameter poll it already uses
  // for the latency and the dirty flag is where the report is made.
  struct Ctx {
    Steinberg_Vst_IEditController* controller;
  } ctx{controller};
  const std::string err = captureStderr(
      [](void* c) {
        auto* ctl = ((Ctx*) c)->controller;
        ctl->lpVtbl->getParamNormalized(ctl, 0);
        ctl->lpVtbl->getParamNormalized(ctl, 1);
      },
      &ctx);
  if (kGuard) {
    std::snprintf(line, sizeof(line), "%s: the parameter poll reports it on stderr, once, naming "
                  "the plugin and the count", label);
    check(mentions(err, kDesc.name) && mentions(err, "2 blocks") &&
              err.find(kDesc.name) == err.rfind(kDesc.name),
          line);
  } else {
    std::snprintf(line, sizeof(line), "%s: guard off: nothing reported", label);
    check(err.empty(), line);
  }
  proc->lpVtbl->setProcessing(proc, 0);
  component->lpVtbl->setActive(component, 0);
  controller->lpVtbl->release(controller);
  proc->lpVtbl->release(proc);
  component->lpVtbl->terminate(component);
  component->lpVtbl->release(component);
}

// ── LV2 ──────────────────────────────────────────────────────────────────────

struct UridMap {
  std::vector<std::string> uris;
  static LV2_URID mapCb(LV2_URID_Map_Handle h, const char* uri) {
    auto* self = (UridMap*) h;
    for (size_t i = 0; i < self->uris.size(); ++i)
      if (self->uris[i] == uri) return (LV2_URID) (i + 1);
    self->uris.emplace_back(uri);
    return (LV2_URID) self->uris.size();
  }
};

struct Lv2HostState {
  int scheduled = 0;
  bool scheduledOnAudioThread = false;
  std::vector<uint8_t> work;
  int logs = 0;
  LV2_URID type = 0;
  std::string message;
  std::thread::id logThread;
} g_lv2;

sonore::lv2::Lv2WorkerStatus scheduleCb(void*, uint32_t size, const void* data) {
  // On the audio thread: record, do not allocate (the counter is armed).
  if (g_lv2.scheduled++ == 0) g_lv2.scheduledOnAudioThread = t_audioThread;
  (void) size;
  (void) data;
  return sonore::lv2::kLv2WorkerSuccess;
}

int logPrintf(void*, LV2_URID type, const char* format, ...) {
  char buffer[512];
  va_list args;
  va_start(args, format);
  std::vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  ++g_lv2.logs;
  g_lv2.type = type;
  g_lv2.message = buffer;
  g_lv2.logThread = std::this_thread::get_id();
  return 0;
}
int logVprintf(void*, LV2_URID, const char*, va_list) { return 0; }

void lv2Path() {
  using namespace sonore::lv2;
  std::printf("\n── LV2 ──\n");
  UridMap urids;
  LV2_URID_Map map{&urids, UridMap::mapCb};
  LV2_Feature mapFeature{LV2_URID__map, &map};
  Lv2WorkerSchedule schedule{nullptr, scheduleCb};
  LV2_Feature workerFeature{SONORE_LV2_WORKER__schedule, &schedule};
  Lv2Log log{nullptr, logPrintf, logVprintf};
  LV2_Feature logFeature{SONORE_LV2_LOG__log, &log};
  const LV2_Feature* withEverything[] = {&mapFeature, &workerFeature, &logFeature, nullptr};
  const LV2_Feature* bare[] = {&mapFeature, nullptr};

  const LV2_Descriptor* d = descriptor();
  const auto* worker = (const Lv2WorkerInterface*) d->extension_data(SONORE_LV2_WORKER__interface);
  for (int round = 0; round < 2; ++round) {
    const bool full = round == 0;
    const char* label = full ? "a host with a worker and a log" : "a host with neither";
    LV2_Handle handle = d->instantiate(d, 48000.0, ".", full ? withEverything : bare);
    char line[400];
    std::snprintf(line, sizeof(line), "%s: the probe instantiates", label);
    check(handle != nullptr, line);
    if (!handle) return;
    float poison = 0.0f, where = 0.0f, enabled = 1.0f, freeWheel = 0.0f, peak = 0.0f, rms = 0.0f;
    Buffers<float> b(256);
    d->connect_port(handle, 0, &poison);
    d->connect_port(handle, 1, &where);
    d->connect_port(handle, (uint32_t) portEnabled(), &enabled);
    d->connect_port(handle, (uint32_t) portFreeWheel(), &freeWheel);
    d->connect_port(handle, (uint32_t) portAudioInL(), b.in[0].data());
    d->connect_port(handle, (uint32_t) portAudioInL() + 1, b.in[1].data());
    d->connect_port(handle, (uint32_t) portAudioOutL(), b.out[0].data());
    d->connect_port(handle, (uint32_t) portAudioOutL() + 1, b.out[1].data());
    d->connect_port(handle, (uint32_t) portAuxOut0(), b.aux[0].data());
    d->connect_port(handle, (uint32_t) portAuxOut0() + 1, b.aux[1].data());
    d->connect_port(handle, (uint32_t) portMeterPeak(), &peak);
    d->connect_port(handle, (uint32_t) portMeterRms(), &rms);
    d->activate(handle);
    g_lv2 = Lv2HostState{};
    g_allocs.store(0);

    const auto run = [&](int p, int w) {
      poison = (float) p;
      where = (float) w;
      b.scribble();
      onAudioThread([&] { d->run(handle, 256); });
    };
    run(0, 0);
    std::snprintf(line, sizeof(line), "%s: a clean block comes out bit for bit", label);
    check(b.exact(), line);
    run(2, 2);
    if (kGuard) {
      std::snprintf(line, sizeof(line), "%s: +inf on the aux port -> every port is silence, and "
                    "the meter ports read 0 (%g, %g)", label, (double) peak, (double) rms);
      check(b.silent() && peak == 0.0f && rms == 0.0f, line);
    } else {
      std::snprintf(line, sizeof(line), "%s: guard off, the +inf comes through", label);
      check(b.poisonedOnly(2), line);
    }
    run(1, 0);
    run(0, 0);
    std::snprintf(line, sizeof(line), "%s: the next clean block is bit for bit, nothing allocated "
                  "(%ld)", label, g_allocs.load());
    check(b.exact() && g_allocs.load() == 0, line);

    auto* plugin = (Plugin*) handle;
    std::snprintf(line, sizeof(line), "%s: %u trip(s) counted", label,
                  (unsigned) plugin->shared.outputGuard.trips.load());
    check(plugin->shared.outputGuard.trips.load() == (kGuard ? 2u : 0u), line);

    if (full) {
      std::snprintf(line, sizeof(line),
                    kGuard ? "%s: run() handed the worker one job, from the audio thread (%d)"
                           : "%s: guard off: run() handed the worker nothing (%d)",
                    label, g_lv2.scheduled);
      check(kGuard ? (g_lv2.scheduled == 1 && g_lv2.scheduledOnAudioThread)
                   : g_lv2.scheduled == 0,
            line);
      if (kGuard) {
        // The host's worker thread runs it: here, this thread.
        const char* job = outputGuardWork();
        worker->work(handle, nullptr, nullptr, (uint32_t) std::strlen(job) + 1, job);
        d->deactivate(handle);
        std::snprintf(line, sizeof(line), "%s: the worker writes one warning to the host's log, "
                      "off the audio thread (%d log(s))", label, g_lv2.logs);
        check(g_lv2.logs == 1 && g_lv2.logThread == g_mainThread &&
                  g_lv2.type == map.map(map.handle, SONORE_LV2_LOG__Warning),
              line);
        std::printf("       \"%s\"\n", g_lv2.message.c_str());
        check(mentions(g_lv2.message, kDesc.name) && mentions(g_lv2.message, "2 blocks"),
              "...naming the plugin and the count");
      } else {
        d->deactivate(handle);
        check(g_lv2.logs == 0, "guard off: nothing logged");
      }
    } else {
      struct Ctx {
        const LV2_Descriptor* d;
        LV2_Handle handle;
      } ctx{d, handle};
      const std::string err = captureStderr(
          [](void* c) {
            auto* x = (Ctx*) c;
            x->d->deactivate(x->handle);
          },
          &ctx);
      if (kGuard)
        check(mentions(err, kDesc.name) && mentions(err, "2 blocks"),
              "with neither, deactivate() reports it on stderr");
      else
        check(err.empty(), "guard off: nothing on stderr");
    }
    d->cleanup(handle);
  }
}

// ── The standalone ───────────────────────────────────────────────────────────

void standalonePath() {
  using namespace sonore::standalone;
  std::printf("\n── the standalone ──\n");
  static App app;
  prepare(app, 48000.0, 256);
  app.testSignal.store(true);
  std::vector<float> left(256), right(256);
  g_allocs.store(0);
  app.instance.params[0] = 1.0f;
  app.instance.params[1] = 1.0f;
  onAudioThread([&] { processBlock(app, left.data(), right.data(), 256); });
  bool silent = true, finite = true;
  size_t zeros = 0;
  for (size_t i = 0; i < 256; ++i) {
    if (left[i] != 0.0f || right[i] != 0.0f) silent = false;
    if (!std::isfinite(left[i]) || !std::isfinite(right[i])) finite = false;
    if (right[i] == 0.0f) ++zeros;
  }
  char line[300];
  if (kGuard) {
    std::snprintf(line, sizeof(line), "NaN in a device block -> the device gets silence, counted "
                  "(%u trip(s)), nothing allocated (%ld)",
                  (unsigned) app.instance.outputGuard.trips.load(), g_allocs.load());
    check(silent && app.instance.outputGuard.trips.load() == 1 && g_allocs.load() == 0, line);
  } else {
    std::snprintf(line, sizeof(line), "guard off: the standalone, being the host, still scrubs the "
                  "NaN sample by sample (%zu zero(s) on the right)", zeros);
    check(finite && !silent && app.instance.outputGuard.trips.load() == 0, line);
  }
  app.instance.params[0] = 0.0f;
  {
    struct Ctx {
      App* app;
      int verdict;
    } ctx{&app, -1};
    // --verify, poisoned and clean: its render no longer shows a NaN (the
    // guard silenced it), so the guard's count is what has to fail it.
    std::printf("  ---- --verify with the probe emitting NaN%s ----\n",
                kGuard ? " (its FAIL lines are the point)" : ", scrubbed by the host");
    static App poisoned;
    poisoned.paramsInitialised = true;
    poisoned.instance.params[0] = 1.0f;
    poisoned.instance.params[1] = 0.0f;
    ctx.app = &poisoned;
    const std::string err = captureStderr(
        [](void* c) {
          auto* x = (Ctx*) c;
          x->verdict = verify(*x->app);
        },
        &ctx);
    if (kGuard) {
      check(ctx.verdict != 0, "--verify fails a DSP that makes NaN, though the render is silence");
      check(mentions(err, kDesc.name) && err.find(kDesc.name) == err.rfind(kDesc.name),
            "...and the offline render reported it once on stderr");
    }
    std::printf("  ---- --verify with the probe clean ----\n");
    static App clean;
    check(verify(clean) == 0, "--verify passes the same probe when it is clean");
    // The offline render is a bounce, and the DSP is told so -- in prepare()
    // and in every block -- as every format tells it when its host bounces.
    // The live device above is not one.
    std::snprintf(line, sizeof(line), "the offline render tells the DSP it is offline (prepare %d, "
                  "blocks %d); the device does not (prepare %d, blocks %d)",
                  (int) clean.instance.dsp.preparedOffline, (int) clean.instance.dsp.ranOffline,
                  (int) app.instance.dsp.preparedOffline, (int) app.instance.dsp.ranOffline);
    check(clean.instance.dsp.preparedOffline && clean.instance.dsp.ranOffline &&
              !app.instance.dsp.preparedOffline && !app.instance.dsp.ranOffline,
          line);
  }
  {
    // A bounce through a plugin that delays (a look-ahead limiter) comes out
    // where the material is, as a host's delay compensation puts it, and
    // whole. --render --input added the plugin's tail to the material and
    // nothing for its latency: the file came out late by the latency, and a
    // plugin whose tail is shorter lost that much of the material's end.
    const uint32_t kFrames = 1000;
    std::vector<float> ramp((size_t) kFrames * 2);
    for (uint32_t i = 0; i < kFrames; ++i)
      ramp[(size_t) i * 2] = ramp[(size_t) i * 2 + 1] = (float) (i + 1) / (float) kFrames;
    const char* kPath = "sonore-latency-material.wav";
    static App delayed;
    const bool wrote = sonore::writeWav(kPath, ramp.data(), kFrames, 2, 48000);
    const bool loaded = wrote && delayed.fileSource.load(kPath, 48000.0);
    SonoreDsp::delay() = 64;
    const std::vector<float> out = loaded ? renderOffline(delayed, 0.0) : std::vector<float>();
    SonoreDsp::delay() = 0;
    std::remove(kPath);
    const size_t got = out.size() / 2;
    long firstWrong = -1;
    for (uint32_t i = 0; i < kFrames && firstWrong < 0; ++i)
      if (i >= got || std::fabs(out[(size_t) i * 2] - 0.5f * ramp[(size_t) i * 2]) > 1e-6f)
        firstWrong = (long) i;
    std::snprintf(line, sizeof(line), "a bounce through a 64-sample latency is aligned and whole "
                  "(%zu frames for %u; first misplaced frame %ld)", got, kFrames, firstWrong);
    check(loaded && got >= kFrames && firstWrong < 0, line);
  }
}

} // namespace

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  g_mainThread = std::this_thread::get_id();
  std::printf("Sonore SDK output guard test (guard %s)\n", kGuard ? "on" : "off");

  // Self-check: a counter that cannot see an allocation proves nothing.
  onAudioThread([] {
    std::vector<double> deliberate(512);
    if (deliberate[0] != 0.0) std::abort();
  });
  check(g_allocs.load() > 0, "the allocation counter sees an allocation (self-check)");

  // The check itself, on its own: the bits decide, at every length.
  {
    bool ok = true;
    for (size_t n = 0; n < 40; ++n) {
      std::vector<float> f(n, 0.25f);
      std::vector<double> g(n, 0.25);
      ok = ok && sonore::detail::samplesFinite(f.data(), n) && sonore::detail::samplesFinite(g.data(), n);
      for (size_t at = 0; at < n; ++at) {
        const float badF[] = {std::numeric_limits<float>::quiet_NaN(),
                              std::numeric_limits<float>::infinity(),
                              -std::numeric_limits<float>::infinity(),
                              -std::numeric_limits<float>::quiet_NaN()};
        const double badD[] = {std::numeric_limits<double>::quiet_NaN(),
                               std::numeric_limits<double>::infinity(),
                               -std::numeric_limits<double>::infinity(),
                               -std::numeric_limits<double>::quiet_NaN()};
        for (int k = 0; k < 4; ++k) {
          f[at] = badF[k];
          g[at] = badD[k];
          ok = ok && !sonore::detail::samplesFinite(f.data(), n) &&
               !sonore::detail::samplesFinite(g.data(), n);
        }
        f[at] = std::numeric_limits<float>::max();
        g[at] = -std::numeric_limits<double>::max();
        ok = ok && sonore::detail::samplesFinite(f.data(), n) && sonore::detail::samplesFinite(g.data(), n);
        f[at] = std::numeric_limits<float>::denorm_min();
        g[at] = std::numeric_limits<double>::denorm_min();
        ok = ok && sonore::detail::samplesFinite(f.data(), n) && sonore::detail::samplesFinite(g.data(), n);
        f[at] = 0.25f;
        g[at] = 0.25;
      }
    }
    char line[160];
    std::snprintf(line, sizeof(line), "samplesFinite (%s): NaN, -NaN and both infinities at every "
                  "position of 0-39 samples, and FLT_MAX/DBL_MAX/denormals pass",
                  sonore::simd::backend());
    check(ok, line);
  }

  clapPath<float>("32-bit");
  clapPath<double>("64-bit");
  clapNoLog();
  vst3Path<float>("32-bit");
  vst3Path<double>("64-bit");
  lv2Path();
  standalonePath();

  std::printf("\n%d checks, %d failure(s)\n", g_checks, g_failures);
  if (g_failures == 0) std::printf("OUTPUT GUARD TESTS PASSED\n");
  else std::printf("OUTPUT GUARD TESTS FAILED\n");
  return g_failures == 0 ? 0 : 1;
}
